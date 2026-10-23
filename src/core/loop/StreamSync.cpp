// SPDX-License-Identifier: GPL-3.0-or-later
//
// StreamSync：流相对寻址的状态机（判据那一半）的实现。
//
// 判据全部按样本计，与宿主怎么切块无关：
//   - 断档：W 未动且 R 前进 ≥ 2·N_max → STALLED，旧回送 5 ms 淡出。
//   - 对齐：严格模式下 Δ = W − R（扣掉 off，即漂移量 m）连续两次检查都不为 0 就重同步；|m| 超过跳变门限立刻重同步。
//     一次检查 = 写头在这一块动了的那一次判据。块长变化、Output 晚到一块再补上，m 都回到 0，不重同步。
//   - 容忍模式：10 s 内 ≥ 3 次对齐重同步（跳变、m 连续两次不为 0）→ 转容忍模式并置「时序不稳定」。
//     容忍模式下不再因 m 不为 0 重同步，跳变与断档判据仍然生效（断档要淡出，这是安全侧）。
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
// off 在两次重同步之间保持不动；只出现一次的 m ≠ 0 只记诊断（被吸收），连续两次才重新锚定。
//
// 淡出：重同步、断档、起播沿、对端没了都是同一件事 —— 旧回送线性淡出。判据在拉环之前跑，所以旧读头的读上界
// 取「本块拉环之后有数据的上界」（环在时就是当前的 W），不取上一块拉到的位置：取后者的话，D 只比上一块的块长
// 多不到一个淡出长度时，淡出读到一半就撞上上界，变成一处阶跃，而且淡出多长要看宿主上一块切得多长。
// 淡出要用的那一段里已经拉进来的部分先从延迟线拷进尾巴区（主区可能已被环覆盖），本块拉环之后才到的部分读主区。
// 数据真的不够（环没了、D 比 5 ms 还短）时，淡出缩短到恰好在数据用完时走到 0。淡出是单调的，不产生阶跃。
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
// 计入容忍模式的重同步：只有对齐判据触发的那两种（跳变、m 连续两次不为 0）是「时序不稳定」的证据。
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
    misaligned_ = 0;
    stalledSamples_ = 0;
    frozenBlocks_ = 0;
    maxFrozenBlocks_ = 0;
    inactiveSamples_ = 0;
    inactiveCleared_ = false;
    lastPaired_ = false;
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
// 取全部淡出读头的并集，一段就够（尾巴区是定长的）。尾巴只拷得到已经拉进来的部分（判据在拉环之前跑），
// 本块拉环之后才到的那部分在淡出的几毫秒里留在主区（主区容得下 D 的上限加一个最大块长，几毫秒内不会被覆盖）。
//
// fadeEnd 是旧读头的 validTo：淡出期间不读这个位置及以后。调用方给的是「本块拉环之后有数据的上界」：
// 环还在时是当前的 W（不是上一块拉到的位置），环不在时是已经拉到的位置。
//
// 淡出长度按读头各自算：读头从 from 起读，fadeEnd 之前只剩 fadeEnd − from 个样本时，名义长度装不下，
// 这个读头的淡出就缩短到那么长 —— 恰好在数据用完时走到 0。否则淡出读到一半撞上 fadeEnd，余下的样本读成 0，
// 回送在那一点上从「淡到一半的值」直接掉到 0，是一处阶跃。一个样本都不剩时只能在下一样本归零（至少一步）。
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

    // 预拷贝尾巴：淡出要用的那一段里已经拉进来的部分先拷出来，此后读那一段不再依赖延迟线的主区。
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
        const i64 from = processed + tap.offset - tap.delay;
        const i64 available = tap.validTo - from;
        tap.env.fadeOutSteps = std::clamp<std::int64_t>(available, 1, length);
        tap.env.fadeOutStep = 0;
    }
}

bool StreamSync::fadeForDelayChange(LocalLine& line, TapSet& taps, i64 processed, i64 fadeEnd) noexcept
{
    SyncOutcome outcome;
    startFade(line, taps, processed, fadeOut_, fadeEnd, outcome);
    return outcome.fadeStarted;
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

void StreamSync::resync(SyncReason reason, LocalLine& line, TapSet& taps, i64 writeHead, i64 processed, i64 dataEnd,
                        SyncOutcome& outcome) noexcept
{
    ++resyncCount_;
    outcome.resynced = true;
    outcome.reason = reason;

    // 旧回送 5 ms 淡出（预拷贝的尾巴）。读上界是本块拉环之后有数据的上界（当前的 W），不是上一块拉到的位置。
    startFade(line, taps, processed, fadeOut_, dataEnd, outcome);
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
    misaligned_ = 0;

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
    // 载荷：a:b 是块长 n 的 16 位值（a 高字节、b 低字节）。走到这里的块长不超过 32768（超长块在 LoopSession 里
    // 先被拦下），16 位装得下；声明值 N 不进载荷（状态快照里有下限与原因）。
    if (in.declaredBlock > 0 && n > in.declaredBlock)
    {
        outcome.hostViolation = true;
        outcome.overrunSamples = n - in.declaredBlock;
        const auto n16 = static_cast<std::uint16_t>(std::min<i64>(n, 0xFFFF));
        sink_.emit(LoopEventCode::kHostViolation, static_cast<std::uint8_t>(n16 >> 8),
                   static_cast<std::uint8_t>(n16 & 0xFFu), R);
    }

    // 两端都停用：累计样本数，超过 250 ms 就清空本地延迟线（防停用止啸后原样复燃）。
    // 一段停用只清一次（锁存）：线已经清空，停用期间不会再拉进数据，每块再清一遍白白让提示码灌满诊断队列。
    // 回到活跃（inactive 为假）时解锁，下一段停用超过 250 ms 再清。
    if (in.inactive)
    {
        inactiveSamples_ += n;
        if (inactiveSamples_ > inactiveClear_ && !inactiveCleared_)
        {
            inactiveCleared_ = true;
            clearLine(line, W, outcome);
            offset_ = W - R;
            havePrevious_ = false;
            misaligned_ = 0;
            writeHead_ = W;
        }
    }
    else
    {
        inactiveSamples_ = 0;
        inactiveCleared_ = false;
    }

    // 没有对端：读不了环，只出干声。活跃读头 10 ms 淡出（读头不会反复读同一段），读位照常前进。
    // 读上界是本块拉环之后有数据的上界：对端刚走、环还在时 LoopSession 照常把环拉到 W，淡出读得到它最后写下的那一段。
    if (!in.paired)
    {
        startFade(line, taps, R, unpairFade_, in.dataEnd, outcome);
        if (outcome.fadeStarted)
        {
            outcome.fadeReason = SyncReason::kBindSwitch;
        }
        state_ = SyncState::kUnpaired;
        lastPaired_ = false;
        havePrevious_ = false;
        misaligned_ = 0;
        writeHead_ = W;
        processed_ = R + n;
        notePlayReset(R, n, outcome);
        return outcome;
    }

    // 起播沿：旧回送 T = min(5 ms, D_eff) 淡出，本地延迟线在沿上复位，DSP 的复位点是绝对样本号 R + T。
    //
    // 本地延迟线的复位：沿后的新读头只读 W + T 起的数据（validFrom 由 LoopSession 定），沿前写下的帧
    // 从此不再回来；旧读头只在淡出的 T 个样本里读它最后那一段，然后释放。
    // 旧读头的 validTo 取本块拉环之后有数据的上界，也就是当前的 W（不是上一块拉到的位置）：它淡出要读的位置
    // R + off − D 起的 T 帧一定在 W 之前（T ≤ D），这一块拉环之后都在本地延迟线里。若取上一块拉到的位置，
    // 淡出读到多少就要看宿主上一块切得多长，复位点前后的输出就随切块变了。
    // 全部按样本定位，所以「导出与播放从 s0 + T 起逐位一致」与宿主怎么切块无关。
    // T 取 min(5 ms, D_eff)：D 比 5 ms 还短时淡出不能超过一个环路延迟。
    if (in.playEdge && in.resetOnPlay)
    {
        const std::int64_t T = playFadeSamples(in.delayEffective);
        ++resyncCount_;
        outcome.resynced = true;
        outcome.reason = SyncReason::kPlay;
        startFade(line, taps, R, T, in.dataEnd, outcome);
        outcome.fadeReason = SyncReason::kPlay;

        offset_ = W - R;
        misaligned_ = 0;
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
        account(W, R, n);
        return outcome;
    }
    notePlayReset(R, n, outcome);

    // 写头倒退（换段遗漏一类）：LoopSession 已把本地延迟线从 W 起重来，这里重新锚定偏移。
    if (in.domainReset)
    {
        resync(SyncReason::kBindSwitch, line, taps, W, R, in.dataEnd, outcome);
        lastPaired_ = true;
        if (state_ != SyncState::kTolerant)
        {
            state_ = SyncState::kStreaming;
        }
        account(W, R, n);
        return outcome;
    }

    // 换了段（换组、重新映射）：重建读头。旧读头的淡出只读旧段已经拉进来的数据（in.dataEnd 是已拉到的位置）。
    if (in.ringSwitched)
    {
        resync(SyncReason::kBindSwitch, line, taps, W, R, in.dataEnd, outcome);
        lastPaired_ = true;
        if (state_ != SyncState::kTolerant)
        {
            state_ = SyncState::kStreaming;
        }
        stalledSamples_ = 0;
        frozenBlocks_ = 0;
        account(W, R, n);
        return outcome;
    }

    // 换了写方 / 换了采样率：都要重建读头。换写方是同一个环上的接力（W 是环头里的同一个计数），
    // [上一块拉到的位置, W) 是连续的流，旧读头淡出读得到它。
    if (in.writerChanged)
    {
        resync(SyncReason::kWriter, line, taps, W, R, in.dataEnd, outcome);
        account(W, R, n);
        return outcome;
    }
    if (in.formatChanged)
    {
        resync(SyncReason::kFormat, line, taps, W, R, in.dataEnd, outcome);
        account(W, R, n);
        return outcome;
    }

    // 第一次配上：锚定同步偏移（m 从 0 起）。这一块不做对齐判据（锚定本身就是对齐），但要算进「W 有没有动」的账。
    const bool firstBlock = !lastPaired_;
    if (firstBlock)
    {
        lastPaired_ = true;
        state_ = SyncState::kStreaming;
        offset_ = W - R;
        havePrevious_ = true;
        misaligned_ = 0;
    }

    // 这一块写头前进的帧数：只用来判断写头动没动（断档判据与「这一块算不算一次对齐检查」）。
    // 它不是对齐判据：宿主改块长时它就会变，延迟却没有变。
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
            startFade(line, taps, R, fadeOut_, in.dataEnd, outcome);
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
            resync(SyncReason::kStallResume, line, taps, W, R, in.dataEnd, outcome);
            account(W, R, n);
            return outcome;
        }
        stalledSamples_ = 0;
        frozenBlocks_ = 0;
    }

    // 对齐检查：断档时、以及这一块写头没动时不做（写头不动的时候没有「对齐」可言，W 冻住而 R 照走，
    // m 每块都在变，那是断档判据的事）。写头没动的块对连续计数是透明的：Output 晚到一块再补上时，
    // 冻住的那块不算一次检查，补上之后 m 回到 0，计数清零。
    if (state_ != SyncState::kStalled && !firstBlock && delta != 0)
    {
        // 漂移量：m = W − (R + off)，也就是 Δ = W − R 扣掉同步偏移。正数是写头领先。
        const i64 m = drift(W, R, offset_);
        const std::int64_t cap = jumpCap(in.declaredBlock, in.peerMaxBlock);

        if (std::llabs(m) > cap)
        {
            // 跳变：立刻重同步（容忍模式下也是：跳得这么远，旧的对齐已经没有意义）。
            resync(SyncReason::kJump, line, taps, W, R, in.dataEnd, outcome);
            account(W, R, n);
            return outcome;
        }

        // 严格模式：m 连续两次检查都不为 0 就重同步。容忍模式下照样计数，但不因它重同步。
        misaligned_ = countMisaligned(m, misaligned_);
        if (state_ != SyncState::kTolerant && alignmentLost(misaligned_))
        {
            resync(SyncReason::kShift, line, taps, W, R, in.dataEnd, outcome);
            account(W, R, n);
            return outcome;
        }

        // 只出现一次的 m ≠ 0：先吸收（只记诊断，不改变延迟）。
        if (m != 0)
        {
            ++absorbedDriftBlocks_;
        }
    }

    account(W, R, n);
    return outcome;
}

// 记账：写头与读位推进。
void StreamSync::account(i64 writeHead, i64 processed, i64 n) noexcept
{
    writeHead_ = writeHead;
    processed_ = processed + n;
    havePrevious_ = true;
}

} // namespace rinne::loop
