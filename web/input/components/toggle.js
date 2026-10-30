// SPDX-License-Identifier: GPL-3.0-or-later
//
// 开关：26×15 的胶囊（样式在 base.css 的 .sc-toggle），原生 button 加 role=switch，空格与回车由浏览器转成点击。
// 命中区在 input.css 里用 ::before 向四周补到 24 px 以上，外观不变。

import { el } from "../../shared/dom.js";
import { setAttr } from "./util.js";

/**
 * @param {object} o
 * @param {string} o.part data-part
 * @param {boolean} [o.green] 开态用语义绿（极性反转）；缺省是薰衣草紫
 * @param {(next:boolean)=>void} o.onToggle 点击：参数是想切到的状态，是否生效由调用方决定（限制器关闭要先确认）
 */
export function createToggle(o) {
    const root = el("button", {
        class: "sc-toggle in-toggle",
        type: "button",
        role: "switch",
        "data-part": o.part,
        "data-toggle": o.green ? "green" : "lavender",
    });
    let on = false;
    root.addEventListener("click", () => {
        if (root.getAttribute("aria-disabled") === "true") return;
        o.onToggle(!on);
    });
    return {
        el: root,
        /** @param {{on:boolean, label:string, title?:string, disabled?:boolean}} p */
        update(p) {
            on = p.on;
            setAttr(root, "data-on", p.on ? "1" : "0");
            setAttr(root, "aria-checked", p.on ? "true" : "false");
            setAttr(root, "aria-label", p.label);
            setAttr(root, "title", p.title || null);
            setAttr(root, "aria-disabled", p.disabled ? "true" : null);
            setAttr(root, "data-disabled", p.disabled ? "1" : null);
        },
    };
}
