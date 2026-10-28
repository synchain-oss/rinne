// SPDX-License-Identifier: GPL-3.0-or-later
//
// mock 后端的共用部分：全局位存储、事件分发、四赫兹异步状态回声、两个角色共用的十二个桥函数。
// 两个角色的后端（bridge-input.js、bridge-output.js）在这个核心上各自加自己的函数与事件。
//
// 与真桥同形的时序，页面按真桥的纪律写才不会只在预览里能用：
//   - 请求初始状态之前，周期回声一律丢弃，也不记账；requestInitialState 之后立刻把本角色所有状态类事件
//     各发一帧（首帧必发）；
//   - 回声默认异步：写函数先返回回执，rinne.state 在下一拍（250 ms 内）才到；
//   - 每隔几拍在新帧之后补发一帧过期的全量快照（序号更小），页面必须按 seq 丢弃；
//   - 全局位（语言、缩放、规则确认、引导已看）存 localStorage["rinne.uiDefaults"]，同源的两个页面共享；
//     ?fresh=1 先清空；每实例的已关闭提示存 sessionStorage；
//   - 缩放锁定（真桥在系统 WebKit 过旧时的行为）：全局位里的 scaleLocked 为真时，setUiScale 与 commitUiScale
//     回 scale-locked，没确认的预览作废，窗口档位回到 1。scaleLocked 是预览专用的全局位，真桥没有这一项；
//     每次调用现读，所以在预览页的控制台里改 localStorage 就能走到锁定态，改回去就解锁；
//   - 兜底面板在场（真桥在运行时缺失、前端启动失败或加载超时后会换上兜底面板，网页隐藏）：全局位里的 fallbackShown 为真时，
//     setLang、setUiScale、commitUiScale 在参数校验之前就回 not-ready，什么都不改（不写全局位、不动档位与预览、不排回声）；
//     requestInitialState 只回 { ok: true }，不发首帧。
//     fallbackShown 同样是预览专用的全局位，真桥没有这一项，每次调用现读。
// 模块顶层不碰 window，存储与计时器都从外面注入，Node 里可以直接 import 和测试。

import { DESIGN } from "../../web/shared/design-box.js";
import { DIAG_KEYS, GROUPS, LANG_CODES } from "./contract.js";

export const UI_DEFAULTS_KEY = "rinne.uiDefaults";
export const DISMISSED_KEY = "rinne.dismissedHints";

/** 周期回声间隔，毫秒（四赫兹）。 */
export const ECHO_MS = 250;

/** 页面一直不请求初始状态时，mock 自己代请求的等待时间，毫秒。 */
export const AUTO_INITIAL_MS = 1500;

/** 每隔几拍补发一帧过期快照。 */
const STALE_EVERY = 4;

const has = (o, k) => Object.prototype.hasOwnProperty.call(o, k);

/** 内存存储，Node 与禁用存储的浏览器里用。 */
export function memoryStorage() {
    const m = new Map();
    return {
        getItem: (k) => (m.has(k) ? m.get(k) : null),
        setItem: (k, v) => void m.set(k, String(v)),
        removeItem: (k) => void m.delete(k),
    };
}

/**
 * 全局位的默认值。除 scaleLocked 与 fallbackShown 外，键名与原生 UiDefaultsStore 对应；
 * 这两项只给预览走到缩放锁定态与兜底面板在场态用，真桥没有。
 */
export function defaultUiDefaults() {
    return {
        langChosen: false,
        lang: "zh",
        rulesAckVersion: 0,
        guideSeenInput: false,
        guideSeenOutput: false,
        uiScaleInput: 1,
        uiScaleOutput: 1,
        diagLog: false,
        scaleLocked: false,
        fallbackShown: false,
    };
}

/** 全局位存储：读时只认已知键和同类型的值，其余丢掉。 */
export function createUiDefaultsStore(storage) {
    const base = defaultUiDefaults();
    const read = () => {
        let raw = null;
        try {
            raw = JSON.parse(storage.getItem(UI_DEFAULTS_KEY) || "null");
        } catch {
            raw = null;
        }
        const out = { ...base };
        if (raw && typeof raw === "object") {
            for (const k of Object.keys(base)) {
                if (has(raw, k) && typeof raw[k] === typeof base[k]) out[k] = raw[k];
            }
        }
        return out;
    };
    return {
        get: read,
        set(patch) {
            const next = { ...read() };
            for (const k of Object.keys(patch)) if (has(base, k)) next[k] = patch[k];
            storage.setItem(UI_DEFAULTS_KEY, JSON.stringify(next));
            return next;
        },
        clear: () => storage.removeItem(UI_DEFAULTS_KEY),
    };
}

/** openExternal 的白名单：https，host 是 synchain.ca，或 github.com 且路径限定在本项目下。 */
export function isAllowedExternal(url) {
    let u;
    try {
        u = new URL(String(url));
    } catch {
        return false;
    }
    if (u.protocol !== "https:" || u.username || u.password || u.port) return false;
    if (u.hostname === "synchain.ca") return true;
    if (u.hostname !== "github.com") return false;
    return u.pathname === "/synchain-oss/rinne" || u.pathname.startsWith("/synchain-oss/rinne/");
}

/** 简单的事件分发器：addEventListener 与 JUCE 的 backend 同名同参。 */
function makeEmitter() {
    const map = new Map();
    return {
        add(name, cb) {
            if (!map.has(name)) map.set(name, []);
            map.get(name).push(cb);
        },
        emit(name, payload) {
            for (const cb of (map.get(name) || []).slice()) cb(payload);
        },
    };
}

/**
 * 创建核心。
 * @param {object} o
 * @param {"input"|"output"} o.role
 * @param {string[]} o.events 本角色的事件名表
 * @param {{local:object, session:object}} o.storage
 * @param {object} [o.timers] setTimeout、clearTimeout、setInterval、clearInterval，缺省取全局
 * @param {object} [o.opts] stale：是否插过期帧（缺省 true）；autoInitial：页面不请求时是否代请求（缺省 true）
 */
export function createCore({ role, events, storage, timers = globalThis, opts = {} }) {
    const em = makeEmitter();
    const defaults = createUiDefaultsStore(storage.local);

    let seq = 0;
    let ready = false;
    let started = false;
    let tick = 0;
    let echoTimer = null;
    let initialTimer = null;
    let autoCalled = false;
    let previous = null; // 上一次发出的 rinne.state，用来造过期帧
    const frameBuilders = {}; // 事件名 -> () => 载荷
    const sent = {}; // 事件名 -> 已发次数，给测试和工具条看

    const core = {
        role,
        box: DESIGN[role],
        /** 全局位每次现读：同源的另一个页面改了，这一边下一帧就能看到。 */
        get ui() {
            return defaults.get();
        },
        defaults,
        storage,
        sent,
        events,
        timers,
        get ready() {
            return ready;
        },
        get autoCalled() {
            return autoCalled;
        },
        nextSeq: () => ++seq,
        /** 登记某个事件的载荷构造函数；首帧必发与周期回声都经它。 */
        register(name, build) {
            if (!events.includes(name)) throw new RangeError(`事件 ${name} 不在 ${role} 侧名表内`);
            frameBuilders[name] = build;
        },
        addEventListener: (name, cb) => em.add(name, cb),
        /** 立即发一帧；请求初始状态之前不发也不记账。 */
        emit(name, payload) {
            if (!ready) return false;
            sent[name] = (sent[name] || 0) + 1;
            if (name === "rinne.state") previous = payload;
            em.emit(name, payload);
            return true;
        },
        push(name) {
            return core.emit(name, frameBuilders[name]());
        },
        /** 错误事件按需发，不属于首帧必发的状态类事件。 */
        fail(code, detail) {
            return core.emit("rinne.error", { seq: core.nextSeq(), code, detail });
        },
        pushAll() {
            for (const name of Object.keys(frameBuilders)) core.push(name);
        },
        /** 异步回声：下一拍把状态类事件各发一帧。 */
        scheduleEcho() {
            timers.setTimeout(() => core.pushAll(), 0);
        },
        start() {
            if (started) return;
            started = true;
            echoTimer = timers.setInterval(() => {
                if (!ready) return;
                tick++;
                const old = previous;
                core.push("rinne.state");
                core.push("rinne.pair");
                if (opts.stale !== false && tick % STALE_EVERY === 0 && old) {
                    // 新帧之后补发一帧序号更小的旧快照，页面必须按 seq 丢掉
                    sent["rinne.state.stale"] = (sent["rinne.state.stale"] || 0) + 1;
                    em.emit("rinne.state", old);
                }
            }, ECHO_MS);
            if (opts.autoInitial !== false) {
                initialTimer = timers.setTimeout(() => {
                    if (!ready) {
                        autoCalled = true;
                        core.requestInitialState();
                    }
                }, AUTO_INITIAL_MS);
            }
        },
        stop() {
            started = false;
            if (echoTimer !== null) timers.clearInterval(echoTimer);
            if (initialTimer !== null) timers.clearTimeout(initialTimer);
            echoTimer = initialTimer = null;
        },
        requestInitialState() {
            ready = true;
            core.pushAll();
            return { ok: true };
        },
    };
    return core;
}

/** 构造诊断文本（rinne-diag v1）：只含白名单 key，不含路径、用户名和主机名。 */
export function buildDiagnostics(fields) {
    const lines = [];
    for (const key of DIAG_KEYS) {
        if (has(fields, key)) lines.push(`${key}=${String(fields[key]).replace(/[\r\n]+/g, " ")}`);
    }
    return lines.join("\n");
}

/**
 * 两个角色共用的十二个桥函数。env 是角色后端的可变状态（scale、group、groupOrigin、clipboard、opened）
 * 加一个 diagnostics() 取文本的函数。每个函数同步返回回执，bridge.js 再把它包成 async。
 */
export function commonFunctions(core, env) {
    const { defaults, role, box } = core;
    const keyScale = role === "input" ? "uiScaleInput" : "uiScaleOutput";
    const keyGuide = role === "input" ? "guideSeenInput" : "guideSeenOutput";
    let pendingScale = null; // setUiScale 应用后、commitUiScale 之前的档位
    // 缩放锁定：与真桥同一套语义。锁定时窗口钉在 100%，没确认的预览作废并退回最近一次确认的档位，选档与确认都回
    // scale-locked；解锁后窗口回到那一档。全局位每次调用现读，所以解锁在下一次选档或确认时才落到窗口档位上。
    let scaleBeforePreview = 1; // 当前这段待确认的预览开始前的档位（只在有待确认的预览时有意义）
    let lockSeen = false; // 上一次调用时是否处于锁定
    let settledScale = 1; // 锁定开始时的「最近一次确认的档位」，解锁时退回它
    const lockGuard = () => {
        const locked = defaults.get().scaleLocked === true;
        if (locked && !lockSeen) {
            settledScale = pendingScale !== null ? scaleBeforePreview : env.scale;
            pendingScale = null;
            env.scale = 1;
            core.scheduleEcho();
        } else if (!locked && lockSeen) {
            env.scale = settledScale;
            core.scheduleEcho();
        }
        lockSeen = locked;
        return locked ? { ok: false, reason: "scale-locked" } : null;
    };
    // 兜底面板在场：与真桥同一套语义，守卫在 setLang、setUiScale、commitUiScale 的最前面，先于参数校验，回执与参数无关
    const panelShown = () => defaults.get().fallbackShown === true;
    const notReady = () => ({ ok: false, reason: "not-ready" });
    return {
        // 面板在场时只回执：不发首帧，也不置就绪（真桥同样不把它当作桥已就绪）
        requestInitialState: () => (panelShown() ? { ok: true } : core.requestInitialState()),
        setLang(code) {
            if (panelShown()) return notReady();
            if (typeof code !== "string" || !LANG_CODES.includes(code)) return { ok: false, reason: "bad-lang" };
            defaults.set({ lang: code, langChosen: true });
            core.scheduleEcho();
            return { ok: true, lang: code };
        },
        setUiScale(f) {
            if (panelShown()) return notReady();
            if (typeof f !== "number" || !box.presets.includes(f)) return { ok: false, reason: "bad-scale" };
            const locked = lockGuard();
            if (locked) return locked;
            if (pendingScale === null) scaleBeforePreview = env.scale;
            pendingScale = f;
            env.scale = f;
            core.scheduleEcho();
            return { ok: true, scale: f };
        },
        commitUiScale() {
            if (panelShown()) return notReady();
            const locked = lockGuard();
            if (locked) return locked;
            if (pendingScale === null) return { ok: false, reason: "no-pending" };
            defaults.set({ [keyScale]: pendingScale });
            pendingScale = null;
            return { ok: true, scale: env.scale };
        },
        setRulesAck(version) {
            if (!Number.isInteger(version) || version < 0) return { ok: false, reason: "bad-version" };
            defaults.set({ rulesAckVersion: version });
            core.scheduleEcho();
            return { ok: true };
        },
        setGuideSeen(seen) {
            if (seen !== true) return { ok: false, reason: "bad-value" };
            defaults.set({ [keyGuide]: true });
            core.scheduleEcho();
            return { ok: true };
        },
        setGroup(g) {
            if (typeof g !== "string" || !GROUPS.includes(g)) return { ok: false, reason: "bad-group" };
            env.group = g;
            env.groupOrigin = "manual";
            core.scheduleEcho();
            return { ok: true, group: g };
        },
        setDiagLog(on) {
            if (typeof on !== "boolean") return { ok: false, reason: "bad-value" };
            defaults.set({ diagLog: on });
            core.scheduleEcho();
            return { ok: true, value: on };
        },
        resolveTakeover(accept) {
            if (typeof accept !== "boolean") return { ok: false, reason: "bad-value" };
            if (!env.takeover) return { ok: false, reason: "no-pending" };
            env.takeover = false;
            if (accept) env.pairState = "searching";
            core.scheduleEcho();
            return { ok: true, accepted: accept };
        },
        showLicense() {
            env.licenseShown = true;
            return { ok: true };
        },
        copyDiagnostics(commit) {
            const text = env.diagnostics();
            if (commit === true) env.clipboard = text;
            return { ok: true, copied: commit === true, text };
        },
        openExternal(url) {
            if (!isAllowedExternal(url)) return { ok: false, reason: "not-allowed" };
            env.opened = String(url);
            return { ok: true };
        },
    };
}
