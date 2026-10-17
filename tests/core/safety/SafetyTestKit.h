// SPDX-License-Identifier: GPL-3.0-or-later
//
// 安全测试的夹具：
//   MachineRig  直接喂状态机：每样本给「读到的是不是有效数据」与测量结果，记下 g_safety；
//   OpenLoop    安全链开环：读头的样本由测试给出；
//   LoopSim     安全链闭环：Input 的输出乘上环内链增益、经与 Output 相同的清洗写进长 D 的环，D 个样本后读回。
// 采样率默认 48 kHz，各阈值的样本数因此都是整数：10 ms = 480、100 ms = 4800、500 ms = 24000、1 s = 48000。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

#include "core/dsp/DspTestKit.h"
#include "dsp/Sanitize.h"
#include "engine/Meters.h"
#include "engine/SafetyMachine.h"

namespace rinne::test
{

using engine::FadeReason;
using engine::Meters;
using engine::SafetyChain;
using engine::SafetyControls;
using engine::SafetyEvent;
using engine::SafetyEventCode;
using engine::SafetyGains;
using engine::SafetyMachine;
using engine::SafetyObservation;
using engine::SafetyState;
using engine::TripReason;

inline constexpr double kRate = 48000.0;

inline std::int64_t samplesOf(double seconds, double sampleRate = kRate)
{
    return static_cast<std::int64_t>(std::llround(seconds * sampleRate));
}

inline SafetyObservation hotGuard()
{
    SafetyObservation o;
    o.contributing = true;
    o.guardHot = true;
    return o;
}

inline SafetyObservation hugeAt(float level)
{
    SafetyObservation o;
    o.contributing = true;
    o.hugeLevel = level;
    return o;
}

struct MachineRig
{
    SafetyMachine m;
    SafetyControls c;
    std::vector<SafetyEvent> events;
    float lastGain = 0.0f;

    explicit MachineRig(double sampleRate = kRate, bool paired = true)
    {
        m.prepare(sampleRate);
        c.paired = paired;
        begin();
    }

    void begin()
    {
        collect();
        m.beginBlock(c);
    }

    // 收走上一块的事件（beginBlock 会清空它们）
    void collect()
    {
        for (int i = 0; i < m.events().size(); ++i)
            events.push_back(m.events()[i]);
    }

    SafetyGains step(bool valid = true, const SafetyObservation& o = {})
    {
        const SafetyGains g = m.advance(valid);
        lastGain = g.safety;
        m.observe(o);
        return g;
    }

    void run(std::int64_t n, bool valid = true, const SafetyObservation& o = {})
    {
        for (std::int64_t i = 0; i < n; ++i)
            step(valid, o);
    }

    // 配对、读到有效数据、淡入走完，停在运行态
    void toRun()
    {
        c.paired = true;
        begin();
        run(samplesOf(0.5) + 1);
    }

    void panic()
    {
        ++c.panicSeq;
        begin();
    }

    void recover()
    {
        ++c.recoverSeq;
        begin();
    }

    bool sawEvent(SafetyEventCode code) const
    {
        for (const auto& e : events)
            if (e.code == code)
                return true;
        return false;
    }
};

// 读头样本的生成器：给出第 n 个样本的读头左右值、是否有效、已乘 g_dry 的干声
struct TapFrame
{
    float tapLeft = 0.0f;
    float tapRight = 0.0f;
    bool valid = true;
    float dryLeft = 0.0f;
    float dryRight = 0.0f;
};

struct OpenLoop
{
    SafetyChain chain;
    SafetyControls controls;
    Meters meters;
    std::int64_t n = 0;

    explicit OpenLoop(double sampleRate = kRate)
    {
        chain.prepare(sampleRate);
        controls.paired = true;
    }

    // 处理一块。gen(n) 返回 TapFrame；sink(n, retLeft, retRight) 收结果
    template<typename Gen, typename Sink>
    void block(int length, Gen&& gen, Sink&& sink)
    {
        chain.beginBlock(controls);
        for (int i = 0; i < length; ++i, ++n)
        {
            const TapFrame f = gen(n);
            float l = f.tapLeft;
            float r = f.tapRight;
            chain.processFront(l, r, f.valid);
            chain.processBack(l, r, f.dryLeft, f.dryRight);
            sink(n, l, r);
        }
        chain.publish(meters);
    }

    template<typename Gen>
    void block(int length, Gen&& gen)
    {
        block(length, gen, [](std::int64_t, float, float) {});
    }
};

// 闭环：out = 干声 + 回送；环 = 清洗(out × 链增益 + 插件自己加进去的信号)，D 个样本后读回。
// D 可以在两次 run 之间改（环保留 maxDelay 个样本的历史）；引擎报给安全链的 D 默认跟着 delay 走，测试可以另设
struct LoopSim
{
    SafetyChain chain;
    SafetyControls controls;
    Meters meters;
    std::int64_t delay;
    float chainGain;
    std::function<float(std::int64_t)> inject; // 环内插件不经输入、自己吐出的信号；空 = 没有
    std::vector<float> ringLeft;
    std::vector<float> ringRight;
    std::int64_t n = 0;

    LoopSim(double sampleRate, std::int64_t delaySamples, float gain, std::int64_t maxDelay = 0)
        : delay(delaySamples), chainGain(gain),
          ringLeft(static_cast<std::size_t>(std::max(delaySamples, maxDelay) + 1), 0.0f),
          ringRight(ringLeft.size(), 0.0f)
    {
        chain.prepare(sampleRate);
        controls.paired = true;
        controls.loopDelaySamples = delaySamples;
    }

    void setDelay(std::int64_t samples)
    {
        delay = samples;
        controls.loopDelaySamples = samples;
    }

    // dry(n) 给第 n 个样本的干声（两声道相同）；probe(n, outL, outR, retL, retR) 观察每个样本
    template<typename Dry, typename Probe>
    void run(std::int64_t count, int blockLength, Dry&& dry, Probe&& probe)
    {
        const auto size = static_cast<std::int64_t>(ringLeft.size());
        std::int64_t done = 0;
        while (done < count)
        {
            const std::int64_t len = std::min<std::int64_t>(blockLength, count - done);
            chain.beginBlock(controls);
            for (std::int64_t i = 0; i < len; ++i, ++n, ++done)
            {
                const bool valid = n >= delay;
                const auto from = static_cast<std::size_t>(valid ? (n - delay) % size : 0);
                float l = valid ? ringLeft[from] : 0.0f;
                float r = valid ? ringRight[from] : 0.0f;
                chain.processFront(l, r, valid);
                const float d = dry(n);
                chain.processBack(l, r, d, d);
                const float outLeft = d + l;
                const float outRight = d + r;
                const float own = inject ? inject(n) : 0.0f;
                const auto to = static_cast<std::size_t>(n % size);
                ringLeft[to] = dsp::sanitizeSample(outLeft * chainGain + own);
                ringRight[to] = dsp::sanitizeSample(outRight * chainGain + own);
                probe(n, outLeft, outRight, l, r);
            }
            chain.publish(meters);
        }
    }
};

// 跑一段闭环并收集结论。dry 给干声
struct LoopSetup
{
    float feedback = 0.0f;
    float chainGain = 1.0f;
    float returnGain = 1.0f; // g_ret：混合 25 % 是 0.5，50 % 及以上是 1
    bool limiterOn = true;
    bool guardOn = true;
    float ceilingDb = engine::safety::kCeilingDefaultDb;
    std::int64_t delay = 4800;
    std::int64_t reportedDelay = -1; // < 0：引擎报的 D 就是环的真实延迟
    double seconds = 8.0;
    std::function<float(std::int64_t)> inject;
};

struct LoopResult
{
    bool guardEngaged = false;
    bool tripped = false;
    TripReason reason = TripReason::None;
    std::int64_t tripSample = -1; // 第一次锁存时的样本
    float maxReturn = 0.0f;
    float maxReduction = 0.0f;
    float maxEstimate = 0.0f; // 链增益估计的最大值
};

template<typename Dry>
LoopResult runLoop(const LoopSetup& setup, Dry&& dry)
{
    LoopSim sim(kRate, setup.delay, setup.chainGain);
    sim.controls.feedback = setup.feedback;
    sim.controls.returnGain = setup.returnGain;
    sim.controls.limiterOn = setup.limiterOn;
    sim.controls.guardOn = setup.guardOn;
    sim.controls.ceilingDb = setup.ceilingDb;
    if (setup.reportedDelay >= 0)
        sim.controls.loopDelaySamples = setup.reportedDelay;
    sim.inject = setup.inject;
    LoopResult res;
    const std::int64_t total = samplesOf(setup.seconds);
    for (std::int64_t done = 0; done < total; done += 256)
    {
        sim.run(256, 256, dry, [&](std::int64_t n, float, float, float l, float r) {
            res.maxReturn = std::max(res.maxReturn, std::max(std::fabs(l), std::fabs(r)));
            res.guardEngaged = res.guardEngaged || sim.chain.machine().guardEngaged();
            res.maxEstimate = std::max(res.maxEstimate, sim.chain.chainGainEstimate());
            if (res.tripSample < 0 && sim.chain.machine().latched())
                res.tripSample = n;
        });
        res.maxReduction = std::max(res.maxReduction, sim.meters.limiterReductionDb.take());
    }
    res.tripped = sim.chain.machine().latched();
    res.reason = sim.chain.machine().tripReason();
    return res;
}

inline float dbfs(double db)
{
    return static_cast<float>(std::pow(10.0, db / 20.0));
}

} // namespace rinne::test
