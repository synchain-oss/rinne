// SPDX-License-Identifier: GPL-3.0-or-later
//
// 跨进程连续性：一个真实的 peer 进程与本进程里的 Rendezvous 在同一个测试作用域里对通。
//   正向：peer 是 Output，写位置编码；本进程 Input 读，逐帧解回绝对位置。读到 96000 帧或 6 s 超时为止，valid 至少
//   kMinFlowFrames，bad 必须为 0。 反向：本进程 Output 按墙钟配速写；peer 是 Input，它的 result 行要报 valid >=
//   kMinFlowFrames 且 bad = 0。
// 计时全是真实时钟（跨进程只能如此）；每个等待都有超时，失败信息里贴 peer 的原话。
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
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

// 「数据确实在流」的帧数下限（20 块，0.2 s）。连续性的判据是 bad == 0（每个判为有效的帧都解码回自己的位置），
// 不是吞吐；吞吐取决于 CI 机器上线程的睡眠粒度（mac runner 上 peer 的 10 ms 音频线程实测只推进到约 21000 帧），
// 所以下限只要足以证明读写两端都在动、又不被调度抖动误伤。
constexpr unsigned long long kMinFlowFrames = 9600;

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
    config.seed = 0x1234;
    return config;
}

bool tickUntil(Rendezvous& rendezvous, RendezvousState wanted, std::uint64_t timeoutMs)
{
    const std::uint64_t deadline = rinne::util::monoNowMs() + timeoutMs;
    while (rinne::util::monoNowMs() < deadline)
    {
        rendezvous.tick(rinne::util::monoNowMs());
        if (rendezvous.state() == wanted)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    rendezvous.tick(rinne::util::monoNowMs());
    return rendezvous.state() == wanted;
}

// 与 peer 的读循环一致：W - readPos 超过半环就重同步；一帧一帧按位置解码核对
void readBlock(RingSegment& ring, std::int64_t& readPos, float* left, float* right, std::uint64_t& valid,
               std::uint64_t& bad, std::uint64_t& resyncs, std::uint64_t& frames)
{
    const std::uint64_t writeHead = LoopRing::writeHead(ring);
    if (readPos < 0)
    {
        readPos = static_cast<std::int64_t>(writeHead);
    }
    if (frameDiff(writeHead, static_cast<std::uint64_t>(readPos)) > static_cast<std::int64_t>(kRingFrames / 2))
    {
        readPos = static_cast<std::int64_t>(writeHead);
        ++resyncs;
    }
    const std::int64_t available = frameDiff(writeHead, static_cast<std::uint64_t>(readPos));
    if (available <= 0)
    {
        return;
    }
    const u32 count = static_cast<u32>(std::min<std::int64_t>(available, kBlockFrames));
    const RingRead read = LoopRing::read(ring, readPos, count, left, right);
    for (std::int64_t position = read.validFrom; position < read.validTo; ++position)
    {
        u64 decoded = 0;
        if (decodeRamp(left[position - readPos], right[position - readPos], decoded) &&
            decoded == static_cast<std::uint64_t>(position))
        {
            ++valid;
        }
        else
        {
            ++bad;
        }
    }
    readPos += count;
    frames += static_cast<std::uint64_t>(count);
}

} // namespace

TEST_CASE("an output peer feeds an in-test input across processes", "[ipc][xproc]")
{
    if (!peerUsable())
    {
        SKIP(peerSkipReason());
    }
    PeerRig rig;
    auto peer = rig.spawn({"--role", "output", "--group", "3", "--seconds", "10"}, "continuity-forward");
    REQUIRE(peer != nullptr);
    const bool reached = peer->waitForLine("state active", 5000);
    INFO(peer->output());
    REQUIRE(reached);

    Rendezvous input(configFor(Role::kInput, rig));
    const u64 start = rinne::util::monoNowMs();
    input.setGroup(3, ClaimIntent::kManual, start);
    input.prepare({48000, kBlockFrames, 2}, start);
    const bool ticked = tickUntil(input, RendezvousState::kActive, 3000);
    INFO(peer->output());
    REQUIRE(ticked);
    REQUIRE(input.bindings().current() != nullptr);
    REQUIRE(input.bindings().current()->ring != nullptr);

    // peer 的 pid 出现在我们对 Output 槽的观察样本里
    bool sawPeerPid = false;
    const u64 pidDeadline = rinne::util::monoNowMs() + 2000;
    while (rinne::util::monoNowMs() < pidDeadline && !sawPeerPid)
    {
        input.tick(rinne::util::monoNowMs());
        sawPeerPid = input.observer().sample(3, Role::kOutput).pid == peer->pid();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(sawPeerPid);

    std::array<float, kBlockFrames> left{};
    std::array<float, kBlockFrames> right{};
    std::int64_t readPos = -1;
    u64 valid = 0;
    u64 bad = 0;
    u64 resyncs = 0;
    u64 frames = 0;
    const u64 readDeadline = rinne::util::monoNowMs() + 6000;
    while (rinne::util::monoNowMs() < readDeadline && valid < 96000)
    {
        input.tick(rinne::util::monoNowMs());
        // 每 10 ms 一轮，每轮最多读 4 块 480 帧（读得比写快，实际读多少由环里可读的量限制）
        for (int blockIndex = 0; blockIndex < 4; ++blockIndex)
        {
            const LoopBinding* binding = input.bindings().current();
            if (binding == nullptr)
            {
                break;
            }
            BoundBlock bounded(binding);
            if (!bounded.ok() || !bounded.stillMine())
            {
                break;
            }
            readBlock(bounded.ring(), readPos, left.data(), right.data(), valid, bad, resyncs, frames);
            bounded.mine().block_seq.fetch_add(1, std::memory_order_relaxed);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    INFO("valid=" << valid << " bad=" << bad << " frames=" << frames << " resyncs=" << resyncs);
    REQUIRE(valid >= kMinFlowFrames);
    CHECK(bad == 0);
    CHECK(frames >= kMinFlowFrames);

    int exitCode = -1;
    REQUIRE(peer->wait(12000, exitCode));
    INFO(peer->output());
    CHECK(exitCode == 0);
    const PeerResult result = parsePeerResult(peer->output());
    REQUIRE(result.ok);
    CHECK(result.state == "active");
    CHECK(result.frames > 0);
}

TEST_CASE("an in-test output feeds an input peer across processes", "[ipc][xproc]")
{
    if (!peerUsable())
    {
        SKIP(peerSkipReason());
    }
    PeerRig rig;
    auto peer = rig.spawn({"--role", "input", "--group", "4", "--seconds", "10"}, "continuity-reverse");
    REQUIRE(peer != nullptr);

    Rendezvous output(configFor(Role::kOutput, rig));
    const u64 start = rinne::util::monoNowMs();
    output.setGroup(4, ClaimIntent::kManual, start);
    output.prepare({48000, kBlockFrames, 2}, start);
    const bool ticked = tickUntil(output, RendezvousState::kActive, 3000);
    INFO(peer->output());
    REQUIRE(ticked);
    // 对端绑定之前写下的帧它读不到（读位置从写头开始），所以先等它 active；等的时候本进程继续 tick，心跳不能停
    const std::uint64_t peerDeadline = rinne::util::monoNowMs() + 5000;
    while (rinne::util::monoNowMs() < peerDeadline && !peer->waitForLine("state active", 1))
    {
        output.tick(rinne::util::monoNowMs());
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!peer->waitForLine("state active", 1))
    {
        FAIL("input peer never reached active\npeer output:\n" << peer->output());
    }
    REQUIRE(output.bindings().current() != nullptr);

    RingWriter writer;
    writer.reset(48000);
    u64 written = 0;
    const u64 writeStart = rinne::util::monoNowMs();
    const u64 writeDeadline = writeStart + 5000;
    u64 blocksTried = 0;
    while (rinne::util::monoNowMs() < writeDeadline)
    {
        output.tick(rinne::util::monoNowMs());
        // 按墙钟配速：每 10 ms 一块 480 帧，与 peer 的音频线程同速，慢了就补（一拍最多补 8 块）
        const u64 due = (rinne::util::monoNowMs() - writeStart) / 10 + 1;
        for (int blockIndex = 0; blockIndex < 8 && blocksTried < due; ++blockIndex)
        {
            const LoopBinding* binding = output.bindings().current();
            if (binding == nullptr)
            {
                break;
            }
            BoundBlock block(binding);
            if (!block.ok() || !block.stillMine() || !LoopRing::isWriter(block.ring(), block.binding().writerToken))
            {
                break;
            }
            const RingWriteResult writtenBlock = writer.write(
                block.ring(), kBlockFrames, [](u32, u64 position, float& l, float& r) { encodeRamp(position, l, r); });
            ++blocksTried;
            if (writtenBlock == RingWriteResult::kOk)
            {
                written += kBlockFrames;
            }
            block.mine().block_seq.fetch_add(1, std::memory_order_relaxed);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    INFO("written=" << written);
    CHECK(written >= kMinFlowFrames);

    int exitCode = -1;
    REQUIRE(peer->wait(12000, exitCode));
    INFO(peer->output());
    CHECK(exitCode == 0);
    const PeerResult result = parsePeerResult(peer->output());
    REQUIRE(result.ok);
    CHECK(result.state == "active");
    CHECK(result.valid >= kMinFlowFrames);
    CHECK(result.bad == 0);
}
