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

// 闭环：out = 干声 + 回送；环 = 清洗(out × 链增益)，D 个样本后读回
struct LoopSim
{
    SafetyChain chain;
    SafetyControls controls;
    Meters meters;
    std::int64_t delay;
    float chainGain;
    std::vector<float> ringLeft;
    std::vector<float> ringRight;
    std::int64_t n = 0;

    LoopSim(double sampleRate, std::int64_t delaySamples, float gain)
        : delay(delaySamples), chainGain(gain), ringLeft(static_cast<std::size_t>(delaySamples), 0.0f),
          ringRight(static_cast<std::size_t>(delaySamples), 0.0f)
    {
        chain.prepare(sampleRate);
        controls.paired = true;
        controls.loopDelaySamples = delaySamples;
    }

    // dry(n) 给第 n 个样本的干声（两声道相同）；probe(n, outL, outR, retL, retR) 观察每个样本
    template<typename Dry, typename Probe>
    void run(std::int64_t count, int blockLength, Dry&& dry, Probe&& probe)
    {
        std::int64_t done = 0;
        while (done < count)
        {
            const std::int64_t len = std::min<std::int64_t>(blockLength, count - done);
            chain.beginBlock(controls);
            for (std::int64_t i = 0; i < len; ++i, ++n, ++done)
            {
                const auto slot = static_cast<std::size_t>(n % delay);
                float l = ringLeft[slot];
                float r = ringRight[slot];
                chain.processFront(l, r, n >= delay);
                const float d = dry(n);
                chain.processBack(l, r, d, d);
                const float outLeft = d + l;
                const float outRight = d + r;
                ringLeft[slot] = dsp::sanitizeSample(outLeft * chainGain);
                ringRight[slot] = dsp::sanitizeSample(outRight * chainGain);
                probe(n, outLeft, outRight, l, r);
            }
            chain.publish(meters);
        }
    }
};

} // namespace rinne::test
