// SPDX-License-Identifier: GPL-3.0-or-later
//
// 智能默认配对的纯函数实现：只看调用方交来的一份会合段快照，不读段、不读时钟、不分配。
// 所有「空」的判断都以本实例为视角：别的进程占着的端点就是占着，自己正占着的那一端在重评时按空算，
// 否则每次重评都会把自己看成对端，永远等不到对方。
#include "ipc/PairingPolicy.h"

#include <cstddef>

namespace rinne::ipc
{

namespace
{

// 本角色一端与相反角色一端：decideOutput 只走 Output 侧，decideInput 只走 Input 侧
const EndView& ownSlot(const GroupView& group, Role role) noexcept
{
    return role == Role::kOutput ? group.output : group.input;
}

const EndView& peerSlot(const GroupView& group, Role role) noexcept
{
    return role == Role::kInput ? group.output : group.input;
}

// 自己正占着的组里，本角色那一端按空算：重评时若还把它当占着，就永远看不出它其实已经在等对端
bool ownEndEmpty(const EndView& end, u32 group, u32 heldGroup) noexcept
{
    return group == heldGroup || end.isFree();
}

// 空组：自己一端（heldGroup 那一端按空算）与另一端都空。heldGroup 满足条件时优先给它，
// 免得重评在两个都空的组之间来回搬；其余情况取编号最小的，保证与扫描顺序无关
u32 pickEmptyGroup(const PairingSelf& self, const GroupViews& groups, Role role) noexcept
{
    const u32 held = self.heldGroup;
    if (held >= 1u && held <= kGroupCount && peerSlot(groups[static_cast<std::size_t>(held - 1)], role).isFree())
    {
        return held;
    }
    for (std::size_t i = 0; i < groups.size(); ++i)
    {
        const u32 group = static_cast<u32>(i) + 1;
        if (group == held)
        {
            continue;
        }
        if (ownSlot(groups[i], role).isFree() && peerSlot(groups[i], role).isFree())
        {
            return group;
        }
    }
    return 0;
}

} // namespace

bool pairingTriggered(const PairingTrigger& t) noexcept
{
    // 五项缺一不可：刚打开的工程、离线渲染、以及用户已经表过态的情形都不该被自动改掉组号
    return t.prepared && t.realtimeBlocks >= kPairingMinBlocks && t.sincePrepareMs >= kPairingMinPrepareMs &&
           !t.hasSavedGroup && !t.userPicked;
}

PairingDecision decideOutput(const PairingSelf& self, const GroupViews& groups) noexcept
{
    u32 sameNameGroup = 0;
    u32 sameNameCount = 0;
    u32 latestGroup = 0;
    u64 latestClaimMs = 0;
    for (std::size_t i = 0; i < groups.size(); ++i)
    {
        const u32 group = static_cast<u32>(i) + 1;
        const GroupView& view = groups[i];
        if (!view.input.isLive() || view.input.pid != self.pid || !ownEndEmpty(view.output, group, self.heldGroup))
        {
            continue;
        }
        // 名字未知（哈希 0）不参与同名匹配；两端都已知且相等才算
        if (self.nameHash != 0 && view.input.nameHash == self.nameHash)
        {
            ++sameNameCount;
            sameNameGroup = group;
        }
        // 只有严格更大才换：并列保持先扫到的小组，结果与扫描顺序无关
        if (latestGroup == 0 || view.input.claimMs > latestClaimMs)
        {
            latestGroup = group;
            latestClaimMs = view.input.claimMs;
        }
    }
    // 同名恰一个才敢信；零个（没有名字可用）或多个（复制轨道）都退回最近认领，交给人去确认
    if (sameNameCount == 1)
    {
        return {sameNameGroup, PairingReason::kSameNameInput, false};
    }
    if (latestGroup != 0)
    {
        return {latestGroup, PairingReason::kLatestInput, false};
    }
    const u32 empty = pickEmptyGroup(self, groups, Role::kOutput);
    if (empty != 0)
    {
        return {empty, PairingReason::kEmptyGroup, true};
    }
    return {0, PairingReason::kAllFull, false};
}

PairingDecision decideInput(const PairingSelf& self, const GroupViews& groups) noexcept
{
    u32 latestGroup = 0;
    u64 latestClaimMs = 0;
    for (std::size_t i = 0; i < groups.size(); ++i)
    {
        const u32 group = static_cast<u32>(i) + 1;
        const GroupView& view = groups[i];
        // 未配对过（neverPaired）才是在等 Input 的 Output；已经配过的组不抢，避免把别人的环路拆开
        if (!view.output.isLive() || view.output.pid != self.pid || !view.output.neverPaired ||
            !ownEndEmpty(view.input, group, self.heldGroup))
        {
            continue;
        }
        if (latestGroup == 0 || view.output.claimMs > latestClaimMs)
        {
            latestGroup = group;
            latestClaimMs = view.output.claimMs;
        }
    }
    if (latestGroup != 0)
    {
        return {latestGroup, PairingReason::kWaitingOutput, false};
    }
    // Input 进空组不需要重评：空组里没有对端可等，等轮到 Output 认领时再匹配
    const u32 empty = pickEmptyGroup(self, groups, Role::kInput);
    if (empty != 0)
    {
        return {empty, PairingReason::kEmptyGroup, false};
    }
    return {0, PairingReason::kAllFull, false};
}

RepairPlan repairInput(const PairingSelf& self, const GroupViews& groups) noexcept
{
    // 第一优先：两端真空的组。heldGroup 那一端不再按空算，因为修复就是要离开它
    for (std::size_t i = 0; i < groups.size(); ++i)
    {
        if (groups[i].input.isFree() && groups[i].output.isFree())
        {
            return {static_cast<u32>(i) + 1, false};
        }
    }
    // 退而求其次：本进程的休眠 Input 槽，且对面不是活的（空或休眠）。接管休眠要用户确认
    for (std::size_t i = 0; i < groups.size(); ++i)
    {
        const GroupView& view = groups[i];
        if (view.input.isDormant() && view.input.pid == self.pid && !view.output.isLive())
        {
            return {static_cast<u32>(i) + 1, true};
        }
    }
    return {0, false};
}

RepairPlan repairOutput(const PairingSelf& self, const GroupViews& groups) noexcept
{
    // 修复要离开当前组：重选时不能把自己那一端当成空的，否则可能又选回自己
    PairingSelf unheld = self;
    unheld.heldGroup = 0;
    const PairingDecision decision = decideOutput(unheld, groups);
    if (decision.group != 0)
    {
        return {decision.group, false};
    }
    // 没有可认领的组：本进程的休眠 Output 槽需要用户确认才能接管
    for (std::size_t i = 0; i < groups.size(); ++i)
    {
        const GroupView& view = groups[i];
        if (view.output.isDormant() && view.output.pid == self.pid)
        {
            return {static_cast<u32>(i) + 1, true};
        }
    }
    return {0, false};
}

} // namespace rinne::ipc
