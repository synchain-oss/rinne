// SPDX-License-Identifier: GPL-3.0-or-later
//
// 引擎测试的夹具：
//   InputReference  参照：用一条独立构造的安全链，按 InputEngine.h 写明的接线次序把 Input 的输出重算一遍。
//                   每块喂它 Input 处理之前的干声与这一块的 lastBlockResult()（读头、有效位、配对、D、重同步），
//                   它记下读头（安全链的输入）与应有的输出，测试拿引擎的输出与它逐位比较。
//                   控制量、淡入请求、混合律都在这里按文档另写一遍，不调引擎的任何内部函数：接线错了（次序、
//                   有效位、干声增益、mono 取平均、旁路门）两边就对不上。
//   位型比较、相邻差与低频正弦这几个小工具。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

#include "engine/EngineContext.h"
#include "engine/EngineParams.h"
#include "engine/InputEngine.h"
#include "engine/Meters.h"
#include "engine/OutputEngine.h"
#include "engine/SafetyMachine.h"
#include "engine/TransportInfo.h"
#include "ipc/LoopRing.h"
#include "loop/LoopSession.h"

namespace rinne::test
{

// 混合律（文档原文）：m = mixPercent / 100，g_dry = min(1, 2(1−m))，g_ret = min(1, 2m)。
inline float referenceDryGain(float mixPercent) noexcept
{
    const float m = std::clamp(mixPercent, engine::kMinMixPercent, engine::kMaxMixPercent) / 100.0f;
    const float g = 2.0f * (1.0f - m);
    return g < 1.0f ? g : 1.0f;
}

inline float referenceReturnGain(float mixPercent) noexcept
{
    const float m = std::clamp(mixPercent, engine::kMinMixPercent, engine::kMaxMixPercent) / 100.0f;
    const float g = 2.0f * m;
    return g < 1.0f ? g : 1.0f;
}

class InputReference
{
public:
    explicit InputReference(double sampleRate)
    {
        chain_.setCommandBaseline(0, 0);
        chain_.prepare(sampleRate);
    }

    // 与引擎的 setCommandBaseline 对应（构造后、第一块之前调）
    void setCommandBaseline(const engine::CommandBaseline& baseline)
    {
        chain_.setCommandBaseline(baseline.panicSeq, baseline.recoverSeq);
        presetSeq_ = baseline.presetSeq;
        stateSeq_ = baseline.stateSeq;
    }

    // 一块。inLeft / inRight 是 Input 处理之前的干声（mono 时 inRight 为空），result 是 Input 这一块的
    // lastBlockResult()（它指向引擎的缓冲，要在下一块之前调）。
    void block(const engine::EngineParams& p, const loop::BlockResult& r, bool bypassed, const float* inLeft,
               const float* inRight, int n)
    {
        engine::SafetyControls c;
        c.feedback = std::clamp(p.feedbackPercent, engine::kMinFeedbackPercent, engine::kMaxFeedbackPercent) / 100.0f;
        c.returnGain = referenceReturnGain(p.mixPercent);
        c.loopOn = p.loopOn;
        c.bypassed = bypassed || r.unsupportedBlock;
        c.limiterOn = p.limiterOn;
        c.ceilingDb = p.ceilingDb;
        c.guardOn = p.guardOn;
        c.panicSeq = p.panicSeq;
        c.recoverSeq = p.recoverSeq;
        c.paired = r.paired;
        c.stalled = r.stalledState;
        c.loopDelaySamples = std::max(r.estimatorDelay, r.delayEffective);
        chain_.beginBlock(c);
        if (r.resynced && r.resyncReason == loop::SyncReason::kWriter)
        {
            chain_.requestFadeIn(engine::FadeReason::NewWriter);
        }
        else if (r.resynced && r.resyncReason == loop::SyncReason::kBindSwitch)
        {
            chain_.requestFadeIn(engine::FadeReason::FirstPair);
        }
        if (p.presetSeq != presetSeq_)
        {
            presetSeq_ = p.presetSeq;
            chain_.requestFadeIn(engine::FadeReason::Preset);
        }
        if (p.stateSeq != stateSeq_)
        {
            stateSeq_ = p.stateSeq;
            chain_.requestFadeIn(engine::FadeReason::SetState);
        }

        const float gDry = referenceDryGain(p.mixPercent);
        const bool unity = gDry == 1.0f;
        const int retLength = r.returnLeft != nullptr && r.returnRight != nullptr ? r.returnLength : 0;
        int resetAt = -1;
        if (r.resetDsp && r.resetAtSample >= 0)
        {
            resetAt = static_cast<int>(std::clamp<std::int64_t>(r.resetAtSample - r.blockStart, 0, n - 1));
        }
        for (int i = 0; i < n; ++i)
        {
            if (i == resetAt)
            {
                chain_.playEdgeReset();
            }
            float l = i < retLength ? r.returnLeft[i] : 0.0f;
            float rr = i < retLength ? r.returnRight[i] : 0.0f;
            const bool valid = i < retLength && r.returnValid != nullptr && r.returnValid[i] != 0;
            tapLeft.push_back(l);
            tapRight.push_back(rr);
            tapValid.push_back(valid ? 1 : 0);
            chain_.processFront(l, rr, valid);
            const float xl = inLeft[i];
            const float xr = inRight != nullptr ? inRight[i] : xl;
            const float dl = unity ? xl : xl * gDry;
            const float dr = unity ? xr : xr * gDry;
            if (inRight == nullptr)
            {
                float m = 0.5f * (l + rr);
                float same = m;
                chain_.processBack(m, same, dl, dl);
                wantLeft.push_back(m != 0.0f ? dl + m : dl);
                retLeft.push_back(m);
                continue;
            }
            chain_.processBack(l, rr, dl, dr);
            wantLeft.push_back(l != 0.0f ? dl + l : dl);
            wantRight.push_back(rr != 0.0f ? dr + rr : dr);
            retLeft.push_back(l);
            retRight.push_back(rr);
        }
        chain_.publish(meters_);
        chain_.takeFlushRequest();
    }

    const engine::SafetyChain& chain() const noexcept { return chain_; }

    // 逐样本的记录（从第一块起接在一起）
    std::vector<float> tapLeft; // 读头（安全链的输入）
    std::vector<float> tapRight;
    std::vector<std::uint8_t> tapValid;
    std::vector<float> retLeft; // 安全链的输出（回送）
    std::vector<float> retRight;
    std::vector<float> wantLeft; // 应有的 Input 输出
    std::vector<float> wantRight;

private:
    engine::SafetyChain chain_;
    engine::Meters meters_;
    std::uint32_t presetSeq_ = 0;
    std::uint32_t stateSeq_ = 0;
};

// 一条闭环：一条轨道上 Input 在前、Output 在后，中间可插一个「环内插件的替身」（middle）。
// 环与会合段都在普通内存里（不碰命名段）。每块：信号源 → Input（就地加回送）→ middle → Output（写环）。
// 记下每块 Input 之前的干声与之后的输出，同时喂参照（见上面的 InputReference）。
// 换写方：switchWriter() 把写权交给第二个 Output（另一个 token），之后由它写环。
struct ClosedLoop
{
    using Source = std::function<float(std::int64_t t, int channel)>;
    using Middle = std::function<void(std::int64_t t, float* left, float* right, int n)>;

    std::unique_ptr<ipc::RingSegment> ring = std::make_unique<ipc::RingSegment>();
    std::unique_ptr<ipc::RvSegment> rv = std::make_unique<ipc::RvSegment>();
    engine::InputEngine input;
    engine::OutputEngine output;
    engine::OutputEngine secondOutput;
    InputReference reference;
    double sampleRate;

    Source source = [](std::int64_t, int) { return 0.0f; };
    Middle middle;
    bool inputActive = true; // 假：Input 这一周期不被调用（宿主挂起），时间轴不前进
    bool outputActive = true; // 假：Output 这一周期不被调用（写头不动）
    bool inputBypassed = false;
    bool outputBypassed = false;
    bool writerIsSecond = false;

    std::int64_t at = 0; // 下一块的第一个样本号
    std::vector<float> dryLeft;
    std::vector<float> dryRight;
    std::vector<float> outLeft;
    std::vector<float> outRight;

    static constexpr std::uint64_t kToken = 1;
    static constexpr std::uint64_t kSecondToken = 2;

    ClosedLoop(double rate, int maxBlock) : reference(rate), sampleRate(rate)
    {
        ipc::LoopRing::initGeometry(*ring);
        ipc::LoopRing::publishWriter(*ring, kToken, static_cast<std::uint32_t>(rate), 2);
        input.prepare(rate, maxBlock);
        output.prepare(rate, maxBlock);
        secondOutput.prepare(rate, maxBlock);
    }

    engine::EngineContext inputContext() const
    {
        engine::EngineContext ctx;
        ctx.ring = ring.get();
        ctx.ringGeneration = 1;
        ctx.rendezvous = rv.get();
        ctx.group = 1;
        return ctx;
    }

    engine::EngineContext outputContext(std::uint64_t token) const
    {
        engine::EngineContext ctx = inputContext();
        ctx.token = token;
        return ctx;
    }

    void switchWriter()
    {
        writerIsSecond = true;
        ipc::LoopRing::publishWriter(*ring, kSecondToken, static_cast<std::uint32_t>(sampleRate), 2);
    }

    // 一块（n 个样本）。参数与走带由调用方给（恒定参数时每块给同一份）。
    void block(const engine::EngineParams& p, int n, const engine::TransportInfo& transport = {})
    {
        std::vector<float> left(static_cast<std::size_t>(n));
        std::vector<float> right(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i)
        {
            left[static_cast<std::size_t>(i)] = source(at + i, 0);
            right[static_cast<std::size_t>(i)] = source(at + i, 1);
        }
        float* channels[2] = {left.data(), right.data()};
        engine::AudioView view;
        view.channels = channels;
        view.numChannels = 2;
        view.length = n;
        if (inputActive)
        {
            dryLeft.insert(dryLeft.end(), left.begin(), left.end());
            dryRight.insert(dryRight.end(), right.begin(), right.end());
            const std::vector<float> dl = left;
            const std::vector<float> dr = right;
            input.process(view, p, transport, inputBypassed, inputContext());
            reference.block(p, input.lastBlockResult(), inputBypassed, dl.data(), dr.data(), n);
            outLeft.insert(outLeft.end(), left.begin(), left.end());
            outRight.insert(outRight.end(), right.begin(), right.end());
        }
        if (middle)
        {
            middle(at, left.data(), right.data(), n);
        }
        if (outputActive)
        {
            engine::OutputEngine& writer = writerIsSecond ? secondOutput : output;
            writer.process(view, p, transport, outputBypassed, outputContext(writerIsSecond ? kSecondToken : kToken));
        }
        if (inputActive)
        {
            at += n;
        }
    }

    void run(const engine::EngineParams& p, int n, std::int64_t samples, const engine::TransportInfo& transport = {})
    {
        for (std::int64_t done = 0; done < samples; done += n)
        {
            block(p, n, transport);
        }
    }

    // 安全链当前的状态（读 Input 的测量量）
    engine::SafetyState state() const
    {
        return engine::PackedSafety::state(input.meters().safety.load(std::memory_order_relaxed));
    }
};

// 两串 float 逐位相同（含 NaN 载荷与 −0.0）。长度不同算不同。
inline bool sameBitsVec(const std::vector<float>& a, const std::vector<float>& b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    return a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

// 第一处位型不同的下标；全同时是 -1（诊断输出用）
inline long long firstBitDiff(const std::vector<float>& a, const std::vector<float>& b)
{
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i)
    {
        if (std::memcmp(&a[i], &b[i], sizeof(float)) != 0)
        {
            return static_cast<long long>(i);
        }
    }
    return a.size() == b.size() ? -1 : static_cast<long long>(n);
}

// [from, to) 里相邻两个样本之差的最大值
inline float maxStep(const std::vector<float>& trace, std::size_t from, std::size_t to)
{
    float worst = 0.0f;
    to = std::min(to, trace.size());
    for (std::size_t i = from + 1; i < to; ++i)
    {
        worst = std::max(worst, std::fabs(trace[i] - trace[i - 1]));
    }
    return worst;
}

} // namespace rinne::test
