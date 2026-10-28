// SPDX-License-Identifier: GPL-3.0-or-later
//
// mock 与桥契约共用的常量：参数表、配对状态、诊断字段白名单、组、语言、外链白名单。
// 这些名字与 docs/UI_BRIDGE.md、src/{input,output}/*BridgeApi.h 逐名相等，scripts/check-bridge-parity.mjs 对拍。
// 数组与对象保持纯字面量，parity 脚本按文本抽取，不执行本文件。
// 模块顶层不碰 window 和 document，Node 里可以直接 import。

/** Input 的 11 个参数 ID，顺序与桥契约文档一致。Output 没有参数。 */
export const PARAM_IDS = [
    "feedback",
    "loop_on",
    "mix",
    "delay_mode",
    "delay_ms",
    "delay_note",
    "low_cut",
    "high_cut",
    "polarity",
    "limiter",
    "ceiling",
];

/**
 * 参数定义。type：float 连续、bool 开关、enum 枚举（values 为取值表）。
 * auto 是宿主能否自动化；mock 只用它在文档里对拍，不影响行为。
 * 范围与默认值的最终冻结归参数文档；这里只是预览用的取值，与 UI_BRIDGE.md 的参数表逐项对拍。
 */
export const PARAM_DEFS = {
    feedback: { type: "float", min: 0, max: 150, def: 0, auto: true },
    loop_on: { type: "bool", def: true, auto: true },
    mix: { type: "float", min: 0, max: 100, def: 25, auto: true },
    delay_mode: { type: "enum", values: ["time", "sync"], def: "time", auto: false },
    delay_ms: { type: "float", min: 1, max: 4000, def: 100, auto: false },
    delay_note: {
        type: "enum",
        values: [
            "n64",
            "n32t",
            "n32",
            "n16t",
            "n16",
            "n8t",
            "n16d",
            "n8",
            "n4t",
            "n8d",
            "n4",
            "n2t",
            "n4d",
            "n2",
            "n2d",
            "n1",
        ],
        def: "n4",
        auto: false,
    },
    low_cut: { type: "float", min: 20, max: 2000, def: 20, auto: true },
    high_cut: { type: "float", min: 1000, max: 20000, def: 20000, auto: true },
    polarity: { type: "bool", def: false, auto: true },
    limiter: { type: "bool", def: true, auto: false },
    ceiling: { type: "float", min: -24, max: 0, step: 0.1, def: -6, auto: false },
};

/** rinne.pair 的 state 字段取值，共 15 个。颜色分组见 docs/UI_BRIDGE.md。 */
export const PAIR_STATES = [
    "ipcUnavailable",
    "abiMismatch",
    "srMismatch",
    "conflict",
    "unassigned",
    "stale",
    "crossProcess",
    "crossTrack",
    "searching",
    "waitingAudio",
    "validator",
    "bypassed",
    "connectedUnverified",
    "connected",
    "connectedStill",
];

/** 配对状态的颜色分组：红（回送已停、需要用户动作）、琥珀（仍在工作但有风险）、灰（主动关闭、宿主暂停或离线）、绿。 */
export const PAIR_COLORS = {
    ipcUnavailable: "red",
    abiMismatch: "red",
    srMismatch: "red",
    conflict: "red",
    unassigned: "red",
    stale: "amber",
    crossProcess: "amber",
    crossTrack: "amber",
    searching: "gray",
    waitingAudio: "gray",
    validator: "gray",
    bypassed: "gray",
    connectedUnverified: "green",
    connected: "green",
    connectedStill: "green",
};

/** 只由 Output 产生的配对状态：Input 的 rinne.pair 里不会出现。Input 被宿主旁路时配对态不变，界面另用提示条表达。 */
export const PAIR_OUTPUT_ONLY = ["bypassed"];

/**
 * 诊断文本（rinne-diag v1）里允许出现的名字，复制前预览与实际复制用同一份，顺序与文本里的出现顺序一致。
 * 名字是「节名.键名」：节名是行首标签（`track:` 的 track），product、backend 两行没有冒号，节名取该行第一个键；
 * 整行的两个名字不带点：rinne-diag（第一行的格式标识）和 events（最近事件段）。末行 ui 段由界面补。
 * 与 core 的生成器（src/core/diag/DiagReport.cpp）逐名相等；文本的形态见 docs/UI_BRIDGE.md 的诊断一节。
 */
export const DIAG_KEYS = [
    "rinne-diag",
    "product.product",
    "product.version",
    "product.build",
    "product.format",
    "product.host",
    "product.os",
    "product.arch",
    "backend.backend",
    "backend.ipc_abi",
    "backend.prefix",
    "backend.group",
    "backend.origin",
    "backend.pid",
    "backend.pair",
    "track.relation",
    "track.hash",
    "peer.role",
    "peer.state",
    "peer.same_pid",
    "peer.version",
    "peer.sr",
    "peer.max_block",
    "peer.ch",
    "peer.name",
    "peer.hb_age_ms",
    "prepare.count",
    "prepare.sr",
    "prepare.max",
    "prepare.nonrealtime_seen",
    "prepare.setstate_before_prepare",
    "prepare.reset_calls",
    "blocks.total",
    "blocks.zero_len",
    "blocks.over_max",
    "blocks.bypassed",
    "blocks.max_n_seen",
    "nsamples.min",
    "nsamples.max",
    "nsamples.hist",
    "loop.d_user_ms",
    "loop.d_eff",
    "loop.d_min",
    "loop.mode",
    "loop.bpm",
    "loop.bpm_src",
    "loop.taps_active",
    "loop.tap_drop",
    "sync.resync",
    "sync.overflow",
    "sync.torn",
    "sync.overrun",
    "sync.w_cas_fail",
    "sync.unstable",
    "sync.off_drift_blocks",
    "sync.frozen_max",
    "order.verdict",
    "order.votes",
    "order.host_ns",
    "safety.state",
    "safety.trips",
    "safety.tail_peak_max_db",
    "safety.limiter_gr_max_db",
    "safety.nonfinite_ring",
    "safety.clamp_ring",
    "events",
    "ui.webview",
    "ui.scale",
    "ui.lang",
];

/** 只有 Input 输出的诊断节；Output 的诊断文本是其余节的子集。 */
export const DIAG_INPUT_ONLY_SECTIONS = ["loop", "sync", "order", "safety"];

/** 组 A–H。 */
export const GROUPS = ["A", "B", "C", "D", "E", "F", "G", "H"];

/** 界面语言。 */
export const LANG_CODES = ["zh", "en", "fr"];

/** 切断原因，rinne.state 的 safety.cut 取值（null 表示未切断）。 */
export const CUT_REASONS = ["panic", "nan", "huge", "runaway"];

/** 恢复回送的冷却，毫秒。 */
export const RESUME_COOLDOWN_MS = 1500;

/** 预设名长度上限（字符数）。 */
export const PRESET_NAME_MAX = 24;

/** 8 个工厂预设：id、三语名、改动的参数。不含 loop_on、组号、limiter 和 ceiling。 */
export const FACTORY_PRESETS = [
    { id: "init", name: { zh: "初始化", en: "Init", fr: "Initialiser" }, values: {} },
    {
        id: "slapback",
        name: { zh: "拍击回声 90 ms", en: "Slapback 90 ms", fr: "Slapback 90 ms" },
        values: { delay_ms: 90, feedback: 20, mix: 25 },
    },
    {
        id: "roundDub",
        name: { zh: "回旋配音 1/4D", en: "Round dub 1/4D", fr: "Dub rotatif 1/4D" },
        values: { delay_mode: "sync", delay_note: "n4d", feedback: 60, mix: 50 },
    },
    {
        id: "pingPong",
        name: { zh: "乒乓 1/8", en: "Ping-pong 1/8", fr: "Ping-pong 1/8" },
        values: { delay_mode: "sync", delay_note: "n8", feedback: 45, mix: 25 },
    },
    {
        id: "shortestComb",
        name: { zh: "最短梳状", en: "Shortest comb", fr: "Peigne minimal" },
        values: { delay_ms: 1, feedback: 70, mix: 50 },
    },
    {
        id: "longHold",
        name: { zh: "长保持 92%", en: "Long hold 92%", fr: "Maintien long 92 %" },
        values: { feedback: 92, mix: 50 },
    },
    {
        id: "edgeOsc",
        name: { zh: "振荡边缘 95%", en: "Edge of oscillation 95%", fr: "Bord d’oscillation 95 %" },
        values: { feedback: 95, mix: 50 },
    },
    {
        id: "lightMix",
        name: { zh: "轻混合 30%", en: "Light mix 30%", fr: "Mixage léger 30 %" },
        values: { feedback: 75, mix: 30 },
    },
];
