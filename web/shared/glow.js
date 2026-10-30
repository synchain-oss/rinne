// SPDX-License-Identifier: GPL-3.0-or-later
//
// 光标跟随发光：document 级单一监听，坐标写到被悬停的 [data-glow] 元素自己的 --mx / --my / --glow-o。
//
// 为什么坐标写在元素自己身上，而不是写到根：密排的行里每个元素的圆心要各自相对自己，
// 写到根上所有元素共用一对坐标，圆心就对不上；监听只挂一个，所以密排也不会有成百个监听器。
// 为什么除以缩放倍率：外壳用 CSS zoom 缩放，getBoundingClientRect 与事件坐标都是放大后的视口坐标，
// 而 base.css 里 radial-gradient 的 110px 半径与 --mx/--my 是设计坐标，不除回去圆心会随档位漂移
// （SCVB 的做法没有除，不得照抄）。换算走 hit.js 的 eventToLocal，倍率取 shell-fit.js 的 shellFitFactor()。
//
// 模块顶层没有副作用；installGlow 被调用时才挂监听。glowPoint 是纯函数，Node 里可直接断言。

import { eventToLocal } from "./hit.js";
import { shellFitFactor } from "./shell-fit.js";

/** 发光圆心：事件坐标折回元素内的设计坐标，返回可直接写进 CSS 变量的字符串。 */
export function glowPoint(clientX, clientY, rect, zoom) {
    const p = eventToLocal(clientX, clientY, rect, zoom);
    return { mx: `${p.x.toFixed(1)}px`, my: `${p.y.toFixed(1)}px` };
}

/** 发光元素的选择器。 */
export const GLOW_SELECTOR = "[data-glow]";

/**
 * 安装发光监听，返回卸载函数。同一个文档重复安装是无害的：先卸掉上一次的。
 * @param {Document} [doc] 缺省取 globalThis.document
 * @param {() => number} [zoomOf] 倍率来源，缺省取 shellFitFactor；测试里可以注入
 */
export function installGlow(doc = globalThis.document, zoomOf = shellFitFactor) {
    if (!doc) return () => {};
    if (doc.__rnGlowOff) doc.__rnGlowOff();
    let lit = null;

    function setLit(next) {
        if (lit === next) return;
        if (lit) lit.style.setProperty("--glow-o", "0");
        lit = next;
        if (lit) lit.style.setProperty("--glow-o", "1");
    }

    function onMove(e) {
        // 触屏没有悬停，不画跟随光
        if (e.pointerType === "touch") return;
        const t = e.target;
        const host = t && t.closest ? t.closest(GLOW_SELECTOR) : null;
        setLit(host);
        if (!host) return;
        const { mx, my } = glowPoint(e.clientX, e.clientY, host.getBoundingClientRect(), zoomOf());
        host.style.setProperty("--mx", mx);
        host.style.setProperty("--my", my);
    }

    // 指针离开整个窗口时不会再有 pointermove，补一个 pointerleave 熄灯
    const onLeave = () => setLit(null);

    doc.addEventListener("pointermove", onMove, { passive: true });
    doc.documentElement.addEventListener("pointerleave", onLeave);
    const off = () => {
        doc.removeEventListener("pointermove", onMove);
        doc.documentElement.removeEventListener("pointerleave", onLeave);
        setLit(null);
        doc.__rnGlowOff = null;
    };
    doc.__rnGlowOff = off;
    return off;
}
