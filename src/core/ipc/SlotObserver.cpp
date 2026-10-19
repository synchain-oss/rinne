// SPDX-License-Identifier: GPL-3.0-or-later
//
// 端点槽的扫描与对端丢失判定。只读共享内存、只写自己的成员，不分配、不取锁；时间全部由调用方注入。
// 每个槽一次扫描只读两遍 ctl，字段夹在中间：稳定才可信，不稳定就整条样本作废（stable = false），
// 接管判定看到 stable=false 一律返回「不接管」。
#include "ipc/SlotObserver.h"

#include "ipc/Rendezvous.h"

namespace rinne::ipc
{

bool SlotObserver::due(u64 nowMs) const noexcept
{
    if (nowMs == 0)
    {
        return false;
    }
    if (!scanned_)
    {
        return true;
    }
    return nowMs < lastScanMs_ || nowMs - lastScanMs_ >= kScanIntervalMs;
}

void SlotObserver::scan(const RvSegment& rv, u64 nowMs) noexcept
{
    if (nowMs == 0)
    {
        return;
    }
    for (u32 group = 1; group <= kGroupCount; ++group)
    {
        for (const Role role : {Role::kInput, Role::kOutput})
        {
            const RoleSlot& slot = roleSlot(rv, group, role);
            SlotSample next;
            const u64 before = slot.ctl.load(std::memory_order_acquire);
            next.pid = slot.pid.load(std::memory_order_relaxed);
            next.heartbeatMs = slot.heartbeat_ms.load(std::memory_order_relaxed);
            next.claimMs = slot.claim_ms.load(std::memory_order_relaxed);
            next.dormantMs = slot.dormant_ms.load(std::memory_order_relaxed);
            next.blockSeq = slot.block_seq.load(std::memory_order_relaxed);
            next.rtFlags = slot.rt_flags.load(std::memory_order_relaxed);
            next.version = slot.version.load(std::memory_order_relaxed);
            next.sampleRate = slot.sample_rate.load(std::memory_order_relaxed);
            next.maxBlock = slot.max_block.load(std::memory_order_relaxed);
            next.channels = slot.channels.load(std::memory_order_relaxed);
            next.format = slot.format.load(std::memory_order_relaxed);
            next.cfgFlags = slot.cfg_flags.load(std::memory_order_relaxed);
            const u64 after = slot.ctl.load(std::memory_order_acquire);
            next.stable = before == after;
            next.ctl = after;
            const auto gi = static_cast<std::size_t>(group - 1);
            const auto ri = index(role);
            const SlotSample& previous = samples_[gi][ri];
            const bool had = have_[gi][ri];
            const bool clockWentBack = had && nowMs < lastScanMs_;
            // 同一个 ctl / block_seq 一直没变就保留原来的起点；变了或者时钟倒退就从这个扫描时刻重新计
            next.ctlSinceMs = (!had || clockWentBack || previous.ctl != next.ctl) ? nowMs : previous.ctlSinceMs;
            next.seqSinceMs =
                (!had || clockWentBack || previous.blockSeq != next.blockSeq) ? nowMs : previous.seqSinceMs;
            samples_[gi][ri] = next;
            have_[gi][ri] = true;
        }
    }
    lastScanMs_ = nowMs;
    scanned_ = true;
}

void SlotObserver::reset() noexcept
{
    for (std::size_t g = 0; g < kGroupCount; ++g)
    {
        for (std::size_t r = 0; r < kRoleCount; ++r)
        {
            samples_[g][r] = SlotSample{};
            have_[g][r] = false;
        }
    }
    scanned_ = false;
    lastScanMs_ = 0;
}

const SlotSample& SlotObserver::sample(u32 group, Role role) const noexcept
{
    return samples_[static_cast<std::size_t>(group - 1)][index(role)];
}

bool SlotObserver::peerLost(u32 group, Role peerRole, u64 nowMs, bool localNonrealtime) const noexcept
{
    if (localNonrealtime || !scanned_ || group == 0 || group > kGroupCount)
    {
        return false;
    }
    const auto gi = static_cast<std::size_t>(group - 1);
    const auto ri = index(peerRole);
    if (!have_[gi][ri])
    {
        return false;
    }
    const SlotSample& s = samples_[gi][ri];
    if (ctlPhase(s.ctl) == kCtlPhaseFree)
    {
        return false; // 没人占着：没有「丢失」一说
    }
    if ((s.rtFlags & kRtNonrealtime) != 0)
    {
        return false; // 对端自己在离线导出
    }
    if (ageMs(nowMs, s.heartbeatMs) <= RendezvousTiming::kLostHeartbeatMs)
    {
        return false; // 心跳还新鲜
    }
    return ageMs(nowMs, s.seqSinceMs) >= RendezvousTiming::kLostStillMs;
}

} // namespace rinne::ipc
