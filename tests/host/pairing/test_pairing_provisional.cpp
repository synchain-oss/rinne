// SPDX-License-Identifier: GPL-3.0-or-later
//
// provisional 认领与转正：工程 state 后到时抢占智能认领；三个转正时刻（第一次 getState、第一次 setState 处理完、
// 定型）各一格，转正之后不再被抢；复制轨道（宿主先对原实例 getState）时原实例照常工作、复制体进冲突，一键修复
// 移到空闲组或重新配对，修复要定型并标脏；目标是休眠槽时先要用户确认。
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "PairingKit.h"

namespace
{

using namespace rinne::pairing_test;
using rinne::InputProcessor;
using rinne::OutputProcessor;

} // namespace

TEST_CASE("pairing: a project state that arrives late preempts a provisional smart claim",
          "[host][pairing][provisional]")
{
    formalOnly();
    needsSharedMemory();
    Bench bench;
    OutputProcessor& smart = bench.addOutput();
    bench.run(400);
    REQUIRE(holds(smart, 1));
    REQUIRE(provisional(smart));

    SECTION("another instance restored into the same group takes the slot")
    {
        OutputProcessor& restored = bench.addOutput();
        restore(restored, groupState(state::Role::Output, 1, state::GroupOrigin::Smart));
        bench.run(400);
        CHECK(holds(restored, 1));
        CHECK_FALSE(provisional(restored));
        CHECK(stateOf(smart) == ipc::RendezvousState::kConflict);
        CHECK(pairStateOf(smart) == PairState::kConflict);
        // 冲突里不重评、不自动迁组
        bench.run(2500);
        CHECK(stateOf(smart) == ipc::RendezvousState::kConflict);
        CHECK(groupOf(smart) == 1);
    }
    SECTION("the instance's own project state wins over its smart choice")
    {
        restore(smart, groupState(state::Role::Output, 4));
        bench.run(200);
        CHECK(holds(smart, 4));
        CHECK(configsOf(smart).group == 4);
        CHECK(configsOf(smart).groupOrigin == state::GroupOrigin::User);
        // 组号已定：之后不再智能默认，也不再重评
        InputProcessor& input = bench.addInput();
        REQUIRE(input.selectGroup(2));
        bench.run(2500);
        CHECK(holds(smart, 4));
    }
}

TEST_CASE("pairing: once confirmed a claim is no longer preempted", "[host][pairing][provisional]")
{
    formalOnly();
    needsSharedMemory();
    Bench bench;

    SECTION("confirmed by the first getState after the claim")
    {
        OutputProcessor& first = bench.addOutput();
        bench.run(400);
        REQUIRE(provisional(first));
        const std::vector<std::uint8_t> bytes = savedState(first);
        CHECK_FALSE(provisional(first));
        CHECK(holds(first, 1));
        OutputProcessor& late = bench.addOutput();
        restore(late, groupState(state::Role::Output, 1));
        bench.run(1500);
        CHECK(holds(first, 1));
        CHECK(stateOf(late) == ipc::RendezvousState::kConflict);
    }
    SECTION("confirmed by the first setState after the claim, whatever it loads")
    {
        OutputProcessor& first = bench.addOutput();
        bench.run(400);
        REQUIRE(provisional(first));
        restore(first, std::vector<std::uint8_t>{1, 2, 3}); // 读不出来：什么都不应用，回调照样调
        REQUIRE(first.stateGlue()->lastLoadStatus() == state::StateLoadStatus::Corrupt);
        CHECK_FALSE(provisional(first));
        CHECK(holds(first, 1));
        OutputProcessor& late = bench.addOutput();
        restore(late, groupState(state::Role::Output, 1));
        bench.run(1500);
        CHECK(holds(first, 1));
        CHECK(stateOf(late) == ipc::RendezvousState::kConflict);
    }
    SECTION("confirmed the moment the instance settles")
    {
        InputProcessor& first = bench.addInput();
        bench.run(400);
        REQUIRE(provisional(first));
        first.userParamGestureBegan(); // 界面上的一次参数手势：定型
        REQUIRE(settled(first));
        CHECK_FALSE(provisional(first));
        InputProcessor& late = bench.addInput();
        restore(late, groupState(state::Role::Input, 1));
        bench.run(1500);
        CHECK(holds(first, 1));
        CHECK(stateOf(late) == ipc::RendezvousState::kConflict);
    }
}

TEST_CASE("pairing: duplicating a track keeps the original working and puts the copy in conflict",
          "[host][pairing][provisional][duplicate]")
{
    formalOnly();
    needsSharedMemory();
    Bench bench;
    InputProcessor& input = bench.addInput();
    OutputProcessor& output = bench.addOutput();
    setTrack(input, juce::String("Lead Vox"));
    setTrack(output, juce::String("Lead Vox"));
    bench.run(600);
    REQUIRE(holds(input, 1));
    REQUIRE(holds(output, 1));
    REQUIRE(provisional(input));
    REQUIRE(provisional(output));

    // 宿主复制轨道：先对原实例 getState（两端都转正），再建复制体、把那份 state 交给它
    const std::vector<std::uint8_t> inputBytes = savedState(input);
    const std::vector<std::uint8_t> outputBytes = savedState(output);
    InputProcessor& inputCopy = bench.addInput();
    OutputProcessor& outputCopy = bench.addOutput();
    setTrack(inputCopy, juce::String("Lead Vox 2"));
    setTrack(outputCopy, juce::String("Lead Vox 2"));
    restore(inputCopy, inputBytes);
    restore(outputCopy, outputBytes);
    bench.run(600);
    CHECK(holds(input, 1));
    CHECK(holds(output, 1));
    CHECK(pairStateOf(input) == PairState::kConnected);
    CHECK(pairStateOf(output) == PairState::kConnected);
    CHECK(stateOf(inputCopy) == ipc::RendezvousState::kConflict);
    CHECK(stateOf(outputCopy) == ipc::RendezvousState::kConflict);
    CHECK(pairStateOf(inputCopy) == PairState::kConflict);
    CHECK(inputCopy.pairState().occupantName == "Lead Vox"); // 冲突时报出占着槽的那条轨
    // 冲突不自动迁组
    bench.run(2500);
    CHECK(groupOf(inputCopy) == 1);
    CHECK(stateOf(inputCopy) == ipc::RendezvousState::kConflict);

    SECTION("the one-click repair moves the Input to a free group and re-pairs the Output with it")
    {
        DirtyListener inputDirty;
        DirtyListener outputDirty;
        inputCopy.addListener(&inputDirty);
        outputCopy.addListener(&outputDirty);
        REQUIRE_FALSE(settled(inputCopy));

        const auto moved = inputCopy.repairGroup();
        CHECK(moved.applied);
        CHECK(moved.group == 2);
        CHECK_FALSE(moved.needsConfirm);
        CHECK(settled(inputCopy));
        CHECK(inputDirty.dirtyMarks == 1);
        CHECK(configsOf(inputCopy).group == 2);
        CHECK(configsOf(inputCopy).groupOrigin == state::GroupOrigin::User);
        bench.run(200);
        CHECK(holds(inputCopy, 2));

        const auto repaired = outputCopy.repairGroup();
        CHECK(repaired.applied);
        CHECK(repaired.group == 2);
        CHECK(settled(outputCopy));
        CHECK(outputDirty.dirtyMarks == 1);
        bench.run(200);
        CHECK(holds(outputCopy, 2));
        CHECK(holds(input, 1));
        CHECK(holds(output, 1));
        bench.run(100);
        CHECK(pairStateOf(inputCopy) == PairState::kConnected);
        inputCopy.removeListener(&inputDirty);
        outputCopy.removeListener(&outputDirty);
    }
    SECTION("outside a conflict there is nothing to repair")
    {
        const auto none = input.repairGroup();
        CHECK(none.group == 0);
        CHECK_FALSE(none.applied);
        CHECK(holds(input, 1));
        CHECK_FALSE(settled(input));
    }
}

TEST_CASE("pairing: a repair that would take over a dormant slot waits for the user's confirmation",
          "[host][pairing][provisional][duplicate]")
{
    formalOnly();
    needsSharedMemory();
    Bench bench;
    InputProcessor& original = bench.addInput();
    REQUIRE(original.selectGroup(1));
    std::vector<InputProcessor*> others;
    for (int group = 2; group <= 8; ++group)
    {
        InputProcessor& other = bench.addInput();
        REQUIRE(other.selectGroup(group));
        others.push_back(&other);
    }
    InputProcessor& copy = bench.addInput();
    restore(copy, groupState(state::Role::Input, 1));
    bench.run(300);
    REQUIRE(stateOf(copy) == ipc::RendezvousState::kConflict);
    // 组 B 的 Input 被宿主停用：槽休眠。没有两端都空的组，能去的只剩这个休眠槽
    others.front()->releaseResources();
    bench.setRunning(*others.front(), false);
    bench.run(300);

    const auto asked = copy.repairGroup();
    CHECK(asked.group == 2);
    CHECK(asked.needsConfirm);
    CHECK_FALSE(asked.applied);
    CHECK_FALSE(settled(copy));
    CHECK(configsOf(copy).group == 1);
    CHECK(stateOf(copy) == ipc::RendezvousState::kConflict);

    const auto confirmed = copy.repairGroup(true);
    CHECK(confirmed.applied);
    CHECK(confirmed.group == 2);
    CHECK(settled(copy));
    CHECK(runUntil(bench, [&] { return holds(copy, 2); }, 4000));
}
