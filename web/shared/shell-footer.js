// SPDX-License-Identifier: GPL-3.0-or-later
//
// 页脚：左边一行提示，右边依次是缩放下拉、版本按钮、「?」按钮。Input、Output 共用。
//
// 尺寸：Input 页脚 760×26，提示最宽 470；Output 页脚 424×26，提示最宽 240。提示单行、超出省略号。
// 提示文字归页面（in.hint.*、out.hint.*），本组件只管版式与省略；旧 WebKit 把缩放锁在 100% 时，
// 提示位换成 common.scale.lockedLegacy，下拉只剩 100% 一项，根节点 data-state 为 macLocked。
// 缩放下拉是触发钮加自绘浮层（scale-select.js），不用原生 select：原生列表由系统自绘，样式碰不到。
// 选项与置灰判据来自 scale-ui.js 的状态机（createScaleUi.options），本组件不重算。
// 缩放根节点 data-state：collapsed（收起）、expanded（展开）、macLocked（旧 WebKit 锁在 100%，打不开）。
//
// 版本按钮点开关于面板；「?」按钮重看使用规则与引导。两个按钮的 data-gb 沿用设计稿锚点。
// 首启链要在页面的提示之上再盖一层（恢复的实例显示 hint.firstUseRestored）：setHintOverride(fn) 收页面给的提示与 t、
// 返回要显示的提示，设上或撤掉时按最近一次的入参立即重画提示位；Output 的页面代码不经手这一层。

import { el } from "./dom.js";
import { icon } from "./icons.js";
import { createScaleSelect } from "./scale-select.js";

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
    const scaleLabel = el("span", { class: "rn-footer__scaleLabel" });
    const trigger = el(
        "button",
        { type: "button", class: "rn-footer__scale" },
        scaleLabel,
        el("span", { class: "rn-footer__scaleChev", aria: { hidden: "true" } }, icon("chev", 10)),
    );
    const panel = el("div", { class: "rn-scale__panel" });
    const scaleWrap = el(
        "span",
        { class: "rn-footer__scaleWrap rn-scale", data: { gb: `${role}.footer.scale`, state: "collapsed" } },
        trigger,
        panel,
    );
    let lockedNow = false;
    /** 缩放根节点的 data-state：锁定优先于展开，展开优先于收起。 */
    function syncScaleState() {
        scaleWrap.setAttribute("data-state", lockedNow ? "macLocked" : scaleSel.isOpen() ? "expanded" : "collapsed");
    }
    const scaleSel = createScaleSelect({
        wrap: scaleWrap,
        trigger,
        label: scaleLabel,
        panel,
        onPick: (f) => {
            if (Number.isFinite(f) && f > 0) onScale(f);
        },
        onOpenChange: () => syncScaleState(),
    });
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

    /** 首启链的提示覆盖层与最近一次的入参。 */
    let hintOverride = null;
    let lastP = null;

    /** 只重画提示位（覆盖层变了时用）：不碰下拉，页面在 update 之后对下拉另加的置灰不会被冲掉。 */
    function applyHint(p) {
        const pageHint = hintOverride ? hintOverride(p.hint || null, p.t) : p.hint;
        const h = p.locked === true ? { state: "locked", text: p.t("common.scale.lockedLegacy") } : pageHint;
        hintText.textContent = h ? h.text : "";
        hint.setAttribute("data-state", h ? h.state : "none");
        root.setAttribute("data-state", h ? h.state : "none");
        hintText.setAttribute("title", h ? h.text : "");
    }

    return {
        el: root,
        /**
         * @param {{hint?:{state:string, text:string}|null, scaleOptions:Array<{factor:number,label:string,disabled:boolean,titleKey:string|null}>,
         *          scale:number, locked?:boolean, version:string, t:(k:string,v?:object)=>string}} p
         *   hint：页面给的提示（state 是锚点 data-state，text 是已取好的文字）；locked 时忽略它，显示锁定提示
         */
        update(p) {
            lastP = p;
            const { t } = p;
            const locked = p.locked === true;
            applyHint(p);
            // 下拉：词条、选项表、当前档都交给 scale-select；选项表没变就不重建（页面每秒会重发好几次同样的状态，
            // 用户正开着浮层或用键盘选档时重建会让焦点丢掉）。当前档始终跟页面给的值：用户选了而页面没接受时，触发钮仍显示原档位
            scaleSel.setT(t);
            scaleSel.setOptions(p.scaleOptions);
            scaleSel.setValue(p.scale);
            const name = t("common.scale.label");
            trigger.setAttribute("aria-label", `${name} ${scaleSel.labelText()}`);
            panel.setAttribute("aria-label", name);
            trigger.disabled = locked;
            if (locked) scaleSel.setOpen(false);
            lockedNow = locked;
            syncScaleState();
            version.textContent = t("common.footer.version", { v: p.version });
            version.setAttribute("title", t("common.footer.aboutTip"));
            help.setAttribute("title", t("common.footer.helpTip"));
            help.setAttribute("aria-label", t("common.footer.helpTip"));
        },
        /** @param {((hint:{state:string,text:string}|null, t:(k:string)=>string)=>({state:string,text:string}|null))|null} fn 第二个参数是页面当前语言的 t */
        setHintOverride(fn) {
            hintOverride = typeof fn === "function" ? fn : null;
            if (lastP) applyHint(lastP);
        },
        /** 缩放下拉当前显示的档位（测试与页面回读用）。 */
        scaleValue: () => scaleSel.value(),
        /** 摘掉文档级监听（页面卸载时）。 */
        destroy: () => scaleSel.destroy(),
    };
}
