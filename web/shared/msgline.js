// SPDX-License-Identifier: GPL-3.0-or-later
//
// 消息位：全窗口唯一的一条消息槽。Input 是一行（452×30），Output 最多三行（424×58）。
// 本文件只实现机制与两种尺寸；具体有哪些消息、各自的颜色与动作，由页面的消息表给。
//
// 页面每次 update 交来「当前成立的全部消息」，已按同色内的优先顺序排好；本组件做三件事：
//   1. 按颜色优先级排序（红 > 琥珀 > 灰 > 紫 > 中性，稳定排序，同色保持页面给的顺序），显示第一条；
//   2. 有多条时右侧出现「+n」chip，点击轮换到下一条，8 秒没人再点就回到第一条；
//   3. 确认条（取消 + 接管一类）临时占用同一个槽，关闭后恢复，槽的高度不变。
//
// 外观（颜色、边、字色都是已有 token 的组合，见 rinne.css）：
//   切断类五条         深底危险条，「恢复回送」按钮在最右；冷却期内按钮带细进度弧；
//   其余红             --light-solid 叠红染 .12，字 --sem-red-ink；
//   琥珀 / 灰 / 紫      各自的浅色条；
//   中性               玻璃数据卡里的 mono 小字，Output 空闲时放 out.hint.idle；
//   可选 dismiss ✕     只给「建议类」消息（可以关掉的），红色且条件仍在的消息不带。
//
// 无障碍：槽是 role=status，当前显示红色时改为 role=alert。只有显示的条目或其文字变了才改写 DOM
// （签名不变就什么都不做）：页面每秒会重发好几次同样的状态，重建节点会让读屏重复播报，
// 也会让正在被按下的按钮被替换掉，点击落空。

import { el, svg } from "./dom.js";
import { icon } from "./icons.js";
import { BUTTON_KEYS } from "./common-keys.js";

/** 颜色优先级，数字越小越靠前。 */
export const TONE_RANK = Object.freeze({ red: 0, amber: 1, gray: 2, violet: 3, neutral: 4 });

/** 切断类五条：画成深底危险条。键在 Input 的词典里。 */
export const CUT_KEYS = Object.freeze([
    "band.in.cutPanic",
    "band.in.cutPanicHot",
    "band.in.cutNan",
    "band.in.cutHuge",
    "band.in.cutRunaway",
]);

/** 动作按钮：词条键与外观。主行动（浅紫实心）与次行动（深色实心）。名字是消息表里 action.name 的取值。 */
export const ACTIONS = Object.freeze({
    restore: { key: "band.action.restore", kind: "cta" },
    moveFree: { key: "band.action.moveFree", kind: "cta" },
    moveTo: { key: "band.action.moveTo", kind: "cta" },
    limOn: { key: "band.action.limOn", kind: "cta" },
    repair: { key: "band.action.repair", kind: "cta" },
    takeover: { key: "band.action.takeover", kind: "cta" },
    guide: { key: "band.action.guide", kind: "darkSolid" },
    ack: { key: "band.action.ack", kind: "darkSolid" },
});

/** 轮换后多久回到第一条（毫秒）。 */
export const ROTATE_RESET_MS = 8000;
/** 恢复回送的冷却时长，进度弧按它走一圈（毫秒）。 */
export const COOLDOWN_MS = 1500;

/**
 * 排序并给出当前应显示的那一条的下标（相对排序后数组）。
 * 纯函数：messages 的元素至少有 id、tone。返回 { list, shown }，shown 是轮换偏移后的下标。
 * @param {Array<{id:string,tone:string}>} messages
 * @param {number} offset 轮换偏移，会对条数取模
 */
export function orderMessages(messages, offset = 0) {
    const list = messages
        .map((m, i) => ({ m, i }))
        .sort((a, b) => (TONE_RANK[a.m.tone] ?? 9) - (TONE_RANK[b.m.tone] ?? 9) || a.i - b.i)
        .map((x) => x.m);
    const n = list.length;
    return { list, shown: n ? ((offset % n) + n) % n : -1 };
}

/** 一条消息是否画成深底危险条。 */
export function isDark(msg) {
    return msg.dark === true || (msg.dark !== false && CUT_KEYS.includes(msg.key));
}

function coolArc() {
    return svg(
        "svg",
        {
            class: "rn-coolArc",
            width: 12,
            height: 12,
            viewBox: "0 0 12 12",
            "aria-hidden": "true",
            focusable: "false",
            data: { part: "cooldown-arc" },
        },
        svg("circle", { class: "rn-coolArc__track", cx: 6, cy: 6, r: 4.6 }),
        svg("circle", { class: "rn-coolArc__arc", cx: 6, cy: 6, r: 4.6 }),
    );
}

/**
 * @param {object} o
 * @param {"input"|"output"} o.role
 * @param {(name:string, msg:object)=>void} [o.onAction] 点动作按钮
 * @param {(id:string)=>void} [o.onDismiss] 点 ✕
 * @param {{setTimeout:Function, clearTimeout:Function}} [o.timers]
 */
export function createMsgline(o) {
    const { role } = o;
    if (role !== "input" && role !== "output") throw new RangeError(`createMsgline：未知角色 ${role}`);
    const timers = o.timers || globalThis;
    const onAction = typeof o.onAction === "function" ? o.onAction : () => {};
    const onDismiss = typeof o.onDismiss === "function" ? o.onDismiss : () => {};
    const size = role === "input" ? "line" : "block";
    const root = el("div", {
        class: `rn-msgline rn-msgline--${size}`,
        role: "status",
        // tabindex=-1：重建内容时焦点原先在槽里却找不到对应的件，要有个落脚处（只能程序化聚焦，不进 Tab 序）
        tabindex: "-1",
        data: { gb: `${role}.band`, state: "idle", size },
    });

    let offset = 0;
    let resetTimer = 0;
    let lastSig = "";
    let lastIds = "";
    let current = null;
    let latest = null; // 最近一次 update 的参数，轮换点击与到点回位时重画用

    /**
     * 重建槽里的内容。重建会把正在持有键盘焦点的按钮一起换掉，焦点掉到 body 上，键盘用户的位置就丢了
     * （「+n」连按轮换、8 秒到点自动回位都会重建）。所以重建前看焦点是否在槽里：在，就还给新节点里同一个 data-part 的件；
     * 没有同名件就退到新的「+n」，再退到槽本身。焦点原本不在槽里不去抢。
     */
    function rebuild(...children) {
        const doc = root.ownerDocument;
        const active = doc.activeElement;
        const had = Boolean(active) && root.contains(active);
        const part = had ? active.getAttribute("data-part") : null;
        root.replaceChildren(...children);
        if (!had) return;
        const next =
            (part && root.querySelector(`[data-part="${part}"]`)) ||
            root.querySelector('[data-part="band.more"]') ||
            root;
        next.focus({ preventScroll: true });
    }

    function armReset() {
        if (resetTimer) timers.clearTimeout(resetTimer);
        resetTimer = timers.setTimeout(() => {
            resetTimer = 0;
            offset = 0;
            if (latest) render(latest);
        }, ROTATE_RESET_MS);
    }

    function actionButton(name, msg, p, cooling) {
        const def = ACTIONS[name];
        if (!def) throw new RangeError(`未知的消息动作 ${name}`);
        const kind = def.kind;
        const btn = el(
            "button",
            {
                type: "button",
                class: `sc-btn rn-btn rn-btn--${kind}${kind === "cta" ? " sc-btn--cta" : ""}`,
                data: { glow: "1", part: "band.action", action: name, state: cooling ? "cooldown" : "normal" },
                on: {
                    click: () => {
                        if (cooling) return;
                        // 签名不变时节点不重建，按钮挂的是第一次渲染时的消息；点击时取页面最近一次给的同 id 消息，免得载荷是旧的
                        const now = latest && (latest.messages || []).find((m) => m.id === msg.id);
                        onAction(name, now || msg);
                    },
                },
            },
            cooling ? coolArc() : null,
            el("span", { text: p.t(def.key, msg.action && msg.action.vals) }),
        );
        if (cooling) {
            btn.setAttribute("aria-disabled", "true");
            btn.setAttribute("title", p.t("band.action.cooldownTip"));
        }
        if (name === "takeover") btn.setAttribute("title", p.t("band.action.takeoverTip"));
        return btn;
    }

    function renderConfirm(p) {
        const t = p.t;
        const c = p.confirm;
        const sig = `confirm|${p.lang}|${c.key}|${JSON.stringify(c.vals || null)}|${c.confirmKey || ""}`;
        if (sig === lastSig) return;
        lastSig = sig;
        current = { id: "confirm", tone: "amber", key: c.key };
        const cancel = el("button", {
            type: "button",
            class: "sc-btn rn-btn",
            data: { glow: "1", part: "confirm.cancel" },
            // 签名不变时节点不重建：点击时从最近一次 update 取回调，不用第一次渲染时捕获的那一份
            on: { click: () => latest && latest.confirm && latest.confirm.onCancel && latest.confirm.onCancel() },
            text: t(BUTTON_KEYS.cancel),
        });
        const go = el("button", {
            type: "button",
            class: "sc-btn sc-btn--cta rn-btn rn-btn--cta",
            data: { glow: "1", part: "band.action", action: "takeover" },
            on: { click: () => latest && latest.confirm && latest.confirm.onConfirm && latest.confirm.onConfirm() },
            text: t(c.confirmKey || "band.action.takeover", c.vals),
        });
        if (!c.confirmKey) go.setAttribute("title", t("band.action.takeoverTip"));
        root.setAttribute("role", "status");
        root.setAttribute("data-state", "confirm");
        root.setAttribute("data-tone", "amber");
        root.setAttribute("data-msg", c.key);
        root.setAttribute("data-variant", "confirm");
        rebuild(
            el(
                "div",
                { class: "rn-banner rn-banner--confirm" },
                icon("amber", 14),
                el("span", { class: "rn-banner__text", text: t(c.key, c.vals) }),
                el("div", { class: "rn-banner__right" }, size === "line" ? [cancel, go] : [go, cancel]),
            ),
        );
    }

    function renderIdle(p) {
        const idle = p.idleKey || null;
        const sig = `idle|${p.lang}|${idle}`;
        if (sig === lastSig) return;
        lastSig = sig;
        current = null;
        root.setAttribute("role", "status");
        root.setAttribute("data-state", "idle");
        root.setAttribute("data-tone", "neutral");
        root.removeAttribute("data-msg");
        root.removeAttribute("data-variant");
        // replaceChildren(null) 会插入文字「null」，没有空闲提示时必须传空参数表
        rebuild(
            ...(idle
                ? [
                      el(
                          "div",
                          { class: "rn-banner rn-banner--idle" },
                          el("div", {
                              class: "rn-banner__idle",
                              data: { gb: "output.hint", state: "idle" },
                              text: p.t(idle),
                          }),
                      ),
                  ]
                : []),
        );
    }

    function renderMessage(p, list, shown) {
        const t = p.t;
        const n = list.length;
        const msg = list[shown];
        const dark = isDark(msg);
        const cooling = Boolean(msg.action && msg.action.cooldown);
        const sig = [
            "msg",
            p.lang,
            msg.id,
            msg.key,
            JSON.stringify(msg.vals || null),
            msg.tone,
            dark ? 1 : 0,
            n,
            shown,
            msg.action ? `${msg.action.name}:${JSON.stringify(msg.action.vals || null)}:${cooling ? 1 : 0}` : "",
            msg.dismiss ? 1 : 0,
        ].join("|");
        if (sig === lastSig) return;
        lastSig = sig;
        current = { id: msg.id, tone: msg.tone, key: msg.key };
        const tone = msg.tone;
        root.setAttribute("role", tone === "red" ? "alert" : "status");
        root.setAttribute("data-state", `${msg.id}${cooling ? "+cooldown" : ""}`);
        root.setAttribute("data-tone", tone);
        root.setAttribute("data-msg", msg.key);
        root.setAttribute("data-variant", dark ? "dark" : "light");

        const chip =
            n > 1
                ? el("button", {
                      type: "button",
                      class: "rn-more",
                      data: { glow: "1", part: "band.more" },
                      title: t("band.moreTip", { n: n - 1 }),
                      aria: { label: t("band.moreTip", { n: n - 1 }) },
                      on: {
                          click: () => {
                              offset += 1;
                              armReset();
                              render(latest);
                          },
                      },
                      text: t("band.more", { n: n - 1 }),
                  })
                : null;
        const act = msg.action ? actionButton(msg.action.name, msg, p, cooling) : null;
        const dismiss = msg.dismiss
            ? el(
                  "button",
                  {
                      type: "button",
                      class: "sc-banner__dismiss rn-dismiss",
                      aria: { label: t(BUTTON_KEYS.close) },
                      data: { part: "band.dismiss" },
                      on: { click: () => onDismiss(msg.id) },
                  },
                  icon("close", 10),
              )
            : null;
        const cls = ["sc-banner", "rn-banner", `rn-banner--${dark ? "danger" : tone}`];
        if (tone === "red" && !dark) cls.push("sc-banner--red");
        if (tone === "violet") cls.push("sc-banner--violet");
        const iconEl = icon(tone, 14);
        iconEl.classList.add("sc-banner__icon");
        rebuild(
            el(
                "div",
                { class: cls.join(" ") },
                iconEl,
                el("span", { class: "rn-banner__text", text: t(msg.key, msg.vals) }),
                el("div", { class: "rn-banner__right" }, chip, act),
                dismiss,
            ),
        );
    }

    function render(p) {
        latest = p;
        if (p.confirm) return renderConfirm(p);
        const messages = p.messages || [];
        const { list } = orderMessages(messages, 0);
        const ids = list.map((m) => m.id).join("\n");
        if (ids !== lastIds) {
            // 成立的消息集合变了：轮换回到第一条
            lastIds = ids;
            offset = 0;
        }
        if (list.length === 0) return renderIdle(p);
        const { shown } = orderMessages(messages, offset);
        return renderMessage(p, list, shown);
    }

    return {
        el: root,
        /**
         * @param {{messages:Array<{id:string,key:string,vals?:object,tone:"red"|"amber"|"gray"|"violet"|"neutral",
         *            action?:{name:string,vals?:object,cooldown?:boolean}, dismiss?:boolean, dark?:boolean}>,
         *          confirm?:{key:string,vals?:object,confirmKey?:string,onConfirm?:()=>void,onCancel?:()=>void}|null,
         *          idleKey?:string, lang:string, t:(k:string,v?:object)=>string}} p
         */
        update(p) {
            render(p);
        },
        /** 当前槽里显示的是哪一条（确认条的 id 是 "confirm"）；空闲时 null。 */
        current: () => current,
        destroy() {
            if (resetTimer) timers.clearTimeout(resetTimer);
            resetTimer = 0;
        },
    };
}
