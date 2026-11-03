// SPDX-License-Identifier: GPL-3.0-or-later
//
// 环映射的代号（LoopBinding::ringGeneration）与转正的那个整字（LoopBinding::confirmedCtl）：同一个数据段句柄上因 ctl
// 变化重新发布绑定（转正、休眠与唤醒）时代号不变、发布序号照常变；接管一个新的句柄（放开再认领同一组、换组）时代号
// 才变，哪怕新映射落在旧地址上。转正的 CAS 到重新发布之间，拿着旧快照的那一块仍认得出槽是自己的。时间全部是合成的。
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <initializer_list>

#include "RendezvousTestKit.h"
#include "ipc/LoopBinding.h"
#include "ipc/Rendezvous.h"

namespace
{

using namespace rinne::test;

constexpr u64 kT0 = 1000000;
const StreamFormat kFormat{48000, 512, 2};

bool provisional(const Rendezvous& r)
{
    return r.myCtl() != 0 && (ctlState(r.myCtl()) & kCtlProvisional) != 0;
}

} // namespace

TEST_CASE("the ring generation stays put while the same ring is republished and moves with a new handle",
          "[ipc][rendezvous][ring_generation]")
{
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    CountingBackend backend;
    SegmentView rvView = openRv(backend, naming);
    RvSegment& rv = rvOf(rvView);

    for (const Role role : {Role::kInput, Role::kOutput})
    {
        DYNAMIC_SECTION((role == Role::kInput ? "input" : "output"))
        {
            Rendezvous r(configFor(role, backend, naming.scope(), 41));
            r.setGroup(2, ClaimIntent::kSmart, kT0);
            r.prepare(kFormat, kT0);
            r.tick(kT0);
            REQUIRE(r.state() == RendezvousState::kActive);
            REQUIRE(provisional(r));
            const LoopBinding* first = r.bindings().current();
            REQUIRE(first != nullptr);
            REQUIRE(first->ring != nullptr);
            REQUIRE(roleSlot(rv, 2, role).ctl.load() == first->myCtl);
            const u64 generation = first->ringGeneration;
            CHECK(generation != 0u);
            // 带 provisional 的快照写明转正会写下的整字：gen + 1、活跃、不带 provisional
            CHECK(first->confirmedCtl == nextCtl(first->myCtl, kCtlPhaseActive));
            const u64 token = first->writerToken;
            CHECK((role == Role::kOutput) == (token != 0u));

            SECTION("confirmClaim: a new snapshot, the same ring and generation")
            {
                r.confirmClaim(kT0 + 1);
                REQUIRE_FALSE(provisional(r));
                const LoopBinding* after = r.bindings().current();
                REQUIRE(after != nullptr);
                CHECK(after != first);
                CHECK(after->seq > first->seq); // 确实重新发布了
                CHECK(after->myCtl == r.myCtl());
                CHECK(after->myCtl == first->confirmedCtl); // 转正写下的正是旧快照写明的那个整字
                CHECK(after->confirmedCtl == 0u); // 已经转正：没有下一个算自己的整字
                CHECK(after->ring == first->ring);
                // ⛔ 每次发布都给新代号 → 这里变红
                CHECK(after->ringGeneration == generation);
                CHECK(after->writerToken == token);
            }
            SECTION("release then prepare on the same ring keeps the generation")
            {
                r.releaseResources(kT0 + 1);
                REQUIRE(r.state() == RendezvousState::kDormant);
                const LoopBinding* dormant = r.bindings().current();
                REQUIRE(dormant != nullptr);
                CHECK(dormant->seq > first->seq);
                CHECK(dormant->ring == first->ring);
                CHECK(dormant->ringGeneration == generation);
                CHECK(dormant->confirmedCtl == 0u); // 休眠的整字不是活跃 + provisional

                r.prepare(kFormat, kT0 + 2);
                REQUIRE(r.state() == RendezvousState::kActive);
                REQUIRE(provisional(r)); // provisional 位跟着认领走，不因休眠而丢
                const LoopBinding* woken = r.bindings().current();
                REQUIRE(woken != nullptr);
                CHECK(woken->seq > dormant->seq);
                CHECK(woken->ring == first->ring);
                CHECK(woken->ringGeneration == generation);
                CHECK(woken->confirmedCtl == nextCtl(woken->myCtl, kCtlPhaseActive));
                CHECK(woken->writerToken == token);
            }
            SECTION("letting go and claiming the same group again opens a new handle with a new generation")
            {
                r.setGroup(0, ClaimIntent::kManual, kT0 + 1);
                CHECK(r.bindings().current() == nullptr);
                r.setGroup(2, ClaimIntent::kManual, kT0 + 1);
                r.tick(kT0 + 2);
                REQUIRE(r.state() == RendezvousState::kActive);
                const LoopBinding* again = r.bindings().current();
                REQUIRE(again != nullptr);
                // 同一块段的新映射：进程内后端里它可能就落在旧地址上，代号照样不同
                CHECK(again->ring != first->ring);
                CHECK(again->ringGeneration > generation);
                CHECK(again->confirmedCtl == 0u); // 手选的认领不带 provisional

                // 换组：又一个新句柄，代号再变
                r.setGroup(3, ClaimIntent::kManual, kT0 + 3);
                r.tick(kT0 + 4);
                REQUIRE(r.state() == RendezvousState::kActive);
                const LoopBinding* moved = r.bindings().current();
                REQUIRE(moved != nullptr);
                CHECK(moved->group == 3u);
                CHECK(moved->ringGeneration > again->ringGeneration);
            }
        }
    }
}

TEST_CASE("a block that took the binding before confirmClaim still owns the slot after the CAS",
          "[ipc][rendezvous][ring_generation]")
{
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    CountingBackend backend;
    SegmentView rvView = openRv(backend, naming);
    RvSegment& rv = rvOf(rvView);

    Rendezvous r(configFor(Role::kInput, backend, naming.scope(), 42));
    r.setGroup(4, ClaimIntent::kSmart, kT0);
    r.prepare(kFormat, kT0);
    r.tick(kT0);
    REQUIRE(r.state() == RendezvousState::kActive);
    REQUIRE(provisional(r));
    // 音频线程在块首拿到的那份快照；之后消息线程转正（CAS 落地、再重新发布）
    const LoopBinding* before = r.bindings().current();
    REQUIRE(before != nullptr);
    const u64 provisionalWord = r.myCtl();

    SECTION("the owner's own confirm")
    {
        r.confirmClaim(kT0 + 1);
        REQUIRE(roleSlot(rv, 4, Role::kInput).ctl.load() == r.myCtl());
        REQUIRE(r.bindings().current() != before);
        // 块首读了旧快照、读 ctl 时转正已经落地：这一块仍是属主
        // ⛔ stillMine 只认 myCtl，或会合不给 confirmedCtl → 这里变红（那一块引擎拿到空段，回送断开再重新配对）
        const BoundBlock late(before);
        REQUIRE(late.ok());
        CHECK(late.stillMine());
        const BoundBlock fresh(r.bindings().current());
        REQUIRE(fresh.ok());
        CHECK(fresh.stillMine());

        // 转正之后有人接管：两份快照都不认
        roleSlot(rv, 4, Role::kInput).ctl.store(nextCtl(r.myCtl(), kCtlPhaseClaiming));
        CHECK_FALSE(late.stillMine());
        CHECK_FALSE(fresh.stillMine());
    }
    SECTION("a restore preempting the provisional claim is not mistaken for the confirm")
    {
        // 工程恢复的认领方从同一个整字出发 CAS 到认领中，再到活跃
        const u64 claiming = nextCtl(provisionalWord, kCtlPhaseClaiming);
        roleSlot(rv, 4, Role::kInput).ctl.store(claiming);
        const BoundBlock late(before);
        REQUIRE(late.ok());
        CHECK_FALSE(late.stillMine());
        roleSlot(rv, 4, Role::kInput).ctl.store(nextCtl(claiming, kCtlPhaseActive));
        CHECK_FALSE(late.stillMine());
        // 属主自己丢槽之后的转正失败：撤下绑定，什么都不发布
        r.confirmClaim(kT0 + 1);
        CHECK(r.bindings().current() == nullptr);
        CHECK(r.state() == RendezvousState::kConflict);
    }
}
