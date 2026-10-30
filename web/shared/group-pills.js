// SPDX-License-Identifier: GPL-3.0-or-later
//
// 组选择：小标「组」，A 到 H 八枚 mono 胶囊，可选的「自动」徽标。Input、Output 共用。
//
// 状态：
//   选中    白底（--seg-on-bg）深字；
//   对端在线 胶囊里带一个 4 px 绿点（该组有一个未配对的对端在等），title 说明；点不脉冲、不加光晕；
//   自动    有「自动」徽标，title 说明是自动选的，点任意组改为手动；
//   未分配  group 为 null（8 个组都被占）：八枚都不选，根节点 data-state="unassigned"；
//   禁用    确认条展开时其余胶囊 .35 禁用，由页面传 disabled。
//
// 键盘：role=radiogroup，用 roving tabindex（选中项或第一项可 Tab 进入），方向键只移动焦点，
// 空格或回车才选中。不让方向键直接选中，是因为已连接时选组会弹确认条、甚至断开环路，
// 焦点扫过的每一格都触发它是不对的。
// 命中区：胶囊高 20，上下各补 2 px 透明扩展区（::before），命中高度 24。

import { el } from "./dom.js";

export const GROUP_LETTERS = Object.freeze(["A", "B", "C", "D", "E", "F", "G", "H"]);

/** 各页自己的词条键：对端在线点和自动徽标的 title。 */
const KEYS = Object.freeze({
    input: { peerDot: "in.group.peerDotTip", auto: "in.group.autoTip", autoFallback: "in.group.autoTip" },
    output: { peerDot: "out.group.peerDotTip", auto: "out.group.autoTip", autoFallback: "out.group.autoFallback" },
});

/** groupMask（八位位图，第 n 位为 1 表示第 n 个组里有未配对的对端）展开成组字母数组。 */
export function groupsFromMask(mask) {
    const m = Number.isInteger(mask) ? mask : 0;
    return GROUP_LETTERS.filter((_, n) => (m >> n) & 1);
}

/** 根节点的 data-state：none（未选）、unassigned（8 组已满）、manual、auto、autoFallback。 */
export function groupState({ group, auto, unassigned }) {
    if (unassigned) return "unassigned";
    if (!group) return "none";
    if (!auto) return "manual";
    return auto === "fallback" ? "autoFallback" : "auto";
}

/**
 * @param {{role:"input"|"output", onSelect?:(letter:string)=>void, gb?:string}} o
 */
export function createGroupPills(o) {
    const keys = KEYS[o.role];
    if (!keys) throw new RangeError(`createGroupPills：未知角色 ${o.role}`);
    const onSelect = typeof o.onSelect === "function" ? o.onSelect : () => {};
    const label = el("span", { class: "rn-group__label" });
    const autoBadge = el("span", { class: "sc-badge rn-group__auto", hidden: true });
    const pills = GROUP_LETTERS.map((letter) =>
        el(
            "span",
            {
                class: "rn-group__pill",
                role: "radio",
                tabindex: "-1",
                data: { glow: "1", group: letter },
                aria: { checked: "false" },
                on: {
                    click: () => {
                        if (!pills[GROUP_LETTERS.indexOf(letter)].hasAttribute("aria-disabled")) onSelect(letter);
                    },
                },
            },
            el("span", { class: "rn-group__letter", text: letter }),
        ),
    );
    const bar = el("div", { class: "rn-group__bar", role: "radiogroup" }, ...pills);
    const root = el(
        "div",
        { class: "rn-group", data: { gb: o.gb || `${o.role}.group`, state: "none" } },
        label,
        bar,
        autoBadge,
    );

    bar.addEventListener("keydown", (e) => {
        const i = pills.indexOf(document.activeElement);
        if (i < 0) return;
        if (e.key === "ArrowRight" || e.key === "ArrowDown") {
            e.preventDefault();
            pills[(i + 1) % pills.length].focus();
        } else if (e.key === "ArrowLeft" || e.key === "ArrowUp") {
            e.preventDefault();
            pills[(i + pills.length - 1) % pills.length].focus();
        } else if (e.key === " " || e.key === "Enter") {
            e.preventDefault();
            pills[i].click();
        }
    });

    return {
        el: root,
        /**
         * @param {{group:string|null, auto?:boolean|"fallback", peers?:string[], disabled?:boolean, unassigned?:boolean,
         *          autoGroup?:string, t:(k:string,v?:object)=>string}} p
         *   auto：true 为自动选的组，"fallback" 为 Output 没找到未配对 Input、暂用某组；autoGroup 是 title 里的 {g}
         */
        update(p) {
            const peers = new Set(p.peers || []);
            label.textContent = p.t("common.group.label");
            const sel = p.unassigned ? null : p.group;
            const firstTab = sel && GROUP_LETTERS.includes(sel) ? sel : GROUP_LETTERS[0];
            pills.forEach((pill, n) => {
                const letter = GROUP_LETTERS[n];
                const on = letter === sel;
                const dot = pill.querySelector(".rn-group__dot");
                pill.setAttribute("aria-checked", on ? "true" : "false");
                pill.setAttribute("data-state", on ? "selected" : peers.has(letter) ? "peerOnline" : "normal");
                pill.tabIndex = letter === firstTab && !p.disabled ? 0 : -1;
                if (p.disabled && !on) pill.setAttribute("aria-disabled", "true");
                else pill.removeAttribute("aria-disabled");
                if (peers.has(letter)) {
                    const tip = p.t(keys.peerDot, { g: letter });
                    // 4 px 的点太小悬停不到，title 同时挂在整枚胶囊上
                    pill.setAttribute("title", tip);
                    if (dot) dot.setAttribute("title", tip);
                    else {
                        pill.append(
                            el("span", { class: "rn-group__dot", data: { peerDot: "1", tone: "green" }, title: tip }),
                        );
                    }
                } else {
                    pill.removeAttribute("title");
                    if (dot) dot.remove();
                }
            });
            const state = groupState({ group: p.group, auto: p.auto, unassigned: p.unassigned });
            root.setAttribute("data-state", state);
            autoBadge.hidden = !p.auto;
            if (p.auto) {
                autoBadge.textContent = p.t("common.group.auto");
                const g = p.autoGroup || p.group || "";
                autoBadge.setAttribute("title", p.t(p.auto === "fallback" ? keys.autoFallback : keys.auto, { g }));
            } else autoBadge.removeAttribute("title");
        },
    };
}
