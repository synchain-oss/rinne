// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/web.json
//
// 引导的蒙版、亮区与说明框的公用画法。
//
// 为什么单立一件：两个插件的引导是两套步骤机，但「暗蒙版 + 挖亮圆角洞 + 四向避让的说明框」是同一套画法。
// 画法留两份会各自漂移（羽化半径、内边距、避让间距、箭头方位），两个插件的引导就不像同一个产品。
// 所以把无状态的几何与绘制放在这里，步骤机、文案、锚点和桥调用留在各页。
//
// 职责边界：
//   - 本文件不建 DOM，不挂事件，不读 i18n，不调桥，不写 state；只对调用方传进来的
//     canvas、说明框、箭头元素做「量、画、摆」。
//   - 模块顶层没有副作用，也不触碰 document（检查脚本与冒烟脚本 import 即跑，顶层碰 document 会当场报错）。
//   - 颜色一律读设计 token（--void、--r-callout、--dark-*），不写裸色值；holeMask 遮罩图里的 #000 只表示不透明度。

/** 亮区洞的圆角，与说明框同款浮层卡圆角（--r-callout）。 */
export const SPOT_RADIUS = 16;

/** 亮区洞的内边距（设计 px），不小于 8。 */
export const SPOT_PAD = 10;

/** 羽化半径（设计 px）：亮区边缘的柔化。 */
export const SPOT_FEATHER = 12;

/**
 * 引导层的基础样式规则（蒙版、说明框、步骤指示、按钮行、箭头）。
 * 调用方在自己的 ensureStyle 里写 [...TOUR_BASE_CSS, ...页内专有规则].join("\n")。
 * 选择器一律以 .tour- 开头且互不重复，与页内追加的规则拼接时顺序无关。
 */
export const TOUR_BASE_CSS = Object.freeze([
    ".tour-overlay { position: absolute; inset: 0; z-index: 1000; pointer-events: auto; }",
    ".tour-mask { position: absolute; inset: 0; width: 100%; height: 100%; display: block; }",
    ".tour-callout { position: absolute; z-index: 1; box-sizing: border-box; width: 360px;",
    "  padding: var(--sp-16) var(--sp-18) var(--sp-14); border-radius: var(--r-callout);",
    "  background: linear-gradient(158deg, rgba(34, 31, 44, 0.72), rgba(20, 17, 28, 0.8));",
    "  -webkit-backdrop-filter: blur(16px) saturate(140%);",
    "  backdrop-filter: blur(16px) saturate(140%);",
    "  border: 1px solid rgba(var(--wh), 0.16); box-shadow: var(--dark-modal-shadow);",
    "  color: var(--txt-dark-2); outline: none; }",
    ".tour-callout__title { margin: 0 0 var(--sp-6); font-family: var(--ff-grotesk);",
    "  font-weight: 600; font-size: var(--fs-160); color: var(--txt-dark-1); }",
    ".tour-callout__body { font-size: var(--fs-125); line-height: 1.55;",
    "  color: var(--txt-dark-2); text-wrap: pretty; }",
    ".tour-callout__hint { margin-top: var(--sp-8); font-family: var(--ff-mono);",
    "  font-size: var(--fs-105); letter-spacing: var(--mono-ls-pill); color: var(--txt-dark-4); }",
    ".tour-callout__foot { display: flex; flex-direction: column; align-items: stretch;",
    "  gap: var(--sp-8); margin-top: var(--sp-12); }",
    ".tour-callout__indicator { display: flex; align-items: center; gap: var(--sp-10); flex: none; }",
    ".tour-callout__step { font-family: var(--ff-mono); font-size: var(--fs-105);",
    "  letter-spacing: var(--mono-ls); color: var(--txt-dark-3); font-variant-numeric: tabular-nums; }",
    ".tour-callout__dots { display: inline-flex; gap: 3px; }",
    ".tour-callout__dot { width: 4px; height: 4px; border-radius: 50%; background: rgba(var(--wh), 0.22); }",
    '.tour-callout__dot[data-current="1"] { background: var(--acc-cta); }',
    ".tour-callout__actions { display: flex; justify-content: flex-end; gap: var(--sp-8); flex: none; }",
    ".tour-callout__arrow { position: absolute; width: 12px; height: 12px;",
    "  background: var(--dark-solid); transform: rotate(45deg); pointer-events: none; }",
]);

/** 蒙版底色：读 --void token 并压到 0.72 透明度；读不到就用 token 的默认值兜底。 */
export function maskFill(card) {
    let v = "14,10,22";
    try {
        const c = getComputedStyle(card).getPropertyValue("--void");
        if (c && c.trim()) v = c.trim();
    } catch {
        /* 兜底：--void 的默认值 */
    }
    return "rgba(" + v + ", 0.72)";
}

/** 圆角矩形路径；半径按短边收敛，避免小目标画出花瓣形。 */
export function roundRectPath(ctx, x, y, w, h, r) {
    const rr = Math.min(r, w / 2, h / 2);
    ctx.beginPath();
    ctx.moveTo(x + rr, y);
    ctx.arcTo(x + w, y, x + w, y + h, rr);
    ctx.arcTo(x + w, y + h, x, y + h, rr);
    ctx.arcTo(x, y + h, x, y, rr);
    ctx.arcTo(x, y, x + w, y, rr);
    ctx.closePath();
}

/**
 * 画一帧蒙版：整卡铺暗色，再用 destination-out 加 shadowBlur 挖出羽化的圆角亮区。
 * @param {HTMLCanvasElement} mask 蒙版 canvas
 * @param {Element} card 挂载卡（尺寸基准）
 * @param {{x:number,y:number,w:number,h:number}|null} spotLocal 亮区（卡内坐标）；null 表示没有亮区
 */
export function drawMask(mask, card, spotLocal) {
    const dpr = (typeof window !== "undefined" && window.devicePixelRatio) || 1;
    const w = card.clientWidth;
    const h = card.clientHeight;
    mask.width = Math.round(w * dpr);
    mask.height = Math.round(h * dpr);
    const ctx = mask.getContext("2d");
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);

    ctx.fillStyle = maskFill(card);
    ctx.fillRect(0, 0, w, h);

    if (spotLocal && spotLocal.w > 0 && spotLocal.h > 0) {
        ctx.save();
        ctx.globalCompositeOperation = "destination-out";
        ctx.shadowColor = "rgba(0, 0, 0, 1)";
        ctx.shadowBlur = SPOT_FEATHER;
        ctx.beginPath();
        roundRectPath(ctx, spotLocal.x, spotLocal.y, spotLocal.w, spotLocal.h, SPOT_RADIUS);
        ctx.fill();
        ctx.fill(); // 画两遍让洞心完全透明，羽化只留在边缘
        ctx.restore();
    }
}

/**
 * 量 [data-tour="<anchor>"] 的亮区矩形（卡内坐标，已按 SPOT_PAD 外扩）。
 * 目标在滚动区里不可见时，先做最小滚动让它可见。
 * @returns {{x:number,y:number,w:number,h:number}|null} 锚点不存在时返回 null（即没有亮区）
 */
export function spotRectOf(root, anchor, mask) {
    if (!anchor) return null;
    const target = root.querySelector('[data-tour="' + anchor + '"]');
    if (!target) return null;
    try {
        target.scrollIntoView({ block: "nearest", inline: "nearest" });
    } catch {
        /* 个别 WebView 不支持 options 参数，忽略即可 */
    }
    // 以蒙版 canvas 自身的包围盒为坐标基准（与 drawMask 用的 clientWidth 同源），
    // 避免卡片带 1px 描边时 border-box 与 padding-box 差 2px 造成亮区偏移。
    const cr = mask.getBoundingClientRect();
    const tr = target.getBoundingClientRect();
    // 缩放档下（外壳走 CSS zoom）包围盒量到的是视觉 px，而 drawMask 与 placeCallout 全程按
    // clientWidth / clientHeight 的布局 px 作画；不换算的话，非 1.0 档的亮区会整体偏移放大，
    // 大档甚至跑出卡外。蒙版铺满整卡、与画布同源，所以用它两种量法之比求有效缩放，
    // 把视觉 px 除回布局 px；SPOT_PAD 本来就是设计 px，除完再外扩。
    const k = mask.clientWidth > 0 ? cr.width / mask.clientWidth : 1;
    const s = Number.isFinite(k) && k > 0 ? k : 1;
    return {
        x: (tr.left - cr.left) / s - SPOT_PAD,
        y: (tr.top - cr.top) / s - SPOT_PAD,
        w: tr.width / s + SPOT_PAD * 2,
        h: tr.height / s + SPOT_PAD * 2,
    };
}

function clamp(v, lo, hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

/**
 * 说明框四向避让亮区、限制在设计盒内，并把箭头摆到朝向亮区的那条边上。
 * 没有亮区（首步的居中卡）时居中放置并隐藏箭头。
 * @param {Element} callout 说明框
 * @param {Element} arrowEl 箭头
 * @param {Element} card 挂载卡
 * @param {{x:number,y:number,w:number,h:number}|null} spotLocal 亮区（卡内坐标）
 */
export function placeCallout(callout, arrowEl, card, spotLocal) {
    const cw = callout.offsetWidth;
    const ch = callout.offsetHeight;
    const W = card.clientWidth;
    const H = card.clientHeight;
    const margin = 12;
    const gap = 14;

    let x;
    let y;
    let edge = "none";
    let off = 0; // 箭头沿边缘的偏移（距说明框左上角）

    if (!spotLocal) {
        x = (W - cw) / 2;
        y = (H - ch) / 2;
    } else {
        const sx = spotLocal.x;
        const sy = spotLocal.y;
        const sw = spotLocal.w;
        const sh = spotLocal.h;
        const cx = sx + sw / 2;
        const cy = sy + sh / 2;

        const fitsBelow = sy + sh + gap + ch <= H - margin;
        const fitsAbove = sy - gap - ch >= margin;
        const fitsRight = sx + sw + gap + cw <= W - margin;
        const fitsLeft = sx - gap - cw >= margin;

        if (fitsBelow) {
            edge = "top";
            x = clamp(cx - cw / 2, margin, W - margin - cw);
            y = sy + sh + gap;
            off = clamp(cx - x, 18, cw - 18);
        } else if (fitsAbove) {
            edge = "bottom";
            x = clamp(cx - cw / 2, margin, W - margin - cw);
            y = sy - gap - ch;
            off = clamp(cx - x, 18, cw - 18);
        } else if (fitsRight) {
            edge = "left";
            y = clamp(cy - ch / 2, margin, H - margin - ch);
            x = sx + sw + gap;
            off = clamp(cy - y, 18, ch - 18);
        } else if (fitsLeft) {
            edge = "right";
            y = clamp(cy - ch / 2, margin, H - margin - ch);
            x = sx - gap - cw;
            off = clamp(cy - y, 18, ch - 18);
        } else {
            // 四向都放不下（极小视口）：贴目标右下，箭头朝上
            edge = "top";
            x = clamp(sx + sw + gap, margin, W - margin - cw);
            y = clamp(sy + sh + gap, margin, H - margin - ch);
            off = 18;
        }
    }

    callout.style.left = Math.round(x) + "px";
    callout.style.top = Math.round(y) + "px";

    const AW = 12;
    if (edge === "none") {
        arrowEl.hidden = true;
    } else {
        arrowEl.hidden = false;
        if (edge === "top") {
            arrowEl.style.left = Math.round(off - AW / 2) + "px";
            arrowEl.style.top = Math.round(-AW / 2) + "px";
        } else if (edge === "bottom") {
            arrowEl.style.left = Math.round(off - AW / 2) + "px";
            arrowEl.style.top = Math.round(ch - AW / 2) + "px";
        } else if (edge === "left") {
            arrowEl.style.left = Math.round(-AW / 2) + "px";
            arrowEl.style.top = Math.round(off - AW / 2) + "px";
        } else {
            arrowEl.style.left = Math.round(cw - AW / 2) + "px";
            arrowEl.style.top = Math.round(off - AW / 2) + "px";
        }
    }
}

// ---------------------------------------------------------------- 首启导览用的画法（设计稿 v2.5）
// 导览的遮罩与模态同款：暗幕加背景模糊，亮区是用遮罩图挖出的羽化矩形洞（不是上面的 canvas 圆角洞）；
// 说明框按步骤给的首选位置摆，压到亮区、要避开的矩形或越出设计盒时才换方位。

/** 亮区洞边缘的羽化（设计 px）。 */
export const HOLE_FEATHER = 7;

/** 换方位时说明框离设计盒边缘、离亮区的距离（设计 px）。首选位置只要求整框在设计盒内（设计稿 Output 第 3 步的框就贴着底边）。 */
export const CALLOUT_MARGIN = 8;
export const CALLOUT_GAP = 8;

/**
 * 挖洞的遮罩图：两条线性渐变叠加，横向或纵向落在洞外的点总有一条不透明，所以洞外整片不透明、洞内透明，
 * 四边各羽化 f。遮罩只看不透明度，#000 不是界面颜色。
 * @param {{x:number,y:number,w:number,h:number}} r 亮区（设计盒坐标）
 */
export function holeMask(r, f = HOLE_FEATHER) {
    const span = (a, len) =>
        `#000 ${a - f}px, transparent ${a + f}px, transparent ${a + len - f}px, #000 ${a + len + f}px`;
    return `linear-gradient(to right, ${span(r.x, r.w)}), linear-gradient(to bottom, ${span(r.y, r.h)})`;
}

/** 两个矩形是否相交（贴边不算）。 */
export function rectsOverlap(a, b) {
    return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

/**
 * 说明框的位置。先试首选位置，再依次试亮区的下、上、右、左；合格是指整框在设计盒内，
 * 不压亮区，也不压 avoid 里的任何矩形（Input 的紧急切断）。全都不合格时返回首选位置，ok 为 false，由冒烟报出来。
 * @param {{w:number,h:number}} box 设计盒
 * @param {{w:number,h:number}} size 说明框的实际尺寸（随语言变）
 * @param {{x:number,y:number,w:number,h:number}|null} spot 亮区；null 表示这一步没有锚点
 * @param {{x?:number, y?:number, below?:number, above?:number, center?:boolean}} pref
 *   x 缺省时水平居中；center 垂直居中；below / above 是相对亮区的下沿 / 上沿的间距；否则用 y
 * @param {Array<{x:number,y:number,w:number,h:number}>} [avoid]
 * @returns {{x:number, y:number, ok:boolean, how:string}}
 */
export function pickCalloutPos(box, size, spot, pref, avoid = []) {
    const M = CALLOUT_MARGIN;
    const fits = ([, x, y]) => {
        const r = { x, y, w: size.w, h: size.h };
        if (x < 0 || y < 0 || x + size.w > box.w || y + size.h > box.h) return false;
        return !(spot && rectsOverlap(r, spot)) && !avoid.some((a) => rectsOverlap(r, a));
    };
    const x0 = Number.isFinite(pref.x) ? pref.x : Math.round((box.w - size.w) / 2);
    let y0 = pref.y;
    if (pref.center || !Number.isFinite(y0)) y0 = Math.round((box.h - size.h) / 2);
    if (spot && Number.isFinite(pref.below)) y0 = spot.y + spot.h + pref.below;
    if (spot && Number.isFinite(pref.above)) y0 = spot.y - pref.above - size.h;
    const cands = [["pref", x0, y0]];
    if (spot) {
        const cx = clamp(x0, M, box.w - M - size.w);
        const cy = clamp(spot.y, M, box.h - M - size.h);
        cands.push(
            ["below", cx, spot.y + spot.h + CALLOUT_GAP],
            ["above", cx, spot.y - CALLOUT_GAP - size.h],
            ["right", spot.x + spot.w + CALLOUT_GAP, cy],
            ["left", spot.x - CALLOUT_GAP - size.w, cy],
        );
    }
    const hit = cands.find(fits);
    const [how, x, y] = hit || cands[0];
    return { x: Math.round(x), y: Math.round(y), ok: Boolean(hit), how };
}
