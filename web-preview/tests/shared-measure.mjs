// SPDX-License-Identifier: GPL-3.0-or-later
//
// 共享件页面级冒烟的公共件：夹具场景表、开页、页内量尺脚本、PNG 解码与对比度计算。
// 这里只放可复用的函数，不自己跑断言；smoke-scenarios / smoke-fit / smoke-contrast / smoke-a11y 的共享件那几个脚本各自引用。
// 夹具页是 web-preview/fixtures/shared/index.html，观测接口挂在 window.__fx 上（见 fixture.js 头部）。

import zlib from "node:zlib";
import { pathToFileURL } from "node:url";
import path from "node:path";

export const ROLES = Object.freeze(["input", "output"]);

/** 配对 15 态的色语义（红 / 琥珀 / 灰 / 绿），行为冒烟与纯函数单测共用，不各写一份。 */
export const PAIR_TONES = Object.freeze({
    ipcUnavailable: "red",
    abiMismatch: "red",
    srMismatch: "red",
    conflict: "red",
    unassigned: "red",
    stale: "amber",
    crossProcess: "amber",
    crossTrack: "amber",
    searching: "gray",
    waitingAudio: "gray",
    validator: "gray",
    bypassed: "gray",
    connectedUnverified: "green",
    connected: "green",
    connectedStill: "green",
});
export const LANGS = Object.freeze(["zh", "en", "fr"]);

/** 设计盒：从仓库里读，不在这里写第二份。 */
export async function loadDesign(root) {
    const m = await import(pathToFileURL(path.join(root, "web/shared/design-box.js")).href);
    return m.DESIGN;
}

/**
 * 场景表。win 为真的是整窗（视口取设计盒，倍率 1），否则是目录页（宽视口，高度随内容）。
 * 每个场景的 query 不含 role 与 lang，由 sceneUrl 补上。
 */
export const SCENES = Object.freeze([
    { id: "window", q: "scene=window", win: true },
    { id: "window-danger", q: "scene=window&danger=1&st=cutPanic", win: true },
    { id: "window-unassigned", q: "scene=window&unassigned=1&pair=unassigned", win: true },
    { id: "about", q: "overlay=about", win: true },
    { id: "about-copyFail", q: "overlay=about&st=copyFail", win: true },
    { id: "scale-confirm", q: "overlay=scale&st=7", win: true },
    { id: "toast-group", q: "overlay=toast&st=group", win: true },
    { id: "toast-diagCopied", q: "overlay=toast&st=diagCopied", win: true },
    { id: "toast-copyFail", q: "overlay=toast&st=copyFail", win: true },
    { id: "bands", q: "scene=bands", win: false },
    { id: "pair", q: "scene=pair", win: false },
    { id: "badges", q: "scene=badges", win: false },
    { id: "group", q: "scene=group", win: false },
    { id: "chain", q: "scene=chain", win: false },
]);

export function sceneUrl(server, role, lang, query) {
    return `${server.url}/web-preview/fixtures/shared/index.html?role=${role}&lang=${lang}&${query}`;
}

/** 打开一个场景并等夹具就绪；夹具自己报错时抛出。窗口场景的视口是设计盒，目录场景按内容撑高。 */
export async function openScene(page, server, design, role, lang, scene, extra = "") {
    const box = design[role];
    if (scene.win) await page.setViewport(box.w, box.h);
    else await page.setViewport(1200, 900);
    await page.goto(sceneUrl(server, role, lang, scene.q + extra));
    await page.waitFor("(window.__fx && window.__fx.ready === true) || Boolean(window.__fxError)", { timeout: 15000 });
    const err = await page.eval("window.__fxError || ''");
    if (err) throw new Error(`夹具出错（${role}/${lang}/${scene.id}）：${err}`);
    if (!scene.win) {
        const h = await page.eval("Math.ceil(document.documentElement.scrollHeight)");
        await page.setViewport(1200, Math.max(300, h));
    }
    await settleScene(page);
}

/**
 * 等页面真正静下来再量：字体、有限时长的过渡与动画都走完，再多等 260 ms 兜底。
 * 为什么不只靠固定等待：fonts.ready 在排版触发字体加载之前就可能已经兑现，机器偶尔慢的时候字体换入会发生在
 * 采样与截图之间，文字矩形挪位；过渡按墙钟走，主线程被占住时 260 ms 不一定够它走完。
 * 无限循环的脉冲与呼吸不等（它们不改文字与文字底）。
 */
export async function settleScene(page) {
    await page.eval(`(async () => {
        const frames = () => new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)));
        void document.body.offsetHeight;
        await frames();
        await document.fonts.ready;
        const t0 = performance.now();
        for (;;) {
            const pending = document.getAnimations().filter((a) => {
                const t = a.effect && a.effect.getComputedTiming();
                return a.playState === "running" && t && t.iterations !== Infinity;
            });
            if (!pending.length || performance.now() - t0 > 3000) break;
            await Promise.allSettled(pending.map((a) => a.finished));
        }
        await frames();
    })()`);
    await page.eval("new Promise((r) => setTimeout(r, 260))");
}

// ---------------------------------------------------------------- 页内量尺

/**
 * 页内求值的量尺脚本，返回 { problems:[{kind,detail}], texts:N, gb:N, visible:"…" }。
 * kind：outside（文字越出所在件或外壳）、clipped（被 overflow 裁掉）、overlap（两段文字互相压住）、
 * gb-outside（data-gb 件越出外壳）、wrapped（Input 一行消息位里的文字折行）。tol 是亚像素容差。
 */
export const FIT_PROBE = `(() => {
    const TOL = 1.5;
    const stage = document.getElementById("stage");
    const norm = (s) => s.replace(/\\s+/g, " ").trim();
    const problems = [];
    const shellEl = stage.querySelector('[data-gb$=".shell"]');
    const shellRect = shellEl ? shellEl.getBoundingClientRect() : null;
    const inside = (r, o) => r.left >= o.left - TOL && r.right <= o.right + TOL && r.top >= o.top - TOL && r.bottom <= o.bottom + TOL;
    const name = (e) => (e.getAttribute && (e.getAttribute("data-gb") || e.getAttribute("data-part"))) || e.className || e.tagName;
    const shown = (e) => {
        for (let n = e; n && n !== document.documentElement; n = n.parentElement) {
            if (n.hidden) return false;
            const cs = getComputedStyle(n);
            if (cs.display === "none" || cs.visibility === "hidden" || Number(cs.opacity) === 0) return false;
        }
        return true;
    };
    // 纵向可滚动的区域（关于面板正文）：文字先滚进视野再量，互压则在区域自己的内容坐标里比
    const isScroller = (a) => {
        const oy = getComputedStyle(a).overflowY;
        return (oy === "auto" || oy === "scroll") && a.scrollHeight > a.clientHeight + 1;
    };
    const scrollerOf = (e) => {
        for (let a = e.parentElement; a && a !== stage; a = a.parentElement) if (isScroller(a)) return a;
        return null;
    };
    for (const e of stage.querySelectorAll("*")) {
        const oy = getComputedStyle(e).overflowY;
        if (oy === "auto" || oy === "scroll") e.scrollTop = 0;
    }
    const layerOf = (e) => (e.closest(".rn-toast") ? "toast" : e.closest(".rn-scrim") ? "modal" : "base");
    const items = [];
    const walker = document.createTreeWalker(stage, NodeFilter.SHOW_TEXT);
    for (let n = walker.nextNode(); n; n = walker.nextNode()) {
        const text = norm(n.nodeValue);
        const p = n.parentElement;
        if (!text || !p || p.closest(".fx-cap") || !shown(p)) continue;
        const sc = scrollerOf(p);
        if (sc) p.scrollIntoView({ block: "nearest", inline: "nearest" });
        const range = document.createRange();
        range.selectNodeContents(n);
        let r = range.getBoundingClientRect();
        if (r.width === 0 && r.height === 0) continue;
        // 文字直接放在滚动框里（诊断预览）：滚动的那个轴只看露出来的一段
        {
            const cs = getComputedStyle(p);
            const pr = p.getBoundingClientRect();
            const sx = /auto|scroll/.test(cs.overflowX) && p.scrollWidth > p.clientWidth + 1;
            const sy = /auto|scroll/.test(cs.overflowY) && p.scrollHeight > p.clientHeight + 1;
            if (sx || sy) {
                r = {
                    left: sx ? Math.max(r.left, pr.left) : r.left,
                    right: sx ? Math.min(r.right, pr.right) : r.right,
                    top: sy ? Math.max(r.top, pr.top) : r.top,
                    bottom: sy ? Math.min(r.bottom, pr.bottom) : r.bottom,
                    width: r.width,
                    height: r.height,
                };
            }
        }
        const scr = sc ? sc.getBoundingClientRect() : null;
        const box = sc
            ? { left: r.left - scr.left + sc.scrollLeft, right: r.right - scr.left + sc.scrollLeft, top: r.top - scr.top + sc.scrollTop, bottom: r.bottom - scr.top + sc.scrollTop }
            : r;
        items.push({ p, r, box, sc, text });
        // 所在件：最近的 data-gb；没有就取目录里的底板
        const root = p.closest("[data-gb]") || p.closest(".fx-plate") || stage;
        const rr = root.getBoundingClientRect();
        if (!inside(r, rr)) problems.push({ kind: "outside", detail: name(p) + " 的文字 " + JSON.stringify(text.slice(0, 40)) + " 越出所在件 " + name(root) + "（" + [r.left - rr.left, r.right - rr.right, r.top - rr.top, r.bottom - rr.bottom].map((v) => v.toFixed(1)).join("/") + "）" });
        if (shellRect && !inside(r, shellRect)) problems.push({ kind: "outside", detail: name(p) + " 的文字 " + JSON.stringify(text.slice(0, 40)) + " 越出外壳" });
        // 祖先链上被裁的框
        for (let a = p; a && a !== stage; a = a.parentElement) {
            // 外壳自己裁掉装饰层（光晕、描边）是设计，不算；文字只看到所在件为止
            if (a === shellEl) break;
            const cs = getComputedStyle(a);
            // 只检查被裁的那个轴：overflow-y:auto 的区域（关于面板正文）纵向本来就靠滚动到达
            const clipX = /hidden|clip/.test(cs.overflowX);
            const clipY = /hidden|clip/.test(cs.overflowY);
            if (!clipX && !clipY) continue;
            const ar = a.getBoundingClientRect();
            const within =
                (!clipX || (r.left >= ar.left - TOL && r.right <= ar.right + TOL)) &&
                (!clipY || (r.top >= ar.top - TOL && r.bottom <= ar.bottom + TOL));
            if (!within) problems.push({ kind: "clipped", detail: name(p) + " 的文字 " + JSON.stringify(text.slice(0, 40)) + " 被 " + name(a) + " 裁掉" });
            if ((clipX && a.scrollWidth > a.clientWidth + 1) || (clipY && a.scrollHeight > a.clientHeight + 1))
                problems.push({ kind: "clipped", detail: name(a) + " 的内容比框大（" + a.scrollWidth + "x" + a.scrollHeight + " > " + a.clientWidth + "x" + a.clientHeight + "）" });
        }
    }
    // 文字互压：不在同一祖先链上的两段文字，相交面积不得超过 4 平方像素
    for (let i = 0; i < items.length; i++) {
        for (let j = i + 1; j < items.length; j++) {
            const a = items[i], b = items[j];
            if (a.p.contains(b.p) || b.p.contains(a.p)) continue;
            if (a.sc !== b.sc) continue;
            // 轻提示与模态浮在内容上面，压住下面的文字是设计；只比同一层
            if (layerOf(a.p) !== layerOf(b.p)) continue;
            const w = Math.min(a.box.right, b.box.right) - Math.max(a.box.left, b.box.left);
            const h = Math.min(a.box.bottom, b.box.bottom) - Math.max(a.box.top, b.box.top);
            if (w > 0 && h > 0 && w * h > 4)
                problems.push({ kind: "overlap", detail: JSON.stringify(a.text.slice(0, 24)) + " 与 " + JSON.stringify(b.text.slice(0, 24)) + " 互相压住（" + w.toFixed(1) + "x" + h.toFixed(1) + "）" });
        }
    }
    // Input 的消息位是一行槽（452×30）：文字一律不折行，太长就该越出槽被上面的检查抓到，不能靠折成两行悄悄塞进去
    for (const e of stage.querySelectorAll('[data-gb="input.band"] .rn-banner__text')) {
        if (!shown(e)) continue;
        const ws = getComputedStyle(e).whiteSpace;
        if (ws !== "nowrap") problems.push({ kind: "wrapped", detail: "input.band 的文字允许折行（white-space:" + ws + "）" });
        const range = document.createRange();
        range.selectNodeContents(e);
        const tops = new Set(Array.from(range.getClientRects()).map((r) => Math.round(r.top / 3)));
        if (tops.size > 1) problems.push({ kind: "wrapped", detail: "input.band 的文字折成了 " + tops.size + " 行" });
    }
    // data-gb 件不得越出外壳（遮罩与面板都在外壳里）
    let gb = 0;
    for (const e of stage.querySelectorAll("[data-gb]")) {
        if (!shown(e)) continue;
        gb++;
        if (!shellRect || e === shellEl) continue;
        const r = e.getBoundingClientRect();
        if (r.width === 0 && r.height === 0) continue;
        if (!inside(r, shellRect)) problems.push({ kind: "gb-outside", detail: name(e) + " 越出外壳" });
    }
    for (const e of stage.querySelectorAll("*")) {
        const oy = getComputedStyle(e).overflowY;
        if (oy === "auto" || oy === "scroll") e.scrollTop = 0;
    }
    // 悬停提示与读屏名称里的文字也要查「裸键、占位符、null」，一并带出
    const attrs = [];
    for (const e of stage.querySelectorAll("[title],[aria-label],[placeholder]")) {
        if (!shown(e)) continue;
        for (const a of ["title", "aria-label", "placeholder"]) {
            const v = e.getAttribute(a);
            if (v) attrs.push(v);
        }
    }
    return {
        problems,
        attrs,
        texts: items.length,
        gb,
        joined: norm(items.map((x) => x.text).join(" ")),
        glued: norm(items.map((x) => x.text).join("")),
    };
})()`;

/** 夹具登记的「必须上屏」文字里，没出现在可见文字里的那些。 */
export function missingExpected(expected, probe) {
    const norm = (s) => s.replace(/\s+/g, " ").trim();
    return expected.filter((e) => {
        const want = norm(e.text);
        return !probe.joined.includes(want) && !probe.glued.includes(want.replace(/ /g, ""));
    });
}

/** 界面上不该出现的东西：裸词条键、没替换的占位符、JS 的 null / undefined / [object X]。返回命中的片段。 */
export function findLeaks(text) {
    const pats = [
        /\b(?:in|out|common|band|about|toast)\.[A-Za-z][A-Za-z0-9]*(?:\.[A-Za-z][A-Za-z0-9]*)*/g,
        /\{\w+\}/g,
        /\b(?:null|undefined)\b/g,
        /\[object \w+\]/g,
    ];
    const hits = [];
    for (const re of pats) for (const m of text.matchAll(re)) hits.push(m[0]);
    return hits;
}

/** 页内读夹具登记的上屏文字。 */
export const EXPECTED_PROBE = "window.__fx.expectedTexts";

// ---------------------------------------------------------------- 文字对比度的页内采样

/**
 * 页内求值：列出每段可见文字的矩形、计算色（含各层 opacity）与分档。分档按 data 属性与类名划，
 *   A：危险横幅与红条、模态里的全部文字（正文与按钮）、确认条、主行动与次行动按钮；
 *   B：琥珀 / 灰 / 紫横幅，配对状态灯文字；
 *   C：其余，只记录不判（mono 小标等按规范不设门禁）。
 */
export const CONTRAST_PROBE = `(() => {
    const stage = document.getElementById("stage");
    const out = [];
    const shown = (e) => {
        for (let n = e; n && n !== document.documentElement; n = n.parentElement) {
            if (n.hidden) return false;
            const cs = getComputedStyle(n);
            if (cs.display === "none" || cs.visibility === "hidden" || Number(cs.opacity) === 0) return false;
        }
        return true;
    };
    const parse = (c) => {
        const m = /rgba?\\(([^)]+)\\)/.exec(c);
        if (!m) return null;
        const v = m[1].split(/[ ,\\/]+/).filter(Boolean).map(Number);
        return { r: v[0], g: v[1], b: v[2], a: v.length > 3 ? v[3] : 1 };
    };
    const tierOf = (p) => {
        if (p.closest(".fx-cap")) return null;
        // mono 小标（eyebrow）按规范不设门禁
        if (p.closest('[class*="eyebrow"]')) return "C";
        const band = p.closest(".rn-banner");
        // 诊断预览是 mono 读数，设计取 --txt-dark-mono；按 B 档门禁，不与正文同档
        if (p.closest(".rn-about__preview")) return "B";
        if (p.closest(".rn-modal")) return "A";
        if (band) {
            if (band.classList.contains("rn-banner--danger") || band.classList.contains("rn-banner--red") || band.classList.contains("rn-banner--confirm")) return "A";
            if (p.closest(".rn-btn")) return "A";
            if (/amber|gray|violet/.test(band.className)) return "B";
            if (band.classList.contains("rn-banner--neutral")) return "C";
        }
        if (p.closest(".rn-msgline") && p.closest(".rn-btn")) return "A";
        if (p.closest(".rn-pair")) return "B";
        return "C";
    };
    const isScroller = (a) => {
        const oy = getComputedStyle(a).overflowY;
        return (oy === "auto" || oy === "scroll") && a.scrollHeight > a.clientHeight + 1;
    };
    const scrollerOf = (e) => {
        for (let a = e.parentElement; a && a !== stage; a = a.parentElement) if (isScroller(a)) return a;
        return null;
    };
    for (const e of stage.querySelectorAll("*")) if (isScroller(e)) e.scrollTop = 0;
    // 模态打开时，盖在遮罩下面的文字实际被压暗，不是这里要判的对象
    const scrimOn = [...stage.querySelectorAll(".rn-scrim")].some(shown);
    const rectOf = (n, p) => {
        const range = document.createRange();
        range.selectNodeContents(n);
        let r = range.getBoundingClientRect();
        // 文字直接放在滚动框里（诊断预览）：滚动的那个轴只取露出来的一段
        const cs = getComputedStyle(p);
        const pr = p.getBoundingClientRect();
        const sx = /auto|scroll/.test(cs.overflowX) && p.scrollWidth > p.clientWidth + 1;
        const sy = /auto|scroll/.test(cs.overflowY) && p.scrollHeight > p.clientHeight + 1;
        const left = sx ? Math.max(r.left, pr.left) : r.left;
        const right = sx ? Math.min(r.right, pr.right) : r.right;
        const top = sy ? Math.max(r.top, pr.top) : r.top;
        const bottom = sy ? Math.min(r.bottom, pr.bottom) : r.bottom;
        return { x: left, y: top, w: right - left, h: bottom - top };
    };
    window.__cp = [];
    window.__cpPlace = (i) => {
        const it = window.__cp[i];
        if (it.sc) it.p.scrollIntoView({ block: "nearest", inline: "nearest" });
        return rectOf(it.n, it.p);
    };
    window.__cpRects = () => window.__cp.map((it) => (it.sc ? null : rectOf(it.n, it.p)));
    const walker = document.createTreeWalker(stage, NodeFilter.SHOW_TEXT);
    for (let n = walker.nextNode(); n; n = walker.nextNode()) {
        const text = n.nodeValue.replace(/\\s+/g, " ").trim();
        const p = n.parentElement;
        if (!text || !p || !shown(p)) continue;
        if (scrimOn && !p.closest(".rn-scrim")) continue;
        const tier = tierOf(p);
        if (!tier) continue;
        const r = rectOf(n, p);
        const sc = scrollerOf(p);
        if (!sc && (r.w < 1 || r.h < 1)) continue;
        let op = 1;
        for (let a = p; a && a !== document.documentElement; a = a.parentElement) op *= Number(getComputedStyle(a).opacity);
        const c = parse(getComputedStyle(p).color);
        if (!c) continue;
        const cls = (p.className && p.className.baseVal === undefined ? p.className : "") || p.tagName;
        const toneEl = p.closest("[data-tone]");
        window.__cp.push({ n, p, sc });
        out.push({ idx: window.__cp.length - 1, scrolled: Boolean(sc), tone: toneEl ? toneEl.getAttribute("data-tone") : "", text: text.slice(0, 48), tier, sel: cls.split(" ").filter(Boolean).slice(0, 3).join("."), r, c, op });
    }
    return out;
})()`;

/** 把页内所有文字改成透明（背景、边框、图标之外的东西都不动），截图时只剩合成底。 */
export const HIDE_TEXT_STYLE =
    "*{color:transparent !important;-webkit-text-fill-color:transparent !important;text-shadow:none !important;caret-color:transparent !important}";

// ---------------------------------------------------------------- PNG 解码与对比度

/** 解码 8 位、非隔行的 RGB / RGBA PNG，返回 { w, h, ch, data }。CDP 截图就是这种。 */
export function decodePng(buf) {
    if (buf.readUInt32BE(0) !== 0x89504e47) throw new Error("不是 PNG");
    let pos = 8;
    let w = 0;
    let h = 0;
    let depth = 0;
    let ctype = 0;
    let interlace = 0;
    const idat = [];
    while (pos < buf.length) {
        const len = buf.readUInt32BE(pos);
        const type = buf.toString("latin1", pos + 4, pos + 8);
        const body = buf.subarray(pos + 8, pos + 8 + len);
        if (type === "IHDR") {
            w = body.readUInt32BE(0);
            h = body.readUInt32BE(4);
            depth = body[8];
            ctype = body[9];
            interlace = body[12];
        } else if (type === "IDAT") idat.push(body);
        else if (type === "IEND") break;
        pos += 12 + len;
    }
    if (depth !== 8 || interlace !== 0 || (ctype !== 2 && ctype !== 6)) {
        throw new Error(`不支持的 PNG 格式：位深 ${depth}、颜色类型 ${ctype}、隔行 ${interlace}`);
    }
    const ch = ctype === 6 ? 4 : 3;
    const raw = zlib.inflateSync(Buffer.concat(idat));
    const stride = w * ch;
    const data = Buffer.alloc(stride * h);
    for (let y = 0; y < h; y++) {
        const f = raw[y * (stride + 1)];
        const src = y * (stride + 1) + 1;
        const dst = y * stride;
        for (let x = 0; x < stride; x++) {
            const a = x >= ch ? data[dst + x - ch] : 0;
            const b = y > 0 ? data[dst - stride + x] : 0;
            const c = x >= ch && y > 0 ? data[dst - stride + x - ch] : 0;
            let v = raw[src + x];
            if (f === 1) v += a;
            else if (f === 2) v += b;
            else if (f === 3) v += (a + b) >> 1;
            else if (f === 4) {
                const pa = Math.abs(b - c);
                const pb = Math.abs(a - c);
                const pc = Math.abs(a + b - 2 * c);
                v += pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
            }
            data[dst + x] = v & 255;
        }
    }
    return { w, h, ch, data };
}

const lin = (v) => {
    const s = v / 255;
    return s <= 0.04045 ? s / 12.92 : ((s + 0.055) / 1.055) ** 2.4;
};
export const luminance = (r, g, b) => 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b);
export function contrastRatio(l1, l2) {
    const hi = Math.max(l1, l2);
    const lo = Math.min(l1, l2);
    return (hi + 0.05) / (lo + 0.05);
}

/**
 * 一段文字在合成底上的最差对比度：矩形内逐像素取底色，把文字色（含 alpha 与各层 opacity）合成到该像素上，
 * 取全矩形的最小比值。返回 { min, at:{x,y}, bg:[r,g,b], fg:[r,g,b] }。
 */
export function worstContrast(png, item) {
    const { c, op, r } = item;
    const alpha = c.a * op;
    const x0 = Math.max(0, Math.floor(r.x));
    const y0 = Math.max(0, Math.floor(r.y));
    const x1 = Math.min(png.w, Math.ceil(r.x + r.w));
    const y1 = Math.min(png.h, Math.ceil(r.y + r.h));
    let worst = { min: Infinity, at: null, bg: null, fg: null };
    for (let y = y0; y < y1; y++) {
        for (let x = x0; x < x1; x++) {
            const i = (y * png.w + x) * png.ch;
            const br = png.data[i];
            const bgc = png.data[i + 1];
            const bb = png.data[i + 2];
            const fr = c.r * alpha + br * (1 - alpha);
            const fg = c.g * alpha + bgc * (1 - alpha);
            const fb = c.b * alpha + bb * (1 - alpha);
            const ratio = contrastRatio(luminance(fr, fg, fb), luminance(br, bgc, bb));
            if (ratio < worst.min)
                worst = {
                    min: ratio,
                    at: { x, y },
                    bg: [br, bgc, bb],
                    fg: [Math.round(fr), Math.round(fg), Math.round(fb)],
                };
        }
    }
    return worst;
}

/** 给一组页内采样项量出最差对比度，返回带 worst 的列表（用整页截图 png，都在视野里的那些）。 */
export function measureAll(png, items) {
    return items.map((it) => ({ ...it, worst: worstContrast(png, it) }));
}

/** 两次采样之间允许的矩形漂移（亚像素抖动）与不透明度 / 色值漂移。 */
const DRIFT_PX = 0.5;
const DRIFT_ALPHA = 0.01;

/**
 * 比较两遍采样，返回第一处不一致的说明；一致返回空串。
 * 对比度按「文字色 × 各层不透明度」合成到截图上算：采样时某层还在淡入淡出，或文字矩形随字体换入挪了位，
 * 量出来的就是一张从未同时存在过的画面，所以采样前后必须完全一致才往下量。
 */
export function itemsDrift(a, b) {
    if (a.length !== b.length) return `文字段数变了：${a.length} → ${b.length}`;
    for (let k = 0; k < a.length; k++) {
        const p = a[k];
        const q = b[k];
        const who = `${JSON.stringify(p.text)}（${p.sel}）`;
        if (p.text !== q.text || p.sel !== q.sel)
            return `第 ${k} 段换了内容：${who} → ${JSON.stringify(q.text)}（${q.sel}）`;
        for (const f of ["x", "y", "w", "h"])
            if (Math.abs(p.r[f] - q.r[f]) > DRIFT_PX)
                return `${who} 的矩形 ${f} 在漂：${p.r[f].toFixed(2)} → ${q.r[f].toFixed(2)}`;
        if (Math.abs(p.op - q.op) > DRIFT_ALPHA)
            return `${who} 的各层不透明度在变：${p.op.toFixed(3)} → ${q.op.toFixed(3)}`;
        for (const f of ["r", "g", "b"])
            if (Math.abs(p.c[f] - q.c[f]) > 1) return `${who} 的文字色 ${f} 通道在变：${p.c[f]} → ${q.c[f]}`;
        if (Math.abs(p.c.a - q.c.a) > DRIFT_ALPHA) return `${who} 的文字色 alpha 在变：${p.c.a} → ${q.c.a}`;
    }
    return "";
}

/** 截图后重量矩形，与采样时对照；返回第一处挪位的说明，没挪返回空串。 */
export function rectsDrift(items, rects) {
    for (let k = 0; k < items.length; k++) {
        const r = rects[k];
        if (!r || items[k].scrolled) continue;
        for (const f of ["x", "y", "w", "h"])
            if (Math.abs(items[k].r[f] - r[f]) > DRIFT_PX)
                return `${JSON.stringify(items[k].text)}（${items[k].sel}）截图期间矩形 ${f} 挪了：${items[k].r[f].toFixed(2)} → ${r[f].toFixed(2)}`;
    }
    return "";
}

/**
 * 页内量尺的完整一遍：采样（两遍确认页面已静止）→ 文字透明 → 整页截图量视野内的 →
 * 核对截图期间没有挪位 → 在可滚动区域里的逐段滚进视野、截取那一小块再量。
 * 页面一直不静止（重试 5 次）就抛错并带上第一处差异，不拿一张拼凑的画面去量。
 * 调用前页面必须已经打开夹具场景。
 */
export async function measureContrastOnPage(page) {
    let items;
    let png;
    let last = "";
    for (let attempt = 0; attempt < 5; attempt++) {
        const first = await page.eval(CONTRAST_PROBE);
        await page.eval("new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)))");
        items = await page.eval(CONTRAST_PROBE);
        last = itemsDrift(first, items);
        if (!last) {
            await page.eval(
                `(() => { const s = document.createElement("style"); s.id = "fx-hide-text"; s.textContent = ${JSON.stringify(HIDE_TEXT_STYLE)}; document.head.append(s); })()`,
            );
            await page.eval("new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)))");
            png = decodePng(await page.screenshot());
            last = rectsDrift(items, await page.eval("window.__cpRects()"));
            if (!last) break;
            await page.eval('(() => { const s = document.getElementById("fx-hide-text"); if (s) s.remove(); })()');
        }
        await settleScene(page);
    }
    if (last) throw new Error(`页面量不到静止画面：${last}`);
    const out = [];
    for (const it of items) {
        if (!it.scrolled) {
            if (it.r.w < 1 || it.r.h < 1) continue;
            out.push({ ...it, worst: worstContrast(png, it) });
            continue;
        }
        const r = await page.eval(`window.__cpPlace(${it.idx})`);
        if (!r || r.w < 1 || r.h < 1) continue;
        const cx = Math.max(0, Math.floor(r.x));
        const cy = Math.max(0, Math.floor(r.y));
        const cw = Math.max(1, Math.ceil(r.x + r.w) - cx);
        const ch = Math.max(1, Math.ceil(r.y + r.h) - cy);
        const shot = await page.send("Page.captureScreenshot", {
            format: "png",
            clip: { x: cx, y: cy, width: cw, height: ch, scale: 1 },
        });
        const small = decodePng(Buffer.from(shot.data, "base64"));
        out.push({ ...it, r, worst: worstContrast(small, { ...it, r: { x: r.x - cx, y: r.y - cy, w: r.w, h: r.h } }) });
    }
    return out;
}

export const TIER_MIN = Object.freeze({ A: 4.5, B: 3.0 });
