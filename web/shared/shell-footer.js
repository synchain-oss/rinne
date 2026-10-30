// SPDX-License-Identifier: GPL-3.0-or-later
//
// 页脚：左边一行提示，右边依次是缩放下拉、版本按钮、「?」按钮。Input、Output 共用。
//
// 尺寸：Input 页脚 760×26，提示最宽 470；Output 页脚 424×26，提示最宽 240。提示单行、超出省略号。
// 提示文字归页面（in.hint.*、out.hint.*），本组件只管版式与省略；旧 WebKit 把缩放锁在 100% 时，
// 提示位换成 common.scale.lockedLegacy，下拉只剩 100% 一项，根节点 data-state 为 macLocked。
// 缩放下拉的选项与置灰判据来自 scale-ui.js 的状态机（createScaleUi.options），本组件不重算。
//
// 版本按钮点开关于面板；「?」按钮重看使用规则与引导。两个按钮的 data-gb 沿用设计稿锚点。

import { el } from "./dom.js";
import { icon } from "./icons.js";

const HINT_MAX = Object.freeze({ input: 470, output: 240 });

/**
 * @param {{role:"input"|"output", onScale?:(factor:number)=>void, onAbout?:()=>void, onHelp?:()=>void}} o
 */
export function createFooter(o) {
    const { role } = o;
    if (!HINT_MAX[role]) throw new RangeError(`createFooter：未知角色 ${role}`);
    const noop = () => {};
    const onScale = typeof o.onScale === "function" ? o.onScale : noop;
    const onAbout = typeof o.onAbout === "function" ? o.onAbout : noop;
    const onHelp = typeof o.onHelp === "function" ? o.onHelp : noop;

    const hintText = el("span", { class: "rn-footer__hintText" });
    const hint = el(
        "div",
        {
            class: "rn-footer__hint",
            data: { gb: `${role}.footer.hint`, state: "none" },
            style: { maxWidth: `${HINT_MAX[role]}px` },
        },
        hintText,
    );
    const select = el("select", {
        class: "sc-select rn-footer__scale",
        on: {
            change: () => {
                const f = Number(select.value);
                if (Number.isFinite(f) && f > 0) onScale(f);
            },
        },
    });
    const scaleWrap = el(
        "span",
        { class: "rn-footer__scaleWrap", data: { gb: `${role}.footer.scale`, state: "collapsed" } },
        select,
    );
    const version = el("button", {
        type: "button",
        class: "rn-footer__version",
        data: { gb: `${role}.footer.version`, glow: "1" },
        on: { click: () => onAbout() },
    });
    const help = el(
        "button",
        {
            type: "button",
            class: "rn-footer__help",
            data: { gb: `${role}.footer.help`, tour: "help", glow: "1" },
            on: { click: () => onHelp() },
        },
        icon("q", 13),
    );
    const root = el(
        "div",
        { class: "rn-footer", data: { gb: `${role}.footer`, state: "none" } },
        hint,
        el("div", { class: "rn-footer__right" }, scaleWrap, version, help),
    );

    /** 下拉选项的签名：页面每秒会重发好几次同样的状态，选项没变就不重建（用户正打开下拉或用键盘选档时重建会让弹出层关掉、高亮跳回）。 */
    let optionsSig = "";

    return {
        el: root,
        /**
         * @param {{hint?:{state:string, text:string}|null, scaleOptions:Array<{factor:number,label:string,disabled:boolean,titleKey:string|null}>,
         *          scale:number, locked?:boolean, version:string, t:(k:string,v?:object)=>string}} p
         *   hint：页面给的提示（state 是锚点 data-state，text 是已取好的文字）；locked 时忽略它，显示锁定提示
         */
        update(p) {
            const { t } = p;
            const locked = p.locked === true;
            const h = locked ? { state: "locked", text: t("common.scale.lockedLegacy") } : p.hint;
            hintText.textContent = h ? h.text : "";
            hint.setAttribute("data-state", h ? h.state : "none");
            root.setAttribute("data-state", h ? h.state : "none");
            hintText.setAttribute("title", h ? h.text : "");
            // 下拉：档位、标签、置灰与置灰说明（已按当前语言取好）、锁定都没变就不重建，只在签名变了时整表重写
            const titleOf = (opt) => (opt.disabled && opt.titleKey ? t(opt.titleKey) : "");
            const sig = JSON.stringify([
                p.scaleOptions.map((opt) => [opt.factor, opt.label, opt.disabled, titleOf(opt)]),
                locked,
            ]);
            if (sig !== optionsSig) {
                optionsSig = sig;
                select.textContent = "";
                for (const opt of p.scaleOptions) {
                    const node = el("option", { value: String(opt.factor), text: opt.label });
                    if (opt.disabled) {
                        node.disabled = true;
                        if (opt.titleKey) node.setAttribute("title", titleOf(opt));
                    }
                    select.append(node);
                }
            }
            // 选中项始终跟页面给的档位（用户选了但页面没接受时要弹回去）；值相同就不写，免得打扰打开着的下拉
            if (select.value !== String(p.scale)) select.value = String(p.scale);
            select.setAttribute("aria-label", t("common.scale.label"));
            select.disabled = locked;
            scaleWrap.setAttribute("data-state", locked ? "macLocked" : "collapsed");
            version.textContent = t("common.footer.version", { v: p.version });
            version.setAttribute("title", t("common.footer.aboutTip"));
            help.setAttribute("title", t("common.footer.helpTip"));
            help.setAttribute("aria-label", t("common.footer.helpTip"));
        },
        /** 缩放下拉当前显示的档位（测试与页面回读用）。 */
        scaleValue: () => Number(select.value),
    };
}
