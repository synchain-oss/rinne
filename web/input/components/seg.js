// SPDX-License-Identifier: GPL-3.0-or-later
//
// 分段控件（反馈的 % 与 dB、延迟的毫秒 / 采样 / 节拍）：沿用语言胶囊的做法，tablist 加 tab，
// 选中项 tabindex=0（roving），方向键只移焦点、不选中，回车与空格（浏览器转成点击）才选中。

import { el } from "../../shared/dom.js";
import { setAttr, setText } from "./util.js";

/**
 * @param {object} o
 * @param {string} o.part data-part
 * @param {string[]} o.ids 各项 id
 * @param {(id:string)=>void} o.onPick
 */
export function createSeg(o) {
    const tabs = o.ids.map((id) =>
        el("button", {
            type: "button",
            class: "sc-seg__item",
            role: "tab",
            tabindex: "-1",
            "data-glow": "1",
            "data-seg": id,
            aria: { selected: "false" },
            on: {
                click: () => {
                    if (root.getAttribute("aria-disabled") === "true") return;
                    o.onPick(id);
                },
            },
        }),
    );
    const root = el("div", { class: "sc-seg sc-seg--sm in-seg", role: "tablist", "data-part": o.part }, ...tabs);
    root.addEventListener("keydown", (e) => {
        const i = tabs.indexOf(document.activeElement);
        if (i < 0) return;
        const step = e.key === "ArrowRight" ? 1 : e.key === "ArrowLeft" ? -1 : 0;
        if (!step) return;
        e.preventDefault();
        tabs[(i + step + tabs.length) % tabs.length].focus();
    });
    return {
        el: root,
        /** @param {{value:string, labels:string[], label:string, disabled?:boolean}} p disabled：快照没到，整组不响应 */
        update(p) {
            setAttr(root, "aria-label", p.label);
            setAttr(root, "aria-disabled", p.disabled ? "true" : null);
            setAttr(root, "data-state", p.value);
            tabs.forEach((tab, i) => {
                const on = o.ids[i] === p.value;
                setText(tab, p.labels[i]);
                setAttr(tab, "aria-selected", on ? "true" : "false");
                if (tab.tabIndex !== (on ? 0 : -1)) tab.tabIndex = on ? 0 : -1;
            });
        },
    };
}
