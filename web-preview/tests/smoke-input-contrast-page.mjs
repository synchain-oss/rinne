// SPDX-License-Identifier: GPL-3.0-or-later
//
// Input 页面文字对比度冒烟（无头浏览器）：范围内的 30 个 in-* 场景 × 中 / 英 / 法。
// 量法与共享件的对比度冒烟相同：页内采样每段可见文字的矩形、计算色与各层不透明度，把页面文字改成透明后截图得到合成底，
// 逐像素把文字色合成上去，取矩形内最差的一格。分档：
//   A（≥ 4.5）：危险与确认横幅、模态里的文字、横幅与消息位里的按钮、紧急切断；
//   B（≥ 3.0）：琥珀 / 灰 / 紫横幅、配对状态灯；
//   C：其余只记录不判（mono 小标等按规范不设门禁）。
// 采样两遍、截图前后核对矩形，页面没静止就不量（不拿一张拼凑的画面去量）。
// 坏样例：把一段 A 档文字改成与底近似的颜色，量尺必须量出低于门槛；采样为空必须报错（防空转）。
// 浏览器缺失时整套跳过（退出码 2），不是通过。本机同时只能有一个浏览器冒烟。
//
// 用法：node web-preview/tests/smoke-input-contrast-page.mjs [仓库根]

import { createRun, repoRoot } from "./lib/harness.mjs";
import { findBrowser, launchBrowser } from "./lib/browser.mjs";
import { startStaticServer } from "./lib/static-server.mjs";
import { HIDE_TEXT_STYLE, TIER_MIN, decodePng, itemsDrift, rectsDrift, worstContrast } from "./shared-measure.mjs";
import { LANGS, SCENES, VIEW, contrastProbe, frameRect, inFrame, openInput, settle } from "./smoke-input-kit-page.mjs";

const ROOT = repoRoot();
const run = createRun("Input 页面对比度");
if (!findBrowser()) run.skip("没有 Chromium 系浏览器");

const raf2 = (page) =>
    inFrame(page, (doc, win) => new Promise((r) => win.requestAnimationFrame(() => win.requestAnimationFrame(r))));
const dropHide = (page) =>
    inFrame(page, (doc) => {
        const s = doc.getElementById("fx-hide-text");
        if (s) s.remove();
    });

/** 截 iframe 里某个矩形（iframe 内坐标），返回解码后的 PNG。 */
async function shotRect(page, origin, r) {
    const x = Math.max(0, Math.floor(r.x));
    const y = Math.max(0, Math.floor(r.y));
    const w = Math.max(1, Math.ceil(r.x + r.w) - x);
    const h = Math.max(1, Math.ceil(r.y + r.h) - y);
    const shot = await page.send("Page.captureScreenshot", {
        format: "png",
        clip: { x: origin.x + x, y: origin.y + y, width: w, height: h, scale: 1 },
    });
    return { png: decodePng(Buffer.from(shot.data, "base64")), x, y };
}

/** 页面静止后量出每段文字的最差对比度。 */
async function measure(page) {
    const origin = await frameRect(page);
    let items;
    let whole;
    let last = "";
    for (let attempt = 0; attempt < 5; attempt++) {
        await dropHide(page);
        const first = await inFrame(page, contrastProbe);
        await raf2(page);
        items = await inFrame(page, contrastProbe);
        last = itemsDrift(first, items);
        if (!last) {
            await inFrame(
                page,
                (doc, win, css) => {
                    const s = doc.createElement("style");
                    s.id = "fx-hide-text";
                    s.textContent = css;
                    doc.head.append(s);
                },
                HIDE_TEXT_STYLE,
            );
            await raf2(page);
            whole = (await shotRect(page, origin, { x: 0, y: 0, w: origin.w, h: origin.h })).png;
            last = rectsDrift(items, await inFrame(page, (doc, win) => win.__cpRects()));
            if (!last) break;
            await dropHide(page);
        }
        await settle(page);
    }
    if (last) throw new Error(`页面量不到静止画面：${last}`);
    const out = [];
    for (const it of items) {
        if (!it.scrolled) {
            if (it.r.w < 1 || it.r.h < 1) continue;
            out.push({ ...it, worst: worstContrast(whole, it) });
            continue;
        }
        const r = await inFrame(page, (doc, win, i) => win.__cpPlace(i), it.idx);
        if (!r || r.w < 1 || r.h < 1) continue;
        const { png, x, y } = await shotRect(page, origin, r);
        out.push({ ...it, r, worst: worstContrast(png, { ...it, r: { x: r.x - x, y: r.y - y, w: r.w, h: r.h } }) });
    }
    await dropHide(page);
    return out;
}

const server = await startStaticServer(ROOT);
const browser = await launchBrowser({ width: VIEW.w, height: VIEW.h });
try {
    const page = await browser.newPage();
    let pages = 0;
    const tally = { A: 0, B: 0, C: 0 };
    const dimmedScenes = [];
    for (const lang of LANGS) {
        for (const scene of SCENES) {
            const tag = `${scene.id}/${lang}`;
            await openInput(page, server, scene.id, { lang });
            // 缩放确认的 10 秒倒计时每秒重写一次文本，量尺的取样窗口会和它共振；量的是画面不是倒计时，冻结页内定时器
            if (scene.id === "in-scale-confirm")
                await inFrame(page, (doc, win) => {
                    for (let i = 1; i < 4096; i++) win.clearInterval(i);
                });
            const items = await measure(page);
            pages++;
            // 模态与确认框打开时只量它们自己里面的字，段数天然少
            const overlay = scene.id === "in-scale-confirm" || scene.id === "in-about";
            run.check(
                items.length > (overlay ? 2 : 10),
                `${tag}：应采到足够多的文字段（防空转），实得 ${items.length}`,
            );
            // 宿主旁路时内容整体降到 0.55（共用外壳的规定，等同「不可用」的灰显），不判门槛；只允许 in-bypassed 这一个场景这样
            const dimmed = await inFrame(page, (doc) =>
                doc.querySelector('[data-gb="input.shell"]').hasAttribute("data-bypass"),
            );
            if (dimmed) dimmedScenes.push(scene.id);
            for (const it of items) {
                tally[it.tier]++;
                const min = TIER_MIN[it.tier];
                if (!min || dimmed) continue;
                run.check(
                    it.worst.min >= min,
                    `${tag}：${it.tier} 档文字 ${JSON.stringify(it.text)}（${it.sel}）对比度 ${it.worst.min.toFixed(2)} 低于 ${min}`,
                );
            }
        }
    }
    run.log(`  共量 ${pages} 页；A 档 ${tally.A} 段、B 档 ${tally.B} 段、C 档 ${tally.C} 段（只记录）`);
    run.check(tally.A > 0 && tally.B > 0, "A 档与 B 档都应采到文字（防空转）");
    run.eq([...new Set(dimmedScenes)], ["in-bypassed"], "只有宿主旁路的场景可以按灰显免判，别的场景不得混进来");

    // ---- 坏样例：把一段 A 档文字（紧急切断）的颜色改成与底近似，量尺必须量出低于门槛
    await openInput(page, server, "in-playing", { lang: "zh" });
    await inFrame(page, (doc) => {
        const e = doc.querySelector(".in-panic");
        e.style.setProperty("color", "rgba(255, 190, 190, 0.55)", "important");
        for (const c of e.querySelectorAll("*")) c.style.setProperty("color", "rgba(255, 190, 190, 0.55)", "important");
    });
    {
        const items = await measure(page);
        const panic = items.filter((i) => i.tier === "A" && /切断/.test(i.text));
        run.check(panic.length > 0, "坏样例：应采到紧急切断的文字");
        run.check(
            panic.some((i) => i.worst.min < TIER_MIN.A),
            `坏样例：把紧急切断改成浅红后对比度应低于 ${TIER_MIN.A}，实得 ${JSON.stringify(panic.map((i) => +i.worst.min.toFixed(2)))}`,
        );
    }
    // 坏样例：矩形在截图之外时一个像素都没采到，必须抛错而不是悄悄通过
    {
        let threw = false;
        try {
            worstContrast(
                { w: 10, h: 10, ch: 4, data: Buffer.alloc(400) },
                { c: { r: 0, g: 0, b: 0, a: 1 }, op: 1, r: { x: 50, y: 50, w: 5, h: 5 }, text: "x", sel: "x" },
            );
        } catch {
            threw = true;
        }
        run.check(threw, "坏样例：矩形与截图没有交集时应抛错");
    }

    run.check(page.errors.length === 0, `页面不应有错误：${page.errors.slice(0, 3).join("；")}`);
} finally {
    await browser.close();
    await server.close();
}
run.finish();
