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

// 最后一条 "state <名字>" 行里的名字；一条都没有就是空串
std::string lastState(const std::string& output)
{
    const std::size_t at = output.rfind("state ");
    if (at == std::string::npos)
    {
        return {};
    }
    const std::size_t begin = at + 6;
    const std::size_t end = output.find_first_of(" \r\n", begin);
    return output.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
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
    const bool reached = peer->waitForLine("state active", 5000);
    INFO(peer->output());
    REQUIRE(reached);

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
    if (activeAt == 0)
    {
        FAIL("no takeover before the deadline; local state=" << static_cast<int>(local.state()) << "\npeer output:\n"
                                                             << peer->output());
    }
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
    auto first = rig.spawn({"--role", "output", "--group", "6", "--seconds", "6"}, "race-first");
    auto second = rig.spawn({"--role", "output", "--group", "6", "--seconds", "6"}, "race-second");
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);

    // 两个 peer 都还活着的时候判胜负：退出时的状态不可靠（先退的放槽，后退的可能赶上冲突重试而占到空槽）
    const auto decided = [](const Spawned& peer) {
        return peer.waitForLine("state active", 1) || peer.waitForLine("state conflict", 1);
    };
    const std::uint64_t deadline = rinne::util::monoNowMs() + 4000;
    while (rinne::util::monoNowMs() < deadline && !(decided(*first) && decided(*second)))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    // 再等一个冲突重试周期，确认没有人在中途翻盘
    std::this_thread::sleep_for(std::chrono::milliseconds(1300));
    const std::string firstState = lastState(first->output());
    const std::string secondState = lastState(second->output());
    INFO(first->output());
    INFO(second->output());
    const int actives = (firstState == "active" ? 1 : 0) + (secondState == "active" ? 1 : 0);
    const int conflicts = (firstState == "conflict" ? 1 : 0) + (secondState == "conflict" ? 1 : 0);
    CHECK(actives == 1);
    CHECK(conflicts == 1);

    int firstCode = -1;
    int secondCode = -1;
    const bool firstExited = first->wait(10000, firstCode);
    const bool secondExited = second->wait(10000, secondCode);
    REQUIRE(firstExited);
    REQUIRE(secondExited);
    CHECK(firstCode == 0);
    CHECK(secondCode == 0);
    CHECK(parsePeerResult(first->output()).ok);
    CHECK(parsePeerResult(second->output()).ok);
}
