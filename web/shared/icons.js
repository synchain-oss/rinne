// SPDX-License-Identifier: GPL-3.0-or-later
//
// 共享图标：16 格线性图标（描边 1.6，圆头圆角）和 Rinne 产品子标识。
// 路径一律写成字符串数组，一个元素一段 path，门禁的 SVG path 白名单按这个形态放行。

import { svg } from "./dom.js";

/** 16×16 线性图标的路径。键名同时是 data-icon 的值。 */
export const ICON = Object.freeze({
    panic: ["M14 8a6 6 0 1 1-12 0a6 6 0 1 1 12 0", "M3.8 3.8l8.4 8.4"],
    red: ["M5.6 2h4.8L14 5.6v4.8L10.4 14H5.6L2 10.4V5.6Z", "M8 5v3.4", "M8 10.9v.1"],
    amber: ["M8 2.2l6.2 10.8H1.8Z", "M8 6.4v2.9", "M8 11.1v.1"],
    gray: ["M14 8a6 6 0 1 1-12 0a6 6 0 1 1 12 0", "M5.6 8h4.8"],
    violet: ["M14 8a6 6 0 1 1-12 0a6 6 0 1 1 12 0", "M5.4 8h5", "M8.6 5.8L10.8 8l-2.2 2.2"],
    neutral: ["M12.6 6.2a5 5 0 1 0 .2 3.2", "M12.7 9.4l-1.9-.6"],
    check: ["M3.5 8.5l3 3 6-7"],
    chev: ["M4.5 6.5L8 10l3.5-3.5"],
    eq: ["M3.5 6.2h9", "M3.5 9.8h9"],
    neq: ["M3.5 6.2h9", "M3.5 9.8h9", "M10.6 3.4L5.4 12.6"],
    q: ["M5.9 6.1a2.2 2.2 0 1 1 3.2 1.9c-.7.4-1.1.9-1.1 1.6v.3", "M8 12.2v.1"],
    arrowR: ["M3 8h10", "M10 5l3 3-3 3"],
    link: ["M6.5 9.5l3-3", "M7.2 4.6l1-1a2.4 2.4 0 0 1 3.4 3.4l-1 1", "M8.8 11.4l-1 1a2.4 2.4 0 0 1-3.4-3.4l1-1"],
    copy: ["M5.5 5.5h6.5v7h-6.5Z", "M4 10.5V3.5h6"],
    close: ["M4.5 4.5l7 7", "M11.5 4.5l-7 7"],
});

/** Rinne 产品子标识 mark-a（18×18 视窗）：两道开口的同心弧，页眉图标块里用。 */
export const SUBMARK_A = Object.freeze(["M15.06 5.91A6.8 6.8 0 1 1 12.09 2.94", "M5.72 9.88A3.4 3.4 0 1 1 8.12 12.28"]);

/**
 * 建一枚 16 格图标，颜色走 currentColor 之外的 color 参数时由调用方写在 stroke 上。
 * @param {string} name ICON 的键
 * @param {number} size 像素边长
 * @param {string} [color] 描边色；缺省 currentColor
 */
export function icon(name, size, color) {
    const paths = ICON[name];
    if (!paths) throw new RangeError(`未知图标 ${name}`);
    return svg(
        "svg",
        {
            width: size,
            height: size,
            viewBox: "0 0 16 16",
            fill: "none",
            stroke: color || "currentColor",
            "stroke-width": 1.6,
            "stroke-linecap": "round",
            "stroke-linejoin": "round",
            "aria-hidden": "true",
            focusable: "false",
            class: "rn-icon",
            data: { icon: name },
        },
        ...paths.map((d) => svg("path", { d })),
    );
}

/** 产品子标识。 */
export function subMark(size, color) {
    return svg(
        "svg",
        {
            width: size,
            height: size,
            viewBox: "0 0 18 18",
            fill: "none",
            stroke: color || "currentColor",
            "stroke-width": 1.6,
            "stroke-linecap": "round",
            "stroke-linejoin": "round",
            "aria-hidden": "true",
            focusable: "false",
            class: "rn-submark",
            data: { submark: "mark-a" },
        },
        ...SUBMARK_A.map((d) => svg("path", { d })),
    );
}
