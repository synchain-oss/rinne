// SPDX-License-Identifier: GPL-3.0-or-later
//
// InputEngine 的实现。见 InputEngine.h 的说明。
//
// 一块的流程：
//   1. 块首的命令序号比对：变了就执行一次（紧急切断 / 恢复 / 载入预设 / 宿主设置状态）。
//   2. 干声的增益（线性律：m = mixPercent / 100）与回送增益的目标（feedback × g_ret）。
//   3. 交给环路层：参数与走带整体下传，环路层算好 D、拉环、混好回送。
//   4. 回送先清洗（非有限 → 0、钳 ±16），再乘增益、硬钳，加到干声上写进宿主缓冲。回送增益为 0 且没有在滑动时，
//      干声逐位直通。
//   5. 发布本端的会合槽与诊断的账。
//
// 安全链接上之后，第 4 步的回送换成「安全链处理过的回送」（清洗是它的第一级）；本引擎的最小实现只做清洗、增益与硬钳。
//
// 实时约定：不分配、不加锁、不读墙钟、不抛异常；0 样本的调用立刻返回，不推进任何计数。
#include "engine/InputEngine.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "dsp/Sanitize.h"
#include "engine/SafetyMachine.h"

namespace rinne::engine
{

namespace
{
// 干声与回送增益的线性律：m = mixPercent / 100（0..1）。
//   g_dry = min(1, 2(1−m))、g_ret = min(1, 2m)
// m ≤ 50 % 时 g_dry 恰为 1；mix 为 0 时干声逐位直通（g_dry = 1、g_ret = 0）。
inline float mixLawDry(float mixPercent) noexcept
{
    const float m = std::clamp(mixPercent, kMinMixPercent, kMaxMixPercent) / 100.0f;
    const float g = 2.0f * (1.0f - m);
    return g < 1.0f ? g : 1.0f;
}

inline float mixLawReturn(float mixPercent) noexcept
{
    const float m = std::clamp(mixPercent, kMinMixPercent, kMaxMixPercent) / 100.0f;
    const float g = 2.0f * m;
    return g < 1.0f ? g : 1.0f;
}

// 回送增益的平滑时长（秒）：30 ms 线性平滑（增益变化不得出现阶跃）。
inline constexpr double kGainRampSeconds = 0.030;

// 安全链接入之前的回送硬上界：|ret| < 1.0（回送有硬上界这条不变量的兜底）。
inline constexpr float kInterimReturnBound = 0.99999994f; // 1.0f 的前一个 float

// 只钳位、不判有限性：它的输入是「读环之后清洗过的样本（|x| ≤ 16）× 增益（0..1）」，恒为有限值。
// std::clamp 碰到 NaN 会原样返回，所以清洗必须在它之前做（见 process 里读回送的那一步）。
inline float boundReturn(float v) noexcept
{
    return std::clamp(v, -kInterimReturnBound, kInterimReturnBound);
}

// 线性幅度 → dBFS（峰值诊断用；0 或非有限时给安静的下限）。
inline float toDb(float linear) noexcept
{
    if (!(linear > 0.0f) || !dsp::isFiniteSample(linear))
    {
        return SafetyDiag::kSilentDb;
    }
    return 20.0f * std::log10(linear);
}
} // namespace

InputEngine::InputEngine() noexcept
{
    // 环路层的事件进本引擎的诊断队列（两段码共用一条队列，消息线程一次排空）。
    loop_.setEventSink(loop::LoopEventSink(&InputEngine::onLoopEvent, this));
}

void InputEngine::onLoopEvent(void* context, std::uint16_t code, std::uint8_t a, std::uint8_t b,
                              std::int64_t sample) noexcept
{
    auto* self = static_cast<InputEngine*>(context);
    self->events_.push(diag::Event{code, a, b, sample});
}

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
    const bool rateChanged = !prepared_ || sampleRate_ != sr;
    sampleRate_ = sr;
    maxBlock_ = maxBlock > 0 ? maxBlock : 0;
    // N_obs 每次 prepare 清零：延迟下限因此回落，重新按声明值算。
    maxBlockSeen_ = 0;
    prepared_ = true;

    // 回送直流均值：左右各一个一阶平均，时间常数与安全链的同一个量相同（300 ms）。换采样率时从 0 重来。
    dcCoeff_ = 1.0 - std::exp(-1.0 / (safety::kDcMeanSeconds * sr));
    if (rateChanged)
    {
        dcMeanLeft_ = 0.0;
        dcMeanRight_ = 0.0;
    }

    loop_.prepare(sr);
    // 平滑长度按采样率换算；当前值不动（同采样率重新 prepare 时增益不跳）。
    returnSmoother_.reset(sr, kGainRampSeconds);

    // 混好的回送：定长，容得下支持的最大块长。音频线程上不再分配。
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

SafetyDiag InputEngine::safetyDiag() const noexcept
{
    SafetyDiag diag;
    diag.state = static_cast<std::uint8_t>(PackedSafety::state(meters_.safety.load(std::memory_order_relaxed)));
    diag.trips = trips_.load(std::memory_order_relaxed);
    diag.tailPeakMaxDb = toDb(tailPeakMax_.load());
    diag.limiterGrMaxDb = limiterGrMax_.load();
    diag.nonfiniteRing = ringNonFinite_.load(std::memory_order_relaxed);
    diag.clampRing = ringClamp_.load(std::memory_order_relaxed);
    return diag;
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
//   panicSeq   紧急切断 → 安全链执行（接上之后）；本引擎只更新基线
//   recoverSeq 恢复回送 → 同上
//   presetSeq  载入工厂预设 → 请求淡入，原因 FadeReason::Preset
//   stateSeq   宿主设置工程状态 → 请求淡入，原因 FadeReason::SetState
//
// presetSeq 与 stateSeq 分开的原因：安全状态机有两种不同的淡入原因，合成一个序号就分不出该报哪一个。
void InputEngine::checkCommands(const EngineParams& params) noexcept
{
    if (params.panicSeq != seen_.panicSeq)
    {
        seen_.panicSeq = params.panicSeq;
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

// 发布到本端（Input）的会合槽：块序号、宿主时间戳、实际环路延迟、实时标志、本会话最大块长、回送峰值。
//
// 写的是自己的槽：只有端点槽的属主能写它（安全不变量 9），角色固定为 Input，不由调用方传。
// 组号 0 或越界时不去写：那会写到别的组或者越界。
// d_eff 写在 Input 槽：延迟只有 Input 那一端在算，Output 侧不需要。
void InputEngine::publishToSlot(const EngineContext& context, const TransportInfo& transport, bool bypassed,
                                const loop::BlockResult& result, float blockPeak) noexcept
{
    if (context.rendezvous == nullptr || context.group == 0 || context.group > ipc::kGroupCount)
    {
        return;
    }

    ipc::RoleSlot& mine = ipc::roleSlot(*context.rendezvous, context.group, ipc::Role::kInput);

    // 宿主时间戳：宿主没给就是 0（槽里的约定：0 = 没给）。块序号最后 release，对端先看见序号就看得见时间戳。
    mine.last_host_ns.store(transport.hostNsValid ? transport.hostNs : 0u, std::memory_order_relaxed);
    // 实际环路延迟（样本）。d_eff 写 Input 槽。
    mine.d_eff.store(static_cast<std::uint32_t>(std::clamp<std::int64_t>(result.delayEffective, 0, 0x7fffffff)),
                     std::memory_order_relaxed);
    mine.max_n_seen.store(static_cast<std::uint32_t>(maxBlockSeen_), std::memory_order_relaxed);
    std::uint32_t peakBits = 0;
    static_assert(sizeof peakBits == sizeof blockPeak, "峰值按 float 位型存");
    std::memcpy(&peakBits, &blockPeak, sizeof peakBits);
    mine.peak_bits.store(peakBits, std::memory_order_relaxed);

    // 实时标志：整字 relaxed store。
    std::uint32_t flags = 0;
    if (bypassed)
    {
        flags |= ipc::kRtBypassed;
    }
    if (transport.nonrealtime)
    {
        flags |= ipc::kRtNonrealtime;
    }
    if (result.hostViolation)
    {
        flags |= ipc::kRtOverMax;
    }
    if (result.unsupportedBlock)
    {
        flags |= ipc::kRtUnsupportedBlock;
    }
    if (result.stalled)
    {
        flags |= ipc::kRtStalled;
    }
    mine.rt_flags.store(flags, std::memory_order_relaxed);

    // 块序号：每块 +1。对端靠它判「对端是不是还活着」（心跳之外的一路证据）。
    mine.block_seq.fetch_add(1, std::memory_order_release);
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
    // 超长块（n > 32768）：环路这一块不走（两端都不碰环），只出干声。
    const bool oversize = static_cast<std::uint64_t>(n) > static_cast<std::uint64_t>(ipc::kMaxSupportedBlock);

    // 块首：命令序号比对与块长观察。
    // 超长块不计入 N_obs：D 的下限保证的是「读头读的帧在本块之前已经写好」，超长块里环路根本不走、Output 也不写环，
    // 它不构成这个约束。计进去的话一块 40000 帧就会把 D 下限抬到 40000（约 0.83 s）直到下一次 prepare。
    checkCommands(params);
    if (!oversize)
    {
        noteBlockLength(n);
    }

    // 进入离线渲染：只发一次事件（不改变 DSP）。
    if (transport.nonrealtime && !lastNonrealtime_)
    {
        events_.push(diag::Event{static_cast<std::uint16_t>(EngineEventCode::kNonRealtime), 1, 0, 0});
    }
    lastNonrealtime_ = transport.nonrealtime;

    // 干声与回送的增益（线性律）。
    dryGain_ = mixLawDry(params.mixPercent);
    const float gRet = mixLawReturn(params.mixPercent);
    const float feedback = std::clamp(params.feedbackPercent, kMinFeedbackPercent, kMaxFeedbackPercent) / 100.0f;
    returnGain_ = params.loopOn && !bypassed ? feedback * gRet : 0.0f;
    // 30 ms 线性平滑：目标没变时进行中的斜坡照常走完，不会每块重新起算。
    // 超长块里目标是 0：环路层只给开头一段淡出，增益同时滑到 0（超长块不短于 32769 帧，采样率不超过 1 MHz 时都比
    // 30 ms 的平滑长，块末增益已到 0），回到支持的块长之后增益从 0 起按 30 ms 淡回：回送在进出超长块的两头都不阶跃。
    const float returnTarget = oversize ? 0.0f : returnGain_;
    returnSmoother_.setTargetValue(returnTarget);

    // 环路层：参数与走带整体下传。
    loop::BlockContext ctx(params, transport);
    ctx.ring = context.ring;
    ctx.ringGeneration = context.ringGeneration;
    ctx.rendezvous = context.rendezvous;
    ctx.group = context.group;
    ctx.role = ipc::Role::kInput;
    ctx.declaredBlock = maxBlock_;
    ctx.observedBlock = maxBlockSeen_;
    ctx.bypassed = bypassed;
    ctx.blockLength = n;
    ctx.returnLeft = returnLeft_.data();
    ctx.returnRight = returnRight_.data();
    ctx.returnValid = returnValid_.data();

    lastResult_ = loop_.process(ctx);
    const loop::BlockResult& result = lastResult_;

    // 起播沿的 DSP 复位点（result.resetDsp / resetAtSample）交给接在后面的 DSP：本地延迟线已由环路层
    // 在沿上复位过，这里不再清它（再清会把起播沿上建好的新读头一起作废，复位点就随宿主的切块漂移了）。

    // 干声 + 回送写进宿主缓冲。
    // 回送增益为 0 且不在滑动时：干声逐位直通（含 −0.0 与 NaN 载荷）。
    //   干声增益恰为 1 时不乘；回送增益为 0 时不加（加 +0.0 会把 −0.0 变成 +0.0）。
    // mono（一个声道）时回送取两格的平均 0.5·(L + R)，不放大环路增益。
    // 回送只有 returnLength 个样本（一般等于块长；超长块里只有开头那段淡出），之后的样本当 0。
    float* outLeft = buffer.left();
    float* outRight = buffer.numChannels > 1 ? buffer.right() : nullptr;
    const bool mono = buffer.numChannels == 1;
    const int retLength = result.returnLeft != nullptr && result.returnRight != nullptr ? result.returnLength : 0;
    const float* const retLeft = result.returnLeft;
    const float* const retRight = result.returnRight;
    const bool dryIsUnity = dryGain_ == 1.0f;
    const bool noReturn =
        returnTarget == 0.0f && !returnSmoother_.isSmoothing() && returnSmoother_.getCurrentValue() == 0.0f;

    float blockPeak = 0.0f;
    std::uint32_t nonFinite = 0;
    for (int i = 0; i < n; ++i)
    {
        // 读环之后的清洗（回送链第一级，与安全链的第一级同一个函数）：非有限 → 0、钳 ±16、极小值冲 0，逐声道做。
        // 环在跨进程的共享内存里，Output 写环前清洗过也不能担保读出来的一定有限（别的版本或异常的写方、内存损坏）。
        // 清洗在 mono 取平均、峰值与直流均值之前：一个声道坏了，另一个声道照常；之后的增益与硬钳都只见有限值。
        // 这里置 0 的是环里来的坏样本（环内的 NaN 由清洗处理、不跳闸），计进 returnNonFinite（按帧计，与安全链一致）。
        float rawL = 0.0f;
        float rawR = 0.0f;
        if (i < retLength)
        {
            bool bad = false;
            rawL = dsp::sanitizeSample(retLeft[i], bad);
            rawR = dsp::sanitizeSample(retRight[i], bad);
            if (bad)
            {
                ++nonFinite;
            }
        }
        blockPeak = std::max({blockPeak, std::fabs(rawL), std::fabs(rawR)});
        // 回送直流均值（清洗之后、乘增益之前）：左右各自平均，发布时取绝对值较大的那个声道。
        dcMeanLeft_ = dsp::flushTiny(dcMeanLeft_ + dcCoeff_ * (static_cast<double>(rawL) - dcMeanLeft_));
        dcMeanRight_ = dsp::flushTiny(dcMeanRight_ + dcCoeff_ * (static_cast<double>(rawR) - dcMeanRight_));

        if (noReturn)
        {
            // 没有回送：干声逐位直通（干声增益不为 1 时只乘干声增益）。
            if (!dryIsUnity)
            {
                if (outLeft != nullptr)
                {
                    outLeft[i] *= dryGain_;
                }
                if (outRight != nullptr)
                {
                    outRight[i] *= dryGain_;
                }
            }
            continue;
        }

        const float gain = returnSmoother_.getNextValue();
        if (mono)
        {
            if (outLeft != nullptr)
            {
                const float dry = dryIsUnity ? outLeft[i] : outLeft[i] * dryGain_;
                outLeft[i] = dry + boundReturn(0.5f * (rawL + rawR) * gain);
            }
            continue;
        }
        if (outLeft != nullptr)
        {
            const float dry = dryIsUnity ? outLeft[i] : outLeft[i] * dryGain_;
            outLeft[i] = dry + boundReturn(rawL * gain);
        }
        if (outRight != nullptr)
        {
            const float dry = dryIsUnity ? outRight[i] : outRight[i] * dryGain_;
            outRight[i] = dry + boundReturn(rawR * gain);
        }
    }

    // 发布到本端的会合槽（写的是自己的槽，安全不变量 9）。
    publishToSlot(context, transport, bypassed, result, blockPeak);

    // 测量量（本引擎只填自己这一层算得出的量；安全链接上之后由安全链填）：读环后清洗掉的非有限帧数、
    // 回送直流均值（左右各自 300 ms 平均，取绝对值较大者，口径见 Meters）。
    if (nonFinite > 0)
    {
        meters_.returnNonFinite.fetch_add(nonFinite, std::memory_order_relaxed);
    }
    meters_.returnDcMean.store(static_cast<float>(std::max(std::fabs(dcMeanLeft_), std::fabs(dcMeanRight_))));

    // 诊断安全段的账：读头峰值、限制器增益缩减的最大值、跳闸次数、环头的清洗计数。
    tailPeakMax_.publishMax(blockPeak);
    limiterGrMax_.publishMax(meters_.limiterReductionDb.load());
    const auto state = static_cast<std::uint8_t>(PackedSafety::state(meters_.safety.load(std::memory_order_relaxed)));
    if (state == static_cast<std::uint8_t>(SafetyState::Tripped) &&
        lastSafetyState_ != static_cast<std::uint8_t>(SafetyState::Tripped))
    {
        trips_.fetch_add(1, std::memory_order_relaxed);
    }
    lastSafetyState_ = state;
    if (context.ring != nullptr)
    {
        ringNonFinite_.store(context.ring->header.nonfinite_count.load(std::memory_order_relaxed),
                             std::memory_order_relaxed);
        ringClamp_.store(context.ring->header.clamp_count.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }

    return true;
}

} // namespace rinne::engine
