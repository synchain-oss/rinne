// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rinne Input 的 mock 后端：十二个共用函数加十四个 Input 专用函数，六个事件。
// 名表与 docs/UI_BRIDGE.md、src/input/InputBridgeApi.h、web/shared/bridge.js 逐名相等，由 check-bridge-parity 对拍。
// 载荷形状见 docs/UI_BRIDGE.md；这里只造够页面开发用的数据，不模拟 DSP。

import {
    buildDiagnostics,
    commonFunctions,
    createCore,
    memoryStorage,
    previewDiagEvents,
    previewDiagValues,
} from "./bridge-common.js";
import {
    CUT_REASONS,
    FACTORY_PRESETS,
    GROUPS,
    PARAM_DEFS,
    PARAM_IDS,
    PRESET_NAME_MAX,
    RESUME_COOLDOWN_MS,
} from "./contract.js";

export const FUNCTIONS = [
    "requestInitialState",
    "setLang",
    "setUiScale",
    "commitUiScale",
    "setRulesAck",
    "setGuideSeen",
    "setGroup",
    "copyDiagnostics",
    "openExternal",
    "setDiagLog",
    "resolveTakeover",
    "showLicense",
    "beginParamGesture",
    "setParam",
    "endParamGesture",
    "setLimiter",
    "panic",
    "resumeLoop",
    "dismissHint",
    "loadPreset",
    "savePreset",
    "deletePreset",
    "setGuardOn",
    "setResetOnPlay",
    "setTailMode",
    "moveToFreeGroup",
];

export const EVENTS = ["rinne.state", "rinne.pair", "rinne.error", "rinne.params", "rinne.meters", "rinne.presets"];

/** 载入预设时不动的参数：环路开关、限制器和上限不进预设。 */
const PRESET_EXEMPT = ["loop_on", "limiter", "ceiling"];

/** tail 模式取值。 */
const TAIL_MODES = ["auto", "infinite", "30s"];

const CONTROL_CHARS = /[\u0000-\u001f\u007f-\u009f]/g;

/** 预设名清洗：去控制字符、首尾空白，按字符（码点）截到上限。 */
export function cleanPresetName(name) {
    const s = String(name ?? "")
        .replace(CONTROL_CHARS, "")
        .trim();
    return Array.from(s).slice(0, PRESET_NAME_MAX).join("").trim();
}

/** 把值按参数定义夹取或校验；不合法返回 undefined。 */
export function coerceParam(id, value) {
    const d = Object.prototype.hasOwnProperty.call(PARAM_DEFS, id) ? PARAM_DEFS[id] : null;
    if (!d) return undefined;
    if (d.type === "bool") return typeof value === "boolean" ? value : undefined;
    if (d.type === "enum") return d.values.includes(value) ? value : undefined;
    if (typeof value !== "number" || !Number.isFinite(value)) return undefined;
    let v = Math.min(d.max, Math.max(d.min, value));
    if (d.step) v = Math.round(v / d.step) * d.step;
    return Math.round(v * 1e6) / 1e6;
}

const defaultValues = () => Object.fromEntries(PARAM_IDS.map((id) => [id, PARAM_DEFS[id].def]));

/**
 * 创建 Input 后端。
 * @param {object} o
 * @param {{local:object, session:object}} [o.storage]
 * @param {object} [o.timers]
 * @param {object} [o.opts] 见 createCore
 * @param {() => number} [o.now] 毫秒时钟，冷却判定用
 */
export function createInputBackend({ storage, timers, opts, now = () => Date.now() } = {}) {
    const store = storage || { local: memoryStorage(), session: memoryStorage() };
    const core = createCore({ role: "input", events: EVENTS, storage: store, timers, opts });

    const model = {
        version: "0.0.0-preview",
        host: "preview",
        sampleRate: 48000,
        blockSize: 512,
        scale: core.ui.uiScaleInput,
        lang: null, // 非空时覆盖全局语言（?lang=），不落盘
        group: "A",
        groupOrigin: "auto",
        restored: false,
        takeover: false, // 休眠实例占着本组，等用户确认接管
        values: defaultValues(),
        cfg: { resetOnPlay: true, tailMode: "auto", delayUnit: "ms", fbUnit: "pct", guardOn: true },
        presetId: "init",
        presetDirty: false,
        cut: null,
        cutAt: 0,
        delay: { actualMs: 100, minMs: 10.7, reason: "ok", maxBlock: 512 },
        pair: {
            state: "searching",
            groupMask: 0,
            peer: { name: null, color: null, channels: null, version: null, holder: null, sampleRate: null },
            self: { name: null, channels: null },
            verify: "unknown",
        },
        // dryPart / retPart / highCutLimited 为 undefined 时按 in、ret、Mix 和高切现算（deriveParts）；给 null 或数值就原样发
        meters: {
            in: null,
            ret: null,
            out: null,
            dryPart: undefined,
            retPart: undefined,
            gr: 0,
            highCutLimited: undefined,
            lamps: { dc: "ok", nonfinite: "ok", nonfiniteCount: 0, guard: "ok", guardDb: 0 },
        },
        exportCauses: { bits: 0, count: 0 },
        silentCause: null,
        userPresets: [],
        dismissed: new Set(),
    };
    try {
        const raw = JSON.parse(store.session.getItem("rinne.dismissedHints.input") || "[]");
        for (const id of Array.isArray(raw) ? raw : []) if (typeof id === "string") model.dismissed.add(id);
    } catch {
        // 存储里不是 JSON 就当没有
    }

    const env = {
        get scale() {
            return model.scale;
        },
        set scale(v) {
            model.scale = v;
        },
        get group() {
            return model.group;
        },
        set group(v) {
            model.group = v;
        },
        get groupOrigin() {
            return model.groupOrigin;
        },
        set groupOrigin(v) {
            model.groupOrigin = v;
        },
        get takeover() {
            return model.takeover;
        },
        set takeover(v) {
            model.takeover = v;
        },
        set pairState(v) {
            model.pair.state = v;
        },
        licenseShown: false,
        clipboard: null,
        opened: null,
        diagnostics: () => {
            const sr = model.sampleRate;
            const sync = model.values.delay_mode === "sync";
            return buildDiagnostics(
                "input",
                {
                    ...previewDiagValues("input", model, core),
                    "loop.d_user_ms": model.values.delay_ms.toFixed(3),
                    "loop.d_eff": Math.round((model.delay.actualMs * sr) / 1000),
                    "loop.d_min": Math.round((model.delay.minMs * sr) / 1000),
                    "loop.mode": sync ? "sync" : "ms", // core 的写法：time 档写成 ms
                    "loop.bpm": "120.0",
                    "loop.bpm_src": sync ? "manual" : "none",
                    "loop.taps_active": 0,
                    "loop.tap_drop": 0,
                    "sync.resync": 0,
                    "sync.overflow": 0,
                    "sync.torn": 0,
                    "sync.overrun": 0,
                    "sync.w_cas_fail": 0,
                    "sync.unstable": 0,
                    "sync.off_drift_blocks": 0,
                    "sync.frozen_max": 0,
                    "order.verdict": model.pair.state === "connected" ? "correct" : "unknown",
                    "order.votes": "0.00",
                    "order.host_ns": "valid",
                    "safety.state": model.cut ? "TRIPPED" : "RUN",
                    "safety.trips": model.cut ? 1 : 0,
                    "safety.tail_peak_max_db": "-96.0",
                    "safety.limiter_gr_max_db": "0.0",
                    "safety.nonfinite_ring": model.meters.lamps.nonfiniteCount,
                    "safety.clamp_ring": 0,
                },
                previewDiagEvents(model),
            );
        },
    };
    const common = commonFunctions(core, env);

    const cooldownLeft = () => (model.cut ? Math.max(0, model.cutAt + RESUME_COOLDOWN_MS - now()) : 0);
    const persistDismissed = () =>
        store.session.setItem("rinne.dismissedHints.input", JSON.stringify([...model.dismissed]));

    // ---- 事件载荷 ----
    core.register("rinne.state", () => ({
        seq: core.nextSeq(),
        role: "input",
        version: model.version,
        lang: model.lang ?? core.ui.lang,
        langChosen: core.ui.langChosen,
        rulesAck: core.ui.rulesAckVersion,
        guideSeen: core.ui.guideSeenInput,
        uiScale: model.scale,
        restored: model.restored,
        takeover: model.takeover,
        group: model.group,
        groupOrigin: model.groupOrigin,
        diagLog: core.ui.diagLog,
        sampleRate: model.sampleRate,
        cfg: { ...model.cfg },
        preset: { id: model.presetId, dirty: model.presetDirty, name: presetName(model.presetId) },
        delay: { ...model.delay },
        safety: { cut: model.cut, cooldownMs: cooldownLeft(), l2: model.cut !== null },
        dismissedHints: [...model.dismissed],
        exportCauses: { ...model.exportCauses },
        silentCause: model.silentCause,
    }));
    core.register("rinne.pair", () => ({
        seq: core.nextSeq(),
        state: model.pair.state,
        groupMask: model.pair.groupMask,
        peer: { ...model.pair.peer },
        self: { ...model.pair.self },
        verify: model.pair.verify,
    }));
    core.register("rinne.params", () => ({ seq: core.nextSeq(), values: { ...model.values } }));
    core.register("rinne.meters", () => ({
        seq: core.nextSeq(),
        ...structuredCloneSafe(model.meters),
        ...deriveParts(model),
    }));
    core.register("rinne.presets", () => ({
        factory: FACTORY_PRESETS.map((p) => ({ id: p.id, name: { ...p.name } })),
        user: model.userPresets.map((p) => ({ id: p.id, name: p.name })),
        current: model.presetId,
    }));

    const presetName = (id) => {
        const f = FACTORY_PRESETS.find((p) => p.id === id);
        if (f) return f.name;
        const u = model.userPresets.find((p) => p.id === id);
        return u ? u.name : null;
    };

    const known = (id) => typeof id === "string" && Object.prototype.hasOwnProperty.call(PARAM_DEFS, id);
    const applyValues = (values) => {
        for (const id of PARAM_IDS) {
            if (PRESET_EXEMPT.includes(id)) continue;
            model.values[id] = Object.prototype.hasOwnProperty.call(values, id) ? values[id] : PARAM_DEFS[id].def;
        }
    };

    const fns = {
        ...common,
        beginParamGesture: (id) => (known(id) ? { ok: true } : { ok: false, reason: "bad-param" }),
        setParam(id, value) {
            if (!known(id)) return { ok: false, reason: "bad-param" };
            const v = coerceParam(id, value);
            if (v === undefined) return { ok: false, reason: "bad-value" };
            model.values[id] = v;
            if (!PRESET_EXEMPT.includes(id)) model.presetDirty = true; // 环路开关、限制器和上限不进预设
            const seq = core.nextSeq();
            core.timers.setTimeout(() => core.emit("rinne.params", { seq, values: { ...model.values } }), 0);
            core.scheduleEcho();
            return { ok: true, value: v, seq };
        },
        endParamGesture: (id) => (known(id) ? { ok: true } : { ok: false, reason: "bad-param" }),
        setLimiter(on) {
            if (typeof on !== "boolean") return { ok: false, reason: "bad-value" };
            return fns.setParam("limiter", on);
        },
        panic() {
            if (model.cut === null) {
                model.cut = "panic";
                model.cutAt = now();
            }
            core.scheduleEcho();
            return { ok: true, cut: model.cut };
        },
        resumeLoop() {
            if (model.cut === null) return { ok: false, reason: "not-cut" };
            const left = cooldownLeft();
            if (left > 0) return { ok: false, reason: "cooldown", remainingMs: left };
            model.cut = null;
            core.scheduleEcho();
            return { ok: true };
        },
        setGuardOn(on) {
            if (typeof on !== "boolean") return { ok: false, reason: "bad-value" };
            model.cfg.guardOn = on;
            core.scheduleEcho();
            return { ok: true, value: on };
        },
        setResetOnPlay(on) {
            if (typeof on !== "boolean") return { ok: false, reason: "bad-value" };
            model.cfg.resetOnPlay = on;
            core.scheduleEcho();
            return { ok: true, value: on };
        },
        setTailMode(mode) {
            if (!TAIL_MODES.includes(mode)) return { ok: false, reason: "bad-value" };
            model.cfg.tailMode = mode;
            core.scheduleEcho();
            return { ok: true, value: mode };
        },
        moveToFreeGroup() {
            if (model.pair.state !== "conflict") return { ok: false, reason: "not-conflict" };
            const free = GROUPS.find((g) => g !== model.group && !(model.pair.groupMask & (1 << GROUPS.indexOf(g))));
            if (!free) return { ok: false, reason: "no-free-group" };
            model.group = free;
            model.groupOrigin = "manual"; // 用户点按钮选的，之后不再自动迁组
            model.pair.state = "searching";
            core.scheduleEcho();
            return { ok: true, group: free };
        },
        dismissHint(id) {
            if (typeof id !== "string" || id === "") return { ok: false, reason: "bad-id" };
            model.dismissed.add(id);
            persistDismissed();
            core.scheduleEcho();
            return { ok: true };
        },
        loadPreset(id) {
            const f = FACTORY_PRESETS.find((p) => p.id === id);
            const u = model.userPresets.find((p) => p.id === id);
            if (!f && !u) return { ok: false, reason: "no-preset" };
            applyValues((f || u).values);
            model.presetId = id;
            model.presetDirty = false;
            core.scheduleEcho();
            core.emit("rinne.params", { seq: core.nextSeq(), values: { ...model.values } });
            return { ok: true };
        },
        savePreset(name, overwrite) {
            const clean = cleanPresetName(name);
            if (clean === "") return { ok: false, reason: "bad-name" };
            const values = {};
            for (const id of PARAM_IDS) if (!PRESET_EXEMPT.includes(id)) values[id] = model.values[id];
            const same = model.userPresets.find((p) => p.name === clean);
            if (same && overwrite !== true) return { ok: false, reason: "exists" };
            const entry = same || { id: `user:${clean}`, name: clean, values };
            entry.values = values;
            if (!same) model.userPresets.push(entry);
            model.presetId = entry.id;
            model.presetDirty = false;
            core.scheduleEcho();
            core.push("rinne.presets");
            return { ok: true, id: entry.id };
        },
        deletePreset(id) {
            const i = model.userPresets.findIndex((p) => p.id === id);
            if (i < 0) return { ok: false, reason: "no-preset" };
            model.userPresets.splice(i, 1);
            if (model.presetId === id) model.presetId = "init";
            core.scheduleEcho();
            core.push("rinne.presets");
            return { ok: true };
        },
    };

    const mock = { addEventListener: core.addEventListener };
    for (const name of FUNCTIONS) mock[name] = fns[name];
    // previewScene 由预览会话在套完场景状态之后赋值；赋值时补全设计稿里该场景的底座与新增字段（只给预览用，真桥没有）
    let scene = null;
    Object.defineProperty(mock, "previewScene", {
        enumerable: true,
        get: () => scene,
        set(v) {
            scene = v;
            if (v && v.state) applyPreviewExtras(model, v);
        },
    });
    return { role: "input", core, model, mock, cutReasons: CUT_REASONS };
}

// ---- 预览场景补全 ----
// scenarios.js 里每个场景只写「与底座不同的部分」，设计稿的底座（inBase / playing）不在表里，所以这里按场景 ID 补上；
// 再把新增的桥字段（分量电平、导出原因、静音原因、对端采样率等）从场景描述推出来。只补场景没写的键，写了的不动。

const has = (o, k) => Object.prototype.hasOwnProperty.call(o, k);
const TRACK_NAME = "Vox Lead";
const TRACK_COLOR = "#2d79c6";
/** 设计稿里对端在场的配对状态。 */
const PEER_STATES = ["connected", "connectedUnverified", "connectedStill", "crossTrack", "crossProcess", "stale"];

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

/** 场景串里的第一个数（兼容 U+2212）；没有数给 undefined。 */
function firstNumber(text) {
    if (typeof text === "number") return text;
    const m = typeof text === "string" ? /[-−]?\d+(?:\.\d+)?/.exec(text) : null;
    return m ? Number(m[0].replace("−", "-")) : undefined;
}

const dbOf = (ratio) => (ratio > 0 ? 20 * Math.log10(ratio) : null);
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
 * Mix 推子的两路分量：干声 = 输入电平 + 干声比例，回送 = 回送电平 + 回送比例（与设计稿的 mixLevels 同式，
 * 比例 g_dry = min(1, 2(1 - m))、g_ret = min(1, 2m)）；静音、环路关闭或比例为 0 的一路给 null。
 * 高切截断位：设定高于 0.45 倍采样率而又不是「关」。meters 里显式给了值就用它的。
 */
export function deriveParts(model) {
    const mx = Math.max(0, Math.min(100, model.values.mix)) / 100;
    const dry = dbOf(Math.min(1, 2 * (1 - mx)));
    const wet = dbOf(Math.min(1, 2 * mx));
    const { in: lvIn, ret: lvRet, dryPart, retPart, highCutLimited } = model.meters;
    const hc = model.values.high_cut;
    return {
        dryPart: dryPart !== undefined ? dryPart : lvIn !== null && dry !== null ? round1(lvIn + dry) : null,
        retPart:
            retPart !== undefined
                ? retPart
                : model.values.loop_on && lvRet !== null && wet !== null
                  ? round1(lvRet + wet)
                  : null,
        highCutLimited: highCutLimited !== undefined ? highCutLimited : hc < 20000 && hc > 0.45 * model.sampleRate,
    };
}

/**
 * 套用设计稿的场景底座与新增字段。scene.state 是 scenarios.js 里的描述；model 已经被 state-driver 套过一遍。
 * 覆盖层场景（预设面板、关于、缩放确认）按 playing 底座补，细节归页面。
 * @param {object} model
 * @param {{id:string, state:object}} scene
 */
export function applyPreviewExtras(model, scene) {
    const st = scene.state;
    const playing = PLAYING_BASED.has(scene.id);

    // 底座：已连接、对端 Vox Lead、核验一致
    if (!has(st, "pair")) model.pair.state = "connected";
    if (PEER_STATES.includes(model.pair.state)) {
        const unverified = model.pair.state === "connectedUnverified";
        if (!has(st, "peer") && !unverified) {
            model.pair.peer.name = TRACK_NAME;
            model.pair.verify = model.pair.state === "crossTrack" ? "diff" : "match";
        }
        if (model.pair.peer.name) model.pair.peer.color = TRACK_COLOR;
        model.pair.peer.channels = 2;
        model.pair.peer.sampleRate = model.sampleRate;
    }
    model.pair.self.name = model.pair.self.name ?? TRACK_NAME;
    model.pair.self.channels = 2;

    const lv = has(st, "meters") ? st.meters : {};
    if (!has(lv, "in")) model.meters.in = playing ? -12.4 : -18;
    if (!has(lv, "out")) model.meters.out = playing ? -9.1 : -18;
    if (!has(lv, "ret")) model.meters.ret = playing ? -14.8 : null;
    if (playing) {
        if (!has(st, "mix")) model.values.mix = 25;
        if (!has(st, "feedback")) model.values.feedback = 72;
        if (!has(st, "highCut")) model.values.high_cut = 6000;
        if (!has(st, "gr")) model.meters.gr = -2.1;
        model.presetId = "slapback";
        model.presetDirty = true;
    }
    if (st.panic === "cut") {
        // 切断：回送静音，输出保留干声
        if (!has(lv, "ret")) model.meters.ret = null;
        if (!has(lv, "out")) model.meters.out = -12.6;
        if (!has(st, "gr")) model.meters.gr = 0;
    }
    if (has(st, "gr")) model.meters.gr = firstNumber(st.gr) ?? 0;

    // 延迟卡：实际值由设定与下限推出；同步档按 120 BPM 换算
    model.delay.actualMs =
        model.values.delay_mode === "sync"
            ? round1(NOTE_BEATS[model.values.delay_note] * BEAT_MS)
            : round1(Math.max(model.values.delay_ms, model.delay.minMs));

    // 失控保护降了多少 dB
    if (st.lamps && typeof st.lamps.run === "string" && st.lamps.run.startsWith("reduced")) {
        model.meters.lamps.guardDb = firstNumber(st.lamps.run) ?? firstNumber(st.vals && st.vals.x) ?? 0;
    }
    // 导出锁存：设计稿的 exportMismatch 对应违约加补 0 两个原因
    if (Array.isArray(st.band) && st.band.includes("exportMismatch")) model.exportCauses = { bits: 3, count: 3 };
    // 宿主旁路并入回送静音，原因单独给
    if (st.hostBypass === true) {
        if (model.pair.state === "connected") model.pair.state = "connectedStill";
        model.silentCause = "hostBypass";
    }
}

function structuredCloneSafe(o) {
    return JSON.parse(JSON.stringify(o));
}
