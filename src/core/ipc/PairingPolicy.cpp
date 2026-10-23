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

// 自己正占着的组里，本角色那一端按空算：重评时若还把它当占着，就永远看不出它其实已经在等对端。
// 前提是那一端确实是自己的：本角色一端空（认领还没被观察到），或者 pid 是本进程的。别的进程占着的端点就是
// 占着，调用方说「自己占着」也不能把它当空位或当已经配上（冲突里 Rendezvous 不持有槽，却仍记着请求的组）。
// 核对不过的 heldGroup 按没填处理，下面的判断一律用核对后的组号。
// 快照里只有 pid 可核对：同进程里别的实例（复制轨道）与自己分不开，所以调用方只在持有槽时才填 heldGroup
u32 verifiedHeldGroup(const PairingSelf& self, const GroupViews& groups) noexcept
{
    const u32 held = self.heldGroup;
    if (held < 1u || held > kGroupCount)
    {
        return 0;
    }
    const EndView& own = ownSlot(groups[static_cast<std::size_t>(held - 1)], self.role);
    return own.isFree() || own.pid == self.pid ? held : 0;
}

// 空组：自己一端（held 那一端按空算）与另一端都空。held 满足条件时优先给它，
// 免得重评在两个都空的组之间来回搬；其余情况取编号最小的，保证与扫描顺序无关
u32 pickEmptyGroup(u32 held, const GroupViews& groups, Role role) noexcept
{
    if (held != 0 && peerSlot(groups[static_cast<std::size_t>(held - 1)], role).isFree())
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
    const u32 held = verifiedHeldGroup(self, groups);
    // 已经配上了：所在组里有同进程的活 Input，就留在原地。重评是给还在等对端的 Output 做的，不能把一个已经工作的
    // 环路拆开，否则结果取决于重评落在别的 Input 认领之前还是之后。
    // 循环里要求 Input 是同进程的活端、本角色一端空着；held 组若满足前一条，已经在这里返回了，所以循环里不会有
    // 「held 组的本角色端按空算」的情形，循环只认 isFree()
    if (held != 0)
    {
        const EndView& heldInput = groups[static_cast<std::size_t>(held - 1)].input;
        if (heldInput.isLive() && heldInput.pid == self.pid)
        {
            const bool sameName = self.nameHash != 0 && heldInput.nameHash == self.nameHash;
            return {held, sameName ? PairingReason::kSameNameInput : PairingReason::kLatestInput, false};
        }
    }
    u32 sameNameGroup = 0;
    u32 sameNameCount = 0;
    u32 latestGroup = 0;
    u64 latestClaimMs = 0;
    for (std::size_t i = 0; i < groups.size(); ++i)
    {
        const u32 group = static_cast<u32>(i) + 1;
        const GroupView& view = groups[i];
        if (!view.input.isLive() || view.input.pid != self.pid || !view.output.isFree())
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
    const u32 empty = pickEmptyGroup(held, groups, Role::kOutput);
    if (empty != 0)
    {
        return {empty, PairingReason::kEmptyGroup, true};
    }
    return {0, PairingReason::kAllFull, false};
}

PairingDecision decideInput(const PairingSelf& self, const GroupViews& groups) noexcept
{
    const u32 held = verifiedHeldGroup(self, groups);
    // 对称地：所在组里已有同进程的活 Output，就留在原地（Input 本身不重评，这里只是不让调用方得到迁走的结果）
    if (held != 0)
    {
        const EndView& heldOutput = groups[static_cast<std::size_t>(held - 1)].output;
        if (heldOutput.isLive() && heldOutput.pid == self.pid)
        {
            return {held, PairingReason::kWaitingOutput, false};
        }
    }
    u32 latestGroup = 0;
    u64 latestClaimMs = 0;
    for (std::size_t i = 0; i < groups.size(); ++i)
    {
        const u32 group = static_cast<u32>(i) + 1;
        const GroupView& view = groups[i];
        // 未配对过（neverPaired）才是在等 Input 的 Output；已经配过的组不抢，避免把别人的环路拆开
        if (!view.output.isLive() || view.output.pid != self.pid || !view.output.neverPaired || !view.input.isFree())
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
    const u32 empty = pickEmptyGroup(held, groups, Role::kInput);
    if (empty != 0)
    {
        return {empty, PairingReason::kEmptyGroup, false};
    }
    return {0, PairingReason::kAllFull, false};
}

RepairPlan repairInput(const PairingSelf& self, const GroupViews& groups) noexcept
{
    // 第一优先：两端真空的组。本实例占着的组不算：修复就是要离开它
    for (std::size_t i = 0; i < groups.size(); ++i)
    {
        const u32 group = static_cast<u32>(i) + 1;
        if (group != self.heldGroup && groups[i].input.isFree() && groups[i].output.isFree())
        {
            return {group, false};
        }
    }
    // 退而求其次：本进程的休眠 Input 槽需要用户确认才能接管
    for (std::size_t i = 0; i < groups.size(); ++i)
    {
        const u32 group = static_cast<u32>(i) + 1;
        const GroupView& view = groups[i];
        // 修复要离开当前组；对面的 Output 端要么空、要么也是本进程的且不是活的：不能接管之后配到别的进程的 Output 上
        if (group != self.heldGroup && view.input.isDormant() && view.input.pid == self.pid &&
            (view.output.isFree() || (view.output.pid == self.pid && !view.output.isLive())))
        {
            return {group, true};
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
        const u32 group = static_cast<u32>(i) + 1;
        const GroupView& view = groups[i];
        // 修复要离开当前组；对面的 Input 端要么空、要么也是本进程的：不能接管之后配到别的进程的 Input 上
        if (group != self.heldGroup && view.output.isDormant() && view.output.pid == self.pid &&
            (view.input.isFree() || view.input.pid == self.pid))
        {
            return {group, true};
        }
    }
    return {0, false};
}

} // namespace rinne::ipc
