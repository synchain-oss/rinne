// SPDX-License-Identifier: GPL-3.0-or-later
//
// InputEngine 的实现。见 InputEngine.h 的说明。
//
// 一块的流程：
//   1. 块首：块长观察；两端停用之后的清空请求（在环路层处理本块之前执行）。
//   2. 干声的增益（线性律：m = mixPercent / 100）与回送增益的目标（feedback × g_ret）。
//   3. 交给环路层：参数与走带整体下传，环路层算好 D、拉环、混好回送（含逐样本有效位）。
//   4. 安全链的块首控制量（参数 + 环路层报的配对、断档与 D）；命令序号与环路层的换写方 / 换绑定变成淡入请求。
//   5. 逐样本：读头 → 安全链前段 → （音色级的空位）→ 安全链后段 → 加到干声上。起播沿的复位点上复位安全链。
//   6. 块末：安全链发布测量量；跳闸淡出走完时清空本地延迟线；事件进诊断队列；琥珀灯与诊断的账；发布本端的会合槽。
//
// 实时约定：不分配、不加锁、不读墙钟、不抛异常；0 样本的调用立刻返回，不推进任何计数。
#include "engine/InputEngine.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "dsp/Sanitize.h"

namespace rinne::engine
{

// 端点槽里安全状态的取值与安全链的枚举逐个相同：发布时直接按数值写。
static_assert(ipc::kSafetyUnpaired == static_cast<std::uint32_t>(SafetyState::Unpaired));
static_assert(ipc::kSafetyFadeIn == static_cast<std::uint32_t>(SafetyState::FadeIn));
static_assert(ipc::kSafetyRun == static_cast<std::uint32_t>(SafetyState::Run));
static_assert(ipc::kSafetyStalled == static_cast<std::uint32_t>(SafetyState::Stalled));
static_assert(ipc::kSafetyGuard == static_cast<std::uint32_t>(SafetyState::Guard));
static_assert(ipc::kSafetyTripped == static_cast<std::uint32_t>(SafetyState::Tripped));
static_assert(ipc::kTripPanic == static_cast<std::uint32_t>(TripReason::Panic));
static_assert(ipc::kTripNan == static_cast<std::uint32_t>(TripReason::Nan));
static_assert(ipc::kTripHuge == static_cast<std::uint32_t>(TripReason::Huge));
static_assert(ipc::kTripRunaway == static_cast<std::uint32_t>(TripReason::Runaway));

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

// 线性幅度 → dBFS（峰值诊断用；0 或非有限时给安静的下限）。
inline float toDb(float linear) noexcept
{
    if (!(linear > 0.0f) || !dsp::isFiniteSample(linear))
    {
        return SafetyDiag::kSilentDb;
    }
    return 20.0f * std::log10(linear);
}

// 回送加到干声上。回送为 ±0 时不加：g_dry 恰为 1 就一位不碰（−0.0 与 NaN 载荷原样留着），否则只写乘过的干声。
inline void addReturn(float* out, int i, float dry, float ret, bool dryIsUnity) noexcept
{
    if (out == nullptr)
    {
        return;
    }
    if (ret != 0.0f)
    {
        out[i] = dry + ret;
    }
    else if (!dryIsUnity)
    {
        out[i] = dry;
    }
}
} // namespace

InputEngine::InputEngine() noexcept
{
    // 环路层的事件进本引擎的诊断队列（几段码共用一条队列，消息线程一次排空）。
    loop_.setEventSink(loop::LoopEventSink(&InputEngine::onLoopEvent, this));
    // 安全链的命令基线从 0 起：首个音频块之前按下的紧急切断在首块生效，不会被当成基线吞掉。
    chain_.setCommandBaseline(0, 0);
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
    // 基线本身不算「新命令」：seen_ 也设成基线，第一块不执行任何一条。紧急切断与恢复由安全链执行，基线同时交给它。
    seen_ = baseline;
    chain_.setCommandBaseline(baseline.panicSeq, baseline.recoverSeq);
}

void InputEngine::setSafetyChain(void* observer, FadeInRequest request) noexcept
{
    fadeObserver_ = observer;
    fadeObserverFn_ = request;
}

void InputEngine::prepare(double sampleRate, int maxBlock) noexcept
{
    const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    sampleRate_ = sr;
    maxBlock_ = maxBlock > 0 ? maxBlock : 0;
    // N_obs 每次 prepare 清零：延迟下限因此回落，重新按声明值算。
    maxBlockSeen_ = 0;
    prepared_ = true;

    // 同采样率重新 prepare 时环路层与安全链什么都不动（流保持连续）；换采样率时它们各自从头来。
    loop_.prepare(sr);
    chain_.prepare(sr);
    lampSamples_ = static_cast<std::int64_t>(std::llround(kLoopNanLampSeconds * sr));

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
    diag.loopNanLamp = lampOn_.load(std::memory_order_relaxed);
    return diag;
}

void InputEngine::noteBlockLength(int n) noexcept
{
    if (n > maxBlockSeen_)
    {
        maxBlockSeen_ = n;
    }
}

void InputEngine::requestFadeIn(FadeReason reason) noexcept
{
    chain_.requestFadeIn(reason);
    if (fadeObserverFn_ != nullptr)
    {
        fadeObserverFn_(fadeObserver_, reason);
    }
}

// 块首的命令序号比对：变了就执行一次（不重放旧命令）。
//
//   panicSeq   紧急切断 → 原样交给安全链的块首控制量，由它执行；这里只记下看到的值
//   recoverSeq 恢复回送 → 同上
//   presetSeq  载入工厂预设 → 请求淡入，原因 FadeReason::Preset
//   stateSeq   宿主设置工程状态 → 请求淡入，原因 FadeReason::SetState
//
// presetSeq 与 stateSeq 分开的原因：安全状态机有两种不同的淡入原因，合成一个序号就分不出该报哪一个。
// 在安全链的 beginBlock 之后调：同一块里先切断、再请求淡入时，淡入请求落在锁存里被忽略（预设不解除锁存）。
void InputEngine::checkCommands(const EngineParams& params) noexcept
{
    seen_.panicSeq = params.panicSeq;
    seen_.recoverSeq = params.recoverSeq;
    if (params.presetSeq != seen_.presetSeq)
    {
        seen_.presetSeq = params.presetSeq;
        requestFadeIn(FadeReason::Preset);
    }
    if (params.stateSeq != seen_.stateSeq)
    {
        seen_.stateSeq = params.stateSeq;
        requestFadeIn(FadeReason::SetState);
    }
}

// 安全链每块的控制量。旁路与超长块都按「回送 10 ms 淡到 0」处理（安全链的环路门）：超长块里环路层只给开头一段
// 淡出，回到支持的块长之后环路门从 0 起 10 ms 打开、新数据照常 onset，进出超长块的两头都不阶跃。
SafetyControls InputEngine::makeControls(const EngineParams& params, const loop::BlockResult& result,
                                         bool bypassed) const noexcept
{
    SafetyControls c;
    c.feedback = std::clamp(params.feedbackPercent, kMinFeedbackPercent, kMaxFeedbackPercent) / 100.0f;
    c.returnGain = mixLawReturn(params.mixPercent);
    c.loopOn = params.loopOn;
    c.bypassed = bypassed || result.unsupportedBlock;
    c.limiterOn = params.limiterOn;
    c.ceilingDb = params.ceilingDb;
    c.guardOn = params.guardOn;
    c.panicSeq = params.panicSeq;
    c.recoverSeq = params.recoverSeq;
    c.paired = result.paired;
    c.stalled = result.stalledState;
    c.loopDelaySamples = std::max(result.estimatorDelay, result.delayEffective);
    return c;
}

// 逐样本：读头 → ① 清洗 → ② DC 阻断 → （③ 低切 → ④ 高切 → ⑤ 极性：音色级还没有接入，这是它们的位置）
// → ⑥–⑨ → 加到干声上。安全链每个样本都要走（快速路径也不跳过）：等首个有效读样本、冷却与淡变的计时都靠它推进。
// mono（一个声道）时回送取两格的平均 0.5·(L + R)，不放大环路增益；平均在清洗之后，一个声道坏了另一个照常。
// 回送只有 returnLength 个样本（一般等于块长；超长块里只有开头那段淡出），之后的样本当 0、不算有效。
void InputEngine::renderBlock(const AudioView& buffer, const loop::BlockResult& result, float& blockPeak) noexcept
{
    const int n = buffer.length;
    float* outLeft = buffer.left();
    float* outRight = buffer.numChannels > 1 ? buffer.right() : nullptr;
    const bool mono = buffer.numChannels == 1;
    const int retLength = result.returnLeft != nullptr && result.returnRight != nullptr ? result.returnLength : 0;
    const float* const retLeft = result.returnLeft;
    const float* const retRight = result.returnRight;
    const std::uint8_t* const retValid = result.returnValid;
    const bool dryIsUnity = dryGain_ == 1.0f;

    // 起播沿的复位点：本地延迟线已由环路层在沿上复位过，这里在同一个样本上复位安全链的 DSP 与判据。
    int resetAt = -1;
    if (result.resetDsp && result.resetAtSample >= 0)
    {
        resetAt = static_cast<int>(std::clamp<std::int64_t>(result.resetAtSample - result.blockStart, 0, n - 1));
    }

    for (int i = 0; i < n; ++i)
    {
        if (i == resetAt)
        {
            chain_.playEdgeReset();
        }
        float l = 0.0f;
        float r = 0.0f;
        bool valid = false;
        if (i < retLength)
        {
            l = retLeft[i];
            r = retRight[i];
            valid = retValid != nullptr && retValid[i] != 0;
        }
        blockPeak = std::max({blockPeak, std::fabs(dsp::sanitizeSample(l)), std::fabs(dsp::sanitizeSample(r))});
        chain_.processFront(l, r, valid);

        const float inLeft = outLeft != nullptr ? outLeft[i] : 0.0f;
        const float inRight = outRight != nullptr ? outRight[i] : inLeft;
        const float dryLeft = dryIsUnity ? inLeft : inLeft * dryGain_;
        const float dryRight = dryIsUnity ? inRight : inRight * dryGain_;
        if (mono)
        {
            float m = 0.5f * (l + r);
            float same = m;
            chain_.processBack(m, same, dryLeft, dryLeft);
            addReturn(outLeft, i, dryLeft, m, dryIsUnity);
            continue;
        }
        chain_.processBack(l, r, dryLeft, dryRight);
        addReturn(outLeft, i, dryLeft, l, dryIsUnity);
        addReturn(outRight, i, dryRight, r, dryIsUnity);
    }
}

void InputEngine::drainSafetyEvents() noexcept
{
    const SafetyEventLog& log = chain_.machine().events();
    for (int i = 0; i < log.size(); ++i)
    {
        const SafetyEvent& e = log[i];
        if (e.code == SafetyEventCode::Tripped)
        {
            trips_.fetch_add(1, std::memory_order_relaxed);
        }
        events_.push(diag::Event{static_cast<std::uint16_t>(e.code), e.a, e.b, e.sample});
    }
}

// 环内 NaN 的琥珀灯：Output 写环前清洗掉非有限样本时把环头的清洗计数加一。本端看到它比基线大了就亮 5 s
// （按样本计：从看到的那一块起再走 5 s 的样本才灭），只点灯、不跳闸。换段、换写方、换绑定时重取基线：
// 别的写方的历史不算进来。
void InputEngine::updateLoopNanLamp(const EngineContext& context, const loop::BlockResult& result, int n) noexcept
{
    lampLeft_ = lampLeft_ > n ? lampLeft_ - n : 0;
    if (context.ring == nullptr)
    {
        lampRing_ = nullptr;
    }
    else
    {
        const std::uint32_t count = context.ring->header.nonfinite_count.load(std::memory_order_relaxed);
        const bool rebase = context.ring != lampRing_ || context.ringGeneration != lampGeneration_ ||
                            (result.resynced && (result.resyncReason == loop::SyncReason::kWriter ||
                                                 result.resyncReason == loop::SyncReason::kBindSwitch));
        if (!rebase && count != lampBase_)
        {
            lampLeft_ = lampSamples_;
        }
        lampRing_ = context.ring;
        lampGeneration_ = context.ringGeneration;
        lampBase_ = count;
    }
    lampOn_.store(lampLeft_ > 0, std::memory_order_relaxed);
}

// 发布到本端（Input）的会合槽：块序号、宿主时间戳、实际环路延迟、实时标志、本会话最大块长、回送峰值、安全状态。
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
    // 安全状态：状态机的状态，跳闸原因放在第 8–15 位（取值与枚举逐个相同，见文件头的 static_assert）。
    const SafetyMachine& machine = chain_.machine();
    mine.safety_state.store(static_cast<std::uint32_t>(machine.state()) |
                                (static_cast<std::uint32_t>(machine.tripReason()) << ipc::kTripShift),
                            std::memory_order_relaxed);

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
    // 超长块（n > 32768）：环路这一块不走（两端都不碰环），回送只有开头一段淡出。
    const bool oversize = static_cast<std::uint64_t>(n) > static_cast<std::uint64_t>(ipc::kMaxSupportedBlock);

    // 超长块不计入 N_obs：D 的下限保证的是「读头读的帧在本块之前已经写好」，超长块里环路根本不走、Output 也不写环，
    // 它不构成这个约束。计进去的话一块 40000 帧就会把 D 下限抬到 40000（约 0.83 s）直到下一次 prepare。
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

    // 两端停用之后的清空：在环路层处理本块之前执行，本块就从写头起重建读头。
    if (flushRequest_.exchange(false, std::memory_order_acq_rel))
    {
        loop_.flushAfterSuspend();
    }

    // 干声与回送的增益（线性律）。returnGain_ 是诊断看的目标值，真正的平滑在安全链里。
    dryGain_ = mixLawDry(params.mixPercent);
    const float feedback = std::clamp(params.feedbackPercent, kMinFeedbackPercent, kMaxFeedbackPercent) / 100.0f;
    returnGain_ = params.loopOn && !bypassed ? feedback * mixLawReturn(params.mixPercent) : 0.0f;

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

    // 安全链的块首：控制量（紧急切断与恢复在这里执行），再是淡入请求。
    // 换写方与换绑定（换段、换组、写头倒退）之后从首个有效读样本起重新淡入；失去配对之后重新配上由安全链自己按配对位判。
    chain_.beginBlock(makeControls(params, result, bypassed));
    if (result.resynced && result.resyncReason == loop::SyncReason::kWriter)
    {
        requestFadeIn(FadeReason::NewWriter);
    }
    else if (result.resynced && result.resyncReason == loop::SyncReason::kBindSwitch)
    {
        requestFadeIn(FadeReason::FirstPair);
    }
    checkCommands(params);

    float blockPeak = 0.0f;
    renderBlock(buffer, result, blockPeak);

    // 块末：测量量、跳闸淡出走完之后的清空（只改几个整数）、事件。
    chain_.publish(meters_);
    if (chain_.takeFlushRequest())
    {
        loop_.flushLocalDelayLine();
    }
    drainSafetyEvents();

    // 发布到本端的会合槽（写的是自己的槽，安全不变量 9）。
    publishToSlot(context, transport, bypassed, result, blockPeak);

    // 诊断安全段的账：读头峰值、限制器增益缩减的最大值、环头的清洗计数、琥珀灯。
    tailPeakMax_.publishMax(blockPeak);
    limiterGrMax_.publishMax(meters_.limiterReductionDb.load());
    if (context.ring != nullptr)
    {
        ringNonFinite_.store(context.ring->header.nonfinite_count.load(std::memory_order_relaxed),
                             std::memory_order_relaxed);
        ringClamp_.store(context.ring->header.clamp_count.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
    updateLoopNanLamp(context, result, n);

    return true;
}

} // namespace rinne::engine
