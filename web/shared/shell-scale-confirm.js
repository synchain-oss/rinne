// SPDX-License-Identifier: GPL-3.0-or-later
//
// 缩放确认框：选档后窗口已经按新档位改过，10 秒内不点保存就回退。Input、Output 共用。
// 状态机在 scale-ui.js（createScaleUi），本组件只画框、转发按钮，并显示剩余秒数：
// 页面把状态机的 onChange 回调接到 update()，把 onKeep、onCancel 接到 scaleUi.keep()、scaleUi.revert()。
//
// 位置：Input 放在左两栏（中心 x=272、顶边 210，不压右侧安全栏）；Output 水平居中，顶边按设计稿的 (窗高 - 130) / 2。宽 300。
// Esc 等于取消（回退），不等于保存：不确认就当没改。

import { el } from "./dom.js";
import { DESIGN } from "./design-box.js";
import { createModal } from "./modal.js";
import { BUTTON_KEYS } from "./common-keys.js";
import { SCALE_KEYS } from "./scale-ui.js";

const WIDTH = 300;
/** Input 模态的中心 x 与顶边：落在左两栏正中。 */
const INPUT_CENTER_X = 272;
const INPUT_TOP = 210;
/** Output 模态的顶边按设计稿的假定框高 130 居中，不随内容实高变（实高约 102，所以视觉上略偏上）。 */
const OUTPUT_ASSUMED_HEIGHT = 130;

/**
 * @param {{role:"input"|"output", host:HTMLElement, inert?:HTMLElement[], onKeep:()=>void, onCancel:()=>void}} o
 */
export function createScaleConfirm(o) {
    const body = el("div", { class: "sc-modal__body rn-scaleConfirm__body", id: `rn-scale-body-${o.role}` });
    const cancel = el("button", {
        type: "button",
        class: "sc-btn sc-btn--dark rn-btn rn-btn--lg",
        data: { glow: "1", part: "confirm.cancel" },
        on: { click: () => o.onCancel() },
    });
    const keep = el("button", {
        type: "button",
        class: "sc-btn sc-btn--cta rn-btn rn-btn--lg",
        data: { glow: "1", part: "confirm.keep" },
        on: { click: () => o.onKeep() },
    });
    const modal = createModal({
        host: o.host,
        gb: `${o.role}.scale.confirm`,
        width: WIDTH,
        ...(o.role === "input"
            ? { x: INPUT_CENTER_X - WIDTH / 2, y: INPUT_TOP }
            : { x: (DESIGN.output.w - WIDTH) / 2, y: (DESIGN.output.h - OUTPUT_ASSUMED_HEIGHT) / 2 }),
        inert: o.inert,
        onEsc: () => o.onCancel(),
        cls: "rn-scaleConfirm",
    });
    modal.panel.setAttribute("aria-labelledby", body.id);
    modal.panel.append(body, el("div", { class: "rn-scaleConfirm__actions" }, cancel, keep));
    return {
        el: modal.panel,
        /** @param {{confirming:boolean, left:number, t:(k:string,v?:object)=>string}} p 状态机 onChange 的载荷加 t */
        update({ confirming, left, t }) {
            if (!confirming) {
                modal.hide();
                return;
            }
            modal.panel.setAttribute("data-state", `countdown-${left}`);
            body.textContent = t(SCALE_KEYS.confirmBody, { s: left });
            cancel.textContent = t(BUTTON_KEYS.cancel);
            keep.textContent = t(SCALE_KEYS.keep);
            modal.show();
        },
        isShown: modal.isShown,
    };
}
