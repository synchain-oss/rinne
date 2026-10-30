// SPDX-License-Identifier: GPL-3.0-or-later
//
// 深色 toast：右下角，4 秒后消失。Input、Output 共用。
//
// 外观：深色玻璃底（--dark-toast），圆角 14，最宽 306，前导 6 px 圆点按语气着色，正文 11.5/1.5。
// 位置：Input 距右 20、距底 44，Output 距右 18、距底 44（不压页脚，也不盖 Input 右栏的紧急切断，
// 后者在更高的安全层里）。role=status，读屏会播报，不抢焦点。
// 共享的三条：toast.group（绿）、toast.diagCopied（绿）、toast.copyFail（红）；
// 各页自己的 toast 键（预设载入、限制器等）调用时自带语气。

import { el } from "./dom.js";

/** toast 停留时间（毫秒）。 */
export const TOAST_MS = 4000;

/** 共享三条的语气；页面自己的键在 show() 里显式给语气。 */
export const SHARED_TOAST_TONES = Object.freeze({
    "toast.group": "green",
    "toast.diagCopied": "green",
    "toast.copyFail": "red",
});

/**
 * @param {{host:HTMLElement, role:"input"|"output", timers?:{setTimeout:Function, clearTimeout:Function}}} o
 */
export function createToast(o) {
    const timers = o.timers || globalThis;
    const dot = el("span", { class: "sc-toast__dot", aria: { hidden: "true" } });
    const text = el("span", { class: "rn-toast__text" });
    const root = el(
        "div",
        { class: "sc-toast rn-toast", role: "status", hidden: true, data: { gb: `${o.role}.toast`, role: o.role } },
        dot,
        text,
    );
    o.host.append(root);
    let timer = 0;

    function hide() {
        if (timer) timers.clearTimeout(timer);
        timer = 0;
        root.hidden = true;
        text.textContent = "";
    }

    return {
        el: root,
        /**
         * @param {{key:string, vals?:object, tone?:"green"|"red"|"amber", t:(k:string,v?:object)=>string}} p
         *   tone 缺省取共享三条的语气表，表里没有就按绿
         */
        show({ key, vals, tone, t }) {
            const tn = tone || SHARED_TOAST_TONES[key] || "green";
            root.setAttribute("data-state", key.replace(/^toast\./, ""));
            root.setAttribute("data-tone", tn);
            dot.setAttribute("data-tone", tn);
            text.textContent = t(key, vals);
            root.hidden = false;
            if (timer) timers.clearTimeout(timer);
            timer = timers.setTimeout(hide, TOAST_MS);
        },
        hide,
        isShown: () => !root.hidden,
    };
}
