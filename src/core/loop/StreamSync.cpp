// SPDX-License-Identifier: GPL-3.0-or-later
//
// StreamSync：流相对寻址的状态机（判据那一半）的实现。
//
// 判据全部按样本计，与宿主怎么切块无关：
//   - 断档：W 未动且 R 前进 ≥ 2·N_max → STALLED，旧回送 5 ms 淡出。
//   - 对齐：严格模式下 Δ（本块写头的增量）两次不等就重同步。
//   - 容忍模式：10 s 内 ≥ 3 次对齐重同步（跳变、持续漂移）→ 转容忍模式并置「时序不稳定」。
//     容忍模式下不再因 Δ 变化重同步，断档判据仍然生效（断档要淡出，这是安全侧）。
//   - 两端停用 > 250 ms → 清空本地延迟线（只改整数，不 memset），起点锚在当前写头。
//   - 对端没了 → 活跃读头 10 ms 淡出，读位照常前进。
//   - 同采样率重新 prepare → 什么都不动，流保持连续。
//   - reset() → 只记诊断。
//   - 起播（reset_on_play，默认开）：旧回送 T = min(5 ms, D_eff) 淡出（读预拷贝的尾巴），本地延迟线在沿上复位，
//     DSP 的复位点是绝对样本号 R_edge + T。
//   - 宿主违约（n > N_prep）→ 记一笔（补 0 与抬升 D 下限在 LoopSession 里做）。
//
// 几何：同步偏移 off 的意义是「写方的帧计数比读位 R 多多少」。锚定在某一刻时 off = W − R，
// 于是读头在输出下标 i 上读环位置 R + i + off − D，也就是写方在 D 帧之前写下的那一帧：环路延迟恰为 D。
// off 在两次重同步之间保持不动，中间的漂移只记诊断；漂移连续 8 块不停就转成一次重同步重新锚定。
//
// 淡出：重同步、断档、起播沿、对端没了都是同一件事 —— 旧回送线性淡出。淡出期间旧读头读的是「预拷贝的尾巴」：
// 淡出要用的那一段在开始淡出之前先从延迟线拷进尾巴区，此后读那一段不再依赖延迟线的主区
// （主区可能已被环覆盖，起播沿时还被整个作废）。淡出是单调的，不产生阶跃。
//
// 实时约定：不分配、不加锁、不读墙钟、不抛异常；0 样本的调用立刻返回，不推进任何计数。
#include "loop/StreamSync.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace rinne::loop
{

namespace
{
// 小漂移连续多少块之后转成一次重同步（对齐用）。
constexpr int kDriftBlocksBeforeShift = 8;

// 计入容忍模式的重同步：只有对齐判据触发的那两种（跳变、持续漂移）是「时序不稳定」的证据。
constexpr bool countsTowardTolerant(SyncReason reason) noexcept
{
    return reason == SyncReason::kJump || reason == SyncReason::kShift;
}
} // namespace

// 时长（秒）→ 样本数。至少一个样本：时长为 0 时也要有一个样本的推进。
std::int64_t StreamSync::samples(double seconds) const noexcept
{
    const double sr = sampleRate_ > 0.0 ? sampleRate_ : 48000.0;
    const auto n = static_cast<std::int64_t>(std::llround(seconds * sr));
    return n > 0 ? n : 1;
}

void StreamSync::prepare(double sampleRate) noexcept
{
    const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    const bool changed = !prepared_ || sampleRate_ != sr;
    sampleRate_ = sr;

    // 全部时长当场换算成样本数（不读墙钟）
    fadeOut_ = samples(timing::kResyncFadeOutSeconds);
    unpairFade_ = samples(timing::kUnpairFadeOutSeconds);
    onset_ = samples(timing::kResyncOnsetSeconds);
    inactiveClear_ = samples(timing::kInactiveClearSeconds);
    tolerantWindow_ = samples(timing::kTolerantWindowSeconds);
    tailFrames_ = samples(timing::kResyncFadeOutSeconds);

    if (!changed)
    {
        // 同采样率重新 prepare：什么都不动。W、R、读头、延迟线的内容都逐位不变，流保持连续。
        return;
    }

    // 换采样率：域状态重来（延迟线由 LocalLine 重新分配）
    prepared_ = true;
    state_ = SyncState::kUnpaired;
    writeHead_ = 0;
    processed_ = 0;
    offset_ = 0;
    havePrevious_ = false;
    haveDelta_ = false;
    lastDelta_ = 0;
    stalledSamples_ = 0;
    frozenBlocks_ = 0;
    maxFrozenBlocks_ = 0;
    inactiveSamples_ = 0;
    lastPaired_ = false;
    driftBlocks_ = 0;
    haveLastDrift_ = false;
    lastDrift_ = 0;
    absorbedDrift_ = false;
    resyncTimeCount_ = 0;
    resyncCursor_ = 0;
    pendingPlayReset_ = false;
    playResetAt_ = 0;
}

void StreamSync::noteResyncTime(i64 processed) noexcept
{
    resyncTimes_[resyncCursor_] = processed;
    resyncCursor_ = (resyncCursor_ + 1) % timing::kTolerantThreshold;
    if (resyncTimeCount_ < timing::kTolerantThreshold)
    {
        ++resyncTimeCount_;
    }
}

void StreamSync::clearLine(LocalLine& line, i64 writeHead, SyncOutcome& outcome) noexcept
{
    // 清空只改整数：延迟线清掉（不 memset，几兆字节清零不能在音频线程上做），起点锚在当前写头，
    // 之后从 W 起往后拉。读头由 LoopSession 在本块末统一作废，这样「清空」与「读头作废」发生在同一个时点上。
    line.resetTo(writeHead);
    outcome.lineCleared = true;
    sink_.emit(LoopEventCode::kLineCleared, 1);
}

// 旧回送开始淡出：给还没在淡出的活跃读头挂淡出包络，并把淡出要用的那一段预拷贝进尾巴区。
//
// 预拷贝的位置：淡出期间读头读的是 R + off − D 起的 fadeLength 帧（本块的下标 i 从 0 开始）。
// 取全部淡出读头的并集，一段就够（尾巴区是定长的）。
// fadeEnd 是旧读头的 validTo：淡出期间不读这个位置及以后（那些位置上的数据属于新的对齐，由新读头去读）。
void StreamSync::startFade(LocalLine& line, TapSet& taps, i64 processed, std::int64_t fadeLength, i64 fadeEnd,
                           SyncOutcome& outcome) noexcept
{
    const std::int64_t length = fadeLength > 0 ? fadeLength : 1;

    i64 lo = std::numeric_limits<i64>::max();
    i64 hi = std::numeric_limits<i64>::min();
    bool any = false;
    for (int i = 0; i < kMaxTaps; ++i)
    {
        const Tap& tap = taps.at(i);
        if (!tap.active || tap.env.fadingOut())
        {
            continue;
        }
        any = true;
        const i64 from = processed + tap.offset - tap.delay;
        lo = std::min(lo, from);
        hi = std::max(hi, from + length);
    }
    if (!any)
    {
        return;
    }
    outcome.fadeStarted = true;

    // 预拷贝尾巴：淡出要用的那一段先拷出来。此后读那一段不再依赖延迟线的主区。
    if (hi > lo)
    {
        line.snapshotRange(lo, hi - lo);
    }

    // 淡出：包络从当前值线性降到 0；validTo 收到 fadeEnd，淡出期间不再读那以后的位置。
    for (int i = 0; i < kMaxTaps; ++i)
    {
        Tap& tap = taps.at(i);
        if (!tap.active || tap.env.fadingOut())
        {
            continue;
        }
        tap.validTo = std::min(tap.validTo, fadeEnd);
        tap.env.fadeOutSteps = length;
        tap.env.fadeOutStep = 0;
    }
}

void StreamSync::notePlayReset(i64 processed, i64 n, SyncOutcome& outcome) noexcept
{
    if (pendingPlayReset_ && playResetAt_ < processed + n)
    {
        // 复位点落在本块里（或者已经过了：本块从它之后开始，那就在块首复位）。
        outcome.resetDsp = true;
        outcome.resetAtSample = std::max(playResetAt_, processed);
        pendingPlayReset_ = false;
    }
}

void StreamSync::resync(SyncReason reason, LocalLine& line, TapSet& taps, i64 writeHead, i64 processed,
                        SyncOutcome& outcome) noexcept
{
    ++resyncCount_;
    outcome.resynced = true;
    outcome.reason = reason;

    // 旧回送 5 ms 淡出（预拷贝的尾巴）。
    startFade(line, taps, processed, fadeOut_, line.pulled(), outcome);
    outcome.fadeReason = reason;

    // 容忍模式：最近 3 次对齐重同步里最老的那次与本次的间隔 ≤ 10 s 就转容忍模式。
    if (countsTowardTolerant(reason))
    {
        noteResyncTime(processed);
        if (state_ != SyncState::kTolerant &&
            shouldEnterTolerant(resyncTimes_, resyncTimeCount_, resyncCursor_, timing::kTolerantThreshold,
                                tolerantWindow_, processed))
        {
            state_ = SyncState::kTolerant;
            outcome.enteredTolerant = true;
            unstable_ = true;
            sink_.emit(LoopEventCode::kTolerantMode, 1);
            sink_.emit(LoopEventCode::kUnstable, 1);
        }
    }

    // 重新锚定同步偏移：off = W − R。读头在输出下标 i 上读环位置 R + i + off − D。
    offset_ = writeHead - processed;
    driftBlocks_ = 0;
    haveLastDrift_ = false;
    absorbedDrift_ = false;

    sink_.emit(LoopEventCode::kResync, static_cast<std::uint8_t>(reason), 0, processed);
}

SyncOutcome StreamSync::process(const SyncInputs& in, LocalLine& line, TapSet& taps) noexcept
{
    SyncOutcome outcome;

    // 0 样本的调用立刻返回，不推进任何计数
    if (in.blockLength <= 0)
    {
        return outcome;
    }

    const i64 n = static_cast<i64>(in.blockLength);
    const i64 R = in.processed;
    const i64 W = in.writeHead;

    // 宿主违约：送来的块长超过声明值。补 0 的准确个数由 LoopSession 算（它知道读头读到了哪里）。
    if (in.declaredBlock > 0 && n > in.declaredBlock)
    {
        outcome.hostViolation = true;
        outcome.overrunSamples = n - in.declaredBlock;
        sink_.emit(LoopEventCode::kHostViolation, static_cast<std::uint8_t>(n),
                   static_cast<std::uint8_t>(in.declaredBlock), R);
    }

    // 两端都停用：累计样本数，超过 250 ms 就清空本地延迟线（防停用止啸后原样复燃）。
    if (in.inactive)
    {
        inactiveSamples_ += n;
        if (inactiveSamples_ > inactiveClear_)
        {
            clearLine(line, W, outcome);
            offset_ = W - R;
            havePrevious_ = false;
            haveDelta_ = false;
            writeHead_ = W;
        }
    }
    else
    {
        inactiveSamples_ = 0;
    }

    // 没有对端：读不了环，只出干声。活跃读头 10 ms 淡出（读头不会反复读同一段），读位照常前进。
    if (!in.paired)
    {
        startFade(line, taps, R, unpairFade_, line.pulled(), outcome);
        if (outcome.fadeStarted)
        {
            outcome.fadeReason = SyncReason::kBindSwitch;
        }
        state_ = SyncState::kUnpaired;
        lastPaired_ = false;
        havePrevious_ = false;
        haveLastDrift_ = false;
        haveDelta_ = false;
        writeHead_ = W;
        processed_ = R + n;
        notePlayReset(R, n, outcome);
        return outcome;
    }

    // 起播沿：旧回送 T = min(5 ms, D_eff) 淡出，本地延迟线在沿上复位，DSP 的复位点是绝对样本号 R + T。
    //
    // 本地延迟线的复位：沿后的新读头只读 W + T 起的数据（validFrom 由 LoopSession 定），沿前写下的帧
    // 从此不再回来；旧读头只在淡出的 T 个样本里读它最后那一段，然后释放。
    // 旧读头的 validTo 取当前的 W（不是上一块拉到的位置）：它淡出要读的位置 R + off − D 起的 T 帧
    // 一定在 W 之前（T ≤ D），这一块拉环之后都在本地延迟线里。若取上一块拉到的位置，淡出读到多少
    // 就要看宿主上一块切得多长，复位点前后的输出就随切块变了。
    // 全部按样本定位，所以「导出与播放从 s0 + T 起逐位一致」与宿主怎么切块无关。
    // T 取 min(5 ms, D_eff)：D 比 5 ms 还短时淡出不能超过一个环路延迟。
    if (in.playEdge && in.resetOnPlay)
    {
        const std::int64_t T = playFadeSamples(in.delayEffective);
        ++resyncCount_;
        outcome.resynced = true;
        outcome.reason = SyncReason::kPlay;
        startFade(line, taps, R, T, W, outcome);
        outcome.fadeReason = SyncReason::kPlay;

        offset_ = W - R;
        driftBlocks_ = 0;
        haveLastDrift_ = false;
        absorbedDrift_ = false;
        lastPaired_ = true;
        if (state_ != SyncState::kTolerant)
        {
            state_ = SyncState::kStreaming;
        }
        stalledSamples_ = 0;
        frozenBlocks_ = 0;

        pendingPlayReset_ = true;
        playResetAt_ = R + T;
        outcome.resetAtSample = playResetAt_; // 预告：复位点所在的那一块再置 resetDsp
        sink_.emit(LoopEventCode::kResync, static_cast<std::uint8_t>(SyncReason::kPlay), 0, R);

        notePlayReset(R, n, outcome);
        account(W, R, n, W - writeHead_, false);
        return outcome;
    }
    notePlayReset(R, n, outcome);

    // 写头倒退（换段遗漏一类）：LoopSession 已把本地延迟线从 W 起重来，这里重新锚定偏移。
    if (in.domainReset)
    {
        resync(SyncReason::kBindSwitch, line, taps, W, R, outcome);
        lastPaired_ = true;
        if (state_ != SyncState::kTolerant)
        {
            state_ = SyncState::kStreaming;
        }
        account(W, R, n, 0, false);
        return outcome;
    }

    // 换了写方 / 换了采样率：都要重建读头。
    if (in.writerChanged)
    {
        const i64 delta = W - writeHead_;
        resync(SyncReason::kWriter, line, taps, W, R, outcome);
        account(W, R, n, delta, true);
        return outcome;
    }
    if (in.formatChanged)
    {
        const i64 delta = W - writeHead_;
        resync(SyncReason::kFormat, line, taps, W, R, outcome);
        account(W, R, n, delta, true);
        return outcome;
    }

    // 第一次配上：锚定同步偏移。这一块不做对齐判据（还没有基准），但要算进「W 有没有动」的账。
    //
    // Δ 的基准在第一块记成 0：写方的「速率」要到第二块才知道（第一块的 Δ 是它的累计帧数，不是速率）。
    // 于是第二块拿真实的速率去比 0：不一致就重同步一次 —— 那一次是配对时的重新锚定，不是故障。
    const bool firstBlock = !lastPaired_;
    if (firstBlock)
    {
        lastPaired_ = true;
        state_ = SyncState::kStreaming;
        offset_ = W - R;
        havePrevious_ = true;
        haveDelta_ = false;
        lastDelta_ = 0;
        driftBlocks_ = 0;
        haveLastDrift_ = false;
    }

    // 这一块写头前进的帧数。第一块里它是相对上一次见到的 W 的增量（从没见过时相对 0），用来判断写头动没动。
    const i64 delta = W - writeHead_;

    // 断档判据：W 未动且 R 前进 ≥ 2·N_max。推进量不够就还不算断档（判据按样本计）。
    const std::int64_t stallLimit = stallThreshold(in.declaredBlock, in.peerMaxBlock);
    if (delta == 0)
    {
        stalledSamples_ += n;
        ++frozenBlocks_;
        if (frozenBlocks_ > maxFrozenBlocks_)
        {
            maxFrozenBlocks_ = frozenBlocks_;
        }
        if (stalledSamples_ >= stallLimit && state_ != SyncState::kStalled)
        {
            state_ = SyncState::kStalled;
            outcome.stalled = true;
            startFade(line, taps, R, fadeOut_, line.pulled(), outcome);
            outcome.fadeReason = SyncReason::kStallResume;
            sink_.emit(LoopEventCode::kStalled, 1, 0, R);
        }
    }
    else
    {
        if (state_ == SyncState::kStalled)
        {
            // 写头恢复推进：解除断档并重建读头。
            state_ = SyncState::kStreaming;
            sink_.emit(LoopEventCode::kStallCleared, 1, 0, R);
            stalledSamples_ = 0;
            frozenBlocks_ = 0;
            resync(SyncReason::kStallResume, line, taps, W, R, outcome);
            account(W, R, n, delta, true);
            return outcome;
        }
        stalledSamples_ = 0;
        frozenBlocks_ = 0;
    }

    // 断档时、以及这一块写头没动时，不做对齐与漂移的判据：写头不动的时候没有「对齐」可言
    // （W 冻住而 R 照走，m 每块都在变，那不是漂移）。
    if (state_ != SyncState::kStalled && !firstBlock && delta != 0)
    {
        // 漂移：m = W − (R + off)。正数是写头领先。
        const i64 m = drift(W, R, offset_);
        const std::int64_t cap = jumpCap(in.declaredBlock, in.peerMaxBlock);

        if (std::llabs(m) > cap)
        {
            // 跳变：立刻重同步。
            resync(SyncReason::kJump, line, taps, W, R, outcome);
            account(W, R, n, delta, true);
            return outcome;
        }

        // 严格模式：Δ 两次不等就重同步（写方的速率变了）。容忍模式下不做这一判据。
        if (state_ != SyncState::kTolerant && haveDelta_ && alignmentChanged(delta, lastDelta_, havePrevious_))
        {
            resync(SyncReason::kShift, line, taps, W, R, outcome);
            account(W, R, n, delta, true);
            return outcome;
        }

        // 小漂移：先吸收（只记诊断）。漂移持续同一个非零值 8 块，才转成一次重同步重新锚定
        // （有界振荡——错序、可变块长——只影响 m 的这一块值，不改变延迟，不重同步）。
        if (state_ != SyncState::kTolerant && m != 0 && haveLastDrift_ && m == lastDrift_)
        {
            ++driftBlocks_;
            if (driftBlocks_ >= kDriftBlocksBeforeShift)
            {
                resync(SyncReason::kShift, line, taps, W, R, outcome);
                account(W, R, n, delta, true);
                return outcome;
            }
        }
        else
        {
            driftBlocks_ = 0;
        }
        if (m != 0)
        {
            absorbedDrift_ = true;
            ++absorbedDriftBlocks_;
        }
        lastDrift_ = m;
        haveLastDrift_ = true;
    }

    // 第一块不记 Δ 基准（写方的速率从第二块才知道）：第二块立基准，第三块起才比。
    account(W, R, n, delta, !firstBlock);
    return outcome;
}

// 记账：写头与读位推进，Δ 记下这一块的写头增量。
//
// keepDelta 为假时不记 Δ：重同步之后的第一块不该拿「重同步前那一块」当基准来比 Δ
// （重同步重新锚定了偏移，这一块的 Δ 与之前没有可比性）。下一块才有基准。
void StreamSync::account(i64 writeHead, i64 processed, i64 n, i64 delta, bool keepDelta) noexcept
{
    writeHead_ = writeHead;
    processed_ = processed + n;
    havePrevious_ = true;
    if (keepDelta)
    {
        haveDelta_ = true;
        lastDelta_ = delta;
    }
    else
    {
        haveDelta_ = false;
        lastDelta_ = 0;
    }
}

} // namespace rinne::loop
