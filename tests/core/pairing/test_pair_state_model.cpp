// SPDX-License-Identifier: GPL-3.0-or-later
//
// 配对状态模型：15 个状态各自一格，再单独验优先级链、颜色分组、机读标识与脉冲、名字核验、版本格式，
// 以及两个位图与可选字段的取舍。每个状态的构造只让一条判定为真，这样顺序改错时能直接指出被谁压过。
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <set>
#include <string>
#include <vector>

#include "ipc/LoopBinding.h"
#include "ipc/SegmentLayout.h"
#include "pairing/PairStateModel.h"

namespace
{

using rinne::ipc::EndView;
using rinne::ipc::GroupViews;
using rinne::ipc::kCtlPhaseActive;
using rinne::ipc::kCtlPhaseDormant;
using rinne::ipc::packVersion;
using rinne::ipc::Role;
using rinne::ipc::u32;
using rinne::pairing::buildPairModel;
using rinne::pairing::checkNames;
using rinne::pairing::colourOf;
using rinne::pairing::formatVersion;
using rinne::pairing::idOf;
using rinne::pairing::kPairStateCount;
using rinne::pairing::NameCheck;
using rinne::pairing::PairColour;
using rinne::pairing::PairInputs;
using rinne::pairing::PairModel;
using rinne::pairing::PairState;
using rinne::pairing::PeerFacts;
using rinne::pairing::pulses;

constexpr u32 kSelfPid = 4321;
constexpr u32 kOtherPid = 8765;
constexpr u32 kSelfHash = 0x1234u;
constexpr u32 kOtherHash = 0x9876u;

const GroupViews kEmptyGroups{};

constexpr u32 bitAt(u32 index)
{
    return static_cast<u32>(1u) << index;
}

PairInputs baseInputs()
{
    PairInputs in;
    in.selfPid = kSelfPid;
    in.selfVersion = packVersion(1, 2, 3);
    in.selfSampleRate = 48000;
    in.selfChannels = 2;
    in.selfNameHash = kSelfHash;
    in.group = 3;
    return in;
}

PeerFacts samePeer()
{
    PeerFacts peer;
    peer.present = true;
    peer.pid = kSelfPid;
    peer.version = packVersion(4, 5, 6);
    peer.sampleRate = 48000;
    peer.channels = 2;
    peer.nameHash = kSelfHash;
    peer.name = "Guitar";
    peer.colourArgb = 0xff336699u;
    return peer;
}

PairInputs linkedInputs()
{
    PairInputs in = baseInputs();
    in.peer = samePeer();
    in.realtimeAudioSeen = true;
    return in;
}

EndView dormantSlot(u32 pid)
{
    EndView slot;
    slot.phase = kCtlPhaseDormant;
    slot.pid = pid;
    return slot;
}

EndView activeSlot(u32 pid)
{
    EndView slot;
    slot.phase = kCtlPhaseActive;
    slot.fresh = true;
    slot.pid = pid;
    return slot;
}

struct StateCase
{
    const char* name;
    PairInputs inputs;
    PairState expected;
};

std::vector<StateCase> stateCases()
{
    std::vector<StateCase> cases;

    PairInputs validator = baseInputs();
    validator.validator = true;
    cases.push_back({"validator", validator, PairState::kValidator});

    PairInputs ipc = baseInputs();
    ipc.ipcUnavailable = true;
    cases.push_back({"ipcUnavailable", ipc, PairState::kIpcUnavailable});

    PairInputs abi = baseInputs();
    abi.abiMismatch = true;
    cases.push_back({"abiMismatch", abi, PairState::kAbiMismatch});

    PairInputs sr = linkedInputs();
    sr.peer.sampleRate = 44100;
    cases.push_back({"srMismatch", sr, PairState::kSrMismatch});

    PairInputs conflict = baseInputs();
    conflict.conflict = true;
    cases.push_back({"conflict", conflict, PairState::kConflict});

    PairInputs unassigned = baseInputs();
    unassigned.allGroupsFull = true;
    cases.push_back({"unassigned", unassigned, PairState::kUnassigned});

    PairInputs stale = linkedInputs();
    stale.peer.lost = true;
    cases.push_back({"stale", stale, PairState::kStale});

    PairInputs bypassed = baseInputs();
    bypassed.role = Role::kOutput;
    bypassed.selfBypassed = true;
    cases.push_back({"bypassed", bypassed, PairState::kBypassed});

    PairInputs crossProcess = linkedInputs();
    crossProcess.peer.pid = kOtherPid;
    cases.push_back({"crossProcess", crossProcess, PairState::kCrossProcess});

    PairInputs crossTrack = linkedInputs();
    crossTrack.peer.nameHash = kOtherHash;
    cases.push_back({"crossTrack", crossTrack, PairState::kCrossTrack});

    PairInputs waitingAudio = linkedInputs();
    waitingAudio.realtimeAudioSeen = false;
    cases.push_back({"waitingAudio", waitingAudio, PairState::kWaitingAudio});

    PairInputs searching = baseInputs();
    searching.realtimeAudioSeen = true;
    cases.push_back({"searching", searching, PairState::kSearching});

    PairInputs unverified = linkedInputs();
    unverified.selfNameHash = 0;
    cases.push_back({"connectedUnverified", unverified, PairState::kConnectedUnverified});

    PairInputs connected = linkedInputs();
    cases.push_back({"connected", connected, PairState::kConnected});

    PairInputs still = linkedInputs();
    still.loopSilent = true;
    cases.push_back({"connectedStill", still, PairState::kConnectedStill});

    return cases;
}

} // namespace

TEST_CASE("each of the fifteen inputs hits its own state", "[pairing][model]")
{
    const std::vector<StateCase> cases = stateCases();
    REQUIRE(cases.size() == static_cast<std::size_t>(kPairStateCount));
    for (const StateCase& c : cases)
    {
        INFO("state: " << c.name);
        const PairModel model = buildPairModel(c.inputs, kEmptyGroups);
        CHECK(model.state == c.expected);
        CHECK(model.colour == colourOf(c.expected));
        CHECK(model.group == c.inputs.group);
    }
}

TEST_CASE("the state priority is first match wins", "[pairing][model]")
{
    struct PriorityCase
    {
        const char* name;
        PairInputs inputs;
        PairState expected;
    };
    std::vector<PriorityCase> cases;

    PairInputs validator = linkedInputs();
    validator.validator = true;
    validator.ipcUnavailable = true;
    validator.abiMismatch = true;
    cases.push_back({"validator beats ipcUnavailable and abiMismatch", validator, PairState::kValidator});

    PairInputs ipc = linkedInputs();
    ipc.ipcUnavailable = true;
    ipc.abiMismatch = true;
    cases.push_back({"ipcUnavailable beats abiMismatch", ipc, PairState::kIpcUnavailable});

    PairInputs abi = linkedInputs();
    abi.abiMismatch = true;
    abi.peer.sampleRate = 44100;
    cases.push_back({"abiMismatch beats srMismatch", abi, PairState::kAbiMismatch});

    PairInputs sr = linkedInputs();
    sr.peer.sampleRate = 44100;
    sr.conflict = true;
    cases.push_back({"srMismatch beats conflict", sr, PairState::kSrMismatch});

    PairInputs conflict = linkedInputs();
    conflict.conflict = true;
    conflict.peer.lost = true;
    conflict.peer.pid = kOtherPid;
    cases.push_back({"conflict beats stale and crossProcess", conflict, PairState::kConflict});

    PairInputs conflictFull = linkedInputs();
    conflictFull.conflict = true;
    conflictFull.allGroupsFull = true;
    cases.push_back({"conflict beats unassigned", conflictFull, PairState::kConflict});

    PairInputs unassigned = linkedInputs();
    unassigned.allGroupsFull = true;
    unassigned.peer.lost = true;
    cases.push_back({"unassigned beats stale", unassigned, PairState::kUnassigned});

    PairInputs stale = linkedInputs();
    stale.peer.lost = true;
    stale.peer.pid = kOtherPid;
    cases.push_back({"stale beats crossProcess", stale, PairState::kStale});

    PairInputs staleBypassed = linkedInputs();
    staleBypassed.role = Role::kOutput;
    staleBypassed.selfBypassed = true;
    staleBypassed.peer.lost = true;
    cases.push_back({"stale beats bypassed", staleBypassed, PairState::kStale});

    PairInputs bypassed = linkedInputs();
    bypassed.role = Role::kOutput;
    bypassed.selfBypassed = true;
    bypassed.peer.pid = kOtherPid;
    cases.push_back({"bypassed beats crossProcess", bypassed, PairState::kBypassed});

    PairInputs crossProcess = linkedInputs();
    crossProcess.peer.pid = kOtherPid;
    crossProcess.peer.nameHash = kOtherHash;
    cases.push_back({"crossProcess beats crossTrack", crossProcess, PairState::kCrossProcess});

    PairInputs crossTrack = linkedInputs();
    crossTrack.peer.nameHash = kOtherHash;
    crossTrack.realtimeAudioSeen = false;
    cases.push_back({"crossTrack beats waitingAudio", crossTrack, PairState::kCrossTrack});

    PairInputs waiting = baseInputs();
    waiting.realtimeAudioSeen = false; // 没有对端，但 waitingAudio 排在 searching 之前
    cases.push_back({"waitingAudio beats searching", waiting, PairState::kWaitingAudio});

    for (const PriorityCase& c : cases)
    {
        INFO("priority: " << c.name);
        CHECK(buildPairModel(c.inputs, kEmptyGroups).state == c.expected);
    }
}

TEST_CASE("colours, ids and pulses cover all fifteen states", "[pairing][model]")
{
    constexpr PairState states[kPairStateCount] = {
        PairState::kValidator,
        PairState::kIpcUnavailable,
        PairState::kAbiMismatch,
        PairState::kSrMismatch,
        PairState::kConflict,
        PairState::kUnassigned,
        PairState::kStale,
        PairState::kBypassed,
        PairState::kCrossProcess,
        PairState::kCrossTrack,
        PairState::kWaitingAudio,
        PairState::kSearching,
        PairState::kConnectedUnverified,
        PairState::kConnected,
        PairState::kConnectedStill,
    };
    constexpr PairColour colours[kPairStateCount] = {
        PairColour::kGrey, PairColour::kRed,   PairColour::kRed,   PairColour::kRed,   PairColour::kRed,
        PairColour::kRed,  PairColour::kAmber, PairColour::kGrey,  PairColour::kAmber, PairColour::kAmber,
        PairColour::kGrey, PairColour::kGrey,  PairColour::kGreen, PairColour::kGreen, PairColour::kGreen,
    };
    constexpr const char* ids[kPairStateCount] = {
        "validator",
        "ipcUnavailable",
        "abiMismatch",
        "srMismatch",
        "conflict",
        "unassigned",
        "stale",
        "bypassed",
        "crossProcess",
        "crossTrack",
        "waitingAudio",
        "searching",
        "connectedUnverified",
        "connected",
        "connectedStill",
    };

    int red = 0;
    int amber = 0;
    int grey = 0;
    int green = 0;
    int pulsing = 0;
    std::set<std::string> seen;
    for (int i = 0; i < kPairStateCount; ++i)
    {
        const PairState state = states[i];
        INFO("state: " << ids[i]);
        CHECK(colourOf(state) == colours[i]);
        switch (colourOf(state))
        {
        case PairColour::kRed:
            ++red;
            break;
        case PairColour::kAmber:
            ++amber;
            break;
        case PairColour::kGrey:
            ++grey;
            break;
        case PairColour::kGreen:
            ++green;
            break;
        }
        const std::string id = idOf(state);
        CHECK(!id.empty());
        CHECK(id == ids[i]);
        CHECK(seen.insert(id).second);
        const bool shouldPulse = state == PairState::kConnectedUnverified || state == PairState::kConnected;
        CHECK(pulses(state) == shouldPulse);
        if (pulses(state))
        {
            ++pulsing;
        }
    }
    CHECK(red == 5);
    CHECK(amber == 3);
    CHECK(grey == 4);
    CHECK(green == 3);
    CHECK(pulsing == 2);
    CHECK(seen.size() == static_cast<std::size_t>(kPairStateCount));
}

TEST_CASE("name verification treats an unknown side as unknown", "[pairing][model]")
{
    CHECK(checkNames(0, 0) == NameCheck::kUnknown);
    CHECK(checkNames(0, 7) == NameCheck::kUnknown);
    CHECK(checkNames(7, 0) == NameCheck::kUnknown);
    CHECK(checkNames(7, 7) == NameCheck::kSame);
    CHECK(checkNames(7, 8) == NameCheck::kDifferent);
}

TEST_CASE("formatVersion prints major.minor.patch", "[pairing][model]")
{
    CHECK(formatVersion(0) == "0.0.0");
    CHECK(formatVersion(packVersion(0, 9, 0)) == "0.9.0");
    CHECK(formatVersion(packVersion(1, 2, 3)) == "1.2.3");
    CHECK(formatVersion(packVersion(10, 20, 30)) == "10.20.30");
    CHECK(formatVersion(packVersion(255, 255, 255)) == "255.255.255");
}

TEST_CASE("occupied and peer masks follow the role's own and opposite ends", "[pairing][model]")
{
    PairInputs input = baseInputs();
    input.role = Role::kInput;
    input.group = 3;
    GroupViews inputGroups{};
    inputGroups[0].input = activeSlot(kOtherPid); // bit0：本角色端，另一个组，active 也算占着
    inputGroups[1].output = activeSlot(kSelfPid); // bit1：对端端，同进程
    inputGroups[2].input = dormantSlot(kSelfPid); // 自己组：occupiedMask 排除
    inputGroups[2].output = dormantSlot(kSelfPid); // 自己组：对端是休眠的，不算 peerMask
    inputGroups[3].input = dormantSlot(kSelfPid); // bit3：dormant 也算占着
    inputGroups[4].output = activeSlot(kOtherPid); // 对端在别的进程，不算 peerMask
    inputGroups[7].output = dormantSlot(kSelfPid); // bit7
    const PairModel inputModel = buildPairModel(input, inputGroups);
    CHECK(inputModel.occupiedMask == (bitAt(0) | bitAt(3)));
    // 只有活着的同进程对端、而本角色一端空着才算在等：休眠的对端和已经配好的组都不算
    CHECK(inputModel.peerMask == bitAt(1));

    PairInputs output = baseInputs();
    output.role = Role::kOutput;
    output.group = 2;
    GroupViews outputGroups{};
    outputGroups[0].output = activeSlot(kSelfPid); // bit0：本角色端
    outputGroups[1].output = dormantSlot(kSelfPid); // 自己组：occupiedMask 排除
    outputGroups[1].input = activeSlot(kSelfPid); // 自己组：本角色端非空（休眠），已经配上，不算 peerMask
    outputGroups[2].input = dormantSlot(kOtherPid); // 对端在别的进程
    outputGroups[3].input = dormantSlot(kSelfPid); // bit3
    outputGroups[4].output = dormantSlot(kOtherPid); // bit4
    outputGroups[5].input = activeSlot(kSelfPid); // peerMask bit5：Input 在等，Output 一端空着
    const PairModel outputModel = buildPairModel(output, outputGroups);
    CHECK(outputModel.occupiedMask == (bitAt(0) | bitAt(4)));
    CHECK(outputModel.peerMask == bitAt(5));

    // 冲突：本组的本角色端是别人占着的，位图上要看得出来
    PairInputs conflict = baseInputs();
    conflict.role = Role::kOutput;
    conflict.group = 2;
    conflict.conflict = true;
    GroupViews conflictGroups{};
    conflictGroups[1].output = activeSlot(kOtherPid);
    const PairModel conflictModel = buildPairModel(conflict, conflictGroups);
    REQUIRE(conflictModel.state == PairState::kConflict);
    CHECK(conflictModel.occupiedMask == bitAt(1));
}

TEST_CASE("channel mismatch and peer fields only appear when the peer is present", "[pairing][model]")
{
    PairInputs absent = baseInputs();
    absent.peer.name = "Ghost";
    absent.peer.nameHash = kSelfHash;
    absent.peer.colourArgb = 0xff010203u;
    absent.peer.version = packVersion(9, 9, 9);
    absent.peer.channels = 1;
    const PairModel absentModel = buildPairModel(absent, kEmptyGroups);
    CHECK_FALSE(absentModel.channelMismatch);
    CHECK(absentModel.nameCheck == NameCheck::kUnknown);
    CHECK(absentModel.peerName.empty());
    CHECK(absentModel.peerColourArgb == 0);
    CHECK(absentModel.peerVersion.empty());

    PairInputs present = linkedInputs();
    present.peer.channels = 1;
    const PairModel presentModel = buildPairModel(present, kEmptyGroups);
    CHECK(presentModel.channelMismatch);
    CHECK(presentModel.peerName == "Guitar");
    CHECK(presentModel.peerColourArgb == 0xff336699u);
    CHECK(presentModel.peerVersion == formatVersion(packVersion(4, 5, 6)));
    CHECK(presentModel.selfVersion == formatVersion(packVersion(1, 2, 3)));

    PairInputs zeroSelfChannels = linkedInputs();
    zeroSelfChannels.selfChannels = 0;
    zeroSelfChannels.peer.channels = 1;
    CHECK_FALSE(buildPairModel(zeroSelfChannels, kEmptyGroups).channelMismatch);

    PairInputs zeroPeerChannels = linkedInputs();
    zeroPeerChannels.peer.channels = 0;
    CHECK_FALSE(buildPairModel(zeroPeerChannels, kEmptyGroups).channelMismatch);

    PairInputs equalChannels = linkedInputs();
    CHECK_FALSE(buildPairModel(equalChannels, kEmptyGroups).channelMismatch);
}

TEST_CASE("occupantName is carried only by conflict", "[pairing][model]")
{
    PairInputs conflict = baseInputs();
    conflict.conflict = true;
    conflict.occupantName = "Other Track";
    CHECK(buildPairModel(conflict, kEmptyGroups).occupantName == "Other Track");

    PairInputs other = linkedInputs();
    other.occupantName = "Other Track";
    CHECK(buildPairModel(other, kEmptyGroups).occupantName.empty());
}

TEST_CASE("loopSilent only turns connected into connectedStill", "[pairing][model]")
{
    PairInputs connected = linkedInputs();
    connected.loopSilent = false;
    CHECK(buildPairModel(connected, kEmptyGroups).state == PairState::kConnected);
    connected.loopSilent = true;
    CHECK(buildPairModel(connected, kEmptyGroups).state == PairState::kConnectedStill);

    PairInputs unverified = linkedInputs();
    unverified.selfNameHash = 0; // 名字未知：对端在也只能是 connectedUnverified
    unverified.loopSilent = true;
    CHECK(buildPairModel(unverified, kEmptyGroups).state == PairState::kConnectedUnverified);
    unverified.loopSilent = false;
    CHECK(buildPairModel(unverified, kEmptyGroups).state == PairState::kConnectedUnverified);

    PairInputs unnamedPeer = linkedInputs();
    unnamedPeer.peer.nameHash = 0;
    CHECK(buildPairModel(unnamedPeer, kEmptyGroups).state == PairState::kConnectedUnverified);
}

TEST_CASE("an Input bypassed by the host never shows bypassed", "[pairing][model]")
{
    // Input 没有 bypassed 配对态：旁路由壳并入 loopSilent，已连接时是 connectedStill，没有对端时是 searching，
    // 跨进程、跨轨道的琥珀提示照常
    PairInputs linked = linkedInputs();
    linked.role = Role::kInput;
    linked.selfBypassed = true;
    linked.loopSilent = true;
    CHECK(buildPairModel(linked, GroupViews{}).state == PairState::kConnectedStill);

    PairInputs alone = baseInputs();
    alone.role = Role::kInput;
    alone.selfBypassed = true;
    alone.realtimeAudioSeen = true;
    CHECK(buildPairModel(alone, GroupViews{}).state == PairState::kSearching);

    PairInputs cross = linkedInputs();
    cross.role = Role::kInput;
    cross.selfBypassed = true;
    cross.peer.pid = kOtherPid;
    CHECK(buildPairModel(cross, GroupViews{}).state == PairState::kCrossProcess);
}
