// SPDX-License-Identifier: GPL-3.0-or-later
//
// 接管与选举的跨进程版本：
//   杀掉一个 active 的 Output peer，本进程的同组 Output 必须等满死进程的观察窗口（>= 5 s）才能接管：
//   delay 落在 [4.75 s, 7.5 s]，接管之后槽里的 pid 是我们、ctl 的 gen 前进、环的 writer_token 换成我们的。
//   同时起两个 Output peer：真人竞争同一槽，恰好一个 active、另一个 conflict（活着的对端永不被自动接管）。
// 所有等待都有超时；失败信息里贴 peer 的原话。
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "IpcTestKit.h"
#include "ipc/LoopRing.h"
#include "ipc/Rendezvous.h"
#include "util/MonoClock.h"

namespace
{

using namespace rinne::test;

RendezvousConfig configFor(Role role, PeerRig& rig)
{
    RendezvousConfig config;
    config.role = role;
    config.backend = &rig.backend();
    config.scope = rig.scope();
    config.ipcEnabled = true;
    config.version = packVersion(0, 9, 0);
    config.format = kFormatVst3;
    config.hostPpid = 0;
    config.seed = 0x5151;
    return config;
}

} // namespace

TEST_CASE("a killed output peer is taken over only after the dead-process window", "[ipc][xproc]")
{
    if (!peerUsable())
    {
        SKIP(peerSkipReason());
    }
    PeerRig rig;
    auto peer = rig.spawn({"--role", "output", "--group", "5", "--seconds", "30"}, "takeover-dead");
    REQUIRE(peer != nullptr);
    INFO(peer->output());
    REQUIRE(peer->waitForLine("state active", 5000));

    const std::string rvName = segmentName(rig.naming(), SegmentId::rendezvous());
    SegmentView rvView;
    REQUIRE(rig.backend().openExisting(rvName, rvView) == InitResult::kOk);
    RoleSlot& slot = roleSlot(*static_cast<RvSegment*>(rvView.base), 5, Role::kOutput);
    const u64 oldGen = ctlGen(slot.ctl.load());
    CHECK(slot.pid.load() == peer->pid());

    Rendezvous local(configFor(Role::kOutput, rig));
    const u64 start = rinne::util::monoNowMs();
    local.setGroup(5, ClaimIntent::kManual, start);
    local.prepare({48000, kBlockFrames, 2}, start);

    bool conflict = false;
    const u64 conflictDeadline = rinne::util::monoNowMs() + 2000;
    while (rinne::util::monoNowMs() < conflictDeadline)
    {
        local.tick(rinne::util::monoNowMs());
        if (local.state() == RendezvousState::kConflict)
        {
            conflict = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    INFO(peer->output());
    REQUIRE(conflict);

    const u64 killAt = rinne::util::monoNowMs();
    peer->kill();
    u64 activeAt = 0;
    const u64 takeoverDeadline = killAt + 9000;
    while (rinne::util::monoNowMs() < takeoverDeadline)
    {
        local.tick(rinne::util::monoNowMs());
        if (local.state() == RendezvousState::kActive)
        {
            activeAt = rinne::util::monoNowMs();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    UNSCOPED_INFO("local final state=" << static_cast<int>(local.state()));
    REQUIRE(activeAt != 0); // 超时：贴出 local 的状态与 peer 的最后输出
    const u64 delay = activeAt - killAt;
    INFO("takeover delay ms=" << delay);
    CHECK(delay >= 4750);
    CHECK(delay <= 7500);

    const RoleSlot& taken = roleSlot(*static_cast<RvSegment*>(rvView.base), 5, Role::kOutput);
    CHECK(taken.ctl.load() == local.myCtl());
    CHECK(ctlGen(taken.ctl.load()) > oldGen);
    CHECK(taken.pid.load() == currentProcessId());
    const LoopBinding* binding = local.bindings().current();
    REQUIRE(binding != nullptr);
    REQUIRE(binding->ring != nullptr);
    const auto& ring = *static_cast<const RingSegment*>(binding->ring->base());
    CHECK(local.writerToken() != 0);
    CHECK(ring.header.writer_token.load() == local.writerToken());
    rig.backend().unmap(rvView);
}

TEST_CASE("two output peers elect exactly one owner", "[ipc][xproc]")
{
    if (!peerUsable())
    {
        SKIP(peerSkipReason());
    }
    PeerRig rig;
    auto first = rig.spawn({"--role", "output", "--group", "6", "--seconds", "3"}, "race-first");
    auto second = rig.spawn({"--role", "output", "--group", "6", "--seconds", "3"}, "race-second");
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);

    int firstCode = -1;
    int secondCode = -1;
    const bool firstExited = first->wait(8000, firstCode);
    const bool secondExited = second->wait(8000, secondCode);
    INFO(first->output());
    INFO(second->output());
    REQUIRE(firstExited);
    REQUIRE(secondExited);
    CHECK(firstCode == 0);
    CHECK(secondCode == 0);

    const PeerResult firstResult = parsePeerResult(first->output());
    const PeerResult secondResult = parsePeerResult(second->output());
    REQUIRE(firstResult.ok);
    REQUIRE(secondResult.ok);
    const int actives = (firstResult.state == "active" ? 1 : 0) + (secondResult.state == "active" ? 1 : 0);
    const int conflicts = (firstResult.state == "conflict" ? 1 : 0) + (secondResult.state == "conflict" ? 1 : 0);
    CHECK(actives == 1);
    CHECK(conflicts == 1);
}
