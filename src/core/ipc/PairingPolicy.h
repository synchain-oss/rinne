// SPDX-License-Identifier: GPL-3.0-or-later
//
// 智能默认配对：一组纯函数，只看调用方交来的一份会合段快照，不读段、不读时钟、不分配。壳的 25 Hz 定时器在
// 触发条件满足时用观察者的样本和标签拼出快照，调用这里拿到组号，再经 Rendezvous::setGroup(group, kSmart) 去认领。
//
// 规则：
// - 触发：首次 prepare 之后，累计 >= 8 个实时块，距 prepare >= 300 ms；没有存档组号；本会话没有手选过组。
// - 作用域：只看与本实例同 pid 的对端；别的进程占着的端点算占着，不参与匹配，也不当空位。
// - 新 Output：同名轨上唯一的未配对 Input（两端名字都已知、哈希相等、恰好一个）；否则最近认领的未配对 Input；
//   否则编号最小的空组，并进入等待，壳每 kPairingReevalMs 重评一次。
// - 新 Input：先加入有未配对 Output 在等待的组（最近认领的优先）；否则编号最小的空组。
// - 8 组都占着：没有组（kAllFull），界面显示 unassigned。
// - 手选之后永不自动迁组；复制轨道造成的冲突也不自动迁组，只给一键修复（repairInput / repairOutput）。
// 并列时一律取编号小的组，保证结果与扫描顺序无关。
#pragma once

#include <array>
#include <cstdint>

#include "ipc/LoopBinding.h"
#include "ipc/SegmentLayout.h"

namespace rinne::ipc
{

inline constexpr u32 kPairingMinBlocks = 8;
inline constexpr u64 kPairingMinPrepareMs = 300;
inline constexpr u64 kPairingReevalMs = 1000;

// 触发条件的各项，由壳填
struct PairingTrigger
{
    bool prepared = false; // 已经 prepare 过
    u32 realtimeBlocks = 0; // prepare 以来累计的实时块数（离线渲染的块不算）
    u64 sincePrepareMs = 0; // 距首次 prepare 的毫秒数
    bool hasSavedGroup = false; // 工程 state 里带了组号
    bool userPicked = false; // 本会话里用户手选过组
};

// 五项条件同时满足才做一次决定
bool pairingTriggered(const PairingTrigger& t) noexcept;

// 一个端点槽的快照。phase 取 SegmentLayout.h 的 kCtlPhase*；fresh 表示心跳新鲜、没有被判陈旧
struct EndView
{
    u32 phase = kCtlPhaseFree;
    bool fresh = false;
    u32 pid = 0;
    u64 claimMs = 0;
    u32 nameHash = 0; // 0 = 名字未知
    bool neverPaired = false; // cfg_flags 的 kCfgNeverPaired

    bool isFree() const noexcept { return phase == kCtlPhaseFree; }
    bool isLive() const noexcept { return phase == kCtlPhaseActive && fresh; }
    bool isDormant() const noexcept { return phase == kCtlPhaseDormant; }
};

struct GroupView
{
    EndView input;
    EndView output;
};

using GroupViews = std::array<GroupView, kGroupCount>; // 下标 0 = 组 1

// 本实例自己的信息。heldGroup 是自己已经占着的组号（1..8，0 = 没有）：重评时它那一端按空算。
// decideOutput / decideInput 只在 Rendezvous 持有槽时才填它；冲突里本实例并不占着请求的那一组（槽是别人的），
// 填 0。快照里只有 pid 可核对：heldGroup 那一组里本角色一端若是别的进程占着的，按没填处理；同进程里别的实例
// （复制轨道）与自己分不开，靠调用方守这条前提。repairInput / repairOutput 只拿它当「要离开的组」排除，
// 冲突里照填请求的组
struct PairingSelf
{
    Role role = Role::kInput;
    u32 pid = 0;
    u32 nameHash = 0; // 0 = 名字未知
    u32 heldGroup = 0;
};

enum class PairingReason : std::uint8_t
{
    kSameNameInput, // Output：同名轨上唯一的未配对 Input
    kLatestInput, // Output：最近认领的未配对 Input
    kWaitingOutput, // Input：有未配对 Output 在等的组
    kEmptyGroup, // 编号最小的空组（Output 在这里等待）
    kAllFull, // 没有可去的组
};

struct PairingDecision
{
    u32 group = 0; // 1..8；0 = 没有组（kAllFull）
    PairingReason reason = PairingReason::kAllFull;
    // Output 进了空组等对端：壳每 kPairingReevalMs 用 decideOutput 重评，拿到非 waiting 的决定（含「留在原组」）就停止
    bool waiting = false;
    // 智能认领一律带 provisional 标记（Rendezvous 的 kSmart），可被工程 state 的恢复抢占
};

PairingDecision decideOutput(const PairingSelf& self, const GroupViews& groups) noexcept;
PairingDecision decideInput(const PairingSelf& self, const GroupViews& groups) noexcept;

// 一键修复的结果。needsConfirm = 目标组里本角色的槽是休眠的（接管休眠需要用户确认，对应 kManualConfirmed）
struct RepairPlan
{
    u32 group = 0; // 0 = 没有可去的组
    bool needsConfirm = false;
};

// Input「移到空闲组」：编号最小的两端全空的组；没有时退到本角色一端是同进程休眠槽的最小组（需确认）。
// 对面的 Output 端要么空、要么也是本进程的且不是活的：不能接管之后配到别的进程的 Output 上
RepairPlan repairInput(const PairingSelf& self, const GroupViews& groups) noexcept;
// Output「重新配对」：按 decideOutput 重选；选出空组以外的结果同 decideOutput。没有任何可去的组时，
// 退到本角色一端是同进程休眠槽的最小组（需确认）。对面的 Input 端要么空、要么也是本进程的：
// 不能接管之后配到别的进程的 Input 上
RepairPlan repairOutput(const PairingSelf& self, const GroupViews& groups) noexcept;

} // namespace rinne::ipc
