// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Rinne Output 的桥名常量表：界面与原生之间的函数名、事件名与配对状态名在 C++ 侧的唯一真源。
// Output 只做分组与配对，没有参数，所以不设 kParam 组；函数与事件是 Input 那份的共用部分。
// docs/UI_BRIDGE.md（output 与 common 两节）、web/shared/bridge.js 的名表和 web-preview 的 mock 与本表逐名相等，
// scripts/check-bridge-parity.mjs 对拍，任一边少一项都判红。
// 抽取规则：只读下面三组 constexpr 行里的字符串字面量，组由变量名前缀区分（kFn 函数、kEv 事件、kPair 配对状态）。
// 本文件不放桥名以外的字符串常量。
// 契约不冻结：加减名字时本文件、契约文档、bridge.js 与 mock 在同一个提交里改。

namespace rinne::output::bridge
{
// ---- 函数：界面调原生 ----
inline constexpr const char* kFnRequestInitialState = "requestInitialState";
inline constexpr const char* kFnSetLang = "setLang";
inline constexpr const char* kFnSetUiScale = "setUiScale";
inline constexpr const char* kFnCommitUiScale = "commitUiScale";
inline constexpr const char* kFnSetRulesAck = "setRulesAck";
inline constexpr const char* kFnSetGuideSeen = "setGuideSeen";
inline constexpr const char* kFnSetGroup = "setGroup";
inline constexpr const char* kFnCopyDiagnostics = "copyDiagnostics";
inline constexpr const char* kFnOpenExternal = "openExternal";
inline constexpr const char* kFnSetDiagLog = "setDiagLog";
inline constexpr const char* kFnResolveTakeover = "resolveTakeover";
inline constexpr const char* kFnShowLicense = "showLicense";
// ---- 函数：只在 Output ----
inline constexpr const char* kFnRepairPair = "repairPair";

// ---- 事件：原生推给界面 ----
inline constexpr const char* kEvState = "rinne.state";
inline constexpr const char* kEvPair = "rinne.pair";
inline constexpr const char* kEvError = "rinne.error";

// ---- 配对状态：rinne.pair 的 state 字段取值，共 15 个 ----
inline constexpr const char* kPairIpcUnavailable = "ipcUnavailable";
inline constexpr const char* kPairAbiMismatch = "abiMismatch";
inline constexpr const char* kPairSrMismatch = "srMismatch";
inline constexpr const char* kPairConflict = "conflict";
inline constexpr const char* kPairUnassigned = "unassigned";
inline constexpr const char* kPairStale = "stale";
inline constexpr const char* kPairCrossProcess = "crossProcess";
inline constexpr const char* kPairCrossTrack = "crossTrack";
inline constexpr const char* kPairSearching = "searching";
inline constexpr const char* kPairWaitingAudio = "waitingAudio";
inline constexpr const char* kPairValidator = "validator";
inline constexpr const char* kPairBypassed = "bypassed";
inline constexpr const char* kPairConnectedUnverified = "connectedUnverified";
inline constexpr const char* kPairConnected = "connected";
inline constexpr const char* kPairConnectedStill = "connectedStill";
} // namespace rinne::output::bridge
