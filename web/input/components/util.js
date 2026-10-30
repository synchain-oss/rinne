// SPDX-License-Identifier: GPL-3.0-or-later
//
// Input 页面件的小工具：只写有变化的 DOM（恒值段不产生任何变更记录）、SVG 弧线。
// 所有 set* 都先读后写：宿主自动化回放里大量帧的值没变，这里是「不闪」的最后一道闸。

// 汉字、CJK 标点与全角形式。汉字区间从常用区起：字体检查把源码里出现的字符都当必需字符，扩展 A 区的起点字不在子集字体里
const CJK = /[\u4e00-\u9fff\u3000-\u303f\uff00-\uffef]/g;

/**
 * 写一行中英数字混排的说明，并按汉字占比标 data-cjk：占比超过三成的行在中文界面下收紧字距（汉字自带字距），
 * 数字多的行保持原字距。纯标签的元素不用它，样式表里直接按语言收紧。
 */
export function setLine(node, text) {
    const s = String(text);
    setText(node, s);
    const han = (s.match(CJK) || []).length;
    const heavy = han / Math.max(1, s.replace(/\s/g, "").length) > 0.3;
    if (heavy) {
        if (node.getAttribute("data-cjk") !== "1") node.setAttribute("data-cjk", "1");
    } else if (node.hasAttribute("data-cjk")) node.removeAttribute("data-cjk");
}

/** 文字内容变了才写。 */
export function setText(node, text) {
    const s = String(text);
    if (node.textContent !== s) node.textContent = s;
}

/** 属性：null、undefined、false 为移除，true 写 "1"，其余转字符串；值相同不写。 */
export function setAttr(node, name, value) {
    if (value === null || value === undefined || value === false) {
        if (node.hasAttribute(name)) node.removeAttribute(name);
        return;
    }
    const s = value === true ? "1" : String(value);
    if (node.getAttribute(name) !== s) node.setAttribute(name, s);
}

/** 自定义属性或样式属性：值相同不写。 */
export function setVar(node, name, value) {
    const s = String(value);
    if (node.style.getPropertyValue(name) !== s) node.style.setProperty(name, s);
}

/** hidden 属性（HTML 与 SVG 节点都适用，样式在 input.css 里统一）：值相同不写。 */
export function setHidden(node, hidden) {
    if (node.hasAttribute("hidden") === hidden) return;
    if (hidden) node.setAttribute("hidden", "");
    else node.removeAttribute("hidden");
}

/** 悬停说明：title 属性。 */
export function setTitle(node, text) {
    setAttr(node, "title", text || null);
}

let uid = 0;
/** 页内唯一的 id 前缀（SVG 的滤镜与图案要引用 id）。 */
export function nextId(prefix) {
    uid += 1;
    return `${prefix}${uid}`;
}

/** 极坐标：角度 0 在正上方，顺时针为正。 */
export function polar(cx, cy, r, deg) {
    const rad = ((deg - 90) * Math.PI) / 180;
    return [cx + r * Math.cos(rad), cy + r * Math.sin(rad)];
}

/** 圆弧路径（顺时针，a0 到 a1 度）；太短的弧返回空串。 */
export function arcPath(cx, cy, r, a0, a1) {
    if (!(a1 - a0 >= 0.05)) return "";
    const [x0, y0] = polar(cx, cy, r, a0);
    const [x1, y1] = polar(cx, cy, r, a1);
    return `M${x0.toFixed(2)} ${y0.toFixed(2)}A${r} ${r} 0 ${a1 - a0 > 180 ? 1 : 0} 1 ${x1.toFixed(2)} ${y1.toFixed(2)}`;
}
