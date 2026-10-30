// SPDX-License-Identifier: GPL-3.0-or-later
//
// 两个只读小件：
//   常驻红字位置提示（O-04）：告诉用户 Output 放在哪。它是安全信息，放在外壳的 layer 层而不是 content 层：
//     宿主旁路时 content 整体降到 .55，红字不随之降级；两行以内、不截断，三语都在 2 行内（冒烟量行数）。
//   发送状态行（O-07）：一行 mono 小字，写本实例此刻在往环路里写什么（发送中 · 声道 · 采样率 / 旁路 / 暂停 / 破折号）。

import { el } from "../shared/dom.js";
import { SEND_KEYS } from "./view-model.js";

/** 常驻红字块。 */
export function createPlaceBlock() {
    const text = el("span", { class: "rn-place__text" });
    const root = el("div", { class: "rn-place", data: { gb: "output.place", state: "normal", tone: "red" } }, text);
    return {
        el: root,
        update({ t }) {
            text.textContent = t("out.place");
        },
    };
}

/** 发送状态行。 */
export function createSendLine() {
    const root = el("div", { class: "rn-send", data: { gb: "output.send", state: "none" } });
    return {
        el: root,
        /** @param {{send:{kind:string,ch:string,sr:string}, t:(k:string,v?:object)=>string}} p */
        update({ send, t }) {
            root.setAttribute("data-state", send.kind);
            if (send.kind === "bypassed" || send.kind === "idle") root.setAttribute("data-tone", "gray");
            else root.removeAttribute("data-tone");
            const chKey = send.ch === "MONO" ? "common.ch.mono" : "common.ch.st";
            root.textContent = t(SEND_KEYS[send.kind], { ch: t(chKey), sr: send.sr });
        },
    };
}
