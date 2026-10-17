// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/SafetyMachine.h"

#include <cmath>

#include "dsp/Sanitize.h"
#include "dsp/SoftClip.h"

namespace rinne::engine
{

namespace
{

using dsp::LinearSmoother;

double usableRate(double sampleRate) noexcept
{
    return (sampleRate > 0.0 && sampleRate < 1.0e7) ? sampleRate : 48000.0;
}

std::int64_t samplesFor(double sampleRate, double seconds) noexcept
{
    return LinearSmoother::samplesFor(sampleRate, seconds);
}

float absMax(float a, float b) noexcept
{
    a = std::fabs(a);
    b = std::fabs(b);
    return a > b ? a : b;
}

float dbToGain(double db) noexcept
{
    return static_cast<float>(std::pow(10.0, db / 20.0));
}

// 控制量进来先夹到合法范围；非有限值按最安全的一侧处理
float clampUnit(float v, float hi) noexcept
{
    if (!(v > 0.0f))
        return 0.0f;
    return v < hi ? v : hi;
}

} // namespace

// ---------------------------------------------------------------- SafetyMachine

void SafetyMachine::prepare(double sampleRate) noexcept
{
    const double sr = usableRate(sampleRate);
    if (prepared_ && sr == sampleRate_)
        return;
    sampleRate_ = sr;
    fadeDown_ = samplesFor(sr, safety::kFadeDownSeconds);
    fadeIn_ = samplesFor(sr, safety::kFadeInSeconds);
    cooldownSamples_ = samplesFor(sr, safety::kRecoverCooldownSeconds);
    hugeSamples_ = samplesFor(sr, safety::kHugeSeconds);
    hugeGap_ = samplesFor(sr, safety::kHugeGapSeconds);
    guardLimit_ = samplesFor(sr, safety::kGuardLimitSeconds);
    guardClip_ = samplesFor(sr, safety::kGuardClipSeconds);
    guardCalm_ = samplesFor(sr, safety::kGuardCalmSeconds);
    guardFloorHold_ = samplesFor(sr, safety::kGuardFloorSeconds);
    unitsPerDb_ = samplesFor(sr, 1.0);
    guardWindow_.prepare(dsp::hopSamplesFor(sr), safety::kGuardWindowHops);

    resetDetectors();
    resetGuard();
    // 首次 prepare 或采样率变了：流已经不连续，回送从 0 起；跳闸时重新计冷却，并再请求一次清空延迟线
    gSafety_.setCurrentAndTargetValue(0.0f);
    if (latched_)
    {
        cooldown_ = cooldownSamples_;
        phase_ = FadePhase::Idle;
        flushIssued_ = false;
    }
    else
    {
        phase_ = FadePhase::WaitValid;
        fadeReason_ = FadeReason::Load;
        pendingGuardReset_ = false;
    }
    prepared_ = true;
}

void SafetyMachine::setCommandBaseline(std::uint32_t panicSeq, std::uint32_t recoverSeq) noexcept
{
    panicSeq_ = panicSeq;
    recoverSeq_ = recoverSeq;
    syncCommands_ = false;
}

void SafetyMachine::beginBlock(const SafetyControls& c) noexcept
{
    events_.clear();
    if (!prepared_)
        return;
    // 限制器开关决定失控保护数哪种样本，换了就从头数；开关失控保护同理
    if (c.limiterOn != limiterOn_)
    {
        limiterOn_ = c.limiterOn;
        guardWindow_.reset();
    }
    if (c.guardOn != guardOn_)
    {
        guardOn_ = c.guardOn;
        guardWindow_.reset();
        calm_ = 0;
    }
    if (paired_ && !c.paired && !latched_)
    {
        fadeReason_ = FadeReason::FirstPair;
        phase_ = FadePhase::Down;
        pendingGuardReset_ = true;
        gSafety_.setTargetValue(0.0f, fadeDown_);
    }
    paired_ = c.paired;
    stalled_ = c.stalled;

    if (syncCommands_)
    {
        panicSeq_ = c.panicSeq;
        recoverSeq_ = c.recoverSeq;
        syncCommands_ = false;
    }
    else
    {
        // 同一块里两样都来时先切断再恢复：恢复必然落在冷却里，被忽略
        if (c.panicSeq != panicSeq_)
        {
            panicSeq_ = c.panicSeq;
            trip(TripReason::Panic);
        }
        if (c.recoverSeq != recoverSeq_)
        {
            recoverSeq_ = c.recoverSeq;
            recover();
        }
    }
    noteState();
}

void SafetyMachine::requestFadeIn(FadeReason reason) noexcept
{
    if (prepared_)
        startFadeIn(reason);
}

void SafetyMachine::startFadeIn(FadeReason reason) noexcept
{
    if (latched_)
        return;
    fadeReason_ = reason;
    // 拓扑变了（新实例、重新配对、换写方、恢复）：失控保护的旧证据作废，等回送降到 0 时清掉
    if (reason == FadeReason::Load || reason == FadeReason::FirstPair || reason == FadeReason::NewWriter ||
        reason == FadeReason::Recover)
        pendingGuardReset_ = true;
    if (gSafety_.getCurrentValue() > 0.0f || gSafety_.getTargetValue() > 0.0f)
    {
        phase_ = FadePhase::Down;
        gSafety_.setTargetValue(0.0f, fadeDown_);
    }
    else
    {
        phase_ = FadePhase::WaitValid;
    }
}

SafetyGains SafetyMachine::advance(bool tapValid) noexcept
{
    if (!prepared_)
        return {};
    if (cooldown_ > 0)
        --cooldown_;
    if (!latched_ && phase_ == FadePhase::WaitValid)
    {
        if (pendingGuardReset_)
        {
            resetGuard();
            pendingGuardReset_ = false;
        }
        if (paired_ && tapValid)
        {
            phase_ = FadePhase::Up;
            gSafety_.setTargetValue(1.0f, fadeIn_);
        }
    }
    const float g = gSafety_.getNextValue();
    if (latched_)
    {
        if (!flushIssued_ && g == 0.0f && !gSafety_.isSmoothing())
        {
            flushIssued_ = true;
            flushPending_ = true;
            push(SafetyEventCode::FlushRequested);
        }
    }
    else if (phase_ == FadePhase::Down && g == 0.0f && !gSafety_.isSmoothing())
    {
        phase_ = FadePhase::WaitValid;
    }
    else if (phase_ == FadePhase::Up && !gSafety_.isSmoothing())
    {
        phase_ = FadePhase::Idle;
    }
    return {g, guardGain_};
}

void SafetyMachine::observe(const SafetyObservation& o) noexcept
{
    if (!prepared_)
        return;
    if (o.internalNonFinite)
        trip(TripReason::Nan);
    if (!latched_)
        hugeStep(o);
    if (!latched_)
        guardStep(o.guardHot);
    noteState();
    ++n_;
}

void SafetyMachine::hugeStep(const SafetyObservation& o) noexcept
{
    if (o.contributing && o.hugeLevel >= safety::kHugeLevel)
    {
        if (!hugeRun_ || n_ - hugeLast_ > hugeGap_)
        {
            hugeRun_ = true;
            hugeStart_ = n_;
        }
        hugeLast_ = n_;
        if (hugeLast_ - hugeStart_ + 1 >= hugeSamples_)
            trip(TripReason::Huge);
    }
    else if (hugeRun_ && (!o.contributing || n_ - hugeLast_ > hugeGap_))
    {
        hugeRun_ = false;
    }
}

void SafetyMachine::guardStep(bool hot) noexcept
{
    if (!guardOn_)
    {
        // 关掉失控保护：不再计数，已压低的部分按回升速度放回，不跳变
        if (guardUnits_ < 0)
        {
            const std::int64_t up = guardUnits_ + safety::kGuardRiseDbPerSecond;
            setGuardUnits(up < 0 ? up : 0);
        }
        if (guardUnits_ == 0 && guardEngaged_)
        {
            guardEngaged_ = false;
            push(SafetyEventCode::GuardReleased);
        }
        floorHot_ = 0;
        return;
    }
    guardWindow_.push(hot);
    if (hot)
        calm_ = 0;
    else if (calm_ < guardCalm_)
        ++calm_;
    const bool criterion = guardWindow_.sum() >= (limiterOn_ ? guardLimit_ : guardClip_);
    if (!guardEngaged_)
    {
        if (!criterion)
            return;
        guardEngaged_ = true;
        push(SafetyEventCode::GuardEngaged);
    }
    const std::int64_t floor = static_cast<std::int64_t>(safety::kGuardFloorDb) * unitsPerDb_;
    if (calm_ >= guardCalm_)
    {
        const std::int64_t up = guardUnits_ + safety::kGuardRiseDbPerSecond;
        setGuardUnits(up < 0 ? up : 0);
        if (guardUnits_ == 0)
        {
            guardEngaged_ = false;
            floorHot_ = 0;
            push(SafetyEventCode::GuardReleased);
            return;
        }
    }
    else if (criterion)
    {
        const std::int64_t down = guardUnits_ - safety::kGuardFallDbPerSecond;
        setGuardUnits(down > floor ? down : floor);
    }
    // 压到底之后还在命中：累计满 1 s 就判失控，跳闸
    if (guardUnits_ <= floor)
    {
        if (hot && ++floorHot_ >= guardFloorHold_)
            trip(TripReason::Runaway);
    }
    else
    {
        floorHot_ = 0;
    }
}

void SafetyMachine::setGuardUnits(std::int64_t units) noexcept
{
    if (units == guardUnits_)
        return;
    guardUnits_ = units;
    guardGain_ = units == 0 ? 1.0f : dbToGain(static_cast<double>(units) / static_cast<double>(unitsPerDb_));
}

double SafetyMachine::guardGainDb() const noexcept
{
    return static_cast<double>(guardUnits_) / static_cast<double>(unitsPerDb_);
}

void SafetyMachine::trip(TripReason reason) noexcept
{
    if (latched_)
    {
        // 已跳闸时再按 Panic 不会恢复，只把冷却重新计起
        if (reason == TripReason::Panic)
            cooldown_ = cooldownSamples_;
        return;
    }
    latched_ = true;
    tripReason_ = reason;
    cooldown_ = cooldownSamples_;
    flushIssued_ = false;
    phase_ = FadePhase::Idle;
    pendingGuardReset_ = false;
    gSafety_.setTargetValue(0.0f, fadeDown_);
    resetDetectors();
    push(SafetyEventCode::Tripped, static_cast<std::uint8_t>(reason));
}

void SafetyMachine::recover() noexcept
{
    if (!latched_)
    {
        push(SafetyEventCode::RecoverIgnored, 2);
        return;
    }
    // 冷却里的恢复直接丢掉，不排队：排队就等于冷却结束时自动恢复
    if (cooldown_ > 0)
    {
        push(SafetyEventCode::RecoverIgnored, 1);
        return;
    }
    const TripReason old = tripReason_;
    latched_ = false;
    tripReason_ = TripReason::None;
    resetDetectors();
    resetGuard();
    startFadeIn(FadeReason::Recover);
    push(SafetyEventCode::Recovered, static_cast<std::uint8_t>(old));
}

void SafetyMachine::resetGuard() noexcept
{
    guardWindow_.reset();
    guardEngaged_ = false;
    setGuardUnits(0);
    calm_ = 0;
    floorHot_ = 0;
}

void SafetyMachine::resetDetectors() noexcept
{
    hugeRun_ = false;
    hugeStart_ = 0;
    hugeLast_ = 0;
}

void SafetyMachine::playEdgeReset() noexcept
{
    if (!prepared_)
        return;
    // 进行中的淡入不提前置满：仍从首个有效读样本起走完 500 ms
    resetDetectors();
    resetGuard();
}

void SafetyMachine::noteReturnNonFinite(int count) noexcept
{
    push(SafetyEventCode::TapNonFinite, static_cast<std::uint8_t>(count < 255 ? count : 255));
}

bool SafetyMachine::takeFlushRequest() noexcept
{
    const bool pending = flushPending_;
    flushPending_ = false;
    return pending;
}

SafetyState SafetyMachine::state() const noexcept
{
    if (latched_)
        return SafetyState::Tripped;
    if (!paired_)
        return SafetyState::Unpaired;
    if (stalled_)
        return SafetyState::Stalled;
    if (phase_ != FadePhase::Idle)
        return SafetyState::FadeIn;
    if (guardEngaged_ || guardUnits_ < 0)
        return SafetyState::Guard;
    return SafetyState::Run;
}

void SafetyMachine::noteState() noexcept
{
    const SafetyState s = state();
    std::uint8_t reason = 0;
    if (s == SafetyState::Tripped)
        reason = static_cast<std::uint8_t>(tripReason_);
    else if (s == SafetyState::FadeIn)
        reason = static_cast<std::uint8_t>(fadeReason_);
    if (s != lastState_ || reason != lastReason_)
    {
        lastState_ = s;
        lastReason_ = reason;
        push(SafetyEventCode::StateChanged, static_cast<std::uint8_t>(s), reason);
    }
}

void SafetyMachine::push(SafetyEventCode code, std::uint8_t a, std::uint8_t b) noexcept
{
    events_.push(code, a, b, n_);
}

// ---------------------------------------------------------------- SafetyChain

void SafetyChain::prepare(double sampleRate) noexcept
{
    machine_.prepare(sampleRate);
    const double sr = usableRate(sampleRate);
    if (prepared_ && sr == sampleRate_)
        return;
    sampleRate_ = sr;
    dcLeft_.prepare(sr);
    dcRight_.prepare(sr);
    limiter_.prepare(sr);
    paramGain_.reset(sr, safety::kParamSmoothSeconds);
    loopGate_.reset(sr, safety::kLoopGateSeconds);
    ceiling_.reset(sr, safety::kParamSmoothSeconds);
    limiterMix_.reset(sr, safety::kLimiterCrossfadeSeconds);
    const std::int64_t hop = dsp::hopSamplesFor(sr);
    tapHistory_.prepare(hop);
    sentHistory_.prepare(hop);
    chainLatency_ = samplesFor(sr, safety::kChainLatencySeconds);
    nearGap_ = samplesFor(sr, safety::kNearFullGapSeconds);
    amplifyHold_ = samplesFor(sr, safety::kChainAmplifyHoldSeconds);
    dcCoeff_ = 1.0 - std::exp(-1.0 / (safety::kDcMeanSeconds * sr));
    dcMeanLeft_ = 0.0;
    dcMeanRight_ = 0.0;
    nearRun_ = false;
    tapMax_ = 0.0f;
    sentMax_ = 0.0f;
    chainAmplifies_ = false;
    amplifyUntil_ = -1;
    requestedDelay_ = 0;
    delayInUse_ = 0;
    delayDropAt_ = -1;
    minLimiterGain_ = 1.0f;
    // 第一块的控制量直接落定，不做斜坡：这时 g_safety 是 0，落定没有可听的跳变
    snapPending_ = true;
    prepared_ = true;
}

void SafetyChain::beginBlock(const SafetyControls& c) noexcept
{
    feedback_ = clampUnit(c.feedback, safety::kFeedbackMax);
    limiterOn_ = c.limiterOn;
    // D 变大立即用；变小时旧的较大 D 继续用，等旧内容全部移出回读窗（旧 D + 1 s + 100 ms）且 D 不再变才换
    const std::int64_t delay = c.loopDelaySamples > 0 ? c.loopDelaySamples : 0;
    if (delay >= delayInUse_)
    {
        delayInUse_ = delay;
        delayDropAt_ = -1;
    }
    else if (delay != requestedDelay_ || delayDropAt_ < 0)
    {
        delayDropAt_ = machine_.samplesProcessed() + delayInUse_ + chainLatency_ +
                       safety::kChainTapHops * tapHistory_.hopSamples();
    }
    requestedDelay_ = delay;
    float ceilingDb = dsp::isFiniteSample(c.ceilingDb) ? c.ceilingDb : safety::kCeilingDefaultDb;
    ceilingDb = ceilingDb < safety::kCeilingMinDb ? safety::kCeilingMinDb : ceilingDb;
    ceilingDb = ceilingDb > safety::kCeilingMaxDb ? safety::kCeilingMaxDb : ceilingDb;

    const float cap = limiterOn_ ? safety::kFeedbackMax : safety::kFeedbackMaxLimiterOff;
    const float userGain = feedback_ < cap ? feedback_ : cap;
    paramGain_.setTargetValue(userGain * clampUnit(c.returnGain, 1.0f));
    loopGate_.setTargetValue(c.loopOn && !c.bypassed ? 1.0f : 0.0f);
    ceiling_.setTargetValue(dbToGain(ceilingDb));
    limiterMix_.setTargetValue(limiterOn_ ? 1.0f : 0.0f);
    if (snapPending_)
    {
        snapSmoothers();
        snapPending_ = false;
    }
    machine_.beginBlock(c);
}

void SafetyChain::processFront(float& left, float& right, bool tapValid) noexcept
{
    bool nonFinite = false;
    left = dsp::sanitizeSample(left, nonFinite);
    right = dsp::sanitizeSample(right, nonFinite);
    if (nonFinite)
        ++blockNonFinite_;
    tapValid_ = tapValid;
    tapPeak_ = absMax(left, right);
    dcMeanLeft_ = dsp::flushTiny(dcMeanLeft_ + dcCoeff_ * (static_cast<double>(left) - dcMeanLeft_));
    dcMeanRight_ = dsp::flushTiny(dcMeanRight_ + dcCoeff_ * (static_cast<double>(right) - dcMeanRight_));
    left = dcLeft_.process(left);
    right = dcRight_.process(right);
}

void SafetyChain::processBack(float& left, float& right, float dryLeft, float dryRight) noexcept
{
    const SafetyGains g = machine_.advance(tapValid_);
    const float gain = paramGain_.getNextValue() * loopGate_.getNextValue() * g.safety * g.guard;
    const float ceiling = ceiling_.getNextValue();
    const float mix = limiterMix_.getNextValue();
    lastGain_ = gain;

    // ⑥ 增益。音色级若产生了非有限值，在这里截住：本样本输出 0，限制器状态复位，状态机跳闸
    float xl = left * gain;
    float xr = right * gain;
    bool nonFinite = !dsp::isFiniteSample(xl) || !dsp::isFiniteSample(xr);
    if (nonFinite)
    {
        xl = 0.0f;
        xr = 0.0f;
        limiter_.reset();
    }
    const float xPeak = absMax(xl, xr);

    // ⑦ 限制器；开关时在限制与不限制之间 10 ms 线性交叉
    float ll = xl;
    float lr = xr;
    const float limiterGain = limiter_.process(ll, lr, ceiling);
    float yl = xl;
    float yr = xr;
    if (mix >= 1.0f)
    {
        yl = ll;
        yr = lr;
    }
    else if (mix > 0.0f)
    {
        yl = xl + (ll - xl) * mix;
        yr = xr + (lr - xr) * mix;
    }

    // ⑧ 软削波与 ⑨ 有限性检查
    bool clipped = false;
    yl = dsp::softClip(yl, clipped);
    yr = dsp::softClip(yr, clipped);
    if (!dsp::isFiniteSample(yl) || !dsp::isFiniteSample(yr))
    {
        yl = 0.0f;
        yr = 0.0f;
        nonFinite = true;
    }

    // 链增益估计：这一样本送进环路的就是 g_dry·干声 + 回送，经 Output 同样的清洗后记峰值
    const float sent = absMax(dsp::sanitizeSample(dryLeft + yl), dsp::sanitizeSample(dryRight + yr));
    tapHistory_.push(tapPeak_);
    if (sentHistory_.push(sent))
        updateChainGain();

    const bool hot = !guardSuppressed() && (limiterOn_ ? limiterGain <= safety::kGuardReductionGain : clipped);

    if (limiterOn_ && limiterGain < minLimiterGain_)
        minLimiterGain_ = limiterGain;
    const std::int64_t n = machine_.samplesProcessed();
    if (xPeak >= safety::kNearFullLevel)
    {
        if (!nearRun_ || n - nearLast_ > nearGap_)
        {
            nearRun_ = true;
            nearStart_ = n;
        }
        nearLast_ = n;
    }
    else if (nearRun_ && n - nearLast_ > nearGap_)
    {
        nearRun_ = false;
    }

    SafetyObservation o;
    // 超大值只计回送路径自己放大出来的电平（见 SafetyMachine.h 里 kHugeLevel 的说明）；g = 0 时状态机整个不计
    const float tapTerm = chainAmplifies_ ? tapPeak_ : 0.0f;
    const float gainTerm = (chainAmplifies_ || gain > 1.0f) ? xPeak : 0.0f;
    o.contributing = gain > 0.0f;
    o.hugeLevel = tapTerm > gainTerm ? tapTerm : gainTerm;
    o.guardHot = hot;
    o.internalNonFinite = nonFinite;
    machine_.observe(o);

    left = yl;
    right = yr;
}

void SafetyChain::updateChainGain() noexcept
{
    if (delayDropAt_ >= 0 && machine_.samplesProcessed() >= delayDropAt_)
    {
        delayInUse_ = requestedDelay_;
        delayDropAt_ = -1;
    }
    const std::int64_t hop = sentHistory_.hopSamples();
    std::int64_t lagHops = (delayInUse_ + chainLatency_ + hop - 1) / hop;
    const std::int64_t maxLag = safety::kChainHistoryHops - safety::kChainTapHops;
    if (lagHops > maxLag)
        lagHops = maxLag;
    tapMax_ = tapHistory_.maxOfLast(safety::kChainTapHops);
    sentMax_ = sentHistory_.maxOfLast(safety::kChainTapHops + static_cast<int>(lagHops));
    // 判为放大之后保持 3 s：送出窗口里的干声瞬态会让估计间歇地偏小，不能让它把整段真失控放过去
    const std::int64_t now = machine_.samplesProcessed();
    if (tapMax_ > sentMax_)
        amplifyUntil_ = now + amplifyHold_;
    chainAmplifies_ = now < amplifyUntil_;
}

float SafetyChain::chainGainEstimate() const noexcept
{
    if (sentMax_ > 0.0f)
        return tapMax_ / sentMax_;
    return tapMax_ > 0.0f ? HUGE_VALF : 0.0f;
}

bool SafetyChain::guardSuppressed() const noexcept
{
    return feedback_ <= safety::kGuardFeedbackLimit && !chainAmplifies_;
}

void SafetyChain::resetDsp() noexcept
{
    dcLeft_.reset();
    dcRight_.reset();
    limiter_.reset();
}

void SafetyChain::snapSmoothers() noexcept
{
    paramGain_.setCurrentAndTargetValue(paramGain_.getTargetValue());
    loopGate_.setCurrentAndTargetValue(loopGate_.getTargetValue());
    ceiling_.setCurrentAndTargetValue(ceiling_.getTargetValue());
    limiterMix_.setCurrentAndTargetValue(limiterMix_.getTargetValue());
}

void SafetyChain::playEdgeReset() noexcept
{
    resetDsp();
    snapSmoothers();
    tapHistory_.reset();
    sentHistory_.reset();
    tapMax_ = 0.0f;
    sentMax_ = 0.0f;
    chainAmplifies_ = false;
    amplifyUntil_ = -1;
    // 历史清空了，旧 D 的内容也就不在了
    delayInUse_ = requestedDelay_;
    delayDropAt_ = -1;
    nearRun_ = false;
    machine_.playEdgeReset();
}

void SafetyChain::publish(Meters& meters) noexcept
{
    const double dc =
        std::fabs(dcMeanLeft_) > std::fabs(dcMeanRight_) ? std::fabs(dcMeanLeft_) : std::fabs(dcMeanRight_);
    meters.returnDcMean.store(static_cast<float>(dc));
    const std::int64_t near = nearRun_ ? nearLast_ - nearStart_ + 1 : 0;
    meters.nearFullSeconds.store(prepared_ ? static_cast<float>(static_cast<double>(near) / sampleRate_) : 0.0f);
    if (minLimiterGain_ < 1.0f)
    {
        const float g = minLimiterGain_ > 1.0e-6f ? minLimiterGain_ : 1.0e-6f;
        meters.limiterReductionDb.publishMax(static_cast<float>(-20.0 * std::log10(static_cast<double>(g))));
    }
    minLimiterGain_ = 1.0f;
    meters.guardGainDb.store(static_cast<float>(machine_.guardGainDb()));
    meters.safety.store(
        PackedSafety::pack(machine_.state(), machine_.tripReason(), machine_.fadeReason(), machine_.coolingDown()),
        std::memory_order_relaxed);
    if (blockNonFinite_ > 0)
    {
        meters.returnNonFinite.fetch_add(static_cast<std::uint32_t>(blockNonFinite_), std::memory_order_relaxed);
        machine_.noteReturnNonFinite(blockNonFinite_);
        blockNonFinite_ = 0;
    }
}

} // namespace rinne::engine
