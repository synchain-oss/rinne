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
//   6. 判错序：读对端槽的宿主时间戳，按 ADR-0002 的口径累票。
//   7. 发布状态：写进 LoopStatus（原子量），消息线程读快照。
//
// 读头的生命周期（3 个槽）：
//   稳态      1 个读头在跑，包络恒为 1。
//   淡出期间  旧读头在淡出（5 ms），新读头同时在 onset（20 ms）：两个槽。
//   重同步    旧读头淡出 + 新读头 onset：两个槽。
//   改 D      过渡做法同上（旧读头 5 ms 淡出 + 新读头 onset），一次只走一个过渡：两个槽。
//             改 D 的过渡还没走完时来了重同步，用到第三个槽。40 ms 双读头交叉淡化由时序扩展接入，同样是两个槽。
//   槽满      丢掉包络最小的那个（不是硬丢），并记一次 tapDrop。
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

// 错序判据：连续这么多次判为「Output 在 Input 之前」才定案（口径见 ADR-0002）。
constexpr std::uint32_t kOrderVotesToJudge = 3;

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
    if (params.delayMode == engine::DelayMode::kSync)
    {
        // 节拍同步：拍数 × 60/BPM × sr。bpm 无效时用回退值（120）。
        const double bpm =
            bpmCenti_ > 0 ? static_cast<double>(bpmCenti_) * 0.01 : static_cast<double>(engine::kFallbackBpm);
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

    if (choice.raised && choice.effective != delayEffective_)
    {
        // 载荷：a = D_min ÷256（向上取整、钳 255），b = 原因；准确值在状态快照的 delayMin 里。
        sink_.emit(LoopEventCode::kDMinRaised, eventUnits256(choice.minimum), static_cast<std::uint8_t>(floor));
    }

    delayRequested_ = requested;
    delayMin_ = choice.minimum;
    delayEffective_ = choice.effective;
    delayMinReason_ = floor;
    return choice.effective;
}

// 速度：宿主给的有效值、来源。宿主没给（或给的值超出 20..999）就沿用本会话最后一个有效值，
// 从没拿到过就回退到 120 并记成 fallback。
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
    else if (bpmSource_ == static_cast<std::uint8_t>(BpmSource::kHost))
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
    // 起点锚在已拉到的位置：之后从那里往后拉，比它早的帧一律读不出来。
    line_.resetTo(line_.pulled());
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

// 改 D 的过渡（安全链与时序扩展接入之前的过渡做法）。
//
// 主读头的 D 不当场改写：那样读位置跳变，回送里是一处阶跃（宿主违约当场抬 D 下限、节拍模式下 BPM 变了也走到这里）。
// 旧读头按重同步的口径淡出（名义 5 ms，读到本块拉环之后有数据的上界为止，数据不够时缩短到恰好用完），新读头按新的 D
// 建起来，从它读到的第一个有效位置起 20 ms onset。新读头的 validFrom 不早于旧读头的（起播沿、换写方定下的下界照旧），
// 也不早于本地延迟线的有效下界：D 变大、新位置落在延迟线还没有数据的地方时，onset 从数据真正开始的那一帧起算，
// 不会在数据出现时已经走完、以满增益突然冒出来。
//
// 一次只走一个过渡：还有读头在淡出、或主读头的 onset 已经出声但还没走完时不换，新的 D 先挂着（主读头照旧的 D 读，
// 不跳变），过渡走完的那一块再换成当时的 D。拖着旋钮时 D 每块都变，每块都起一个过渡的话，读头槽很快用完（丢一个
// 还在出声的读头是一处凹陷），预拷贝的尾巴也会被下一次淡出盖掉。挂着的时间不超过一个淡出加一个 onset（约 25 ms）。
// 主读头还没出声（onset 还没读到第一帧有效数据）时不必等：它这时淡出，最多只在淡出途中读到开头几帧，
// onset 与淡出两条斜坡相乘，幅度很小而且连续。
// 挂着的那几块里主读头的 D 可能低于刚抬上去的下限（宿主违约）：那时它读到还没写到的位置，由「写头落后时先淡到 0」接住。
bool LoopSession::changeDelay(i64 blockStart, i64 delay) noexcept
{
    int main = -1;
    bool fading = false;
    for (int slot = 0; slot < kMaxTaps; ++slot)
    {
        const Tap& tap = taps_.at(slot);
        if (!tap.active)
        {
            continue;
        }
        if (tap.env.fadingOut())
        {
            fading = true;
        }
        else if (main < 0)
        {
            main = slot;
        }
    }
    if (main < 0 || taps_.at(main).delay == delay)
    {
        return false;
    }
    const Tap& current = taps_.at(main);
    const bool audibleOnset = current.env.fadingIn() && current.env.fadeInStep > 0;
    if (fading || audibleOnset)
    {
        return false; // 上一个过渡还没走完：新的 D 先挂着
    }

    const i64 validFrom = std::max({current.validFrom, blockStart + offset_ - delay, line_.lineValidFrom()});
    if (!sync_.fadeForDelayChange(line_, taps_, blockStart, line_.pulled()))
    {
        return false;
    }
    newTap(validFrom, delay, sync_.onsetSamples());
    sink_.emit(LoopEventCode::kResync, static_cast<std::uint8_t>(ResyncReason::kDChange), 0, blockStart);
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

    // 淡出的长度：名义 5 ms；主读头（没在淡出的那个）从块首起只剩不到 5 ms 的数据时，缩短到恰好在数据用完时走到 0。
    // 在淡出的读头自己的淡出恰好在它的数据用完时走到 0，再乘上这一段淡出照样连续，不必算进来。
    // 没有读头时回送本来就是 0。
    if (taps_.count() == 0)
    {
        return 0;
    }
    const i64 R0 = sync_.processed();
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
        break;
    }
    mixTaps(taps_, line_, R0, ctx.returnLeft, ctx.returnRight, length, ctx.returnValid);
    for (i64 i = 0; i < length; ++i)
    {
        const float g = static_cast<float>(length - i) / static_cast<float>(length);
        ctx.returnLeft[i] *= g;
        ctx.returnRight[i] *= g;
    }
    return length;
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

// 主读头本块里补 0 的样本数：位置 s = R0 + i + off − D 落在读头的有效区间里，但还没被拉进来（s ≥ pulled）。
// 主读头是「没在淡出」的那个；都在淡出时本块没有主读头，不计。
std::int64_t LoopSession::mainTapOverrun(i64 blockStart, i64 n) const noexcept
{
    for (int slot = 0; slot < kMaxTaps; ++slot)
    {
        const Tap& tap = taps_.at(slot);
        if (!tap.active || tap.env.fadingOut())
        {
            continue;
        }
        const i64 base = blockStart + tap.offset - tap.delay;
        const i64 lo = std::max({base, line_.pulled(), tap.validFrom});
        const i64 hi = std::min(base + n, tap.validTo);
        return hi > lo ? hi - lo : 0;
    }
    return 0;
}

// 主读头本块会读到还没写到的位置：让它在还读得到的样本里线性淡到 0（读上界收到已拉到的位置）。
// 剩下的样本数就是淡出的长度（至少一步），淡出在数据用完的那一点恰好走到 0，之后补的 0 与它相接，没有阶跃。
// 淡出的长度不超过本块的块长，本块末这个读头就释放了。
void LoopSession::fadeMainTapAtUnderrun(i64 blockStart) noexcept
{
    for (int slot = 0; slot < kMaxTaps; ++slot)
    {
        Tap& tap = taps_.at(slot);
        if (!tap.active || tap.env.fadingOut())
        {
            continue;
        }
        const i64 from = blockStart + tap.offset - tap.delay;
        tap.validTo = std::min(tap.validTo, line_.pulled());
        tap.env.fadeOutSteps = std::max<std::int64_t>(tap.validTo - from, 1);
        tap.env.fadeOutStep = 0;
        return;
    }
}

// 对端槽的事实：宿主时间戳、块序号、实时标志、对端声明的最大块长。
//
// 会合段按组块排布，组号 0 或越界时不去读：那会越界访问。
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
//   两端在同一个宿主周期里拿到的是同一个宿主时间戳（它是这一块的时间戳，不是各自被调用的时刻）。
//   Input 在块首读 Output 槽里发布的 last_host_ns：
//     比本块的时间戳小  → Output 还停在上一块，本块里 Input 先被处理 → 顺序正确；
//     等于或大于本块的  → Output 已经处理过本块（或更晚的块）→ Output 在 Input 之前，记一票。
//   连续 3 票才定为错序（界面给琥珀提示）；判对一次就把票数清零（票是连续计数）。
//   宿主时间戳无效（没给，或给了 0）、或读不到对端槽、或对端还没发布过时间戳时，一律记 unknown，票数清零：
//   不猜、不用别的时间源代替。
void LoopSession::judgeOrder(const BlockContext& ctx) noexcept
{
    const engine::TransportInfo& transport = ctx.transport;
    const bool hostNsValid = transport.hostNsValid && transport.hostNs != 0;

    if (!hostNsValid || !havePeerSlot_ || peerHostNs_ == 0)
    {
        orderVotes_ = 0;
        orderVerdict_ = static_cast<std::uint8_t>(OrderVerdict::kUnknown);
        status_.setOrder(orderVerdict_, 0, hostNsValid);
        return;
    }

    const bool outputFirst = peerHostNs_ >= transport.hostNs;
    if (outputFirst)
    {
        if (orderVotes_ < kOrderVotesToJudge)
        {
            ++orderVotes_;
        }
        orderVerdict_ = orderVotes_ >= kOrderVotesToJudge ? static_cast<std::uint8_t>(OrderVerdict::kOutputFirst)
                                                          : static_cast<std::uint8_t>(OrderVerdict::kUnknown);
    }
    else
    {
        orderVotes_ = 0;
        orderVerdict_ = static_cast<std::uint8_t>(OrderVerdict::kCorrect);
    }

    status_.setOrder(orderVerdict_, orderVotes_, true);
}

BlockResult LoopSession::process(const BlockContext& ctx) noexcept
{
    BlockResult result;
    result.blockStart = sync_.processed();

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
        const std::int64_t faded = fadeOutUnsupportedBlock(ctx);
        result.returnLeft = ctx.returnLeft;
        result.returnRight = ctx.returnRight;
        result.returnValid = ctx.returnValid;
        result.returnLength = static_cast<int>(faded);
        result.delayEffective = delayEffective_;
        result.delayMin = delayMin_;
        result.estimatorDelay = largestTapDelay(taps_);
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
    // 写头倒退（换段遗漏一类）时整个域重来，读头作废，判据重新锚定偏移。
    bool domainReset = false;
    if (paired)
    {
        if (!wasPaired)
        {
            line_.resetTo(writeHead);
        }
        else if (writeHead < line_.pulled())
        {
            line_.resetTo(writeHead);
            taps_.clear();
            domainReset = true;
        }
    }
    // 两边都配着、但换了段（换组、重新映射）：读头重建（原因：换绑定）。新段的写头不比已拉到的位置小时
    // 走这一条；比它小时上面已经按写头倒退整个域重来。
    const bool ringSwitched = paired && wasPaired && !sameRing && !domainReset;

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
            newTap(validFrom, delay, sync_.onsetSamples());
        }
        else
        {
            // D 变了：旧读头淡出、新读头 onset（过渡做法，读位置不跳变）。40 ms 交叉淡化与 100 ms 去抖由时序扩展接入
            // （常量与口径已在 LoopTiming.h 与 ADR-0002 钉住）。
            delayTransition = changeDelay(R0, delay);
        }
    }

    // 补 0 的样本数：主读头本该有数据、但环里还没写到的那些样本。
    // 写头落后得比 D 还多（Output 停了、晚到或丢了块）时会出现：取不到的样本只能补 0。补 0 之前先让主读头
    // 在它还读得到的那几个样本里淡到 0，回送不在数据断开的那一点上阶跃；之后写头恢复时由「没有读头 → 建新读头」
    // 接上（新数据 20 ms onset）。正确摆放、D 不小于块长时写头从不落后到这一步，这条路径不碰稳态的输出。
    const std::int64_t overrun = paired ? mainTapOverrun(R0, n) : 0;
    if (overrun > 0)
    {
        fadeMainTapAtUnderrun(R0);
    }

    // 混音：读头按包络混成一路，并标出逐样本的有效位。位置用本块的读位起点 R0。
    std::uint64_t nonZero = 0;
    if (ctx.returnLeft != nullptr && ctx.returnRight != nullptr)
    {
        nonZero = mixTaps(taps_, line_, R0, ctx.returnLeft, ctx.returnRight, n, ctx.returnValid);
    }

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
    result.blockStart = R0;
    result.lineCleared = outcome.lineCleared;
    result.stalled = outcome.stalled;
    result.enteredTolerant = outcome.enteredTolerant;
    result.tapSettledSilent = tapsSettledSilent(taps_) && nonZero == 0;

    if (overrun > 0)
    {
        sink_.emit(LoopEventCode::kOverrun, static_cast<std::uint8_t>(std::min<std::int64_t>(overrun, 255)), 0, R0);
    }

    // 包络按样本推进 n（与块长无关）：淡入按本块读到的位置记账，淡出按样本推进。
    // 读位 R 由判据那一半推进，这里不再另推一份。
    settleOnsets(taps_, R0, n);
    advanceTaps(taps_, n);

    // 错序判据：宿主时间戳有效时比两端的时间戳。
    judgeOrder(ctx);

    // 发布状态
    status_.setPaired(paired);
    status_.setStalled(sync_.state() == SyncState::kStalled);
    status_.setTolerant(sync_.tolerant());
    status_.setUnstable(sync_.unstable());
    status_.setDelay(result.delayUserMs, delayEffective_, delayMin_, static_cast<std::uint8_t>(ctx.params.delayMode));
    status_.setDelayFloor(static_cast<std::uint8_t>(delayMinReason_), maxBlockSeen_);
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
