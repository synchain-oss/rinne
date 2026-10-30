// SPDX-License-Identifier: GPL-3.0-or-later
//
// 电平卡（右栏下，248×102）：输入、回送（增益前）、输出三根玻璃管，量程 -60 到 0 dBFS。
// 峰线高于 -3 dBFS 转警戒色，读数在峰值超过 -0.1 dBFS 时变红；点读数清除峰值保持。
// 液柱与峰线由 meter.js 的弹道每帧写 --lv、--pk；回送被切断时管内写「已静音」，淡入期间标签后带「淡入中」。

import { el } from "../../shared/dom.js";
import { fmtLevel, levelPct, peakState } from "../model.js";
import { setAttr, setHidden, setText, setVar } from "./util.js";

const ROWS = [
    { id: "in", label: "in.meter.in" },
    { id: "ret", label: "in.meter.ret", tip: "in.meter.retTip" },
    { id: "out", label: "in.meter.out" },
];

/**
 * @param {object} ctx 页面上下文
 * @param {object} o
 * @param {(id:string)=>void} o.onClearPeak 点读数
 */
export function createMeterCard(ctx, o) {
    const rows = {};
    const eyebrow = el("span", { class: "sc-eyebrow in-meters__eyebrow" });
    const list = el("div", { class: "in-meters__rows" });
    for (const r of ROWS) {
        const label = el("span", { class: "in-meter__label" });
        const fade = el("span", { class: "in-meter__fade" });
        const liquid = el("div", { class: "sc-tube__liquid in-liq" });
        const peak = el("div", { class: "sc-tube__peak" });
        setHidden(peak, true);
        const muted = el("span", { class: "in-meter__muted" });
        setHidden(muted, true);
        const track = el(
            "div",
            {
                class: "sc-tube in-tube in-meter__track",
                role: "meter",
                "data-part": "meter.track",
                aria: { valuemin: -60, valuemax: 0 },
            },
            el("div", { class: "sc-tube__slot" }, liquid, peak),
            el("div", { class: "sc-tube__gloss", "aria-hidden": "true" }),
            muted,
        );
        const readout = el("button", {
            class: "in-meter__readout",
            type: "button",
            "data-part": "meter.readout",
            on: { click: () => o.onClearPeak(r.id) },
        });
        const name = el("span", { class: "in-meter__name" }, label, fade);
        const row = el("div", { class: "in-meter", "data-row": r.id }, name, track, readout);
        list.append(row);
        rows[r.id] = {
            row,
            name,
            label,
            fade,
            liquid,
            peak,
            muted,
            track,
            readout,
            spec: r,
            lastText: "",
            lastRatio: -1,
        };
    }
    const root = el(
        "section",
        { class: "sc-card in-card in-meters", "data-gb": "input.meters" },
        el("div", { class: "in-meters__head" }, eyebrow),
        list,
    );

    /** 弹道写一路的状态。 */
    function write(id, st) {
        const r = rows[id];
        if (!r) return;
        const lv = levelPct(st.db);
        const pk = levelPct(st.peakDb);
        setVar(r.liquid, "--lv", `${lv.toFixed(1)}%`);
        setVar(r.peak, "--pk", `${pk.toFixed(1)}%`);
        setHidden(r.peak, pk <= 0);
        const ps = peakState(st.peakDb);
        const alert = ps === "near" || ps === "over" ? "1" : "0";
        setAttr(r.peak, "data-alert", alert);
        const none = lv <= 0;
        const text = none ? fmtLevel(null) : fmtLevel(st.db);
        if (text !== r.lastText) {
            r.lastText = text;
            setText(r.readout, text);
            setAttr(r.track, "aria-valuenow", none ? -60 : Math.round(st.db * 10) / 10);
            setAttr(r.track, "aria-valuetext", `${text} dBFS`);
        }
        setAttr(r.readout, "data-alert", ps === "over" ? "1" : null);
        const muted = r.track.getAttribute("data-muted") === "1";
        setAttr(
            r.track,
            "data-state",
            muted ? "muted" : none ? "none" : ps === "over" ? "over" : ps === "near" ? "near" : "normal",
        );
    }

    function render() {
        if (!ctx.view) return;
        const view = ctx.view;
        const t = view.t;
        const safety = view.state.safety || {};
        const muted = Boolean(safety.cut);
        const fadeIn = !muted && safety.fadeIn === true;
        setText(eyebrow, t("in.meter.eyebrow"));
        setAttr(root, "aria-label", t("in.meter.eyebrow"));
        for (const r of ROWS) {
            const row = rows[r.id];
            setText(row.label, t(r.label));
            setAttr(row.track, "aria-label", t(r.label));
            setAttr(row.name, "title", r.tip ? t(r.tip) : null);
            setAttr(row.readout, "title", t("in.meter.clearTip"));
        }
        const ret = rows.ret;
        setText(ret.fade, fadeIn ? t("in.meter.fadeIn") : "");
        setHidden(ret.fade, !fadeIn);
        setAttr(ret.row, "data-fade", fadeIn ? "1" : null);
        setAttr(ret.track, "data-muted", muted ? "1" : null);
        setHidden(ret.muted, !muted);
        setHidden(ret.liquid, muted);
        setText(ret.muted, muted ? t("in.meter.muted") : "");
        for (const r of ROWS) write(r.id, ctx.meterState(r.id));
        setAttr(
            root,
            "data-state",
            `ret:${muted ? "muted" : fadeIn ? "fadeIn" : ret.lastText === fmtLevel(null) ? "none" : "normal"}`,
        );
    }

    return { el: root, render, write };
}
