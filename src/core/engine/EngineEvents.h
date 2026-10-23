// SPDX-License-Identifier: GPL-3.0-or-later
//
// 引擎层的事件码。
//
// 事件码按模块分段，各模块在自己的头文件里定义，不建全局枚举：
//   0x0200–0x02FF  安全链独占（src/core/engine/SafetyEvents.h）
//   0x0300–0x03FF  环路层独占（src/core/loop/LoopEvents.h）
//   0x0400–0x04FF  引擎独占（本文件）
//   0x0500–0x05FF  探针独占（src/core/diag/ProbeEvents.h）
// 每段 256 个码；探针段从 0x0500 起（0x0300 段已归环路层）。本段内的新码只能追加。
// 四段是跨模块的数值契约：段归属定死，已有的值不改不删（诊断日志与桥都按数值存）。
#pragma once

#include <cstdint>

namespace rinne::engine
{

// 引擎层的提示码：界面按它给提示（琥珀或红字），诊断报告也列它们。
// a、b 各一个字节：「÷256」= 以 256 个样本为单位向上取整、钳到 255（与环路段同一种编码，loop::eventUnits256）。
//
// 目前有发送方的：kUnsupportedBlock（OutputEngine）、kNonRealtime（InputEngine）、kWriteConflict（OutputEngine）。
// 其余是已占的码位，现在没有发送方：违约与 D 下限抬升由环路段的 0x0306 / 0x0305 报，引擎段的 0x0401 / 0x0403
// 不再重复发；码位留着（已有的值不改不删），接上发送方时照这里写的载荷发。
enum class EngineEventCode : std::uint16_t
{
    kUnsupportedBlock = 0x0400, // a = 块长 n ÷256，b = 支持的上限 ÷256（32768 → 128）；只在进入超限时发一条
    kHostViolation = 0x0401, // （没有发送方）a:b = 块长 n 的 16 位值
    kDClamped = 0x0402, // （没有发送方）a = 实际 D ÷256，b = 0
    kDelayRaised = 0x0403, // （没有发送方）a = 新的 D_min ÷256，b = 原因
    kBpmFallback = 0x0404, // （没有发送方）a = 1（没拿到宿主速度，按 120 BPM）
    kDelayModeClamped = 0x0405, // （没有发送方）a = 1（时值对应的延迟超出 4000 ms）
    kNoPair = 0x0406, // （没有发送方）a = 1（没有对端）
    kSampleRateChanged = 0x0407, // （没有发送方）a = 新的采样率 ÷1000（钳 255）
    kNonRealtime = 0x0408, // a = 1（进入离线渲染）
    kWriteConflict = 0x0409, // a = 1（写权不是自己的：不写环，置冲突状态）；只在进入冲突时发一条
};

} // namespace rinne::engine
