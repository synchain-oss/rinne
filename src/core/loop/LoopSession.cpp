// SPDX-License-Identifier: GPL-3.0-or-later
//
// LoopSession：环路层的门面。参数与走带整体下传、D 的计算、拉环、读头混合、违约路径、状态发布都在这里。
//
// 一块的流程：
//   1. 算 D：用户要的（毫秒或节拍）→ 样本 → 抬到下限（max(N_prep, N_obs, n)）→ 截到上限。
//   2. 域锚定：第一次配上或写头倒退时，本地延迟线从当前写头 W 起重来（比 W 早的帧一律作废）。
//   3. 判据：交给 StreamSync（断档、对齐、容忍、重同步、停用清空、起播复位）。
//   4. 拉环：把环里 [pulled, W) 的帧全部搬进本地延迟线（一圈之内），pulled 追到 W。
//   5. 建新读头（需要时），把读头按包络混成一路，写进 ctx.returnLeft / ctx.returnRight，并标出逐样本的有效位。
//   6. 判错序：读对端槽的宿主时间戳，每块判一次（口径见 OrderEvidence.h 与 ADR-0009）。
//   7. 导出不一致：离线块里出了导出可能与播放不同的事，计一次（口径见 ExportLatch.h）。
//   8. 发布状态：写进 LoopStatus（原子量），消息线程读快照。
//
// 读头的生命周期（3 个槽）：
//   稳态      1 个读头在跑，包络恒为 1。
//   淡出期间  旧读头在淡出（5 ms），新读头同时在 onset（20 ms）：两个槽。
//   重同步    旧读头淡出 + 新读头 onset：两个槽。
//   改 D      40 ms 双读头线性交叉淡化：旧读头增益 1 − x、新读头增益 x，两路之和恒为 1，两个槽。
//             同一时刻至多一段；受 100 ms 前沿限速（第一次立即开始，窗口内的改动合并，窗口到点再向最新目标淡一次）。
//             交叉淡化进行中来了重同步，两个读头各自再挂 5 ms 淡出一起淡进尾巴，新读头占第三个槽。
//   槽满      淡化或前一次重同步的淡出还没走完又来重同步，要第 4 个读头：收掉包络最小的那个并记一次 tapDrop；
//             它在上一个样本上的值接成 5 ms 线性衰减的末值保持，回送不阶跃（见 newTap）。
//
// 读位 R 只有一份：判据那一半（StreamSync）持有它。本块的读位起点 R0 在判据之前取一次，
// 混音、漂移量与逐样本的位置全部用这同一个 R0；判据之后 R 已推进到 R0 + n，那是下一块的起点。
// 两份 R（或混音用推进之后的 R）是「漂移量 m 算错」与「环路延迟变成 D − n」的典型来源。
//
// 实时约定：不分配、不加锁、不读墙钟、不抛异常；0 样本的调用立刻返回，不推进任何计数。
#include "loop/LoopSession.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "engine/TransportInfo.h"
#include "ipc/LoopRing.h"

namespace rinne::loop
{

namespace
{
// 读头的 onset（20 ms）之后包络恒为 1。onset 的长度由 StreamSync 按采样率算好。
constexpr std::int64_t kOnsetFloor = 1;

// 「没有上界」：读头 validTo 的哨兵值。
constexpr std::int64_t kNoUpperBound = std::numeric_limits<std::int64_t>::max();

// 拉环的护栏 G：写方最多有一段（≤ 8192 帧）公告了还没发布，比 W 早 (一圈 − G) 帧以上的位置可能正被覆盖。
// 取 max(8192, 对端声明的最大块长)，截在 [8192, 32768]。
constexpr std::int64_t kMinPullGuard = 8192;
constexpr std::int64_t kMaxPullGuard = 32768;
} // namespace

bool LoopSession::prepare(double sampleRate) noexcept
{
    const bool changed = !prepared_ || sampleRate_ != sampleRate;
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;

    if (changed)
    {
        // 换采样率：延迟线重新分配，域状态重来
        line_.allocate(sampleRate_);
        taps_.clear();
        hold_ = ReturnHold{};
        lastReturnAt_ = -1;
        xfadeLen_ = xfadeSamplesFor(sampleRate_);
        retarget_.prepare(sampleRate_);
        retargetHeld_ = false;
        syncFilter_.reset();
        delayHints_ = 0;
        offset_ = 0;
        delayTarget_ = 0;
        delayInUse_ = 0;
        delayMin_ = 0;
        delayRequested_ = 0;
        havePeer_ = false;
        lastWriterToken_ = 0;
        lastEpoch_ = 0;
        tapDropCount_ = 0;
        haveExportPeerSeq_ = false;
        lastExportPeerSeq_ = 0;
    }
    // 错序证据自己判断要不要重来（换采样率才清；同采样率重新 prepare 证据保持连续）
    order_.prepare(sampleRate_);
    // N_obs 每次 prepare 清零（同采样率也清）：下限因此回落到声明值。
    maxBlockSeen_ = 0;
    delayMinReason_ = DelayMinReason::kNone;

    // 拉环的暂存：定长，容得下支持的最大块长。音频线程上不再分配（一次拉不完就分段拉）。
    const std::size_t stage = static_cast<std::size_t>(ipc::kMaxSupportedBlock);
    if (pullLeft_.size() != stage)
    {
        pullLeft_.assign(stage, 0.0f);
        pullRight_.assign(stage, 0.0f);
    }

    sync_.prepare(sampleRate_);
    prepared_ = true;
    return line_.ready();
}

void LoopSession::setEventSink(LoopEventSink sink) noexcept
{
    sink_ = sink;
    sync_.setEventSink(sink);
}

// D 的目标值：按参数里的模式（时间 / 节拍）算成样本，再落到合法范围。
//
// 毫秒与节拍到样本的换算全在环路层做（引擎不预先把 delay_ms 换算成样本）：节拍同步因此只需要改环路层。
// 下限 = max(N_prep, N_obs)，其中 N_obs 取引擎给的值与本块块长 n 里较大的：宿主违约（n > N_prep）的那一块
// D 下限就立即抬到 n，不等引擎下一块再报。
std::int64_t LoopSession::computeDelay(const BlockContext& ctx) noexcept
{
    const engine::EngineParams& params = ctx.params;
    const double sr = sampleRate_ > 0.0 ? sampleRate_ : 48000.0;

    std::int64_t requested = delayRequested_;
    bool noTempo = false;
    if (params.delayMode == engine::DelayMode::kSync)
    {
        // 节拍同步：拍数 × 60/BPM × sr（速度由 updateBpm 在本块开头记下：无效时沿用最后一个有效值，
        // 从没拿到过才回退 120 并置「没有速度」提示）。
        // 精确值与当前目标相差不到 1 个样本时目标不动（抗抖，见 BeatSync.h）。
        const int note = noteIndexOrDefault(static_cast<int>(params.delayNote));
        const double exact = syncDelayExact(note, tempo_.bpm(), sr);
        const std::int64_t frames = syncFilter_.settle(exact, note);
        requested = frames > 0 ? frames : 1;
        noTempo = tempo_.fallback();
    }
    else
    {
        // 时间模式：毫秒
        syncFilter_.reset();
        requested = msToFrames(static_cast<double>(params.delayMs), sr);
    }

    const std::int64_t n = ctx.blockLength;
    maxBlockSeen_ = std::max({maxBlockSeen_, ctx.observedBlock, n});
    const DelayChoice choice = chooseDelay(requested, ctx.declaredBlock, maxBlockSeen_, sr);

    // 下限从哪来（界面常驻「实际 / 最小 / 原因 / 本会话最大块长」里的原因；与用户的 D 是否高于下限无关）：
    //   见过的块比声明值长 → 宿主违约（声明值未知时只算实测块长）；否则就是声明值。
    DelayMinReason floor = DelayMinReason::kNone;
    if (maxBlockSeen_ > ctx.declaredBlock)
    {
        floor = ctx.declaredBlock > 0 ? DelayMinReason::kHostViolation : DelayMinReason::kObserved;
    }
    else if (ctx.declaredBlock > 0)
    {
        floor = DelayMinReason::kPrepare;
    }

    // 只有 D 被抬到下限（实际比要的大）才发 D_min 抬升；被上限截断的不是下限的事，走提示位。
    if (choice.effective > choice.requested && choice.effective != delayTarget_)
    {
        // 载荷：a = D_min ÷256（向上取整、钳 255），b = 原因；准确值在状态快照的 delayMin 里。
        sink_.emit(LoopEventCode::kDMinRaised, eventUnits256(choice.minimum), static_cast<std::uint8_t>(floor));
    }

    // 提示位：节拍模式没有速度、要的 D 超过上限被截断。只在变化时发一条。
    const std::uint32_t hints = (noTempo ? kDelayHintNoTempo : 0u) | (choice.clipped ? kDelayHintClipped : 0u);
    if (hints != delayHints_)
    {
        sink_.emit(LoopEventCode::kDelayHints, static_cast<std::uint8_t>(hints),
                   static_cast<std::uint8_t>(hints ^ delayHints_), sync_.processed());
        delayHints_ = hints;
    }

    delayRequested_ = requested;
    delayMin_ = choice.minimum;
    delayTarget_ = choice.effective;
    delayMinReason_ = floor;
    return choice.effective;
}

// 速度：宿主给的有效值、来源。宿主没给（或给的值超出 20..999）就沿用本会话最后一个有效值，
// 从没拿到过就回退到 120 并记成 fallback。D 用的是 TempoTracker 里的原值；这里的百分之一拍/分钟只给显示。
void LoopSession::updateBpm(const BlockContext& ctx) noexcept
{
    const double bpm = tempo_.observe(ctx.transport.bpm);
    const auto centi = static_cast<std::uint32_t>(std::lround(bpm * 100.0));
    const auto source = static_cast<std::uint8_t>(tempo_.fallback() ? BpmSource::kFallback : BpmSource::kHost);
    bpmCenti_ = centi;
    bpmSource_ = source;
    status_.setBpm(centi, source);
}

void LoopSession::flushLocalDelayLine() noexcept
{
    // 跳闸淡出后清空：只改两个整数，不 memset（几兆字节清零不能在音频线程上做）。
    // 起点锚在已拉到的位置：之后从那里往后拉，比它早的帧一律读不出来。
    line_.resetTo(line_.pulled());
    taps_.clear();
    hold_ = ReturnHold{};
}

// 两端停用超过 250 ms 之后的清空（块首、本块的 process 之前）：清线、作废读头，上一块最后一个回送样本按末值保持
// 衰减 5 ms（见 holdLastReturn）。
void LoopSession::flushAfterSuspend() noexcept
{
    flushLocalDelayLine();
    holdLastReturn();
    sink_.emit(LoopEventCode::kLineCleared, 2);
}

// 读头整体作废时（两端停用之后的清空、写头倒退）：上一块最后一个回送样本按末值保持衰减 5 ms，回送不从它直接落到 0。
// 保持的是上一块实际送出的那一个值（含正在进行的保持），所以先清掉旧的保持再起新的，不重复计入。
// 上一个输出样本不在 R − 1 上（还没出过回送、或超长块打断了读位）时没有可保持的。
void LoopSession::holdLastReturn() noexcept
{
    const bool adjacent = lastReturnAt_ >= 0 && lastReturnAt_ == sync_.processed() - 1;
    hold_ = ReturnHold{};
    if (adjacent)
    {
        hold_.start(lastReturnLeft_, lastReturnRight_, sync_.fadeOutSamples());
    }
}

// 本块的第一个样本就读不到数据、而上一个输出样本还在出声的读头：写头恰好在块边界落后（补 0、断档、失去对端的淡出
// 恰好从块边界开始），块内没有样本可淡。它在上一个样本上的贡献并进末值保持（5 ms 线性衰减，跨块延续），
// 回送不从上一块的末值直接落到 0。上一个样本不是它读的（lastAt 不相邻）或者贡献是 0 时没有可保持的。
void LoopSession::holdStrandedTaps(i64 blockStart) noexcept
{
    for (int slot = 0; slot < kMaxTaps; ++slot)
    {
        Tap& tap = taps_.at(slot);
        if (!tap.active || tap.lastAt != blockStart - 1 || (tap.lastLeft == 0.0f && tap.lastRight == 0.0f))
        {
            continue;
        }
        const i64 p = blockStart + tap.offset - tap.delay;
        float l = 0.0f;
        float r = 0.0f;
        if (p >= tap.validFrom && p < tap.validTo && (line_.readTail(p, l, r) || line_.read(p, l, r)))
        {
            continue; // 块首还读得到：它自己的淡出接得上
        }
        hold_.start(tap.lastLeft, tap.lastRight, sync_.fadeOutSamples());
        tap.lastAt = -1;
    }
}

// 建一个新的读头：validFrom 是环上「从哪一帧起才有数据」，包络走 onset。
//
// 槽满了就收掉包络最小的那个，并记一次 tapDrop。读头只有 3 个槽，而淡化中（或上一次重同步的 5 ms 淡出里）连续来重同步，
// 会同时要旧 A、旧 B、上一次建的读头和这一次的新读头，没有办法一个不丢。取舍：
//   - 收掉的是包络最小的读头：这时最新的读头至多走了 5 ms 的 20 ms onset（或 40 ms 交叉淡化的头 5 ms），
//     所以被收掉的增益不超过 1/4，实际上多半是还没读到数据的新读头（增益 0）；
//   - 回送不阶跃：它在上一个样本上的贡献并进末值保持，从那个值起 5 ms 线性衰减（与写头落后时同一套办法）。
//     被收掉的只是它之后那几毫秒的内容，换成了衰减的常数；
//   - 事件的 b 带被收掉读头的包络（×255），诊断里看得到丢的是多大的份额。
// 其余读头不动，也没有谁的尾巴被覆盖（尾巴格跟读头槽走，见 StreamSync::startFade）。
// blockStart 是本块的读位起点（上一个样本是 blockStart − 1）。
int LoopSession::newTap(i64 blockStart, i64 validFrom, i64 delay, std::int64_t onset) noexcept
{
    int slot = taps_.allocate();
    if (slot < 0)
    {
        slot = taps_.smallestEnvelopeSlot();
        ++tapDropCount_;
        if (slot < 0)
        {
            sink_.emit(LoopEventCode::kTapDropped, 1, 0, blockStart);
            return -1;
        }
        const Tap& victim = taps_.at(slot);
        if (victim.lastAt == blockStart - 1)
        {
            hold_.start(victim.lastLeft, victim.lastRight, sync_.fadeOutSamples());
        }
        const float env = std::clamp(taps_.envelopeNow(victim), 0.0f, 1.0f);
        sink_.emit(LoopEventCode::kTapDropped, 1, static_cast<std::uint8_t>(std::lround(env * 255.0f)), blockStart);
        taps_.at(slot) = Tap{};
    }
    Tap& tap = taps_.at(slot);
    tap.active = true;
    tap.offset = offset_;
    tap.delay = delay;
    tap.validFrom = validFrom;
    tap.validTo = kNoUpperBound;
    tap.env.fadeOutSteps = 0;
    tap.env.fadeOutStep = 0;
    if (onset > kOnsetFloor)
    {
        tap.env.fadeInSteps = onset;
        tap.env.fadeInStep = 0;
    }
    return slot;
}

// 改 D：40 ms 线性交叉淡化。
//
// 主读头的 D 不当场改写：那样读位置跳变，回送里是一处阶跃（宿主违约当场抬 D 下限、节拍模式下 BPM 变了也走到这里）。
// 做法：再开一个读头，按新的 D 读同一条流；旧读头增益 1 − x、新读头增益 x（x 从 0 线性升到 1，总长 40 ms），
// 两路增益之和逐样本恰为 1。走完时旧读头退场，新读头成为普通读头。读位置不跳变，相关信号上也不过冲。
//
// 100 ms 前沿限速（RetargetLimiter）：第一次改动立即开始；之后每 100 ms 至多开始一次。窗口里的改动不记中间值，
// 每一块都拿当时的目标去比，窗口到点那个样本上目标与当前的 D 不同，就从那个样本起向最新目标淡一次。
// 起点可以落在块中间（偏移 at），读头集合的交叉淡化步数从 −at 起算：块内 at 之前新读头 0、旧读头 1。
// 窗口比交叉淡化长，所以上一段一定先走完，同一时刻至多一段。
//
// 新读头的 validFrom 取「它在起点读的第一个位置」与旧读头的 validFrom 里较晚的那个：起播沿、换写方定下的下界照旧
// （沿前写下的帧不会被新读头读回来）。新位置落在有数据的起点之前（刚配对、D 变大）时，新读头要等数据出现，
// 这时给它 20 ms onset，数据出现时不会以现有的交叉淡化增益突然冒出来；数据早就有时不给 onset，
// 否则它与交叉淡化增益相乘，两路之和就不是 1 了。
//
// 读头槽没有空位（交叉淡化进行中又遇重同步，占满 3 个）时新目标先挂着，下一块再试。
// 对外发布的实际 D（状态快照、Input 槽的 d_eff）取主读头的 D：交叉淡化一开始就是新的 D；
// 要覆盖两个读头的量（估计窗口）看 estimatorDelay，它在这 40 ms 里取较大的那个。
// 挂着的那几块里旧读头的 D 可能低于刚抬上去的下限（宿主违约）：那时它读到还没写到的位置，由「写头落后时先淡到 0」接住。
bool LoopSession::startDelayCrossfade(i64 blockStart, i64 n, i64 delay) noexcept
{
    const int mainSlot = taps_.mainSlot();
    if (mainSlot < 0 || taps_.at(mainSlot).delay == delay)
    {
        retargetHeld_ = false;
        return false;
    }
    const i64 at = retarget_.startOffset(n);
    if (at < 0)
    {
        if (!retargetHeld_)
        {
            retargetHeld_ = true;
            sink_.emit(LoopEventCode::kRetargetHeld, 1, 0, blockStart);
        }
        return false;
    }
    if (taps_.crossfadeActive())
    {
        return false; // 上一段还没走完（限速窗口比它长，只在重同步打乱读头时才会碰到）
    }
    if (taps_.count() >= kMaxTaps)
    {
        return false;
    }

    Tap& current = taps_.at(mainSlot);
    const i64 firstPosition = blockStart + at + current.offset - delay;
    const i64 validFrom = std::max(current.validFrom, firstPosition);
    const std::int64_t onset = current.validFrom > firstPosition ? sync_.onsetSamples() : 0;
    const int slot = newTap(blockStart, validFrom, delay, onset);
    if (slot < 0)
    {
        return false;
    }
    Tap& incoming = taps_.at(slot);
    incoming.offset = current.offset;
    incoming.xfade = XfadeRole::kIncoming;
    current.xfade = XfadeRole::kOutgoing;
    taps_.beginCrossfade(xfadeLen_, -at);
    retarget_.noteStart(at);
    retargetHeld_ = false;
    sink_.emit(LoopEventCode::kResync, static_cast<std::uint8_t>(ResyncReason::kDChange), 0, blockStart + at);
    return true;
}

// 超长块（n > 32768）：环路的时间线在这一块不走 —— 读位 R、读头的包络与判据都不动（Output 那一端同样不写环，
// 写头也不动），回送缓冲也只容得下支持的最大块长。正在响的回送不在块首直接掉到
// 0：读头从本块的读位起点照常往下读，在开头淡到 0 （名义 5
// ms，主读头的数据不够时缩短到恰好用完），写进回送缓冲的开头，其余的样本是 0；引擎再把回送增益在这一块里 滑到
// 0，下一块从 0 起淡回。这一块只读、不推进：回到支持的块长之后从同一个读位接着读（淡出读过的那几毫秒那时 从增益 0
// 起淡回来，不是旧数据，是同一段流）。 同一个段、同一个写方还在、写头没有倒退时先把环拉到
// W（下一块本来也要拉这一段）：淡出读得到上一周期刚写下的帧， D
// 只比块长多一点时也走得满。换了段、换了写方或写头倒退时不拉，只用已经拉进来的数据（判据留给下一块）。
std::int64_t LoopSession::fadeOutUnsupportedBlock(const BlockContext& ctx) noexcept
{
    if (ctx.returnLeft == nullptr || ctx.returnRight == nullptr)
    {
        return 0;
    }
    if (ctx.ring != nullptr && havePeer_ && ctx.ring == lastRing_ && ctx.ringGeneration == lastRingGeneration_)
    {
        const ipc::RingFormat format = ipc::LoopRing::readFormat(*ctx.ring);
        const auto writeHead = static_cast<i64>(ipc::LoopRing::writeHead(*ctx.ring));
        if (format.writerToken == lastWriterToken_ && format.epoch == lastEpoch_ && writeHead >= line_.pulled())
        {
            pullRing(*ctx.ring, writeHead, peerMaxBlock_ > 0 ? peerMaxBlock_ : ctx.peerMaxBlock);
        }
    }

    // 淡出的长度：名义 5 ms；没在淡出的读头（交叉淡化里是两个）从块首起只剩不到 5 ms 的数据时，
    // 缩短到最短那个恰好在数据用完时走到 0。
    // 在淡出的读头自己的淡出恰好在它的数据用完时走到 0，再乘上这一段淡出照样连续，不必算进来。
    // 没有读头时回送本来就是 0。
    // 读头从块首起一个可读的样本都没有（写头恰好在块边界落后）时，它在上一个样本上的贡献交给末值保持，
    // 与正在进行的保持一起在这一块里走完（保持名义 5 ms，超长块总比它长）。
    const i64 R0 = sync_.processed();
    holdStrandedTaps(R0);
    if (taps_.count() == 0 && !hold_.active())
    {
        return 0;
    }
    i64 length = sync_.fadeOutSamples();
    for (int slot = 0; slot < kMaxTaps; ++slot)
    {
        const Tap& tap = taps_.at(slot);
        if (!tap.active || tap.env.fadingOut())
        {
            continue;
        }
        const i64 from = R0 + tap.offset - tap.delay;
        const i64 lo = std::max({from, tap.validFrom, line_.lineValidFrom()});
        const i64 hi = std::min(tap.validTo, line_.pulled());
        if (hi > lo)
        {
            length = std::min(length, hi - from);
        }
    }
    const i64 total = std::max(length, hold_.remaining());
    mixTaps(taps_, line_, R0, ctx.returnLeft, ctx.returnRight, length, ctx.returnValid);
    for (i64 i = 0; i < length; ++i)
    {
        const float g = static_cast<float>(length - i) / static_cast<float>(length);
        ctx.returnLeft[i] *= g;
        ctx.returnRight[i] *= g;
    }
    for (i64 i = length; i < total; ++i)
    {
        ctx.returnLeft[i] = 0.0f;
        ctx.returnRight[i] = 0.0f;
        if (ctx.returnValid != nullptr)
        {
            ctx.returnValid[i] = 0;
        }
    }
    hold_.addInto(ctx.returnLeft, ctx.returnRight, total);
    // 这一块不推进读位：读头记下的「最后一个样本」与上一块的末样本都不再与下一块相邻
    for (int slot = 0; slot < kMaxTaps; ++slot)
    {
        taps_.at(slot).lastAt = -1;
    }
    lastReturnAt_ = -1;
    return total;
}

// 拉环：把 [pulled, W) 的帧搬进本地延迟线。
//
// 下界：比 W 早 (一圈 − G) 帧以上的位置可能正被写方的下一段覆盖，不读，记 overflow 并抬高有效区间的下界。
// 撕裂：读完一段之后读写方的公告 intent，intent − 一圈之前的帧可能是半截数据，同样作废并记 torn。
// 环里的帧先落在暂存里（定长，prepare 里分配），再一次搬进延迟线：延迟线的 pull 是按区间搬的。
void LoopSession::pullRing(const ipc::RingSegment& ring, i64 writeHead, i64 peerMaxBlock) noexcept
{
    const i64 lap = static_cast<i64>(ipc::kRingFrames);
    const i64 guard = std::clamp<i64>(std::max<i64>(kMinPullGuard, peerMaxBlock), kMinPullGuard, kMaxPullGuard);

    i64 from = line_.pulled();
    if (writeHead <= from)
    {
        return;
    }
    const i64 earliest = writeHead - (lap - guard);
    if (earliest > from)
    {
        const auto dropped = static_cast<std::uint64_t>(earliest - from);
        status_.addOverflow(dropped);
        sink_.emit(LoopEventCode::kOverflow, static_cast<std::uint8_t>(std::min<std::uint64_t>(dropped, 255u)));
        line_.raiseValidFrom(earliest);
        from = earliest;
    }

    const auto stage = static_cast<i64>(pullLeft_.size());
    while (from < writeHead && stage > 0)
    {
        const i64 to = std::min(writeHead, from + stage);
        const i64 base = from;
        const ipc::RingPull pulled = ipc::LoopRing::pull(ring, base, to, [&](i64 p, float l, float r) {
            const auto at = static_cast<std::size_t>(p - base);
            pullLeft_[at] = l;
            pullRight_[at] = r;
        });
        // validFrom 之前的帧可能已被覆盖或只写了一半：一律当 0，并把有效区间的下界抬上去（绝不读旧数据）。
        const i64 validFrom = pulled.validFrom;
        line_.pull(base, to, [&](i64 p, float& l, float& r) {
            if (p < validFrom)
            {
                l = 0.0f;
                r = 0.0f;
                return;
            }
            const auto at = static_cast<std::size_t>(p - base);
            l = pullLeft_[at];
            r = pullRight_[at];
        });
        if (validFrom > base)
        {
            const auto torn = static_cast<std::uint64_t>(std::min(validFrom, to) - base);
            status_.addTorn(torn);
            sink_.emit(LoopEventCode::kTorn, static_cast<std::uint8_t>(std::min<std::uint64_t>(torn, 255u)));
            line_.raiseValidFrom(std::min(validFrom, to));
        }
        from = to;
    }
}

// 一个读头本块里补 0 的样本数：位置 s = R0 + i + off − D 落在读头的有效区间里，但还没被拉进来（s ≥ pulled）。
std::int64_t LoopSession::tapOverrun(const Tap& tap, i64 blockStart, i64 n) const noexcept
{
    const i64 base = blockStart + tap.offset - tap.delay;
    const i64 lo = std::max({base, line_.pulled(), tap.validFrom});
    const i64 hi = std::min(base + n, tap.validTo);
    return hi > lo ? hi - lo : 0;
}

// 主读头本块里补 0 的样本数。主读头见 TapSet::mainSlot（没在淡出、也不是交叉淡化里的旧读头）；本块没有主读头时不计。
std::int64_t LoopSession::mainTapOverrun(i64 blockStart, i64 n) const noexcept
{
    const int slot = taps_.mainSlot();
    return slot >= 0 ? tapOverrun(taps_.at(slot), blockStart, n) : 0;
}

// 没在淡出的读头本块会读到还没写到的位置：让它在还读得到的样本里线性淡到 0（读上界收到已拉到的位置）。
// 剩下的样本数就是淡出的长度（至少一步），淡出在数据用完的那一点恰好走到 0，之后补的 0 与它相接，没有阶跃。
// 淡出的长度不超过本块的块长，本块末这个读头就释放了。交叉淡化里的旧读头也算：宿主违约当场抬高 D 下限时，
// 要淡的恰恰是 D 偏小的旧读头；它的淡出与交叉淡化增益相乘，仍然连续。
//
// 写头落后恰好从块边界开始（本块一个可读的样本都没有）时，块内没有样本可淡：读头本块不出声、块末释放，
// 它在上一块最后一个样本上的贡献由 holdStrandedTaps 交给末值保持（混音之前统一做）。
void LoopSession::fadeTapsAtUnderrun(i64 blockStart, i64 n) noexcept
{
    for (int slot = 0; slot < kMaxTaps; ++slot)
    {
        Tap& tap = taps_.at(slot);
        if (!tap.active || tap.env.fadingOut() || tapOverrun(tap, blockStart, n) <= 0)
        {
            continue;
        }
        const i64 from = blockStart + tap.offset - tap.delay;
        tap.validTo = std::min(tap.validTo, line_.pulled());
        tap.env.fadeOutSteps = std::max<std::int64_t>(tap.validTo - from, 1);
        tap.env.fadeOutStep = 0;
    }
}

// 对端槽的事实：宿主时间戳、块序号、实时标志、对端声明的最大块长。
//
// 会合段按组块排布，组号 0 或越界时不去读：那会越界访问。
// 读不到就当没有对端槽：判据退回 unknown，绝不猜。
// 读序：先 acquire 读块序号，再读其余字段。写方（publishToSlot）先写时间戳、标志，最后才 release 块序号，
// 对端 acquire 读到某个块序号，就保证看得见这个序号之前写下的时间戳；反过来先读时间戳，读到的可能是上一块的。
void LoopSession::readPeerSlot(const BlockContext& ctx) noexcept
{
    havePeerSlot_ = false;
    peerHostNs_ = 0;
    peerBlockSeq_ = 0;
    peerRtFlags_ = 0;
    peerMaxBlock_ = 0;

    // 换组：旧组的槽记下的块序号与时间戳作废。新组的槽里可能是上一个会话留下的旧值，第一次读到只记块序号。
    if (ctx.group != peerGroup_)
    {
        peerGroup_ = ctx.group;
        haveExportPeerSeq_ = false;
        order_.forgetPeer();
    }

    if (ctx.rendezvous == nullptr || ctx.group == 0 || ctx.group > ipc::kGroupCount)
    {
        return;
    }

    const ipc::RoleSlot& peer = ipc::roleSlot(*ctx.rendezvous, ctx.group, ipc::peerOf(ctx.role));
    peerBlockSeq_ = peer.block_seq.load(std::memory_order_acquire);
    peerHostNs_ = peer.last_host_ns.load(std::memory_order_acquire);
    peerRtFlags_ = peer.rt_flags.load(std::memory_order_acquire);
    peerMaxBlock_ = peer.max_block.load(std::memory_order_acquire);
    havePeerSlot_ = true;
}

// 错序证据（口径见 OrderEvidence.h 与 ADR-0009）：每个 n > 0 的支持块判一次。
//
// 读到的是块首的对端槽（readPeerSlot 在拉环之前读好）：Input 读 Output 槽里的时间戳与块序号。
// 宿主时间戳无效、读不到对端槽、对端自上次检查起没有处理新块、时间戳回退或跳变时这一块判不了：
// 连续票数清零，已有的结论保持，1 s（按样本计）没有有效检查才回到 unknown。
void LoopSession::judgeOrder(const BlockContext& ctx) noexcept
{
    const engine::TransportInfo& transport = ctx.transport;
    order_.observe(OrderSide::kInput, ctx.blockLength, transport.hostNsValid, transport.hostNs, havePeerSlot_,
                   peerBlockSeq_, peerHostNs_);
    status_.setOrder(order_.snapshot());
}

// 超长块（环路这一块不走）：错序证据不投票，但宿主时间戳与样本数照样在走，基线与失效计时要跟上
// （口径见 OrderEvidence::advance），不然下一个正常块会被当成时间戳跳变。
void LoopSession::advanceOrder(const BlockContext& ctx) noexcept
{
    const engine::TransportInfo& transport = ctx.transport;
    order_.advance(ctx.blockLength, transport.hostNsValid, transport.hostNs);
    status_.setOrder(order_.snapshot());
}

// 对端槽的违约标志：只在对端的一个新块上看一次（同一块的标志不重复计）。
// 第一次读到只记下块序号，不计：那是上一个会话留在槽里的旧标志也说不定。
// 读不到对端槽的块清掉记账（再读到时算第一次）；换组与换绑定的清法见 readPeerSlot 和 process。
bool LoopSession::peerOverMaxOnNewBlock() noexcept
{
    if (!havePeerSlot_)
    {
        haveExportPeerSeq_ = false;
        return false;
    }
    const bool known = haveExportPeerSeq_;
    const bool fresh = peerBlockSeq_ != lastExportPeerSeq_;
    haveExportPeerSeq_ = true;
    lastExportPeerSeq_ = peerBlockSeq_;
    return known && fresh && (peerRtFlags_ & ipc::kRtOverMax) != 0;
}

BlockResult LoopSession::process(const BlockContext& ctx) noexcept
{
    BlockResult result;
    result.blockStart = sync_.processed();

    // 0 样本的调用立刻返回，不推进任何计数
    if (ctx.blockLength <= 0)
    {
        result.delayEffective = delayInUse_;
        result.delayMin = delayMin_;
        result.estimatorDelay = largestTapDelay(taps_);
        result.tapSettledSilent = tapsSettledSilent(taps_);
        return result;
    }

    const i64 n = static_cast<i64>(ctx.blockLength);

    // 块长超过支持的上限：环路这一块不走，只出干声并给提示码；正在响的回送在开头淡到 0（见 fadeOutUnsupportedBlock）。
    // 提示码只在进入超限时发一条（超限会连续很多块，每块一条会把诊断队列灌满）；回到支持的块长之后再超限，再发一条。
    if (static_cast<std::uint64_t>(ctx.blockLength) > static_cast<std::uint64_t>(ipc::kMaxSupportedBlock))
    {
        result.unsupportedBlock = true;
        if (!lastUnsupported_)
        {
            sink_.emit(LoopEventCode::kUnsupportedBlock, eventUnits256(n),
                       eventUnits256(static_cast<std::int64_t>(ipc::kMaxSupportedBlock)), sync_.processed());
        }
        lastUnsupported_ = true;
        // 对端槽照样读一次：错序证据不投票，但时间戳基线与失效计时照样前进；对端的违约标志也在这里看，
        // 块序号的记账才跟得上
        readPeerSlot(ctx);
        advanceOrder(ctx);
        // 离线渲染里的超长块：导出与播放不会一致（环路这一块不走），计一次；对端违约的原因位并进同一个掩码，
        // 这一块仍只计一次
        ExportBlockFacts unsupportedFacts;
        unsupportedFacts.nonrealtime = ctx.transport.nonrealtime;
        unsupportedFacts.unsupportedBlock = true;
        unsupportedFacts.peerOverMax = peerOverMaxOnNewBlock();
        status_.noteExport(exportCauses(unsupportedFacts));
        const std::int64_t faded = fadeOutUnsupportedBlock(ctx);
        result.returnLeft = ctx.returnLeft;
        result.returnRight = ctx.returnRight;
        result.returnValid = ctx.returnValid;
        result.returnLength = static_cast<int>(faded);
        result.delayEffective = delayInUse_;
        result.delayMin = delayMin_;
        result.estimatorDelay = largestTapDelay(taps_);
        result.paired = havePeer_;
        result.stalledState = sync_.state() == SyncState::kStalled;
        return result;
    }
    lastUnsupported_ = false;

    // 速度与 D
    updateBpm(ctx);
    const std::int64_t delay = computeDelay(ctx);

    // 走带沿：上一块没走、这一块走了
    const engine::TransportInfo& transport = ctx.transport;
    const bool playing = transport.isPlaying;
    const bool playEdge = playing && !lastPlaying_;
    lastPlaying_ = playing;

    // 对端的事实：写头、epoch、写权
    i64 writeHead = 0;
    i64 writeIntent = 0;
    bool paired = false;
    bool writerChanged = false;
    bool formatChanged = false;
    const bool wasPaired = havePeer_;

    // 是不是上一块那个段：地址与映射代号都相同才算（同一个地址上可能是一块新的映射）。只比，不经旧地址去读。
    const bool sameRing = ctx.ring != nullptr && ctx.ring == lastRing_ && ctx.ringGeneration == lastRingGeneration_;
    lastRing_ = ctx.ring;
    lastRingGeneration_ = ctx.ring != nullptr ? ctx.ringGeneration : 0u;

    if (ctx.ring != nullptr)
    {
        const ipc::RingFormat format = ipc::LoopRing::readFormat(*ctx.ring);
        writeHead = static_cast<i64>(ipc::LoopRing::writeHead(*ctx.ring));
        writeIntent = static_cast<i64>(ipc::LoopRing::writeIntent(*ctx.ring));
        paired = format.writerToken != 0;

        if (havePeer_ && paired)
        {
            writerChanged = format.writerToken != lastWriterToken_;
            formatChanged = format.epoch != lastEpoch_;
        }
        lastWriterToken_ = format.writerToken;
        lastEpoch_ = format.epoch;
        havePeer_ = paired;
    }
    else
    {
        havePeer_ = false;
        lastWriterToken_ = 0;
        lastEpoch_ = 0;
    }

    // 对端槽：宿主时间戳、块序号、实时标志。组号不合法时读不到。
    readPeerSlot(ctx);

    // 本块的读位起点。判据会把 R 推进到 R0 + n，混音与几何量都用推进之前的这一个。
    const i64 R0 = sync_.processed();

    // 域锚定：第一次配上时本地延迟线从当前写头起算（W 可能早已很大，从 0 起拉永远追不上）；
    // 写头倒退（换段遗漏一类）时整个域重来，读头作废（上一块的末值保持衰减，旧数据一样不读），判据重新锚定偏移。
    bool domainReset = false;
    if (paired)
    {
        if (!wasPaired)
        {
            line_.resetTo(writeHead);
        }
        else if (writeHead < line_.pulled())
        {
            holdLastReturn();
            line_.resetTo(writeHead);
            taps_.clear();
            domainReset = true;
        }
    }
    // 两边都配着、但换了段（换组、重新映射）：读头重建（原因：换绑定）。新段的写头不比已拉到的位置小时
    // 走这一条；比它小时上面已经按写头倒退整个域重来。
    const bool ringSwitched = paired && wasPaired && !sameRing && !domainReset;
    // 换绑定（换段、域重置）：对端槽的块序号记账也从头来，换绑定之后第一次读到的槽只记块序号，不计超限标志
    if (domainReset || ringSwitched)
    {
        haveExportPeerSeq_ = false;
    }

    // 本块拉环之后本地延迟线里有数据的上界。同一个段还在、而且这一块配着或上一块还配着（对端刚走）时，
    // 本块照常把环拉到 W：对端走之前发布的帧都是它写完了的真数据，旧读头的淡出要读到它们。
    // 环不在、或换了段时，旧读头只剩已经拉进来的那部分（新段的帧不进旧读头的淡出）。
    // 判据在拉环之前跑，淡出的读上界用的就是这个值（不是上一块拉到的位置）。
    const bool pullThisBlock = ctx.ring != nullptr && (paired || (wasPaired && sameRing));
    const i64 dataEnd = pullThisBlock && !ringSwitched ? std::max(line_.pulled(), writeHead) : line_.pulled();

    // 判据。R 只有一份：判据那一半持有它，这里把同一个值传进去。
    SyncInputs in;
    in.paired = paired;
    in.writerChanged = writerChanged;
    in.formatChanged = formatChanged;
    in.playEdge = playEdge;
    in.resetOnPlay = ctx.params.resetOnPlay;
    in.resetRequested = ctx.resetRequested;
    in.domainReset = domainReset;
    in.ringSwitched = ringSwitched;
    in.inactive = !paired;
    in.writeHead = writeHead;
    in.dataEnd = dataEnd;
    in.peerMaxBlock = peerMaxBlock_ > 0 ? peerMaxBlock_ : ctx.peerMaxBlock;
    in.processed = R0;
    in.declaredBlock = ctx.declaredBlock;
    in.delayEffective = delay;
    in.blockLength = static_cast<int>(n);

    if (ctx.resetRequested)
    {
        sync_.noteReset(); // reset() 只记诊断
    }

    const SyncOutcome outcome = sync_.process(in, line_, taps_);
    offset_ = sync_.offset();

    // 延迟线被清空：读头全部作废（否则会读到清空前的位置）。
    if (outcome.lineCleared)
    {
        taps_.clear();
    }
    // 换了段：判据已经让旧读头淡出、并把它淡出要读的那一段拷进了尾巴。主区从新段的写头起重来：
    // 新段 W 之前的帧不拉（那是另一条流，没有读头要读），旧读头之后只从尾巴里读到旧段的数据。
    if (ringSwitched)
    {
        line_.rebaseKeepingTail(writeHead);
    }

    // 拉环：pulled 追到 W（对端刚走的那一块也拉：见 dataEnd）。延迟线被清空的那一块不拉（清空锚在 W，没有可拉的）。
    if (pullThisBlock && !outcome.lineCleared)
    {
        pullRing(*ctx.ring, writeHead, in.peerMaxBlock);
    }

    // 建新读头：环上「从哪一帧起才有数据」。
    //
    // 起播沿：validFrom = W + T（T = min(5 ms, D_eff)），沿后的那 T 帧没有回送。
    // 换写方、换段：validFrom = max(W, intent)，旧写方公告过但没发布的那一段不读；换段时新段 W 之前的帧
    // （拉进来的位置与旧段相接，但那是另一条流）也不读。
    // 其他（第一次配上、重同步）：validFrom = W，新数据从下一帧起。
    bool delayTransition = false;
    if (paired && !outcome.lineCleared)
    {
        const bool needFreshTap = outcome.resynced || taps_.count() == 0;
        if (needFreshTap)
        {
            i64 validFrom = writeHead;
            if (outcome.resynced && outcome.reason == SyncReason::kPlay)
            {
                validFrom = writeHead + sync_.playFadeSamples(delay);
            }
            else if (outcome.resynced &&
                     (outcome.reason == SyncReason::kWriter || outcome.reason == SyncReason::kBindSwitch))
            {
                validFrom = std::max(writeHead, writeIntent);
            }
            newTap(R0, validFrom, delay, sync_.onsetSamples());
        }
        else
        {
            // D 变了：40 ms 线性交叉淡化，100 ms 前沿限速（读位置不跳变）。
            // 重同步 / 清空 / 域重置那几块直接用新的 D 建读头，不走这里。
            delayTransition = startDelayCrossfade(R0, n, delay);
        }
    }

    // 补 0 的样本数：主读头本该有数据、但环里还没写到的那些样本。
    // 写头落后得比 D 还多（Output 停了、晚到或丢了块）时会出现：取不到的样本只能补 0。补 0 之前先让主读头
    // 在它还读得到的那几个样本里淡到 0，回送不在数据断开的那一点上阶跃；之后写头恢复时由「没有读头 → 建新读头」
    // 接上（新数据 20 ms onset）。正确摆放、D 不小于块长时写头从不落后到这一步，这条路径不碰稳态的输出。
    const std::int64_t overrun = paired ? mainTapOverrun(R0, n) : 0;
    if (paired)
    {
        fadeTapsAtUnderrun(R0, n);
    }

    // 混音：读头按包络混成一路，并标出逐样本的有效位。位置用本块的读位起点 R0。
    // 块首就读不到数据的读头先把末值交给保持；保持叠在读头之上（有效位不置：保持的值不是环里的新数据），按样本推进。
    holdStrandedTaps(R0);
    std::uint64_t nonZero = 0;
    if (ctx.returnLeft != nullptr && ctx.returnRight != nullptr)
    {
        nonZero = mixTaps(taps_, line_, R0, ctx.returnLeft, ctx.returnRight, n, ctx.returnValid);
        if (hold_.active())
        {
            hold_.addInto(ctx.returnLeft, ctx.returnRight, n);
            nonZero = countNonZero(ctx.returnLeft, ctx.returnRight, n);
        }
        lastReturnLeft_ = ctx.returnLeft[n - 1];
        lastReturnRight_ = ctx.returnRight[n - 1];
        lastReturnAt_ = R0 + n - 1;
    }
    else
    {
        hold_.addInto(nullptr, nullptr, n);
        lastReturnAt_ = -1;
    }

    result.returnLeft = ctx.returnLeft;
    result.returnRight = ctx.returnRight;
    result.returnLength = ctx.blockLength;
    result.returnValid = ctx.returnValid;
    // 发布主读头本块实际读的 D：新目标被限速窗口挂着时，目标 D 已经算好了，读头还没换上它（见 startDelayCrossfade）。
    // 状态快照、Input 槽的 d_eff 与界面读的都是这一个，和正在读的位置一致。
    delayInUse_ = mainTapDelay(taps_, delayTarget_);
    result.delayEffective = delayInUse_;
    result.delayMin = delayMin_;
    result.delayUserMs = ctx.params.delayMs;
    result.estimatorDelay = largestTapDelay(taps_);
    result.nonZeroReturn = nonZero;
    result.overrun = static_cast<std::uint64_t>(overrun);
    result.hostViolation = outcome.hostViolation;
    result.paired = paired;
    result.stalledState = sync_.state() == SyncState::kStalled;
    result.resynced = outcome.resynced;
    result.resyncReason = outcome.reason;
    result.resetDsp = outcome.resetDsp;
    result.resetAtSample = outcome.resetAtSample;
    result.blockStart = R0;
    result.lineCleared = outcome.lineCleared;
    result.stalled = outcome.stalled;
    result.enteredTolerant = outcome.enteredTolerant;
    result.tapSettledSilent = tapsSettledSilent(taps_) && !hold_.active() && nonZero == 0;

    if (overrun > 0)
    {
        sink_.emit(LoopEventCode::kOverrun, static_cast<std::uint8_t>(std::min<std::int64_t>(overrun, 255)), 0, R0);
    }

    // 包络按样本推进 n（与块长无关）：淡入按本块读到的位置记账，淡出按样本推进。
    // 读位 R 由判据那一半推进，这里不再另推一份。
    settleOnsets(taps_, R0, n);
    advanceTaps(taps_, n); // 淡出与交叉淡化都按样本推进 n
    retarget_.endBlock(n); // 限速窗口按样本计：每个正常块恰好一次（0 样本与超长块不走到这里）

    // 错序证据：每块判一次（宿主时间戳有效、对端有新块时才有票）。
    judgeOrder(ctx);

    // 导出不一致：离线块里出了导出可能与播放不同的事，计一次。对端的违约标志每块都要看一眼（记块序号），
    // 不管是不是离线块。只算账不出样本的块（ctx.returnLeft 为空的快速路径）照样走到这里。
    ExportBlockFacts exportFacts;
    exportFacts.nonrealtime = transport.nonrealtime;
    exportFacts.hostViolation = outcome.hostViolation;
    exportFacts.overrun = static_cast<std::uint64_t>(overrun);
    exportFacts.peerOverMax = peerOverMaxOnNewBlock();
    exportFacts.resynced = outcome.resynced;
    exportFacts.resyncReason = outcome.reason;
    status_.noteExport(exportCauses(exportFacts));

    // 发布状态
    status_.setPaired(paired);
    status_.setStalled(sync_.state() == SyncState::kStalled);
    status_.setTolerant(sync_.tolerant());
    status_.setUnstable(sync_.unstable());
    status_.setDelay(result.delayUserMs, delayInUse_, delayMin_, static_cast<std::uint8_t>(ctx.params.delayMode));
    status_.setDelayFloor(static_cast<std::uint8_t>(delayMinReason_), maxBlockSeen_);
    status_.setDelayTarget(delayTarget_, delayHints_, taps_.crossfadeRemaining());
    status_.setTaps(static_cast<std::uint32_t>(taps_.count()), tapDropCount_);
    status_.addOverrun(static_cast<std::uint64_t>(overrun));
    if (outcome.resynced)
    {
        status_.addResync(static_cast<std::uint8_t>(outcome.reason));
    }
    if (delayTransition)
    {
        status_.addResync(static_cast<std::uint8_t>(SyncReason::kDChange));
    }
    // frozenMax 是「连续最多几块写头没动」（块数），不是断档推进的样本数。
    status_.setFrozenMax(sync_.maxFrozenBlocks());
    // 漂移被吸收的块数：漂移被吸收是真实事件，计数要看得见。
    status_.setOffsetDriftBlocks(sync_.absorbedDriftBlocks());
    // 写头 CAS 失败次数：写方在环头里记的（读侧只是转出来）。
    if (ctx.ring != nullptr)
    {
        status_.setWCasFail(ctx.ring->header.w_cas_fail.load(std::memory_order_relaxed));
    }
    // 当前的几何量：m = W − R − off，R 取本块的读位起点，与判据看到的是同一组 W、R、off。正数 = 写头领先。
    status_.setGeometry(drift(writeHead, R0, offset_), writeHead, offset_);

    return result;
}

} // namespace rinne::loop
