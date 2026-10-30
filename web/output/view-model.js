// SPDX-License-Identifier: GPL-3.0-or-later
//
// Output 页面的视图模型：把桥的两路快照（rinne.state、rinne.pair）整理成各元件的入参。
// 全是纯函数，不碰 DOM，Node 里可以直接断言；页面（page.js）只负责把结果交给共享件。
//
// 为什么页面自己推导而不是让原生给「要显示哪些」：配对状态是桥契约里的 15 个枚举，消息位的 14 条里有十条
// 正是这 15 个状态的文字版，推导规则放在这里一处，三语、冒烟与预览都读同一份。
// 契约里没有载荷的四条（ipcSandbox、newerProject、stateDecodeFailed、outOfOrder）读 rinne.state 的可缺省字段 notices。
//
// 快照的校验口径：字段缺失、类型不对一律回落到安全的默认值，不抛错；seq 不比已收到的大就整帧丢弃
// （mock 会在新帧之后补发一帧旧快照，真桥在 1 Hz 兜底与变更即发交叠时也可能乱序）。

import { GROUP_LETTERS, groupsFromMask } from "../shared/group-pills.js";
import { PAIR_STATES } from "../shared/pair-status.js";
import { LANGS } from "../shared/i18n.js";

/** 页面认得的 15 个配对状态（Output 侧：含 bypassed）。 */
const KNOWN_PAIR = new Set(PAIR_STATES.map((d) => d.state));
const ORIGINS = new Set(["auto", "manual", "project"]);
const VERIFY = new Set(["match", "diff", "unknown"]);
/** notices 里认得的 id：契约里没有载荷的那四条消息。 */
export const NOTICE_IDS = Object.freeze(["ipcSandbox", "newerProject", "stateDecodeFailed", "outOfOrder"]);

/** 名字上限：宿主给的轨道名不受控，超长的先截断再交给省略号；title 里仍是截断后的全名。 */
const NAME_MAX = 200;
/** 版本串上限，同上。 */
const VERSION_MAX = 24;

/** 轨道名等宿主文字：去控制字符与首尾空白，空串当没有。 */
export function cleanText(v, max = NAME_MAX) {
    if (typeof v !== "string") return null;
    // eslint 之外的控制字符一律去掉（含换行与制表符）：名字是单行文字
    const s = v
        .replace(/[\u0000-\u001f\u007f-\u009f]/g, " ")
        .replace(/\s+/g, " ")
        .trim();
    return s ? s.slice(0, max) : null;
}

/** 轨道颜色只认 #rgb、#rrggbb 与 rgb()/rgba() 的数字写法，其余当没有（颜色只写进点的 background）。 */
export function cleanColor(v) {
    if (typeof v !== "string") return null;
    const s = v.trim();
    if (/^#(?:[0-9a-fA-F]{3}|[0-9a-fA-F]{6})$/.test(s)) return s;
    if (/^rgba?\(\s*\d{1,3}\s*[, ]\s*\d{1,3}\s*[, ]\s*\d{1,3}\s*(?:[,/]\s*(?:0|1|0?\.\d+)\s*)?\)$/.test(s)) return s;
    return null;
}

/** 声道：1 或 "MONO" 是单声道，2 或 "ST" 是立体声；认不出给 null。 */
export function channelKind(v) {
    if (v === 1 || (typeof v === "string" && v.toLowerCase() === "mono")) return "mono";
    if (v === 2 || (typeof v === "string" && v.toLowerCase() === "st")) return "st";
    return null;
}

const finitePositive = (v) => typeof v === "number" && Number.isFinite(v) && v > 0;

/** 还没收到任何快照时的默认状态：只用来铺第一帧，页面此时把内容层藏起来（见 page.js）。 */
export function defaultState() {
    return {
        seq: -1,
        version: "",
        lang: "zh",
        langChosen: true,
        uiScale: 1,
        takeover: false,
        group: null,
        groupOrigin: "manual",
        diagLog: false,
        sampleRate: 0,
        send: { channels: "ST", sampleRate: 0, bypassed: false, hostPaused: false },
        notices: [],
        build: null,
    };
}

export function defaultPair() {
    return {
        seq: -1,
        state: "searching",
        groupMask: 0,
        peer: { name: null, color: null, channels: null, version: null, holder: null, sampleRate: null },
        self: { name: null, color: null },
        verify: "unknown",
    };
}

/**
 * 校验并整理 rinne.state。返回新的状态；raw 不是对象或 seq 不比 prev 大时返回 null（整帧丢弃）。
 * prev 缺省时不比序号。
 */
export function readState(raw, prev) {
    if (!raw || typeof raw !== "object") return null;
    const seq = typeof raw.seq === "number" && Number.isFinite(raw.seq) ? raw.seq : null;
    if (prev && seq !== null && prev.seq >= 0 && seq <= prev.seq) return null;
    const base = prev || defaultState();
    const send = raw.send && typeof raw.send === "object" ? raw.send : {};
    const sampleRate = finitePositive(raw.sampleRate) ? raw.sampleRate : base.sampleRate;
    const notices = [];
    if (Array.isArray(raw.notices)) {
        for (const n of raw.notices) {
            if (!n || typeof n !== "object" || !NOTICE_IDS.includes(n.id)) continue;
            if (notices.some((x) => x.id === n.id)) continue;
            notices.push({
                id: n.id,
                a: cleanText(String(n.a ?? ""), VERSION_MAX),
                b: cleanText(String(n.b ?? ""), VERSION_MAX),
            });
        }
    }
    const b = raw.build && typeof raw.build === "object" ? raw.build : null;
    return {
        seq: seq === null ? base.seq : seq,
        version: cleanText(raw.version, VERSION_MAX) || base.version,
        lang: LANGS.includes(raw.lang) ? raw.lang : base.lang,
        langChosen: raw.langChosen !== false,
        uiScale: finitePositive(raw.uiScale) ? raw.uiScale : base.uiScale,
        takeover: raw.takeover === true,
        group: typeof raw.group === "string" && GROUP_LETTERS.includes(raw.group) ? raw.group : null,
        groupOrigin: ORIGINS.has(raw.groupOrigin) ? raw.groupOrigin : "manual",
        diagLog: raw.diagLog === true,
        sampleRate,
        send: {
            channels: channelKind(send.channels) === "mono" ? "MONO" : "ST",
            sampleRate: finitePositive(send.sampleRate) ? send.sampleRate : sampleRate,
            bypassed: send.bypassed === true,
            hostPaused: send.hostPaused === true,
        },
        notices,
        build: b
            ? {
                  sha: cleanText(b.sha, 16),
                  fmt: cleanText(b.fmt, 12),
                  arch: cleanText(b.arch, 24),
              }
            : null,
    };
}

/** 校验并整理 rinne.pair。未知的配对状态整帧丢弃（不猜）；规则同 readState。 */
export function readPair(raw, prev) {
    if (!raw || typeof raw !== "object") return null;
    if (typeof raw.state !== "string" || !KNOWN_PAIR.has(raw.state)) return null;
    const seq = typeof raw.seq === "number" && Number.isFinite(raw.seq) ? raw.seq : null;
    if (prev && seq !== null && prev.seq >= 0 && seq <= prev.seq) return null;
    const peer = raw.peer && typeof raw.peer === "object" ? raw.peer : {};
    const self = raw.self && typeof raw.self === "object" ? raw.self : {};
    return {
        seq: seq === null ? (prev ? prev.seq : -1) : seq,
        state: raw.state,
        groupMask: Number.isInteger(raw.groupMask) ? raw.groupMask & 0xff : 0,
        peer: {
            name: cleanText(peer.name),
            color: cleanColor(peer.color),
            channels: channelKind(peer.channels),
            version: cleanText(typeof peer.version === "string" ? peer.version : null, VERSION_MAX),
            holder: cleanText(peer.holder),
            sampleRate: finitePositive(peer.sampleRate) ? peer.sampleRate : null,
        },
        self: { name: cleanText(self.name), color: cleanColor(self.color) },
        verify: VERIFY.has(raw.verify) ? raw.verify : "unknown",
    };
}

// ------------------------------------------------------------------ 配对卡的副行与页脚提示

/** 副行的七种说法，键是配对状态归并后的名字。 */
export const SUB_KEYS = Object.freeze({
    searching: "out.pair.sub.searching",
    connected: "out.pair.sub.connected",
    connectedUnverified: "out.pair.sub.connectedUnverified",
    inputMuted: "out.pair.sub.inputMuted",
    stale: "out.pair.sub.stale",
    bypassed: "out.pair.sub.bypassed",
    notice: "out.pair.sub.notice",
});

/** 配对状态 → 副行的哪一种。没有专句的状态（冲突、版本、采样率、等待音频……）一律指向下面的消息位。 */
export function subKind(pairState) {
    switch (pairState) {
        case "searching":
            return "searching";
        case "connected":
            return "connected";
        case "connectedUnverified":
            return "connectedUnverified";
        case "connectedStill":
            return "inputMuted";
        case "stale":
            return "stale";
        case "bypassed":
            return "bypassed";
        default:
            return "notice";
    }
}

/** 页脚提示三种说法。 */
export const HINT_KEYS = Object.freeze({
    unpaired: "out.hint.unpaired",
    connected: "out.hint.connected",
    conflict: "out.hint.conflict",
});

/** 「环路这头已经接上、对端在」的状态：页脚写「全部调节都在 Rinne Input」。 */
const CONNECTEDISH = new Set([
    "connected",
    "connectedUnverified",
    "connectedStill",
    "stale",
    "crossProcess",
    "crossTrack",
    "bypassed",
]);

export function hintKind(pairState) {
    if (pairState === "conflict") return "conflict";
    return CONNECTEDISH.has(pairState) ? "connected" : "unpaired";
}

/** 有对端可核对的状态：这些状态下第二条轨道行才带核验徽标与声道徽标。 */
const PEER_PRESENT = new Set([
    "connected",
    "connectedUnverified",
    "connectedStill",
    "stale",
    "crossProcess",
    "crossTrack",
    "abiMismatch",
    "srMismatch",
    "bypassed",
]);

// ------------------------------------------------------------------ 发送状态行

export const SEND_KEYS = Object.freeze({
    ok: "out.send.ok",
    bypassed: "out.send.bypassed",
    idle: "out.send.idle",
    none: "out.send.none",
});

/** 没有接入环路的状态：发送行写一个破折号。 */
const NOT_IN_LOOP = new Set([
    "searching",
    "conflict",
    "unassigned",
    "ipcUnavailable",
    "abiMismatch",
    "srMismatch",
    "validator",
]);

/** 采样率写成 kHz 的数字串：48000 → "48"，44100 → "44.1"。 */
export function srLabel(hz) {
    if (!finitePositive(hz)) return "—";
    return String(Math.round(hz / 100) / 10);
}

/**
 * 发送状态行：{ kind: ok|bypassed|idle|none, ch: ST|MONO, sr }。
 * 旁路最先判（宿主旁路了 Output，写入静音）；宿主没在调用处理，或配对卡在等音频，写「暂停写入」；
 * 没接入环路的状态写破折号；其余是发送中。
 */
export function sendView(state, pair) {
    const s = state.send;
    let kind = "ok";
    if (s.bypassed || pair.state === "bypassed") kind = "bypassed";
    else if (s.hostPaused || pair.state === "waitingAudio") kind = "idle";
    else if (NOT_IN_LOOP.has(pair.state)) kind = "none";
    return { kind, ch: s.channels, sr: srLabel(s.sampleRate || state.sampleRate) };
}

// ------------------------------------------------------------------ 消息位

/** 消息表：按优先级排好（takeover 是确认条，不在表里）。tone 是颜色，action 是动作名（共享件 ACTIONS 的名字）。 */
export const MESSAGES = Object.freeze([
    { id: "conflict", key: "band.out.conflict", tone: "red", action: "repair" },
    { id: "unassigned", key: "band.out.unassigned", tone: "red" },
    { id: "abi", key: "band.out.abi", tone: "red", action: "guide" },
    { id: "srMismatch", key: "band.out.srMismatch", tone: "red" },
    { id: "ipc", key: "band.out.ipc", tone: "red", action: "guide" },
    { id: "ipcSandbox", key: "band.out.ipcSandbox", tone: "red", action: "guide" },
    { id: "newerProject", key: "band.out.newerProject", tone: "amber" },
    { id: "stateDecodeFailed", key: "band.out.stateDecodeFailed", tone: "amber" },
    { id: "stale", key: "band.out.stale", tone: "amber" },
    { id: "crossProcess", key: "band.out.crossProcess", tone: "amber", action: "guide" },
    { id: "crossTrack", key: "band.out.crossTrack", tone: "amber" },
    { id: "outOfOrder", key: "band.out.outOfOrder", tone: "amber" },
    { id: "waitingAudio", key: "band.out.waitingAudio", tone: "gray" },
    { id: "validator", key: "band.out.validator", tone: "gray" },
]);

/** 接管确认条的词条键。 */
export const TAKEOVER_KEY = "band.out.takeover";

/** 空闲时消息位里的灰字。 */
export const IDLE_KEY = "out.hint.idle";

/**
 * 当前成立的消息 id 集合。跨轨除了配对状态本身，还看核验结果：配对状态按原生的优先级只取一个，
 * 而「两端名字都已知且不同」这个条件可以和别的状态同时成立（比如版本不匹配的同时轨道名也不同）。
 */
function activeIds(state, pair) {
    const n = new Set(state.notices.map((x) => x.id));
    const ps = pair.state;
    const on = new Set();
    if (ps === "conflict") on.add("conflict");
    if (ps === "unassigned") on.add("unassigned");
    if (ps === "abiMismatch") on.add("abi");
    if (ps === "srMismatch") on.add("srMismatch");
    // 共享内存不可用：有沙盒通知时只报沙盒那条（更具体），不再叠一条泛泛的
    if (n.has("ipcSandbox")) on.add("ipcSandbox");
    else if (ps === "ipcUnavailable") on.add("ipc");
    if (n.has("newerProject")) on.add("newerProject");
    if (n.has("stateDecodeFailed")) on.add("stateDecodeFailed");
    if (ps === "stale") on.add("stale");
    if (ps === "crossProcess") on.add("crossProcess");
    if (ps === "crossTrack" || (pair.verify === "diff" && PEER_PRESENT.has(ps))) on.add("crossTrack");
    if (n.has("outOfOrder")) on.add("outOfOrder");
    if (ps === "waitingAudio") on.add("waitingAudio");
    if (ps === "validator") on.add("validator");
    return on;
}

/** 占位符缺值时统一写的破折号：版本号、组号、采样率都一样，不让 {x} 原样上屏。 */
const MISSING = "—";

/** 占位符的取值；缺的一律写 MISSING。 */
function valsFor(id, state, pair) {
    const q = (v) => (v === null || v === undefined || v === "" ? MISSING : String(v));
    const hz = (v) => (finitePositive(v) ? String(Math.round(v)) : MISSING);
    switch (id) {
        case "conflict":
            return { g: q(state.group) };
        case "abi":
            return { a: q(state.version), b: q(pair.peer.version) };
        case "srMismatch":
            return { a: hz(state.sampleRate), b: hz(pair.peer.sampleRate) };
        case "newerProject": {
            const n = state.notices.find((x) => x.id === "newerProject");
            return { a: q(n && n.a), b: q(n && n.b) };
        }
        default:
            return undefined;
    }
}

/** 共享消息位的入参：当前成立的全部消息（已按优先级排好）。 */
export function messagesFor(state, pair) {
    const on = activeIds(state, pair);
    return MESSAGES.filter((m) => on.has(m.id)).map((m) => {
        const msg = { id: m.id, key: m.key, tone: m.tone, vals: valsFor(m.id, state, pair) };
        if (m.action) msg.action = { name: m.action };
        return msg;
    });
}

// ------------------------------------------------------------------ 整页

/** L2 危险描边：只有冲突和版本不匹配。 */
export function isDanger(pairState) {
    return pairState === "conflict" || pairState === "abiMismatch";
}

/**
 * 整页视图模型：纯数据，页面拿它的 JSON 签名判断要不要重画。
 * @param {ReturnType<typeof defaultState>} state
 * @param {ReturnType<typeof defaultPair>} pair
 */
export function deriveView(state, pair) {
    const ps = pair.state;
    const send = sendView(state, pair);
    const peerHere = PEER_PRESENT.has(ps);
    const peerName = pair.peer.name;
    const bypass = state.send.bypassed || ps === "bypassed";
    return {
        lang: state.lang,
        version: state.version,
        danger: isDanger(ps),
        hostBypass: bypass,
        takeover: state.takeover,
        group: {
            group: state.group,
            unassigned: ps === "unassigned",
            auto: state.group && state.groupOrigin === "auto" ? (ps === "searching" ? "fallback" : true) : false,
            peers: groupsFromMask(pair.groupMask),
            disabled: state.takeover,
        },
        pair: {
            state: ps,
            // 宿主有音频在走才脉冲：宿主暂停或本实例被旁路时静止
            flowing: !state.send.hostPaused && !bypass,
            sub: subKind(ps),
        },
        tracks: {
            thisTrack: { name: pair.self.name, color: pair.self.color },
            input:
                peerName === null && ps === "searching"
                    ? { mode: "chain" }
                    : { mode: peerName === null ? "noName" : "name", name: peerName, color: pair.peer.color },
            verify: peerHere ? pair.verify : null,
            peerChannel: peerHere ? pair.peer.channels : null,
            sendChannel: state.send.channels === "MONO" ? "mono" : "st",
        },
        send,
        messages: messagesFor(state, pair),
        confirm: state.takeover ? { key: TAKEOVER_KEY, g: state.group || MISSING } : null,
        hint: hintKind(ps),
    };
}
