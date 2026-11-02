// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/web.json
//
// 首启语言选择卡：首启链的最前一环（要不要出、选完去哪由 first-run.js 判，本文件只管卡片）。Input、Output 共用。
//
// 版式（设计稿 v2.5）：宽 300，Input 的中心落在左两栏正中（x 272、顶 164），Output 居中；上方 22 px 的产品子标识，
// 标题三语常显，三个按钮各用各自的语言。
// 点击契约：只有三个语言按钮可点（点了回调 onPick(代码)）；卡片其余区域与遮罩不挂点击处理器，点了一律无动作。
// 遮罩是共用模态（modal.js）：打开时只把内容层设为 inert，不碰 layer 与 top，紧急切断在卡片之上照常可点。

import { el } from "./dom.js";
import { createModal } from "./modal.js";
import { DESIGN } from "./design-box.js";
import { subMark } from "./icons.js";

/** 可选语言（与 i18n.js 的 LANGS 同序；各按钮用各自的语言显示）。 */
export const LANG_PICK_CODES = Object.freeze(["zh", "en", "fr"]);

const WIDTH = 300;
/** Output 卡按设计稿的假定高 210 居中。 */
const OUTPUT_ASSUMED_HEIGHT = 210;
const GEOM = Object.freeze({
    input: { x: 272 - WIDTH / 2, y: 164 },
    output: { x: (DESIGN.output.w - WIDTH) / 2, y: Math.round((DESIGN.output.h - OUTPUT_ASSUMED_HEIGHT) / 2) },
});

/**
 * @param {{role:"input"|"output", host:HTMLElement, inert?:HTMLElement[], onPick:(code:string)=>void}} o
 */
export function createLangStart(o) {
    const title = el("div", { class: "rn-langStart__title", id: `rn-lang-start-title-${o.role}` });
    const buttons = LANG_PICK_CODES.map((code) =>
        // data-lang-pick 而不是 data-lang：避免命中页面里 [data-lang] 的全局切语言绑定
        el("button", {
            type: "button",
            class: "sc-btn sc-btn--dark rn-langStart__btn",
            data: { glow: "1", "lang-pick": code, part: `langStart.${code}` },
            on: { click: () => o.onPick(code) },
        }),
    );
    const modal = createModal({
        host: o.host,
        gb: `${o.role}.langStart`,
        width: WIDTH,
        ...GEOM[o.role],
        inert: o.inert,
        cls: "rn-langStart",
    });
    modal.panel.setAttribute("aria-labelledby", title.id);
    modal.panel.append(
        el("div", { class: "rn-langStart__mark" }, subMark(22)),
        title,
        el("div", { class: "rn-langStart__btns" }, ...buttons),
    );

    return {
        el: modal.panel,
        isShown: modal.isShown,
        /** @param {(k:string)=>string} t */
        show(t) {
            title.textContent = t("common.langStart.title");
            LANG_PICK_CODES.forEach((code, i) => (buttons[i].textContent = t("common.langStart." + code)));
            modal.show();
        },
        hide: () => modal.hide(),
    };
}
