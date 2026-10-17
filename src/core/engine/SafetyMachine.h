// SPDX-License-Identifier: GPL-3.0-or-later
//
// 回送安全：状态机（SafetyMachine）与回送链里归安全管的各级（SafetyChain）。
//
// 回送链每个样本按固定顺序走（③④⑤ 的音色级不在本文件，由引擎夹在前后两段之间）：
//   SafetyChain::processFront   ① 清洗（非有限 → 0，钳 ±16）→ ② DC 阻断 5 Hz
//   （引擎）                     ③ 低切 → ④ 高切 → ⑤ 极性
//   SafetyChain::processBack    ⑥ ×g → [超大值检测] → ⑦ 峰值限制器 → ⑧ 软削波 → ⑨ 有限性检查
//   （引擎）                     out = g_dry·干声 + 回送
// ⑥ 的 g = 平滑(min(feedback, 上限)·g_ret) × 平滑(loop_on·未旁路) × g_safety × g_guard：
//   反馈与 g_ret 30 ms 线性平滑；环路开关与旁路 10 ms（止啸路径要求 ≤ 1 块 + 10 ms 归零）；
//   g_safety 由状态机给（淡入 500 ms、各种淡出 10 ms）；g_guard 由失控保护给（−6 dB/s 下降、+3 dB/s 回升）。
//   反馈上限：限制器开 1.5，关 1.0。
//
// 实时约定：全部成员定长，处理函数 noexcept、不加锁、不分配、不读墙钟；一切计时按样本数（采样率由 prepare 注入），
// 所以参数恒定时，同一段输入怎么切块，输出与状态转移都逐位相同。前后两段每个样本都要调用，快速路径也不能跳过：
// 恢复冷却、等待首个有效读样本这些计时都靠它推进。
#pragma once

#include <cstdint>

#include "dsp/DcBlocker.h"
#include "dsp/HopEnvelope.h"
#include "dsp/LinearSmoother.h"
#include "dsp/PeakLimiter.h"
#include "engine/Meters.h"
#include "engine/SafetyEvents.h"

namespace rinne::engine
{

namespace safety
{
// 淡变
inline constexpr double kFadeDownSeconds = 0.010; // 进淡入前的下行、失去配对、跳闸
inline constexpr double kFadeInSeconds = 0.500; // 从首个有效读样本起
inline constexpr double kParamSmoothSeconds = 0.030;
inline constexpr double kLoopGateSeconds = 0.010; // loop_on 与旁路
inline constexpr double kLimiterCrossfadeSeconds = 0.010;
inline constexpr double kRecoverCooldownSeconds = 1.5;

// 反馈与上限
inline constexpr float kFeedbackMax = 1.5f;
inline constexpr float kFeedbackMaxLimiterOff = 1.0f;
inline constexpr float kCeilingMinDb = -24.0f;
inline constexpr float kCeilingMaxDb = 0.0f;
inline constexpr float kCeilingDefaultDb = -6.0f;

// 超大值：电平 ≥ +12 dBFS（10^(12/20)），连续 100 ms；两个超限样本相隔不超过 10 ms 仍算连续
inline constexpr float kHugeLevel = 3.98107171f;
inline constexpr double kHugeSeconds = 0.100;
inline constexpr double kHugeGapSeconds = 0.010;

// 失控保护
inline constexpr int kGuardWindowHops = 300; // 3 s 窗（10 ms 格）
inline constexpr double kGuardLimitSeconds = 2.0; // 限制器开：GR ≥ 6 dB 的累计时长
inline constexpr double kGuardClipSeconds = 1.5; // 限制器关：软削波样本占比 ≥ 50 %
inline constexpr float kGuardReductionGain = 0.501187234f; // 限制器增益 ≤ 10^(−6/20) 即 GR ≥ 6 dB
inline constexpr int kGuardFallDbPerSecond = 6;
inline constexpr int kGuardRiseDbPerSecond = 3;
inline constexpr double kGuardCalmSeconds = 3.0;
inline constexpr int kGuardFloorDb = -24;
inline constexpr double kGuardFloorSeconds = 1.0;
// 防误报：反馈 ≤ 90 % 且链增益 ≤ 0 dB 时零介入
inline constexpr float kGuardFeedbackLimit = 0.90f;

// 链增益估计：最近 100 ms 回读的峰值 ÷ 同期送出信号的峰值；送出一侧往前多看 D + 1 s，盖住环路延迟与环内插件的延迟
inline constexpr int kChainTapHops = 10;
inline constexpr double kChainLatencySeconds = 1.0;
inline constexpr int kChainHistoryHops = 1024;

// 测量量
inline constexpr double kDcMeanSeconds = 0.300;
inline constexpr float kNearFullLevel = 0.7f;
} // namespace safety

// 每块开头从引擎来的控制量
struct SafetyControls
{
    float feedback = 0.0f; // 参数原值 0…1.5
    float returnGain = 1.0f; // g_ret（混合律给出）
    bool loopOn = true;
    bool bypassed = false; // Input 被宿主旁路
    bool limiterOn = true;
    float ceilingDb = safety::kCeilingDefaultDb;
    bool guardOn = true;
    std::uint32_t panicSeq = 0; // 界面动作的命令序号：与上一块不同就执行一次
    std::uint32_t recoverSeq = 0;
    bool paired = false; // 有对端且能读环
    bool stalled = false; // 写头不动（断档）
    std::int64_t loopDelaySamples = 0;
};

// 每样本交给状态机的测量结果（由 SafetyChain 算好）
struct SafetyObservation
{
    bool contributing = false; // 本样本的回送增益 > 0
    float hugeLevel = 0.0f; // 超大值判据看的电平
    bool guardHot = false; // 失控保护命中（已过防误报门）
    bool internalNonFinite = false; // Input 内部出现了非有限值
};

struct SafetyGains
{
    float safety = 0.0f;
    float guard = 1.0f;
};

class SafetyMachine
{
public:
    // 采样率没变的重 prepare 什么都不动（流连续）；采样率变了，重算常数、复位判据，未跳闸时从 0 重新淡入。
    // 跳闸锁存从不因 prepare 清除。
    void prepare(double sampleRate) noexcept;
    void beginBlock(const SafetyControls& controls) noexcept;
    // 引擎在样本之间调用：载入预设、setState、换写方等。跳闸期间无效（预设与 setState 都不解除锁存）
    void requestFadeIn(FadeReason reason) noexcept;
    // 每样本先调 advance 取本样本的增益，再把测量结果交给 observe
    SafetyGains advance(bool tapValid) noexcept;
    void observe(const SafetyObservation& observation) noexcept;
    // 起播沿复位（回送此时已淡出、延迟线已清）：清判据与失控保护；load / first_pair 的淡入直接置满
    void playEdgeReset() noexcept;
    void noteReturnNonFinite(int count) noexcept;

    SafetyState state() const noexcept;
    TripReason tripReason() const noexcept { return tripReason_; }
    FadeReason fadeReason() const noexcept { return fadeReason_; }
    bool latched() const noexcept { return latched_; }
    bool coolingDown() const noexcept { return latched_ && cooldown_ > 0; }
    // 跳闸淡出完成后置位一次：引擎读到就清空本地延迟线
    bool takeFlushRequest() noexcept;
    float safetyGain() const noexcept { return gSafety_.getCurrentValue(); }
    float guardGain() const noexcept { return guardGain_; }
    double guardGainDb() const noexcept;
    bool guardEngaged() const noexcept { return guardEngaged_; }
    std::int64_t samplesProcessed() const noexcept { return n_; }
    const SafetyEventLog& events() const noexcept { return events_; }

private:
    enum class FadePhase : std::uint8_t
    {
        Idle, // 不在淡入
        Down, // 先降到 0
        WaitValid, // 等首个有效读样本
        Up, // 500 ms 上行
    };

    void trip(TripReason reason) noexcept;
    void recover() noexcept;
    void startFadeIn(FadeReason reason) noexcept;
    void resetGuard() noexcept;
    void resetDetectors() noexcept;
    void hugeStep(const SafetyObservation& o) noexcept;
    void guardStep(bool hot) noexcept;
    void setGuardUnits(std::int64_t units) noexcept;
    void noteState() noexcept;
    void push(SafetyEventCode code, std::uint8_t a = 0, std::uint8_t b = 0) noexcept;

    // 由采样率换算的样本数
    double sampleRate_ = 0.0;
    std::int64_t fadeDown_ = 1;
    std::int64_t fadeIn_ = 1;
    std::int64_t cooldownSamples_ = 1;
    std::int64_t hugeSamples_ = 1;
    std::int64_t hugeGap_ = 1;
    std::int64_t guardLimit_ = 1;
    std::int64_t guardClip_ = 1;
    std::int64_t guardCalm_ = 1;
    std::int64_t guardFloorHold_ = 1;
    std::int64_t unitsPerDb_ = 1; // 失控保护的增益用整数计：1 单位 = 1/round(sr) dB，每样本 −6 或 +3 单位

    bool prepared_ = false;
    bool syncCommands_ = true; // 第一块只记下命令序号，不执行（Panic 不重放）
    std::uint32_t panicSeq_ = 0;
    std::uint32_t recoverSeq_ = 0;
    bool paired_ = false;
    bool stalled_ = false;
    bool limiterOn_ = true;
    bool guardOn_ = true;

    dsp::LinearSmoother gSafety_;
    FadePhase phase_ = FadePhase::WaitValid;
    FadeReason fadeReason_ = FadeReason::Load;
    bool pendingGuardReset_ = false;

    bool latched_ = false;
    TripReason tripReason_ = TripReason::None;
    std::int64_t cooldown_ = 0;
    bool flushIssued_ = false;
    bool flushPending_ = false;

    bool hugeRun_ = false;
    std::int64_t hugeStart_ = 0;
    std::int64_t hugeLast_ = 0;

    dsp::HopCountWindow<safety::kGuardWindowHops + 1> guardWindow_;
    bool guardEngaged_ = false;
    std::int64_t guardUnits_ = 0; // ≤ 0
    float guardGain_ = 1.0f;
    std::int64_t calm_ = 0;
    std::int64_t floorHot_ = 0;

    std::int64_t n_ = 0;
    SafetyState lastState_ = SafetyState::Unpaired;
    std::uint8_t lastReason_ = 0;
    SafetyEventLog events_;
};

class SafetyChain
{
public:
    void prepare(double sampleRate) noexcept;
    void beginBlock(const SafetyControls& controls) noexcept;
    // ①②：就地处理读头读出的一对样本。tapValid 表示本样本来自有效的环数据（等首个有效读样本靠它）
    void processFront(float& left, float& right, bool tapValid) noexcept;
    // ⑥–⑨：就地处理音色级的输出，结果就是回送。dry 是已乘 g_dry 的干声，只用来估计链增益，不在这里相加
    void processBack(float& left, float& right, float dryLeft, float dryRight) noexcept;
    void requestFadeIn(FadeReason reason) noexcept { machine_.requestFadeIn(reason); }
    // 起播沿复位：DSP 状态、平滑器落到目标、判据与链增益估计清零，再交给状态机
    void playEdgeReset() noexcept;
    // 每块末尾调用一次
    void publish(Meters& meters) noexcept;
    bool takeFlushRequest() noexcept { return machine_.takeFlushRequest(); }

    const SafetyMachine& machine() const noexcept { return machine_; }
    // 上一个样本 ⑥ 用的总增益
    float lastGain() const noexcept { return lastGain_; }
    // 链增益估计（线性）；送出为 0 而回读不为 0 时是 +∞，两者都为 0 时是 0
    float chainGainEstimate() const noexcept;
    // 防误报门是否关着（关着时失控保护不计任何命中）
    bool guardSuppressed() const noexcept;

private:
    void resetDsp() noexcept;
    void snapSmoothers() noexcept;
    void updateChainGain() noexcept;

    SafetyMachine machine_;
    dsp::DcBlocker dcLeft_;
    dsp::DcBlocker dcRight_;
    dsp::PeakLimiter limiter_;
    dsp::LinearSmoother paramGain_;
    dsp::LinearSmoother loopGate_;
    dsp::LinearSmoother ceiling_;
    dsp::LinearSmoother limiterMix_;
    dsp::HopPeakHistory<safety::kChainHistoryHops> tapHistory_;
    dsp::HopPeakHistory<safety::kChainHistoryHops> sentHistory_;

    double sampleRate_ = 0.0;
    bool prepared_ = false;
    bool snapPending_ = true;
    float feedback_ = 0.0f;
    bool limiterOn_ = true;
    std::int64_t loopDelay_ = 0;
    std::int64_t chainLatency_ = 0;
    std::int64_t nearGap_ = 1;

    bool tapValid_ = false;
    float tapPeak_ = 0.0f;
    float lastGain_ = 0.0f;
    float tapMax_ = 0.0f;
    float sentMax_ = 0.0f;
    bool chainAmplifies_ = false;

    double dcCoeff_ = 0.0;
    double dcMeanLeft_ = 0.0;
    double dcMeanRight_ = 0.0;
    bool nearRun_ = false;
    std::int64_t nearStart_ = 0;
    std::int64_t nearLast_ = 0;
    float minLimiterGain_ = 1.0f;
    int blockNonFinite_ = 0;
};

} // namespace rinne::engine
