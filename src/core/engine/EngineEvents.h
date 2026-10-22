// SPDX-License-Identifier: GPL-3.0-or-later
//
// 引擎层的事件码。
//
// 事件码按模块分段，各模块在自己的头文件里定义，不建全局枚举：
//   0x0200–0x02FF  安全链独占（src/core/engine/SafetyEvents.h）
//   0x0300–0x03FF  环路层独占（src/core/loop/LoopEvents.h）
//   0x0400–0x04FF  引擎独占（本文件）
//   0x0500–0x05FF  探针独占（src/core/diag/ProbeEvents.h）
// 本段内的新码只能追加。四段是跨卡的数值契约：已有的值不改不删（诊断日志与桥都按数值存）。
#pragma once

#include <cstdint>

namespace rinne::engine
{

// 引擎层的提示码：界面按它给提示（琥珀或红字），诊断报告也列它们。
enum class EngineEventCode : std::uint16_t
{
    kUnsupportedBlock = 0x0400, // a = 块长 n，b = 支持的上限
    kHostViolation = 0x0401,    // a = 块长 n，b = 声明值 N
    kDClamped = 0x0402,         // a = 实际 D（样本），b = 0
    kDelayRaised = 0x0403,      // a = 新的 D_min（样本），b = 原因
    kBpmFallback = 0x0404,      // a = 1（没拿到宿主速度，按 120 BPM）
    kDelayModeClamped = 0x0405, // a = 1（时值对应的延迟超出 4000 ms）
    kNoPair = 0x0406,           // a = 1
    kSampleRateChanged = 0x0407, // a = 新的采样率
    kNonRealtime = 0x0408,      // a = 1（进入离线渲染）
};

} // namespace rinne::engine
