// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/web.json
//
// 滚轮归一化：把一次滚轮事件折成带符号的像素位移，各画布共用同一份实现。
//
// 归一化要做两件事，少一件滚动就会在某条路径上失灵：
//   1. 轴：取 |deltaX| 与 |deltaY| 中较大的那个（带原符号）。鼠标滚轮给 deltaY，触控板横向双指给 deltaX；
//      Shift+滚轮在 Chromium 系会被改写成横向滚动（值落到 deltaX、deltaY 归零），Firefox 则仍走 deltaY。
//      取主轴对三种来源都成立，不必逐浏览器分支。只读 deltaY 的话，Shift 那一路永远拿到 0。
//   2. 单位：deltaMode 可能是像素（0）、行（1）、页（2）。不折成像素的话，同一次滚动在不同浏览器和
//      鼠标驱动上相差两个数量级，平移会从「挪一点」变成「跳一屏」。

/** deltaMode=1（行）时一行的像素当量（Chrome 桌面约 16 px 一行）。 */
export const WHEEL_LINE_PX = 16;

/** deltaMode=2（页）时一页的像素当量兜底值；调用方知道真实视口宽高时应当传进来。 */
export const WHEEL_PAGE_PX = 400;

/**
 * 滚轮位移归一到像素（带符号，正为向下或向右）。
 *
 * @param {WheelEvent|object} e 只读 deltaX / deltaY / deltaMode，方便在 node 里构造
 * @param {number} [pagePx] deltaMode=2 时一页折算多少像素
 * @returns {number} 像素位移；缺参或非数一律返回 0，调用方据此早退
 */
export function wheelPx(e, pagePx) {
    const ev = e || {};
    const dx = Number.isFinite(ev.deltaX) ? ev.deltaX : 0;
    const dy = Number.isFinite(ev.deltaY) ? ev.deltaY : 0;
    const raw = Math.abs(dx) > Math.abs(dy) ? dx : dy;
    if (ev.deltaMode === 1) return raw * WHEEL_LINE_PX;
    if (ev.deltaMode === 2) {
        const page = Number.isFinite(pagePx) && pagePx > 0 ? pagePx : WHEEL_PAGE_PX;
        return raw * page;
    }
    return raw;
}
