// SPDX-License-Identifier: GPL-3.0-or-later
//
// 插入链示意：一排 6 个槽，第 1 槽写 IN，推子竖线前最后一槽（虚线）写 OUT，竖线写 FADER。
// 告诉用户 Rinne Input 放链首、Rinne Output 放推子前的最后一格。
//
// 状态：
//   highlight="out"  Input 视角，OUT 槽呼吸描边（提示「把 Output 放这儿」）；
//   highlight="in"   Output 视角，IN 槽外加一圈呼吸环；
//   highlight=null   静态说明（规则页旁用）。
//   still            关掉呼吸；系统「减少动效」时 CSS 也会关，不依赖这个参数。
// 尺寸 132×31，紧凑版 132×25（Input 的一行消息位里放不下 31 高）。

import { el, svg } from "./dom.js";

/** 6 个槽的左边缘 x。第 0 槽是 IN，第 4 槽是 OUT，第 5 槽在推子之后。 */
export const CHAIN_SLOTS = Object.freeze([0, 21, 42, 63, 84, 114]);
/** 推子竖线的 x。 */
export const CHAIN_FADER_X = 108;

const IN_SLOT = 0;
const OUT_SLOT = 4;

/**
 * @param {{compact?:boolean, dark?:boolean}} [o] dark：画在深色面上（消息位的深底危险条里）
 */
export function createChainDiagram(o = {}) {
    const height = o.compact ? 25 : 31;
    const root = el("div", {
        class: "rn-chain",
        data: { dark: o.dark ? "1" : null },
        style: { height: `${height}px` },
    });
    return {
        el: root,
        /** @param {{highlight?:"in"|"out"|null, still?:boolean, t:(k:string)=>string}} p */
        update({ highlight = null, still = false, t }) {
            root.setAttribute("data-chain", highlight || "static");
            root.setAttribute("data-state", still ? "static" : "breathing");
            const kids = [];
            CHAIN_SLOTS.forEach((x, i) => {
                const isIn = i === IN_SLOT;
                const isOut = i === OUT_SLOT;
                const lit = (highlight === "out" && isOut) || (highlight === "in" && isIn);
                kids.push(
                    svg("rect", {
                        class: "rn-chain__slot",
                        x: x + 0.5,
                        y: 4.5,
                        width: 17,
                        height: 11,
                        rx: 5,
                        data: { slot: isIn ? "in" : isOut ? "out" : "free", lit: lit ? "1" : null },
                    }),
                );
                if (lit && isIn) {
                    // IN 槽自己是实心填充，没有描边可以呼吸，所以在外面加一圈环
                    kids.push(
                        svg("rect", { class: "rn-chain__ring", x: x - 1.5, y: 2.5, width: 21, height: 15, rx: 7 }),
                    );
                }
                if (isIn || isOut) {
                    kids.push(
                        svg(
                            "text",
                            {
                                class: "rn-chain__label",
                                x: x + 9,
                                y: 12.4,
                                "text-anchor": "middle",
                                data: { slot: isIn ? "in" : "out" },
                            },
                            t(isIn ? "common.chain.in" : "common.chain.out"),
                        ),
                    );
                }
            });
            kids.push(svg("line", { class: "rn-chain__fader", x1: CHAIN_FADER_X, y1: 0, x2: CHAIN_FADER_X, y2: 21 }));
            kids.push(
                svg(
                    "text",
                    {
                        class: "rn-chain__faderLabel",
                        x: CHAIN_FADER_X,
                        y: height === 25 ? 25 : 30,
                        "text-anchor": "middle",
                    },
                    t("common.chain.fader"),
                ),
            );
            root.textContent = "";
            root.append(
                svg(
                    "svg",
                    {
                        width: 132,
                        height,
                        viewBox: `0 0 132 ${height}`,
                        "aria-hidden": "true",
                        focusable: "false",
                        class: "rn-chain__svg",
                    },
                    ...kids,
                ),
            );
        },
    };
}
