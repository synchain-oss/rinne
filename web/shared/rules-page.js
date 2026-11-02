// SPDX-License-Identifier: GPL-3.0-or-later
//
// 使用规则页：首启链的第二环，也是「?」与关于面板「查看使用规则」的落点。Input、Output 共用。
//
// 版式（设计稿 v2.5）：Input 放在左两栏（24, 40, 496×504），不压右侧的紧急切断；Output 铺满（14, 12, 432×高−24）。
// 标题、四条的引导词与正文、脚注全部是红字（--sem-red-ink-dark），带 data-tone="red"：安全红字不降级、不截断，
// 只有正文区滚动，按钮栏常驻。第 1 条旁放插入链示意。四条的文字来自 hard-rules.gen.js 的条数与 guide.js 的生成段词条，
// 本文件不抄任何规则原文。
//
// 按钮栏三种变体：
//   first   首启链里还要接导览：[跳过导览] [我已了解，开始导览]
//   ack     要确认、但导览已看过（规则改版、恢复的实例从关于面板进来）：[我已了解]
//   review  重看（已确认当前版本）：[关闭] [继续导览]
// 放行：first、ack 打开时按钮禁用（title 写 rules.readFirst），正文读到底（用户把正文区滚到底）或打开满 8 秒后放行；
// review 不设闸。内容没有溢出时滚不动，只等 8 秒。预览的静态场景不起计时，截图时状态固定。
// Esc 与「这一步之后去哪」都不在这里判：按钮回调 onAction(名字)，键盘由首启控制器统一接。

import { el } from "./dom.js";
import { createModal } from "./modal.js";
import { createChainDiagram } from "./chain-diagram.js";
import { BUTTON_KEYS } from "./common-keys.js";
import { DESIGN } from "./design-box.js";
import { RULES } from "./hard-rules.gen.js";

/** 打开后多久放行按钮（毫秒）。 */
export const RULES_GATE_MS = 8000;

/** 各变体的按钮：[名字, 词条, 是否 CTA]。名字就是 onAction 收到的值。 */
export const RULES_BUTTONS = Object.freeze({
    first: [
        ["skip", "rules.btn.skip", false],
        ["start", "rules.btn.start", true],
    ],
    ack: [["ack", "rules.btn.ack", true]],
    review: [
        ["close", BUTTON_KEYS.close, false],
        ["continue", "rules.btn.continue", true],
    ],
});

/** 规则页按钮是否放行：review 不设闸；其余读到底或打开满 RULES_GATE_MS。 */
export function rulesGateOpen(variant, { elapsedMs = 0, readToEnd = false } = {}) {
    return variant === "review" || readToEnd === true || elapsedMs >= RULES_GATE_MS;
}

/** 第 n 条规则的词条键（guide.js 生成段摊开的 rules.rN.lead / body）。前缀写成字面量：词条门禁按前缀认引用。 */
const ruleKey = (n, part) => "rules." + `r${n}.${part}`;

/** 正文区是否已经滚到底（留 2 px 的取整余量）。 */
export const scrolledToEnd = (node) => node.scrollTop + node.clientHeight >= node.scrollHeight - 2;

const GEOM = Object.freeze({
    input: { x: 24, y: 40, width: 496, height: 504 },
    output: { x: 14, y: 12, width: 432, height: DESIGN.output.h - 24 },
});

/**
 * @param {{role:"input"|"output", host:HTMLElement, inert?:HTMLElement[], onAction:(name:string)=>void,
 *          timers?:{setTimeout:Function, clearTimeout:Function}}} o
 */
export function createRulesPage(o) {
    const timers = o.timers || globalThis;
    const red = (tag, cls, attrs) => el(tag, { class: cls, data: { tone: "red" }, ...attrs });
    const title = red("div", "rn-rules__title", { id: `rn-rules-title-${o.role}` });
    const chain = createChainDiagram({ compact: true, dark: true });
    const items = RULES.map((r, i) => {
        const lead = red("span", "rn-rules__lead");
        const body = red("div", "rn-rules__text");
        const head = el("div", { class: "rn-rules__head" }, lead, i === 0 ? chain.el : null);
        return { id: r.id, lead, body, li: el("li", { class: "rn-rules__item", data: { rule: r.id } }, head, body) };
    });
    const footnote = red("div", "rn-rules__footnote");
    const scroller = el(
        "div",
        { class: "rn-rules__body", data: { part: "rules.body", state: "top" }, tabindex: "0" },
        el("ol", { class: "rn-rules__list" }, ...items.map((x) => x.li)),
        footnote,
    );
    const more = el("div", { class: "rn-rules__more", aria: { hidden: "true" } });
    const actions = el("div", { class: "rn-rules__actions" });
    const modal = createModal({
        host: o.host,
        gb: `${o.role}.rules`,
        ...GEOM[o.role],
        inert: o.inert,
        cls: "rn-rules",
    });
    modal.panel.setAttribute("aria-labelledby", title.id);
    modal.panel.append(title, scroller, more, actions);

    let variant = "review";
    let gated = false;
    let readToEnd = false;
    let timer = 0;
    let t = (k) => k;
    let buttons = [];

    function applyGate() {
        modal.panel.setAttribute("data-state", variant === "review" ? "review" : gated ? "gated" : "ready");
        more.hidden = !gated;
        for (const b of buttons) {
            b.disabled = gated;
            b.toggleAttribute("data-disabled", gated);
            if (gated) b.setAttribute("title", t("rules.readFirst"));
            else b.removeAttribute("title");
        }
    }

    function ungate() {
        if (timer) timers.clearTimeout(timer);
        timer = 0;
        if (!gated) return;
        gated = false;
        applyGate();
    }

    scroller.addEventListener("scroll", () => {
        const end = scrolledToEnd(scroller);
        scroller.setAttribute("data-state", end ? "end" : "top");
        if (end && modal.isShown()) {
            readToEnd = true;
            if (gated && rulesGateOpen(variant, { readToEnd })) ungate();
        }
    });

    function render() {
        title.textContent = t("rules.title");
        items.forEach((x, i) => {
            x.lead.textContent = t(ruleKey(i + 1, "lead"));
            x.body.textContent = t(ruleKey(i + 1, "body"));
        });
        footnote.textContent = t("rules.footnote");
        chain.update({ highlight: "out", still: true, t });
        actions.textContent = "";
        buttons = RULES_BUTTONS[variant].map(([name, key, cta]) =>
            el("button", {
                type: "button",
                class: `sc-btn ${cta ? "sc-btn--cta" : "sc-btn--dark"} rn-btn rn-btn--lg`,
                data: { glow: "1", part: `rules.${name}` },
                text: t(key),
                on: { click: () => !gated && o.onAction(name) },
            }),
        );
        actions.append(...buttons);
        applyGate();
    }

    return {
        el: modal.panel,
        body: scroller,
        isShown: modal.isShown,
        variant: () => variant,
        isGated: () => gated,
        /**
         * @param {{variant:"first"|"ack"|"review", t:(k:string,v?:object)=>string, still?:boolean, gated?:boolean}} p
         *   still：预览的静态场景，不起放行计时；gated：静态场景要摆出的放行状态（缺省按变体）
         */
        open(p) {
            variant = RULES_BUTTONS[p.variant] ? p.variant : "review";
            t = p.t;
            readToEnd = false;
            gated = typeof p.gated === "boolean" ? p.gated : !rulesGateOpen(variant);
            if (timer) timers.clearTimeout(timer);
            timer = gated && !p.still ? timers.setTimeout(ungate, RULES_GATE_MS) : 0;
            render();
            modal.show();
            scroller.scrollTop = 0;
            scroller.setAttribute("data-state", "top");
        },
        /** 换语言：文字重写，放行状态不变。 */
        retext(nextT) {
            if (!modal.isShown()) return;
            t = nextT;
            render();
        },
        close() {
            if (timer) timers.clearTimeout(timer);
            timer = 0;
            modal.hide();
        },
    };
}
