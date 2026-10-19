// SPDX-License-Identifier: GPL-3.0-or-later
//
// 会合的状态机：认领空闲槽、没 prepare 不认领、同组冲突与 1 s 重试、被接管后的自检、心跳节奏、休眠与唤醒、
// 换组、析构释放、provisional 认领与抢占、validator、kBusy 退避、abi 不兼容、后端拒绝、25 Hz 回收与最后一个
// 实例的收尾、标签的所有权与修复、对端丢失。时间全部是合成的；只有涉及 SegmentHandle 析构的用例用真实时钟
// （句柄析构里调的是 util::monoNowMs()）。
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "RendezvousTestKit.h"
#include "ipc/ProcessInfo.h"
#include "ipc/Rendezvous.h"
#include "util/MonoClock.h"

namespace
{

using namespace rinne::test;

constexpr u64 kT0 = 1000000;

void drainRetired()
{
    SegmentHandle::reapRetired(1);
    SegmentHandle::reapRetired(1 + SegmentHandle::kReleaseGraceMs);
}

} // namespace

TEST_CASE("a prepared instance claims a free slot and publishes its binding", "[ipc][rendezvous]")
{
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    CountingBackend backend;
    SegmentView rvView = openRv(backend, naming);
    RvSegment& rv = rvOf(rvView);
    backend.resetCounts();
    const StreamFormat format{48000, 512, 2};

    Rendezvous input(configFor(Role::kInput, backend, naming.scope(), 11));
    input.setGroup(3, ClaimIntent::kManual, kT0);
    input.prepare(format, kT0);
    input.tick(kT0);

    CHECK(input.state() == RendezvousState::kActive);
    const u64 mine = input.myCtl();
    REQUIRE(mine != 0);
    RoleSlot& inputSlot = roleSlot(rv, 3, Role::kInput);
    CHECK(inputSlot.ctl.load() == mine);
    CHECK(ctlPhase(mine) == kCtlPhaseActive);
    CHECK(ctlGen(mine) == 2u); // 0 空闲 -> 1 claiming -> 2 active
    CHECK(inputSlot.pid.load() == currentProcessId());
    CHECK(inputSlot.host_ppid.load() == 4242u);
    CHECK(inputSlot.claim_ms.load() == kT0);
    CHECK(inputSlot.heartbeat_ms.load() == kT0);
    CHECK(inputSlot.sample_rate.load() == 48000u);
    CHECK(inputSlot.max_block.load() == 512u);
    CHECK(inputSlot.channels.load() == 2u);
    CHECK(inputSlot.format.load() == kFormatVst3);
    CHECK(inputSlot.version.load() == packVersion(0, 9, 0));
    CHECK(input.writerToken() == 0u);

    const LoopBinding* inputBinding = input.bindings().current();
    REQUIRE(inputBinding != nullptr);
    CHECK(inputBinding->group == 3u);
    CHECK(inputBinding->role == Role::kInput);
    CHECK(inputBinding->myCtl == mine);
    CHECK(inputBinding->writerToken == 0u);
    CHECK(inputBinding->rv != nullptr);
    CHECK(inputBinding->ring != nullptr);
    const RingSegment& ring = *static_cast<const RingSegment*>(inputBinding->ring->base());
    CHECK(ring.header.writer_token.load() == 0u); // Input 不发布写权

    Rendezvous output(configFor(Role::kOutput, backend, naming.scope(), 12));
    output.setGroup(3, ClaimIntent::kManual, kT0);
    output.prepare(format, kT0);
    output.tick(kT0);
    CHECK(output.state() == RendezvousState::kActive);
    RoleSlot& outputSlot = roleSlot(rv, 3, Role::kOutput);
    CHECK(outputSlot.ctl.load() == output.myCtl());
    CHECK(ctlGen(output.myCtl()) == 2u);
    const u64 token = output.writerToken();
    REQUIRE(token != 0);
    CHECK(ring.header.writer_token.load() == token);
    CHECK(ring.header.epoch.load() == 1u);
    CHECK(ring.header.sample_rate.load() == 48000u);
    const LoopBinding* outputBinding = output.bindings().current();
    REQUIRE(outputBinding != nullptr);
    CHECK(outputBinding->role == Role::kOutput);
    CHECK(outputBinding->writerToken == token);
    CHECK(outputBinding->myCtl == output.myCtl());
}

TEST_CASE("nothing is claimed before prepare and group 0 means unassigned", "[ipc][rendezvous]")
{
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    CountingBackend backend;
    SegmentView rvView = openRv(backend, naming);
    RvSegment& rv = rvOf(rvView);
    backend.resetCounts();

    Rendezvous r(configFor(Role::kInput, backend, naming.scope()));
    r.setGroup(3, ClaimIntent::kManual, kT0);
    r.tick(kT0);
    CHECK(r.state() == RendezvousState::kWaitingPrepare);
    CHECK(roleSlot(rv, 3, Role::kInput).ctl.load() == 0u);
    CHECK(roleSlot(rv, 3, Role::kInput).pid.load() == 0u);

    r.setGroup(0, ClaimIntent::kManual, kT0);
    r.tick(kT0);
    CHECK(r.state() == RendezvousState::kUnassigned);
    CHECK(r.myCtl() == 0u);
}

TEST_CASE("a second instance in the same group conflicts and retries at the conflict cadence", "[ipc][rendezvous]")
{
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    CountingBackend backend;
    SegmentView rvView = openRv(backend, naming);
    RvSegment& rv = rvOf(rvView);
    const StreamFormat format{48000, 512, 2};

    Rendezvous first(configFor(Role::kOutput, backend, naming.scope(), 21));
    first.setGroup(4, ClaimIntent::kManual, kT0);
    first.prepare(format, kT0);
    first.tick(kT0);
    REQUIRE(first.state() == RendezvousState::kActive);
    const u64 firstCtl = roleSlot(rv, 4, Role::kOutput).ctl.load();

    Rendezvous second(configFor(Role::kOutput, backend, naming.scope(), 22));
    second.setGroup(4, ClaimIntent::kManual, kT0);
    second.prepare(format, kT0);
    second.tick(kT0);
    CHECK(second.state() == RendezvousState::kConflict);
    CHECK(roleSlot(rv, 4, Role::kOutput).ctl.load() == firstCtl); // 第一个保持自己的槽

    // 冲突重试每 kConflictRetryMs 一次：没到点不碰，到了点才去认领
    roleSlot(rv, 4, Role::kOutput).ctl.store(makeCtl(ctlGen(firstCtl), kCtlPhaseFree));
    second.tick(kT0 + 999);
    CHECK(second.state() == RendezvousState::kConflict);
    CHECK(ctlPhase(roleSlot(rv, 4, Role::kOutput).ctl.load()) == kCtlPhaseFree);
    second.tick(kT0 + 1000);
    CHECK(second.state() == RendezvousState::kActive);
    CHECK(roleSlot(rv, 4, Role::kOutput).ctl.load() == second.myCtl());
}

TEST_CASE("a takeover is noticed by the per-tick self-check and the old owner stops writing", "[ipc][rendezvous]")
{
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    CountingBackend backend;
    SegmentView rvView = openRv(backend, naming);
    RvSegment& rv = rvOf(rvView);
    const StreamFormat format{48000, 512, 2};

    Rendezvous owner(configFor(Role::kInput, backend, naming.scope()));
    owner.setGroup(3, ClaimIntent::kManual, kT0);
    owner.prepare(format, kT0);
    owner.tick(kT0);
    REQUIRE(owner.state() == RendezvousState::kActive);

    RoleSlot& slot = roleSlot(rv, 3, Role::kInput);
    const u64 heartbeat = slot.heartbeat_ms.load();
    const u64 pid = slot.pid.load();
    const u64 thief = makeCtl(ctlGen(owner.myCtl()) + 5u, kCtlPhaseActive);
    slot.ctl.store(thief); // 被别人接管

    owner.tick(kT0 + 250);
    CHECK(owner.state() == RendezvousState::kConflict);
    CHECK(owner.bindings().current() == nullptr);
    CHECK(slot.ctl.load() == thief);
    CHECK(slot.heartbeat_ms.load() == heartbeat); // 旧的属主不再写
    CHECK(slot.pid.load() == pid);

    owner.tick(kT0 + 500);
    CHECK(slot.ctl.load() == thief);
    CHECK(slot.heartbeat_ms.load() == heartbeat);
}

TEST_CASE("the heartbeat is written at most every 250 ms of synthetic time", "[ipc][rendezvous]")
{
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    CountingBackend backend;
    SegmentView rvView = openRv(backend, naming);
    RvSegment& rv = rvOf(rvView);
    const StreamFormat format{48000, 512, 2};

    Rendezvous r(configFor(Role::kInput, backend, naming.scope()));
    r.setGroup(5, ClaimIntent::kManual, kT0);
    r.prepare(format, kT0);
    r.tick(kT0);
    REQUIRE(r.state() == RendezvousState::kActive);
    RoleSlot& slot = roleSlot(rv, 5, Role::kInput);
    CHECK(slot.heartbeat_ms.load() == kT0);

    for (u64 dt = 40; dt < 250; dt += 40)
    {
        r.tick(kT0 + dt);
        CHECK(slot.heartbeat_ms.load() == kT0);
    }
    r.tick(kT0 + 250);
    CHECK(slot.heartbeat_ms.load() == kT0 + 250);
    r.tick(kT0 + 260);
    CHECK(slot.heartbeat_ms.load() == kT0 + 250);
}

TEST_CASE("dormant and active swap the phase without touching claim_ms", "[ipc][rendezvous]")
{
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    CountingBackend backend;
    SegmentView rvView = openRv(backend, naming);
    RvSegment& rv = rvOf(rvView);

    Rendezvous r(configFor(Role::kOutput, backend, naming.scope()));
    r.setGroup(6, ClaimIntent::kManual, kT0);
    r.prepare({48000, 512, 2}, kT0);
    r.tick(kT0);
    REQUIRE(r.state() == RendezvousState::kActive);
    RoleSlot& slot = roleSlot(rv, 6, Role::kOutput);
    const u64 claimMs = slot.claim_ms.load();

    r.releaseResources(kT0 + 100);
    CHECK(r.state() == RendezvousState::kDormant);
    CHECK(ctlPhase(slot.ctl.load()) == kCtlPhaseDormant);
    CHECK(ctlGen(slot.ctl.load()) == 3u);
    CHECK(slot.dormant_ms.load() == kT0 + 100);
    const LoopBinding* binding = r.bindings().current();
    REQUIRE(binding != nullptr);
    CHECK(binding->myCtl == r.myCtl());
    CHECK(binding->myCtl == slot.ctl.load());

    r.prepare({44100, 1024, 1}, kT0 + 200);
    CHECK(r.state() == RendezvousState::kActive);
    CHECK(ctlPhase(slot.ctl.load()) == kCtlPhaseActive);
    CHECK(ctlGen(slot.ctl.load()) == 4u);
    CHECK(slot.claim_ms.load() == claimMs); // 同一次认领：claim_ms 不动
    CHECK(slot.sample_rate.load() == 44100u);
    CHECK(slot.max_block.load() == 1024u);
    CHECK(slot.channels.load() == 1u);
    binding = r.bindings().current();
    REQUIRE(binding != nullptr);
    CHECK(binding->myCtl == r.myCtl());
    const RingSegment& ring = *static_cast<const RingSegment*>(binding->ring->base());
    CHECK(ring.header.epoch.load() == 2u); // Output 用新采样率重新发布写权
    CHECK(ring.header.sample_rate.load() == 44100u);
}

TEST_CASE("regrouping frees the old slot and retires its ring before dropping it", "[ipc][rendezvous]")
{
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    CountingBackend backend;
    SegmentView rvView = openRv(backend, naming);
    RvSegment& rv = rvOf(rvView);
    const StreamFormat format{48000, 512, 2};

    drainRetired();
    const std::size_t doneBefore = SegmentHandle::retiredDone();

    Rendezvous r(configFor(Role::kInput, backend, naming.scope()));
    r.setGroup(3, ClaimIntent::kManual, kT0);
    r.prepare(format, kT0);
    r.tick(kT0);
    REQUIRE(r.state() == RendezvousState::kActive);
    RoleSlot& oldSlot = roleSlot(rv, 3, Role::kInput);
    REQUIRE(ctlPhase(oldSlot.ctl.load()) == kCtlPhaseActive);

    r.setGroup(5, ClaimIntent::kManual, kT0 + 10);
    CHECK(ctlPhase(oldSlot.ctl.load()) == kCtlPhaseFree);
    CHECK(ctlGen(oldSlot.ctl.load()) == 3u); // 2 active -> 3 free
    r.tick(kT0 + 10);
    CHECK(r.state() == RendezvousState::kActive);
    CHECK(r.group() == 5u);
    CHECK(roleSlot(rv, 5, Role::kInput).ctl.load() == r.myCtl());
    const LoopBinding* binding = r.bindings().current();
    REQUIRE(binding != nullptr);
    CHECK(binding->group == 5u);
    CHECK(binding->ring != nullptr);

    // 旧组的环先退休：kRetireDelayMs 之前不会被 release
    r.tick(kT0 + 10 + HandleKeeper::kRetireDelayMs - 1);
    CHECK(SegmentHandle::retiredDone() == doneBefore);
    r.tick(kT0 + 10 + HandleKeeper::kRetireDelayMs); // 这里才第一次请求 release，宽限期从头计
    CHECK(SegmentHandle::retiredDone() == doneBefore);
    r.tick(kT0 + 10 + HandleKeeper::kRetireDelayMs + SegmentHandle::kReleaseGraceMs);
    CHECK(SegmentHandle::retiredDone() == doneBefore + 1);
}

TEST_CASE("the destructor frees its slot but never overwrites a thief's word", "[ipc][rendezvous]")
{
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    CountingBackend backend;
    SegmentView rvView = openRv(backend, naming);
    RvSegment& rv = rvOf(rvView);
    const StreamFormat format{48000, 512, 2};

    {
        Rendezvous r(configFor(Role::kInput, backend, naming.scope()));
        r.setGroup(7, ClaimIntent::kManual, kT0);
        r.prepare(format, kT0);
        r.tick(kT0);
        REQUIRE(r.state() == RendezvousState::kActive);
    }
    const u64 freed = roleSlot(rv, 7, Role::kInput).ctl.load();
    CHECK(ctlPhase(freed) == kCtlPhaseFree);
    CHECK(ctlGen(freed) == 3u);

    u64 thief = 0;
    {
        Rendezvous r(configFor(Role::kInput, backend, naming.scope(), 2));
        r.setGroup(7, ClaimIntent::kManual, kT0);
        r.prepare(format, kT0);
        r.tick(kT0);
        REQUIRE(r.state() == RendezvousState::kActive);
        thief = makeCtl(ctlGen(r.myCtl()) + 10u, kCtlPhaseActive);
        roleSlot(rv, 7, Role::kInput).ctl.store(thief);
    }
    CHECK(roleSlot(rv, 7, Role::kInput).ctl.load() == thief); // 释放的 CAS 输了就什么都不改
}

TEST_CASE("smart claims carry the provisional bit, only restore preempts them, confirm clears it", "[ipc][rendezvous]")
{
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    CountingBackend backend;
    SegmentView rvView = openRv(backend, naming);
    RvSegment& rv = rvOf(rvView);
    const StreamFormat format{48000, 512, 2};

    Rendezvous smart(configFor(Role::kInput, backend, naming.scope(), 31));
    smart.setGroup(2, ClaimIntent::kSmart, kT0);
    smart.prepare(format, kT0);
    smart.tick(kT0);
    REQUIRE(smart.state() == RendezvousState::kActive);
    CHECK((ctlState(smart.myCtl()) & kCtlProvisional) != 0);
    CHECK((roleSlot(rv, 2, Role::kInput).cfg_flags.load() & kCfgSmartOrigin) != 0);

    Rendezvous manual(configFor(Role::kInput, backend, naming.scope(), 32));
    manual.setGroup(2, ClaimIntent::kManual, kT0);
    manual.prepare(format, kT0);
    manual.tick(kT0);
    CHECK(manual.state() == RendezvousState::kConflict); // kManual 不抢占 provisional

    Rendezvous restore(configFor(Role::kInput, backend, naming.scope(), 33));
    restore.setGroup(2, ClaimIntent::kRestore, kT0);
    restore.prepare(format, kT0);
    restore.tick(kT0);
    CHECK(restore.state() == RendezvousState::kActive);
    CHECK((ctlState(restore.myCtl()) & kCtlProvisional) == 0);
    smart.tick(kT0 + 1);
    CHECK(smart.state() == RendezvousState::kConflict);
    CHECK(smart.bindings().current() == nullptr);

    Rendezvous confirmed(configFor(Role::kInput, backend, naming.scope(), 34));
    confirmed.setGroup(8, ClaimIntent::kSmart, kT0);
    confirmed.prepare(format, kT0);
    confirmed.tick(kT0);
    REQUIRE(confirmed.state() == RendezvousState::kActive);
    REQUIRE((ctlState(confirmed.myCtl()) & kCtlProvisional) != 0);
    const u64 beforeConfirm = confirmed.myCtl();
    confirmed.confirmClaim(kT0 + 1);
    CHECK((ctlState(confirmed.myCtl()) & kCtlProvisional) == 0);
    CHECK(ctlGen(confirmed.myCtl()) == ctlGen(beforeConfirm) + 1u);
    CHECK(roleSlot(rv, 8, Role::kInput).ctl.load() == confirmed.myCtl());
    const LoopBinding* binding = confirmed.bindings().current();
    REQUIRE(binding != nullptr);
    CHECK(binding->myCtl == confirmed.myCtl());
}

TEST_CASE("a validator host keeps IPC entirely closed", "[ipc][rendezvous]")
{
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    CountingBackend backend;
    RendezvousConfig config = configFor(Role::kInput, backend, naming.scope());
    config.ipcEnabled = false;
    backend.resetCounts();

    Rendezvous r(config);
    r.setGroup(3, ClaimIntent::kManual, kT0);
    r.prepare({48000, 512, 2}, kT0);
    r.tick(kT0);
    CHECK(r.state() == RendezvousState::kValidator);
    CHECK(backend.createOrOpenCalls == 0);
    CHECK(backend.unmapCalls == 0);
}

TEST_CASE("kBusy unmaps immediately, backs off inside the contract, and restarts at k = 0 after a success",
          "[ipc][rendezvous]")
{
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    CountingBackend backend;
    // 一个「初始化者还活着」的 rv：initHeader 会一直返回 kBusy
    SegmentView rvView = createView(backend, naming, SegmentId::rendezvous(), kRvBytes);
    RvSegment& rv = rvOf(rvView);
    rv.header.magic.store(kInitMarker);
    rv.header.init_pid.store(currentProcessId());
    backend.resetCounts();

    Rendezvous r(configFor(Role::kInput, backend, naming.scope()));
    r.setGroup(3, ClaimIntent::kManual, kT0);
    r.prepare({48000, 512, 2}, kT0);
    r.tick(kT0);
    CHECK(r.state() == RendezvousState::kOpening);
    CHECK(r.lastResult() == InitResult::kBusy);
    CHECK(backend.createOrOpenCalls == 1);
    CHECK(backend.unmapCalls == 1); // kBusy 立刻解映射
    CHECK(r.consecutiveFailures() == 1);

    u64 now = kT0;
    const u64 ceilings[] = {50, 100, 200, 250, 250};
    for (u32 attempt = 0; attempt < 5; ++attempt)
    {
        const std::size_t before = backend.createOrOpenCalls;
        u64 gap = 0;
        for (u64 dt = 0; dt <= RetryBackoff::kMaxDelayMs; ++dt)
        {
            r.tick(now + dt);
            if (backend.createOrOpenCalls != before)
            {
                gap = dt;
                now += dt;
                break;
            }
        }
        INFO("attempt " << attempt << " gap " << gap);
        CHECK(gap >= RetryBackoff::kMinDelayMs);
        CHECK(gap <= ceilings[attempt]);
    }
    CHECK(r.consecutiveFailures() == 6);

    // 把段头发布出来：下一次到点就附着、认领、打开数据段
    rv.header.abi.store(kIpcAbi);
    rv.header.generation.store(1);
    rv.header.layout_bytes.store(kRvBytes);
    rv.header.groups.store(kGroupCount);
    rv.header.group_stride.store(kGroupStride);
    rv.header.magic.store(kRvMagic, std::memory_order_release);
    const std::size_t beforeOpen = backend.createOrOpenCalls;
    for (u64 dt = 0; dt <= RetryBackoff::kMaxDelayMs; ++dt)
    {
        r.tick(now + dt);
        if (backend.createOrOpenCalls != beforeOpen)
        {
            now += dt;
            break;
        }
    }
    CHECK(r.state() == RendezvousState::kActive);
    CHECK(r.myCtl() != 0u);

    // 数据段上再来一次 kBusy：rv 成功打开时重置了退避，所以这次从 k = 0 开始（[25, 50]）
    SegmentView ringView = createView(backend, naming, SegmentId::groupData(4), kRingBytes);
    ringOf(ringView).header.magic.store(kInitMarker);
    ringOf(ringView).header.init_pid.store(currentProcessId());
    r.setGroup(4, ClaimIntent::kManual, now);
    r.tick(now);
    CHECK(r.state() == RendezvousState::kOpening);
    CHECK(r.lastResult() == InitResult::kBusy);
    const std::size_t beforeRing = backend.createOrOpenCalls;
    u64 ringGap = 0;
    for (u64 dt = 0; dt <= RetryBackoff::kMaxDelayMs; ++dt)
    {
        r.tick(now + dt);
        if (backend.createOrOpenCalls != beforeRing)
        {
            ringGap = dt;
            break;
        }
    }
    INFO("first ring retry gap " << ringGap);
    CHECK(ringGap >= RetryBackoff::kMinDelayMs);
    CHECK(ringGap <= 50);
}

TEST_CASE("an abi mismatch is reported, never written, and re-checked at the conflict cadence", "[ipc][rendezvous]")
{
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    CountingBackend backend;
    SegmentView rvView = createView(backend, naming, SegmentId::rendezvous(), kRvBytes);
    publishRv(backend, rvView, 2); // 对方的 abi 是 2
    storeSlotVersion(rvView, 3, /*output=*/true, 0x00010203u);
    const u32 version = loadSlotVersion(rvView, 3, true);
    std::vector<unsigned char> before(static_cast<unsigned char*>(rvView.base),
                                      static_cast<unsigned char*>(rvView.base) + kRvBytes);
    backend.resetCounts();

    Rendezvous r(configFor(Role::kInput, backend, naming.scope()));
    r.setGroup(3, ClaimIntent::kManual, kT0);
    r.prepare({48000, 512, 2}, kT0);
    r.tick(kT0);
    CHECK(r.state() == RendezvousState::kAbiMismatch);
    CHECK(r.abiMismatchVersion() == version);
    CHECK(r.lastResult() == InitResult::kAbiMismatch);
    CHECK(backend.unmapCalls == 1);
    CHECK(std::memcmp(before.data(), rvView.base, kRvBytes) == 0); // 一个字节都没写

    const std::size_t calls = backend.createOrOpenCalls;
    r.tick(kT0 + RendezvousTiming::kConflictRetryMs - 1);
    CHECK(backend.createOrOpenCalls == calls);
    r.tick(kT0 + RendezvousTiming::kConflictRetryMs);
    CHECK(backend.createOrOpenCalls == calls + 1);
}

TEST_CASE("a denied backend is permanent and never retried", "[ipc][rendezvous]")
{
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    CountingBackend backend;
    backend.forced = InitResult::kFailedDenied;
    backend.resetCounts();

    Rendezvous r(configFor(Role::kInput, backend, naming.scope()));
    r.setGroup(3, ClaimIntent::kManual, kT0);
    r.prepare({48000, 512, 2}, kT0);
    r.tick(kT0);
    CHECK(r.state() == RendezvousState::kIpcUnavailable);
    CHECK(r.lastResult() == InitResult::kFailedDenied);
    CHECK(backend.createOrOpenCalls == 1);
    r.tick(kT0 + 100000);
    CHECK(backend.createOrOpenCalls == 1);
    backend.forced = InitResult::kOk;
}

TEST_CASE("tick reaps retired mappings even when the instance is unassigned", "[ipc][rendezvous]")
{
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    CountingBackend backend;
    drainRetired();
    const std::size_t pendingBefore = SegmentHandle::retiredPending();
    {
        SegmentView view = createView(backend, naming, SegmentId::groupData(6), kRingBytes);
        SegmentHandle handle(std::move(view), &backend); // 在宽限期内析构：交给进程级回收
    }
    const u64 now = rinne::util::monoNowMs();
    CHECK(SegmentHandle::retiredPending() == pendingBefore + 1);

    Rendezvous r(configFor(Role::kInput, backend, naming.scope())); // 组号 0：不认领，只走回收
    r.tick(now);
    CHECK(SegmentHandle::retiredPending() == pendingBefore + 1);
    r.tick(now + SegmentHandle::kReleaseGraceMs);
    CHECK(SegmentHandle::retiredPending() == pendingBefore);
}

TEST_CASE("the last instance reaps the mappings still inside their grace period", "[ipc][rendezvous]")
{
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    CountingBackend backend;
    REQUIRE(Rendezvous::liveInstances() == 0);
    drainRetired();
    const std::size_t pendingBefore = SegmentHandle::retiredPending();

    {
        auto r1 = std::make_unique<Rendezvous>(configFor(Role::kInput, backend, naming.scope(), 1));
        auto r2 = std::make_unique<Rendezvous>(configFor(Role::kInput, backend, naming.scope(), 2));
        {
            SegmentView view = createView(backend, naming, SegmentId::groupData(7), kRingBytes);
            SegmentHandle handle(std::move(view), &backend);
        }
        CHECK(SegmentHandle::retiredPending() == pendingBefore + 1);
        r1.reset();
        CHECK(Rendezvous::liveInstances() == 1);
        CHECK(SegmentHandle::retiredPending() == pendingBefore + 1); // 还有实例：先不收
        r2.reset();
        CHECK(Rendezvous::liveInstances() == 0);
        CHECK(SegmentHandle::retiredPending() == pendingBefore); // 最后一个实例收掉
    }

    {
        auto r = std::make_unique<Rendezvous>(configFor(Role::kInput, backend, naming.scope(), 3));
        {
            SegmentView view = createView(backend, naming, SegmentId::groupData(8), kRingBytes);
            SegmentHandle handle(std::move(view), &backend);
        }
        CHECK(SegmentHandle::retiredPending() == pendingBefore + 1);
        r.reset();
        CHECK(SegmentHandle::retiredPending() == pendingBefore);
    }
}

TEST_CASE("the owner writes its label, repairs a dead writer's odd sequence, and restores a foreign payload",
          "[ipc][rendezvous]")
{
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    CountingBackend backend;
    SegmentView rvView = openRv(backend, naming);
    RvSegment& rv = rvOf(rvView);
    const StreamFormat format{48000, 512, 2};

    {
        Rendezvous r(configFor(Role::kInput, backend, naming.scope(), 41));
        CHECK_FALSE(r.setLabel("Vox", 0xff112233u)); // 还没有槽
        r.setGroup(5, ClaimIntent::kManual, kT0);
        r.prepare(format, kT0);
        r.tick(kT0);
        REQUIRE(r.state() == RendezvousState::kActive);
        r.tick(kT0); // ownerTick 把脏标签写下去
        LabelPayload got;
        REQUIRE(readLabel(roleSlot(rv, 5, Role::kInput), got));
        CHECK(got == makeLabelPayload("Vox", 0xff112233u));
    }

    {
        // 接管之后旧属主再也不写标签
        Rendezvous owner(configFor(Role::kInput, backend, naming.scope(), 42));
        owner.setLabel("Smart", 1);
        owner.setGroup(6, ClaimIntent::kSmart, kT0);
        owner.prepare(format, kT0);
        owner.tick(kT0);
        owner.tick(kT0);
        RoleSlot& slot = roleSlot(rv, 6, Role::kInput);
        LabelPayload got;
        REQUIRE(readLabel(slot, got));
        CHECK(got == makeLabelPayload("Smart", 1));

        Rendezvous thief(configFor(Role::kInput, backend, naming.scope(), 43));
        thief.setLabel("Thief", 2);
        thief.setGroup(6, ClaimIntent::kRestore, kT0);
        thief.prepare(format, kT0);
        thief.tick(kT0);
        thief.tick(kT0);
        REQUIRE(readLabel(slot, got));
        CHECK(got == makeLabelPayload("Thief", 2));

        owner.tick(kT0 + 250);
        CHECK(owner.state() == RendezvousState::kConflict);
        CHECK_FALSE(owner.setLabel("Old", 3));
        owner.tick(kT0 + 500);
        owner.tick(kT0 + 750);
        REQUIRE(readLabel(slot, got));
        CHECK(got == makeLabelPayload("Thief", 2));
    }

    {
        // 死在「写了一半」的旧属主：同一个奇数在心跳上连续停够两个心跳，才由我们接手写上自己的标签
        Rendezvous r(configFor(Role::kInput, backend, naming.scope(), 44));
        r.setLabel("Repair", 7);
        r.setGroup(8, ClaimIntent::kManual, kT0);
        r.prepare(format, kT0);
        r.tick(kT0);
        r.tick(kT0);
        RoleSlot& slot = roleSlot(rv, 8, Role::kInput);
        LabelPayload got;
        REQUIRE(readLabel(slot, got));
        CHECK(got == makeLabelPayload("Repair", 7));

        const u32 stuck = slot.label_seq.load() + 1u; // 停在奇数上，字段是半份
        slot.label_seq.store(stuck);
        slot.colour_argb.store(0xbadu);
        r.tick(kT0 + 250); // 心跳拍：读失败，记下这个奇数与起点
        r.tick(kT0 + 300); // 不是心跳拍：不计
        r.tick(kT0 + 500); // 250 ms：还不到两个心跳
        CHECK(slot.label_seq.load() == stuck);
        r.tick(kT0 + 750); // 500 ms：接手，序号 stuck → stuck + 2（写）→ stuck + 3，读方随即拿到我们的标签
        CHECK(slot.label_seq.load() == stuck + 3u);
        REQUIRE(readLabel(slot, got));
        CHECK(got == makeLabelPayload("Repair", 7));

        // 奇数在心跳之间变了（写方还活着、在动）：从新的奇数重新计，不会提前接手
        slot.label_seq.store(stuck + 4u);
        r.tick(kT0 + 1000); // 记下 stuck + 4
        slot.label_seq.store(stuck + 6u);
        r.tick(kT0 + 1250); // 换成了 stuck + 6：重新计
        r.tick(kT0 + 1500); // 只停了 250 ms
        CHECK(slot.label_seq.load() == stuck + 6u);
        r.tick(kT0 + 1750); // 停够 500 ms：接手
        CHECK(slot.label_seq.load() == stuck + 9u);
        REQUIRE(readLabel(slot, got));
        CHECK(got == makeLabelPayload("Repair", 7));

        // 字段被别的写方覆盖成另一个稳定的负载：下一个心跳读回来发现不一致，当场重写
        REQUIRE(writeLabel(slot, makeLabelPayload("Foreign", 9)));
        r.tick(kT0 + 1800); // 不是心跳拍：不读回
        REQUIRE(readLabel(slot, got));
        CHECK(got == makeLabelPayload("Foreign", 9));
        r.tick(kT0 + 2000);
        REQUIRE(readLabel(slot, got));
        CHECK(got == makeLabelPayload("Repair", 7));
    }
}

TEST_CASE("peerLost through the instance follows the observer and the export flag", "[ipc][rendezvous]")
{
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    CountingBackend backend;
    SegmentView rvView = openRv(backend, naming);
    RvSegment& rv = rvOf(rvView);
    const StreamFormat format{48000, 512, 2};

    Rendezvous input(configFor(Role::kInput, backend, naming.scope(), 51));
    input.setGroup(2, ClaimIntent::kManual, kT0);
    input.prepare(format, kT0);
    input.tick(kT0);
    Rendezvous output(configFor(Role::kOutput, backend, naming.scope(), 52));
    output.setGroup(2, ClaimIntent::kManual, kT0);
    output.prepare(format, kT0);
    output.tick(kT0);
    REQUIRE(input.state() == RendezvousState::kActive);
    REQUIRE(output.state() == RendezvousState::kActive);

    // 不再 tick Output：心跳停在 kT0，block_seq 也不动
    input.tick(kT0 + 2000);
    CHECK_FALSE(input.peerLost(kT0 + 2000));
    CHECK(input.peerLost(kT0 + 2001));
    input.setNonrealtime(true);
    CHECK_FALSE(input.peerLost(kT0 + 2001));
    input.setNonrealtime(false);

    // block_seq 动了：对端还活着，只是心跳旧
    roleSlot(rv, 2, Role::kOutput).block_seq.store(9);
    input.tick(kT0 + 2250);
    CHECK_FALSE(input.peerLost(kT0 + 2500));
}
