// SPDX-License-Identifier: GPL-3.0-or-later
//
// StreamSync：流相对寻址的状态机（纯逻辑那一半）。
//
// LoopSession 是对外的门面（参数与走带整体下传、读头混合、状态对象都在那里），
// StreamSync 是它内部的状态机：断档、对齐、容忍模式、重同步、停用清空、起播复位。
// 拆成两个文件是为了让「判据」与「门面的数据搬运」各自可读；两者由 LoopSession 组合使用。
//
// 状态与判据（全部按样本计，与宿主怎么切块无关）：
//   - W 与 R 只单调前进。差值一律走有符号运算：W = 0 的新段、W ≈ 2^40、from > pulled 都按有符号算。
//   - 断档：W 未动且 R 前进 ≥ 2·N_max → STALLED，5 ms 淡出。
//   - 对齐：严格模式下 Δ 两次不为 0 就重同步（Δ 是写头每块前进的帧数）。
//   - 容忍模式：10 s 内 ≥ 3 次重同步 → 转容忍模式并置「时序不稳定」。容忍模式下不再因 Δ 变化重同步，
//     但断档判据仍然生效（断档要淡出，这是安全侧）。
//   - 两端停用 > 250 ms → 清空本地延迟线（防停用止啸后原样复燃）。
//   - 同采样率重新 prepare → 保持流连续（不动读头与延迟线）。
//   - reset() → 只记诊断，不动任何音频状态。
//   - 起播（reset_on_play，默认开）：在 s0 用 5 ms 淡出旧回送（不做阶跃），同时复位 DSP 与本地延迟线。
//   - 宿主违约（n > N_prep）→ 取不到的样本补 0，D_min 立即抬升。
//
// 线程：音频线程每块调 process。全部按样本计数，不读墙钟、不分配、不加锁、不抛异常。
// 采样率在 prepare 时注入，所有时长当场换算成样本数。
#pragma once

#include <cstdint>

#include "loop/LocalLine.h"
#include "loop/LoopEvents.h"
#include "loop/LoopTiming.h"
#include "loop/Taps.h"

namespace rinne::loop
{

// 一次重同步的原因（与 LoopEvents 里的 ResyncReason 对应，索引也对应）。
enum class SyncReason : std::uint8_t
{
    kBindSwitch = 0,
    kWriter = 1,
    kFormat = 2,
    kPlay = 3,
    kStallResume = 4,
    kJump = 5,
    kShift = 6,
    kOffset = 7,
    kDChange = 8,
    kReasonCount = 9,
};

// 同步状态。
enum class SyncState : std::uint8_t
{
    kUnpaired = 0, // 没有对端或读不了环
    kStreaming = 1, // 正常
    kStalled = 2,  // 断档（写头不动）
    kTolerant = 3, // 容忍模式：时序不稳定，不再因漂移重同步
};

// 一块的处理输入。宿主每块给的事实。
struct SyncInputs
{
    bool paired = false;        // 有对端且能读环
    bool writerChanged = false; // 写方换了
    bool formatChanged = false; // 写方改了采样率（epoch 变了）
    bool playEdge = false;      // 起播沿（上一块没走、这一块走了）
    bool resetOnPlay = true;    // 起播复位开关
    bool resetRequested = false; // 宿主调用了 reset()：只记诊断
    bool inactive = false;      // 两端都停用（宿主没在调我们，或对端报告停调）
    i64 writeHead = 0;          // W 的快照
    i64 peerMaxBlock = 0;       // 对端 prepare 声明的最大块长
    i64 processed = 0;          // R（Input 本地已处理帧数）
    i64 declaredBlock = 0;      // N_prep
    i64 delayEffective = 0;     // D_eff（样本）。起播淡出的长度取 min(5 ms, D_eff)
    int blockLength = 0;        // n
};

// 一次重同步的结果：读头重建后，LoopSession 据此决定要不要复位 DSP。
struct SyncOutcome
{
    bool resynced = false;
    SyncReason reason = SyncReason::kOffset;
    bool resetDsp = false;        // 起播沿：复位 DSP 与本地延迟线
    // 起播沿的复位点（绝对样本号，与 R 同一坐标）：引擎在这个样本上执行 DSP 复位。
    // 没有复位点时是 -1。口径见 ADR-0002：导出与播放从 s0 + T 起逐位一致，T = min(5 ms, D_eff)。
    std::int64_t resetAtSample = -1;
    bool lineCleared = false;     // 本地延迟线被清空
    bool enteredTolerant = false; // 本次转入容忍模式
    bool stalled = false;         // 本次判为断档
    bool hostViolation = false;   // 本块宿主违约（n > N_prep）
    std::int64_t overrunSamples = 0; // 补 0 的样本数

    // 旧读头要开始淡出（重同步、断档、起播沿都是这一种）。LoopSession 据此给旧读头挂 5 ms 淡出包络。
    bool fadeStarted = false;
    SyncReason fadeReason = SyncReason::kOffset;
};

// 时长（秒）→ 样本数。这些常量是契约的一部分，改动算兼容性事件。
namespace timing
{
inline constexpr double kResyncFadeOutSeconds = 0.005; // 旧读头淡出：5 ms
inline constexpr double kResyncOnsetSeconds = 0.020;   // 新数据 onset：20 ms
inline constexpr double kStallFadeOutSeconds = 0.005;  // 断档淡出：5 ms
inline constexpr double kPlayFadeOutSeconds = 0.005;   // 起播淡出：5 ms
inline constexpr double kInactiveClearSeconds = 0.250; // 两端停用 > 250 ms 清空本地延迟线
inline constexpr double kTolerantWindowSeconds = 10.0; // 10 s 内 ≥ 3 次重同步转容忍模式
inline constexpr int kTolerantThreshold = 3;
} // namespace timing

// StreamSync 本体。
class StreamSync
{
public:
    // 采样率注入：换采样率时重算全部时长并重置域状态（延迟线重分配由 LocalLine 负责）。
    // 同采样率重新 prepare 什么都不动：流保持连续。
    // 非实时线程调用。
    void prepare(double sampleRate) noexcept;

    // 宿主调用 reset()：只记诊断，不动音频状态
    void noteReset() noexcept { ++resetCount_; }

    // 音频线程每块调一次。处理完返回这一块的结果。
    SyncOutcome process(const SyncInputs& in, LocalLine& line, TapSet& taps) noexcept;

    // 当前状态与诊断计数
    SyncState state() const noexcept { return state_; }
    bool tolerant() const noexcept { return state_ == SyncState::kTolerant; }
    std::uint32_t resyncCount() const noexcept { return resyncCount_; }
    std::uint32_t resetCount() const noexcept { return resetCount_; }
    std::uint64_t overflowFrames() const noexcept { return overflowFrames_; }
    std::uint64_t tornFrames() const noexcept { return tornFrames_; }
    std::uint64_t overrunSamples() const noexcept { return overrunSamples_; }
    bool unstable() const noexcept { return unstable_; }

    // 事件出口（可空）
    void setEventSink(LoopEventSink sink) noexcept { sink_ = sink; }

    // 淡变时长（样本）：旧读头淡出与新数据 onset。LoopSession 建新读头时要用 onset 的长度。
    std::int64_t fadeOutSamples() const noexcept { return fadeOut_; }
    std::int64_t onsetSamples() const noexcept { return onset_; }

    // 起播淡出的长度 T = min(5 ms, D_eff)（样本）。D 比 5 ms 还短时淡出不超过一个环路延迟。
    std::int64_t playFadeSamples(std::int64_t delayEffective) const noexcept
    {
        return delayEffective > 0 && delayEffective < fadeOut_ ? delayEffective : fadeOut_;
    }

    // 写头与读位的当前账（诊断用）
    i64 writeHead() const noexcept { return writeHead_; }
    i64 processed() const noexcept { return processed_; }
    i64 offset() const noexcept { return offset_; }
    i64 lastDelta() const noexcept { return lastDelta_; }
    i64 stalledSamples() const noexcept { return stalledSamples_; }
    // 连续最多几块写头没动（块数）。诊断字段 frozenMax 用的是这个，不是 stalledSamples（那是样本数）。
    std::uint32_t maxFrozenBlocks() const noexcept { return maxFrozenBlocks_; }
    // 小漂移被吸收的块数（漂移被吸收是真实事件，诊断要看得见）。
    std::uint32_t absorbedDriftBlocks() const noexcept { return absorbedDriftBlocks_; }

private:
    void resync(SyncReason reason, LocalLine& line, TapSet& taps, i64 writeHead, i64 processed,
                SyncOutcome& outcome) noexcept;
    void clearLine(LocalLine& line, SyncOutcome& outcome) noexcept;
    // 旧回送开始 5 ms 淡出：预拷贝尾巴 + 给全部活跃读头挂淡出包络。
    void startFade(LocalLine& line, TapSet& taps, i64 processed, SyncOutcome& outcome) noexcept;
    void noteResyncTime(i64 processed) noexcept;
    // 记账：写头与读位推进，Δ 记下这一块的写头增量。
    // keepDelta 为假时不记 Δ（重同步之后的第一块没有可比的基准）。
    void account(i64 writeHead, i64 processed, i64 n, i64 delta, bool keepDelta) noexcept;
    std::int64_t samples(double seconds) const noexcept;

    double sampleRate_ = 0.0;
    bool prepared_ = false;

    SyncState state_ = SyncState::kUnpaired;
    i64 writeHead_ = 0;      // 上一块的 W
    i64 processed_ = 0;      // 上一块的 R
    i64 offset_ = 0;         // 主读头的同步偏移
    bool havePrevious_ = false;
    i64 lastDelta_ = 0;      // 上一块的 Δ（写头每块前进的帧数）
    bool haveDelta_ = false;
    i64 stalledSamples_ = 0; // W 一直没动时 R 前进了多少（断档判据按样本计）
    i64 inactiveSamples_ = 0; // 两端都停用时过了多少样本（> 250 ms 清空延迟线）
    bool lastPaired_ = false;

    // 连续多少块写头没动（诊断用：frozenMax）
    std::uint32_t frozenBlocks_ = 0;
    std::uint32_t maxFrozenBlocks_ = 0;

    // 小漂移被吸收的块数：连续 8 块同一个非零漂移就转成一次重同步
    int driftBlocks_ = 0;
    bool absorbedDrift_ = false;
    // 累计被吸收的漂移块数（诊断用：漂移被吸收是真实事件，计数要看得见）
    std::uint32_t absorbedDriftBlocks_ = 0;

    // 起播沿：旧回送 5 ms 淡出；淡出走完之后的下一块复位 DSP 与本地延迟线。
    // 淡出的结束点按样本记（R + fadeOut），不读墙钟。
    bool pendingPlayReset_ = false;
    bool playFadeStarted_ = false;
    i64 playFadeEnd_ = 0;

    std::uint32_t resyncCount_ = 0;
    std::uint32_t resetCount_ = 0;
    std::uint64_t overflowFrames_ = 0;
    std::uint64_t tornFrames_ = 0;
    std::uint64_t overrunSamples_ = 0;
    bool unstable_ = false;

    // 容忍模式的窗口：记最近 3 次重同步的时刻（样本计数）。
    std::int64_t resyncTimes_[timing::kTolerantThreshold]{};
    int resyncTimeCount_ = 0;
    int resyncCursor_ = 0;

    // 淡变时长（样本）
    std::int64_t fadeOut_ = 1;
    std::int64_t onset_ = 1;
    std::int64_t inactiveClear_ = 1;
    std::int64_t tolerantWindow_ = 1;
    std::int64_t tailFrames_ = 1;

    LoopEventSink sink_;
};

} // namespace rinne::loop
