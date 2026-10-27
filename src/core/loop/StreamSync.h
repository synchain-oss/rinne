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
//   - 对齐：严格模式下 Δ 连续两次检查都不为 0 就重同步。Δ = W − R 扣掉同步偏移 off，即漂移量
//     m = W − (R + off)；正确摆放时恒为 0。一次检查 = 写头在这一块动了的那一次判据。
//     宿主改块长、Output 晚到一块再补上都不改变 m（它回到 0），不重同步。|m| 超过跳变门限时立刻重同步。
//   - 容忍模式：10 s 内 ≥ 3 次**对齐重同步**（跳变、m 连续两次不为 0）→ 转容忍模式并置「时序不稳定」。
//     换写方、改采样率、断档恢复与起播沿不计入：那些是宿主或用户的动作，不是时序不稳定的证据。
//     容忍模式下不再因 m 不为 0 重同步，但跳变与断档判据仍然生效（断档要淡出，这是安全侧）。
//   - 淡出（重同步、断档、对端没了、起播沿）：旧读头读到「本块拉环之后有数据的上界」为止（环还在时就是当前的 W，
//     不是上一块拉到的位置）。数据不够一个完整的淡出时，淡出缩短到恰好在数据用完时走到 0：绝不以阶跃收尾。
//   - 两端停用 > 250 ms → 清空本地延迟线（防停用止啸后原样复燃）。
//   - 对端没了 → 活跃读头 10 ms 淡出；读位照常前进（读头不会反复读同一段）。
//   - 同采样率重新 prepare → 保持流连续（不动读头与延迟线）。
//   - reset() → 只记诊断，不动任何音频状态。
//   - 起播（reset_on_play，默认开）：在 s0 用 T = min(5 ms, D_eff) 淡出旧回送（读预拷贝的尾巴，不做阶跃），
//     本地延迟线在沿上复位（沿后 T 帧之前的帧不再读），DSP 的复位点是绝对样本号 s0 + T。
//     全部按样本定位，所以复位点与宿主怎么切块无关。
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
    kStalled = 2, // 断档（写头不动）
    kTolerant = 3, // 容忍模式：时序不稳定，不再因漂移重同步
};

// 一块的处理输入。宿主每块给的事实。
struct SyncInputs
{
    bool paired = false; // 有对端且能读环
    bool writerChanged = false; // 写方换了
    bool formatChanged = false; // 写方改了采样率（epoch 变了）
    bool playEdge = false; // 起播沿（上一块没走、这一块走了）
    bool resetOnPlay = true; // 起播复位开关
    bool resetRequested = false; // 宿主调用了 reset()：只记诊断
    bool domainReset = false; // 写头倒退（换段遗漏一类）：本地延迟线已从 W 起重来，重新锚定偏移
    // 换了段（换组、重新映射）而两边都配着：重建读头。旧读头只读旧段已经拉进来的数据（dataEnd 由调用方给成
    // 已拉到的位置），新段的帧不进旧读头的淡出。
    bool ringSwitched = false;
    bool inactive = false; // 两端都停用（宿主没在调我们，或对端报告停调）
    i64 writeHead = 0; // W 的快照
    // 本块拉环之后本地延迟线里有数据的上界（绝对位置）：环还在（配上了，或上一块还配着、对端刚走）时就是 W，
    // 环不在时就是已经拉到的位置。旧读头的淡出只读到这里为止。
    i64 dataEnd = 0;
    i64 peerMaxBlock = 0; // 对端 prepare 声明的最大块长
    i64 processed = 0; // R（Input 本地已处理帧数）
    i64 declaredBlock = 0; // N_prep
    i64 delayEffective = 0; // D_eff（样本）。起播淡出的长度取 min(5 ms, D_eff)
    int blockLength = 0; // n
};

// 一次重同步的结果：读头重建后，LoopSession 据此决定要不要复位 DSP。
struct SyncOutcome
{
    bool resynced = false;
    SyncReason reason = SyncReason::kOffset;
    // DSP 的复位点落在本块里：引擎在 resetAtSample 那个样本上复位 DSP（本地延迟线已在沿上复位过）。
    bool resetDsp = false;
    // 起播沿的复位点（绝对样本号，与 R 同一坐标）。起播沿那一块就给出（预告），复位点所在的那一块
    // 再给一次并置 resetDsp。都没有时是 -1。口径见 ADR-0002：导出与播放从 s0 + T 起逐位一致，T = min(5 ms, D_eff)。
    std::int64_t resetAtSample = -1;
    bool lineCleared = false; // 本地延迟线被清空
    bool enteredTolerant = false; // 本次转入容忍模式
    bool stalled = false; // 本次判为断档
    bool hostViolation = false; // 本块宿主违约（n > N_prep）
    std::int64_t overrunSamples = 0; // 补 0 的样本数

    // 旧读头开始淡出（重同步、断档、起播沿、对端没了都是这一种）。包络与预拷贝的尾巴已在本块挂好。
    bool fadeStarted = false;
    SyncReason fadeReason = SyncReason::kOffset;
};

// 时长（秒）→ 样本数。这些常量是契约的一部分，改动算兼容性事件。
namespace timing
{
inline constexpr double kResyncFadeOutSeconds = 0.005; // 旧读头淡出：5 ms
inline constexpr double kResyncOnsetSeconds = 0.020; // 新数据 onset：20 ms
inline constexpr double kStallFadeOutSeconds = 0.005; // 断档淡出：5 ms
inline constexpr double kPlayFadeOutSeconds = 0.005; // 起播淡出：5 ms
inline constexpr double kUnpairFadeOutSeconds = 0.010; // 对端没了：活跃读头 10 ms 淡出
inline constexpr double kInactiveClearSeconds = 0.250; // 两端停用 > 250 ms 清空本地延迟线
inline constexpr double kTolerantWindowSeconds = 10.0; // 10 s 内 ≥ 3 次对齐重同步转容忍模式
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
    // 严格模式的连续计数：最近连续几次检查漂移量 m 不为 0（够 kMisalignedChecksToResync 次就重同步）。
    int misalignedChecks() const noexcept { return misaligned_; }
    i64 stalledSamples() const noexcept { return stalledSamples_; }
    // 连续最多几块写头没动（块数）。诊断字段 frozenMax 用的是这个，不是 stalledSamples（那是样本数）。
    std::uint32_t maxFrozenBlocks() const noexcept { return maxFrozenBlocks_; }
    // m 不为 0、但还没到重同步门槛的检查次数（累计；漂移被吸收是真实事件，诊断要看得见）。
    std::uint32_t absorbedDriftBlocks() const noexcept { return absorbedDriftBlocks_; }

private:
    // 重同步：旧读头淡出（读到 dataEnd 为止），偏移重新锚定在 W − R，连续计数清零。
    void resync(SyncReason reason, LocalLine& line, TapSet& taps, i64 writeHead, i64 processed, i64 dataEnd,
                SyncOutcome& outcome) noexcept;
    // 清空本地延迟线并把起点锚在写头 W（之后从 W 起往后拉）。
    void clearLine(LocalLine& line, i64 writeHead, SyncOutcome& outcome) noexcept;
    // 旧回送开始淡出（名义上 fadeLength 个样本）：预拷贝尾巴 + 给还没在淡出的活跃读头挂淡出包络。
    // 每个读头只读到 fadeEnd 为止；它在 fadeEnd 之前剩下的样本不够 fadeLength 时，这个读头的淡出缩短到那么长，
    // 恰好在数据用完时走到 0（淡出不以阶跃收尾）。
    // 已经在淡出的读头照它自己的进度走完，不重新起算（重新起算会让包络跳回 1，是一处阶跃）。
    void startFade(LocalLine& line, TapSet& taps, i64 processed, std::int64_t fadeLength, i64 fadeEnd,
                   SyncOutcome& outcome) noexcept;
    // 起播沿的 DSP 复位点落在本块 [R, R + n) 里吗：落在里面就置 resetDsp 并给出复位点。
    void notePlayReset(i64 processed, i64 n, SyncOutcome& outcome) noexcept;
    void noteResyncTime(i64 processed) noexcept;
    // 记账：写头与读位推进。
    void account(i64 writeHead, i64 processed, i64 n) noexcept;
    std::int64_t samples(double seconds) const noexcept;

    double sampleRate_ = 0.0;
    bool prepared_ = false;

    SyncState state_ = SyncState::kUnpaired;
    i64 writeHead_ = 0; // 上一块的 W
    i64 processed_ = 0; // 上一块的 R
    i64 offset_ = 0; // 主读头的同步偏移
    bool havePrevious_ = false;
    // 严格模式：最近连续几次检查 m 不为 0（饱和在 kMisalignedChecksToResync）
    int misaligned_ = 0;
    i64 stalledSamples_ = 0; // W 一直没动时 R 前进了多少（断档判据按样本计）
    i64 inactiveSamples_ = 0; // 两端都停用时过了多少样本（> 250 ms 清空延迟线）
    bool inactiveCleared_ = false; // 这一段停用已经清过线：一段停用只清一次，回到活跃时解锁
    bool lastPaired_ = false;

    // 连续多少块写头没动（诊断用：frozenMax）
    std::uint32_t frozenBlocks_ = 0;
    std::uint32_t maxFrozenBlocks_ = 0;

    // 累计被吸收的漂移检查次数（m 不为 0、还没到重同步门槛；诊断用：漂移被吸收是真实事件，计数要看得见）
    std::uint32_t absorbedDriftBlocks_ = 0;

    // 起播沿的 DSP 复位点（绝对样本号 R_edge + T）：按样本记，不读墙钟。
    bool pendingPlayReset_ = false;
    i64 playResetAt_ = 0;

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
    std::int64_t unpairFade_ = 1;
    std::int64_t onset_ = 1;
    std::int64_t inactiveClear_ = 1;
    std::int64_t tolerantWindow_ = 1;
    std::int64_t tailFrames_ = 1;

    LoopEventSink sink_;
};

} // namespace rinne::loop
