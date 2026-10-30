// SPDX-License-Identifier: GPL-3.0-or-later
//
// 模态的公共骨架：遮罩 + 深色玻璃面板，关于面板与缩放确认共用。
//
// 约定：
//   - 遮罩铺满外壳，面板可以居中，也可以按设计坐标钉位置（Input 的模态放在左两栏，不压右侧安全栏）；
//   - 打开时把 inert 列表里的节点（通常是外壳的 content 层）设为 inert，Tab 出不去，
//     但不碰 layer 与 top：紧急切断那一层要在任何遮罩下都点得到；
//     inert 按节点计数：关于面板与缩放确认共用同一个 content 节点，两个同时开着时先关的那个不能把 inert 清掉，
//     数到 0 才移除；
//   - 打开时记住焦点并把焦点交给面板，关闭时还回去；Esc 关闭（调 onEsc，由调用方决定是取消还是关闭）；
//   - 面板 role=dialog、aria-modal=true。

import { el } from "./dom.js";

/** 每个节点被几个打开着的模态要求 inert。 */
const inertCount = new WeakMap();

function lockInert(node) {
    inertCount.set(node, (inertCount.get(node) || 0) + 1);
    node.setAttribute("inert", "");
}

function unlockInert(node) {
    const left = (inertCount.get(node) || 0) - 1;
    if (left > 0) {
        inertCount.set(node, left);
        return;
    }
    inertCount.delete(node);
    node.removeAttribute("inert");
}

/**
 * @param {object} o
 * @param {HTMLElement} o.host 挂载点（外壳的 top 层）
 * @param {string} o.gb 面板的 data-gb
 * @param {number} o.width 面板宽（设计 px）
 * @param {number} [o.x] 面板左边缘；与 y 一起给才按坐标定位，否则居中
 * @param {number} [o.y]
 * @param {number} [o.height] 面板高，缺省随内容
 * @param {HTMLElement[]} [o.inert] 打开时设为 inert 的节点
 * @param {()=>void} [o.onEsc] 按 Esc 时调用
 * @param {string} [o.cls] 面板追加的类名
 */
export function createModal(o) {
    const placed = Number.isFinite(o.x) && Number.isFinite(o.y);
    const style = { width: `${o.width}px` };
    if (placed) Object.assign(style, { left: `${o.x}px`, top: `${o.y}px` });
    if (Number.isFinite(o.height)) style.height = `${o.height}px`;
    const panel = el("div", {
        class: `sc-modal rn-modal${placed ? " rn-modal--placed" : ""}${o.cls ? " " + o.cls : ""}`,
        role: "dialog",
        tabindex: "-1",
        aria: { modal: "true" },
        data: { gb: o.gb, state: "open" },
        style,
    });
    const scrim = el("div", { class: "sc-scrim rn-scrim", data: { part: "scrim" }, hidden: true }, panel);
    o.host.append(scrim);
    const inertNodes = o.inert || [];
    let returnFocus = null;

    scrim.addEventListener("keydown", (e) => {
        if (e.key === "Escape" && typeof o.onEsc === "function") {
            e.preventDefault();
            e.stopPropagation();
            o.onEsc();
        }
    });

    return {
        scrim,
        panel,
        isShown: () => !scrim.hidden,
        show() {
            if (!scrim.hidden) return;
            returnFocus = document.activeElement;
            scrim.hidden = false;
            for (const n of inertNodes) lockInert(n);
            panel.focus({ preventScroll: true });
        },
        hide() {
            if (scrim.hidden) return;
            scrim.hidden = true;
            for (const n of inertNodes) unlockInert(n);
            if (returnFocus && returnFocus.isConnected && typeof returnFocus.focus === "function") {
                returnFocus.focus({ preventScroll: true });
            }
            returnFocus = null;
        },
    };
}
