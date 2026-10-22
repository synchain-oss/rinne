// SPDX-License-Identifier: GPL-3.0-or-later
//
// InputEngine 的实现。见 InputEngine.h 的说明。
//
// 一块的流程：
//   1. 块首的命令序号比对：变了就执行一次（紧急切断 / 恢复 / 载入预设 / 宿主设置状态）。
//   2. 干声的增益（线性律：m = mixPercent / 100）与回送的增益（feedback × g_ret）。
//   3. 交给环路层：参数与走带整体下传，环路层算好 D、拉环、混好回送。
//   4. 干声 + 回送写进宿主缓冲。回送增益为 0 时干声逐位直通。
//
// 安全链接上之后，第 4 步的回送换成「安全链处理过的回送」；本引擎的最小实现只做增益。
// 没有安全链时回送增益恒为 0（默认安全）：失败只会让回送变成 0，干声照常。
//
// 实时约定：不分配、不加锁、不读墙钟、不抛异常；0 样本的调用立刻返回，不推进任何计数。
#include "engine/InputEngine.h"

#include <algorithm>
#include <cmath>

#include "dsp/LinearSmoother.h"
#include "dsp/Sanitize.h"

namespace rinne::engine
{

namespace
{
// 干声与回送增益的线性律：m = mixPercent / 100（0..1）。
//   g_dry = min(1, 2(1−m))、g_ret = min(1, 2m)
// m ≤ 50 % 时 g_dry 恰为 1；mix 为 0 时干声逐位直通（g_dry = 1、g_ret = 0）。
inline float mixLawDry(float mixPercent) noexcept
{
    const float m = mixPercent / 100.0f;
    const float g = 2.0f * (1.0f - m);
    return g < 1.0f ? g : 1.0f;
}

inline float mixLawReturn(float mixPercent) noexcept
{
    const float m = mixPercent / 100.0f;
    const float g = 2.0f * m;
    return g < 1.0f ? g : 1.0f;
}

// 回送增益的平滑时长（秒）：30 ms 线性平滑（增益变化不得出现阶跃）。
inline constexpr double kGainRampSeconds = 0.030;
} // namespace

void InputEngine::setCommandBaseline(const CommandBaseline& baseline) noexcept
{
    baseline_ = baseline;
    // 基线本身不算「新命令」：seen_ 也设成基线，第一块不执行任何一条。
    seen_ = baseline;
}

void InputEngine::setSafetyChain(void* chain, FadeInRequest request) noexcept
{
    safetyChain_ = chain;
    safetyRequest_ = request;
}

void InputEngine::prepare(double sampleRate, int maxBlock) noexcept
{
    const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    sampleRate_ = sr;
    maxBlock_ = maxBlock > 0 ? maxBlock : 0;
    // N_obs 每次 prepare 清零：延迟下限因此回落，重新按声明值算。
    maxBlockSeen_ = 0;
    prepared_ = true;

    loop_.prepare(sr);

    // 暂存与混好的回送：定长，容得下支持的最大块长。音频线程上不再分配。
    const std::size_t capacity = static_cast<std::size_t>(ipc::kMaxSupportedBlock);
    if (returnLeft_.size() != capacity)
    {
        returnLeft_.assign(capacity, 0.0f);
        returnRight_.assign(capacity, 0.0f);
        returnValid_.assign(capacity, 0);
    }
}

CommandBaseline InputEngine::commandBaseline() const noexcept
{
    return baseline_;
}

void InputEngine::noteBlockLength(int n) noexcept
{
    if (n > maxBlockSeen_)
    {
        maxBlockSeen_ = n;
    }
}

// 块首的命令序号比对：变了就执行一次（不重放旧命令）。
//
// 四个序号各自记基线：
//   panicSeq  紧急切断
//   recoverSeq 恢复回送
//   presetSeq  载入工厂预设 → 请求淡入，原因 FadeReason::Preset
//   stateSeq   宿主设置工程状态 → 请求淡入，原因 FadeReason::SetState
//
// presetSeq 与 stateSeq 分开的原因：安全状态机有两种不同的淡入原因，合成一个序号就分不出该报哪一个。
void InputEngine::checkCommands(const EngineParams& params) noexcept
{
    if (params.panicSeq != seen_.panicSeq)
    {
        seen_.panicSeq = params.panicSeq;
        events_.push(diag::Event{static_cast<std::uint16_t>(EngineEventCode::kNonRealtime), 0, 0, 0});
        // 紧急切断由安全链执行（接上之后）。没接上时只记事件。
    }
    if (params.recoverSeq != seen_.recoverSeq)
    {
        seen_.recoverSeq = params.recoverSeq;
    }
    if (params.presetSeq != seen_.presetSeq)
    {
        seen_.presetSeq = params.presetSeq;
        if (safetyRequest_ != nullptr)
        {
            safetyRequest_(safetyChain_, FadeReason::Preset);
        }
    }
    if (params.stateSeq != seen_.stateSeq)
    {
        seen_.stateSeq = params.stateSeq;
        if (safetyRequest_ != nullptr)
        {
            safetyRequest_(safetyChain_, FadeReason::SetState);
        }
    }
}

bool InputEngine::process(const AudioView& buffer, const EngineParams& params, const TransportInfo& transport,
                           bool bypassed, const EngineContext& context) noexcept
{
    // 0 样本的调用立刻返回，不推进任何计数。
    if (buffer.length <= 0)
    {
        return false;
    }

    const int n = buffer.length;

    // 块首：命令序号比对与块长观察。
    checkCommands(params);
    noteBlockLength(n);

    // 干声与回送的增益（线性律）。
    dryGain_ = mixLawDry(params.mixPercent);
    const float gRet = mixLawReturn(params.mixPercent);
    // 回送增益：反馈量 × 混合律。没有安全链时恒为 0（默认安全：任何失败只会让回送变成 0）。
    const float feedback = std::max(0.0f, params.feedbackPercent / 100.0f);
    const float target = params.loopOn && !bypassed ? feedback * gRet : 0.0f;

    // 增益的 30 ms 线性平滑：增益变化不得出现阶跃。
    if (gainRampSamples_ <= 1)
    {
        // 平滑长度还没算出来（第一次 prepare 之前）：直接落到目标。
        feedbackGain_ = target;
        gainStart_ = target;
    }
    else if (std::fabs(target - feedbackGain_) > 1.0e-9f)
    {
        gainStart_ = feedbackGain_;
        gainStep_ = 0;
    }
    returnGain_ = target;

    // 环路层：参数与走带整体下传。
    loop::BlockContext ctx(params, transport);
    ctx.ring = context.ring;
    ctx.rendezvous = context.rendezvous;
    ctx.group = context.group;
    ctx.role = context.role;
    ctx.declaredBlock = maxBlock_;
    ctx.observedBlock = maxBlockSeen_;
    ctx.bypassed = bypassed;
    ctx.blockLength = n;
    ctx.returnLeft = returnLeft_.data();
    ctx.returnRight = returnRight_.data();
    ctx.returnValid = returnValid_.data();

    const loop::BlockResult result = loop_.process(ctx);

    // 起播沿的复位点到了：引擎在这个样本上执行 DSP 复位（口径见 ADR-0002）。
    // 复位点给的是绝对样本号（与读位同一坐标）；本块内的偏移是它减去本块的读位起点。
    if (result.resetDsp)
    {
        loop_.flushLocalDelayLine();
    }

    // 干声 + 回送写进宿主缓冲。
    // 反馈为 0 时：干声逐位直通（含 −0.0 与 NaN 载荷）。
    //   干声增益恰为 1 时不乘（乘 1.0 不会改数值，但为了「逐位」这条断言的严格性，仍然不碰）；
    //   回送增益为 0 时不加（加 +0.0 会把 −0.0 变成 +0.0）。
    // 这两条是「反馈为 0 时 out == in 逐位」这条验收的落点。
    float* outLeft = buffer.left();
    float* outRight = buffer.right();
    const float* const retLeft = result.returnLeft;
    const float* const retRight = result.returnRight;
    const bool dryIsUnity = dryGain_ == 1.0f;
    const bool noReturn = target == 0.0f && feedbackGain_ == 0.0f;

    for (int i = 0; i < n; ++i)
    {
        const float dryL = outLeft != nullptr ? outLeft[i] : 0.0f;
        const float dryR = outRight != nullptr ? outRight[i] : 0.0f;
        const float retL = retLeft != nullptr ? retLeft[i] : 0.0f;
        const float retR = retRight != nullptr ? retRight[i] : 0.0f;

        // 逐样本的增益平滑：起点到目标之间线性走。
        float gain = feedbackGain_;
        if (gainStep_ < gainRampSamples_)
        {
            ++gainStep_;
            const auto t = static_cast<float>(static_cast<double>(gainStep_) /
                                             static_cast<double>(gainRampSamples_));
            gain = gainStart_ + (target - gainStart_) * t;
        }
        else
        {
            feedbackGain_ = target;
        }

        // 干声：增益为 1 时原样保留（逐位不变）。
        const float outDryL = dryIsUnity ? dryL : dryL * dryGain_;
        const float outDryR = dryIsUnity ? dryR : dryR * dryGain_;

        if (noReturn)
        {
            // 没有回送：干声逐位直通。
            if (outLeft != nullptr)
            {
                outLeft[i] = outDryL;
            }
            if (outRight != nullptr)
            {
                outRight[i] = outDryR;
            }
            continue;
        }

        if (outLeft != nullptr)
        {
            outLeft[i] = outDryL + retL * gain;
        }
        if (outRight != nullptr)
        {
            outRight[i] = dryR * dryGain_ + retR * gain;
        }
    }

    // 测量量：回送 DC 均值（本引擎只知道自己这一层能算的量；安全链接上之后由安全链填）。
    // 非有限值立刻计数：Input 内部产生的非有限值是跳闸的判据之一。
    std::uint32_t nonFinite = 0;
    float dcSum = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const float l = retLeft != nullptr ? retLeft[i] : 0.0f;
        const float r = retRight != nullptr ? retRight[i] : 0.0f;
        if (!dsp::isFiniteSample(l) || !dsp::isFiniteSample(r))
        {
            ++nonFinite;
            continue;
        }
        dcSum += std::fabs(l) > std::fabs(r) ? l : r;
    }
    if (nonFinite > 0)
    {
        meters_.returnNonFinite.fetch_add(nonFinite, std::memory_order_relaxed);
    }
    if (n > 0)
    {
        meters_.returnDcMean.store(dcSum / static_cast<float>(n));
    }

    return true;
}

} // namespace rinne::engine
