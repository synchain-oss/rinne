// SPDX-License-Identifier: GPL-3.0-or-later
//
// 两个只读小件（位置提示带一颗「知道了」按钮）：
//   常驻红字位置提示（O-04，Rinne Sans 600）：告诉用户 Output 放在哪。它是安全信息，放在外壳的 layer 层而不是 content 层：
//     宿主旁路时 content 整体降到 .55，红字不随之降级；两行以内、不截断，三语都在 2 行内（冒烟量行数）。
//     右侧一颗「知道了」按钮（真 button，可聚焦、回车与空格可用）：用户确认读过之后关掉它，由页面经桥记成本机全局设置，
//     以后新开的 Output 都不再显示；关掉后版式上移（output.css 里按外壳的 data-place 切换），
//     「关于 → 高级设置」里的开关可以让它回来。真的放错了（错序、跨轨）的消息位提示与这个开关无关，照常显示。
//   发送状态行（O-07）：一行 mono 小字，居中压在配对卡底部，写本实例此刻在往环路里写什么（发送中 · 声道 · 采样率 / 旁路 / 暂停 / 未接入环路）。

import { el } from "../shared/dom.js";
import { SEND_KEYS } from "./view-model.js";

/**
 * 常驻红字块。
 * @param {{onDismiss?:()=>void}} [o] onDismiss：点「知道了」
 */
export function createPlaceBlock(o = {}) {
    const text = el("span", { class: "rn-place__text" });
    // 按钮没有新的 data-gb 锚点（设计稿的锚点表里没有它），用 data-part 供冒烟定位
    const ok = el("button", {
        type: "button",
        class: "sc-btn rn-place__ok",
        data: { glow: "1", part: "place.dismiss" },
        on: { click: () => (typeof o.onDismiss === "function" ? o.onDismiss() : undefined) },
    });
    const root = el("div", { class: "rn-place", data: { gb: "output.place", state: "normal", tone: "red" } }, text, ok);
    return {
        el: root,
        button: ok,
        /** @param {{t:(k:string)=>string, hidden?:boolean}} p hidden：用户已关掉提示 */
        update({ t, hidden }) {
            text.textContent = t("out.place");
            ok.textContent = t("out.place.ok");
            ok.setAttribute("aria-label", t("out.place.dismissLabel"));
            ok.setAttribute("title", t("out.place.dismissTip"));
            root.hidden = hidden === true;
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
