// SPDX-License-Identifier: GPL-3.0-or-later
//
// 端点槽的扫描节奏与对端丢失判定。观察者只读一段普通内存里的会合段，不碰任何后端：due() 的 250 ms 节拍与时钟
// 倒退、ctl / block_seq 的「从何时起没变」跟踪、reset()、以及 peerLost 的真值表（空闲槽、心跳 2000/2001、
// block_seq 还在动、block_seq 静止 1999/2000、导出中、对端自己声明非实时、心跳从没写过）。
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>

#include "ipc/SlotObserver.h"

namespace
{

using namespace rinne::ipc;

RoleSlot& slotOf(RvSegment& rv, u32 group, Role role) noexcept
{
    return roleSlot(rv, group, role);
}

} // namespace

TEST_CASE("the observer is due every 250 ms and immediately after the clock goes back", "[ipc][observer]")
{
    auto rv = std::make_unique<RvSegment>();
    SlotObserver observer;
    CHECK_FALSE(observer.due(0)); // 读不出时钟：这一拍不扫
    CHECK(observer.due(1000)); // 从没扫过
    observer.scan(*rv, 1000);
    CHECK(observer.scanned());
    CHECK(observer.lastScanMs() == 1000u);
    CHECK_FALSE(observer.due(1000));
    CHECK_FALSE(observer.due(1249));
    CHECK(observer.due(1250));
    CHECK(observer.due(999)); // 时钟倒退：立刻重扫，不拿旧时间卡住

    observer.reset();
    CHECK_FALSE(observer.scanned());
    CHECK(observer.lastScanMs() == 0u);
    CHECK(observer.due(5000));
}

TEST_CASE("ctl and block_seq each keep their own since-timestamp", "[ipc][observer]")
{
    auto rv = std::make_unique<RvSegment>();
    RoleSlot& slot = slotOf(*rv, 2, Role::kInput);
    slot.ctl.store(makeCtl(5, kCtlPhaseActive));
    slot.pid.store(42);
    slot.block_seq.store(7);
    SlotObserver observer;

    observer.scan(*rv, 1000);
    const SlotSample& first = observer.sample(2, Role::kInput);
    CHECK(first.stable);
    CHECK(first.ctl == makeCtl(5, kCtlPhaseActive));
    CHECK(first.pid == 42u);
    CHECK(first.blockSeq == 7u);
    CHECK(first.ctlSinceMs == 1000u);
    CHECK(first.seqSinceMs == 1000u);

    // 什么都没变：两个起点都不动
    observer.scan(*rv, 1250);
    CHECK(observer.sample(2, Role::kInput).ctlSinceMs == 1000u);
    CHECK(observer.sample(2, Role::kInput).seqSinceMs == 1000u);

    // block_seq 动了：只有它的起点刷新
    slot.block_seq.store(8);
    observer.scan(*rv, 1500);
    CHECK(observer.sample(2, Role::kInput).ctlSinceMs == 1000u);
    CHECK(observer.sample(2, Role::kInput).seqSinceMs == 1500u);

    // ctl 动了：它自己刷新，block_seq 的起点保留
    slot.ctl.store(makeCtl(6, kCtlPhaseActive));
    observer.scan(*rv, 1750);
    CHECK(observer.sample(2, Role::kInput).ctlSinceMs == 1750u);
    CHECK(observer.sample(2, Role::kInput).seqSinceMs == 1500u);

    // 时钟倒退：两个起点都从现在重新计
    observer.scan(*rv, 1700);
    CHECK(observer.sample(2, Role::kInput).ctlSinceMs == 1700u);
    CHECK(observer.sample(2, Role::kInput).seqSinceMs == 1700u);

    // reset 之后当作从没扫过：第一个样本从现在起算
    observer.reset();
    observer.scan(*rv, 2000);
    CHECK(observer.sample(2, Role::kInput).ctlSinceMs == 2000u);
    CHECK(observer.sample(2, Role::kInput).seqSinceMs == 2000u);
}

TEST_CASE("peerLost needs a stale heartbeat and a still block_seq, and never fires during exports", "[ipc][observer]")
{
    auto rv = std::make_unique<RvSegment>();
    RoleSlot& peer = slotOf(*rv, 4, Role::kOutput);
    peer.ctl.store(makeCtl(2, kCtlPhaseActive));
    peer.pid.store(77);
    peer.heartbeat_ms.store(1000000);
    peer.block_seq.store(3);
    SlotObserver observer;
    observer.scan(*rv, 1000000);

    // 心跳正好 2000 ms 还不算丢失；2001 才算
    CHECK_FALSE(observer.peerLost(4, Role::kOutput, 1002000, false));
    CHECK(observer.peerLost(4, Role::kOutput, 1002001, false));
    // 本地在导出：一律不判丢失
    CHECK_FALSE(observer.peerLost(4, Role::kOutput, 1002001, true));
    // 对端自己声明非实时（它也在导出）
    peer.rt_flags.store(kRtNonrealtime);
    observer.scan(*rv, 1002001);
    CHECK_FALSE(observer.peerLost(4, Role::kOutput, 1003000, false));
    peer.rt_flags.store(0);

    // block_seq 还在动：心跳再旧也不算丢失
    peer.block_seq.store(4);
    observer.scan(*rv, 1003250);
    CHECK(observer.sample(4, Role::kOutput).seqSinceMs == 1003250u);
    CHECK_FALSE(observer.peerLost(4, Role::kOutput, 1003500, false));

    // 空闲槽永远不算丢失
    peer.ctl.store(makeCtl(3, kCtlPhaseFree));
    observer.scan(*rv, 1004000);
    CHECK_FALSE(observer.peerLost(4, Role::kOutput, 1010000, false));

    // 心跳从没写过（0）按最陈旧算
    peer.ctl.store(makeCtl(4, kCtlPhaseActive));
    peer.heartbeat_ms.store(0);
    peer.block_seq.store(9);
    observer.scan(*rv, 1005000);
    CHECK(observer.peerLost(4, Role::kOutput, 1007000, false));
}

TEST_CASE("peerLost needs block_seq still for exactly 2000 ms", "[ipc][observer]")
{
    auto rv = std::make_unique<RvSegment>();
    RoleSlot& peer = slotOf(*rv, 5, Role::kInput);
    peer.ctl.store(makeCtl(2, kCtlPhaseActive));
    peer.heartbeat_ms.store(1000000);
    peer.block_seq.store(1);
    SlotObserver observer;
    observer.scan(*rv, 1000000);
    peer.block_seq.store(2); // 这次扫描时 block_seq 还在动：静止从这里计
    observer.scan(*rv, 1010000);
    REQUIRE(observer.sample(5, Role::kInput).seqSinceMs == 1010000u);

    // 心跳早已陈旧，只差 block_seq：静止 1999 ms 不算失联，2000 ms 才算
    CHECK_FALSE(observer.peerLost(5, Role::kInput, 1011999, false));
    CHECK(observer.peerLost(5, Role::kInput, 1012000, false));
}
