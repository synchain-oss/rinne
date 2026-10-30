// SPDX-License-Identifier: GPL-3.0-or-later
//
// 配对状态的接线：每拍由 PairStateModel 折一次，pairState() 只读一份拷贝。这里只验各项事实有没有接对（音频有没有来、
// 对端在不在、旁路与宿主暂停并进 loopSilent、冲突、验证宿主），判定顺序本身由 tests/core/pairing 覆盖。
#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include "PairingKit.h"

namespace
{

using namespace rinne::pairing_test;
using rinne::InputProcessor;
using rinne::OutputProcessor;

constexpr std::uint32_t bitOf(std::uint32_t group)
{
    return 1u << (group - 1u);
}

} // namespace

TEST_CASE("pairing: in a validation host the pair state is validator and nothing else",
          "[host][pairing][state][validation]")
{
    formalOnly();
    Bench bench;
    InputProcessor& input = bench.addInput();
    OutputProcessor& output = bench.addOutput();
    bench.run(400);
    CHECK((pairStateOf(input) == PairState::kValidator) == runningAsValidationHost());
    CHECK((pairStateOf(output) == PairState::kValidator) == runningAsValidationHost());
    if (runningAsValidationHost())
    {
        // 验证宿主里不认领，也不进诊断日志
        CHECK(groupOf(input) == 0);
        CHECK(groupOf(output) == 0);
        CHECK(captured().installs.empty());
    }
}

TEST_CASE("pairing: without realtime audio the state waits for audio, then searches for the peer",
          "[host][pairing][state]")
{
    formalOnly();
    needsSharedMemory();
    Bench bench;
    InputProcessor& input = bench.addInput();
    REQUIRE(input.selectGroup(1));
    bench.setRunning(input, false);
    bench.idle(200);
    REQUIRE(holds(input, 1));
    CHECK(pairStateOf(input) == PairState::kWaitingAudio);

    bench.setRunning(input, true);
    bench.run(100);
    CHECK(pairStateOf(input) == PairState::kSearching);
    CHECK(input.pairState().group == 1);
    CHECK(input.pairState().peerVersion.empty());

    OutputProcessor& output = bench.addOutput();
    REQUIRE(output.selectGroup(1));
    bench.run(100);
    CHECK(pairStateOf(input) == PairState::kConnectedUnverified);
    CHECK(pairStateOf(output) == PairState::kConnectedUnverified);
    CHECK(output.pairState().peerVersion == input.pairState().selfVersion);
    CHECK_FALSE(output.pairState().channelMismatch);

    // 对端离开：回到 searching，对端的名字与版本不留残影
    bench.remove(output);
    bench.run(100);
    CHECK(pairStateOf(input) == PairState::kSearching);
    CHECK(input.pairState().peerVersion.empty());
}

TEST_CASE("pairing: a bypassed or stopped end turns a connected pair still on both sides", "[host][pairing][state]")
{
    formalOnly();
    needsSharedMemory();
    Bench bench;
    InputProcessor& input = bench.addInput();
    OutputProcessor& output = bench.addOutput();
    setTrack(input, juce::String("Lead Vox"));
    setTrack(output, juce::String("Lead Vox"));
    REQUIRE(input.selectGroup(1));
    REQUIRE(output.selectGroup(1));
    bench.run(200);
    REQUIRE(pairStateOf(input) == PairState::kConnected);
    REQUIRE(pairStateOf(output) == PairState::kConnected);

    SECTION("the Input is bypassed by the host")
    {
        bench.setBypassed(input, true);
        bench.run(100);
        CHECK(pairStateOf(input) == PairState::kConnectedStill);
        CHECK(pairStateOf(output) == PairState::kConnectedStill); // Output 从槽里的旁路位看到
        bench.setBypassed(input, false);
        bench.run(100);
        CHECK(pairStateOf(input) == PairState::kConnected);
        CHECK(pairStateOf(output) == PairState::kConnected);
    }
    SECTION("the Output is bypassed by the host")
    {
        bench.setBypassed(output, true);
        bench.run(100);
        CHECK(pairStateOf(output) == PairState::kBypassed);
        CHECK(pairStateOf(input) == PairState::kConnectedStill);
    }
    SECTION("the host stops calling back for longer than 250 ms")
    {
        bench.setRunning(input, false);
        bench.setRunning(output, false);
        bench.idle(200);
        CHECK(pairStateOf(input) == PairState::kConnected); // 200 ms 还不算停
        bench.idle(200);
        CHECK(pairStateOf(input) == PairState::kConnectedStill);
        CHECK(pairStateOf(output) == PairState::kConnectedStill);
        bench.setRunning(input, true);
        bench.setRunning(output, true);
        bench.run(100);
        CHECK(pairStateOf(input) == PairState::kConnected);
    }
}

TEST_CASE("pairing: conflict and occupancy come from the shared snapshot", "[host][pairing][state]")
{
    formalOnly();
    needsSharedMemory();
    Bench bench;
    InputProcessor& lonelyInput = bench.addInput();
    REQUIRE(lonelyInput.selectGroup(2));
    OutputProcessor& otherOutput = bench.addOutput();
    REQUIRE(otherOutput.selectGroup(3));
    OutputProcessor& output = bench.addOutput();
    REQUIRE(output.selectGroup(1));
    bench.run(200);

    const auto model = output.pairState();
    CHECK(model.state == PairState::kSearching);
    CHECK(model.group == 1);
    CHECK((model.peerMask & bitOf(2)) != 0u); // 组 B 有个 Input 在等 Output
    CHECK((model.occupiedMask & bitOf(3)) != 0u); // 组 C 的 Output 端被别人占着
    CHECK((model.occupiedMask & bitOf(1)) == 0u); // 自己那一格不算

    OutputProcessor& copy = bench.addOutput();
    REQUIRE(copy.selectGroup(3));
    bench.run(200);
    CHECK(pairStateOf(copy) == PairState::kConflict);
    CHECK((copy.pairState().occupiedMask & bitOf(3)) != 0u); // 冲突时本组被别人占着
    CHECK(pairStateOf(otherOutput) == PairState::kSearching);
}
