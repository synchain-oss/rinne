// SPDX-License-Identifier: GPL-3.0-or-later
//
// 电平管峰线的渲染层冒烟（无头浏览器）：峰线与液柱在电平阶跃之后必须在一个事件间隔内一起追平。
// 为什么要在渲染层量：模型层（弹道的 db 与 peakDb）怎么看都是对的，CSS 过渡造成的位移滞后只有逐帧读
// getBoundingClientRect 才量得到——液柱的 width 与峰线的 left 各走各的过渡，时长不等、或者其一偏长，
// 柱头就永远够不着白线。方法借自 SCVB 的 peakline-render 量尺（那份是量尺不是冒烟，这里把判据定成门槛）：
//   真页面、真样式、真电平渲染器，页内逐帧（rAF）记录液柱右沿与峰线位置，同时记下液柱 --lv 与峰线 --pk 变化的那一帧；
//   阶跃滞后 = 液柱（峰线）追到各自终点 98% 的时刻（相邻两帧之间线性插值）减去它的变量变化那一帧的时刻。
// 峰线只有在电平越过保持中的峰值时才会动，所以每次阶跃前先点读数清掉峰值，让峰线跟着液柱走。
// 判据：四次上行阶跃的滞后中位数，液柱与峰线都不超过 40 ms，且两者到位时刻相差中位数不超过 8 ms；
//   阶跃距离不小于 20 px（防空转）。
// 坏样例：把液柱的过渡改成 0.18 s（SCVB 定谳前的病态值），滞后必须量到 150 ms 以上并被判红；
//   把峰线的过渡改成 0.18 s，峰线滞后与两者到位时刻的分歧都必须被判红。
// 浏览器缺失时整套跳过（退出码 2），不是通过。本机同时只能有一个浏览器冒烟。
//
// 用法：node web-preview/tests/smoke-peakline-render-page.mjs [仓库根]

import { createRun, repoRoot } from "./lib/harness.mjs";
import { findBrowser, launchBrowser } from "./lib/browser.mjs";
import { startStaticServer } from "./lib/static-server.mjs";
import { VIEW, inFrame, openInput } from "./smoke-input-kit-page.mjs";

const ROOT = repoRoot();
const run = createRun("电平峰线渲染");
if (!findBrowser()) run.skip("没有 Chromium 系浏览器");

const LAG_MAX_MS = 40;
const SYNC_MAX_MS = 8;
const STEP_MIN_PX = 20;
const FLOOR_START_DB = -56;
const STEPS_DB = [-43, -30, -17, -4];
const sleep = (page, ms) => page.eval(`new Promise((r) => setTimeout(r, ${ms}))`);
const median = (xs) => [...xs].sort((a, b) => a - b)[Math.floor(xs.length / 2)];

/** 页内开始逐帧记录一路电平管：每帧记 [帧时刻, 液柱宽, 峰线左沿, --lv 文本, --pk 文本]。 */
function startRecording(doc, win, row, ms) {
    const liquid = doc.querySelector(`[data-row="${row}"] .sc-tube__liquid`);
    const peak = doc.querySelector(`[data-row="${row}"] .sc-tube__peak`);
    const rec = { frames: [], done: false };
    win.__rec = rec;
    let t0 = null;
    const loop = (ts) => {
        if (t0 === null) t0 = ts;
        rec.frames.push([
            ts,
            liquid.getBoundingClientRect().width,
            peak.getBoundingClientRect().left,
            liquid.style.getPropertyValue("--lv"),
            peak.style.getPropertyValue("--pk"),
        ]);
        if (ts - t0 < ms) win.requestAnimationFrame(loop);
        else rec.done = true;
    };
    win.requestAnimationFrame(loop);
}

/** 某个变量文本第一次变化的那一帧的时刻。 */
function changeTime(frames, col) {
    for (let i = 1; i < frames.length; i++) if (frames[i][col] !== frames[0][col]) return frames[i][0];
    return null;
}

/** 到达 98% 的时刻：第一个落在终点 2% 以内的帧，与前一帧之间线性插值。 */
function reachTime(frames, col) {
    const first = frames[0][col];
    const last = frames[frames.length - 1][col];
    const dist = last - first;
    const thr = first + 0.98 * dist;
    for (let i = 1; i < frames.length; i++) {
        const a = frames[i - 1][col];
        const b = frames[i][col];
        const crossed = dist > 0 ? b >= thr : b <= thr;
        if (!crossed) continue;
        const f = b === a ? 1 : (thr - a) / (b - a);
        return { t: frames[i - 1][0] + f * (frames[i][0] - frames[i - 1][0]), dist };
    }
    return null;
}

/** 一次阶跃的结果：{ ok, liquid, peak, dist, gap }；ok 为假时带 why。 */
function analyse(rec) {
    const tl = changeTime(rec.frames, 3);
    const tp = changeTime(rec.frames, 4);
    if (tl === null || tp === null) return { ok: false, why: `液柱或峰线的变量没有变化（--lv:${tl}, --pk:${tp}）` };
    const L = reachTime(rec.frames, 1);
    const P = reachTime(rec.frames, 2);
    if (!L || !P) return { ok: false, why: "液柱或峰线没有到达终点" };
    return { ok: true, liquid: L.t - tl, peak: P.t - tp, dist: Math.abs(L.dist), gap: Math.abs(L.t - P.t) };
}

/** 设 in 路电平并让页面收到一帧 rinne.meters。 */
const pushLevel = (page, db) =>
    page.eval(
        `(() => { const s = window.__RINNE_PREVIEW__; s.model.meters.in = ${db}; s.core.push("rinne.meters"); })()`,
    );

/** 落到起点电平并清掉保持中的峰值，等稳。 */
async function prepare(page) {
    await pushLevel(page, FLOOR_START_DB);
    await sleep(page, 400);
    await inFrame(page, (doc) => doc.querySelector('[data-row="in"] .in-meter__readout').click());
    await sleep(page, 500);
}

/** 做一次上行阶跃并记录。 */
async function step(page, toDb) {
    await inFrame(page, startRecording, "in", 420);
    await sleep(page, 90); // 先记几帧静止
    await pushLevel(page, toDb);
    await page.waitFor(`document.querySelector("iframe").contentWindow.__rec.done === true`, {
        label: "记录应在 420 ms 后收尾",
    });
    return analyse(await inFrame(page, (doc, win) => win.__rec));
}

/** 注入一段样式（坏样例用）。 */
const injectStyle = (page, css) =>
    inFrame(
        page,
        (doc, win, text) => {
            const el = doc.createElement("style");
            el.textContent = text;
            doc.head.append(el);
        },
        css,
    );

const server = await startStaticServer(ROOT);
const browser = await launchBrowser({ width: VIEW.w, height: VIEW.h });
try {
    const page = await browser.newPage();
    await openInput(page, server, "in-playing", { lang: "zh" });
    await prepare(page);

    const results = [];
    for (const db of STEPS_DB) {
        const r = await step(page, db);
        run.check(r.ok, `阶跃到 ${db} dB：${r.why || ""}`);
        if (!r.ok) continue;
        results.push(r);
        run.check(r.dist >= STEP_MIN_PX, `阶跃到 ${db} dB：液柱位移 ${r.dist.toFixed(1)} px 太短，量不准（防空转）`);
    }
    run.eq(results.length, STEPS_DB.length, "四次阶跃都应量到");
    if (results.length === STEPS_DB.length) {
        const liquid = median(results.map((r) => r.liquid));
        const peak = median(results.map((r) => r.peak));
        const gap = median(results.map((r) => r.gap));
        const fmt = (k) => results.map((r) => r[k].toFixed(1)).join(" / ");
        run.log(`  阶跃滞后（ms）：液柱 ${fmt("liquid")}；峰线 ${fmt("peak")}；到位相差 ${fmt("gap")}`);
        run.check(liquid <= LAG_MAX_MS, `液柱滞后中位数 ${liquid.toFixed(1)} ms 超过 ${LAG_MAX_MS} ms`);
        run.check(peak <= LAG_MAX_MS, `峰线滞后中位数 ${peak.toFixed(1)} ms 超过 ${LAG_MAX_MS} ms`);
        run.check(
            gap <= SYNC_MAX_MS,
            `液柱与峰线到位时刻相差中位数 ${gap.toFixed(1)} ms 超过 ${SYNC_MAX_MS} ms（柱头够不着白线）`,
        );
    }

    // ---- 坏样例：液柱过渡 0.18 s（SCVB 定谳前的病态值）
    await openInput(page, server, "in-playing", { lang: "zh" });
    await injectStyle(page, ".sc-tube__liquid { transition: width 0.18s linear !important; }");
    await prepare(page);
    {
        const r = await step(page, STEPS_DB[1]);
        run.check(r.ok, `坏样例（液柱过渡 0.18 s）：应量到阶跃：${r.why || ""}`);
        if (r.ok) {
            run.check(
                r.liquid > 150,
                `坏样例（液柱过渡 0.18 s）：液柱滞后应量到 150 ms 以上，实得 ${r.liquid.toFixed(1)}`,
            );
            run.check(r.liquid > LAG_MAX_MS, "坏样例（液柱过渡 0.18 s）：应被 40 ms 的门槛判红");
            run.check(r.gap > SYNC_MAX_MS, `坏样例（液柱过渡 0.18 s）：到位时刻应量出分歧，实得 ${r.gap.toFixed(1)}`);
        }
    }
    // ---- 坏样例：峰线过渡 0.18 s
    await openInput(page, server, "in-playing", { lang: "zh" });
    await injectStyle(page, ".sc-tube__peak { transition: left 0.18s linear !important; }");
    await prepare(page);
    {
        const r = await step(page, STEPS_DB[1]);
        run.check(r.ok, `坏样例（峰线过渡 0.18 s）：应量到阶跃：${r.why || ""}`);
        if (r.ok) {
            run.check(r.peak > 150, `坏样例（峰线过渡 0.18 s）：峰线滞后应量到 150 ms 以上，实得 ${r.peak.toFixed(1)}`);
            run.check(r.gap > SYNC_MAX_MS, `坏样例（峰线过渡 0.18 s）：到位时刻应量出分歧，实得 ${r.gap.toFixed(1)}`);
        }
    }

    run.check(page.errors.length === 0, `页面不应有错误：${page.errors.slice(0, 3).join("；")}`);
} finally {
    await browser.close();
    await server.close();
}
run.finish();
