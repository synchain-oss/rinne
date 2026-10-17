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
import { createInputBackend } from "./bridge-input.js";
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

/** 套用一个场景的 state 到 Input 后端模型。 */
function applyInput(model, st) {
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
    if (scene) (role === "input" ? applyInput : applyOutput)(model, scene.state);

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
