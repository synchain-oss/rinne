// SPDX-License-Identifier: GPL-3.0-or-later
//
// 会合段的只读快照。打开只走 openExistingReadOnly 与 checkHeaderReadOnly，段一律经 const 指针读，类型上就写不了。
#include "pairing/PairingSnapshot.h"

#include <atomic>
#include <cstddef>
#include <string_view>
#include <utility>

namespace rinne::pairing
{
namespace
{

// 与会合打开会合段时用的同一套段头规格（magic、abi、本 abi 的段大小）
constexpr ipc::SegmentHeaderSpec kRvSpec{ipc::kRvMagic, ipc::kIpcAbi, ipc::kRvBytes, ipc::kRvHeaderBytes};

std::size_t roleIndex(ipc::Role role) noexcept
{
    return role == ipc::Role::kOutput ? 1u : 0u;
}

// 显示前缀只在是合法 UTF-8 时才交出：标签是跨进程的不可信数据（别的版本、混写的一份）。
// 缺续字节、过长编码、代理区码点、超出 U+10FFFF 都算坏
bool validUtf8(std::string_view text) noexcept
{
    std::size_t i = 0;
    while (i < text.size())
    {
        const auto lead = static_cast<unsigned char>(text[i]);
        if (lead < 0x80u)
        {
            ++i;
            continue;
        }
        std::size_t extra = 0;
        u32 cp = 0;
        u32 least = 0;
        if ((lead & 0xE0u) == 0xC0u)
        {
            extra = 1;
            cp = lead & 0x1Fu;
            least = 0x80u;
        }
        else if ((lead & 0xF0u) == 0xE0u)
        {
            extra = 2;
            cp = lead & 0x0Fu;
            least = 0x800u;
        }
        else if ((lead & 0xF8u) == 0xF0u)
        {
            extra = 3;
            cp = lead & 0x07u;
            least = 0x10000u;
        }
        else
        {
            return false;
        }
        if (text.size() - i <= extra)
            return false;
        for (std::size_t k = 1; k <= extra; ++k)
        {
            const auto next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xC0u) != 0x80u)
                return false;
            cp = (cp << 6) | (next & 0x3Fu);
        }
        if (cp < least || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu))
            return false;
        i += extra + 1;
    }
    return true;
}

} // namespace

PeerFacts peerFactsOf(const EndFacts& end)
{
    PeerFacts peer;
    peer.present = end.stable && end.phase == ipc::kCtlPhaseActive;
    if (!peer.present)
        return peer;
    peer.pid = end.pid;
    peer.version = end.version;
    peer.sampleRate = end.sampleRate;
    peer.channels = end.channels;
    peer.nameHash = end.nameHash;
    peer.name = end.name;
    peer.colourArgb = end.colourArgb;
    return peer;
}

PairingSnapshot::PairingSnapshot(ipc::ISegmentBackend* backend, const ipc::NameScope& scope) : backend_(backend)
{
    std::string name;
    if (ipc::Naming(scope).name(ipc::SegmentId::rendezvous(), name) == ipc::NameStatus::kOk)
        name_ = std::move(name);
    else
        unavailable_ = true; // 名字不合法：会合自己也开不了，配对状态显示 IPC 不可用
}

PairingSnapshot::~PairingSnapshot()
{
    close();
}

void PairingSnapshot::close() noexcept
{
    if (view_.base != nullptr)
        backend_->unmap(view_);
    view_.reset();
}

void PairingSnapshot::refresh(u64 nowMs, bool force)
{
    if (const ipc::RvSegment* rv = segment())
    {
        observer_.scan(*rv, nowMs);
        readLabels();
        return;
    }
    if (unavailable_ || backend_ == nullptr || !dueToOpen(nowMs, force))
        return;
    open(nowMs);
}

// 从没试过就试；force 只在上一次是「段不存在」（kFailed）时不看间隔。还没发布完（kBusy）按 kRetryMs、ABI 不同的段按
// kAbiRetryMs，都不受 force 影响。时钟倒退时立刻再试，不拿旧时间卡住（与观察者的 due 同一个口径）
bool PairingSnapshot::dueToOpen(u64 nowMs, bool force) const noexcept
{
    if (!attempted_)
        return true;
    if (force && lastResult_ == ipc::InitResult::kFailed)
        return true;
    const u64 interval = lastResult_ == ipc::InitResult::kAbiMismatch ? kAbiRetryMs : kRetryMs;
    return nowMs < lastAttemptMs_ || nowMs - lastAttemptMs_ >= interval;
}

void PairingSnapshot::open(u64 nowMs)
{
    ipc::SegmentView view;
    ipc::InitResult result = backend_->openExistingReadOnly(name_, view);
    if (result == ipc::InitResult::kOk)
    {
        const auto* rv = static_cast<const ipc::RvSegment*>(view.base);
        result = backend_->checkHeaderReadOnly(view, rv->header.magic, rv->header.abi, kRvSpec);
        // 几何不对的段当作还没发布完（会合自己也不用它）
        if (result == ipc::InitResult::kOk &&
            (rv->header.groups.load(std::memory_order_relaxed) != ipc::kGroupCount ||
             rv->header.group_stride.load(std::memory_order_relaxed) != ipc::kGroupStride ||
             rv->header.layout_bytes.load(std::memory_order_relaxed) < ipc::kRvBytes))
            result = ipc::InitResult::kBusy;
    }
    attempted_ = true;
    lastAttemptMs_ = nowMs;
    lastResult_ = result;
    if (result == ipc::InitResult::kOk)
    {
        view_ = std::move(view);
        misses_ = 0;
        observer_.reset();
        labels_ = {};
        observer_.scan(*segment(), nowMs);
        readLabels();
        return;
    }
    if (view.base != nullptr)
        backend_->unmap(view);
    if (result == ipc::InitResult::kFailed)
        misses_ = misses_ < 2u ? misses_ + 1u : misses_;
    else
        misses_ = 0;
    unavailable_ = result == ipc::InitResult::kFailedDenied || result == ipc::InitResult::kFailedBadName;
}

bool PairingSnapshot::usable() const noexcept
{
    return mapped() || (!unavailable_ && lastResult_ == ipc::InitResult::kFailed && misses_ >= 2u);
}

// 连续两次读到同一份才信它（散列只拿来比较两端相同与否）；读不稳的一拍留着上一次的，槽换了主人（ctl 变了）又读不稳
// 就当未知；槽空了就清掉
void PairingSnapshot::readLabels() noexcept
{
    const ipc::RvSegment* rv = segment();
    if (rv == nullptr)
        return;
    for (u32 group = 1; group <= ipc::kGroupCount; ++group)
    {
        for (const ipc::Role role : {ipc::Role::kInput, ipc::Role::kOutput})
        {
            LabelCache& cache = labels_[group - 1u][roleIndex(role)];
            const ipc::SlotSample& sample = observer_.sample(group, role);
            if (!sample.stable)
                continue;
            if (ipc::ctlPhase(sample.ctl) == ipc::kCtlPhaseFree)
            {
                cache = LabelCache{};
                continue;
            }
            const ipc::RoleSlot& slot = ipc::roleSlot(*rv, group, role);
            ipc::LabelPayload first;
            ipc::LabelPayload second;
            if (ipc::readLabel(slot, first) && ipc::readLabel(slot, second) && first == second)
            {
                cache.ctl = sample.ctl;
                cache.known = true;
                cache.label = first;
            }
            else if (cache.ctl != sample.ctl)
            {
                cache = LabelCache{};
                cache.ctl = sample.ctl;
            }
        }
    }
}

const PairingSnapshot::LabelCache& PairingSnapshot::label(u32 group, ipc::Role role) const noexcept
{
    return labels_[group - 1u][roleIndex(role)];
}

ipc::EndView PairingSnapshot::endView(u32 group, ipc::Role role, u64 nowMs) const noexcept
{
    ipc::EndView view;
    if (!mapped())
        return view;
    const ipc::SlotSample& sample = observer_.sample(group, role);
    if (!sample.stable)
    {
        view.phase = ipc::kCtlPhaseClaiming;
        return view;
    }
    view.phase = ipc::ctlPhase(sample.ctl);
    view.fresh = ipc::ageMs(nowMs, sample.heartbeatMs) < ipc::RendezvousTiming::kLostHeartbeatMs;
    view.pid = sample.pid;
    view.claimMs = sample.claimMs;
    const LabelCache& cache = label(group, role);
    view.nameHash = cache.known && cache.label.nameLen != 0u ? cache.label.nameHash : 0u;
    view.neverPaired = (sample.cfgFlags & ipc::kCfgNeverPaired) != 0u;
    return view;
}

ipc::GroupViews PairingSnapshot::groupViews(u64 nowMs) const noexcept
{
    ipc::GroupViews groups{};
    for (u32 group = 1; group <= ipc::kGroupCount; ++group)
    {
        groups[group - 1u].input = endView(group, ipc::Role::kInput, nowMs);
        groups[group - 1u].output = endView(group, ipc::Role::kOutput, nowMs);
    }
    return groups;
}

EndFacts PairingSnapshot::end(u32 group, ipc::Role role) const
{
    EndFacts facts;
    const ipc::RvSegment* rv = segment();
    if (rv == nullptr || group < 1u || group > ipc::kGroupCount)
        return facts;
    const ipc::RoleSlot& slot = ipc::roleSlot(*rv, group, role);
    facts.safetyState = slot.safety_state.load(std::memory_order_relaxed);
    facts.maxNSeen = slot.max_n_seen.load(std::memory_order_relaxed);
    const ipc::SlotSample& sample = observer_.sample(group, role);
    if (!sample.stable)
        return facts;
    facts.stable = true;
    facts.phase = ipc::ctlPhase(sample.ctl);
    facts.pid = sample.pid;
    facts.version = sample.version;
    facts.sampleRate = sample.sampleRate;
    facts.maxBlock = sample.maxBlock;
    facts.channels = sample.channels;
    facts.rtFlags = sample.rtFlags;
    facts.heartbeatMs = sample.heartbeatMs;
    const LabelCache& cache = label(group, role);
    if (!cache.known)
        return facts;
    facts.colourArgb = cache.label.colourArgb;
    if (cache.label.nameLen != 0u)
    {
        facts.nameHash = cache.label.nameHash;
        std::string name = ipc::labelDisplayName(cache.label);
        if (validUtf8(name))
            facts.name = std::move(name);
    }
    return facts;
}

} // namespace rinne::pairing
