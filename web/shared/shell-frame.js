// SPDX-License-Identifier: GPL-3.0-or-later
//
// 外壳、页眉与语言胶囊。Input、Output 两页共用。
//
// 外壳：设计盒大小的卡片（.sc-shell），背后三个柔光球，顶缘一条 angel-eye 高光（.sc-shell::before）。
// 里面分三层，页面把自己的内容挂进去：
//   content  常规内容；宿主旁路时整体降到 .55 不透明度；
//   layer    常驻红字、toast 一类盖在内容上、模态之下的东西；
//   top      遮罩与模态。
// 危险描边（L2：切断、冲突、版本不匹配）画在 .sc-shell::after（见 rinne.css），
// 外壳自己的 box-shadow 不动，描边只对 opacity 做过渡。
//
// 页眉：38×38 图标块放 Rinne 产品子标识（Synchain 主标只出现在关于面板），右边是品牌字与 eyebrow。
// 品牌字、eyebrow 的词条在各页词典里（in.brand.*、out.brand.*），这里按角色取键。

import { DESIGN } from "./design-box.js";
import { el } from "./dom.js";
import { subMark } from "./icons.js";
import { LANG_ITEMS } from "./common-keys.js";

/** 三个柔光球的位置 [top, left]，按角色。 */
const HALOS = Object.freeze({
    input: [
        [-120, -90],
        [380, 560],
        [150, 300],
    ],
    output: [
        [-150, -130],
        [190, 250],
        [80, 120],
    ],
});

/** 外壳 data-state 的取值，优先级由高到低：danger、bypassed、modal、normal。 */
export function shellState({ danger, hostBypass, overlay }) {
    return danger ? "danger" : hostBypass ? "bypassed" : overlay ? "modal" : "normal";
}

/**
 * @param {{role:"input"|"output"}} o
 * @returns {{el:HTMLElement, content:HTMLElement, layer:HTMLElement, top:HTMLElement,
 *            setState:(s:{danger?:boolean, hostBypass?:boolean, overlay?:boolean})=>string}}
 */
export function createShell({ role }) {
    const box = DESIGN[role];
    if (!box) throw new RangeError(`createShell：未知角色 ${role}`);
    const content = el("div", { class: "rn-shell__content", data: { part: "content" } });
    const layer = el("div", { class: "rn-shell__layer", data: { part: "layer" } });
    const top = el("div", { class: "rn-shell__top", data: { part: "top" } });
    const root = el(
        "div",
        {
            class: "sc-shell rn-shell",
            data: { gb: `${role}.shell`, state: "normal", role },
            style: { width: `${box.w}px`, height: `${box.h}px` },
        },
        ...HALOS[role].map(([t, l], i) =>
            el("div", {
                class: `sc-halo sc-halo--${i + 1}`,
                style: { top: `${t}px`, left: `${l}px` },
                aria: { hidden: "true" },
            }),
        ),
        content,
        layer,
        top,
    );
    return {
        el: root,
        content,
        layer,
        top,
        setState(s) {
            const state = shellState(s || {});
            root.setAttribute("data-state", state);
            // 危险描边靠 data-danger 驱动 ::after 的不透明度；它与 data-state 分开写，
            // 是因为宿主旁路与危险可以同时成立，而 data-state 只能取一个值
            root.setAttribute("data-danger", s && s.danger ? "1" : "0");
            root.toggleAttribute("data-bypass", Boolean(s && s.hostBypass));
            return state;
        },
    };
}

// ---------------------------------------------------------------- 页眉

const BRAND_KEYS = Object.freeze({
    input: { title: "in.brand.title", eyebrow: "in.brand.eyebrow" },
    output: { title: "out.brand.title", eyebrow: "out.brand.eyebrow" },
});

/** 页眉：图标块 + 品牌字 + eyebrow；中文界面在品牌字后跟淡色「回环」。 */
export function createHeader({ role }) {
    const keys = BRAND_KEYS[role];
    if (!keys) throw new RangeError(`createHeader：未知角色 ${role}`);
    const title = el("span", { class: "rn-brand__title" });
    const suffix = el("span", { class: "rn-brand__suffix", hidden: true });
    const eyebrow = el("span", { class: "rn-brand__eyebrow" });
    const root = el(
        "div",
        { class: "rn-brand", data: { gb: `${role}.header`, state: "normal" } },
        el("div", { class: "rn-brand__mark" }, subMark(19)),
        el("div", { class: "rn-brand__text" }, el("div", { class: "rn-brand__row" }, title, suffix), eyebrow),
    );
    return {
        el: root,
        /** @param {{lang:string, t:(k:string)=>string}} p */
        update({ lang, t }) {
            title.textContent = t(keys.title);
            eyebrow.textContent = t(keys.eyebrow);
            const showSuffix = lang === "zh";
            suffix.hidden = !showSuffix;
            suffix.textContent = showSuffix ? t("common.brand.cnSuffix") : "";
        },
    };
}

// ---------------------------------------------------------------- 语言胶囊

/**
 * 语言胶囊：中 / EN / FR 三项，选中项白底。点击立即回调 onPick(code)，页面负责 setLang 与刷新。
 * 三项各用自己的语言显示，不随界面语言变。
 */
export function createLangCapsule({ role, onPick }) {
    const pick = typeof onPick === "function" ? onPick : () => {};
    const tabs = LANG_ITEMS.map((it) =>
        el("button", {
            type: "button",
            class: "sc-seg__item rn-lang__item",
            role: "tab",
            tabindex: "-1",
            data: { glow: "1", lang: it.code },
            aria: { selected: "false" },
            on: { click: () => pick(it.code) },
        }),
    );
    const root = el(
        "div",
        { class: "sc-seg sc-seg--sm rn-lang", role: "tablist", data: { gb: `${role}.lang`, state: "zh" } },
        ...tabs,
    );
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
        /** @param {{lang:string, t:(k:string)=>string}} p */
        update({ lang, t }) {
            root.setAttribute("data-state", lang);
            // 语言胶囊的可访问名用三语并列的选择卡标题，它本来就是「选择语言」的三语写法
            root.setAttribute("aria-label", t("common.langStart.title"));
            tabs.forEach((tab, i) => {
                const it = LANG_ITEMS[i];
                const on = it.code === lang;
                tab.textContent = t(it.key);
                tab.setAttribute("aria-selected", on ? "true" : "false");
                tab.tabIndex = on ? 0 : -1;
            });
        },
    };
}
