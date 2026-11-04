// SPDX-License-Identifier: GPL-3.0-or-later
//
// 配对卡（O-06）：玻璃卡片 424×140，画成一条信号链：
//   正上方是状态灯一行（配对 pill 居中，「配对」小标在卡的左上角）；
//   中间是两个节点与一根连线：左边「Input 所在轨」、右边「本轨」，每个节点是色点、小标、名字（最多折两行，再长省略，title 里是全名）；
//     连线的颜色与虚实跟配对状态走（绿实线、琥珀实线、红虚线、灰虚线）；声道徽标骑在线上，轨道名核验徽标在线下；
//     未找到 Input 时左节点换成插入链示意，高亮 IN 槽；
//   链下是副行（七种说法之一，最多两行，居中）。
// DOM 顺序仍是「本轨、Input 所在轨」（读屏顺序不变），左右换位由 CSS 的 row-reverse 做。
// 状态灯这一排包在 data-tour="pair" 的容器里，首启引导第 3 步只亮这一排（副行不亮：说明框要放进设计盒）。
// 本模块只管结构与文字；颜色语义、徽标、插入链都用共享件。update 的入参是 view-model.js 的 deriveView 结果。
// 页面每秒会重发好几次同样的状态：只在文字或属性真的变了才写 DOM，插入链的 SVG 只在语言变时重建（重建会打断呼吸动画）。

import { el } from "../shared/dom.js";
import { createPairPill, createVerifyBadge, createChannelBadge } from "../shared/pair-status.js";
import { createChainDiagram } from "../shared/chain-diagram.js";
import { SUB_KEYS } from "./view-model.js";

/** 一条轨道行。 */
function createTrackRow(part, labelKey) {
    const label = el("span", { class: "rn-track__label", data: { part: `${part}.label` } });
    const dot = el("span", { class: "rn-track__dot", aria: { hidden: "true" }, data: { dot: "hollow" } });
    const name = el("span", { class: "rn-track__name", data: { part: `${part}.name` } });
    const verify = createVerifyBadge();
    const channel = createChannelBadge();
    verify.el.hidden = true;
    channel.el.hidden = true;
    const end = el("span", { class: "rn-track__end" }, verify.el, channel.el);
    const chain = createChainDiagram({ compact: true });
    chain.el.classList.add("rn-track__chain");
    chain.el.hidden = true;
    const root = el("div", { class: "rn-track", data: { part, state: "noName" } }, label, dot, name, chain.el, end);
    let chainLang = "";
    return {
        el: root,
        labelKey,
        /**
         * @param {{mode:"name"|"noName"|"chain", name?:string|null, color?:string|null,
         *          verify?:string|null, peerChannel?:string|null, sendChannel?:string|null, lang:string,
         *          t:(k:string,v?:object)=>string}} p
         */
        update(p) {
            const { t } = p;
            label.textContent = t(labelKey);
            root.setAttribute("data-state", p.mode);
            const isChain = p.mode === "chain";
            dot.hidden = isChain;
            name.hidden = isChain;
            chain.el.hidden = !isChain;
            if (isChain) {
                // 插入链是静态说明里的「提示把 Input 放进链首」：高亮 IN 槽。语言变了标签才变，所以只在语言变时重建
                if (chainLang !== p.lang) {
                    chain.update({ highlight: "in", t });
                    chainLang = p.lang;
                }
            } else {
                chainLang = "";
                const named = p.mode === "name" && p.name;
                name.textContent = named ? p.name : t("out.pair.noName");
                if (named) name.setAttribute("title", p.name);
                else name.removeAttribute("title");
                name.setAttribute("data-noname", named ? "0" : "1");
                // 没名字的行画空心点；有名字有颜色画实心。颜色已在 view-model 里按白名单校验过
                if (named && p.color) {
                    dot.style.background = p.color;
                    dot.removeAttribute("data-dot");
                } else {
                    dot.style.background = "";
                    dot.setAttribute("data-dot", "hollow");
                }
            }
            const showEnd = Boolean(p.verify) && !isChain;
            verify.el.hidden = !showEnd;
            if (showEnd) verify.update({ kind: p.verify, t });
            // 声道徽标：两端声道数相同是中性徽标，不同是琥珀的「Input→Output」
            const showCh = showEnd && Boolean(p.peerChannel);
            channel.el.hidden = !showCh;
            if (showCh) channel.update({ input: p.peerChannel, output: p.sendChannel, t });
        },
    };
}

export function createPairCard() {
    const pill = createPairPill({ gb: "output.pair.pill" });
    const eyebrow = el("span", { class: "sc-eyebrow rn-paircard__eyebrow" });
    const sub = el("div", { class: "rn-paircard__sub", data: { part: "pair.sub", state: "searching" } });
    const head = el(
        "div",
        { class: "rn-paircard__head" },
        el("div", { class: "rn-paircard__top", data: { tour: "pair" } }, pill.el, eyebrow),
        sub,
    );
    const thisRow = createTrackRow("track.this", "out.pair.thisTrack");
    const inputRow = createTrackRow("track.input", "out.pair.inputTrack");
    const tracks = el(
        "div",
        { class: "rn-paircard__tracks", data: { part: "pair.tracks", state: "noName" } },
        thisRow.el,
        inputRow.el,
    );
    const root = el(
        "div",
        { class: "rn-paircard", role: "group", data: { gb: "output.pair", state: "searching" } },
        head,
        tracks,
    );
    return {
        el: root,
        /** @param {{view:ReturnType<import("./view-model.js").deriveView>, t:(k:string,v?:object)=>string}} p */
        update({ view, t }) {
            const lang = view.lang;
            root.setAttribute("data-state", view.pair.state);
            root.setAttribute("aria-label", t("out.pair.eyebrow"));
            pill.update({ state: view.pair.state, role: "output", flowing: view.pair.flowing, t });
            eyebrow.textContent = t("out.pair.eyebrow");
            sub.textContent = t(SUB_KEYS[view.pair.sub]);
            sub.setAttribute("data-state", view.pair.sub);
            const tr = view.tracks;
            thisRow.update({
                mode: tr.thisTrack.name ? "name" : "noName",
                name: tr.thisTrack.name,
                color: tr.thisTrack.color,
                lang,
                t,
            });
            inputRow.update({
                mode: tr.input.mode,
                name: tr.input.name,
                color: tr.input.color,
                verify: tr.verify,
                peerChannel: tr.peerChannel,
                sendChannel: tr.sendChannel,
                lang,
                t,
            });
            tracks.setAttribute("data-state", tr.input.mode === "name" ? tr.verify || "name" : tr.input.mode);
        },
    };
}
