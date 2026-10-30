// SPDX-License-Identifier: GPL-3.0-or-later
//
// 智能默认配对的规则表：触发五项、Output 的同名/最近/空组三级回落、Input 先加入等待中的 Output，
// 以及两条一键修复路径。格子用端点快照的小工厂拼出来，未列出的组一律全空；每个格子都带名字，
// 失败时能直接看出是哪条规则坏了。
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ipc/PairingPolicy.h"

namespace
{

using rinne::ipc::decideInput;
using rinne::ipc::decideOutput;
using rinne::ipc::EndView;
using rinne::ipc::GroupView;
using rinne::ipc::GroupViews;
using rinne::ipc::kCtlPhaseActive;
using rinne::ipc::kCtlPhaseDormant;
using rinne::ipc::kGroupCount;
using rinne::ipc::PairingDecision;
using rinne::ipc::PairingReason;
using rinne::ipc::PairingSelf;
using rinne::ipc::PairingTrigger;
using rinne::ipc::pairingTriggered;
using rinne::ipc::repairInput;
using rinne::ipc::repairOutput;
using rinne::ipc::RepairPlan;
using rinne::ipc::Role;
using rinne::ipc::u32;
using rinne::ipc::u64;

constexpr u32 kSelfPid = 7001;
constexpr u32 kOtherPid = 9002;
constexpr u32 kSelfHash = 4242;
constexpr u32 kOtherNameHash = 8484;

EndView freeSlot()
{
    return EndView{};
}

EndView liveSlot(u32 pid, u64 claimMs, u32 nameHash = 0, bool neverPaired = false)
{
    EndView slot;
    slot.phase = kCtlPhaseActive;
    slot.fresh = true;
    slot.pid = pid;
    slot.claimMs = claimMs;
    slot.nameHash = nameHash;
    slot.neverPaired = neverPaired;
    return slot;
}

EndView staleSlot(u32 pid, u64 claimMs, u32 nameHash = 0)
{
    EndView slot = liveSlot(pid, claimMs, nameHash);
    slot.fresh = false; // active 但心跳陈旧：isLive() 为假
    return slot;
}

EndView dormantSlot(u32 pid)
{
    EndView slot;
    slot.phase = kCtlPhaseDormant;
    slot.pid = pid;
    return slot;
}

struct Placement
{
    u32 group = 0;
    EndView input;
    EndView output;
};

GroupViews groupsOf(const std::vector<Placement>& placements)
{
    GroupViews groups{};
    for (const Placement& p : placements)
    {
        groups[static_cast<std::size_t>(p.group - 1)] = GroupView{p.input, p.output};
    }
    return groups;
}

std::vector<Placement> allGroupsOccupied(u32 pid)
{
    std::vector<Placement> placements;
    for (u32 group = 1; group <= kGroupCount; ++group)
    {
        placements.push_back(
            {group, liveSlot(pid, static_cast<u64>(group), 0), liveSlot(pid, 100 + static_cast<u64>(group), 0)});
    }
    return placements;
}

std::vector<Placement> occupiedInputs(u32 pid)
{
    std::vector<Placement> placements;
    for (u32 group = 1; group <= kGroupCount; ++group)
    {
        placements.push_back({group, liveSlot(pid, static_cast<u64>(group), 0), freeSlot()});
    }
    return placements;
}

std::vector<Placement> withDormantOutput(std::vector<Placement> placements, u32 group)
{
    placements[static_cast<std::size_t>(group - 1)].output = dormantSlot(kSelfPid);
    return placements;
}

std::vector<Placement> withInputEnd(std::vector<Placement> placements, u32 group, const EndView& input)
{
    placements[static_cast<std::size_t>(group - 1)].input = input;
    return placements;
}

PairingSelf selfOf(Role role, u32 nameHash = 0, u32 heldGroup = 0)
{
    PairingSelf self;
    self.role = role;
    self.pid = kSelfPid;
    self.nameHash = nameHash;
    self.heldGroup = heldGroup;
    return self;
}

PairingTrigger triggerOf(bool prepared, u32 realtimeBlocks, u64 sincePrepareMs, bool hasSavedGroup, bool userPicked)
{
    PairingTrigger trigger;
    trigger.prepared = prepared;
    trigger.realtimeBlocks = realtimeBlocks;
    trigger.sincePrepareMs = sincePrepareMs;
    trigger.hasSavedGroup = hasSavedGroup;
    trigger.userPicked = userPicked;
    return trigger;
}

struct DecisionCase
{
    const char* name;
    PairingSelf self;
    std::vector<Placement> groups;
    u32 group;
    PairingReason reason;
    bool waiting;
};

void checkDecision(const DecisionCase& c, const PairingDecision& actual)
{
    INFO("case: " << c.name);
    CHECK(actual.group == c.group);
    CHECK(actual.reason == c.reason);
    CHECK(actual.waiting == c.waiting);
}

struct RepairCase
{
    const char* name;
    PairingSelf self;
    std::vector<Placement> groups;
    u32 group;
    bool needsConfirm;
};

void checkRepair(const RepairCase& c, const RepairPlan& actual)
{
    INFO("case: " << c.name);
    CHECK(actual.group == c.group);
    CHECK(actual.needsConfirm == c.needsConfirm);
}

} // namespace

TEST_CASE("the smart default needs every trigger condition", "[ipc][pairing][policy]")
{
    struct TriggerCase
    {
        const char* name;
        PairingTrigger trigger;
        bool expected;
    };
    const std::vector<TriggerCase> cases = {
        {"all five conditions met", triggerOf(true, 8, 300, false, false), true},
        {"not prepared yet", triggerOf(false, 8, 300, false, false), false},
        {"only seven realtime blocks", triggerOf(true, 7, 300, false, false), false},
        {"only 299 ms since prepare", triggerOf(true, 8, 299, false, false), false},
        {"project state already has a group", triggerOf(true, 8, 300, true, false), false},
        {"the user picked a group this session", triggerOf(true, 8, 300, false, true), false},
    };
    for (const TriggerCase& c : cases)
    {
        INFO("case: " << c.name);
        CHECK(pairingTriggered(c.trigger) == c.expected);
    }
}

TEST_CASE("a new Output prefers a unique same-name input, then the latest claim, then an empty group",
          "[ipc][pairing][policy]")
{
    const std::vector<DecisionCase> cases = {
        {"a unique same-name input beats a later differently named one",
         selfOf(Role::kOutput, kSelfHash),
         {{3, liveSlot(kSelfPid, 100, kSelfHash), freeSlot()},
          {6, liveSlot(kSelfPid, 900, kOtherNameHash), freeSlot()}},
         3,
         PairingReason::kSameNameInput,
         false},
        {"two same-name inputs fall back to the latest claim",
         selfOf(Role::kOutput, kSelfHash),
         {{2, liveSlot(kSelfPid, 500, kSelfHash), freeSlot()}, {5, liveSlot(kSelfPid, 900, kSelfHash), freeSlot()}},
         5,
         PairingReason::kLatestInput,
         false},
        {"an unknown self name never matches and falls back to the latest claim",
         selfOf(Role::kOutput, 0),
         {{2, liveSlot(kSelfPid, 100, kSelfHash), freeSlot()},
          {6, liveSlot(kSelfPid, 900, kOtherNameHash), freeSlot()}},
         6,
         PairingReason::kLatestInput,
         false},
        {"a single differently named input is still taken",
         selfOf(Role::kOutput, kSelfHash),
         {{4, liveSlot(kSelfPid, 700, kOtherNameHash), freeSlot()}},
         4,
         PairingReason::kLatestInput,
         false},
        {"the largest claim wins among several inputs",
         selfOf(Role::kOutput, 0),
         {{2, liveSlot(kSelfPid, 100, 0), freeSlot()},
          {5, liveSlot(kSelfPid, 300, 0), freeSlot()},
          {7, liveSlot(kSelfPid, 200, 0), freeSlot()}},
         5,
         PairingReason::kLatestInput,
         false},
        {"an equal claim keeps the smaller group number",
         selfOf(Role::kOutput, 0),
         {{6, liveSlot(kSelfPid, 500, 0), freeSlot()}, {2, liveSlot(kSelfPid, 500, 0), freeSlot()}},
         2,
         PairingReason::kLatestInput,
         false},
        {"an input from another pid is ignored",
         selfOf(Role::kOutput, kSelfHash),
         {{2, liveSlot(kOtherPid, 900, kSelfHash), freeSlot()}, {5, liveSlot(kSelfPid, 100, 0), freeSlot()}},
         5,
         PairingReason::kLatestInput,
         false},
        {"an input whose output end is taken is not a candidate",
         selfOf(Role::kOutput, 0),
         {{2, liveSlot(kSelfPid, 900, 0), liveSlot(kOtherPid, 100, 0)}},
         1,
         PairingReason::kEmptyGroup,
         true},
        {"a dormant input is not a candidate",
         selfOf(Role::kOutput, kSelfHash),
         {{2, dormantSlot(kSelfPid), freeSlot()}},
         1,
         PairingReason::kEmptyGroup,
         true},
        {"an active but stale input is not a candidate",
         selfOf(Role::kOutput, kSelfHash),
         {{2, staleSlot(kSelfPid, 900, kSelfHash), freeSlot()}},
         1,
         PairingReason::kEmptyGroup,
         true},
        {"no candidate waits in the smallest empty group",
         selfOf(Role::kOutput, kSelfHash),
         {},
         1,
         PairingReason::kEmptyGroup,
         true},
        {"a group occupied by another pid is not empty",
         selfOf(Role::kOutput, 0),
         {{1, liveSlot(kOtherPid, 100, 0), freeSlot()}, {2, freeSlot(), liveSlot(kOtherPid, 200, 0)}},
         3,
         PairingReason::kEmptyGroup,
         true},
        {"a held group is preferred while waiting, so the output does not move",
         selfOf(Role::kOutput, 0, 4),
         {{4, freeSlot(), liveSlot(kSelfPid, 100, 0)}},
         4,
         PairingReason::kEmptyGroup,
         true},
        {"a held group yields to a live input that appears elsewhere",
         selfOf(Role::kOutput, 0, 4),
         {{4, freeSlot(), liveSlot(kSelfPid, 100, 0)}, {6, liveSlot(kSelfPid, 200, 0, true), freeSlot()}},
         6,
         PairingReason::kLatestInput,
         false},
        {"an output already paired in its held group does not move to a later claim",
         selfOf(Role::kOutput, 0, 2),
         {{2, liveSlot(kSelfPid, 100, 0), liveSlot(kSelfPid, 150, 0)},
          {5, liveSlot(kSelfPid, 900, 0, true), freeSlot()}},
         2,
         PairingReason::kLatestInput,
         false},
        {"an output already paired in its held group does not move to a unique same-name input elsewhere",
         selfOf(Role::kOutput, kSelfHash, 2),
         {{2, liveSlot(kSelfPid, 100, kOtherNameHash), liveSlot(kSelfPid, 150, kSelfHash)},
          {5, liveSlot(kSelfPid, 900, kSelfHash, true), freeSlot()}},
         2,
         PairingReason::kLatestInput,
         false},
        {"an output paired with a same-name input reports it as the reason",
         selfOf(Role::kOutput, kSelfHash, 2),
         {{2, liveSlot(kSelfPid, 100, kSelfHash), liveSlot(kSelfPid, 150, kSelfHash)}},
         2,
         PairingReason::kSameNameInput,
         false},
        {"all eight groups occupied means no group at all", selfOf(Role::kOutput, 0), allGroupsOccupied(kOtherPid), 0,
         PairingReason::kAllFull, false},
    };
    for (const DecisionCase& c : cases)
    {
        checkDecision(c, decideOutput(c.self, groupsOf(c.groups)));
    }
}

TEST_CASE("a new Input joins a waiting output, then the smallest empty group", "[ipc][pairing][policy]")
{
    const std::vector<DecisionCase> cases = {
        {"an input joins a group whose output never paired",
         selfOf(Role::kInput),
         {{5, freeSlot(), liveSlot(kSelfPid, 500, 0, true)}},
         5,
         PairingReason::kWaitingOutput,
         false},
        {"the latest waiting output wins among several",
         selfOf(Role::kInput),
         {{2, freeSlot(), liveSlot(kSelfPid, 100, 0, true)}, {7, freeSlot(), liveSlot(kSelfPid, 800, 0, true)}},
         7,
         PairingReason::kWaitingOutput,
         false},
        {"an equal claim keeps the smaller group number",
         selfOf(Role::kInput),
         {{6, freeSlot(), liveSlot(kSelfPid, 500, 0, true)}, {3, freeSlot(), liveSlot(kSelfPid, 500, 0, true)}},
         3,
         PairingReason::kWaitingOutput,
         false},
        {"an output that paired before does not count as waiting",
         selfOf(Role::kInput),
         {{5, freeSlot(), liveSlot(kSelfPid, 500, 0, false)}},
         1,
         PairingReason::kEmptyGroup,
         false},
        // 候选之间按最近认领取胜，所以下面两格里配过的那个认领都更晚：去掉 neverPaired 守卫就会选到它；
        // 两格的组号大小相反，守卫的作用不能靠组号顺序碰巧蒙对
        {"a later claim that paired before loses to an older output still waiting in a smaller group",
         selfOf(Role::kInput),
         {{2, freeSlot(), liveSlot(kSelfPid, 100, 0, true)}, {5, freeSlot(), liveSlot(kSelfPid, 900, 0, false)}},
         2,
         PairingReason::kWaitingOutput,
         false},
        {"a later claim that paired before loses to an older output still waiting in a larger group",
         selfOf(Role::kInput),
         {{2, freeSlot(), liveSlot(kSelfPid, 900, 0, false)}, {5, freeSlot(), liveSlot(kSelfPid, 100, 0, true)}},
         5,
         PairingReason::kWaitingOutput,
         false},
        {"no waiting output means the smallest empty group",
         selfOf(Role::kInput),
         {},
         1,
         PairingReason::kEmptyGroup,
         false},
        {"an input already in a held group stays there instead of moving to a smaller one",
         selfOf(Role::kInput, 0, 4),
         {{1, freeSlot(), freeSlot()}, {4, liveSlot(kSelfPid, 100, 0), freeSlot()}},
         4,
         PairingReason::kEmptyGroup,
         false},
        {"an input already beside a live output of its process does not move to another waiting output",
         selfOf(Role::kInput, 0, 4),
         {{4, liveSlot(kSelfPid, 100, 0), liveSlot(kSelfPid, 90, 0, false)},
          {6, freeSlot(), liveSlot(kSelfPid, 900, 0, true)}},
         4,
         PairingReason::kWaitingOutput,
         false},
        {"a waiting output from another pid is ignored",
         selfOf(Role::kInput),
         {{5, freeSlot(), liveSlot(kOtherPid, 500, 0, true)}},
         1,
         PairingReason::kEmptyGroup,
         false},
        {"a waiting output whose input end is taken is not a candidate",
         selfOf(Role::kInput),
         {{5, liveSlot(kOtherPid, 100, 0), liveSlot(kSelfPid, 500, 0, true)}},
         1,
         PairingReason::kEmptyGroup,
         false},
        {"all eight groups occupied means no group at all", selfOf(Role::kInput), allGroupsOccupied(kOtherPid), 0,
         PairingReason::kAllFull, false},
    };
    for (const DecisionCase& c : cases)
    {
        checkDecision(c, decideInput(c.self, groupsOf(c.groups)));
    }
}

// heldGroup 的本角色一端被别的进程占着（冲突里 Rendezvous 不持有槽，却仍记着请求的组）时，那一组不是空位，
// 也不是「已经配上」：别的进程占着的端点就是占着。本角色一端空（认领还没被观察到）或属于本进程时才按自己算
TEST_CASE("a held group whose own end belongs to another process is not an empty or paired group",
          "[ipc][pairing][policy]")
{
    const std::vector<DecisionCase> outputs = {
        {"output: own end taken by another pid, the held group is not a waiting target",
         selfOf(Role::kOutput, 0, 3),
         {{3, freeSlot(), liveSlot(kOtherPid, 100, 0)}},
         1,
         PairingReason::kEmptyGroup,
         true},
        {"output: own end dormant under another pid, the held group is not a waiting target",
         selfOf(Role::kOutput, 0, 3),
         {{3, freeSlot(), dormantSlot(kOtherPid)}},
         1,
         PairingReason::kEmptyGroup,
         true},
        {"output: the smallest group is held but its own end is another pid's, so the next empty group wins",
         selfOf(Role::kOutput, 0, 1),
         {{1, freeSlot(), liveSlot(kOtherPid, 100, 0)}},
         2,
         PairingReason::kEmptyGroup,
         true},
        {"output: a live input of this process beside a foreign output is not already paired",
         selfOf(Role::kOutput, 0, 3),
         {{3, liveSlot(kSelfPid, 100, 0, true), liveSlot(kOtherPid, 50, 0)}},
         1,
         PairingReason::kEmptyGroup,
         true},
        {"output: a foreign own end in the held group yields to a live input elsewhere",
         selfOf(Role::kOutput, 0, 3),
         {{3, liveSlot(kSelfPid, 100, 0, true), liveSlot(kOtherPid, 50, 0)},
          {5, liveSlot(kSelfPid, 200, 0, true), freeSlot()}},
         5,
         PairingReason::kLatestInput,
         false},
        {"output: own end not yet visible (claim in flight) still counts as ours",
         selfOf(Role::kOutput, 0, 3),
         {{3, freeSlot(), freeSlot()}},
         3,
         PairingReason::kEmptyGroup,
         true},
        {"output: own end dormant under this pid still counts as ours",
         selfOf(Role::kOutput, 0, 3),
         {{3, freeSlot(), dormantSlot(kSelfPid)}},
         3,
         PairingReason::kEmptyGroup,
         true},
        {"output: own end stale under this pid still counts as ours",
         selfOf(Role::kOutput, 0, 3),
         {{3, freeSlot(), staleSlot(kSelfPid, 100, 0)}},
         3,
         PairingReason::kEmptyGroup,
         true},
        {"output: a caller in conflict that passes no held group gets the same answer as the foreign-end case",
         selfOf(Role::kOutput, 0, 0),
         {{3, freeSlot(), liveSlot(kOtherPid, 100, 0)}},
         1,
         PairingReason::kEmptyGroup,
         true},
    };
    for (const DecisionCase& c : outputs)
    {
        checkDecision(c, decideOutput(c.self, groupsOf(c.groups)));
    }
    const std::vector<DecisionCase> inputs = {
        {"input: own end taken by another pid, the held group is not an empty group",
         selfOf(Role::kInput, 0, 3),
         {{3, liveSlot(kOtherPid, 100, 0), freeSlot()}},
         1,
         PairingReason::kEmptyGroup,
         false},
        {"input: own end dormant under another pid, the held group is not an empty group",
         selfOf(Role::kInput, 0, 3),
         {{3, dormantSlot(kOtherPid), freeSlot()}},
         1,
         PairingReason::kEmptyGroup,
         false},
        {"input: a waiting output of this process beside a foreign input is not already paired",
         selfOf(Role::kInput, 0, 3),
         {{3, liveSlot(kOtherPid, 100, 0), liveSlot(kSelfPid, 50, 0, true)}},
         1,
         PairingReason::kEmptyGroup,
         false},
        {"input: a foreign own end in the held group yields to a waiting output elsewhere",
         selfOf(Role::kInput, 0, 3),
         {{3, liveSlot(kOtherPid, 100, 0), liveSlot(kSelfPid, 50, 0, true)},
          {5, freeSlot(), liveSlot(kSelfPid, 300, 0, true)}},
         5,
         PairingReason::kWaitingOutput,
         false},
        {"input: own end not yet visible (claim in flight) still counts as ours",
         selfOf(Role::kInput, 0, 3),
         {{3, freeSlot(), freeSlot()}},
         3,
         PairingReason::kEmptyGroup,
         false},
        {"input: own end dormant under this pid still counts as ours",
         selfOf(Role::kInput, 0, 3),
         {{3, dormantSlot(kSelfPid), freeSlot()}},
         3,
         PairingReason::kEmptyGroup,
         false},
    };
    for (const DecisionCase& c : inputs)
    {
        checkDecision(c, decideInput(c.self, groupsOf(c.groups)));
    }
}

TEST_CASE("repair plans move to a free group or ask before taking a dormant slot", "[ipc][pairing][policy]")
{
    const std::vector<RepairCase> cases = {
        {"repairInput takes the smallest group with both ends free",
         selfOf(Role::kInput),
         {{1, liveSlot(kOtherPid, 10, 0), freeSlot()}},
         2,
         false},
        {"repairInput never treats its held group as empty",
         selfOf(Role::kInput, 0, 2),
         {{1, liveSlot(kOtherPid, 10, 0), freeSlot()},
          {2, liveSlot(kSelfPid, 100, 0), freeSlot()},
          {3, freeSlot(), freeSlot()}},
         3,
         false},
        {"repairInput falls back to a dormant input of the same process, with confirmation",
         selfOf(Role::kInput),
         {{1, freeSlot(), liveSlot(kOtherPid, 101, 0)},
          {2, dormantSlot(kSelfPid), freeSlot()},
          {3, freeSlot(), liveSlot(kOtherPid, 103, 0)},
          {4, freeSlot(), liveSlot(kOtherPid, 104, 0)},
          {5, freeSlot(), liveSlot(kOtherPid, 105, 0)},
          {6, freeSlot(), liveSlot(kOtherPid, 106, 0)},
          {7, freeSlot(), liveSlot(kOtherPid, 107, 0)},
          {8, freeSlot(), liveSlot(kOtherPid, 108, 0)}},
         2,
         true},
        {"repairInput will not take a dormant input whose output belongs to another process",
         selfOf(Role::kInput),
         {{1, dormantSlot(kSelfPid), dormantSlot(kOtherPid)},
          {2, liveSlot(kOtherPid, 10, 0), liveSlot(kOtherPid, 11, 0)},
          {3, liveSlot(kOtherPid, 10, 0), liveSlot(kOtherPid, 11, 0)},
          {4, liveSlot(kOtherPid, 10, 0), liveSlot(kOtherPid, 11, 0)},
          {5, liveSlot(kOtherPid, 10, 0), liveSlot(kOtherPid, 11, 0)},
          {6, liveSlot(kOtherPid, 10, 0), liveSlot(kOtherPid, 11, 0)},
          {7, liveSlot(kOtherPid, 10, 0), liveSlot(kOtherPid, 11, 0)},
          {8, liveSlot(kOtherPid, 10, 0), liveSlot(kOtherPid, 11, 0)}},
         0,
         false},
        {"repairInput will not take a dormant input whose output is another process with a stale heartbeat",
         selfOf(Role::kInput),
         {{1, dormantSlot(kSelfPid), staleSlot(kOtherPid, 12, 0)},
          {2, liveSlot(kOtherPid, 10, 0), liveSlot(kOtherPid, 11, 0)},
          {3, liveSlot(kOtherPid, 10, 0), liveSlot(kOtherPid, 11, 0)},
          {4, liveSlot(kOtherPid, 10, 0), liveSlot(kOtherPid, 11, 0)},
          {5, liveSlot(kOtherPid, 10, 0), liveSlot(kOtherPid, 11, 0)},
          {6, liveSlot(kOtherPid, 10, 0), liveSlot(kOtherPid, 11, 0)},
          {7, liveSlot(kOtherPid, 10, 0), liveSlot(kOtherPid, 11, 0)},
          {8, liveSlot(kOtherPid, 10, 0), liveSlot(kOtherPid, 11, 0)}},
         0,
         false},
        {"repairInput takes a dormant input whose output is this process but not live",
         selfOf(Role::kInput),
         {{1, dormantSlot(kSelfPid), staleSlot(kSelfPid, 12, 0)},
          {2, liveSlot(kOtherPid, 10, 0), liveSlot(kOtherPid, 11, 0)},
          {3, liveSlot(kOtherPid, 10, 0), liveSlot(kOtherPid, 11, 0)},
          {4, liveSlot(kOtherPid, 10, 0), liveSlot(kOtherPid, 11, 0)},
          {5, liveSlot(kOtherPid, 10, 0), liveSlot(kOtherPid, 11, 0)},
          {6, liveSlot(kOtherPid, 10, 0), liveSlot(kOtherPid, 11, 0)},
          {7, liveSlot(kOtherPid, 10, 0), liveSlot(kOtherPid, 11, 0)},
          {8, liveSlot(kOtherPid, 10, 0), liveSlot(kOtherPid, 11, 0)}},
         1,
         true},
        {"repairInput will not take a dormant input whose output is this process but live",
         selfOf(Role::kInput),
         {{1, dormantSlot(kSelfPid), liveSlot(kSelfPid, 12, 0)},
          {2, liveSlot(kOtherPid, 10, 0), liveSlot(kOtherPid, 11, 0)},
          {3, liveSlot(kOtherPid, 10, 0), liveSlot(kOtherPid, 11, 0)},
          {4, liveSlot(kOtherPid, 10, 0), liveSlot(kOtherPid, 11, 0)},
          {5, liveSlot(kOtherPid, 10, 0), liveSlot(kOtherPid, 11, 0)},
          {6, liveSlot(kOtherPid, 10, 0), liveSlot(kOtherPid, 11, 0)},
          {7, liveSlot(kOtherPid, 10, 0), liveSlot(kOtherPid, 11, 0)},
          {8, liveSlot(kOtherPid, 10, 0), liveSlot(kOtherPid, 11, 0)}},
         0,
         false},
        {"repairOutput never hands back its own held group, even when that output is dormant",
         selfOf(Role::kOutput, 0, 3), withDormantOutput(withInputEnd(occupiedInputs(kOtherPid), 3, freeSlot()), 3), 0,
         false},
        {"repairInput has nowhere to go", selfOf(Role::kInput), allGroupsOccupied(kOtherPid), 0, false},
        {"repairOutput reselects a live input after leaving its held group",
         selfOf(Role::kOutput, 0, 4),
         {{4, freeSlot(), liveSlot(kSelfPid, 100, 0)}, {6, liveSlot(kSelfPid, 200, 0), freeSlot()}},
         6,
         false},
        {"repairOutput may pick an empty group, but not its own held one",
         selfOf(Role::kOutput, 0, 4),
         {{1, liveSlot(kOtherPid, 100, 0), freeSlot()}, {4, freeSlot(), liveSlot(kSelfPid, 200, 0)}},
         2,
         false},
        {"repairOutput falls back to a dormant output of the same process, with confirmation", selfOf(Role::kOutput),
         withDormantOutput(withInputEnd(occupiedInputs(kOtherPid), 3, freeSlot()), 3), 3, true},
        {"repairOutput accepts a dormant output whose input end is of the same process", selfOf(Role::kOutput),
         withDormantOutput(withInputEnd(occupiedInputs(kOtherPid), 3, dormantSlot(kSelfPid)), 3), 3, true},
        {"repairOutput will not take a dormant output whose input belongs to another process", selfOf(Role::kOutput),
         withDormantOutput(occupiedInputs(kOtherPid), 3), 0, false},
        {"repairOutput has nowhere to go", selfOf(Role::kOutput), allGroupsOccupied(kOtherPid), 0, false},
    };
    for (const RepairCase& c : cases)
    {
        if (c.self.role == Role::kInput)
        {
            checkRepair(c, repairInput(c.self, groupsOf(c.groups)));
        }
        else
        {
            checkRepair(c, repairOutput(c.self, groupsOf(c.groups)));
        }
    }
}

TEST_CASE("an equal claim goes to the smallest group number", "[ipc][pairing][policy]")
{
    const PairingSelf self = selfOf(Role::kOutput, 0);
    const GroupViews first =
        groupsOf({{2, liveSlot(kSelfPid, 500, 0), freeSlot()}, {6, liveSlot(kSelfPid, 500, 0), freeSlot()}});
    const GroupViews second =
        groupsOf({{3, liveSlot(kSelfPid, 500, 0), freeSlot()}, {7, liveSlot(kSelfPid, 500, 0), freeSlot()}});
    const PairingDecision a = decideOutput(self, first);
    const PairingDecision b = decideOutput(self, second);
    INFO("an equal claim must resolve to the smallest group");
    CHECK(a.reason == PairingReason::kLatestInput);
    CHECK(b.reason == PairingReason::kLatestInput);
    CHECK(a.group == 2);
    CHECK(b.group == 3);
}

TEST_CASE("repair never hands back the group the instance already holds", "[ipc][pairing][policy]")
{
    PairingSelf self = selfOf(Role::kInput, 0);
    self.heldGroup = 1;
    // 组 1 就是自己占着的组，释放后两端都空：修复也不能把它当目的地
    const GroupViews onlyHeldFree = groupsOf({{2, liveSlot(kSelfPid, 10, 0), liveSlot(kSelfPid, 11, 0)},
                                              {3, liveSlot(kSelfPid, 10, 0), liveSlot(kSelfPid, 11, 0)},
                                              {4, liveSlot(kSelfPid, 10, 0), liveSlot(kSelfPid, 11, 0)},
                                              {5, liveSlot(kSelfPid, 10, 0), liveSlot(kSelfPid, 11, 0)},
                                              {6, liveSlot(kSelfPid, 10, 0), liveSlot(kSelfPid, 11, 0)},
                                              {7, liveSlot(kSelfPid, 10, 0), liveSlot(kSelfPid, 11, 0)},
                                              {8, liveSlot(kSelfPid, 10, 0), liveSlot(kSelfPid, 11, 0)}});
    CHECK(repairInput(self, onlyHeldFree).group == 0);
    self.heldGroup = 0;
    CHECK(repairInput(self, onlyHeldFree).group == 1);
}
