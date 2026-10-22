// SPDX-License-Identifier: GPL-3.0-or-later
//
// StreamSync：流相对寻址的状态机（判据那一半）的实现。
//
// 判据全部按样本计，与宿主怎么切块无关：
//   - 断档：W 未动且 R 前进 ≥ 2·N_max → STALLED，旧回送 5 ms 淡出。
//   - 对齐：严格模式下 Δ（本块写头的增量）两次不等就重同步。
//   - 容忍模式：10 s 内 ≥ 3 次重同步 → 转容忍模式并置「时序不稳定」。容忍模式下不再因 Δ 变化重同步，
//     断档判据仍然生效（断档要淡出，这是安全侧）。
//   - 两端停用 > 250 ms → 清空本地延迟线（只改整数，不 memset）。
//   - 同采样率重新 prepare → 什么都不动，流保持连续。
//   - reset() → 只记诊断。
//   - 起播（reset_on_play，默认开）：旧回送 5 ms 淡出，淡出走完之后的那一块复位 DSP 与本地延迟线。
//   - 宿主违约（n > N_prep）→ 记一笔（补 0 与抬升 D 下限在 LoopSession 里做）。
//
// 几何：同步偏移 off 的意义是「写方的帧计数比读位 R 多多少」。锚定在某一刻时 off = W − R，
// 于是读头在输出下标 i 上读环位置 R + i + off − D，也就是写方在 D 帧之前写下的那一帧：环路延迟恰为 D。
// off 在两次重同步之间保持不动，中间的漂移只记诊断；漂移连续 8 块不停就转成一次重同步重新锚定。
//
// 淡出：重同步、断档、起播沿都是同一件事 —— 旧回送 5 ms 线性淡出。淡出期间旧读头读的是「预拷贝的尾巴」：
// 淡出要用的那一段在开始淡出之前先从延迟线拷进尾巴区，此后读那一段不再依赖延迟线的主区
// （主区可能已被环覆盖）。淡出是单调的，不产生阶跃。
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
    absorbedDrift_ = false;
    resyncTimeCount_ = 0;
    resyncCursor_ = 0;
    pendingPlayReset_ = false;
    playFadeStarted_ = false;
    playFadeEnd_ = 0;
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

void StreamSync::clearLine(LocalLine& line, SyncOutcome& outcome) noexcept
{
    // 清空只改整数：延迟线清掉（不 memset，几兆字节清零不能在音频线程上做）。
    // 读头由 LoopSession 在本块末统一作废，这样「清空」与「读头作废」发生在同一个时点上。
    line.reset();
    outcome.lineCleared = true;
    sink_.emit(LoopEventCode::kLineCleared, 1);
}

// 旧回送开始 5 ms 淡出：给全部活跃读头挂淡出包络，并把淡出要用的那一段预拷贝进尾巴区。
//
// 预拷贝的位置：淡出期间读头读的是 R + off − D 那一带（本块的下标 i 从 0 开始）。
// 取全部淡出读头的并集，一段就够（尾巴区是定长的）。
void StreamSync::startFade(LocalLine& line, TapSet& taps, i64 processed, SyncOutcome& outcome) noexcept
{
    outcome.fadeStarted = true;

    // 预拷贝尾巴：淡出要用的那一段先拷出来。此后读那一段不再依赖延迟线的主区。
    i64 lo = std::numeric_limits<i64>::max();
    i64 hi = std::numeric_limits<i64>::min();
    for (int i = 0; i < kMaxTaps; ++i)
    {
        const Tap& tap = taps.at(i);
        if (!tap.active)
        {
            continue;
        }
        const i64 from = processed + tap.offset - tap.delay;
        lo = std::min(lo, from);
        hi = std::max(hi, from + fadeOut_);
    }
    if (hi > lo)
    {
        line.snapshotRange(lo, hi - lo);
    }

    // 淡出：包络从 1 线性降到 0；validTo 收到「已拉到的最后一帧」，淡出期间不再读新位置。
    const i64 fadeEnd = line.pulled();
    for (int i = 0; i < kMaxTaps; ++i)
    {
        Tap& tap = taps.at(i);
        if (!tap.active)
        {
            continue;
        }
        tap.validTo = std::min(tap.validTo, fadeEnd);
        tap.env.fadeOutSteps = fadeOut_;
        tap.env.fadeOutStep = 0;
    }
}

void StreamSync::resync(SyncReason reason, LocalLine& line, TapSet& taps, i64 writeHead, i64 processed,
                        SyncOutcome& outcome) noexcept
{
    ++resyncCount_;
    outcome.resynced = true;
    outcome.reason = reason;

    // 旧回送 5 ms 淡出（预拷贝的尾巴）。
    startFade(line, taps, processed, outcome);
    outcome.fadeReason = reason;

    // 容忍模式：最近 3 次重同步里最老的那次与本次的间隔 ≤ 10 s 就转容忍模式。
    noteResyncTime(processed);
    if (state_ != SyncState::kTolerant && shouldEnterTolerant(resyncTimes_, resyncTimeCount_, resyncCursor_,
                                                            timing::kTolerantThreshold, tolerantWindow_, processed))
    {
        state_ = SyncState::kTolerant;
        outcome.enteredTolerant = true;
        unstable_ = true;
        sink_.emit(LoopEventCode::kTolerantMode, 1);
        sink_.emit(LoopEventCode::kUnstable, 1);
    }

    // 重新锚定同步偏移：off = W − R。读头在输出下标 i 上读环位置 R + i + off − D。
    offset_ = writeHead - processed;
    driftBlocks_ = 0;
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

    // 宿主违约：送来的块长超过声明值。补 0 的准确个数由 LoopSession 算（它知道读到了多少帧）。
    if (in.declaredBlock > 0 && n > in.declaredBlock)
    {
        outcome.hostViolation = true;
        outcome.overrunSamples = n - in.declaredBlock;
        sink_.emit(LoopEventCode::kHostViolation, static_cast<std::uint8_t>(n),
                   static_cast<std::uint8_t>(in.declaredBlock), R);
    }

    // 起播沿：旧回送 5 ms 淡出。淡出走完之后的下一块复位 DSP 与本地延迟线。
    //
    // 淡出的长度 T = min(5 ms, D_eff)：D 比 5 ms 还短时淡出不能超过一个环路延迟，
    // 否则淡出走完之后回送还没淡完，复位会把还在淡出的读头一起作废。
    // 复位点给的是绝对样本号（与 R 同一坐标）：引擎在那个样本上执行 DSP 复位，
    // 于是「导出与播放从 s0 + T 起逐位一致」有样本级的依据。
    if (in.playEdge && in.resetOnPlay)
    {
        startFade(line, taps, R, outcome);
        outcome.fadeReason = SyncReason::kPlay;
        pendingPlayReset_ = true;
        playFadeStarted_ = true;
        playFadeEnd_ = R + playFadeSamples(in.delayEffective);
        outcome.resetAtSample = playFadeEnd_;
    }

    // 起播的淡出走完了吗：走完了就在这一块复位（读头已经是空的）。
    if (pendingPlayReset_ && R >= playFadeEnd_)
    {
        clearLine(line, outcome);
        pendingPlayReset_ = false;
        playFadeStarted_ = false;
        outcome.resetDsp = true;
        outcome.resynced = true;
        outcome.reason = SyncReason::kPlay;
        state_ = SyncState::kStreaming;
        // 复位之后偏移重新锚定：新读位从 0 开始算
        offset_ = W - R;
        havePrevious_ = false;
        haveDelta_ = false;
        writeHead_ = W;
        // R 照常前进：读位是「已处理的帧数」，这一块也处理了。
        processed_ = R + n;
        return outcome;
    }

    // 两端都停用：累计样本数，超过 250 ms 就清空本地延迟线（防停用止啸后原样复燃）。
    if (in.inactive)
    {
        inactiveSamples_ += n;
        if (inactiveSamples_ > inactiveClear_)
        {
            clearLine(line, outcome);
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

    // 没有对端：读不了环，只出干声。不推进读位，也不做判据。
    if (!in.paired)
    {
        state_ = SyncState::kUnpaired;
        lastPaired_ = false;
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
    }

    // 这一块写头前进的帧数。第一块里它是相对 0 的累计值（用来判断写头动没动）。
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
            startFade(line, taps, R, outcome);
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
            resync(SyncReason::kStallResume, line, taps, W, R, outcome);
            account(W, R, n, delta, true);
            return outcome;
        }
        stalledSamples_ = 0;
        frozenBlocks_ = 0;
    }

    // 断档时不做对齐与漂移的判据：写头不动的时候没有「对齐」可言。
    if (state_ != SyncState::kStalled && !firstBlock)
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

        if (state_ != SyncState::kTolerant && m != 0)
        {
            // 小漂移：先吸收（只记诊断），连续 8 块不停就转成一次重同步重新锚定。
            ++driftBlocks_;
            if (driftBlocks_ >= kDriftBlocksBeforeShift)
            {
                resync(SyncReason::kShift, line, taps, W, R, outcome);
                account(W, R, n, delta, true);
                return outcome;
            }
            absorbedDrift_ = true;
            ++absorbedDriftBlocks_;
        }
        else if (m == 0)
        {
            driftBlocks_ = 0;
        }
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
