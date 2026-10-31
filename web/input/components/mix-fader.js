// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mix 推子：一根横向玻璃管，管内两路分量电平（干声从左端、回送从右端各占半管，0 dBFS 时在中线相遇），
// 骑在管上的卡箍是推子把手。液柱与峰线的宽度由 meter.js 的弹道每帧写到 levels 里的节点上（--lv、--pk），
// 本件只管读数、卡箍位置与可访问性。拖动在整条推子上都可以按下，按下卡箍立刻跟手。

import { el } from "../../shared/dom.js";
import { setAttr, setText, setVar } from "./util.js";

/** 创建 Mix 推子。 */
export function createMixFader() {
    const label = el("span", { class: "in-mix__label" });
    const readout = el("span", { class: "in-mix__readout", "data-part": "mix.readout" });
    const slot = el("span", { class: "in-mix__slot" }, readout);
    const head = el("div", { class: "in-mix__head" }, label, slot);

    const dryLevel = el("div", { class: "sc-tube__liquid in-liq in-liq--dry", "data-part": "mix.dryLevel" });
    const wetLevel = el("div", { class: "sc-tube__liquid in-liq in-liq--wet", "data-part": "mix.wetLevel" });
    const dryPeak = el("div", { class: "sc-tube__peak in-pk in-pk--dry", "data-part": "mix.dryPeak" });
    const wetPeak = el("div", { class: "sc-tube__peak in-pk in-pk--wet", "data-part": "mix.wetPeak" });
    const mid = el("div", { class: "in-mix__mid", "aria-hidden": "true" });
    const tube = el(
        "div",
        { class: "sc-tube in-tube in-mix__tube" },
        el("div", { class: "sc-tube__slot" }, dryLevel, wetLevel, dryPeak, wetPeak, mid),
        el("div", { class: "sc-tube__gloss", "aria-hidden": "true" }),
    );
    const collar = el(
        "div",
        { class: "in-mix__collar", "data-part": "mix.collar" },
        el("span", { class: "in-mix__grip", "aria-hidden": "true" }),
    );
    const fader = el(
        "div",
        {
            class: "in-mix__fader",
            role: "slider",
            tabindex: "0",
            "data-glow": "1",
            "data-part": "mix.fader",
            aria: { valuemin: 0, valuemax: 100 },
        },
        tube,
        collar,
    );
    const endDry = el("span", { class: "in-mix__legend-text" });
    const endRet = el("span", { class: "in-mix__legend-text" });
    const legend = el(
        "div",
        { class: "in-mix__legend" },
        el(
            "span",
            { class: "in-mix__key" },
            el("span", { class: "in-mix__swatch in-mix__swatch--dry", "aria-hidden": "true" }),
            endDry,
        ),
        el(
            "span",
            { class: "in-mix__key in-mix__key--right" },
            el("span", { class: "in-mix__swatch", "aria-hidden": "true" }),
            endRet,
        ),
    );
    const root = el("div", { class: "in-mix", "data-gb": "input.mix.knob" }, head, fader, legend);

    /**
     * @param {object} p
     * @param {number} p.value Mix 百分比 0..100
     * @param {boolean} p.drag 拖动中
     * @param {boolean} p.auto 宿主自动化中
     * @param {(k:string)=>string} p.t
     */
    function update(p) {
        const v = Math.min(100, Math.max(0, p.value));
        const shown = `${Math.round(v)}%`;
        setText(readout, shown);
        setText(label, p.t("in.mix.label"));
        setText(endDry, p.t("in.mix.endDry"));
        setText(endRet, p.t("in.mix.endReturn"));
        setVar(fader, "--vol", `${v}%`);
        setAttr(fader, "aria-valuenow", Math.round(v));
        setAttr(fader, "aria-valuetext", shown);
        setAttr(fader, "aria-label", p.t("in.mix.label"));
        setAttr(root, "data-state", p.drag ? "drag" : p.auto ? "auto" : "normal");
    }

    /** 宿主的值还没到：标签照写，读数写「—」（卡箍与液柱由样式表收起）。 */
    function pending(t) {
        setText(readout, "—");
        setText(label, t("in.mix.label"));
        setText(endDry, t("in.mix.endDry"));
        setText(endRet, t("in.mix.endReturn"));
        setAttr(fader, "aria-valuenow", null);
        setAttr(fader, "aria-valuetext", "—");
        setAttr(fader, "aria-label", t("in.mix.label"));
        setAttr(root, "data-state", "pending");
    }

    return {
        el: root,
        pending,
        fader,
        slot,
        readout,
        collar,
        /** meter.js 的弹道往这四个节点写 --lv 与 --pk。 */
        levels: { dry: dryLevel, wet: wetLevel, dryPeak, wetPeak },
        update,
    };
}
