// SPDX-License-Identifier: GPL-3.0-or-later
//
// 反馈大表盘：160×160 的 SVG 弧线加 ⌀140 的玻璃圆盘。0% 在 -135 度，每 1% 走 1.8 度，100% 在 +45 度，150% 在 +135 度。
// 值弧最多画到 100%；限制器开时 100% 以上的部分画成红色的「超过」弧；限制器关时 100–150% 画琥珀斜纹，
// 设定值落在那段里时端点是空心圆，读数仍写设定值（有效值 100% 由旁边的芯片说明）。
// 光晕是两层高斯模糊的同色弧（3 与 7），拖动时加粗。
//
// 本件只管显示：事件由 gesture.js 挂在 disc 上，读数的数值输入由 num-edit.js 接管 readout 槽。

import { el, svg } from "../../shared/dom.js";
import { arcPath, polar, setAttr, setText, nextId, setHidden } from "./util.js";
import { feedbackReadout, feedbackValueText, fbAngle, FB_CAP, FB_MAX } from "../model.js";

const CORE = "#9b93c4";
const GLOW = "rgb(196,188,226)";
const CORE_OVER = "rgb(198,110,120)";
const GLOW_OVER = "#d98c8c";
const AMBER = "212,176,118";
const R = 76;
const C = 80;
const arc = (a0, a1) => arcPath(C, C, R, a0, a1);

/** 创建表盘。返回 { el, disc, readout, update }。 */
export function createDial() {
    const id = nextId("rnd");
    const path = (name, attrs) => svg("path", { fill: "none", "data-part": name, ...attrs });
    const amberTitle = svg("title");
    const capTitle = svg("title");
    const amberBand = path("amber-band", { stroke: `rgba(${AMBER},.35)`, "stroke-width": 6 });
    amberBand.append(amberTitle);
    const capBand = path("cap-band", { stroke: `rgba(${AMBER},.22)`, "stroke-width": 6 });
    const capHatch = path("cap-hatch", { stroke: `url(#${id}h)`, "stroke-width": 6, opacity: 0.55 });
    capHatch.append(capTitle);
    const track = path("track", { stroke: "rgba(255,255,255,.18)", "stroke-width": 6, d: arc(-135, 135) });
    const valueGlow2 = path("value-glow", { stroke: GLOW, opacity: 0.5, filter: `url(#${id}g2)` });
    const valueGlow1 = path("value-glow1", { stroke: GLOW, opacity: 0.9, filter: `url(#${id}g)` });
    const overGlow2 = path("over-glow", { stroke: GLOW_OVER, opacity: 0.55, filter: `url(#${id}g2)` });
    const overGlow1 = path("over-glow1", { stroke: GLOW_OVER, opacity: 0.95, filter: `url(#${id}g)` });
    const valueArc = path("value-arc", { stroke: CORE, "stroke-width": 6 });
    const overArc = path("over-arc", { stroke: CORE_OVER, "stroke-width": 6 });
    const [tx0, ty0] = polar(C, C, 72.5, 45);
    const [tx1, ty1] = polar(C, C, 79.5, 45);
    const tick = svg("line", {
        x1: tx0.toFixed(2),
        y1: ty0.toFixed(2),
        x2: tx1.toFixed(2),
        y2: ty1.toFixed(2),
        stroke: "rgba(255,255,255,.6)",
        "stroke-width": 1,
    });
    const end = svg("circle", { r: 3, fill: "var(--txt-1)", "data-part": "end" });

    const defs = svg(
        "defs",
        null,
        svg(
            "pattern",
            { id: `${id}h`, width: 4, height: 4, patternUnits: "userSpaceOnUse", patternTransform: "rotate(45)" },
            svg("rect", { width: 2, height: 4, fill: `rgba(${AMBER},.95)` }),
        ),
        ...[
            ["g", 3],
            ["g2", 7],
        ].map(([n, sd]) =>
            svg(
                "filter",
                { id: id + n, filterUnits: "userSpaceOnUse", x: -30, y: -30, width: 220, height: 220 },
                svg("feGaussianBlur", { stdDeviation: sd }),
            ),
        ),
    );
    const svgEl = svg(
        "svg",
        { width: 160, height: 160, viewBox: "0 0 160 160", "aria-hidden": "true", class: "in-dial__svg" },
        defs,
        track,
        amberBand,
        capBand,
        capHatch,
        valueGlow2,
        valueGlow1,
        overGlow2,
        overGlow1,
        valueArc,
        overArc,
        tick,
        end,
    );

    const main = el("span", { class: "in-dial__main", "data-part": "fb.readout" });
    const sub = el("span", { class: "in-dial__sub", "data-part": "fb.readoutSub" });
    const readout = el("span", { class: "in-dial__readout" }, main, sub);
    const disc = el(
        "div",
        {
            class: "in-dial__disc",
            role: "slider",
            tabindex: "0",
            "data-glow": "1",
            "data-part": "disc",
            aria: { valuemin: 0, valuemax: FB_MAX },
        },
        readout,
    );
    const root = el("div", { class: "in-dial", "data-gb": "input.fb.knob" }, svgEl, disc);

    const lastShape = { v: null, limOff: null, drag: null };

    /**
     * @param {object} p
     * @param {number} p.value 反馈设定值（0..150）
     * @param {"pct"|"db"} p.unit 主读数单位
     * @param {boolean} p.limOff 限制器关
     * @param {boolean} p.drag 拖动中
     * @param {boolean} p.auto 宿主自动化中
     * @param {boolean} p.cut 已切断
     * @param {boolean} p.loopOff 环路关
     * @param {string} p.lang
     * @param {(k:string,v?:object)=>string} p.t
     */
    function update(p) {
        const v = Math.min(FB_MAX, Math.max(0, p.value));
        const shapeChanged = lastShape.v !== v || lastShape.limOff !== p.limOff || lastShape.drag !== p.drag;
        if (shapeChanged) {
            lastShape.v = v;
            lastShape.limOff = p.limOff;
            lastShape.drag = p.drag;
            const a = (x) => fbAngle(x);
            const vd = arc(-135, a(Math.min(v, FB_CAP)));
            const od = v > FB_CAP && !p.limOff ? arc(45, a(v)) : "";
            const k = p.drag ? 1.35 : 1;
            for (const [node, d, w] of [
                [valueGlow2, vd, 16],
                [valueGlow1, vd, 10],
                [overGlow2, od, 16],
                [overGlow1, od, 10],
            ]) {
                setAttr(node, "d", d || null);
                setAttr(node, "stroke-width", w * k);
                setHidden(node, !d);
            }
            setAttr(valueArc, "d", vd || null);
            setHidden(valueArc, !vd);
            setAttr(overArc, "d", od || null);
            setHidden(overArc, !od);
            const [ex, ey] = polar(C, C, R, a(v));
            setAttr(end, "cx", ex.toFixed(2));
            setAttr(end, "cy", ey.toFixed(2));
            setAttr(end, "r", p.drag ? 4 : 3);
            const hollow = p.limOff && v > FB_CAP;
            setAttr(end, "fill", hollow ? "none" : "var(--txt-1)");
            setAttr(end, "stroke", hollow ? "var(--txt-1)" : null);
            setAttr(end, "stroke-width", hollow ? 1.4 : null);
            const band = arc(45, 135);
            setAttr(amberBand, "d", band);
            setHidden(amberBand, Boolean(p.limOff));
            setAttr(capBand, "d", band);
            setAttr(capHatch, "d", band);
            setHidden(capBand, !p.limOff);
            setHidden(capHatch, !p.limOff);
        }
        setText(amberTitle, p.t("in.fb.amberTip"));
        setText(capTitle, p.t("in.fb.capTip"));
        const r = feedbackReadout(v, p.unit);
        setText(main, r.main);
        setText(sub, r.sub);
        setAttr(disc, "aria-valuenow", Math.round(v));
        setAttr(disc, "aria-valuetext", feedbackValueText(v, p.lang));
        setAttr(disc, "aria-label", p.t("in.fb.label"));
        setAttr(disc, "title", p.t(p.loopOff ? "in.fb.offTip" : "in.fb.tip"));
        const state = p.drag ? "drag" : p.auto ? "auto" : "normal";
        setAttr(root, "data-state", state);
        setAttr(root, "data-loop", p.loopOff ? "off" : "on");
        setAttr(root, "data-cut", p.cut ? "1" : null);
    }

    return { el: root, disc, readout, main, update };
}
