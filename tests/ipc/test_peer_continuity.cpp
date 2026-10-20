// SPDX-License-Identifier: GPL-3.0-or-later
//
// 跨进程连续性：一个真实的 peer 进程与本进程里的 Rendezvous 在同一个测试作用域里对通。
//   正向：peer 是 Output，写位置编码；本进程 Input 读，逐帧解回绝对位置。valid 要够 1 s 的音频，bad 必须为 0。
//   反向：本进程 Output 写，peer 是 Input；peer 的 result 行要报 valid >= 48000 且 bad = 0。
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
    INFO(peer->output());
    REQUIRE(peer->waitForLine("state active", 5000));

    Rendezvous input(configFor(Role::kInput, rig));
    const u64 start = rinne::util::monoNowMs();
    input.setGroup(3, ClaimIntent::kManual, start);
    input.prepare({48000, kBlockFrames, 2}, start);
    INFO(peer->output());
    REQUIRE(tickUntil(input, RendezvousState::kActive, 3000));
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
        // 25 Hz 的一拍最多读 4 x 480 帧，与 peer 的音频线程同速
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
    REQUIRE(valid >= 24000);
    CHECK(bad == 0);
    CHECK(frames >= 24000);

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
    INFO(peer->output());
    REQUIRE(tickUntil(output, RendezvousState::kActive, 3000));
    REQUIRE(output.bindings().current() != nullptr);

    RingWriter writer;
    writer.reset(48000);
    u64 written = 0;
    const u64 writeDeadline = rinne::util::monoNowMs() + 6000;
    while (rinne::util::monoNowMs() < writeDeadline && written < 96000)
    {
        output.tick(rinne::util::monoNowMs());
        // 与 peer 的音频线程同速：一拍最多写 4 x 480 帧
        for (int blockIndex = 0; blockIndex < 4; ++blockIndex)
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
            if (writtenBlock == RingWriteResult::kOk)
            {
                written += kBlockFrames;
            }
            block.mine().block_seq.fetch_add(1, std::memory_order_relaxed);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    INFO("written=" << written);
    CHECK(written >= 24000);

    int exitCode = -1;
    REQUIRE(peer->wait(12000, exitCode));
    INFO(peer->output());
    CHECK(exitCode == 0);
    const PeerResult result = parsePeerResult(peer->output());
    REQUIRE(result.ok);
    CHECK(result.state == "active");
    CHECK(result.valid >= 24000);
    CHECK(result.bad == 0);
}
