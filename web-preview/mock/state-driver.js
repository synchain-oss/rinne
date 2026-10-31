// SPDX-License-Identifier: GPL-3.0-or-later
//
// 预览会话：解析 ?role=&scenario=&lang=&scale=&fresh=，建出对应角色的 mock 后端，并把场景描述套到后端状态上。
//   ?fresh=1      先清空全局位（localStorage["rinne.uiDefaults"]）与本实例的已关闭提示
//   ?scenario=<id>  场景 ID 取自 scenarios.js；表外的 ID 只告警、按默认态打开
//   ?lang=zh|en|fr  只覆盖本次预览的语言，不写回全局位；给了场景而没给语言时取场景自己的语言
//   ?scale=<档位>   开窗档位，由壳页读取，这里只校验后放进 info
//   ?stale=0 / ?auto=0  分别关掉过期帧与代请求初始状态（排查时序用）
// 场景描述怎么变成桥载荷，见 applyScene：能映射的字段映射，其余原样放进 previewScene.state，页面卡再细化。
// previewScene 挂在 mock 上，只给预览用，不属于桥契约。
// 模块顶层不碰 window，存储与计时器从外面注入。

import { memoryStorage } from "./bridge-common.js";
import { createInputBackend, makeUserPreset } from "./bridge-input.js";
import { createOutputBackend } from "./bridge-output.js";
import { LANG_CODES, PAIR_STATES, PARAM_DEFS, PARAM_IDS } from "./contract.js";
import { sceneById } from "./scenarios.js";

const has = (o, k) => Object.prototype.hasOwnProperty.call(o, k);

/** 把 URLSearchParams、查询串或对象统一成取值函数；只认自有键。 */
function readParams(params) {
    if (params instanceof URLSearchParams) return (k) => params.get(k);
    if (typeof params === "string") {
        const sp = new URLSearchParams(params.replace(/^\?/, ""));
        return (k) => sp.get(k);
    }
    const o = params && typeof params === "object" ? params : {};
    return (k) => (has(o, k) ? String(o[k]) : null);
}

/** 设计稿状态串里的数字：兼容 U+2212 减号；「−∞」与 muted 给 null；取不到数字给 undefined。 */
export function parseNum(text) {
    if (typeof text === "number") return text;
    if (typeof text !== "string") return undefined;
    if (text.includes("∞") || text === "muted") return null;
    const m = /[-−]?\d+(?:\.\d+)?/.exec(text);
    return m ? Number(m[0].replace("−", "-")) : undefined;
}

const NOTE_BY_ID = new Set(PARAM_DEFS.delay_note.values);

function setValue(model, id, v) {
    if (v !== undefined && v !== null && PARAM_IDS.includes(id)) model.values[id] = v;
}

/** 套用一个场景描述里能直接映射的字段到 Input 后端模型（逐字段映射，不补设计稿的底座）。 */
function applyInputState(model, st) {
    if (has(st, "pair") && PAIR_STATES.includes(st.pair)) model.pair.state = st.pair;
    if (has(st, "group")) model.group = st.group;
    if (st.groupAuto) model.groupOrigin = "auto";
    if (has(st, "feedback")) setValue(model, "feedback", parseNum(st.feedback));
    if (has(st, "mix")) setValue(model, "mix", parseNum(st.mix));
    if (st.loopOn === false) model.values.loop_on = false;
    if (st.limiter === false) model.values.limiter = false;
    if (has(st, "D")) setValue(model, "delay_ms", parseNum(st.D));
    if (st.delayMode === "Sync") model.values.delay_mode = "sync";
    if (typeof st.note === "string" && NOTE_BY_ID.has(st.note)) model.values.delay_note = st.note;
    if (has(st, "highCut")) setValue(model, "high_cut", (parseNum(st.highCut) ?? 20) * 1000);
    if (has(st, "lowCut")) setValue(model, "low_cut", (parseNum(st.lowCut) ?? 0.02) * 1000);
    if (typeof st.dBadge === "string") model.delay.reason = st.dBadge === "overrun" ? "overrun" : "raised";
    if (has(st, "min")) model.delay.minMs = parseNum(st.min) ?? model.delay.minMs;
    if (has(st, "maxBlock")) model.delay.maxBlock = parseNum(st.maxBlock) ?? model.delay.maxBlock;
    if (st.confirm === "takeover") model.takeover = true;
    if (st.panic === "cut") {
        const c = st.cause === "manual" ? "panic" : st.cause;
        model.cut = c === "nan" || c === "runaway" || c === "huge" ? c : "panic";
        model.cutAt = st.restoreCooldown ? Date.now() : 0;
    }
    if (has(st, "meters")) {
        for (const k of ["in", "ret", "out"]) if (has(st.meters, k)) model.meters[k] = parseNum(st.meters[k]) ?? null;
    }
    if (has(st, "gr")) model.meters.gr = parseNum(st.gr) ?? 0;
    if (has(st, "lamps")) {
        const l = st.lamps;
        if (l.dc === "blocked") model.meters.lamps.dc = "blocked";
        if (typeof l.nan === "string") {
            model.meters.lamps.nonfinite = l.nan.startsWith("zeroed") ? "zeroed" : l.nan;
            model.meters.lamps.nonfiniteCount = parseNum(l.nan) ?? 0;
        }
        if (typeof l.run === "string") model.meters.lamps.guard = l.run.split(" ")[0];
    }
    if (st.peer && typeof st.peer === "object") {
        model.pair.peer.name = st.peer.name ?? null;
        model.pair.verify = { match: "match", diff: "diff" }[st.peer.verify] || "unknown";
    }
    if (st.vals && typeof st.vals === "object") {
        if (has(st.vals, "a")) model.version = String(st.vals.a);
        if (has(st.vals, "b")) model.pair.peer.version = String(st.vals.b);
        if (has(st.vals, "peer")) model.pair.peer.name = String(st.vals.peer);
        if (has(st.vals, "self")) model.pair.self.name = String(st.vals.self);
    }
    if (typeof st.sr === "string") model.sampleRate = (parseNum(st.sr) ?? 48) * 1000;
    if (st.peerDots) model.pair.groupMask = st.peerDots.reduce((m, g) => m | (1 << "ABCDEFGH".indexOf(g)), 0);
    if (st.preset) model.presetDirty = String(st.preset).endsWith("*");
}

/** 场景的用户预设段：字符串数组按名字造成和 savePreset 同形的条目，参数取此刻的模型值（要在场景底座套完之后调）。 */
function applyUserPresets(model, st) {
    if (!Array.isArray(st.user)) return;
    for (const name of st.user)
        if (typeof name === "string" && name !== "") model.userPresets.push(makeUserPreset(model, name));
}

// ---- Input 场景底座 ----
// scenarios.js 里每个场景只写「与设计稿底座不同的部分」，底座（inBase / playing）不在表里，所以这里按场景 ID 补上；
// 再把新增的桥字段（分量电平、导出原因、静音原因、对端采样率等）从场景描述推出来。只补场景没写的键，写了的不动。

const TRACK_NAME_IN = "Vox Lead";
const TRACK_COLOR_IN = "#2d79c6";
/** 配对状态表达不了、经 notices 带给页面的五条消息。 */
const INPUT_NOTICE_IDS = ["ipcSandbox", "outOfOrder", "unstable", "newerProject", "stateDecodeFailed"];
/** 设计稿里对端在场的配对状态。 */
const PEER_STATES = [
    "connected",
    "connectedUnverified",
    "connectedStill",
    "crossTrack",
    "crossProcess",
    "stale",
    "abiMismatch",
    "srMismatch",
];

/** 设计稿里电平与默认不同的场景（dBFS；null 为无信号）。场景自带 meters 的优先。 */
const SCENE_METERS = {
    "in-stale": { in: -12.4, ret: null, out: -12.4 },
    "in-over100": { ret: -5.2, out: -6.3 },
    "in-guard": { ret: -3.8, out: -5.6 },
    "in-fr-worst": { in: -8.2, ret: null, out: -8.4 },
};

/** 在设计稿里以 playing() 为底的场景；其余以 inBase() 为底。 */
const PLAYING_BASED = new Set([
    "in-playing",
    "in-over100",
    "in-guard",
    "in-limiter-confirm",
    "in-limiter-cap",
    "in-mix-mode",
    "in-loop-off",
    "in-bypassed",
    "in-panic",
    "in-nan-latched",
    "in-ring-nan",
    "in-cut-runaway",
    "in-preset-menu",
    "in-preset-save",
    "in-about",
    "in-scale-confirm",
    "in-en",
]);

const round1 = (x) => Math.round(x * 10) / 10;
/** 同步档 1 拍的毫秒数：预览固定 120 BPM。 */
const BEAT_MS = 500;
const NOTE_BEATS = {
    n64: 1 / 16,
    n32t: 1 / 12,
    n32: 1 / 8,
    n16t: 1 / 6,
    n16: 1 / 4,
    n8t: 1 / 3,
    n16d: 3 / 8,
    n8: 1 / 2,
    n4t: 2 / 3,
    n8d: 3 / 4,
    n4: 1,
    n2t: 4 / 3,
    n4d: 3 / 2,
    n2: 2,
    n2d: 3,
    n1: 4,
};

/**
 * 套用设计稿的场景底座与新增字段。scene.state 是 scenarios.js 里的描述；model 已经被 state-driver 套过一遍。
 * 覆盖层场景（预设面板、关于、缩放确认）按 playing 底座补，细节归页面。
 * @param {object} model
 * @param {{id:string, state:object}} scene
 */
function applyInputBase(model, scene) {
    const st = scene.state;
    const playing = PLAYING_BASED.has(scene.id);

    // 组的来源：场景没写 groupAuto 的都按用户手选算，否则页面会在 10 秒内再提示一条「已自动选组」
    if (!st.groupAuto) model.groupOrigin = "manual";

    // 底座：已连接、对端 Vox Lead、核验一致
    if (!has(st, "pair")) model.pair.state = "connected";
    if (PEER_STATES.includes(model.pair.state)) {
        const unverified = model.pair.state === "connectedUnverified";
        if (!has(st, "peer") && !unverified) {
            model.pair.peer.name = TRACK_NAME_IN;
            model.pair.verify = model.pair.state === "crossTrack" ? "diff" : "match";
        }
        if (model.pair.peer.name) model.pair.peer.color = TRACK_COLOR_IN;
        model.pair.peer.channels = 2;
        model.pair.peer.sampleRate = model.sampleRate;
    }
    model.pair.self.name = model.pair.self.name ?? TRACK_NAME_IN;
    model.pair.self.channels = 2;

    const lv = has(st, "meters") ? st.meters : {};
    if (!has(lv, "in")) model.meters.in = playing ? -12.4 : -18;
    if (!has(lv, "out")) model.meters.out = playing ? -9.1 : -18;
    if (!has(lv, "ret")) model.meters.ret = playing ? -14.8 : null;
    if (playing) {
        if (!has(st, "mix")) model.values.mix = 25;
        if (!has(st, "feedback")) model.values.feedback = 72;
        if (!has(st, "highCut")) model.values.high_cut = 6000;
        if (!has(st, "gr")) model.meters.gr = model.values.loop_on === false ? 0 : -2.1;
        model.presetId = "slapback";
        model.presetDirty = true;
    }
    if (st.panic === "cut") {
        // 切断：回送静音，输出保留干声
        if (!has(lv, "ret")) model.meters.ret = null;
        if (!has(lv, "out")) model.meters.out = -12.6;
        if (!has(st, "gr")) model.meters.gr = 0;
    }
    if (has(st, "gr")) model.meters.gr = parseNum(st.gr) ?? 0;
    for (const [k, v] of Object.entries(SCENE_METERS[scene.id] || {})) if (!has(lv, k)) model.meters[k] = v;
    if (scene.id === "in-fr-worst" && !has(st, "mix")) model.values.mix = 100;

    // 延迟卡：实际值由设定与下限推出；同步档按 120 BPM 换算
    model.delay.actualMs =
        model.values.delay_mode === "sync"
            ? round1(NOTE_BEATS[model.values.delay_note] * BEAT_MS)
            : round1(Math.max(model.values.delay_ms, model.delay.minMs));

    // 失控保护降了多少 dB
    if (st.lamps && typeof st.lamps.run === "string" && st.lamps.run.startsWith("reduced")) {
        model.meters.lamps.guardDb = parseNum(st.lamps.run) ?? parseNum(st.vals && st.vals.x) ?? 0;
    }
    // 导出锁存：设计稿的 exportMismatch 对应违约加补 0 两个原因
    if (Array.isArray(st.band) && st.band.includes("exportMismatch")) model.exportCauses = { bits: 3, count: 3 };
    // 配对状态表达不了的五条消息经 notices 带给页面；newerProject 的 a 是工程版本、b 是本插件版本
    if (Array.isArray(st.band)) {
        model.notices = INPUT_NOTICE_IDS.filter((id) => st.band.includes(id)).map((id) =>
            id === "newerProject" ? { id, a: "0.11.0", b: model.version } : { id },
        );
    }
    // 宿主旁路并入回送静音，原因单独给
    if (st.hostBypass === true) {
        if (model.pair.state === "connected") model.pair.state = "connectedStill";
        model.silentCause = "hostBypass";
    }
}

/**
 * 套用一个场景的 state 到 Input 后端模型：先逐字段映射，再补设计稿的底座与新增字段。
 * 场景可以借另一个场景的状态（in-en 是 in-playing 的英文版，写作 { as: "in-playing" }）：被借的在先，自己写的盖在后面。
 * @param {object} model
 * @param {object} st 场景描述的 state
 * @param {string} [sceneId] 场景 ID；不给就只做逐字段映射
 */
function applyInput(model, st, sceneId) {
    const base = typeof st.as === "string" ? sceneById(st.as) : null;
    const merged = base && base.role === "input" ? { ...base.state, ...st } : st;
    applyInputState(model, merged);
    if (sceneId) applyInputBase(model, { id: base && base.role === "input" ? base.id : sceneId, state: merged });
    applyUserPresets(model, merged);
}

/** 套用一个场景的 state 到 Output 后端模型。 */
function applyOutput(model, st) {
    if (has(st, "pair") && PAIR_STATES.includes(st.pair)) model.pair.state = st.pair;
    if (has(st, "group")) model.group = st.group;
    if (st.groupAuto) model.groupOrigin = "auto";
    if (st.tracks && typeof st.tracks === "object") {
        model.pair.self.name = st.tracks.this ?? null;
        model.pair.peer.name = st.tracks.input ?? null;
        model.pair.verify = { match: "match", diff: "diff" }[st.tracks.verify] || "unknown";
    }
    if (typeof st.send === "string") {
        if (st.send === "bypassed") model.send.bypassed = true;
        else if (st.send.startsWith("MONO") || st.send.startsWith("ST")) {
            model.send.channels = st.send.startsWith("MONO") ? "MONO" : "ST";
            model.send.sampleRate = (parseNum(st.send.split("·")[1]) ?? 48) * 1000;
        }
    }
    if (st.confirm === "takeover") model.takeover = true;
    if (st.sub === "inputMuted") model.pair.state = "connectedStill";
    if (st.vals && typeof st.vals === "object") {
        if (has(st.vals, "a")) model.version = String(st.vals.a);
        if (has(st.vals, "b")) model.pair.peer.version = String(st.vals.b);
    }
}

/**
 * 创建预览会话。
 * @param {object} o
 * @param {"input"|"output"} o.role
 * @param {string|URLSearchParams|object} [o.params]
 * @param {{local:object, session:object}} [o.storage] 缺省内存存储；浏览器里由壳页传 localStorage / sessionStorage
 * @param {object} [o.timers]
 * @returns {{mock:object, start():void, stop():void, info:object, core:object, model:object}}
 */
export function createPreviewSession({ role, params = "", storage, timers } = {}) {
    if (role !== "input" && role !== "output")
        throw new TypeError(`createPreviewSession：role 必须是 input 或 output，实得 ${role}`);
    const get = readParams(params);
    const store = storage || { local: memoryStorage(), session: memoryStorage() };
    const warnings = [];

    if (get("fresh") === "1") {
        store.local.removeItem("rinne.uiDefaults");
        store.session.removeItem(`rinne.dismissedHints.${role}`);
    }

    const opts = { stale: get("stale") !== "0", autoInitial: get("auto") !== "0" };
    const backend =
        role === "input"
            ? createInputBackend({ storage: store, timers, opts })
            : createOutputBackend({ storage: store, timers, opts });
    const { model, core, mock } = backend;

    let scene = null;
    const askedScene = get("scenario");
    if (askedScene) {
        scene = sceneById(askedScene);
        if (!scene) warnings.push(`未知场景 ${askedScene}，按默认态打开`);
        else if (scene.role === "catalog") {
            warnings.push(`场景 ${scene.id} 是元件目录格，没有对应页面，按默认态打开`);
            scene = null;
        } else if (scene.role !== role) {
            warnings.push(`场景 ${scene.id} 属于 ${scene.role} 窗口，当前是 ${role}，按默认态打开`);
            scene = null;
        }
    }
    if (scene) (role === "input" ? applyInput : applyOutput)(model, scene.state, scene.id);

    const askedLang = get("lang");
    if (askedLang !== null && !LANG_CODES.includes(askedLang)) warnings.push(`未知语言 ${askedLang}，已忽略`);
    model.lang = LANG_CODES.includes(askedLang) ? askedLang : scene ? scene.lang : null;

    const askedScale = Number(get("scale"));
    const scale = backend.core.box.presets.includes(askedScale) ? askedScale : null;
    if (scale !== null) model.scale = scale;
    else if (get("scale") !== null) warnings.push(`未知档位 ${get("scale")}，已忽略`);

    mock.previewScene = scene ? { id: scene.id, lang: scene.lang, title: scene.title, state: scene.state } : null;

    return {
        mock,
        core,
        model,
        role,
        start: () => core.start(),
        stop: () => core.stop(),
        info: {
            role,
            scenario: scene ? scene.id : null,
            lang: model.lang,
            scale,
            warnings,
            fresh: get("fresh") === "1",
        },
    };
}

export default createPreviewSession;
