// SPDX-License-Identifier: GPL-3.0-or-later
//
// 会合的转移与生命周期。所有函数都只在自己的成员与共享内存的原子字段上工作：不睡、不阻塞、不抛异常；
// 段操作只经调用方持有的后端，失败一律解映射并按拍重试。时间全部由调用方注入，唯一读真实时钟的地方是
// 析构里的最后一次回收（那时模块可能马上被卸载，不会再有 25 Hz 定时器替我们回收了）。
#include "ipc/Rendezvous.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <utility>

#include "ipc/LoopRing.h"
#include "ipc/ProcessInfo.h"
#include "util/MonoClock.h"

namespace rinne::ipc
{

namespace
{

// 模块级的活实例计数：最后一个实例析构时没人再调 25 Hz 回收，映射要在那一刻自己收尾
std::atomic<std::size_t> gLiveInstances{0};
// 写权令牌的模块级计数：同一毫秒里、同一地址上先后建的实例也拿到不同的令牌
std::atomic<u64> gTokenCounter{0};

constexpr SegmentHeaderSpec kRvHeaderSpec{kRvMagic, kIpcAbi, kRvBytes, kRvHeaderBytes};
constexpr SegmentHeaderSpec kRingHeaderSpec{kRingMagic, kIpcAbi, kRingBytes, 16};

} // namespace

CtlCas casCtl(std::atomic<u64>& ctl, u64 expected, u32 newState) noexcept
{
    const u64 desired = nextCtl(expected, newState);
    u64 current = expected;
    if (ctl.compare_exchange_strong(current, desired, std::memory_order_acq_rel, std::memory_order_acquire))
    {
        return {true, desired};
    }
    return {false, current};
}

TakeoverTier takeoverTier(const SlotSample& s, u64 nowMs, u32 selfPid, ClaimIntent intent, PidAliveFn pidAlive) noexcept
{
    if (!s.stable)
    {
        return TakeoverTier::kNone; // 前后两次 ctl 不一致：这一拍什么都不判
    }
    const u32 phase = ctlPhase(s.ctl);
    if (phase == kCtlPhaseFree)
    {
        return TakeoverTier::kFree;
    }
    const u64 ctlStill = ageMs(nowMs, s.ctlSinceMs);
    const u64 seqStill = ageMs(nowMs, s.seqSinceMs);
    // 幽灵认领：claiming 卡住，而且 claim_ms 也老。只看 ctl 不够：刚 CAS 完还没重写 claim_ms 时，
    // 那个字段还留着上一任的时间
    if (phase == kCtlPhaseClaiming && ctlStill >= RendezvousTiming::kGhostMs &&
        ageMs(nowMs, s.claimMs) >= RendezvousTiming::kGhostMs)
    {
        return TakeoverTier::kGhost;
    }
    // 智能默认的认领可以被工程 state 的恢复抢占（不看 pid、不看年龄）
    if (phase == kCtlPhaseActive && (ctlState(s.ctl) & kCtlProvisional) != 0 && intent == ClaimIntent::kRestore)
    {
        return TakeoverTier::kPreemptProvisional;
    }
    // 死进程：跨进程接管，对端 pid 必须真的死了（探活放在最后，且拿不准一律按活）
    if ((phase == kCtlPhaseActive || phase == kCtlPhaseDormant) && s.pid != selfPid &&
        ageMs(nowMs, s.heartbeatMs) >= RendezvousTiming::kDeadMs && seqStill >= RendezvousTiming::kDeadMs &&
        ctlStill >= RendezvousTiming::kDeadMs && pidAlive != nullptr && !pidAlive(s.pid))
    {
        return TakeoverTier::kDeadProcess;
    }
    // 同进程孤儿：定时器与我们同一个消息线程，30 s 不动就说明那个实例已经不在了
    if ((phase == kCtlPhaseActive || phase == kCtlPhaseDormant) && s.pid == selfPid &&
        ageMs(nowMs, s.heartbeatMs) >= RendezvousTiming::kOrphanMs && seqStill >= RendezvousTiming::kOrphanMs &&
        ctlStill >= RendezvousTiming::kOrphanMs)
    {
        return TakeoverTier::kOrphan;
    }
    // 休眠槽：只有工程恢复或用户确认才拿，自动的冲突重试不拿
    if (phase == kCtlPhaseDormant && (intent == ClaimIntent::kRestore || intent == ClaimIntent::kManualConfirmed) &&
        ageMs(nowMs, s.dormantMs) >= RendezvousTiming::kDormantMs && ctlStill >= RendezvousTiming::kDormantMs)
    {
        return TakeoverTier::kDormant;
    }
    return TakeoverTier::kNone;
}

Rendezvous::Rendezvous(const RendezvousConfig& config) : config_(config), naming_(config.scope), backoff_(config.seed)
{
    gLiveInstances.fetch_add(1, std::memory_order_relaxed);
}

Rendezvous::~Rendezvous()
{
    const u64 now = util::monoNowMs();
    letGo(now);
    bindings_.clear();
    // 句柄的析构会停发租约、把没走完宽限期的映射交给进程级回收；这里不等
    keeper_.clear();
    const std::size_t remaining = gLiveInstances.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (remaining != 0)
    {
        return; // 还有别的实例：它们的 25 Hz 定时器会继续回收
    }
    // 最后一个实例：插件马上要卸载，之后没人再调回收。把剩下宽限期的映射立即收掉：
    // 租约计数还是 0 的映射可以马上解映射（租约在途的回收器会自己跳过），传一个宽限期之后的时间就是让
    // 回收器不用再等；真实时间比它小也没关系，回收器会从头重新计
    const u64 t = std::max<u64>(util::monoNowMs(), 1);
    SegmentHandle::reapRetired(t);
    SegmentHandle::reapRetired(t + SegmentHandle::kReleaseGraceMs);
}

void Rendezvous::setGroup(u32 group, ClaimIntent intent, u64 nowMs)
{
    if (group > kGroupCount)
    {
        return;
    }
    if (group == group_)
    {
        intent_ = intent;
        if (state_ == RendezvousState::kConflict && intent != ClaimIntent::kConflictRetry)
        {
            // 用户显式给了新的意图（工程恢复、确认接管）：不再等自动冲突重试，下一拍就按新意图再试
            state_ = RendezvousState::kOpening;
            nextConflictRetryMs_ = 0;
        }
        return;
    }
    letGo(nowMs); // 先放开旧组：CAS free 失败也不去「修」，那是新属主的槽了
    group_ = group;
    intent_ = intent;
    retryAtMs_ = 0;
    nextConflictRetryMs_ = 0;
    labelDirty_ = true; // 换了槽：标签要重新写到新槽
    labelOddSinceMs_ = 0;
    // 后端拒绝过就一直不可用：换组不会让它好起来，不再多试一次
    if (state_ != RendezvousState::kValidator && !permanent_)
    {
        state_ = group == 0 ? RendezvousState::kUnassigned
                            : (prepared_ ? RendezvousState::kOpening : RendezvousState::kWaitingPrepare);
    }
}

void Rendezvous::prepare(const StreamFormat& format, u64 nowMs)
{
    prepared_ = true;
    format_ = format;
    if (myCtl_ == 0 || rv_ == nullptr)
    {
        return;
    }
    RoleSlot& slot = roleSlot(*static_cast<RvSegment*>(rv_->base()), group_, role());
    if (slot.ctl.load(std::memory_order_acquire) != myCtl_)
    {
        lostOwnership(nowMs); // 上一拍之后被接管了：写格式字段之前先核对
        return;
    }
    bool becameActive = false;
    if (ctlPhase(myCtl_) == kCtlPhaseDormant)
    {
        // 休眠 -> active：claim_ms 不变（同一次认领），换一个新的整字
        const CtlCas cas = casCtl(slot.ctl, myCtl_, kCtlPhaseActive);
        if (!cas.ok)
        {
            lostOwnership(nowMs);
            return;
        }
        myCtl_ = cas.word;
        dormantMs_ = 0;
        becameActive = true;
    }
    writeFormat(slot);
    if (role() == Role::kOutput && ring_ != nullptr)
    {
        LoopRing::publishWriter(*static_cast<RingSegment*>(ring_->base()), token_, format_.sampleRate,
                                format_.channels);
    }
    if (becameActive)
    {
        publishBinding(); // myCtl 变了：音频线程的 stillMine 要对得上
    }
}

void Rendezvous::releaseResources(u64 nowMs)
{
    prepared_ = false; // 认领的门关上
    if (myCtl_ == 0 || rv_ == nullptr)
    {
        if (state_ != RendezvousState::kValidator)
        {
            state_ = group_ == 0 ? RendezvousState::kUnassigned : RendezvousState::kWaitingPrepare;
        }
        return;
    }
    RoleSlot& slot = roleSlot(*static_cast<RvSegment*>(rv_->base()), group_, role());
    if (ctlPhase(myCtl_) == kCtlPhaseActive)
    {
        const CtlCas cas = casCtl(slot.ctl, myCtl_, kCtlPhaseDormant);
        if (!cas.ok)
        {
            lostOwnership(nowMs);
            return;
        }
        myCtl_ = cas.word;
        dormantMs_ = nowMs;
        slot.dormant_ms.store(nowMs, std::memory_order_relaxed);
        publishBinding();
    }
    else if (ctlPhase(myCtl_) == kCtlPhaseDormant)
    {
        state_ = RendezvousState::kDormant;
    }
}

void Rendezvous::confirmClaim(u64 nowMs)
{
    if (myCtl_ == 0 || rv_ == nullptr || ctlPhase(myCtl_) != kCtlPhaseActive ||
        (ctlState(myCtl_) & kCtlProvisional) == 0)
    {
        return;
    }
    RoleSlot& slot = roleSlot(*static_cast<RvSegment*>(rv_->base()), group_, role());
    const CtlCas cas = casCtl(slot.ctl, myCtl_, kCtlPhaseActive);
    if (!cas.ok)
    {
        lostOwnership(nowMs);
        return;
    }
    myCtl_ = cas.word;
    publishBinding(); // myCtl 变了
}

void Rendezvous::setNonrealtime(bool on) noexcept
{
    nonrealtime_ = on;
}

bool Rendezvous::setLabel(std::string_view utf8Name, u32 colourArgb)
{
    label_ = makeLabelPayload(utf8Name, colourArgb);
    labelDirty_ = true;
    labelOddSinceMs_ = 0;
    if (myCtl_ == 0 || rv_ == nullptr)
    {
        return false;
    }
    RoleSlot& slot = roleSlot(*static_cast<RvSegment*>(rv_->base()), group_, role());
    if (slot.ctl.load(std::memory_order_acquire) != myCtl_)
    {
        return false; // 已经不是我们的槽了：别碰
    }
    if (!writeLabel(slot, label_))
    {
        return false;
    }
    labelDirty_ = false;
    return true;
}

bool Rendezvous::peerLost(u64 nowMs) const noexcept
{
    if (group_ == 0)
    {
        return false;
    }
    return observer_.peerLost(group_, peerOf(config_.role), nowMs, nonrealtime_);
}

void Rendezvous::tick(u64 nowMs)
{
    if (!config_.ipcEnabled)
    {
        state_ = RendezvousState::kValidator;
        return;
    }
    // 回收先于一切：即使没分组、没准备好，25 Hz 的这一拍也要把没人要的映射收掉
    SegmentHandle::reapRetired(nowMs);
    keeper_.tick(nowMs);
    if (group_ == 0)
    {
        state_ = RendezvousState::kUnassigned;
        return;
    }
    if (state_ == RendezvousState::kIpcUnavailable && permanent_)
    {
        return;
    }
    // 退避没到点就什么都不做；时钟倒退得比整个退避窗口还多时不卡住，立刻重试
    if (retryAtMs_ != 0 && nowMs < retryAtMs_ &&
        retryAtMs_ - nowMs <= RetryBackoff::kMaxDelayMs + RendezvousTiming::kConflictRetryMs)
    {
        return;
    }
    retryAtMs_ = 0;
    if (rv_ == nullptr && !openRendezvous(nowMs))
    {
        return;
    }
    RvSegment& rv = *static_cast<RvSegment*>(rv_->base());
    if (observer_.due(nowMs))
    {
        observer_.scan(rv, nowMs);
    }
    if (myCtl_ != 0)
    {
        ownerTick(rv, nowMs);
    }
    else if (prepared_)
    {
        claimTick(rv, nowMs);
    }
    else
    {
        state_ = RendezvousState::kWaitingPrepare;
    }
    if (myCtl_ != 0 && ctlPhase(myCtl_) == kCtlPhaseActive && ring_ == nullptr)
    {
        openRing(nowMs);
    }
}

bool Rendezvous::openRendezvous(u64 nowMs)
{
    if (config_.backend == nullptr)
    {
        permanent_ = true;
        state_ = RendezvousState::kIpcUnavailable;
        lastResult_ = InitResult::kFailed;
        ++failures_;
        return false;
    }
    std::string name;
    if (naming_.name(SegmentId::rendezvous(), name) != NameStatus::kOk)
    {
        permanent_ = true;
        state_ = RendezvousState::kIpcUnavailable;
        lastResult_ = InitResult::kFailedBadName;
        ++failures_;
        return false;
    }
    SegmentView view;
    InitResult result = config_.backend->createOrOpen(name, kRvBytes, view);
    if (result == InitResult::kFailedDenied || result == InitResult::kFailedBadName)
    {
        permanent_ = true; // 宿主沙盒拒绝或名字配错：本进程内不会自己好
        state_ = RendezvousState::kIpcUnavailable;
        lastResult_ = result;
        ++failures_;
        return false;
    }
    if (result != InitResult::kOk)
    {
        state_ = RendezvousState::kOpening;
        lastResult_ = result;
        ++failures_;
        backoff_.reset();
        return false;
    }
    auto* rv = static_cast<RvSegment*>(view.base);
    result = config_.backend->initHeader(view, headerFields(rv->header), kRvHeaderSpec, [rv] {
        rv->header.layout_bytes.store(kRvBytes, std::memory_order_relaxed);
        rv->header.groups.store(kGroupCount, std::memory_order_relaxed);
        rv->header.group_stride.store(kGroupStride, std::memory_order_relaxed);
    });
    lastResult_ = result;
    switch (result)
    {
    case InitResult::kOk:
        if (rv->header.groups.load(std::memory_order_relaxed) != kGroupCount ||
            rv->header.group_stride.load(std::memory_order_relaxed) != kGroupStride ||
            rv->header.layout_bytes.load(std::memory_order_relaxed) < kRvBytes)
        {
            config_.backend->unmap(view); // 几何对我们来说是坏的：当没有这个段，下一拍重来
            state_ = RendezvousState::kOpening;
            ++failures_;
            backoff_.reset();
            return false;
        }
        rv_ = keeper_.adopt(SegmentHandle(std::move(view), config_.backend));
        backoff_.reset();
        failures_ = 0;
        observer_.reset();
        return true;
    case InitResult::kBusy:
        // 契约：kBusy 之后立刻解映射，绝不在原映射上重试
        config_.backend->unmap(view);
        retryAtMs_ = nowMs + backoff_.nextDelayMs();
        state_ = RendezvousState::kOpening;
        ++failures_;
        return false;
    case InitResult::kAbiMismatch:
        abiVersion_ = 0;
        if (view.size >= kRvMinBytes)
        {
            // 只读对端槽里 version 的绝对位置；视图不小于永久冻结的下限才读
            const u32 at = slotVersionOffset(group_, peerOf(config_.role) == Role::kOutput);
            const auto* word =
                reinterpret_cast<const std::atomic<u32>*>(static_cast<const unsigned char*>(view.base) + at);
            abiVersion_ = word->load(std::memory_order_relaxed);
        }
        config_.backend->unmap(view);
        state_ = RendezvousState::kAbiMismatch;
        retryAtMs_ = nowMs + RendezvousTiming::kConflictRetryMs;
        backoff_.reset();
        ++failures_;
        return false;
    default:
        config_.backend->unmap(view);
        state_ = RendezvousState::kOpening;
        ++failures_;
        backoff_.reset();
        return false;
    }
}

bool Rendezvous::openRing(u64 nowMs)
{
    std::string name;
    if (naming_.name(SegmentId::groupData(group_), name) != NameStatus::kOk)
    {
        permanent_ = true;
        state_ = RendezvousState::kIpcUnavailable;
        lastResult_ = InitResult::kFailedBadName;
        ++failures_;
        return false;
    }
    SegmentView view;
    InitResult result = config_.backend->createOrOpen(name, kRingBytes, view);
    if (result == InitResult::kFailedDenied || result == InitResult::kFailedBadName)
    {
        permanent_ = true;
        state_ = RendezvousState::kIpcUnavailable;
        lastResult_ = result;
        ++failures_;
        return false;
    }
    if (result != InitResult::kOk)
    {
        state_ = RendezvousState::kOpening;
        lastResult_ = result;
        ++failures_;
        backoff_.reset();
        return false;
    }
    auto* ring = static_cast<RingSegment*>(view.base);
    result = config_.backend->initHeader(view, headerFields(ring->header), kRingHeaderSpec,
                                         [ring] { LoopRing::initGeometry(*ring); });
    lastResult_ = result;
    switch (result)
    {
    case InitResult::kOk:
        if (!LoopRing::geometryOk(*ring))
        {
            config_.backend->unmap(view);
            state_ = RendezvousState::kOpening;
            ++failures_;
            backoff_.reset();
            return false;
        }
        ring_ = keeper_.adopt(SegmentHandle(std::move(view), config_.backend));
        if (role() == Role::kOutput)
        {
            token_ = makeWriterToken(nowMs);
            LoopRing::publishWriter(*static_cast<RingSegment*>(ring_->base()), token_, format_.sampleRate,
                                    format_.channels);
        }
        publishBinding();
        backoff_.reset();
        failures_ = 0;
        return true;
    case InitResult::kBusy:
        config_.backend->unmap(view);
        retryAtMs_ = nowMs + backoff_.nextDelayMs();
        state_ = RendezvousState::kOpening;
        ++failures_;
        return false;
    case InitResult::kAbiMismatch:
    default:
        // 数据段名带 abi，正常到不了这里；到了也只当暂时性失败，下一拍重试（与 rv 的 kAbiMismatch 不同：
        // 数据段没有端点槽可读版本，而且名字不同就不该撞上别的 abi）
        config_.backend->unmap(view);
        state_ = RendezvousState::kOpening;
        ++failures_;
        backoff_.reset();
        return false;
    }
}

void Rendezvous::claimTick(RvSegment& rv, u64 nowMs)
{
    if (state_ == RendezvousState::kConflict && nowMs < nextConflictRetryMs_)
    {
        return;
    }
    const ClaimIntent effective = state_ == RendezvousState::kConflict ? ClaimIntent::kConflictRetry : intent_;
    RoleSlot& slot = roleSlot(rv, group_, role());
    const u64 observed = slot.ctl.load(std::memory_order_acquire);
    TakeoverTier tier = TakeoverTier::kNone;
    if (ctlPhase(observed) == kCtlPhaseFree)
    {
        tier = TakeoverTier::kFree;
    }
    else
    {
        // 只有观察者手里的那一份和当前整字一致时才拿它判定；不一致说明槽刚动过，这一拍不碰
        const SlotSample& latest = observer_.sample(group_, role());
        if (latest.ctl == observed)
        {
            tier = takeoverTier(latest, nowMs, currentProcessId(), effective, &isProcessAlive);
        }
    }
    if (tier == TakeoverTier::kNone)
    {
        state_ = RendezvousState::kConflict;
        nextConflictRetryMs_ = nowMs + RendezvousTiming::kConflictRetryMs;
        return;
    }
    const u32 claimState = kCtlPhaseClaiming | (effective == ClaimIntent::kSmart ? kCtlProvisional : 0u);
    const CtlCas claimed = casCtl(slot.ctl, observed, claimState);
    if (!claimed.ok)
    {
        state_ = RendezvousState::kConflict;
        nextConflictRetryMs_ = nowMs + RendezvousTiming::kConflictRetryMs;
        return;
    }
    myCtl_ = claimed.word;
    claimMs_ = nowMs;
    dormantMs_ = 0;
    cfgFlags_ = (effective == ClaimIntent::kSmart ? kCfgSmartOrigin : 0u) | kCfgNeverPaired;
    // 先写身份字段、再换 active：换 active 的 release 让这些字段对读方可见
    slot.heartbeat_ms.store(nowMs, std::memory_order_relaxed);
    writeIdentity(slot);
    const CtlCas activated = casCtl(slot.ctl, myCtl_, kCtlPhaseActive | (claimState & kCtlProvisional));
    if (!activated.ok)
    {
        // 别人把我们的 claiming 判成幽灵或接管了：什么都不写，进冲突重试
        myCtl_ = 0;
        state_ = RendezvousState::kConflict;
        nextConflictRetryMs_ = nowMs + RendezvousTiming::kConflictRetryMs;
        return;
    }
    myCtl_ = activated.word;
    lastHeartbeatMs_ = nowMs;
    labelDirty_ = true; // 新属主把自己的标签写上去
    labelOddSinceMs_ = 0;
    // 状态留给 openRing：成功时由 publishBinding 置 kActive，失败时置 kOpening
}

void Rendezvous::ownerTick(RvSegment& rv, u64 nowMs)
{
    RoleSlot& slot = roleSlot(rv, group_, role());
    if (slot.ctl.load(std::memory_order_acquire) != myCtl_)
    {
        lostOwnership(nowMs);
        return;
    }
    const bool heartbeatDue = nowMs < lastHeartbeatMs_ || nowMs - lastHeartbeatMs_ >= RendezvousTiming::kHeartbeatMs;
    if (heartbeatDue)
    {
        slot.heartbeat_ms.store(nowMs, std::memory_order_relaxed);
        // 每 250 ms 从本地副本重写一遍身份块：旧属主在丢槽的瞬间写到一半的字段，最迟一个心跳就被修回来
        writeIdentity(slot);
        lastHeartbeatMs_ = nowMs;
    }
    if (labelDirty_ || heartbeatDue)
    {
        upkeepLabel(slot, nowMs, heartbeatDue);
    }
    refreshState();
}

// 标签的维护，只在自检通过之后调用（这一拍我们仍是属主）。标脏就写；心跳那一拍不脏也读回来核对，不一致就当场
// 重写。两种情况会让标签错下去，这里各修一种：
// - 旧属主丢槽时正在 writeLabel，它晚到的字段写可以留在一个稳定的偶数序号后面（收尾 CAS 挡住了序号，挡不住
//   字段）：心跳读回来发现和我们的不一样，重写；
// - 写方死在奇数与偶数之间，序号永远停在奇数上，writeLabel 一直退让：我们持有这个槽，同一个奇数在心跳上连续停够
//   两个心跳，就只可能是死掉或被挂起的旧写方，用 rewriteStuckLabel 接过序号写上我们的。被挂起的那个醒来之后收尾
//   CAS 失败，留下的字段由上一条修。
// 标签只用于显示，这两个窗口最长约 500 ms，可以接受
void Rendezvous::upkeepLabel(RoleSlot& slot, u64 nowMs, bool heartbeatDue) noexcept
{
    if (!labelDirty_)
    {
        LabelPayload current;
        if (readLabel(slot, current))
        {
            labelOddSinceMs_ = 0;
            if (current == label_)
            {
                return;
            }
            labelDirty_ = true;
        }
    }
    if (labelDirty_ && writeLabel(slot, label_))
    {
        labelDirty_ = false;
        labelOddSinceMs_ = 0;
        return;
    }
    const u32 seq = slot.label_seq.load(std::memory_order_relaxed);
    if ((seq & 1u) == 0)
    {
        labelOddSinceMs_ = 0; // 序号在这一拍里动过：不是卡住，下一拍再试
        return;
    }
    if (!heartbeatDue)
    {
        return; // 卡住与否只按心跳计
    }
    if (labelOddSinceMs_ == 0 || seq != labelOddSeq_ || nowMs < labelOddSinceMs_)
    {
        labelOddSinceMs_ = nowMs; // 第一次看到这个奇数（或时钟倒退）：从这里计
        labelOddSeq_ = seq;
        return;
    }
    if (nowMs - labelOddSinceMs_ >= 2 * RendezvousTiming::kHeartbeatMs)
    {
        // 接手失败说明序号刚动过：保持脏，下一拍照常再写
        labelDirty_ = !rewriteStuckLabel(slot, seq, label_);
        labelOddSinceMs_ = 0;
    }
}

void Rendezvous::publishBinding()
{
    if (ring_ == nullptr)
    {
        // 数据段还没开：没有可绑定的东西，音频线程照「没有对端」处理；开好之后 openRing 再发布
        bindings_.clear();
        refreshState();
        return;
    }
    LoopBinding binding;
    binding.rv = rv_;
    binding.ring = ring_;
    binding.group = group_;
    binding.role = role();
    binding.myCtl = myCtl_;
    binding.writerToken = token_;
    (void)bindings_.publish(binding);
    refreshState();
}

// 持有槽时的状态：数据段开好、绑定已发布才算 kActive；休眠不管数据段开没开都是 kDormant；其余是还在开
void Rendezvous::refreshState() noexcept
{
    if (myCtl_ == 0)
    {
        return;
    }
    if (ctlPhase(myCtl_) == kCtlPhaseDormant)
    {
        state_ = RendezvousState::kDormant;
        return;
    }
    state_ = ring_ != nullptr && bindings_.current() != nullptr ? RendezvousState::kActive : RendezvousState::kOpening;
}

void Rendezvous::writeIdentity(RoleSlot& slot) noexcept
{
    slot.pid.store(currentProcessId(), std::memory_order_relaxed);
    slot.host_ppid.store(config_.hostPpid, std::memory_order_relaxed);
    slot.claim_ms.store(claimMs_, std::memory_order_relaxed);
    slot.dormant_ms.store(dormantMs_, std::memory_order_relaxed);
    slot.format.store(config_.format, std::memory_order_relaxed);
    slot.version.store(config_.version, std::memory_order_relaxed);
    slot.cfg_flags.store(cfgFlags_, std::memory_order_relaxed);
    writeFormat(slot);
}

void Rendezvous::lostOwnership(u64 nowMs)
{
    bindings_.clear();
    if (ring_ != nullptr)
    {
        keeper_.retire(ring_, nowMs); // 句柄留到 kRetireDelayMs 之后，旧绑定可能还有音频线程在跑
        ring_ = nullptr;
    }
    token_ = 0;
    myCtl_ = 0;
    state_ = RendezvousState::kConflict;
    nextConflictRetryMs_ = nowMs + RendezvousTiming::kConflictRetryMs;
}

void Rendezvous::letGo(u64 nowMs)
{
    bindings_.clear();
    if (role() == Role::kOutput && ring_ != nullptr && token_ != 0)
    {
        LoopRing::retireWriter(*static_cast<RingSegment*>(ring_->base()), token_);
    }
    if (myCtl_ != 0 && rv_ != nullptr)
    {
        // 释放的 TOCTOU：CAS 只在我们还持有那个整字时成功；失败说明别人已经接管，什么都不做
        (void)casCtl(roleSlot(*static_cast<RvSegment*>(rv_->base()), group_, role()).ctl, myCtl_, kCtlPhaseFree);
    }
    myCtl_ = 0;
    token_ = 0;
    if (ring_ != nullptr)
    {
        keeper_.retire(ring_, nowMs);
        ring_ = nullptr;
    }
}

void Rendezvous::writeFormat(RoleSlot& slot) noexcept
{
    slot.sample_rate.store(format_.sampleRate, std::memory_order_relaxed);
    slot.max_block.store(format_.maxBlock, std::memory_order_relaxed);
    slot.channels.store(format_.channels, std::memory_order_relaxed);
}

u64 Rendezvous::makeWriterToken(u64 nowMs) noexcept
{
    // splitmix64：pid、时间、实例地址与一个模块级计数器混进去，绝不为 0
    u64 z = static_cast<u64>(currentProcessId());
    z ^= nowMs * 0x9e3779b97f4a7c15ull;
    z ^= static_cast<u64>(reinterpret_cast<std::uintptr_t>(this));
    z ^= gTokenCounter.fetch_add(1, std::memory_order_relaxed) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    z ^= z >> 31;
    return z == 0 ? 1u : z;
}

std::size_t Rendezvous::liveInstances() noexcept
{
    return gLiveInstances.load(std::memory_order_relaxed);
}

} // namespace rinne::ipc
