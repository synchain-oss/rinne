// SPDX-License-Identifier: GPL-3.0-or-later
//
// 接管：死进程（pid 真的死了，观察 5 s）、活着的外进程（永远不接）、幽灵认领（claiming 卡住 1 s）、
// 休眠槽（死进程 5 s；同进程的活实例只有用户确认才拿，拿完之后原属主的 prepare 输掉 CAS）、
// 同进程孤儿（30 s 不动才接）。观察时长与心跳/block_seq 的静止一起算，所以每个边界都要在「差 1 ms」上验一次。
// 冲突期间的自动重试每 1 s 一次，落不到「差 1 ms」那一刻上：用例在边界前 1 ms 与边界上各用同组 setGroup 给一次显式
// 意图，让实例当场再试，这样两次尝试都真的发生了，判定的是阈值本身而不是重试节拍。
#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include "../backends/ChildProcess.h"
#include "RendezvousTestKit.h"
#include "ipc/ProcessInfo.h"
#include "ipc/Rendezvous.h"

namespace
{

using namespace rinne::test;

constexpr u64 kT0 = 2000000;

} // namespace

TEST_CASE("a dead foreign process is taken over after five seconds, not before", "[ipc][takeover]")
{
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    CountingBackend backend;
    SegmentView rvView = openRv(backend, naming);
    RvSegment& rv = rvOf(rvView);
    RoleSlot& slot = roleSlot(rv, 4, Role::kOutput);
    const u64 old = makeCtl(5, kCtlPhaseActive);
    slot.ctl.store(old);
    slot.pid.store(kDeadPid);
    slot.heartbeat_ms.store(kT0 - 6000);
    slot.claim_ms.store(kT0 - 60000);
    slot.block_seq.store(7);

    Rendezvous r(configFor(Role::kOutput, backend, naming.scope()));
    r.setGroup(4, ClaimIntent::kConflictRetry, kT0);
    r.prepare({48000, 512, 2}, kT0);
    r.tick(kT0);
    CHECK(r.state() == RendezvousState::kConflict);
    CHECK(slot.ctl.load() == old);

    // 自动重试：观察 4 s 时还不够
    r.tick(kT0 + RendezvousTiming::kDeadMs - 1000);
    CHECK(r.state() == RendezvousState::kConflict);
    CHECK(slot.ctl.load() == old);
    // 差 1 ms：当场再试一次，仍然不接
    r.setGroup(4, ClaimIntent::kManual, kT0 + RendezvousTiming::kDeadMs - 1);
    r.tick(kT0 + RendezvousTiming::kDeadMs - 1);
    CHECK(r.state() == RendezvousState::kConflict);
    CHECK(slot.ctl.load() == old);
    // 正好 5 s：接管
    r.setGroup(4, ClaimIntent::kManual, kT0 + RendezvousTiming::kDeadMs);
    r.tick(kT0 + RendezvousTiming::kDeadMs);
    CHECK(r.state() == RendezvousState::kActive);
    CHECK(ctlPhase(slot.ctl.load()) == kCtlPhaseActive);
    CHECK(ctlGen(slot.ctl.load()) == 7u); // 5 -> 6 claiming -> 7 active
    CHECK(slot.claim_ms.load() == kT0 + RendezvousTiming::kDeadMs);
    CHECK(slot.pid.load() == currentProcessId());
}

TEST_CASE("a live foreign process is never taken over, however stale", "[ipc][takeover]")
{
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    CountingBackend backend;
    SegmentView rvView = openRv(backend, naming);
    RvSegment& rv = rvOf(rvView);

    ChildProcess child;
    if (!child.started())
    {
        WARN("child process could not be spawned in this sandbox; skipping the live-pid case");
        return;
    }
    RoleSlot& slot = roleSlot(rv, 4, Role::kOutput);
    const u64 old = makeCtl(5, kCtlPhaseActive);
    slot.ctl.store(old);
    slot.pid.store(child.pid());
    slot.heartbeat_ms.store(kT0 - 3600000);
    slot.claim_ms.store(kT0 - 7200000);
    slot.block_seq.store(7);
    REQUIRE(isProcessAlive(child.pid()));

    Rendezvous r(configFor(Role::kOutput, backend, naming.scope()));
    r.setGroup(4, ClaimIntent::kConflictRetry, kT0);
    r.prepare({48000, 512, 2}, kT0);
    r.tick(kT0);
    r.tick(kT0 + 3600000); // 一个小时之后
    CHECK(r.state() == RendezvousState::kConflict);
    CHECK(slot.ctl.load() == old);
}

TEST_CASE("a ghost claim is taken over after one second of watching the same word", "[ipc][takeover]")
{
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    CountingBackend backend;
    SegmentView rvView = openRv(backend, naming);
    RvSegment& rv = rvOf(rvView);
    RoleSlot& slot = roleSlot(rv, 4, Role::kOutput);
    const u64 old = makeCtl(3, kCtlPhaseClaiming);
    slot.ctl.store(old);
    slot.pid.store(kDeadPid);
    slot.claim_ms.store(kT0 - 60000); // 认领者早就没动静了
    slot.heartbeat_ms.store(kT0 - 60000);
    slot.block_seq.store(7);

    Rendezvous r(configFor(Role::kOutput, backend, naming.scope()));
    r.setGroup(4, ClaimIntent::kManual, kT0);
    r.prepare({48000, 512, 2}, kT0);
    r.tick(kT0);
    CHECK(r.state() == RendezvousState::kConflict);
    r.setGroup(4, ClaimIntent::kManual, kT0 + RendezvousTiming::kGhostMs - 1);
    r.tick(kT0 + RendezvousTiming::kGhostMs - 1);
    CHECK(r.state() == RendezvousState::kConflict);
    CHECK(slot.ctl.load() == old);
    r.setGroup(4, ClaimIntent::kManual, kT0 + RendezvousTiming::kGhostMs);
    r.tick(kT0 + RendezvousTiming::kGhostMs);
    CHECK(r.state() == RendezvousState::kActive);
    CHECK(ctlGen(slot.ctl.load()) == 5u); // 3 -> 4 claiming -> 5 active
}

TEST_CASE("a dormant dead process is taken over as a dead process, not as dormant", "[ipc][takeover]")
{
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    CountingBackend backend;
    SegmentView rvView = openRv(backend, naming);
    RvSegment& rv = rvOf(rvView);
    RoleSlot& slot = roleSlot(rv, 4, Role::kOutput);
    const u64 old = makeCtl(4, kCtlPhaseDormant);
    slot.ctl.store(old);
    slot.pid.store(kDeadPid);
    slot.heartbeat_ms.store(kT0 - 6000);
    slot.claim_ms.store(kT0 - 60000);
    slot.dormant_ms.store(kT0 - 6000);
    slot.block_seq.store(7);

    Rendezvous r(configFor(Role::kOutput, backend, naming.scope()));
    r.setGroup(4, ClaimIntent::kConflictRetry, kT0); // 自动冲突重试永远不拿休眠槽：只能走死进程那一档
    r.prepare({48000, 512, 2}, kT0);
    r.tick(kT0);
    CHECK(r.state() == RendezvousState::kConflict);
    r.tick(kT0 + RendezvousTiming::kDeadMs - 1000);
    CHECK(r.state() == RendezvousState::kConflict);
    CHECK(slot.ctl.load() == old);
    // kManual 同样不拿休眠槽：当场再试只可能走死进程那一档
    r.setGroup(4, ClaimIntent::kManual, kT0 + RendezvousTiming::kDeadMs - 1);
    r.tick(kT0 + RendezvousTiming::kDeadMs - 1);
    CHECK(r.state() == RendezvousState::kConflict);
    r.setGroup(4, ClaimIntent::kManual, kT0 + RendezvousTiming::kDeadMs);
    r.tick(kT0 + RendezvousTiming::kDeadMs);
    CHECK(r.state() == RendezvousState::kActive);
    CHECK(ctlGen(slot.ctl.load()) == 6u); // 4 -> 5 claiming -> 6 active
    CHECK(slot.claim_ms.load() == kT0 + RendezvousTiming::kDeadMs);
}

TEST_CASE("a dormant slot of our own process needs a confirmed takeover", "[ipc][takeover]")
{
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    CountingBackend backend;
    SegmentView rvView = openRv(backend, naming);
    RvSegment& rv = rvOf(rvView);
    const StreamFormat format{48000, 512, 2};
    const u64 t1 = kT0 + 100;

    Rendezvous first(configFor(Role::kInput, backend, naming.scope(), 61));
    first.setGroup(8, ClaimIntent::kManual, kT0);
    first.prepare(format, kT0);
    first.tick(kT0);
    REQUIRE(first.state() == RendezvousState::kActive);
    first.releaseResources(t1);
    REQUIRE(ctlPhase(roleSlot(rv, 8, Role::kInput).ctl.load()) == kCtlPhaseDormant);

    Rendezvous second(configFor(Role::kInput, backend, naming.scope(), 62));
    second.setGroup(8, ClaimIntent::kManual, t1);
    second.prepare(format, t1);
    second.tick(t1); // 观察到休眠槽，kManual 不拿
    CHECK(second.state() == RendezvousState::kConflict);

    // 用户确认接管：同组再给一次意图，当场再试。休眠不满 2 s 时确认了也不拿
    second.setGroup(8, ClaimIntent::kManualConfirmed, t1 + RendezvousTiming::kDormantMs - 1);
    second.tick(t1 + RendezvousTiming::kDormantMs - 1);
    CHECK(second.state() == RendezvousState::kConflict);
    // 满了 2 s，但意图是 kManual：仍不拿
    second.setGroup(8, ClaimIntent::kManual, t1 + RendezvousTiming::kDormantMs);
    second.tick(t1 + RendezvousTiming::kDormantMs);
    CHECK(second.state() == RendezvousState::kConflict);
    // 同一时刻确认：拿
    second.setGroup(8, ClaimIntent::kManualConfirmed, t1 + RendezvousTiming::kDormantMs);
    second.tick(t1 + RendezvousTiming::kDormantMs);
    CHECK(second.state() == RendezvousState::kActive);
    CHECK(roleSlot(rv, 8, Role::kInput).ctl.load() == second.myCtl());

    // 原属主还攥着自己的 dormant 整字：prepare 的 CAS 必输，进冲突
    first.prepare(format, t1 + RendezvousTiming::kDormantMs + 100);
    CHECK(first.state() == RendezvousState::kConflict);
    CHECK(first.myCtl() == 0u);
}

TEST_CASE("a same-process orphan is taken over at thirty seconds, not at 29999 ms", "[ipc][takeover]")
{
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    CountingBackend backend;
    SegmentView rvView = openRv(backend, naming);
    RvSegment& rv = rvOf(rvView);
    RoleSlot& slot = roleSlot(rv, 4, Role::kOutput);
    const u64 old = makeCtl(5, kCtlPhaseActive);
    slot.ctl.store(old);
    slot.pid.store(currentProcessId());
    slot.heartbeat_ms.store(kT0 - 40000);
    slot.claim_ms.store(kT0 - 90000);
    slot.block_seq.store(7);

    Rendezvous r(configFor(Role::kOutput, backend, naming.scope()));
    r.setGroup(4, ClaimIntent::kConflictRetry, kT0);
    r.prepare({48000, 512, 2}, kT0);
    r.tick(kT0);
    CHECK(r.state() == RendezvousState::kConflict);

    // 自动重试：观察 29 s 还不够
    r.tick(kT0 + RendezvousTiming::kOrphanMs - 1000);
    CHECK(r.state() == RendezvousState::kConflict);
    CHECK(slot.ctl.load() == old);
    r.setGroup(4, ClaimIntent::kManual, kT0 + RendezvousTiming::kOrphanMs - 1);
    r.tick(kT0 + RendezvousTiming::kOrphanMs - 1);
    CHECK(r.state() == RendezvousState::kConflict);
    CHECK(slot.ctl.load() == old);
    r.setGroup(4, ClaimIntent::kManual, kT0 + RendezvousTiming::kOrphanMs);
    r.tick(kT0 + RendezvousTiming::kOrphanMs);
    CHECK(r.state() == RendezvousState::kActive);
    CHECK(ctlGen(slot.ctl.load()) == 7u);
    CHECK(slot.claim_ms.load() == kT0 + RendezvousTiming::kOrphanMs);
}
