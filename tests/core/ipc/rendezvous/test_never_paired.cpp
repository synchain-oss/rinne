// SPDX-License-Identifier: GPL-3.0-or-later
//
// 槽里 cfg_flags 的 kCfgNeverPaired（认领以来还没配上过对端）：认领时置位；属主在扫描里第一次看到同组对端
// 活跃且心跳新鲜时清掉，之后对端走了也不再置回；只有新的一次认领（换组、接管、被抢占后重新认领）才重新置位，
// 休眠与唤醒不是新认领。两端各管自己槽里的位。时间全部是合成的，每个用例都按 250 ms 一拍（扫描间隔）推进。
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <initializer_list>

#include "RendezvousTestKit.h"
#include "ipc/Rendezvous.h"

namespace
{

using namespace rinne::test;

constexpr u64 kT0 = 3000000;
constexpr u64 kStep = SlotObserver::kScanIntervalMs;
const StreamFormat kFormat{48000, 512, 2};

bool neverPaired(const RvSegment& rv, u32 group, Role role)
{
    return (roleSlot(rv, group, role).cfg_flags.load() & kCfgNeverPaired) != 0;
}

void claimAt(Rendezvous& r, u32 group, ClaimIntent intent, u64 t)
{
    r.setGroup(group, intent, t);
    r.prepare(kFormat, t);
    r.tick(t);
}

// 从 from 起每 kStep 一拍，到 to（含）为止，每一拍按给定顺序各 tick 一次
void runTicks(std::initializer_list<Rendezvous*> instances, u64 from, u64 to)
{
    for (u64 t = from; t <= to; t += kStep)
    {
        for (Rendezvous* r : instances)
        {
            r->tick(t);
        }
    }
}

} // namespace

TEST_CASE("a claim starts with the never-paired bit set and keeps it while no peer shows up",
          "[ipc][rendezvous][never_paired]")
{
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    CountingBackend backend;
    SegmentView rvView = openRv(backend, naming);
    const RvSegment& rv = rvOf(rvView);

    for (const Role role : {Role::kInput, Role::kOutput})
    {
        Rendezvous alone(configFor(role, backend, naming.scope(), 51));
        const u32 group = role == Role::kInput ? 2u : 5u;
        claimAt(alone, group, ClaimIntent::kManual, kT0);
        REQUIRE(alone.state() == RendezvousState::kActive);
        CHECK(roleSlot(rv, group, role).cfg_flags.load() == kCfgNeverPaired);
        // 许多个心跳与扫描之后仍是它：没有对端就没有「配上」
        runTicks({&alone}, kT0 + kStep, kT0 + 12 * kStep);
        CHECK(roleSlot(rv, group, role).cfg_flags.load() == kCfgNeverPaired);
    }
}

TEST_CASE("the first scan that sees a live peer clears the bit and a peer leaving never sets it back",
          "[ipc][rendezvous][never_paired]")
{
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    CountingBackend backend;
    SegmentView rvView = openRv(backend, naming);
    const RvSegment& rv = rvOf(rvView);

    Rendezvous output(configFor(Role::kOutput, backend, naming.scope(), 52));
    Rendezvous input(configFor(Role::kInput, backend, naming.scope(), 53));
    claimAt(output, 4, ClaimIntent::kManual, kT0);
    claimAt(input, 4, ClaimIntent::kManual, kT0 + 10);
    REQUIRE(output.state() == RendezvousState::kActive);
    REQUIRE(input.state() == RendezvousState::kActive);
    CHECK(neverPaired(rv, 4, Role::kOutput));
    CHECK(neverPaired(rv, 4, Role::kInput));

    // 两端的下一次扫描都看到对端活跃、心跳新鲜：各清各的位，对方槽里的位不是自己写的
    output.tick(kT0 + kStep + 10);
    CHECK_FALSE(neverPaired(rv, 4, Role::kOutput));
    CHECK(neverPaired(rv, 4, Role::kInput));
    input.tick(kT0 + kStep + 10);
    CHECK_FALSE(neverPaired(rv, 4, Role::kInput));
    // 清掉的是这一位，别的位不动
    CHECK(roleSlot(rv, 4, Role::kOutput).cfg_flags.load() == 0u);

    // 配着的时候心跳照常重写身份块：位不会被写回去
    runTicks({&output, &input}, kT0 + 2 * kStep, kT0 + 6 * kStep);
    CHECK_FALSE(neverPaired(rv, 4, Role::kOutput));
    CHECK_FALSE(neverPaired(rv, 4, Role::kInput));

    // Input 走了：Output 的槽里仍是 0，哪怕过了很久
    input.setGroup(0, ClaimIntent::kManual, kT0 + 7 * kStep);
    REQUIRE(ctlPhase(roleSlot(rv, 4, Role::kInput).ctl.load()) == kCtlPhaseFree);
    output.tick(kT0 + 8 * kStep);
    CHECK_FALSE(neverPaired(rv, 4, Role::kOutput));
    runTicks({&output}, kT0 + 9 * kStep, kT0 + 30 * kStep);
    CHECK_FALSE(neverPaired(rv, 4, Role::kOutput));

    // 之后又来了一个新的 Input：位还是 0（不是新认领），新来的那一端自己从 1 开始
    Rendezvous second(configFor(Role::kInput, backend, naming.scope(), 54));
    claimAt(second, 4, ClaimIntent::kManual, kT0 + 31 * kStep);
    CHECK(neverPaired(rv, 4, Role::kInput));
    runTicks({&output, &second}, kT0 + 32 * kStep, kT0 + 34 * kStep);
    CHECK_FALSE(neverPaired(rv, 4, Role::kOutput));
    CHECK_FALSE(neverPaired(rv, 4, Role::kInput));
}

TEST_CASE("a claim beside an already live peer still starts set and clears on the next scan",
          "[ipc][rendezvous][never_paired]")
{
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    CountingBackend backend;
    SegmentView rvView = openRv(backend, naming);
    const RvSegment& rv = rvOf(rvView);

    // 对端早就在了、本端后来认领：认领当时位是 1，不在认领那一拍清（那一拍用的是认领之前的扫描样本）
    for (const Role late : {Role::kInput, Role::kOutput})
    {
        const Role early = late == Role::kInput ? Role::kOutput : Role::kInput;
        const u32 group = late == Role::kInput ? 3u : 6u;
        Rendezvous first(configFor(early, backend, naming.scope(), 55));
        Rendezvous second(configFor(late, backend, naming.scope(), 56));
        claimAt(first, group, ClaimIntent::kManual, kT0);
        runTicks({&first}, kT0 + kStep, kT0 + 3 * kStep);
        claimAt(second, group, ClaimIntent::kManual, kT0 + 3 * kStep + 20);
        REQUIRE(second.state() == RendezvousState::kActive);
        CHECK(neverPaired(rv, group, late));
        // 下一次扫描所在的一拍清掉
        second.tick(kT0 + 4 * kStep + 20);
        CHECK_FALSE(neverPaired(rv, group, late));
        first.tick(kT0 + 4 * kStep + 20);
        CHECK_FALSE(neverPaired(rv, group, early));
    }
}

TEST_CASE("regrouping claims anew and sets the bit, updating the intent in the same group does not",
          "[ipc][rendezvous][never_paired]")
{
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    CountingBackend backend;
    SegmentView rvView = openRv(backend, naming);
    const RvSegment& rv = rvOf(rvView);

    Rendezvous output(configFor(Role::kOutput, backend, naming.scope(), 57));
    Rendezvous input(configFor(Role::kInput, backend, naming.scope(), 58));
    claimAt(output, 4, ClaimIntent::kManual, kT0);
    claimAt(input, 4, ClaimIntent::kManual, kT0 + 10);
    runTicks({&output, &input}, kT0 + kStep + 10, kT0 + 3 * kStep + 10);
    REQUIRE_FALSE(neverPaired(rv, 4, Role::kOutput));

    // 同组更新意图：还是同一次认领
    output.setGroup(4, ClaimIntent::kRestore, kT0 + 4 * kStep);
    output.tick(kT0 + 4 * kStep);
    CHECK_FALSE(neverPaired(rv, 4, Role::kOutput));

    // 换组：旧槽放开，新槽是新的一次认领，位从 1 开始（新组里没有对端）
    output.setGroup(6, ClaimIntent::kManual, kT0 + 5 * kStep);
    output.tick(kT0 + 5 * kStep);
    REQUIRE(output.state() == RendezvousState::kActive);
    REQUIRE(output.group() == 6u);
    CHECK(ctlPhase(roleSlot(rv, 4, Role::kOutput).ctl.load()) == kCtlPhaseFree);
    CHECK(neverPaired(rv, 6, Role::kOutput));
    runTicks({&output}, kT0 + 6 * kStep, kT0 + 9 * kStep);
    CHECK(neverPaired(rv, 6, Role::kOutput));
}

TEST_CASE("dormancy and waking are not a new claim and keep the bit as it was", "[ipc][rendezvous][never_paired]")
{
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    CountingBackend backend;
    SegmentView rvView = openRv(backend, naming);
    const RvSegment& rv = rvOf(rvView);

    SECTION("a bit that is still set stays set")
    {
        Rendezvous output(configFor(Role::kOutput, backend, naming.scope(), 59));
        claimAt(output, 2, ClaimIntent::kManual, kT0);
        output.releaseResources(kT0 + kStep);
        REQUIRE(output.state() == RendezvousState::kDormant);
        CHECK(neverPaired(rv, 2, Role::kOutput));
        runTicks({&output}, kT0 + 2 * kStep, kT0 + 4 * kStep);
        output.prepare(kFormat, kT0 + 5 * kStep);
        runTicks({&output}, kT0 + 5 * kStep, kT0 + 9 * kStep);
        REQUIRE(output.state() == RendezvousState::kActive);
        CHECK(roleSlot(rv, 2, Role::kOutput).cfg_flags.load() == kCfgNeverPaired);
    }

    SECTION("a bit that was cleared stays cleared, even after the peer left in the meantime")
    {
        Rendezvous output(configFor(Role::kOutput, backend, naming.scope(), 60));
        Rendezvous input(configFor(Role::kInput, backend, naming.scope(), 61));
        claimAt(output, 2, ClaimIntent::kManual, kT0);
        claimAt(input, 2, ClaimIntent::kManual, kT0 + 10);
        runTicks({&output, &input}, kT0 + kStep + 10, kT0 + 3 * kStep + 10);
        REQUIRE_FALSE(neverPaired(rv, 2, Role::kOutput));

        output.releaseResources(kT0 + 4 * kStep);
        REQUIRE(output.state() == RendezvousState::kDormant);
        input.setGroup(0, ClaimIntent::kManual, kT0 + 4 * kStep);
        runTicks({&output}, kT0 + 5 * kStep, kT0 + 8 * kStep);
        output.prepare(kFormat, kT0 + 9 * kStep);
        runTicks({&output}, kT0 + 9 * kStep, kT0 + 12 * kStep);
        REQUIRE(output.state() == RendezvousState::kActive);
        CHECK(roleSlot(rv, 2, Role::kOutput).cfg_flags.load() == 0u);
    }

    SECTION("a dormant owner does not clear it, it clears on the first scan after waking")
    {
        Rendezvous output(configFor(Role::kOutput, backend, naming.scope(), 62));
        Rendezvous input(configFor(Role::kInput, backend, naming.scope(), 63));
        claimAt(output, 2, ClaimIntent::kManual, kT0);
        claimAt(input, 2, ClaimIntent::kManual, kT0 + 10);
        output.releaseResources(kT0 + 20); // 还没扫到对端就休眠了
        REQUIRE(output.state() == RendezvousState::kDormant);
        // 两端一直在跑，对端心跳新鲜，但本端休眠着，没有在环里；对端看到的是休眠的 Output，也不算配上
        runTicks({&output, &input}, kT0 + kStep + 10, kT0 + 6 * kStep + 10);
        CHECK(neverPaired(rv, 2, Role::kOutput));
        CHECK(neverPaired(rv, 2, Role::kInput));

        output.prepare(kFormat, kT0 + 7 * kStep);
        REQUIRE(output.state() == RendezvousState::kActive);
        CHECK(neverPaired(rv, 2, Role::kOutput)); // 醒来本身不清
        runTicks({&output, &input}, kT0 + 7 * kStep + 10, kT0 + 8 * kStep + 10);
        CHECK_FALSE(neverPaired(rv, 2, Role::kOutput));
        CHECK_FALSE(neverPaired(rv, 2, Role::kInput));
    }
}

TEST_CASE("a takeover starts the new owner's bit at one and the old owner never writes it again",
          "[ipc][rendezvous][never_paired]")
{
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    CountingBackend backend;
    SegmentView rvView = openRv(backend, naming);
    RvSegment& rv = rvOf(rvView);

    SECTION("a restore preempting a smart claim that had paired")
    {
        Rendezvous output(configFor(Role::kOutput, backend, naming.scope(), 64));
        Rendezvous smart(configFor(Role::kInput, backend, naming.scope(), 65));
        claimAt(output, 2, ClaimIntent::kManual, kT0);
        claimAt(smart, 2, ClaimIntent::kSmart, kT0 + 10);
        runTicks({&output, &smart}, kT0 + kStep + 10, kT0 + 3 * kStep + 10);
        REQUIRE_FALSE(neverPaired(rv, 2, Role::kInput));
        CHECK((roleSlot(rv, 2, Role::kInput).cfg_flags.load() & kCfgSmartOrigin) != 0);

        Rendezvous restore(configFor(Role::kInput, backend, naming.scope(), 66));
        claimAt(restore, 2, ClaimIntent::kRestore, kT0 + 4 * kStep);
        REQUIRE(restore.state() == RendezvousState::kActive);
        CHECK(roleSlot(rv, 2, Role::kInput).cfg_flags.load() == kCfgNeverPaired);

        // 被抢占的一端发现槽换了主人：什么都不再写
        smart.tick(kT0 + 4 * kStep + 20);
        CHECK(smart.state() == RendezvousState::kConflict);
        CHECK(roleSlot(rv, 2, Role::kInput).cfg_flags.load() == kCfgNeverPaired);
        // 新属主自己看到对端之后再清
        runTicks({&restore, &output}, kT0 + 5 * kStep, kT0 + 6 * kStep);
        CHECK_FALSE(neverPaired(rv, 2, Role::kInput));
    }

    SECTION("a takeover of a dead process's slot that had paired")
    {
        RoleSlot& slot = roleSlot(rv, 4, Role::kOutput);
        const u64 old = makeCtl(5, kCtlPhaseActive);
        slot.ctl.store(old);
        slot.pid.store(kDeadPid);
        slot.heartbeat_ms.store(kT0 - 6000);
        slot.claim_ms.store(kT0 - 60000);
        slot.block_seq.store(7);
        slot.cfg_flags.store(0); // 它配上过

        Rendezvous r(configFor(Role::kOutput, backend, naming.scope(), 67));
        r.setGroup(4, ClaimIntent::kConflictRetry, kT0);
        r.prepare(kFormat, kT0);
        r.tick(kT0);
        REQUIRE(r.state() == RendezvousState::kConflict);
        r.setGroup(4, ClaimIntent::kManual, kT0 + RendezvousTiming::kDeadMs);
        r.tick(kT0 + RendezvousTiming::kDeadMs);
        REQUIRE(r.state() == RendezvousState::kActive);
        CHECK(slot.cfg_flags.load() == kCfgNeverPaired);
    }
}

TEST_CASE("a stale or dormant peer does not count as paired", "[ipc][rendezvous][never_paired]")
{
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    CountingBackend backend;
    SegmentView rvView = openRv(backend, naming);
    const RvSegment& rv = rvOf(rvView);

    SECTION("an active peer whose heartbeat went stale")
    {
        Rendezvous input(configFor(Role::kInput, backend, naming.scope(), 68));
        claimAt(input, 7, ClaimIntent::kManual, kT0); // 之后不再 tick：心跳停在 kT0
        Rendezvous output(configFor(Role::kOutput, backend, naming.scope(), 69));
        const u64 late = kT0 + RendezvousTiming::kLostHeartbeatMs + kStep;
        claimAt(output, 7, ClaimIntent::kManual, late);
        runTicks({&output}, late + kStep, late + 6 * kStep);
        CHECK(neverPaired(rv, 7, Role::kOutput));

        // 对端的心跳恢复之后，下一次扫描就认
        input.tick(late + 7 * kStep);
        output.tick(late + 8 * kStep);
        CHECK_FALSE(neverPaired(rv, 7, Role::kOutput));
    }

    SECTION("a dormant peer")
    {
        Rendezvous input(configFor(Role::kInput, backend, naming.scope(), 70));
        claimAt(input, 7, ClaimIntent::kManual, kT0);
        input.releaseResources(kT0 + 10);
        REQUIRE(input.state() == RendezvousState::kDormant);
        Rendezvous output(configFor(Role::kOutput, backend, naming.scope(), 71));
        claimAt(output, 7, ClaimIntent::kManual, kT0 + 20);
        runTicks({&output, &input}, kT0 + kStep + 20, kT0 + 6 * kStep + 20);
        CHECK(neverPaired(rv, 7, Role::kOutput));

        input.prepare(kFormat, kT0 + 7 * kStep);
        runTicks({&input, &output}, kT0 + 7 * kStep + 20, kT0 + 8 * kStep + 20);
        CHECK_FALSE(neverPaired(rv, 7, Role::kOutput));
    }
}
