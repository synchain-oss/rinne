// SPDX-License-Identifier: GPL-3.0-or-later
//
// abi 不同时的双边行为：
//   peer 先：另一个 abi 的 peer 建好 rv 并写它自己角色的 version；我们的 Rendezvous 只许报 kAbiMismatch、读出那个
//            version、一个字节都不写 rv（前后两份只读快照逐字节相同），也不许进 active / conflict。
//   我们先：我们建好 kIpcAbi 的 rv；另一个 abi 的 peer 必须在 result 行之前报 abiMismatch，我们保持 active。
// 两边都不许停在 waitingPrepare / opening。
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "IpcTestKit.h"
#include "ipc/Rendezvous.h"
#include "ipc/SegmentLayout.h"
#include "util/MonoClock.h"

namespace
{

using namespace rinne::test;

// 不是 kIpcAbi：对方构建的版本号，同时拿来验证我们从端点槽里读回的是它
constexpr u32 kForeignVersion = 0x00020304u;

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
    config.seed = 0xabcd;
    return config;
}

} // namespace

TEST_CASE("a foreign-abi peer publishes first and we only report the mismatch", "[ipc][xproc]")
{
    if (!peerUsable())
    {
        SKIP(peerSkipReason());
    }
    PeerRig rig;
    auto peer = rig.spawn({"--role", "output", "--group", "4", "--abi", "2", "--version",
                           std::to_string(kForeignVersion), "--seconds", "3"},
                          "abi-foreign-first");
    REQUIRE(peer != nullptr);
    INFO(peer->output());
    REQUIRE(peer->waitForLine("state foreignPublished", 5000));

    const std::string rvName = segmentName(rig.naming(), SegmentId::rendezvous());
    const std::vector<unsigned char> before = readHeaderBytes(rig.backend(), rvName, kRvHeaderBytes);

    Rendezvous input(configFor(Role::kInput, rig));
    const u64 start = rinne::util::monoNowMs();
    input.setGroup(4, ClaimIntent::kManual, start);
    input.prepare({48000, kBlockFrames, 2}, start);

    bool mismatched = false;
    bool becameActive = false;
    bool becameConflict = false;
    const u64 deadline = rinne::util::monoNowMs() + 2000;
    while (rinne::util::monoNowMs() < deadline)
    {
        input.tick(rinne::util::monoNowMs());
        if (input.state() == RendezvousState::kAbiMismatch)
        {
            mismatched = true;
            break;
        }
        becameActive = becameActive || input.state() == RendezvousState::kActive;
        becameConflict = becameConflict || input.state() == RendezvousState::kConflict;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    INFO(peer->output());
    CHECK(mismatched);
    CHECK_FALSE(becameActive);
    CHECK_FALSE(becameConflict);
    CHECK(input.state() == RendezvousState::kAbiMismatch);
    CHECK(input.abiMismatchVersion() == kForeignVersion);

    // 再看 400 ms：仍然停在 kAbiMismatch（不是 opening / waitingPrepare），期间一个字节都没写 rv
    int mismatches = 0;
    int observations = 0;
    const u64 observeUntil = rinne::util::monoNowMs() + 400;
    while (rinne::util::monoNowMs() < observeUntil)
    {
        input.tick(rinne::util::monoNowMs());
        ++observations;
        mismatches += input.state() == RendezvousState::kAbiMismatch ? 1 : 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    CHECK(observations > 0);
    CHECK(mismatches == observations);
    const std::vector<unsigned char> after = readHeaderBytes(rig.backend(), rvName, kRvHeaderBytes);
    CHECK(before == after);

    int exitCode = -1;
    REQUIRE(peer->wait(6000, exitCode));
    INFO(peer->output());
    CHECK(exitCode == 0);
    const PeerResult result = parsePeerResult(peer->output());
    REQUIRE(result.ok);
    CHECK(result.state == "foreignPublished");
}

TEST_CASE("our kIpcAbi rendezvous makes a foreign-abi peer report the mismatch", "[ipc][xproc]")
{
    if (!peerUsable())
    {
        SKIP(peerSkipReason());
    }
    PeerRig rig;
    Rendezvous input(configFor(Role::kInput, rig));
    const u64 start = rinne::util::monoNowMs();
    input.setGroup(4, ClaimIntent::kManual, start);
    input.prepare({48000, kBlockFrames, 2}, start);
    bool active = false;
    const u64 activeDeadline = rinne::util::monoNowMs() + 3000;
    while (rinne::util::monoNowMs() < activeDeadline)
    {
        input.tick(rinne::util::monoNowMs());
        if (input.state() == RendezvousState::kActive)
        {
            active = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE(active);

    auto peer = rig.spawn({"--role", "output", "--group", "4", "--abi", "2", "--seconds", "2"}, "abi-foreign-second");
    REQUIRE(peer != nullptr);
    INFO(peer->output());
    REQUIRE(peer->waitForLine("state abiMismatch", 5000));

    // 对方报不兼容，我们这边不受影响
    for (int i = 0; i < 10; ++i)
    {
        input.tick(rinne::util::monoNowMs());
        CHECK(input.state() == RendezvousState::kActive);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    int exitCode = -1;
    REQUIRE(peer->wait(6000, exitCode));
    INFO(peer->output());
    CHECK(exitCode == 0);
    const PeerResult result = parsePeerResult(peer->output());
    REQUIRE(result.ok);
    CHECK(result.state == "abiMismatch");
}
