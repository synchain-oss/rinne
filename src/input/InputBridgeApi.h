// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Rinne Input 的桥名常量表：界面与原生之间的函数名、事件名、参数 ID 与配对状态名在 C++ 侧的唯一真源。
// docs/UI_BRIDGE.md（input 与 common 两节）、web/shared/bridge.js 的名表和 web-preview 的 mock 与本表逐名相等，
// scripts/check-bridge-parity.mjs 对拍，任一边少一项都判红。
// 抽取规则：只读下面四组 constexpr 行里的字符串字面量，组由变量名前缀区分（kFn 函数、kEv 事件、kParam 参数 ID、
// kPair 配对状态）。本文件不放桥名以外的字符串常量。
// 契约不冻结：加减名字时本文件、契约文档、bridge.js 与 mock 在同一个提交里改。

namespace rinne::input::bridge
{
// ---- 函数：界面调原生，两个角色共用的十二个在前 ----
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
// ---- 函数：只在 Input ----
inline constexpr const char* kFnBeginParamGesture = "beginParamGesture";
inline constexpr const char* kFnSetParam = "setParam";
inline constexpr const char* kFnEndParamGesture = "endParamGesture";
inline constexpr const char* kFnSetLimiter = "setLimiter";
inline constexpr const char* kFnPanic = "panic";
inline constexpr const char* kFnResumeLoop = "resumeLoop";
inline constexpr const char* kFnDismissHint = "dismissHint";
inline constexpr const char* kFnLoadPreset = "loadPreset";
inline constexpr const char* kFnSavePreset = "savePreset";
inline constexpr const char* kFnDeletePreset = "deletePreset";
inline constexpr const char* kFnSetGuardOn = "setGuardOn";
inline constexpr const char* kFnSetResetOnPlay = "setResetOnPlay";
inline constexpr const char* kFnSetTailMode = "setTailMode";
inline constexpr const char* kFnMoveToFreeGroup = "moveToFreeGroup";

// ---- 事件：原生推给界面 ----
inline constexpr const char* kEvState = "rinne.state";
inline constexpr const char* kEvPair = "rinne.pair";
inline constexpr const char* kEvError = "rinne.error";
inline constexpr const char* kEvParams = "rinne.params";
inline constexpr const char* kEvMeters = "rinne.meters";
inline constexpr const char* kEvPresets = "rinne.presets";

// ---- 参数 ID：APVTS，ParameterID{id, 1}，共 11 个 ----
inline constexpr const char* kParamFeedback = "feedback";
inline constexpr const char* kParamLoopOn = "loop_on";
inline constexpr const char* kParamMix = "mix";
inline constexpr const char* kParamDelayMode = "delay_mode";
inline constexpr const char* kParamDelayMs = "delay_ms";
inline constexpr const char* kParamDelayNote = "delay_note";
inline constexpr const char* kParamLowCut = "low_cut";
inline constexpr const char* kParamHighCut = "high_cut";
inline constexpr const char* kParamPolarity = "polarity";
inline constexpr const char* kParamLimiter = "limiter";
inline constexpr const char* kParamCeiling = "ceiling";

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
} // namespace rinne::input::bridge
