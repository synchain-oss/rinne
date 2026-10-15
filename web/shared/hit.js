// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/web.json
//
// 命中测试工具：只做几何，不碰 DOM。
//
// 口径有三条，出处不同，不要混用：
//   - 一般控件：命中半径不小于 12 设计 px；
//   - 段边界竖线：±6 设计 px（边界排得更密，半径大了会串）；
//   - 小靶件（选区 chip 的叉、窄手柄等）：最小缩放档 0.5 下命中区仍须不小于 24×24 物理 px，
//     用透明扩展区补足，视觉尺寸不变。
// 命中测试一律用 CSS 坐标（getBoundingClientRect 结果按 zoom 校正）。

/** 一般控件命中半径（设计 px）。 */
export const HIT_RADIUS_PX = 12;

/** 段边界竖线命中半径（设计 px）。 */
export const BOUNDARY_HIT_PX = 6;

/** 命中区的物理下限（px）。 */
export const MIN_TARGET_PHYS_PX = 24;

/** 校验小靶件时采用的最小缩放档。 */
export const MIN_UI_SCALE = 0.5;

function num(v, dflt) {
    return Number.isFinite(v) ? v : dflt;
}

/** 一维命中：|x - target| <= r。 */
export function withinRadius(x, target, r = HIT_RADIUS_PX) {
    return Math.abs(num(x, NaN) - num(target, NaN)) <= r;
}

/** 边界竖线命中（±6 设计 px 专用口径）。 */
export function hitsBoundary(x, boundaryX) {
    return withinRadius(x, boundaryX, BOUNDARY_HIT_PX);
}

/**
 * 在升序竖线表里找命中的下标（边界拖拽与吸附共用）。
 * 命中多条时取最近的一条；无命中返回 -1。
 * @param {number} x 指针 x（CSS px）
 * @param {number[]} xs 升序竖线位置表
 * @param {number} [r] 半径，缺省为边界口径 ±6
 */
export function nearestHit(x, xs, r = BOUNDARY_HIT_PX) {
    let best = -1;
    let bestD = Infinity;
    for (let i = 0; i < (xs ? xs.length : 0); i++) {
        const d = Math.abs(num(xs[i], NaN) - num(x, NaN));
        if (d <= r && d < bestD) {
            best = i;
            bestD = d;
        }
    }
    return best;
}

/**
 * 透明扩展区（每侧，设计 px）：让视觉尺寸为 visualPx 的靶件在 uiScale 档下物理命中区达到 24 px。
 * 已经够大则返回 0。
 *   例：15 px 的叉在 0.5 档物理 7.5 px，每侧补 (24 / 0.5 - 15) / 2 = 16.5 设计 px。
 * @param {number} visualPx 视觉尺寸（设计 px）
 * @param {number} [uiScale] 缺省按最小档 0.5 校验
 */
export function hitExpansionPx(visualPx, uiScale = MIN_UI_SCALE) {
    const s = num(uiScale, MIN_UI_SCALE);
    const need = MIN_TARGET_PHYS_PX / (s > 0 ? s : MIN_UI_SCALE);
    const v = num(visualPx, 0);
    return v >= need ? 0 : (need - v) / 2;
}

/**
 * 事件坐标换成元素内的 CSS 坐标（按 zoom 校正）。
 * rect 是 getBoundingClientRect() 的结果；zoom 是设计盒缩放系数，rect 已被它放大，
 * 除回去得到 1x 的设计坐标。纯几何，DOM 读取留给调用方。
 */
export function eventToLocal(clientX, clientY, rect, zoom = 1) {
    const z = num(zoom, 1) || 1;
    return {
        x: (num(clientX, 0) - num(rect && rect.left, 0)) / z,
        y: (num(clientY, 0) - num(rect && rect.top, 0)) / z,
    };
}

// -----------------------------------------------------------------------------
// CSS zoom 下 getBoundingClientRect 的语义探测
//
// 上面的 eventToLocal，以及所有把 rect 折回设计坐标的命中测试，都按新语义写：rect 是缩放后的视觉尺寸，
// left / top 是视口坐标，与事件的 clientX / clientY 同一坐标系。Chromium 128+（WebView2 Evergreen）
// 与 WebKit 26.4+ 如此。
//
// WebKit 26.4 之前（macOS 系统 WKWebView，较旧的系统升不到 26.4）是旧语义：rect 按元素的有效 zoom
// 除回去，宽高与位置都不含缩放，而事件坐标仍是视口坐标，zoom 不为 1 时命中整体偏移。
//
// 旧语义下降级而不换算：外壳缩放锁在 1（见 shell-fit.js）。zoom 为 1 时两种语义逐字相同，命中按构造
// 就是对的。不做换算是因为旧 WebKit 里事件坐标与 rect 的确切关系无法在 CI 上验证，盲换没有保证。
//
// 判据只认量到的语义，不读 UA，不读版本号：放一个 zoom:2、宽 100 px 的不可见探针，
//   新语义：rect 宽 200，offsetWidth 100；
//   旧语义：rect 宽 100，offsetWidth 100。
// offset* 在两种语义下都不含 zoom，所以「rect 宽 / offsetWidth」约等于探针 zoom 即新语义，约等于 1 即旧语义。
// -----------------------------------------------------------------------------

/** rect 含 zoom（新语义）。 */
export const ZOOM_RECT_STANDARD = "standard";
/** rect 不含 zoom（旧语义）。 */
export const ZOOM_RECT_LEGACY = "legacy";
/** 量不出来（没有 DOM，或探针没布局出尺寸）。调用方按新语义处理，不改任何行为。 */
export const ZOOM_RECT_UNKNOWN = "unknown";

/** 探针的 zoom 与宽度（px）。 */
export const ZOOM_PROBE_FACTOR = 2;
export const ZOOM_PROBE_PX = 100;

/**
 * 纯判定（node 里可直接断言）：探针的 rect 宽与 offsetWidth 换成语义。
 *
 * 比值离探针 zoom 不到 0.25 判新语义，离 1 不到 0.1 判旧语义；两头都不沾（含 0 与 NaN）判 unknown。
 * 阈值只为吸收亚像素舍入，两档中间留有大片空档，不会把一种判成另一种。
 */
export function classifyZoomRect(rectWidth, offsetWidth) {
    const rw = num(rectWidth, NaN);
    const ow = num(offsetWidth, NaN);
    if (!(rw > 0) || !(ow > 0)) return ZOOM_RECT_UNKNOWN;
    const k = rw / ow;
    if (Math.abs(k - ZOOM_PROBE_FACTOR) < 0.25) return ZOOM_RECT_STANDARD;
    if (Math.abs(k - 1) < 0.1) return ZOOM_RECT_LEGACY;
    return ZOOM_RECT_UNKNOWN;
}

/** 按文档缓存：一个文档的语义在其生命期内不变；WeakMap 不会留住已拆掉的文档。 */
const zoomModeCache = new WeakMap();

/**
 * 当前文档里 CSS zoom 下 getBoundingClientRect 的语义（按文档缓存）。
 *
 * 探针挂在 body（没有 body 时挂 documentElement）下，量完立刻摘掉；它不可见、不接收指针、
 * 不进无障碍树。unknown 不缓存，下一次调用会重新量。
 *
 * @param {Document} [doc] 注入用，缺省取全局 document
 * @returns {"standard"|"legacy"|"unknown"}
 */
export function zoomRectMode(doc) {
    const d = doc || (typeof document !== "undefined" ? document : null);
    if (!d || typeof d.createElement !== "function") return ZOOM_RECT_UNKNOWN;
    if (zoomModeCache.has(d)) return zoomModeCache.get(d);
    const host = d.body || d.documentElement;
    if (!host || typeof host.appendChild !== "function") {
        return ZOOM_RECT_UNKNOWN;
    }
    const probe = d.createElement("div");
    probe.setAttribute("aria-hidden", "true");
    probe.style.cssText =
        "position:absolute;left:0;top:0;height:1px;margin:0;padding:0;" +
        "border:0;visibility:hidden;pointer-events:none;" +
        `width:${ZOOM_PROBE_PX}px;zoom:${ZOOM_PROBE_FACTOR}`;
    host.appendChild(probe);
    let mode = ZOOM_RECT_UNKNOWN;
    try {
        mode = classifyZoomRect(probe.getBoundingClientRect().width, probe.offsetWidth);
    } finally {
        host.removeChild(probe);
    }
    if (mode !== ZOOM_RECT_UNKNOWN) zoomModeCache.set(d, mode);
    return mode;
}
