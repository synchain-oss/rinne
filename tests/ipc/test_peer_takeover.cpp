// SPDX-License-Identifier: GPL-3.0-or-later
//
// 接管与选举的跨进程版本：
//   杀掉一个 active 的 Output peer，本进程的同组 Output 必须等满死进程的观察窗口（>= 5 s）才能接管：
//   delay 落在 [4.8 s, 6.5 s]，接管之后槽里的 pid 是我们、ctl 的 gen 前进、环的 writer_token 换成我们的。
//   同时起两个同角色的 peer（Output 一组、Input 一组）：真人竞争同一槽，恰好一个 active、另一个 conflict
//   （活着的对端永不被自动接管），两个都正常退出。
//   每条用例收尾都探一遍自己测试作用域下的段，一个都不许剩（崩溃的 peer 也不例外）。
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

// 死进程接管的窗口（毫秒），从 kill 起算。
//   下界：判死（takeoverTier）要求心跳、block_seq、ctl 都静止满 RendezvousTiming::kDeadMs（5 s）。
//   三者里最晚满足的是 block_seq：它的静止起点是观察者第一次看到最终取值的那次扫描，不会早于 peer
//   最后一次写它的时刻。peer 的音频线程每约 10 ms 加一次（tools/rinne_ipc_peer.cpp），所以最后一次
//   写入至多比 kill 早一个循环周期，理论最早约 4.99 s。心跳最多比 kill 早一个 kHeartbeatMs（250 ms）
//   写入，ctl 早已静止，两者都先于 block_seq 满足，不是约束。
//   死进程只在冲突重试的拍子上判（Rendezvous::claimTick）。重试从本测试进入冲突的那一拍起每 1 s 一次，
//   每次再晚一个 tick，所以接管只会落在 kill 后约 k 秒加几十毫秒的栅格上：第 4 次重试约在 4.1 s，
//   那时判死标准没满足，接管不了。
//   取 4.8 s：在第 4 次重试之后、理论最早 4.99 s 之前，比理论低约 190 ms，留给 peer 的音频线程
//   在被杀前被调度走的一小会儿。这个下界抓得到把 kDeadMs 缩短到约 4 s 或更低的实现；只缩短几百毫秒的
//   实现照样等到第 5 次重试，在 5.1 s 左右接管，这个测试看不出来，那是 1 s 重试栅格的粒度，
//   不是下界取得松。Windows 上实测 30 次最小 5.05 s。
//   上界：判死发生在观察者的扫描上（SlotObserver::kScanIntervalMs，250 ms），认领则等下一次冲突重试
//   （RendezvousTiming::kConflictRetryMs，1 s），再加 tick 的粒度与 CI 机器的调度抖动。实测 30 次最大
//   5.17 s；试过在 kill 前先等观察者刚扫完一轮（重试相位错开），也只到 5.8–5.95 s，仍在 6.5 s 以内。
constexpr u64 kTakeoverMinMs = 4800;
constexpr u64 kTakeoverMaxMs = 6500;

// 两个同角色的 peer 抢同一个槽：恰好一个 active、另一个 conflict，两个都正常退出，结果行里 bad = 0。
void expectExactlyOneOwner(const char* role, const char* group, const std::string& stem)
{
    PeerRig rig;
    {
        auto first = rig.spawn({"--role", role, "--group", group, "--seconds", "6"}, stem + "-first");
        auto second = rig.spawn({"--role", role, "--group", group, "--seconds", "6"}, stem + "-second");
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
    expectNoLeftovers(rig);
}

} // namespace

TEST_CASE("a killed output peer is taken over only after the dead-process window", "[ipc][xproc]")
{
    if (!peerUsable())
    {
        SKIP(peerSkipReason());
    }
    PeerRig rig;
    {
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
            FAIL("no takeover before the deadline; local state=" << static_cast<int>(local.state())
                                                                 << "\npeer output:\n"
                                                                 << peer->output());
        }
        const u64 delay = activeAt - killAt;
        INFO("takeover delay ms=" << delay);
        CHECK(delay >= kTakeoverMinMs);
        CHECK(delay <= kTakeoverMaxMs);

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
    expectNoLeftovers(rig);
}

TEST_CASE("two output peers elect exactly one owner", "[ipc][xproc]")
{
    if (!peerUsable())
    {
        SKIP(peerSkipReason());
    }
    expectExactlyOneOwner("output", "6", "race-output");
}

TEST_CASE("two input peers elect exactly one owner", "[ipc][xproc]")
{
    if (!peerUsable())
    {
        SKIP(peerSkipReason());
    }
    expectExactlyOneOwner("input", "7", "race-input");
}
