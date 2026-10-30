// SPDX-License-Identifier: GPL-3.0-or-later
//
// 小表盘：音色卡里的低切与高切（⌀36，带光晕）、安全卡里的限制器上限（⌀26）。
// 玻璃圆盘外面一圈 3 度到 4 度宽的值弧，-135 到 +135 度；fromEnd 为真时弧从末端往回画（高切：越往左切得越多）。
// 位置 pos 是 0..1，换算与读数归调用方；本件只管显示，事件由 gesture.js 挂在根节点上。

import { el, svg } from "../../shared/dom.js";
import { arcPath, polar, setAttr, setHidden, setVar, nextId } from "./util.js";

const CORE = "#9b93c4";
const GLOW = "rgb(196,188,226)";

/**
 * @param {object} o
 * @param {number} o.size 圆盘直径
 * @param {string} [o.halo] 光晕颜色（CSS 值），缺省不画
 * @param {boolean} [o.fromEnd] 值弧从末端往回画
 * @param {boolean} [o.glow] 值弧带光晕
 * @param {string} o.part data-part，区分低切、高切、上限
 */
export function createMiniDial(o) {
    const D = o.size;
    const g = D >= 48 ? 3 : 2;
    const w = D >= 48 ? 4 : 3;
    const S = D + 2 * (g + w);
    const c = S / 2;
    const R = D / 2 + g + w / 2;
    const arc = (a0, a1) => arcPath(c, c, R, a0, a1);
    const id = nextId("rnm");

    const track = svg("path", {
        d: arc(-135, 135),
        stroke: "rgba(255,255,255,.18)",
        "stroke-width": w,
        fill: "none",
    });
    const glow2 = svg("path", { fill: "none", stroke: GLOW, "data-part": "value-glow", filter: `url(#${id}b)` });
    const glow1 = svg("path", { fill: "none", stroke: GLOW, filter: `url(#${id}a)` });
    const value = svg("path", {
        fill: "none",
        stroke: o.glow ? CORE : "var(--acc-slider)",
        "stroke-width": w,
        "data-part": "value-arc",
    });
    const end = svg("circle", { r: w / 2 + 0.7, fill: "var(--txt-1)" });
    const blur = (n, sd) =>
        svg(
            "filter",
            { id: id + n, filterUnits: "userSpaceOnUse", x: -20, y: -20, width: S + 40, height: S + 40 },
            svg("feGaussianBlur", { stdDeviation: sd }),
        );
    const svgEl = svg(
        "svg",
        { width: S, height: S, viewBox: `0 0 ${S} ${S}`, "aria-hidden": "true", class: "in-mini__svg" },
        svg("defs", null, blur("a", 2.2), blur("b", 4.5)),
        track,
        o.glow ? glow2 : null,
        o.glow ? glow1 : null,
        value,
        end,
    );

    const needleLen = Math.round(D * 0.2);
    const needleOff = Math.round(D * 0.4);
    const needle = el("span", {
        class: "in-mini__needle",
        style: { height: `${needleLen}px`, marginTop: `${-needleOff}px`, transformOrigin: `50% ${needleOff}px` },
    });
    const disc = el(
        "div",
        {
            class: "in-mini__disc",
            "data-glow": "1",
            style: { left: `${g + w}px`, top: `${g + w}px`, width: `${D}px`, height: `${D}px` },
        },
        needle,
    );
    const halo = o.halo
        ? el("div", {
              class: "in-mini__halo",
              "aria-hidden": "true",
              style: {
                  left: `${-S * 0.12}px`,
                  top: `${-S * 0.04}px`,
                  width: `${S * 1.24}px`,
                  height: `${S * 1.24}px`,
                  background: o.halo,
                  filter: `blur(${Math.max(9, Math.round(S * 0.2))}px)`,
              },
          })
        : null;
    const root = el(
        "div",
        {
            class: "in-mini",
            role: "slider",
            tabindex: "0",
            "data-part": o.part,
            aria: { valuemin: 0, valuemax: 1 },
            style: { width: `${S}px`, height: `${S}px` },
        },
        halo,
        disc,
        svgEl,
    );

    let last = "";

    /**
     * @param {object} p
     * @param {number} p.pos 0..1
     * @param {string} p.valueText aria-valuetext
     * @param {string} p.label aria-label
     * @param {string} [p.title]
     * @param {boolean} p.drag
     * @param {boolean} p.auto
     * @param {boolean} p.disabled
     */
    function update(p) {
        const pos = Math.min(1, Math.max(0, p.pos));
        const key = `${pos}|${p.drag}|${p.auto}`;
        if (key !== last) {
            last = key;
            const ang = -135 + 270 * pos;
            const va = o.fromEnd ? arc(ang, 135) : arc(-135, ang);
            const k = p.drag ? 1.3 : 1;
            const op = p.auto ? 0.5 : 1;
            for (const [node, width, alpha] of [
                [glow2, (w + 9) * k, 0.5 * op],
                [glow1, (w + 4) * k, 0.9 * op],
            ]) {
                setAttr(node, "d", va || null);
                setAttr(node, "stroke-width", width);
                setAttr(node, "opacity", alpha);
                setHidden(node, !va);
            }
            setAttr(value, "d", va || null);
            setAttr(value, "opacity", p.auto ? 0.6 : 1);
            setHidden(value, !va);
            const [ex, ey] = polar(c, c, R, ang);
            setAttr(end, "cx", ex.toFixed(2));
            setAttr(end, "cy", ey.toFixed(2));
            setAttr(end, "r", w / 2 + (p.drag ? 1.6 : 0.7));
            setAttr(end, "fill", p.auto ? "rgba(33,33,42,.45)" : "var(--txt-1)");
            setVar(needle, "--ang", `${ang}deg`);
        }
        setAttr(root, "aria-valuenow", Math.round(pos * 100) / 100);
        setAttr(root, "aria-valuetext", p.valueText);
        setAttr(root, "aria-label", p.label);
        setAttr(root, "title", p.title || null);
        setAttr(root, "aria-disabled", p.disabled ? "true" : null);
        setAttr(root, "data-state", p.disabled ? "disabled" : p.drag ? "drag" : p.auto ? "auto" : "normal");
    }

    return { el: root, disc, size: S, update };
}
