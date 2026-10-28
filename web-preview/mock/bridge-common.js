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
import { DIAG_INPUT_ONLY_SECTIONS, DIAG_KEYS, GROUPS, LANG_CODES, PAIR_STATES } from "./contract.js";

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

/** core 的 sanitizeText 拒收的字符：路径分隔符、冒号、@、~、%。 */
const DIAG_REJECTED = /[/\\:@~%]/;

/**
 * 诊断值的清洗，与 core 的 sanitizeText 同一规则：按空白切成片段，任何一个片段带拒收字符，整串不要，写 none
 * （路径里的目录名可以带空格，只丢带分隔符的那一段，会把目录名留在文本里）；否则逐字符清洗，只留 ASCII 字母数字与
 * . _ + - ( ) 和单个空格，其余写 _（含 =，值不会冒充下一个键）。清空了也写 none。
 * 与 core 有两处不同：多留逗号（nsamples.hist 的直方图要用，core 里那个字段走数字格式化，不过清洗）；不做 48 字符
 * 封顶（预览值都很短）。
 */
const diagValue = (v) => {
    const fragments = String(v)
        .split(/[ \t\n\r\v\f]+/)
        .filter((f) => f !== "");
    if (fragments.some((f) => DIAG_REJECTED.test(f))) return "none";
    return fragments.join(" ").replace(/[^A-Za-z0-9 .,_+()-]/g, "_") || "none";
};

/** 事件行：`  t+<秒>.<毫秒> <码名>[ <a>[/<b>]]`，与 core 的 appendEvent 同形。 */
function diagEventLine(e) {
    const t = `${Math.floor(e.tMs / 1000)}.${String(e.tMs % 1000).padStart(3, "0")}`;
    const args = e.a === undefined ? "" : ` ${e.a}${e.b === undefined ? "" : `/${e.b}`}`;
    return `  t+${t} ${e.code}${args}`;
}

/**
 * 构造诊断文本（rinne-diag v1），形态与 core 的生成器一致：第一行格式标识，之后每节一行（product、backend 两行
 * 没有冒号，其余是「节名: key=value …」），然后是最近事件段，末行 ui 段。
 * values 的键是 DIAG_KEYS 里「节名.键名」形式的名字，不在表里的名字一律不输出；Output 不写 Input 专有的节；
 * 没有对端时 peer 一行写 `peer: none`。events 是 { tMs, code, a?, b? } 的数组。
 */
export function buildDiagnostics(role, values, events = []) {
    const lines = ["rinne-diag v1"];
    const done = new Set();
    for (const name of DIAG_KEYS) {
        if (name === "rinne-diag") continue; // 第一行已写
        if (name === "events") {
            const shown = events.slice(-64);
            lines.push(`events(last ${shown.length} of ${events.length}):`, ...shown.map(diagEventLine));
            continue;
        }
        const section = name.slice(0, name.indexOf("."));
        if (done.has(section)) continue;
        done.add(section);
        if (role !== "input" && DIAG_INPUT_ONLY_SECTIONS.includes(section)) continue;
        const pairs = DIAG_KEYS.filter((k) => k.startsWith(`${section}.`) && has(values, k)).map(
            (k) => `${k.slice(section.length + 1)}=${diagValue(values[k])}`,
        );
        if (section === "product" || section === "backend") lines.push(pairs.join(" "));
        else lines.push(`${section}: ${pairs.length ? pairs.join(" ") : "none"}`);
    }
    return `${lines.join("\n")}\n`;
}

/** 配对核验结果（rinne.pair 的 verify）到诊断文本里「相同 / 不同 / 未知」的映射。 */
const TRACK_RELATION = { match: "same", diff: "different", unknown: "unknown" };
/** groupOrigin 到诊断文本里 origin 的映射（与 core 的来源名字表一一对应）。 */
const ORIGIN_NAME = { auto: "smart", manual: "manual", project: "restore" };
/** 配对状态里「有对端」的那些：诊断文本的 peer 一行只在这些状态下写字段，其余写 none。 */
const PEER_PRESENT = ["stale", "crossProcess", "crossTrack", "connectedUnverified", "connected", "connectedStill"];

/**
 * 预览用的诊断字段（两个角色共有的那几节）：值是预览里能给出的合理数字，不是真实环境；os、host 等写 preview。
 * model 是角色后端的模型（version、host、sampleRate、blockSize、group、groupOrigin、restored、scale、lang、pair）。
 */
export function previewDiagValues(role, model, core) {
    const pair = model.pair;
    const present = PEER_PRESENT.includes(pair.state);
    // 没有对端时核验结果不作数：track.relation 固定写 unknown（core 的 buildDiagReport：present ? relation : unknown）
    const relation = present ? (TRACK_RELATION[pair.verify] ?? "unknown") : "unknown";
    const v = {
        "product.product": role === "input" ? "Rinne Input" : "Rinne Output",
        "product.version": model.version,
        "product.build": "preview",
        "product.format": "preview",
        "product.host": model.host,
        "product.os": "preview",
        "product.arch": "preview",
        "backend.backend": "preview",
        "backend.ipc_abi": 1,
        "backend.prefix": "SnchRinne",
        "backend.group": model.group ?? "-",
        "backend.origin": model.group ? (ORIGIN_NAME[model.groupOrigin] ?? "none") : "none",
        "backend.pid": 0,
        "backend.pair": pair.state,
        "track.relation": relation,
        "track.hash": "none",
        "prepare.count": 1,
        "prepare.sr": model.sampleRate,
        "prepare.max": model.blockSize,
        "prepare.nonrealtime_seen": 0,
        "prepare.setstate_before_prepare": model.restored ? 1 : 0,
        "prepare.reset_calls": 0,
        "blocks.total": 4096,
        "blocks.zero_len": 0,
        "blocks.over_max": 0,
        "blocks.bypassed": 0,
        "blocks.max_n_seen": model.blockSize,
        "nsamples.min": model.blockSize,
        "nsamples.max": model.blockSize,
        "nsamples.hist": "0,0,0,0,4096,0,0,0",
        "ui.webview": "preview",
        "ui.scale": model.scale,
        "ui.lang": model.lang ?? core.ui.lang,
    };
    if (present) {
        Object.assign(v, {
            "peer.role": role === "input" ? "output" : "input",
            "peer.state": pair.state === "stale" ? "dormant" : "active",
            "peer.same_pid": pair.state === "crossProcess" ? 0 : 1,
            "peer.version": "0.0.0",
            "peer.sr": model.sampleRate,
            "peer.max_block": model.blockSize,
            "peer.ch": pair.peer.channels ?? 2,
            "peer.name": relation,
            "peer.hb_age_ms": pair.state === "stale" ? 2400 : 12,
        });
    }
    return v;
}

/** 预览用的最近事件：构造、prepare、选组、当前配对状态，参数含义见 core 的事件名字表。 */
export function previewDiagEvents(model) {
    const origin = { auto: 1, project: 2, manual: 3 }[model.groupOrigin] ?? 0;
    return [
        { tMs: 0, code: "construct" },
        { tMs: 12, code: "prepare", a: model.sampleRate, b: model.blockSize },
        { tMs: 15, code: "group_set", a: model.group ? GROUPS.indexOf(model.group) + 1 : 0, b: origin },
        { tMs: 480, code: "pair_state", a: PAIR_STATES.indexOf(model.pair.state) },
    ];
}

/**
 * 从诊断文本里抽出它用到的名字（「节名.键名」，另有 rinne-diag 与 events 两个整行名字），顺序同文本。
 * 认不出的行以 `?<行首>` 的形式混进结果，让白名单断言判红；`peer: none`（没有对端）不产生名字，别的节写 none 一概认不出。
 */
export function diagNamesOf(text) {
    const names = [];
    for (const line of String(text).split("\n")) {
        if (line === "") continue;
        if (line === "rinne-diag v1") names.push("rinne-diag");
        else if (/^events\(last \d+ of \d+\):$/.test(line)) names.push("events");
        else if (/^ {2}t\+\d+\.\d{3} [a-z_]+(?: \d+(?:\/\d+)?)?$/.test(line))
            continue; // 事件行
        else {
            const m = /^([a-z]+): (.*)$/.exec(line); // 带冒号的节；product、backend 两行没有冒号，节名取第一个键
            const body = m ? m[2] : line;
            if (m && m[1] === "peer" && body === "none") continue; // 只有 peer 一节有「没有对端」的写法
            const keys = [...body.matchAll(/(?:^| )([a-z][a-z0-9_]*)=/g)].map((x) => x[1]);
            if (keys.length === 0) names.push(`?${line.slice(0, 24)}`);
            else for (const k of keys) names.push(`${m ? m[1] : keys[0]}.${k}`);
        }
    }
    return names;
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
