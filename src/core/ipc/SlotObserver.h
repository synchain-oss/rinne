// SPDX-License-Identifier: GPL-3.0-or-later
//
// 会合段 16 个端点槽的扫描视图。消息线程每 250 ms 扫一遍，把每个槽做成一份前后一致的快照：ctl 先读一次，其余字段
// 读完再读一次，两次相同才算稳定；不稳定的一拍不拿它做任何接管判定，宁可晚一拍。
// 每个槽按 ctl 与 block_seq 各跟踪一个「从何时起没变过」的时间戳。接管判据要的是「同一个 ctl 已经被观察了至少
// N 毫秒」，而不是槽里写的 claim_ms 有多老：claim_ms 可能还留着上一任的时间（刚 CAS 过、字段还没来得及重写），
// 也可能被一个写到一半就死掉的属主留在那里。block_seq 的静止用来把「对端还活着但消息线程没跑」与「对端真的
// 没了」分开。字段一律 relaxed 读，前后两次 ctl 读守住一致性；acquire / release 的配对在写方。
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

#include "ipc/LoopBinding.h"
#include "ipc/SegmentLayout.h"

namespace rinne::ipc
{

// now 与某个时间戳的毫秒差。时间戳为 0 表示「从没写过」，按最陈旧算；时间戳在 now 之后（两边时钟不一致或写方
// 写坏）按刚刚算，绝不让一个未来的时间戳把对端说成活的
inline u64 ageMs(u64 nowMs, u64 thenMs) noexcept
{
    if (thenMs == 0)
    {
        return std::numeric_limits<u64>::max();
    }
    return thenMs > nowMs ? 0 : nowMs - thenMs;
}

// 一次扫描里某个槽的字段。stable 表示前后两次 ctl 读相同；ctlSinceMs / seqSinceMs 是这次扫描之后「这个值已经
// 连续保持了多久」的起点（扫描时刻）
struct SlotSample
{
    u64 ctl = 0;
    u32 pid = 0;
    u64 heartbeatMs = 0;
    u64 claimMs = 0;
    u64 dormantMs = 0;
    u64 blockSeq = 0;
    u32 rtFlags = 0;
    u32 version = 0;
    u32 sampleRate = 0;
    u32 maxBlock = 0;
    u32 channels = 0;
    u32 format = 0;
    u32 cfgFlags = 0;
    bool stable = false;
    u64 ctlSinceMs = 0;
    u64 seqSinceMs = 0;
};

class SlotObserver
{
public:
    static constexpr u64 kScanIntervalMs = 250;

    // 该扫了：从没扫过、距上次满 250 ms、或者时钟倒退（倒退时立刻重扫，不拿旧时间当基准卡住）
    bool due(u64 nowMs) const noexcept;
    // 扫全部 16 个槽。nowMs == 0 时什么都不做（读不出时钟的一拍不刷新任何「静止时长」的起点）
    void scan(const RvSegment& rv, u64 nowMs) noexcept;
    void reset() noexcept;
    bool scanned() const noexcept { return scanned_; }
    u64 lastScanMs() const noexcept { return lastScanMs_; }
    // 组号 1..8 是前提条件
    const SlotSample& sample(u32 group, Role role) const noexcept;

    // 对端丢失：槽被占着、心跳超过 2 s 没动、block_seq 也静止了至少 2 s，并且双方都不在离线导出里。
    // 导出时宿主可能饿死消息线程，心跳会陈旧而音频还在跑，所以导出期间一律不判丢失
    bool peerLost(u32 group, Role peerRole, u64 nowMs, bool localNonrealtime) const noexcept;

private:
    static constexpr std::size_t kRoleCount = 2;
    static std::size_t index(Role role) noexcept { return role == Role::kOutput ? 1u : 0u; }

    SlotSample samples_[kGroupCount][kRoleCount];
    bool have_[kGroupCount][kRoleCount] = {};
    bool scanned_ = false;
    u64 lastScanMs_ = 0;
};

} // namespace rinne::ipc
