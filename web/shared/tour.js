// SPDX-License-Identifier: GPL-3.0-or-later
//
// 首启导览（mini tour）的运行器：遮罩挖亮区、说明框、步号与按钮。Input、Output 共用，步骤表在各页（tour-in.js、tour-out.js）。
//
// 版式（设计稿 v2.5）：遮罩是暗幕加背景模糊，亮区是 [data-tour="<锚点>"] 外扩 SPOT_PAD 的羽化矩形洞；说明框宽 360，
// 按步骤表的首选位置摆，压到亮区、紧急切断或越出设计盒时换方位（tour-paint.js 的 pickCalloutPos）。
// 层级：整层在外壳 top 层里，z-index 低于紧急切断那一层，所以紧急切断在导览之上照常可点。
// 交互：左键点任意处（含说明框本身）进下一步，末步即完成；三个按钮各管各的；右键与滚轮不触发也不穿透；
// Esc 与 ←、→ 由首启控制器转进 key()。打开时内容层 inert（与模态共用计数），焦点交给说明框。
// 结束只回调 onEnd("done" | "skip" | "esc")；写不写「引导已看」由控制器判（重看不写）。

import { el } from "./dom.js";
import { lockInert, unlockInert } from "./modal.js";
import { DESIGN } from "./design-box.js";
import { SPOT_PAD, holeMask, pickCalloutPos } from "./tour-paint.js";

/**
 * 亮区的设计盒坐标：锚点的包围盒按外壳的有效缩放换回设计 px，再外扩 pad。坐标以外壳的内边框为原点（版面各件的
 * left / top 也是从那里量的），所以减掉外壳自己的描边。有效缩放用包围盒宽比 offsetWidth（两者都含描边）；
 * 比 clientWidth 会把描边算进倍率，右下角的件偏出几 px。锚点不存在返回 null。
 */
export function anchorRect(shellEl, sel, pad) {
    const target = sel ? shellEl.querySelector(sel) : null;
    if (!target) return null;
    const sr = shellEl.getBoundingClientRect();
    const tr = target.getBoundingClientRect();
    const k = shellEl.offsetWidth > 0 ? sr.width / shellEl.offsetWidth : 1;
    const s = Number.isFinite(k) && k > 0 ? k : 1;
    return {
        x: Math.round((tr.left - sr.left) / s - shellEl.clientLeft - pad),
        y: Math.round((tr.top - sr.top) / s - shellEl.clientTop - pad),
        w: Math.round(tr.width / s + pad * 2),
        h: Math.round(tr.height / s + pad * 2),
    };
}

/**
 * @param {{role:"input"|"output", host:HTMLElement, shell:HTMLElement, inert?:HTMLElement[],
 *          steps:Array<{anchor:string|null, keys:string, card:object}>, onEnd:(reason:string)=>void}} o
 *   steps[i].keys 是这一步词条的前缀（如 "tour.in.step1."），后接 title / body；card 是 pickCalloutPos 的首选位置；
 *   pad 缺省为 SPOT_PAD，锚点自带内边距时可以改小
 */
export function createTour(o) {
    const n = o.steps.length;
    const inertNodes = o.inert || [];
    const btn = (part, cta, fn) =>
        el("button", {
            type: "button",
            class: `sc-btn ${cta ? "sc-btn--cta" : "sc-btn--dark"} rn-btn rn-tour__btn`,
            data: { glow: "1", part },
            on: {
                click: (e) => {
                    e.stopPropagation();
                    fn();
                },
            },
        });
    const title = el("span", { class: "rn-tour__title", id: `rn-tour-title-${o.role}` });
    const stepNo = el("span", { class: "rn-tour__step" });
    const dots = o.steps.map(() => el("span", { class: "rn-tour__dot" }));
    const body = el("div", { class: "rn-tour__body" });
    const hint = el("span", { class: "rn-tour__hint" });
    const skipBtn = btn("tour.skip", false, () => o.onEnd("skip"));
    const prevBtn = btn("tour.prev", false, () => go(cur - 1));
    const nextBtn = btn("tour.next", true, () => next());
    const callout = el(
        "div",
        { class: "rn-tour__callout", data: { part: "tour.callout" }, tabindex: "-1" },
        el(
            "div",
            { class: "rn-tour__head" },
            title,
            el("span", { class: "rn-tour__ind" }, stepNo, el("span", { class: "rn-tour__dots" }, ...dots)),
        ),
        body,
        el("div", { class: "rn-tour__foot" }, hint, el("span", { class: "rn-tour__btns" }, skipBtn, prevBtn, nextBtn)),
    );
    const mask = el("div", { class: "rn-tour__mask", data: { part: "tour.mask" } });
    const root = el(
        "div",
        {
            class: "rn-tour",
            role: "dialog",
            // 对话框的名称是当前步的标题（与语言卡、规则页一致：名称由 aria-labelledby 指到标题节点）
            aria: { modal: "true", labelledby: title.id },
            data: { gb: `${o.role}.tour`, state: "closed" },
            hidden: true,
        },
        mask,
        callout,
    );
    o.host.append(root);
    root.addEventListener("click", (e) => {
        if (e.button === 0) next();
    });
    root.addEventListener("contextmenu", (e) => e.preventDefault());
    root.addEventListener("wheel", (e) => e.preventDefault(), { passive: false });

    let cur = 1;
    let t = (k) => k;
    let place = null;

    function render() {
        const step = o.steps[cur - 1];
        root.setAttribute("data-state", `step-${cur}`);
        callout.setAttribute("data-state", `step-${cur}`);
        title.textContent = t(step.keys + "title");
        body.textContent = t(step.keys + "body");
        stepNo.textContent = t("tour.common.step", { i: cur, n });
        dots.forEach((d, i) => d.toggleAttribute("data-current", i === cur - 1));
        hint.textContent = t("tour.common.clickAnywhere");
        skipBtn.textContent = t("tour.common.skip");
        prevBtn.textContent = t("tour.common.prev");
        prevBtn.disabled = cur === 1;
        nextBtn.textContent = t(cur === n ? "tour.common.done" : "tour.common.next");
        nextBtn.setAttribute("data-part", cur === n ? "tour.done" : "tour.next");
        layout();
    }

    /** 量亮区、挖洞、摆说明框。字体与语言都会改说明框的高，所以每次换步、换语言都重摆。 */
    function layout() {
        const step = o.steps[cur - 1];
        const pad = Number.isFinite(step.pad) ? step.pad : SPOT_PAD;
        const spot = step.anchor ? anchorRect(o.shell, `[data-tour="${step.anchor}"]`, pad) : null;
        const panic = anchorRect(o.shell, '[data-tour="panic"]', 0);
        // 能摆的范围是外壳的内边框以内（描边里面），不是含描边的设计盒
        const box = { w: o.shell.clientWidth || DESIGN[o.role].w, h: o.shell.clientHeight || DESIGN[o.role].h };
        const m = spot ? holeMask(spot) : "none";
        mask.style.webkitMaskImage = m;
        mask.style.maskImage = m;
        mask.setAttribute("data-spot", spot ? [spot.x, spot.y, spot.w, spot.h].join(",") : "none");
        place = pickCalloutPos(
            box,
            { w: callout.offsetWidth, h: callout.offsetHeight },
            spot,
            step.card,
            panic ? [panic] : [],
        );
        callout.style.left = `${place.x}px`;
        callout.style.top = `${place.y}px`;
        callout.setAttribute("data-place", place.ok ? place.how : "unfit");
    }

    function go(i) {
        if (i < 1 || i > n) return;
        cur = i;
        render();
    }

    function next() {
        if (cur >= n) o.onEnd("done");
        else go(cur + 1);
    }

    return {
        el: root,
        isShown: () => !root.hidden,
        step: () => cur,
        /** 最近一次摆放的结果（冒烟用）。 */
        placement: () => place,
        /** @param {{step?:number, t:(k:string,v?:object)=>string}} p */
        start(p) {
            t = p.t;
            cur = Math.min(Math.max(1, p.step || 1), n);
            if (root.hidden) {
                root.hidden = false;
                for (const node of inertNodes) lockInert(node);
            }
            render();
            callout.focus({ preventScroll: true });
        },
        retext(nextT) {
            if (root.hidden) return;
            t = nextT;
            render();
        },
        /** 版面变了（快照改了配对卡的高之类）：重量亮区、重摆说明框。 */
        relayout() {
            if (!root.hidden) layout();
        },
        /** 键盘：Esc 结束（算跳过），← / → 翻页。返回是否处理了。 */
        key(e) {
            if (root.hidden) return false;
            if (e.key === "Escape") o.onEnd("esc");
            else if (e.key === "ArrowRight") next();
            else if (e.key === "ArrowLeft") go(cur - 1);
            else return false;
            return true;
        },
        stop() {
            if (root.hidden) return;
            root.hidden = true;
            root.setAttribute("data-state", "closed");
            for (const node of inertNodes) unlockInert(node);
        },
    };
}
