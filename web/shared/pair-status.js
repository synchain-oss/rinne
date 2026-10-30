// SPDX-License-Identifier: GPL-3.0-or-later
//
// 配对状态 pill（15 态）、轨道名核验徽标、声道徽标。Input、Output 两页共用。
//
// 状态与色语义取自 docs/UI_BRIDGE.md 的配对状态一节：
//   红  ipcUnavailable、abiMismatch、srMismatch、conflict、unassigned：回送已停、被拒或不可用，需要用户动作；
//   琥珀 stale、crossProcess、crossTrack：仍在工作但有风险；
//   灰  searching、waitingAudio、validator、bypassed（仅 Output）：主动关闭、宿主暂停或离线；
//   绿  connectedUnverified、connected、connectedStill：已连接。connectedStill 是「已连接但回送静音」，
//       文案与 connected 相同，绿点不脉冲；另两个绿态只在音频流动时才脉冲（2.4 s）。
// 颜色之外还有形状冗余：点是实心圆，文字是状态词本身，色盲用户靠文字读状态。
//
// 模型函数（pairModel 等）是纯函数，不碰 DOM，Node 里可直接断言；create* 才建节点。

import { el } from "./dom.js";
import { icon } from "./icons.js";

/** 15 个配对状态，按桥契约的表序之外再按色语义分组排列。label 为 null 的两条按角色取词条。 */
export const PAIR_STATES = Object.freeze([
    { state: "ipcUnavailable", tone: "red", key: "common.pair.ipcUnavailable" },
    { state: "abiMismatch", tone: "red", key: "common.pair.abiMismatch" },
    { state: "srMismatch", tone: "red", key: "common.pair.srMismatch" },
    { state: "conflict", tone: "red", key: "common.pair.conflict" },
    { state: "unassigned", tone: "red", key: "common.pair.unassigned" },
    { state: "stale", tone: "amber", key: "common.pair.stale" },
    { state: "crossProcess", tone: "amber", key: "common.pair.crossProcess" },
    { state: "crossTrack", tone: "amber", key: "common.pair.crossTrack" },
    { state: "searching", tone: "gray", key: null },
    { state: "waitingAudio", tone: "gray", key: "common.pair.waitingAudio" },
    { state: "validator", tone: "gray", key: "common.pair.validator" },
    { state: "bypassed", tone: "gray", key: "out.pair.bypassed", only: "output" },
    { state: "connectedUnverified", tone: "green", key: "common.pair.connectedUnverified" },
    { state: "connected", tone: "green", key: "common.pair.connected" },
    { state: "connectedStill", tone: "green", key: "common.pair.connected" },
]);

const BY_STATE = new Map(PAIR_STATES.map((d) => [d.state, d]));

/** 某角色下可能出现的状态：bypassed 只由 Output 产生，Input 的配对帧里不会有它。 */
export function statesFor(role) {
    return PAIR_STATES.filter((d) => !d.only || d.only === role).map((d) => d.state);
}

/**
 * 一个状态在某角色下的词条键。searching 的文案各页自己的词典里有（in.pair.searching、out.pair.searching），
 * 因为「未找到 Output」与「未找到 Input」指的是对端，两页不同。
 */
export function pairLabelKey(state, role) {
    const d = BY_STATE.get(state);
    if (!d || (d.only && d.only !== role)) return null;
    if (state === "searching") return role === "input" ? "in.pair.searching" : "out.pair.searching";
    return d.key;
}

/**
 * pill 的展示模型。未知状态，或 Input 收到 bypassed，返回 null，由调用方决定怎么处理（本模块的 DOM 版会隐藏）。
 * @param {{state:string, role:"input"|"output", flowing?:boolean}} o flowing：宿主此刻有音频在流动
 */
export function pairModel({ state, role, flowing }) {
    const d = BY_STATE.get(state);
    if (!d || (d.only && d.only !== role)) return null;
    return {
        state,
        tone: d.tone,
        labelKey: pairLabelKey(state, role),
        // 只有「已连接」与「已连接（未核验）」在音频流动时脉冲；connectedStill 永不脉冲
        pulse: (state === "connected" || state === "connectedUnverified") && flowing === true,
    };
}

/**
 * 建配对 pill。root 上的 data-gb 由调用方给（input.pair.pill 或 output.pair.pill），data-state 是桥状态名。
 * @param {{gb?:string}} [o]
 */
export function createPairPill(o = {}) {
    const dot = el("span", { class: "sc-pill__dot", aria: { hidden: "true" } });
    const text = el("span", { class: "rn-pair__text" });
    const root = el("span", { class: "sc-pill rn-pair", data: { gb: o.gb } }, dot, text);
    return {
        el: root,
        /** @param {{state:string, role:"input"|"output", flowing?:boolean, t:(k:string,v?:object)=>string}} p */
        update(p) {
            const m = pairModel(p);
            root.hidden = !m;
            if (!m) {
                root.setAttribute("data-state", "invalid");
                return null;
            }
            root.setAttribute("data-state", m.state);
            root.setAttribute("data-tone", m.tone);
            // 脉冲规则写在 .sc-pill[data-pulse="1"] .sc-pill__dot 上，属性必须在 pill 根上，不能写在点上
            root.setAttribute("data-pulse", m.pulse ? "1" : "0");
            text.textContent = p.t(m.labelKey);
            return m;
        },
    };
}

// ---------------------------------------------------------------- 轨道名核验徽标

const VERIFY = Object.freeze({
    match: { icon: "eq", key: "common.verify.match", tone: "green" },
    diff: { icon: "neq", key: "common.verify.diff", tone: "amber" },
    unknown: { icon: "q", key: "common.verify.unknown", tone: "gray" },
});

/** 核验徽标的展示模型；kind 不是 match、diff、unknown 时返回 null。 */
export function verifyModel(kind) {
    return Object.prototype.hasOwnProperty.call(VERIFY, kind) ? { kind, ...VERIFY[kind] } : null;
}

/** 核验徽标：等号（一致，绿）、不等号（不同，琥珀）、问号（无法核验，灰），后跟文字。 */
export function createVerifyBadge() {
    const root = el("span", { class: "rn-verify" });
    return {
        el: root,
        update({ kind, t }) {
            const m = verifyModel(kind);
            root.hidden = !m;
            root.textContent = "";
            if (!m) return;
            root.setAttribute("data-verify", m.kind);
            root.setAttribute("data-tone", m.tone);
            root.append(icon(m.icon, 12), el("span", { class: "rn-verify__text", text: t(m.key) }));
        },
    };
}

// ---------------------------------------------------------------- 声道徽标

const CH_KEY = Object.freeze({ st: "common.ch.st", mono: "common.ch.mono" });

/**
 * 声道徽标的展示模型。两端声道数相同时是中性徽标（ST 或 MONO）；不同时是琥珀的「Input→Output」徽标，
 * 并带 title 说明两端各是几个声道。
 * @param {"st"|"mono"|null} input  Input 端声道
 * @param {"st"|"mono"|null} output Output 端声道；缺省（只知道本端）时按单端徽标画
 */
export function channelModel(input, output) {
    const known = (c) => c === "st" || c === "mono";
    if (!known(input)) return null;
    if (!known(output) || output === input) return { mismatch: false, ch: input };
    return { mismatch: true, from: input, to: output };
}

/** 声道徽标。 */
export function createChannelBadge() {
    const root = el("span", { class: "sc-badge rn-ch" });
    return {
        el: root,
        update({ input, output, t }) {
            const m = channelModel(input, output);
            root.hidden = !m;
            root.textContent = "";
            root.removeAttribute("title");
            if (!m) return;
            if (!m.mismatch) {
                root.setAttribute("data-ch", m.ch);
                root.removeAttribute("data-tone");
                root.append(t(CH_KEY[m.ch]));
                return;
            }
            root.setAttribute("data-ch", `${m.from}>${m.to}`);
            root.setAttribute("data-tone", "amber");
            root.append(t(CH_KEY[m.from]), icon("arrowR", 9), t(CH_KEY[m.to]));
            root.setAttribute("title", t("common.ch.mismatchTip", { a: t(CH_KEY[m.from]), b: t(CH_KEY[m.to]) }));
        },
    };
}
