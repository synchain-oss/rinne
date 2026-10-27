// SPDX-License-Identifier: GPL-3.0-or-later
//
// 导出不一致锁存的判据与消息线程一侧的「已读」值（口径见 ADR-0002）。
//
// 背景：离线渲染（导出）时宿主可以用任意大的块、任意快的节奏调我们。只要块长不超过声明值、两端按序配合，
// 导出与播放逐位一致；一旦出了下面任何一件事，导出就可能和播放听起来不一样，界面要留一个提示让用户知道。
//
// 判据：在离线块（transport.nonrealtime）里，下面四件事任何一件发生，计一次（一块至多一次）：
//   1. 宿主违约：块长超过 prepare 声明值（n > N_prep），或超过支持上限（n > 32768）；
//   2. 主读头补 0：环里还没写到主读头本该读的样本（Output 晚了，或没跟上）；
//   3. 对端报告违约：对端槽的 kRtOverMax 位在一个新块上为真；
//   4. 一次重同步：除了起播沿与换绑定（域重置、换段）之外的原因。
// 第一次配上之后的对齐只是锚定，不是重同步，不计。起播沿与换绑定是工程正常的操作，也不计。
// 反例：块长不小于最大块长（D ≥ N_max）的干净离线导出，一件都不会发生，计数不动。
//
// 「已读」的机制：音频线程只累计一个计数（LoopStatus::exportEvents）；消息线程自己持有已读值，
// latched = 已见计数 ≠ 已读值。用户确认之后已读值追上当前计数，之后再有新事件又会锁存。
// 计数不随 prepare 清零（已读值在消息线程上，清零会让两边对不上）；比较只用 ≠，回绕无碍。
//
// 实时约定：exportCauses 是纯函数，没有状态；ExportLatchView 只在消息线程上用。
#pragma once

#include <cstdint>

#include "loop/StreamSync.h"

namespace rinne::loop
{

// 一块被计入的原因（位）。LoopStatusFields::exportCauses 是最近一次被计入的那一块的位或。
inline constexpr std::uint32_t kExportCauseViolation = 1u << 0; // 宿主违约（块长超声明值或超支持上限）
inline constexpr std::uint32_t kExportCauseOverrun = 1u << 1; // 主读头补 0
inline constexpr std::uint32_t kExportCausePeerOverMax = 1u << 2; // 对端报告违约
inline constexpr std::uint32_t kExportCauseResync = 1u << 3; // 重同步

// 这个重同步原因算不算导出不一致。起播沿与换绑定（含域重置）是正常操作，不算。
constexpr bool resyncCountsForExport(SyncReason reason) noexcept
{
    switch (reason)
    {
    case SyncReason::kWriter:
    case SyncReason::kFormat:
    case SyncReason::kStallResume:
    case SyncReason::kJump:
    case SyncReason::kShift:
        return true;
    case SyncReason::kBindSwitch:
    case SyncReason::kPlay:
    case SyncReason::kOffset:
    case SyncReason::kDChange:
    case SyncReason::kReasonCount:
        return false;
    }
    return false;
}

// 一块的事实（音频线程每块填一份）。
struct ExportBlockFacts
{
    bool nonrealtime = false; // 本块是离线块
    bool hostViolation = false; // n > N_prep
    bool unsupportedBlock = false; // n > 32768
    std::uint64_t overrun = 0; // 主读头补 0 的样本数
    bool peerOverMax = false; // 对端在一个新块上报告了 kRtOverMax
    bool resynced = false;
    SyncReason resyncReason = SyncReason::kOffset;
};

// 这一块的原因位；0 = 不计。非离线块恒为 0（实时播放里的这些事走各自的提示，不进导出锁存）。
constexpr std::uint32_t exportCauses(const ExportBlockFacts& facts) noexcept
{
    if (!facts.nonrealtime)
    {
        return 0;
    }
    std::uint32_t causes = 0;
    if (facts.hostViolation || facts.unsupportedBlock)
    {
        causes |= kExportCauseViolation;
    }
    if (facts.overrun > 0)
    {
        causes |= kExportCauseOverrun;
    }
    if (facts.peerOverMax)
    {
        causes |= kExportCausePeerOverMax;
    }
    if (facts.resynced && resyncCountsForExport(facts.resyncReason))
    {
        causes |= kExportCauseResync;
    }
    return causes;
}

// 消息线程一侧：已见计数与已读值。不在音频线程上用，没有原子量。
class ExportLatchView
{
public:
    // 每次读到新的状态快照时调：seen 是 LoopStatusFields::exportEvents。
    void update(std::uint32_t seen) noexcept { seen_ = seen; }

    // 锁存中：已见计数与已读值不同。
    bool latched() const noexcept { return seen_ != acked_; }

    // 用户确认：已读值追上当前已见的计数。之后再有新事件会重新锁存。
    void acknowledge() noexcept { acked_ = seen_; }

    std::uint32_t seen() const noexcept { return seen_; }
    std::uint32_t acknowledged() const noexcept { return acked_; }
    // 自上次确认起新增的事件数（无符号回绕无碍）。
    std::uint32_t pending() const noexcept { return seen_ - acked_; }

private:
    std::uint32_t seen_ = 0;
    std::uint32_t acked_ = 0;
};

} // namespace rinne::loop
