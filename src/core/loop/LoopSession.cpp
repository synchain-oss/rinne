// SPDX-License-Identifier: GPL-3.0-or-later
//
// LoopSession：环路层的门面。参数与走带整体下传、D 的计算、读头混合、违约路径、状态发布都在这里。
//
// 一块的流程：
//   1. 算 D：用户要的（毫秒或节拍）→ 样本 → 抬到下限（max(N_prep, N_obs)）→ 截到上限。
//   2. 判据：交给 StreamSync（断档、对齐、容忍、重同步、停用清空、起播复位）。
//   3. 拉环：把环里 [pulled, min(W, pulled + n)) 的帧搬进本地延迟线。读不到的帧补 0。
//   4. 混音：把读头按包络混成一路，写进 ctx.returnLeft / ctx.returnRight，并标出逐样本的有效位。
//   5. 判错序：读对端槽的宿主时间戳，按 ADR-0002 的口径累票。
//   6. 发布状态：写进 LoopStatus（原子量），消息线程读快照。
//
// 读头的生命周期（3 个槽）：
//   稳态      1 个读头在跑（槽 0），包络恒为 1。
//   淡出期间  旧读头在淡出（5 ms），新读头同时在 onset（20 ms）：两个槽。
//   重同步    旧读头淡出 + 新读头 onset：两个槽。第三个槽留给「改 D 的交叉淡化」。
//   槽满      丢掉包络最小的那个（不是硬丢），并记一次 tapDrop。
//
// 读位 R 只有一份：判据那一半（StreamSync）持有它，本文件每块把同一个值传进去，不再自己推进一份。
// 两份 R 是「漂移量 m 算错」的典型来源：m = W − (R + off) 里的 R 必须是同一个。
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

// 错序判据：连续这么多次判为「Output 在 Input 之前」才定案（口径见 ADR-0002）。
constexpr std::uint32_t kOrderVotesToJudge = 3;
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
        offset_ = 0;
        delayEffective_ = 0;
        delayMin_ = 0;
        delayRequested_ = 0;
        havePeer_ = false;
        lastWriterToken_ = 0;
        lastEpoch_ = 0;
        tapDropCount_ = 0;
        orderVotes_ = 0;
        orderVerdict_ = static_cast<std::uint8_t>(OrderVerdict::kUnknown);
    }

    // 拉环的暂存：定长，容得下支持的最大块长。音频线程上不再分配。
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
std::int64_t LoopSession::computeDelay(const BlockContext& ctx) noexcept
{
    const engine::EngineParams& params = ctx.params;
    const double sr = sampleRate_ > 0.0 ? sampleRate_ : 48000.0;

    std::int64_t requested = delayRequested_;
    if (params.delayMode == engine::DelayMode::kSync)
    {
        // 节拍同步：拍数 × 60/BPM × sr。bpm 无效时用回退值（120）。
        const double bpm = bpmCenti_ > 0 ? static_cast<double>(bpmCenti_) * 0.01
                                        : static_cast<double>(engine::kFallbackBpm);
        const int note = static_cast<int>(params.delayNote) % engine::kDelayNoteCount;
        const double beats = engine::kDelayNoteBeats[note];
        const auto frames = static_cast<std::int64_t>(std::llround(beats * 60.0 / bpm * sr));
        requested = frames > 0 ? frames : 1;
    }
    else
    {
        // 时间模式：毫秒
        requested = msToFrames(static_cast<double>(params.delayMs), sr);
    }

    // 下限：max(N_prep, N_obs)。N_obs 由引擎每块更新。
    const DelayChoice choice = chooseDelay(requested, ctx.declaredBlock, ctx.observedBlock, sr);

    if (choice.raised && choice.effective != delayEffective_)
    {
        sink_.emit(LoopEventCode::kDMinRaised, static_cast<std::uint8_t>(choice.minimum),
                   static_cast<std::uint8_t>(choice.reason));
    }

    delayRequested_ = requested;
    delayMin_ = choice.minimum;
    delayEffective_ = choice.effective;
    return choice.effective;
}

// 速度：宿主给的有效值、来源。宿主没给（或给的值超出 20..999）就回退到 120 并记成 fallback。
void LoopSession::updateBpm(const BlockContext& ctx) noexcept
{
    const engine::TransportInfo& transport = ctx.transport;
    std::uint8_t source = static_cast<std::uint8_t>(BpmSource::kNone);
    std::uint32_t centi = 0;

    if (transport.bpm >= engine::kMinHostBpm && transport.bpm <= engine::kMaxHostBpm)
    {
        centi = static_cast<std::uint32_t>(std::lround(static_cast<double>(transport.bpm) * 100.0));
        source = static_cast<std::uint8_t>(BpmSource::kHost);
    }
    else if (bpmCenti_ != 0)
    {
        // 宿主这一次没给，但之前给过：保留上一次的值，来源仍然标成 host（那个值仍然有效）
        centi = bpmCenti_;
        source = static_cast<std::uint8_t>(BpmSource::kHost);
    }
    else
    {
        centi = static_cast<std::uint32_t>(std::lround(static_cast<double>(engine::kFallbackBpm) * 100.0));
        source = static_cast<std::uint8_t>(BpmSource::kFallback);
    }

    bpmCenti_ = centi;
    bpmSource_ = source;
    status_.setBpm(centi, source);
}

void LoopSession::flushLocalDelayLine() noexcept
{
    // 跳闸淡出后清空：只改两个整数，不 memset（几兆字节清零不能在音频线程上做）。
    line_.reset();
    taps_.clear();
}

// 建一个新的读头：validFrom 是环上「从哪一帧起才有数据」，包络走 onset。
//
// 槽满了就丢掉包络最小的那个（不是硬丢），并记一次 tapDrop —— 那样丢的时候回送会有一处凹陷，
// 硬丢则会让一个正在淡出的读头凭空消失，听感更差。
int LoopSession::newTap(i64 validFrom, i64 delay, std::int64_t onset) noexcept
{
    int slot = taps_.allocate();
    if (slot < 0)
    {
        slot = taps_.dropSmallestEnvelope();
        ++tapDropCount_;
        sink_.emit(LoopEventCode::kTapDropped, 1);
        if (slot < 0)
        {
            return -1;
        }
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

// 对端槽的事实：宿主时间戳、块序号、实时标志、对端声明的最大块长。
//
// 会合段按组块排布（组 G 在 1024·G），组号 0 或越界时不去读：那会越界访问。
// 读不到就当没有对端槽：判据退回 unknown，绝不猜。
void LoopSession::readPeerSlot(const BlockContext& ctx) noexcept
{
    havePeerSlot_ = false;
    peerHostNs_ = 0;
    peerBlockSeq_ = 0;
    peerRtFlags_ = 0;
    peerMaxBlock_ = 0;

    if (ctx.rendezvous == nullptr || ctx.group == 0 || ctx.group > ipc::kGroupCount)
    {
        return;
    }

    const ipc::RoleSlot& peer = ipc::roleSlot(*ctx.rendezvous, ctx.group, ipc::peerOf(ctx.role));
    peerHostNs_ = peer.last_host_ns.load(std::memory_order_acquire);
    peerBlockSeq_ = peer.block_seq.load(std::memory_order_acquire);
    peerRtFlags_ = peer.rt_flags.load(std::memory_order_acquire);
    peerMaxBlock_ = peer.max_block.load(std::memory_order_acquire);
    havePeerSlot_ = true;
}

// 错序判据（口径见 ADR-0002）：
//
//   宿主时间戳有效时，比对两端的时间戳：谁的更小，谁的那一端的块首更早被处理。
//   连续 3 次都判为「对端（Output）在本端（Input）之前」才定为错序（界面给琥珀提示）。
//   宿主时间戳无效（没给，或给了 0）时一律记 unknown，票数清零：不猜、不用别的时间源代替。
//   判为正确一次就把票数清零（票是连续计数）。
void LoopSession::judgeOrder(const BlockContext& ctx) noexcept
{
    const engine::TransportInfo& transport = ctx.transport;
    const bool hostNsValid = transport.hostNsValid && transport.hostNs != 0;

    if (!hostNsValid || !havePeerSlot_ || peerHostNs_ == 0)
    {
        orderVotes_ = 0;
        orderVerdict_ = static_cast<std::uint8_t>(OrderVerdict::kUnknown);
        status_.setOrder(orderVerdict_, 0, false);
        return;
    }

    // 对端的时间戳更小 = 对端那一端的块首更早被处理 = Output 在 Input 之前。
    const bool peerFirst = peerHostNs_ < transport.hostNs;
    if (peerFirst)
    {
        ++orderVotes_;
        orderVerdict_ = orderVotes_ >= kOrderVotesToJudge ? static_cast<std::uint8_t>(OrderVerdict::kOutputFirst)
                                                          : static_cast<std::uint8_t>(OrderVerdict::kUnknown);
    }
    else
    {
        orderVotes_ = 0;
        orderVerdict_ = static_cast<std::uint8_t>(OrderVerdict::kCorrect);
    }

    status_.setOrder(orderVerdict_, orderVotes_, true);
    lastHostNs_ = transport.hostNs;
}

BlockResult LoopSession::process(const BlockContext& ctx) noexcept
{
    BlockResult result;

    // 0 样本的调用立刻返回，不推进任何计数
    if (ctx.blockLength <= 0)
    {
        result.delayEffective = delayEffective_;
        result.delayMin = delayMin_;
        result.estimatorDelay = largestTapDelay(taps_);
        result.tapSettledSilent = tapsSettledSilent(taps_);
        return result;
    }

    const i64 n = static_cast<i64>(ctx.blockLength);

    // 块长超过支持的上限：只出干声并给提示码。
    if (static_cast<std::uint64_t>(ctx.blockLength) > static_cast<std::uint64_t>(ipc::kMaxSupportedBlock))
    {
        result.unsupportedBlock = true;
        sink_.emit(LoopEventCode::kUnsupportedBlock, static_cast<std::uint8_t>(ctx.blockLength),
                   static_cast<std::uint8_t>(ipc::kMaxSupportedBlock));
        return result;
    }

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
    bool paired = false;
    bool writerChanged = false;
    bool formatChanged = false;

    if (ctx.ring != nullptr)
    {
        const ipc::RingFormat format = ipc::LoopRing::readFormat(*ctx.ring);
        writeHead = static_cast<i64>(ipc::LoopRing::writeHead(*ctx.ring));
        paired = format.writerToken != 0;

        if (havePeer_)
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

    // 判据。R 只有一份：判据那一半持有它，这里把同一个值传进去。
    SyncInputs in;
    in.paired = paired;
    in.writerChanged = writerChanged;
    in.formatChanged = formatChanged;
    in.playEdge = playEdge;
    in.resetOnPlay = ctx.params.resetOnPlay;
    in.resetRequested = ctx.resetRequested;
    in.inactive = !paired;
    in.writeHead = writeHead;
    in.peerMaxBlock = peerMaxBlock_ > 0 ? peerMaxBlock_ : ctx.peerMaxBlock;
    in.processed = sync_.processed();
    in.declaredBlock = ctx.declaredBlock;
    in.delayEffective = delay;
    in.blockLength = static_cast<int>(n);

    if (ctx.resetRequested)
    {
        sync_.noteReset(); // reset() 只记诊断
    }

    const SyncOutcome outcome = sync_.process(in, line_, taps_);
    offset_ = sync_.offset();

    // 拉环：把 [pulled, min(W, pulled + n)) 的帧搬进延迟线。读不到的帧补 0。
    //
    // 补 0 的样本数 = n − 读到的帧数。这是「宿主违约」的可见后果：宿主送来的块长超过声明值时，
    // 环里没有那么多帧可读，多出来的那些只能是 0。
    //
    // 环里的帧先落在暂存里（定长，prepare 里分配），再一次搬进延迟线：延迟线的 pull 是按区间搬的，
    // 不能在环的 sink 里逐帧调用（那会把 pulled 改得不成样子）。
    std::int64_t readFrames = 0;
    if (paired && ctx.ring != nullptr)
    {
        const i64 from = line_.pulled();
        const i64 to = std::min(writeHead, from + n);
        if (to > from)
        {
            // 暂存按位置对齐：pullPositions_[i] 是这一帧的绝对位置。
            // 环的 pull 是连续的，位置就是 from + i，所以不需要另外记位置。
            const ipc::RingPull pulled = ipc::LoopRing::pull(*ctx.ring, from, to, [&](i64 p, float l, float r) {
                const auto at = static_cast<std::size_t>(p - from);
                if (at < pullLeft_.size())
                {
                    pullLeft_[at] = l;
                    pullRight_[at] = r;
                }
            });
            // validFrom 之前的帧可能已被覆盖，一律当 0（绝不读旧数据）
            const i64 validFrom = pulled.validFrom;
            readFrames = to - from;
            // 整段搬进延迟线：位置 p 的样本取自暂存的 [p - from]
            line_.pull(from, to, [&](i64 p, float& l, float& r) {
                const auto at = static_cast<std::size_t>(p - from);
                if (p < validFrom || at >= pullLeft_.size())
                {
                    l = 0.0f;
                    r = 0.0f;
                    return;
                }
                l = pullLeft_[at];
                r = pullRight_[at];
            });
            // 两种「被丢掉」的帧要分开记（诊断报告按这两个字段取数，不能混）：
            //   overflow 环只有一圈：from 比 to 早一圈以上的那些帧，已经被覆盖，丢帧数 = lo − from。
            //   torn     写方的公告（intent）领先一圈以上：那一段没写完就被下一段盖掉了，
            //            读到的可能是半截数据，丢帧数 = validFrom − lo。
            // lo 与 pull 内部算的是同一个：环只有一圈，比 to 早一圈的帧一定已经不存在了。
            const i64 lap = static_cast<i64>(ipc::kRingFrames);
            const i64 lo = std::max(std::max(from, to - lap), static_cast<i64>(0));
            if (lo > from)
            {
                const std::uint64_t dropped = static_cast<std::uint64_t>(lo - from);
                status_.addOverflow(dropped);
                sink_.emit(LoopEventCode::kOverflow, static_cast<std::uint8_t>(dropped));
            }
            if (validFrom > lo)
            {
                const std::uint64_t torn = static_cast<std::uint64_t>(validFrom - lo);
                status_.addTorn(torn);
                sink_.emit(LoopEventCode::kTorn, static_cast<std::uint8_t>(torn));
            }
            // 有效区间的下界要跟着抬：比它更早的位置一律读成 0（绝不读旧数据或别名数据）。
            line_.raiseValidFrom(validFrom);
        }
    }

    // 旧读头要开始淡出：断档、重同步、起播沿都是这一种。淡出是 5 ms 线性。
    // validTo 收到「这一块拉进来的最后一帧 + 1」：淡出期间不再读新位置。
    if (outcome.fadeStarted)
    {
        const i64 fadeEnd = line_.pulled();
        for (int slot = 0; slot < kMaxTaps; ++slot)
        {
            Tap& tap = taps_.at(slot);
            if (!tap.active)
            {
                continue;
            }
            tap.validTo = std::min(tap.validTo, fadeEnd);
            tap.env.fadeOutSteps = sync_.fadeOutSamples();
            tap.env.fadeOutStep = 0;
        }
    }

    // 延迟线被清空：读头全部作废（否则会读到清空前的位置）。
    if (outcome.lineCleared)
    {
        taps_.clear();
    }

    // 建新读头：环上「从哪一帧起才有数据」。
    //
    // 起播沿的新读头 validFrom = W + T（T = min(5 ms, D_eff)）：沿后的那 T 帧没有回送。
    // 稳态与重同步：validFrom = 当前拉到的最后一帧 + 1（新数据从下一帧起）。
    if (paired && !outcome.lineCleared)
    {
        const bool needFreshTap = outcome.resynced || taps_.count() == 0;
        if (needFreshTap)
        {
            const i64 validFrom = outcome.fadeReason == SyncReason::kPlay
                                      ? writeHead + sync_.playFadeSamples(delay)
                                      : line_.pulled();
            newTap(validFrom, delay, sync_.onsetSamples());
        }
        else
        {
            // D 变了：把主读头的 D 更新到新值（改 D 的交叉淡化由改 D 的那一张卡接）
            for (int slot = 0; slot < kMaxTaps; ++slot)
            {
                Tap& tap = taps_.at(slot);
                if (tap.active && tap.env.fadeOutSteps == 0)
                {
                    tap.delay = delay;
                    break;
                }
            }
        }
    }

    // 混音：读头按包络混成一路，并标出逐样本的有效位。
    std::uint64_t nonZero = 0;
    if (ctx.returnLeft != nullptr && ctx.returnRight != nullptr)
    {
        nonZero = mixTaps(taps_, line_, sync_.processed(), ctx.returnLeft, ctx.returnRight, n, ctx.returnValid);
    }

    // 补 0 的样本数：本块 n 个样本里读不到的那些
    const std::int64_t overrun = std::max<i64>(0, n - readFrames);

    result.returnLeft = ctx.returnLeft;
    result.returnRight = ctx.returnRight;
    result.returnLength = ctx.blockLength;
    result.returnValid = ctx.returnValid;
    result.delayEffective = delayEffective_;
    result.delayMin = delayMin_;
    result.delayUserMs = ctx.params.delayMs;
    result.estimatorDelay = largestTapDelay(taps_);
    result.nonZeroReturn = nonZero;
    result.overrun = static_cast<std::uint64_t>(overrun);
    result.hostViolation = outcome.hostViolation;
    result.resynced = outcome.resynced;
    result.resetDsp = outcome.resetDsp;
    result.resetAtSample = outcome.resetAtSample;
    result.lineCleared = outcome.lineCleared;
    result.stalled = outcome.stalled;
    result.enteredTolerant = outcome.enteredTolerant;
    result.tapSettledSilent = tapsSettledSilent(taps_) && nonZero == 0;

    // 包络按样本推进 n（与块长无关）。读位 R 由判据那一半推进，这里不再另推一份。
    advanceTaps(taps_, n);

    // 错序判据：宿主时间戳有效时比两端的时间戳。
    judgeOrder(ctx);

    // 发布状态
    status_.setPaired(paired);
    status_.setStalled(sync_.state() == SyncState::kStalled);
    status_.setTolerant(sync_.tolerant());
    status_.setUnstable(sync_.unstable());
    status_.setDelay(result.delayUserMs, delayEffective_, delayMin_,
                      static_cast<std::uint8_t>(ctx.params.delayMode));
    status_.setTaps(static_cast<std::uint32_t>(taps_.count()), tapDropCount_);
    status_.addOverrun(static_cast<std::uint64_t>(overrun));
    if (outcome.resynced)
    {
        status_.addResync(static_cast<std::uint8_t>(outcome.reason));
    }
    // frozenMax 是「连续最多几块写头没动」（块数），不是断档推进的样本数。
    status_.setFrozenMax(sync_.maxFrozenBlocks());
    // 漂移被吸收的块数：漂移被吸收是真实事件，计数要看得见。
    status_.setOffsetDriftBlocks(sync_.absorbedDriftBlocks());
    // 当前的几何量：m = W − (R + off)。正数 = 写头领先。
    status_.setGeometry(drift(writeHead, sync_.processed(), offset_), writeHead, offset_);

    return result;
}

} // namespace rinne::loop
