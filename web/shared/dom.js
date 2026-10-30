// SPDX-License-Identifier: GPL-3.0-or-later
//
// 共享件的 DOM 小工具：建元素、建 SVG、取词条。
//
// 为什么单独一份：门禁禁止一切字符串转 HTML 的写法（innerHTML、insertAdjacentHTML 等），共享件只能靠
// DOM API 搭结构。这里把「建元素 + 设属性 + 挂子节点」收成两个函数，文字一律走 textContent。
//
// 模块顶层没有副作用，也不触碰 window 或 document；el() 与 svg() 被调用时才读 document，
// 所以纯函数部分（makeT、图标路径）在 Node 里可以直接 import。

import { dict, format } from "./i18n.js";

const SVG_NS = "http://www.w3.org/2000/svg";

/**
 * 词条取值函数：t(key, vals) 按当前语言取词条并填占位符。
 * 取不到的键返回键名本身：缺词条在界面上是一眼可见的键名，冒烟用这个特征抓它，而不是静默留空。
 * @param {string} lang zh、en 或 fr
 */
export function makeT(lang) {
    return (key, vals) => {
        const table = dict(lang);
        const raw = Object.prototype.hasOwnProperty.call(table, key) ? table[key] : key;
        return format(raw, vals);
    };
}

/** 把一组属性写到节点上：class、style（对象）、data（对象）、aria（对象）、on（事件表）、text 和普通属性。 */
function applyAttrs(node, attrs) {
    if (!attrs) return;
    for (const [k, v] of Object.entries(attrs)) {
        if (v === undefined || v === null || v === false) continue;
        if (k === "class") node.setAttribute("class", v);
        else if (k === "text") node.textContent = v;
        else if (k === "style" && typeof v === "object") {
            for (const [p, val] of Object.entries(v)) {
                if (p.startsWith("--")) node.style.setProperty(p, String(val));
                else node.style[p] = val;
            }
        } else if (k === "data") {
            for (const [dk, dv] of Object.entries(v)) {
                if (dv !== undefined && dv !== null && dv !== false)
                    node.setAttribute("data-" + dk, dv === true ? "1" : String(dv));
            }
        } else if (k === "aria") {
            for (const [ak, av] of Object.entries(v)) {
                if (av !== undefined && av !== null) node.setAttribute("aria-" + ak, String(av));
            }
        } else if (k === "on") {
            for (const [ev, fn] of Object.entries(v)) node.addEventListener(ev, fn);
        } else node.setAttribute(k, v === true ? "" : String(v));
    }
}

function append(node, kids) {
    for (const kid of kids.flat(Infinity)) {
        if (kid === undefined || kid === null || kid === false) continue;
        node.append(kid instanceof Node ? kid : document.createTextNode(String(kid)));
    }
}

/** 建 HTML 元素。kids 可以是节点、字符串或它们的数组，假值被忽略。 */
export function el(tag, attrs, ...kids) {
    const node = document.createElement(tag);
    applyAttrs(node, attrs);
    append(node, kids);
    return node;
}

/** 建 SVG 元素。 */
export function svg(tag, attrs, ...kids) {
    const node = document.createElementNS(SVG_NS, tag);
    applyAttrs(node, attrs);
    append(node, kids);
    return node;
}

/** 清空节点的子节点。 */
export function clear(node) {
    while (node.firstChild) node.removeChild(node.firstChild);
    return node;
}
