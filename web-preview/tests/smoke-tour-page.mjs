// SPDX-License-Identifier: GPL-3.0-or-later
//
// 导览的页面级冒烟（无头浏览器）：Input 6 步、Output 4 步，三语逐步量说明框。
//   ① 首启的导览（?fresh=1，默认态）与重看的导览（预置全局位、真实场景下「?」→ 重看 → [继续导览]），每一步：
//      有锚点的步亮区恰好是锚点外扩步骤表的 pad（容差 1.5 px）；说明框整框在外壳内边框以内、不压亮区、不压紧急切断，摆放结果不是 unfit；
//      说明框内的标题、正文、页脚按钮都完整显示（不溢出、不被裁）；紧急切断按钮内五个点的 elementFromPoint 都命中它自己；
//   ② 步数：Input 6 步、Output 4 步，末步按钮是 [完成]；重看的导览结束不写「引导已看」；
//   ③ 预览的静态场景：in-tour-N / out-tour-N 摆出第 N 步且摆放合格；in-lang、in-rules、out-lang、out-rules 摆出对应的叠层；
//   ④ Output 第 3 步（配对卡）的亮区只罩状态行：罩住状态胶囊，不罩两个节点与副行，高度只比状态行大外扩的两倍；
//   坏样例：把说明框挪到亮区上、挪出设计盒、把说明框正文改成单行截断、把第 3 步亮区撑成整张配对卡，量尺必须判红。
// 浏览器缺失时整套跳过（退出码 2），不是通过。本机同时只能有一个浏览器冒烟。
//
// 用法：node web-preview/tests/smoke-tour-page.mjs [仓库根]

import path from "node:path";
import { pathToFileURL } from "node:url";
import { createRun, repoRoot } from "./lib/harness.mjs";
import { findBrowser, launchBrowser } from "./lib/browser.mjs";
import { startStaticServer } from "./lib/static-server.mjs";
import {
    PANIC,
    VIEW,
    WRITES,
    clickPart,
    hitProbe,
    inFrame,
    openRole,
    recordBridge,
    scrollRulesToEnd,
    stateOf,
    waitOverlay,
} from "./smoke-first-run-page.mjs";

const ROOT = repoRoot();
const run = createRun("导览页面");
const load = (rel) => import(pathToFileURL(path.join(ROOT, rel)).href);
const { TOUR_IN_STEPS } = await load("web/input/tour-in.js");
const { TOUR_OUT_STEPS } = await load("web/output/tour-out.js");
const { SPOT_PAD } = await load("web/shared/tour-paint.js");
if (!findBrowser()) run.skip("没有 Chromium 系浏览器");

const STEPS = { input: 6, output: 4 };
const LANGS = ["zh", "en", "fr"];

/**
 * 量当前这一步：返回 { step, place, spot, callout, problems }。坐标都是设计 px，原点是外壳的内边框。
 */
function tourProbe(doc, win, role) {
    const TOL = 1;
    const shell = doc.querySelector(`[data-gb="${role}.shell"]`);
    const sr = shell.getBoundingClientRect();
    const k = shell.offsetWidth > 0 ? sr.width / shell.offsetWidth : 1;
    const toDesign = (e) => {
        const r = e.getBoundingClientRect();
        return {
            x: (r.left - sr.left) / k - shell.clientLeft,
            y: (r.top - sr.top) / k - shell.clientTop,
            w: r.width / k,
            h: r.height / k,
        };
    };
    const overlap = (a, b) =>
        a.x < b.x + b.w - TOL && b.x < a.x + a.w - TOL && a.y < b.y + b.h - TOL && b.y < a.y + a.h - TOL;
    const box = { w: shell.clientWidth, h: shell.clientHeight };
    const root = doc.querySelector(`[data-gb="${role}.tour"]`);
    const callout = root.querySelector(".rn-tour__callout");
    const mask = root.querySelector(".rn-tour__mask");
    const step = Number((root.getAttribute("data-state") || "").replace("step-", ""));
    const raw = mask.getAttribute("data-spot");
    const spot = raw && raw !== "none" ? (([x, y, w, h]) => ({ x, y, w, h }))(raw.split(",").map(Number)) : null;
    const c = toDesign(callout);
    const problems = [];
    if (c.x < -TOL || c.y < -TOL || c.x + c.w > box.w + TOL || c.y + c.h > box.h + TOL)
        problems.push(`说明框越出设计盒：${JSON.stringify(c)}，盒 ${box.w}×${box.h}`);
    if (spot && overlap(c, spot)) problems.push(`说明框压到亮区：${JSON.stringify(c)} / ${raw}`);
    const panic = doc.querySelector('[data-gb="input.panic"]');
    if (panic && overlap(c, toDesign(panic))) problems.push("说明框压到紧急切断");
    if (callout.getAttribute("data-place") === "unfit") problems.push("摆放结果是 unfit");
    // 说明框里的文字完整
    const cr = callout.getBoundingClientRect();
    if (callout.scrollHeight > callout.clientHeight + 1 || callout.scrollWidth > callout.clientWidth + 1)
        problems.push("说明框内容溢出");
    for (const sel of [".rn-tour__title", ".rn-tour__body", ".rn-tour__hint", ".rn-tour__btn"]) {
        for (const e of callout.querySelectorAll(sel)) {
            const r = e.getBoundingClientRect();
            if (
                r.left < cr.left - TOL ||
                r.right > cr.right + TOL ||
                r.top < cr.top - TOL ||
                r.bottom > cr.bottom + TOL
            )
                problems.push(`${sel} 越出说明框`);
            if (e.scrollWidth > e.clientWidth + 1 || e.scrollHeight > e.clientHeight + 1) problems.push(`${sel} 被裁`);
            const cs = win.getComputedStyle(e);
            if (cs.textOverflow === "ellipsis" && e.scrollWidth > e.clientWidth) problems.push(`${sel} 带省略号`);
        }
    }
    // 标题不压步号
    const title = callout.querySelector(".rn-tour__title").getBoundingClientRect();
    const ind = callout.querySelector(".rn-tour__ind").getBoundingClientRect();
    if (title.right > ind.left + TOL) problems.push("标题压到步号");
    return { step, place: callout.getAttribute("data-place"), spot, callout: c, problems };
}

/** 亮区是否恰好是锚点外扩 pad（有锚点的步，容差 1.5 px）。返回问题或 null。 */
function anchorCovered(doc, win, role, anchor, pad) {
    const shell = doc.querySelector(`[data-gb="${role}.shell"]`);
    const sr = shell.getBoundingClientRect();
    const k = shell.offsetWidth > 0 ? sr.width / shell.offsetWidth : 1;
    const t = shell.querySelector(`[data-tour="${anchor}"]`);
    if (!t) return `找不到锚点 ${anchor}`;
    const r = t.getBoundingClientRect();
    const a = {
        x: (r.left - sr.left) / k - shell.clientLeft,
        y: (r.top - sr.top) / k - shell.clientTop,
        w: r.width / k,
        h: r.height / k,
    };
    const raw = doc.querySelector(`[data-gb="${role}.tour"] .rn-tour__mask`).getAttribute("data-spot");
    if (!raw || raw === "none") return "没有亮区";
    const [x, y, w, h] = raw.split(",").map(Number);
    const near = (u, v) => Math.abs(u - v) <= 1.5;
    const ok = near(x, a.x - pad) && near(y, a.y - pad) && near(w, a.w + pad * 2) && near(h, a.h + pad * 2);
    return ok ? null : `亮区 ${raw} 不是锚点 ${JSON.stringify(a)} 外扩 ${pad}`;
}

/**
 * Output 第 3 步：亮区只罩配对状态行（锚点就是状态行），不罩两个节点与副行。返回问题或 null。
 * 坐标是设计 px，原点是外壳内边框。
 */
function statusOnlySpot(doc, win, role, pad) {
    const shell = doc.querySelector(`[data-gb="${role}.shell"]`);
    const sr = shell.getBoundingClientRect();
    const k = shell.offsetWidth > 0 ? sr.width / shell.offsetWidth : 1;
    const toDesign = (e) => {
        const r = e.getBoundingClientRect();
        return {
            x: (r.left - sr.left) / k - shell.clientLeft,
            y: (r.top - sr.top) / k - shell.clientTop,
            w: r.width / k,
            h: r.height / k,
        };
    };
    const raw = doc.querySelector(`[data-gb="${role}.tour"] .rn-tour__mask`).getAttribute("data-spot");
    if (!raw || raw === "none") return "没有亮区";
    const [x, y, w, h] = raw.split(",").map(Number);
    const top = shell.querySelector('[data-tour="pair"]');
    const pill = top && top.querySelector(".rn-pair");
    if (!pill) return "找不到配对状态胶囊";
    const t = toDesign(top);
    const p = toDesign(pill);
    const TOL = 1.5;
    if (h > t.h + pad * 2 + TOL) return `亮区高 ${h} 超过状态行高 ${t.h} 外扩 ${pad}：罩得太多`;
    if (!(x <= p.x + TOL && x + w >= p.x + p.w - TOL && y <= p.y + TOL && y + h >= p.y + p.h - TOL)) {
        return `亮区 ${raw} 没罩住状态胶囊 ${JSON.stringify(p)}`;
    }
    const others = [
        ...shell.querySelectorAll(`[data-gb="${role}.pair"] .rn-track`),
        ...shell.querySelectorAll(`[data-gb="${role}.pair"] .rn-paircard__sub`),
    ];
    if (others.length < 3) return `应有两个节点与副行，实得 ${others.length} 个`;
    for (const n of others) {
        const r = toDesign(n);
        if (x < r.x + r.w && r.x < x + w && y < r.y + r.h && r.y < y + h) {
            return `亮区 ${raw} 罩到了 ${n.className} ${JSON.stringify(r)}`;
        }
    }
    return null;
}

/** 各步的锚点与外扩：直接读页面用的步骤表。 */
const TABLES = { input: TOUR_IN_STEPS, output: TOUR_OUT_STEPS };

/** 从当前在第 1 步的导览一步步走到末步，逐步量。where 是报错用的前缀。 */
async function walk(page, role, where) {
    const n = STEPS[role];
    for (let i = 1; i <= n; i++) {
        const p = await inFrame(page, tourProbe, role);
        run.eq(p.step, i, `${where}：应在第 ${i} 步`);
        run.check(p.problems.length === 0, `${where} 第 ${i} 步：${p.problems.join("；")}`);
        const st = TABLES[role][i - 1];
        if (st.anchor) {
            const pad = Number.isFinite(st.pad) ? st.pad : SPOT_PAD;
            const miss = await inFrame(page, anchorCovered, role, st.anchor, pad);
            run.check(miss === null, `${where} 第 ${i} 步：${miss}`);
        } else run.check(p.spot === null, `${where} 第 ${i} 步：没有锚点时不该有亮区`);
        if (role === "output" && i === 3) {
            const bad = await inFrame(page, statusOnlySpot, role, st.pad);
            run.check(bad === null, `${where} 第 3 步：亮区应只罩状态行：${bad}`);
        }
        if (role === "input") {
            const h = await inFrame(page, hitProbe, PANIC);
            run.check(h.hits === h.points, `${where} 第 ${i} 步：紧急切断被盖住 ${h.points - h.hits} 个点（${h.who}）`);
        }
        const last = await inFrame(
            page,
            (doc, win, r) => !!doc.querySelector(`[data-gb="${r}.tour"] [data-part="tour.done"]`),
            role,
        );
        run.eq(last, i === n, `${where} 第 ${i} 步：末步才是 [完成]`);
        await clickPart(page, `[data-gb="${role}.tour"] [data-part="${i === n ? "tour.done" : "tour.next"}"]`);
    }
    await waitOverlay(page, role, "(s) => s.tour === null", `${where}：末步后导览结束`);
}

const server = await startStaticServer(ROOT);
const browser = await launchBrowser({ width: VIEW.w, height: VIEW.h });
try {
    const page = await browser.newPage();

    // ============================================================ ① 首启的导览（默认态）
    for (const role of ["input", "output"]) {
        for (const lang of LANGS) {
            const where = `${role}/${lang}/首启`;
            await openRole(page, server, role);
            await waitOverlay(page, role, "(s) => s.lang", `${where}：应出语言卡`);
            await clickPart(page, `[data-lang-pick="${lang}"]`);
            await waitOverlay(page, role, '(s) => s.rules === "gated"', `${where}：应进规则页`);
            await scrollRulesToEnd(page, role);
            await waitOverlay(page, role, '(s) => s.rules === "ready"', `${where}：应放行`);
            await clickPart(page, `[data-gb="${role}.rules"] [data-part="rules.start"]`);
            await waitOverlay(page, role, "(s) => s.tour === 1", `${where}：应进导览`);
            await walk(page, role, where);
        }
    }

    // ============================================================ ① ② 重看的导览（真实场景）
    const REPLAY = {
        input: [["in-playing", LANGS]],
        output: [
            ["out-connected", LANGS],
            ["out-cross-track", LANGS],
            ["out-fr-worst", ["fr"]],
        ],
    };
    for (const role of ["input", "output"]) {
        for (const [scenario, langs] of REPLAY[role]) {
            for (const lang of langs) {
                const where = `${role}/${lang}/${scenario}/重看`;
                await openRole(page, server, role, { scenario, lang });
                await page.eval(
                    `localStorage.setItem("rinne.uiDefaults", JSON.stringify({ langChosen: true, lang: ${JSON.stringify(lang)}, rulesAckVersion: 1, guideSeenInput: true, guideSeenOutput: true }))`,
                );
                await openRole(page, server, role, { fresh: false, scenario, lang });
                const rec = await recordBridge(page, WRITES);
                await clickPart(page, `[data-gb="${role}.footer.help"]`);
                await waitOverlay(page, role, '(s) => s.rules === "review"', `${where}：「?」应打开重看的规则页`);
                await clickPart(page, `[data-gb="${role}.rules"] [data-part="rules.continue"]`);
                await waitOverlay(page, role, "(s) => s.tour === 1", `${where}：[继续导览] 应进导览`);
                await walk(page, role, where);
                run.eq(await rec.calls(), [], `${where}：重看不写任何位`);
            }
        }
    }

    // ============================================================ ③ 静态场景
    for (const [role, prefix] of [
        ["input", "in"],
        ["output", "out"],
    ]) {
        for (let i = 1; i <= STEPS[role]; i++) {
            await openRole(page, server, role, { scenario: `${prefix}-tour-${i}` });
            const p = await inFrame(page, tourProbe, role);
            run.eq(p.step, i, `${prefix}-tour-${i}：应摆出第 ${i} 步`);
            run.check(p.problems.length === 0, `${prefix}-tour-${i}：${p.problems.join("；")}`);
            if (role === "output" && i === 3) {
                const bad = await inFrame(page, statusOnlySpot, role, TOUR_OUT_STEPS[2].pad);
                run.check(bad === null, `${prefix}-tour-3：亮区应只罩状态行：${bad}`);
            }
        }
        await openRole(page, server, role, { scenario: `${prefix}-lang` });
        run.check((await stateOf(page, role)).lang, `${prefix}-lang：应摆出语言卡`);
        await openRole(page, server, role, { scenario: `${prefix}-rules` });
        run.eq((await stateOf(page, role)).rules, "gated", `${prefix}-rules：应摆出未放行的规则页`);
    }

    // ============================================================ 坏样例：量尺本身可信
    await openRole(page, server, "input", { scenario: "in-tour-3" });
    {
        const spot = await inFrame(page, tourProbe, "input");
        await inFrame(
            page,
            (doc, win, s) => {
                const c = doc.querySelector(".rn-tour__callout");
                c.style.left = `${s.x}px`;
                c.style.top = `${s.y}px`;
            },
            spot.spot,
        );
        const p = await inFrame(page, tourProbe, "input");
        run.check(
            p.problems.some((x) => /压到亮区/.test(x)),
            "坏样例：说明框挪到亮区上应判红",
        );
        await inFrame(page, (doc) => {
            doc.querySelector(".rn-tour__callout").style.top = "500px";
        });
        const q = await inFrame(page, tourProbe, "input");
        run.check(
            q.problems.some((x) => /越出设计盒/.test(x)),
            "坏样例：说明框挪出设计盒应判红",
        );
        await inFrame(page, (doc) => {
            const b = doc.querySelector(".rn-tour__body");
            Object.assign(b.style, { whiteSpace: "nowrap", overflow: "hidden", textOverflow: "ellipsis" });
        });
        const r = await inFrame(page, tourProbe, "input");
        run.check(
            r.problems.some((x) => /rn-tour__body/.test(x)),
            "坏样例：正文改成单行截断应判红",
        );
    }
    await openRole(page, server, "input", { scenario: "in-tour-5" });
    {
        await inFrame(page, (doc) => {
            const c = doc.querySelector(".rn-tour__callout");
            c.style.left = "500px";
            c.style.top = "100px";
        });
        const p = await inFrame(page, tourProbe, "input");
        run.check(
            p.problems.some((x) => /紧急切断/.test(x)),
            "坏样例：说明框挪到紧急切断上应判红",
        );
    }

    // 坏样例：把第 3 步的亮区撑成整张配对卡（旧版的写法），只罩状态行的量尺必须判红
    await openRole(page, server, "output", { scenario: "out-tour-3" });
    {
        await inFrame(
            page,
            (doc, win, role) => {
                const shell = doc.querySelector(`[data-gb="${role}.shell"]`);
                const sr = shell.getBoundingClientRect();
                const k = shell.offsetWidth > 0 ? sr.width / shell.offsetWidth : 1;
                const r = doc.querySelector(`[data-gb="${role}.pair"]`).getBoundingClientRect();
                const v = [
                    (r.left - sr.left) / k - shell.clientLeft - 3,
                    (r.top - sr.top) / k - shell.clientTop - 3,
                    r.width / k + 6,
                    r.height / k + 6,
                ];
                doc.querySelector(`[data-gb="${role}.tour"] .rn-tour__mask`).setAttribute(
                    "data-spot",
                    v.map((n) => n.toFixed(1)).join(","),
                );
            },
            "output",
        );
        const bad = await inFrame(page, statusOnlySpot, "output", TOUR_OUT_STEPS[2].pad);
        run.check(
            typeof bad === "string" && /罩得太多/.test(bad),
            `坏样例：第 3 步亮区撑成整张配对卡应判红，实得 ${bad}`,
        );
    }

    run.check(page.errors.length === 0, `页面不应有错误：${page.errors.slice(0, 3).join("；")}`);
} finally {
    await browser.close();
    await server.close();
}
run.finish();
