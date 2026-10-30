// SPDX-License-Identifier: GPL-3.0-or-later
//
// Input 页面级冒烟的公共件：开页、在 iframe 里求值、页内量尺（适配与对比度采样）、观察桥调用。
// 其余几个 smoke-input-*-page 与 smoke-knob-page、smoke-peakline-render-page 都从这里取工具。
//
// 与 shared-measure.mjs 的区别：那一份量的是共享件夹具页（舞台是 #stage、观测接口挂在 window.__fx）；
// 这里量的是真源页面本身，经预览壳页（web-preview/input.html）装载，页面在同源 iframe 里，
// 所以探针都写成函数 (doc, win, ...args)，由 inFrame 序列化后送进 iframe 求值。探针里不能引用外面的变量。
//
// 直接运行本文件时只自检量尺：对夹出来的坏页面（文字越框、被裁、互压、裸词条键）逐项断言必须被量出，
// 防止量尺本身失灵而整套全绿。浏览器缺失时整套跳过（退出码 2），不是通过。
//
// 用法：node web-preview/tests/smoke-input-kit-page.mjs [仓库根]

import { pathToFileURL } from "node:url";
import { createRun, repoRoot } from "./lib/harness.mjs";
import { findBrowser, launchBrowser } from "./lib/browser.mjs";
import { startStaticServer } from "./lib/static-server.mjs";
import { SCENES as ALL_SCENES } from "../mock/scenarios.js";
import { findLeaks } from "./shared-measure.mjs";

export { findLeaks };
export const LANGS = Object.freeze(["zh", "en", "fr"]);

/** 本页范围外的场景：预设面板（I-03、I-04）与首启（I-30 到 I-32）由后续的页面接手。 */
const OUT_OF_SCOPE = /^(in-lang|in-rules|in-tour-|in-preset-)/;
export const SCENES = Object.freeze(ALL_SCENES.filter((s) => s.role === "input" && !OUT_OF_SCOPE.test(s.id)));

/** 预览壳页的视口：够放下 2 档（1600×1160）。 */
export const VIEW = Object.freeze({ w: 1700, h: 1300 });

/**
 * 打开 Input 页面的某个场景并等它画完：壳页报「注入成功」、页面置位 __rinneBooted、字体与有限时长的动画走完。
 * 场景缺省时按默认态。fresh 先清全局位，免得上一页留下的语言或档位串进来。
 */
export async function openInput(page, server, scenario, { lang, scale, extra = "" } = {}) {
    const q = new URLSearchParams({ fresh: "1" });
    if (scenario) q.set("scenario", scenario);
    if (lang) q.set("lang", lang);
    if (scale) q.set("scale", String(scale));
    await page.setViewport(VIEW.w, VIEW.h, 1);
    await page.goto(`${server.url}/web-preview/input.html?${q}${extra}`);
    const label = `${scenario || "默认态"}/${lang || "-"}`;
    await page.waitFor(
        `(() => { const s = document.querySelector(".pv-status"); return !!s && s.getAttribute("data-ok") !== "wait"; })()`,
        { timeout: 20000, label: `壳页注入（${label}）` },
    );
    const status = await page.eval(`document.querySelector(".pv-status").textContent`);
    if (await page.eval(`document.querySelector(".pv-status").getAttribute("data-ok") === "0"`))
        throw new Error(`壳页注入失败（${label}）：${status}`);
    await page.waitFor(
        `(() => { const f = document.querySelector("iframe"); return !!(f && f.contentWindow && f.contentWindow.__rinneBooted === true && f.contentDocument.querySelector('[data-gb="input.shell"]')); })()`,
        { timeout: 20000, label: `页面 boot（${label}）` },
    );
    await settle(page);
}

/** 等页面静下来：字体、有限时长的动画与过渡走完再多等 260 ms；无限循环的脉冲不等。 */
export async function settle(page) {
    await inFrame(page, async (doc, win) => {
        const frames = () => new Promise((r) => win.requestAnimationFrame(() => win.requestAnimationFrame(r)));
        void doc.body.offsetHeight;
        await frames();
        await doc.fonts.ready;
        const t0 = win.performance.now();
        for (;;) {
            const pending = doc.getAnimations().filter((a) => {
                const t = a.effect && a.effect.getComputedTiming();
                return a.playState === "running" && t && t.iterations !== Infinity;
            });
            if (!pending.length || win.performance.now() - t0 > 3000) break;
            await Promise.allSettled(pending.map((a) => a.finished));
        }
        await frames();
        await new Promise((r) => win.setTimeout(r, 260));
    });
}

/** 在页面所在的 iframe 里求值：fn 是 (doc, win, ...args) => 可序列化的值（可以是 async），不能引用外面的变量。 */
export function inFrame(page, fn, ...args) {
    return page.eval(
        `(() => { const f = document.querySelector("iframe"); return (${fn.toString()})(f.contentDocument, f.contentWindow, ...${JSON.stringify(args)}); })()`,
    );
}

/** iframe 在壳页视口里的矩形：页内坐标换成壳页坐标（派发指针事件、截图取色）要用。 */
export function frameRect(page) {
    return page.eval(
        `(() => { const r = document.querySelector("iframe").getBoundingClientRect(); return { x: r.x, y: r.y, w: r.width, h: r.height }; })()`,
    );
}

/** 页面里某元素的中心，换算成壳页视口坐标。 */
export async function centerOf(page, selector) {
    const r = await page.eval(
        `(() => { const f = document.querySelector("iframe"); const e = f.contentDocument.querySelector(${JSON.stringify(selector)}); if (!e) return null; const b = e.getBoundingClientRect(); const o = f.getBoundingClientRect(); return { x: o.x + b.x + b.width / 2, y: o.y + b.y + b.height / 2, w: b.width, h: b.height }; })()`,
    );
    if (!r) throw new Error(`页面里没有 ${selector}`);
    return r;
}

/**
 * 在壳页里给桥函数装上调用记录：页面经 mock[name](...) 现查现调，所以改 session.mock 上的同名函数即可。
 * 返回 { calls() 取记录, clear() }。记录存在壳页的 window.__calls 里。
 */
export async function recordBridge(page, names) {
    await page.eval(`(() => {
        const mock = window.__RINNE_PREVIEW__.mock;
        window.__calls = [];
        for (const n of ${JSON.stringify(names)}) {
            if (mock.__wrapped && mock.__wrapped.includes(n)) continue;
            const orig = mock[n];
            mock[n] = function (...a) { window.__calls.push([n, ...a]); return orig.apply(mock, a); };
            (mock.__wrapped = mock.__wrapped || []).push(n);
        }
    })()`);
    return {
        calls: () => page.eval(`window.__calls.slice()`),
        clear: () => page.eval(`void (window.__calls.length = 0)`),
    };
}

// ---------------------------------------------------------------- 页内量尺

/**
 * 适配量尺：逐段可见文字查「越出所在件或外壳、被 overflow 裁掉、互相压住」，data-gb 件不越出外壳，
 * 消息位的文字不折行。返回 { problems:[{kind,detail}], texts, gb, joined, glued, attrs }。
 * kind：outside、clipped、overlap、gb-outside、wrapped。
 */
export function fitProbe(doc, win) {
    const TOL = 1.5;
    const gcs = (e) => win.getComputedStyle(e);
    const norm = (s) => s.replace(/\s+/g, " ").trim();
    const problems = [];
    const shellEl = doc.querySelector('[data-gb="input.shell"]');
    const shellRect = shellEl.getBoundingClientRect();
    const inside = (r, o) =>
        r.left >= o.left - TOL && r.right <= o.right + TOL && r.top >= o.top - TOL && r.bottom <= o.bottom + TOL;
    const name = (e) =>
        (e.getAttribute && (e.getAttribute("data-gb") || e.getAttribute("data-part"))) || e.className || e.tagName;
    const shown = (e) => {
        for (let n = e; n && n !== doc.documentElement; n = n.parentElement) {
            if (n.hidden) return false;
            const cs = gcs(n);
            if (cs.display === "none" || cs.visibility === "hidden" || Number(cs.opacity) === 0) return false;
        }
        return true;
    };
    const isScroller = (a) => {
        const oy = gcs(a).overflowY;
        return (oy === "auto" || oy === "scroll") && a.scrollHeight > a.clientHeight + 1;
    };
    const scrollerOf = (e) => {
        for (let a = e.parentElement; a && a !== shellEl; a = a.parentElement) if (isScroller(a)) return a;
        return null;
    };
    // 浮在内容上面的层：轻提示、模态、限制器关闭确认（不透明，盖住下面那几行）；压住下面的文字是设计，只比同一层
    const layerOf = (e) =>
        e.closest(".rn-toast")
            ? "toast"
            : e.closest(".rn-scrim")
              ? "modal"
              : e.closest(".in-limconfirm")
                ? "confirm"
                : "base";
    const items = [];
    const walker = doc.createTreeWalker(shellEl, win.NodeFilter.SHOW_TEXT);
    for (let n = walker.nextNode(); n; n = walker.nextNode()) {
        const text = norm(n.nodeValue);
        const p = n.parentElement;
        if (!text || !p || !shown(p)) continue;
        const sc = scrollerOf(p);
        if (sc) p.scrollIntoView({ block: "nearest", inline: "nearest" });
        const range = doc.createRange();
        range.selectNodeContents(n);
        let r = range.getBoundingClientRect();
        if (r.width === 0 && r.height === 0) continue;
        // 文字矩形取行盒：行高小于字体内容区（大号数字常见 line-height:1）时，内容区上下各多出一截留白，
        // 墨迹不在那里，按内容区算会把相邻两行误判成互压或越出所在件
        const lh = parseFloat(gcs(p).lineHeight);
        if (Number.isFinite(lh) && lh < r.height - 0.5 && r.height < lh * 2) {
            const cy = (r.top + r.bottom) / 2;
            r = { left: r.left, right: r.right, width: r.width, top: cy - lh / 2, bottom: cy + lh / 2, height: lh };
        }
        // 文字直接放在滚动框里（诊断预览）：滚动的那个轴只看露出来的一段
        {
            const cs = gcs(p);
            const pr = p.getBoundingClientRect();
            const sx = /auto|scroll/.test(cs.overflowX) && p.scrollWidth > p.clientWidth + 1;
            const sy = /auto|scroll/.test(cs.overflowY) && p.scrollHeight > p.clientHeight + 1;
            if (sx || sy)
                r = {
                    left: sx ? Math.max(r.left, pr.left) : r.left,
                    right: sx ? Math.min(r.right, pr.right) : r.right,
                    top: sy ? Math.max(r.top, pr.top) : r.top,
                    bottom: sy ? Math.min(r.bottom, pr.bottom) : r.bottom,
                    width: r.width,
                    height: r.height,
                };
        }
        const scr = sc ? sc.getBoundingClientRect() : null;
        const box = sc
            ? {
                  left: r.left - scr.left + sc.scrollLeft,
                  right: r.right - scr.left + sc.scrollLeft,
                  top: r.top - scr.top + sc.scrollTop,
                  bottom: r.bottom - scr.top + sc.scrollTop,
              }
            : r;
        items.push({ p, r, box, sc, text });
        const root = p.closest("[data-gb]") || shellEl;
        const rr = root.getBoundingClientRect();
        const cut = (v) => v.toFixed(1);
        if (!inside(r, rr))
            problems.push({
                kind: "outside",
                detail: `${name(p)} 的文字 ${JSON.stringify(text.slice(0, 40))} 越出所在件 ${name(root)}（${[r.left - rr.left, r.right - rr.right, r.top - rr.top, r.bottom - rr.bottom].map(cut).join("/")}）`,
            });
        if (!inside(r, shellRect))
            problems.push({
                kind: "outside",
                detail: `${name(p)} 的文字 ${JSON.stringify(text.slice(0, 40))} 越出外壳`,
            });
        for (let a = p; a && a !== shellEl; a = a.parentElement) {
            const cs = gcs(a);
            const clipX = /hidden|clip/.test(cs.overflowX);
            const clipY = /hidden|clip/.test(cs.overflowY);
            if (!clipX && !clipY) continue;
            const ar = a.getBoundingClientRect();
            const within =
                (!clipX || (r.left >= ar.left - TOL && r.right <= ar.right + TOL)) &&
                (!clipY || (r.top >= ar.top - TOL && r.bottom <= ar.bottom + TOL));
            if (!within)
                problems.push({
                    kind: "clipped",
                    detail: `${name(p)} 的文字 ${JSON.stringify(text.slice(0, 40))} 被 ${name(a)} 裁掉`,
                });
            if ((clipX && a.scrollWidth > a.clientWidth + 1) || (clipY && a.scrollHeight > a.clientHeight + 1))
                problems.push({
                    kind: "clipped",
                    detail: `${name(a)} 的内容比框大（${a.scrollWidth}x${a.scrollHeight} > ${a.clientWidth}x${a.clientHeight}）`,
                });
        }
    }
    for (let i = 0; i < items.length; i++) {
        for (let j = i + 1; j < items.length; j++) {
            const a = items[i];
            const b = items[j];
            if (a.p.contains(b.p) || b.p.contains(a.p) || a.sc !== b.sc) continue;
            if (layerOf(a.p) !== layerOf(b.p)) continue;
            const w = Math.min(a.box.right, b.box.right) - Math.max(a.box.left, b.box.left);
            const h = Math.min(a.box.bottom, b.box.bottom) - Math.max(a.box.top, b.box.top);
            if (w > 0 && h > 0 && w * h > 4)
                problems.push({
                    kind: "overlap",
                    detail: `${JSON.stringify(a.text.slice(0, 24))} 与 ${JSON.stringify(b.text.slice(0, 24))} 互相压住（${w.toFixed(1)}x${h.toFixed(1)}）`,
                });
        }
    }
    // 消息位是一行槽：文字一律不折行，太长就该越出槽被上面的检查抓到
    for (const e of doc.querySelectorAll('[data-gb="input.band"] .rn-banner__text')) {
        if (!shown(e)) continue;
        const ws = gcs(e).whiteSpace;
        if (ws !== "nowrap")
            problems.push({ kind: "wrapped", detail: `input.band 的文字允许折行（white-space:${ws}）` });
        const range = doc.createRange();
        range.selectNodeContents(e);
        const tops = new Set(Array.from(range.getClientRects()).map((r) => Math.round(r.top / 3)));
        if (tops.size > 1) problems.push({ kind: "wrapped", detail: `input.band 的文字折成了 ${tops.size} 行` });
    }
    let gb = 0;
    for (const e of doc.querySelectorAll("[data-gb]")) {
        if (!shown(e)) continue;
        gb++;
        if (e === shellEl) continue;
        const r = e.getBoundingClientRect();
        if (r.width === 0 && r.height === 0) continue;
        if (!inside(r, shellRect)) problems.push({ kind: "gb-outside", detail: `${name(e)} 越出外壳` });
    }
    const attrs = [];
    for (const e of doc.querySelectorAll("[title],[aria-label],[placeholder],[aria-valuetext]")) {
        if (!shown(e)) continue;
        for (const a of ["title", "aria-label", "placeholder", "aria-valuetext"]) {
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
}

/** 限制器关闭确认框背景的不透明度。适配量尺把确认框当独立一层，前提是它盖得住下面那几行字。 */
export function confirmAlpha(doc, win) {
    const e = doc.querySelector(".in-limconfirm");
    if (!e) return null;
    const m = /rgba?(([^)]+))/.exec(win.getComputedStyle(e).backgroundColor);
    if (!m) return 0;
    const v = m[1]
        .split(/[ ,/]+/)
        .filter(Boolean)
        .map(Number);
    return v.length > 3 ? v[3] : 1;
}

/**
 * 对比度采样：每段可见文字的矩形（iframe 内坐标）、计算色、各层不透明度与分档。分档：
 *   A：危险与确认横幅、模态里的文字、横幅与消息位里的按钮、紧急切断；B：琥珀 / 灰 / 紫横幅、配对状态灯；C：其余只记录。
 * 模态遮罩打开时只取遮罩里的文字（下面的被压暗，不是要判的对象）。
 */
export function contrastProbe(doc, win) {
    const gcs = (e) => win.getComputedStyle(e);
    const shellEl = doc.querySelector('[data-gb="input.shell"]');
    const out = [];
    const shown = (e) => {
        for (let n = e; n && n !== doc.documentElement; n = n.parentElement) {
            if (n.hidden) return false;
            const cs = gcs(n);
            if (cs.display === "none" || cs.visibility === "hidden" || Number(cs.opacity) === 0) return false;
        }
        return true;
    };
    const parse = (c) => {
        const m = /rgba?\(([^)]+)\)/.exec(c);
        if (!m) return null;
        const v = m[1]
            .split(/[ ,/]+/)
            .filter(Boolean)
            .map(Number);
        return { r: v[0], g: v[1], b: v[2], a: v.length > 3 ? v[3] : 1 };
    };
    const tierOf = (p) => {
        if (p.closest('[class*="eyebrow"]')) return "C";
        if (p.closest(".rn-about__preview")) return "B";
        if (p.closest(".rn-modal")) return "A";
        if (p.closest(".in-panic")) return "A";
        const band = p.closest(".rn-banner");
        if (band) {
            if (/danger|red|confirm/.test(band.className)) return "A";
            if (p.closest(".rn-btn")) return "A";
            if (/amber|gray|violet/.test(band.className)) return "B";
            return "C";
        }
        if (p.closest(".rn-msgline") && p.closest(".rn-btn")) return "A";
        if (p.closest(".rn-pair, .sc-pill")) return "B";
        return "C";
    };
    const isScroller = (a) => {
        const oy = gcs(a).overflowY;
        return (oy === "auto" || oy === "scroll") && a.scrollHeight > a.clientHeight + 1;
    };
    const scrollerOf = (e) => {
        for (let a = e.parentElement; a && a !== shellEl; a = a.parentElement) if (isScroller(a)) return a;
        return null;
    };
    for (const e of shellEl.querySelectorAll("*")) if (isScroller(e)) e.scrollTop = 0;
    const scrimOn = [...shellEl.querySelectorAll(".rn-scrim")].some(shown);
    const rectOf = (n) => {
        // 采样之后页面把这段文字换掉了（节点已脱离文档）：给一个远在视野外的矩形，让两遍对照判成「没静止」
        if (!n.isConnected) return { x: -1e6, y: -1e6, w: 0, h: 0 };
        const range = doc.createRange();
        range.selectNodeContents(n);
        const r = range.getBoundingClientRect();
        // 文字直接放在滚动框里（诊断预览）：滚动的那个轴只取露出来的一段
        const p = n.parentElement;
        const cs = gcs(p);
        const pr = p.getBoundingClientRect();
        const sx = /auto|scroll/.test(cs.overflowX) && p.scrollWidth > p.clientWidth + 1;
        const sy = /auto|scroll/.test(cs.overflowY) && p.scrollHeight > p.clientHeight + 1;
        const left = sx ? Math.max(r.left, pr.left) : r.left;
        const right = sx ? Math.min(r.right, pr.right) : r.right;
        const top = sy ? Math.max(r.top, pr.top) : r.top;
        const bottom = sy ? Math.min(r.bottom, pr.bottom) : r.bottom;
        return { x: left, y: top, w: right - left, h: bottom - top };
    };
    win.__cp = [];
    win.__cpPlace = (i) => {
        const it = win.__cp[i];
        if (it.sc) it.p.scrollIntoView({ block: "nearest", inline: "nearest" });
        return rectOf(it.n);
    };
    win.__cpRects = () => win.__cp.map((it) => (it.sc ? null : rectOf(it.n)));
    const walker = doc.createTreeWalker(shellEl, win.NodeFilter.SHOW_TEXT);
    for (let n = walker.nextNode(); n; n = walker.nextNode()) {
        const text = n.nodeValue.replace(/\s+/g, " ").trim();
        const p = n.parentElement;
        if (!text || !p || !shown(p)) continue;
        if (scrimOn && !p.closest(".rn-scrim")) continue;
        const tier = tierOf(p);
        const r = rectOf(n);
        const sc = scrollerOf(p);
        if (!sc && (r.w < 1 || r.h < 1)) continue;
        let op = 1;
        for (let a = p; a && a !== doc.documentElement; a = a.parentElement) op *= Number(gcs(a).opacity);
        const c = parse(gcs(p).color);
        if (!c) continue;
        const cls = (p.className && p.className.baseVal === undefined ? p.className : "") || p.tagName;
        const toneEl = p.closest("[data-tone]");
        win.__cp.push({ n, p, sc });
        out.push({
            idx: win.__cp.length - 1,
            scrolled: Boolean(sc),
            tone: toneEl ? toneEl.getAttribute("data-tone") : "",
            text: text.slice(0, 48),
            tier,
            sel: cls.split(" ").filter(Boolean).slice(0, 3).join("."),
            r,
            c,
            op,
        });
    }
    return out;
}

// ---------------------------------------------------------------- 自检

const isMain = process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href;
if (isMain) {
    const ROOT = repoRoot();
    const run = createRun("Input 页面冒烟公共件");
    run.check(SCENES.length === 30, `范围内的场景应有 30 个，实得 ${SCENES.length}`);
    run.check(
        SCENES.every((s) => !OUT_OF_SCOPE.test(s.id)),
        "预设面板与首启场景不得混进来",
    );
    run.check(
        findLeaks("反馈 band.in.neverDefined").length === 1 &&
            findLeaks("{g} 组").length === 1 &&
            findLeaks("值 undefined").length === 1 &&
            findLeaks("环路已关闭：回送不会加入输出。").length === 0,
        "裸键、占位符、undefined 要扫得出，正常文案不误报",
    );
    if (!findBrowser()) {
        // 纯函数部分有失败照报失败；全过才按「没有浏览器」整套跳过，跳过不是通过
        if (!run.finish()) process.exit(1);
        run.skip("没有 Chromium 系浏览器；纯函数部分已验");
    }
    const server = await startStaticServer(ROOT);
    const browser = await launchBrowser({ width: VIEW.w, height: VIEW.h });
    try {
        const page = await browser.newPage();
        await openInput(page, server, "in-playing", { lang: "zh" });
        const good = await inFrame(page, fitProbe);
        run.check(good.texts > 20, `正常页应量到足够多的文字，实得 ${good.texts}`);
        run.eq(
            good.problems.map((p) => p.kind),
            [],
            "正常页不应有适配问题",
        );

        // 坏样例一：把一个标签撑宽，越出所在件
        const kinds = async (mutate, label) => {
            await openInput(page, server, "in-playing", { lang: "zh" });
            await inFrame(page, mutate);
            const probe = await inFrame(page, fitProbe);
            return new Set(probe.problems.map((p) => p.kind));
        };
        let got = await kinds((doc) => {
            const e = doc.querySelector('[data-gb="input.footer.hint"] span');
            e.textContent = "这是一段故意写得很长的页脚提示，会越出外壳。".repeat(8);
            doc.querySelector('[data-gb="input.footer.hint"]').style.maxWidth = "none";
            doc.querySelector('[data-gb="input.footer.hint"]').style.overflow = "visible";
            e.style.overflow = "visible";
            e.style.textOverflow = "clip";
        });
        run.check(got.has("outside"), `坏样例（文字越框）应量出 outside，实得 ${JSON.stringify([...got])}`);

        // 坏样例二：被 overflow:hidden 的祖先裁掉
        got = await kinds((doc) => {
            const t = doc.querySelector('[data-gb="input.dly"]');
            t.style.overflow = "hidden";
            t.style.height = "60px";
        });
        run.check(got.has("clipped"), `坏样例（被裁）应量出 clipped，实得 ${JSON.stringify([...got])}`);

        // 坏样例三：两段文字压在一起
        got = await kinds((doc) => {
            const a = doc.querySelector('[data-gb="input.footer.version"]');
            const b = doc.querySelector('[data-gb="input.footer.help"]');
            b.style.position = "relative";
            b.style.left = "-60px";
            a.style.position = "relative";
            const note = doc.createElement("span");
            note.textContent = "压住版本号的另一段字";
            note.style.cssText = "position:absolute;left:660px;top:548px;font-size:12px;z-index:5";
            doc.querySelector('[data-gb="input.shell"]').append(note);
        });
        run.check(got.has("overlap"), `坏样例（互压）应量出 overlap，实得 ${JSON.stringify([...got])}`);

        // 坏样例四：消息位允许折行
        got = await kinds((doc) => {
            for (const e of doc.querySelectorAll('[data-gb="input.band"] .rn-banner__text'))
                e.style.whiteSpace = "normal";
        });
        run.check(got.has("wrapped"), `坏样例（消息位允许折行）应量出 wrapped，实得 ${JSON.stringify([...got])}`);

        // 坏样例五：data-gb 件越出外壳
        got = await kinds((doc) => {
            doc.querySelector('[data-gb="input.meters"]').style.transform = "translateX(120px)";
        });
        run.check(got.has("gb-outside"), `坏样例（件越出外壳）应量出 gb-outside，实得 ${JSON.stringify([...got])}`);

        // 确认框要不透明；改成透明时判据必须判红
        await openInput(page, server, "in-limiter-confirm", { lang: "zh" });
        run.check((await inFrame(page, confirmAlpha)) >= 0.95, "限制器关闭确认框应不透明");
        await inFrame(page, (doc) => {
            doc.querySelector(".in-limconfirm").style.background = "transparent";
        });
        run.check((await inFrame(page, confirmAlpha)) < 0.95, "坏样例：确认框改成透明后应判不过");

        // 对比度采样：正常页要采到 A 档与 B 档文字，坏页（把按钮文字改成与底近似的色）要在上层被量出
        await openInput(page, server, "in-conflict", { lang: "zh" });
        const cp = await inFrame(page, contrastProbe);
        run.check(
            cp.some((i) => i.tier === "A") && cp.some((i) => i.tier === "C"),
            `对比度采样应有 A 档与 C 档文字，实得 ${JSON.stringify([...new Set(cp.map((i) => i.tier))])}`,
        );
        run.check(page.errors.length === 0, `页面不应有错误：${page.errors.slice(0, 3).join("；")}`);
    } finally {
        await browser.close();
        await server.close();
    }
    run.finish();
}
