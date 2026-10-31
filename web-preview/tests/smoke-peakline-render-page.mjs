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
//   阶跃距离不小于 20 px（防空转）。每一步最多量 3 次、取最小值再进中位数（原因见 measureGoodStep）。
// 记录窗口锚在变化帧上：页内一看到 --lv 或 --pk 变了，再记固定时长才收尾；一直没变化最多等 2 秒。
//   窗口不锚在变化帧的话，机器忙、测试进程到页面的往返被拖长时，变化落在窗口后段，过渡被窗口截断，终点量成了窗口末帧。
// 坏样例：把液柱的过渡改成 0.18 s（SCVB 定谳前的病态值），滞后必须量到 150 ms 以上并被判红；
//   把峰线的过渡改成 0.18 s，峰线滞后与两者到位时刻的分歧都必须被判红。
//   坏样例只量得偏小、不会量得偏大（原因见 measureBad），所以最多量 3 次、取滞后最大的一次再比 150 ms，达标就提前结束；门槛不变。
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
const BAD_LAG_MIN_MS = 150;
const BAD_TRIES = 3;
const GOOD_TRIES = 3;
/** 变量变化之后再记多久（0.18 s 的病态过渡走完之后还留有平台）。 */
const AFTER_CHANGE_MS = 330;
/** 一直没看到变量变化时，记录最多等多久。 */
const WAIT_CHANGE_MS = 2000;
const FLOOR_START_DB = -56;
const STEPS_DB = [-43, -30, -17, -4];
const sleep = (page, ms) => page.eval(`new Promise((r) => setTimeout(r, ${ms}))`);
const median = (xs) => [...xs].sort((a, b) => a - b)[Math.floor(xs.length / 2)];

/**
 * 页内开始逐帧记录一路电平管：每帧记 [帧时刻, 液柱宽, 峰线左沿, --lv 文本, --pk 文本]。
 * 看到 --lv 或 --pk 与第一帧不同的那一帧起，再记 afterMs 毫秒收尾；一直没变化，记满 maxMs 毫秒收尾。
 */
function startRecording(doc, win, row, afterMs, maxMs) {
    const liquid = doc.querySelector(`[data-row="${row}"] .sc-tube__liquid`);
    const peak = doc.querySelector(`[data-row="${row}"] .sc-tube__peak`);
    const rec = { frames: [], done: false };
    win.__rec = rec;
    let t0 = null;
    let tc = null;
    const loop = (ts) => {
        if (t0 === null) t0 = ts;
        const f = [
            ts,
            liquid.getBoundingClientRect().width,
            peak.getBoundingClientRect().left,
            liquid.style.getPropertyValue("--lv"),
            peak.style.getPropertyValue("--pk"),
        ];
        rec.frames.push(f);
        if (tc === null && (f[3] !== rec.frames[0][3] || f[4] !== rec.frames[0][4])) tc = ts;
        if (tc === null ? ts - t0 < maxMs : ts - tc < afterMs) win.requestAnimationFrame(loop);
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

/**
 * 一次阶跃的结果：{ ok, liquid, peak, dist, gap }；ok 为假时带 why。
 * 滞后不大于 0 的不算有效：第一次看到变量变化时过渡已经走完，说明帧间隔比过渡还长，起点量丢了，
 * 这样的数取最小值会把任何过渡都量成「很快」。
 */
function analyse(rec) {
    const tl = changeTime(rec.frames, 3);
    const tp = changeTime(rec.frames, 4);
    if (tl === null || tp === null) return { ok: false, why: `液柱或峰线的变量没有变化（--lv:${tl}, --pk:${tp}）` };
    const L = reachTime(rec.frames, 1);
    const P = reachTime(rec.frames, 2);
    if (!L || !P) return { ok: false, why: "液柱或峰线没有到达终点" };
    const liquid = L.t - tl;
    const peak = P.t - tp;
    if (liquid <= 0 || peak <= 0)
        return {
            ok: false,
            why: `第一次看到变量变化时过渡已经走完，量不到起点（液柱 ${liquid.toFixed(1)} ms，峰线 ${peak.toFixed(1)} ms）`,
        };
    return { ok: true, liquid, peak, dist: Math.abs(L.dist), gap: Math.abs(L.t - P.t) };
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
    await inFrame(page, startRecording, "in", AFTER_CHANGE_MS, WAIT_CHANGE_MS);
    await sleep(page, 90); // 先记几帧静止
    await pushLevel(page, toDb);
    await page.waitFor(`document.querySelector("iframe").contentWindow.__rec.done === true`, {
        label: `记录应在变化后 ${AFTER_CHANGE_MS} ms（一直没变化则 ${WAIT_CHANGE_MS} ms）收尾`,
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

/**
 * 坏样例的量尺：开页、注入病态过渡、量一次上行阶跃，最多 BAD_TRIES 次，取 key 对应滞后最大的一次；某次超过 BAD_LAG_MIN_MS 就提前结束。
 * 为什么只会偏小：滞后是「看到变量变化的那一帧」到「到位」的时间。主线程被占住、帧间隔被拉长时，过渡在变化帧之前就起跑了，
 * 量出来的比真值小（降速 20 倍时见过 114.4 ms）；记录窗口没锚在变化帧的老写法下，窗口还会把过渡截断（恒量出 65.4 ms）。
 * 这些都不会比真值大，所以取最大值不会放过病态值，门槛 BAD_LAG_MIN_MS 不变。
 * 返回 { best, tries }：best 是滞后最大的一次有效阶跃（一次都没量到为 null），tries 是每次的结果。
 */
async function measureBad(page, server, css, key) {
    const tries = [];
    let best = null;
    for (let i = 0; i < BAD_TRIES; i++) {
        await openInput(page, server, "in-playing", { lang: "zh" });
        await injectStyle(page, css);
        await prepare(page);
        const r = await step(page, STEPS_DB[1]);
        tries.push(r);
        if (r.ok && (best === null || r[key] > best[key])) best = r;
        if (best !== null && best[key] > BAD_LAG_MIN_MS) break;
    }
    return { best, tries };
}
const triesText = (tries, key) => tries.map((r) => (r.ok ? r[key].toFixed(1) : "没量到")).join(" / ");

/**
 * 好样例的一步：量一次上行阶跃，最多 GOOD_TRIES 次，各项取有效结果里的最小值；这一步已经在门槛内就提前结束。
 * 为什么取最小值：单次读数会有一帧（约 16.7 ms）的量化差，而门槛 40 ms 与产品实测的 32.6 ms 之间不到一帧，晚一帧不代表过渡慢。
 * 这不是说噪声只会量大：调度噪声也可能量小（坏样例取最大值就是为这个），帧间隔比过渡还长时甚至量出负数，所以 analyse 把不大于 0 的判为无效。
 * 取最小值后读数可能比典型值小一帧；0.06 s 的过渡仍会判红，由删除式验证核对过，更小的差没有测过。门槛（LAG_MAX_MS、SYNC_MAX_MS）与中位数的算法不变。
 * 重量之前要先回到起点电平并清掉峰值（同一个电平再推一次，变量不会变）。
 * 返回 { step, tries }：step 是各项最小值与最大位移（一次都没量到为 null），tries 是每次的结果。
 */
async function measureGoodStep(page, toDb) {
    const tries = [];
    let acc = null;
    for (let i = 0; i < GOOD_TRIES; i++) {
        if (i > 0) await prepare(page);
        const r = await step(page, toDb);
        tries.push(r);
        if (r.ok) {
            acc =
                acc === null
                    ? { ...r }
                    : {
                          ok: true,
                          liquid: Math.min(acc.liquid, r.liquid),
                          peak: Math.min(acc.peak, r.peak),
                          gap: Math.min(acc.gap, r.gap),
                          dist: Math.max(acc.dist, r.dist),
                      };
        }
        if (acc !== null && acc.liquid <= LAG_MAX_MS && acc.peak <= LAG_MAX_MS && acc.gap <= SYNC_MAX_MS) break;
    }
    return { step: acc, tries };
}

const server = await startStaticServer(ROOT);
const browser = await launchBrowser({ width: VIEW.w, height: VIEW.h });
try {
    const page = await browser.newPage();
    await openInput(page, server, "in-playing", { lang: "zh" });
    await prepare(page);

    const results = [];
    const counts = [];
    for (const db of STEPS_DB) {
        const { step: r, tries } = await measureGoodStep(page, db);
        counts.push(tries.length);
        run.check(r !== null, `阶跃到 ${db} dB：${tries.length} 次都没量到：${tries.map((t) => t.why).join("；")}`);
        if (r === null) continue;
        results.push(r);
        run.check(r.dist >= STEP_MIN_PX, `阶跃到 ${db} dB：液柱位移 ${r.dist.toFixed(1)} px 太短，量不准（防空转）`);
    }
    run.eq(results.length, STEPS_DB.length, "四次阶跃都应量到");
    if (results.length === STEPS_DB.length) {
        const liquid = median(results.map((r) => r.liquid));
        const peak = median(results.map((r) => r.peak));
        const gap = median(results.map((r) => r.gap));
        const fmt = (k) => results.map((r) => r[k].toFixed(1)).join(" / ");
        run.log(
            `  阶跃滞后（ms，每步取至多 ${GOOD_TRIES} 次里的最小值）：液柱 ${fmt("liquid")}；峰线 ${fmt("peak")}；到位相差 ${fmt("gap")}；各步量了 ${counts.join(" / ")} 次`,
        );
        run.check(liquid <= LAG_MAX_MS, `液柱滞后中位数 ${liquid.toFixed(1)} ms 超过 ${LAG_MAX_MS} ms`);
        run.check(peak <= LAG_MAX_MS, `峰线滞后中位数 ${peak.toFixed(1)} ms 超过 ${LAG_MAX_MS} ms`);
        run.check(
            gap <= SYNC_MAX_MS,
            `液柱与峰线到位时刻相差中位数 ${gap.toFixed(1)} ms 超过 ${SYNC_MAX_MS} ms（柱头够不着白线）`,
        );
    }

    // ---- 坏样例：液柱过渡 0.18 s（SCVB 定谳前的病态值）
    {
        const { best: r, tries } = await measureBad(
            page,
            server,
            ".sc-tube__liquid { transition: width 0.18s linear !important; }",
            "liquid",
        );
        run.check(r !== null, `坏样例（液柱过渡 0.18 s）：应量到阶跃：${tries.map((t) => t.why).join("；")}`);
        if (r) {
            run.check(
                r.liquid > BAD_LAG_MIN_MS,
                `坏样例（液柱过渡 0.18 s）：液柱滞后应量到 ${BAD_LAG_MIN_MS} ms 以上，${tries.length} 次实得 ${triesText(tries, "liquid")}`,
            );
            run.check(r.liquid > LAG_MAX_MS, "坏样例（液柱过渡 0.18 s）：应被 40 ms 的门槛判红");
            run.check(r.gap > SYNC_MAX_MS, `坏样例（液柱过渡 0.18 s）：到位时刻应量出分歧，实得 ${r.gap.toFixed(1)}`);
        }
    }
    // ---- 坏样例：峰线过渡 0.18 s
    {
        const { best: r, tries } = await measureBad(
            page,
            server,
            ".sc-tube__peak { transition: left 0.18s linear !important; }",
            "peak",
        );
        run.check(r !== null, `坏样例（峰线过渡 0.18 s）：应量到阶跃：${tries.map((t) => t.why).join("；")}`);
        if (r) {
            run.check(
                r.peak > BAD_LAG_MIN_MS,
                `坏样例（峰线过渡 0.18 s）：峰线滞后应量到 ${BAD_LAG_MIN_MS} ms 以上，${tries.length} 次实得 ${triesText(tries, "peak")}`,
            );
            run.check(r.gap > SYNC_MAX_MS, `坏样例（峰线过渡 0.18 s）：到位时刻应量出分歧，实得 ${r.gap.toFixed(1)}`);
        }
    }

    run.check(page.errors.length === 0, `页面不应有错误：${page.errors.slice(0, 3).join("；")}`);
} finally {
    await browser.close();
    await server.close();
}
run.finish();
