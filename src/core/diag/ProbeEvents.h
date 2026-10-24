// SPDX-License-Identifier: GPL-3.0-or-later
//
// 探针的事件码与事件名表。
//
// 事件码按模块分段，各模块在自己的头文件里定义，不建全局枚举：
//   0x0200–0x02FF  安全链独占（src/core/engine/SafetyEvents.h）
//   0x0300–0x03FF  环路层独占（src/core/loop/LoopEvents.h）
//   0x0400–0x04FF  引擎独占（src/core/engine/EngineEvents.h）
//   0x0500–0x05FF  探针独占（本文件）
// 本段内的新码只能追加；已有的值不改不删（日志里按名字存，码只在进程内流转，但追加的纪律一样）。
//
// 事件名是探针日志格式的一部分：analyze-probe.py 只认下面这些名字，写出表外的名字整次分析作废（退出码 2）。
// 名字与顺序同 scripts/probe/LOG-FORMAT.md 的「事件」表逐字相等，tests/core/probe 逐项对拍。
// 每个事件的两个参数（日志里的 a 与 b）各有一个类型，决定 ProbeLog 怎么把数值写成文本。
#pragma once

#include <cstddef>
#include <cstdint>

namespace rinne::diag
{

inline constexpr std::uint16_t kProbeEventSegmentFirst = 0x0500;
inline constexpr std::uint16_t kProbeEventSegmentLast = 0x05FF;

enum class ProbeEventCode : std::uint16_t
{
    kPrepare = 0x0500,
    kRelease = 0x0501,
    kReset = 0x0502,
    kSetProcessing = 0x0503,
    kOfflineRender = 0x0504,
    kSetState = 0x0505,
    kGetState = 0x0506,
    kFirstBlock = 0x0507,
    kFirstPairedBlock = 0x0508,
    kPaired = 0x0509,
    kTrackProps = 0x050A,
    kBackend = 0x050B,
    kShmDenied = 0x050C,
    kMonitorToggle = 0x050D,
    kArmToggle = 0x050E,
    kProcessMode = 0x050F,
    kPing = 0x0510,
    kDestruct = 0x0511,
};

// 参数怎么写成文本。数值参数一律放在 ProbeEventArgs 的 a、b 两个 u64 里，文本参数（轨道名）放在 text 里。
enum class ProbeArgKind : std::uint8_t
{
    kNone, // 写空串
    kUInt, // 十进制整数（采样率、块长、errno、ping 的样本数）
    kBool, // 0 或 1，非零写 1
    kGroup, // 0..7 写 A..H，其余写 -
    kPid, // 十进制整数，钳到 4294967295
    kText, // 百分号编码的文本，至多 200 字节（见 ProbeLog.h）
    kColour, // 见 probeColourArg：空，或 RRGGBB / RRGGBBAA
    kBackend, // 0 写 win32，1 写 posix
    kMode, // 0 写 realtime，1 写 offline
};

struct ProbeEventInfo
{
    ProbeEventCode code;
    const char* name;
    ProbeArgKind a;
    ProbeArgKind b;
};

inline constexpr ProbeEventInfo kProbeEvents[] = {
    {ProbeEventCode::kPrepare, "prepare", ProbeArgKind::kUInt, ProbeArgKind::kUInt},
    {ProbeEventCode::kRelease, "release", ProbeArgKind::kNone, ProbeArgKind::kNone},
    {ProbeEventCode::kReset, "reset", ProbeArgKind::kNone, ProbeArgKind::kNone},
    {ProbeEventCode::kSetProcessing, "set_processing", ProbeArgKind::kBool, ProbeArgKind::kNone},
    {ProbeEventCode::kOfflineRender, "offline_render", ProbeArgKind::kBool, ProbeArgKind::kNone},
    {ProbeEventCode::kSetState, "set_state", ProbeArgKind::kGroup, ProbeArgKind::kNone},
    {ProbeEventCode::kGetState, "get_state", ProbeArgKind::kNone, ProbeArgKind::kNone},
    {ProbeEventCode::kFirstBlock, "first_block", ProbeArgKind::kNone, ProbeArgKind::kNone},
    {ProbeEventCode::kFirstPairedBlock, "first_paired_block", ProbeArgKind::kNone, ProbeArgKind::kNone},
    {ProbeEventCode::kPaired, "paired", ProbeArgKind::kPid, ProbeArgKind::kGroup},
    {ProbeEventCode::kTrackProps, "track_props", ProbeArgKind::kText, ProbeArgKind::kColour},
    {ProbeEventCode::kBackend, "backend", ProbeArgKind::kBackend, ProbeArgKind::kUInt},
    {ProbeEventCode::kShmDenied, "shm_denied", ProbeArgKind::kNone, ProbeArgKind::kNone},
    {ProbeEventCode::kMonitorToggle, "monitor_toggle", ProbeArgKind::kBool, ProbeArgKind::kNone},
    {ProbeEventCode::kArmToggle, "arm_toggle", ProbeArgKind::kBool, ProbeArgKind::kNone},
    {ProbeEventCode::kProcessMode, "process_mode", ProbeArgKind::kMode, ProbeArgKind::kNone},
    {ProbeEventCode::kPing, "ping", ProbeArgKind::kUInt, ProbeArgKind::kNone},
    {ProbeEventCode::kDestruct, "destruct", ProbeArgKind::kNone, ProbeArgKind::kNone},
};

inline constexpr std::size_t kProbeEventCount = sizeof(kProbeEvents) / sizeof(kProbeEvents[0]);

// 码 -> 表项。不在探针段内、或段内没定义的码返回空指针
constexpr const ProbeEventInfo* probeEventInfo(std::uint16_t code) noexcept
{
    for (const ProbeEventInfo& info : kProbeEvents)
    {
        if (static_cast<std::uint16_t>(info.code) == code)
        {
            return &info;
        }
    }
    return nullptr;
}

constexpr const ProbeEventInfo* probeEventInfo(ProbeEventCode code) noexcept
{
    return probeEventInfo(static_cast<std::uint16_t>(code));
}

// 颜色参数（kColour）放在 b 里：第 33 位表示有颜色，第 32 位表示带 alpha（写 8 位十六进制），低 32 位是值。
// 全 0 就是没有颜色（写空串）。
constexpr std::uint64_t probeColourArg(std::uint32_t rgb) noexcept
{
    return (std::uint64_t{1} << 33) | (rgb & 0xFFFFFFu);
}

constexpr std::uint64_t probeColourArgWithAlpha(std::uint32_t rgba) noexcept
{
    return (std::uint64_t{1} << 33) | (std::uint64_t{1} << 32) | rgba;
}

} // namespace rinne::diag
