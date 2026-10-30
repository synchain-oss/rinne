// SPDX-License-Identifier: GPL-3.0-or-later
//
// 共享件的文字对比度冒烟（无头浏览器）：按实际合成底取最差点，不拿 token 的名义值估算。
// 做法：页内列出每段可见文字的矩形与计算色 → 把页内所有文字改成透明再截图 → 在每个文字矩形内逐像素取合成底，
// 把文字色（含 alpha 与各层 opacity）合成到该像素上算 WCAG 对比度，取最差点。
// 分档：A 档 ≥ 4.5:1（危险与红色横幅、确认条、全部模态的正文与按钮、主次行动按钮），
//       B 档 ≥ 3.0:1（琥珀 / 灰 / 紫横幅、配对状态灯文字），其余只记录不判（mono 小标按规范不设门禁）。
// 每个场景每档的最差值都打印出来，贴进 PR 用。无头浏览器的光栅路径与真机有极小差异，设计验收以真机为准。
// 浏览器缺失时整套跳过（退出码 2）。
//
// 用法：node web-preview/tests/smoke-contrast-shared-page.mjs [仓库根] [--table]

import { createRun, repoRoot } from "./lib/harness.mjs";
import { findBrowser, launchBrowser } from "./lib/browser.mjs";
import { startStaticServer } from "./lib/static-server.mjs";
import {
    ROLES,
    LANGS,
    SCENES,
    loadDesign,
    openScene,
    measureContrastOnPage,
    measureAll,
    itemsDrift,
    rectsDrift,
    TIER_MIN,
    KNOWN_SHORT,
} from "./shared-measure.mjs";

const ROOT = repoRoot();
const run = createRun("共享件文字对比度");
if (!findBrowser()) run.skip("没有 Chromium 系浏览器");
const showTable = process.argv.includes("--table");

const design = await loadDesign(ROOT);
const server = await startStaticServer(ROOT);
const browser = await launchBrowser();

/** 打开场景，量一遍，返回 [{...item, worst}]。extra 追加到 query 里（坏样例用）。 */
async function measureScene(page, role, lang, scene, extra = "") {
    await openScene(page, server, design, role, lang, scene, extra);
    return measureContrastOnPage(page);
}

try {
    const page = await browser.newPage();
    /** 每档的全局最差：tier -> { v, tag, text } */
    const worstOf = { A: null, B: null, C: null };
    const rows = [];
    const counts = { A: 0, B: 0, C: 0 };
    const knownSeen = new Set();

    for (const role of ROLES) {
        for (const lang of LANGS) {
            for (const scene of SCENES) {
                const tag = `${role}/${lang}/${scene.id}`;
                const items = await measureScene(page, role, lang, scene);
                for (const it of items) {
                    counts[it.tier]++;
                    const v = it.worst.min;
                    const rec = { tag, tier: it.tier, v, text: it.text, sel: it.sel, bg: it.worst.bg, fg: it.worst.fg };
                    if (!worstOf[it.tier] || v < worstOf[it.tier].v) worstOf[it.tier] = rec;
                    if (it.tier !== "C" && v < TIER_MIN[it.tier]) {
                        const known = it.tier === "B" ? KNOWN_SHORT[`${it.sel.split(".")[0]}|${it.tone}`] : null;
                        if (known) {
                            knownSeen.add(`${it.sel.split(".")[0]}|${it.tone}`);
                            run.check(
                                v >= known.floor,
                                `${tag}：已知不达标项 ${known.note} 又变差了：${v.toFixed(2)}:1 < 下限 ${known.floor}:1，` +
                                    `底 rgb(${it.worst.bg}) 字 rgb(${it.worst.fg}) 文字色 ${JSON.stringify(it.c)} 各层不透明度 ${it.op} 矩形 ${JSON.stringify(it.r)}`,
                            );
                        } else {
                            run.check(
                                false,
                                `${tag}：${it.tier} 档文字 ${JSON.stringify(it.text)}（${it.sel}）对比度 ${v.toFixed(2)}:1 < ${TIER_MIN[it.tier]}:1，底 rgb(${it.worst.bg}) 字 rgb(${it.worst.fg})`,
                            );
                        }
                    }
                    rows.push(rec);
                }
            }
        }
    }
    // 已知不达标项必须真的出现过：它们消失（被修好）时要把这张表同步删掉，不留死条目
    for (const k of Object.keys(KNOWN_SHORT))
        run.check(knownSeen.has(k), `已知不达标项 ${k} 没有再出现：若已修好，请从 KNOWN_SHORT 里删掉`);
    for (const tier of ["A", "B"])
        run.check(counts[tier] > 20, `${tier} 档应量到足够多的文字（防空转），实得 ${counts[tier]}`);

    // 汇总：每档的最差一条，以及每个场景每档的最差值（贴 PR 用）
    for (const tier of ["A", "B", "C"]) {
        const w = worstOf[tier];
        if (w)
            run.log(
                `  ${tier} 档（${counts[tier]} 段）最差 ${w.v.toFixed(2)}:1：${w.tag} ${JSON.stringify(w.text)}（${w.sel}）`,
            );
    }
    if (showTable) {
        const byKey = new Map();
        for (const r of rows) {
            const k = `${r.tag.split("/")[2]}|${r.tier}`;
            if (!byKey.has(k) || r.v < byKey.get(k).v) byKey.set(k, r);
        }
        for (const [k, r] of [...byKey.entries()].sort())
            run.log(`  ${k.replace("|", " 档 ")}：${r.v.toFixed(2)}:1（${r.tag} ${JSON.stringify(r.text)}）`);
    }

    // ---- 坏样例：量尺必须抓得出 ----
    {
        // 琥珀与灰条的字色换成几乎与底一样的色：B 档应当红
        const scene = SCENES.find((s) => s.id === "bands");
        const items = await measureScene(page, "input", "zh", scene, "&defect=lowcontrast");
        const bad = items.filter((it) => it.tier === "B" && it.worst.min < TIER_MIN.B);
        run.check(bad.length > 0, "坏样例 lowcontrast：琥珀 / 灰条文字应被量出 B 档不达标");
    }
    {
        // 量尺自检：在已知的黑底白字上量出 21:1，在同色上量出 1:1
        const png = { w: 4, h: 4, ch: 3, data: Buffer.alloc(4 * 4 * 3, 0) };
        const white = { c: { r: 255, g: 255, b: 255, a: 1 }, op: 1, r: { x: 0, y: 0, w: 4, h: 4 } };
        const same = { c: { r: 0, g: 0, b: 0, a: 1 }, op: 1, r: { x: 0, y: 0, w: 4, h: 4 } };
        const half = { c: { r: 255, g: 255, b: 255, a: 0.5 }, op: 1, r: { x: 0, y: 0, w: 4, h: 4 } };
        const [a, b, c] = measureAll(png, [white, same, half]).map((x) => x.worst.min);
        run.check(Math.abs(a - 21) < 0.01, `量尺自检：黑底白字应为 21:1，实得 ${a}`);
        run.check(Math.abs(b - 1) < 0.01, `量尺自检：同色应为 1:1，实得 ${b}`);
        run.check(c > 1 && c < a, `量尺自检：半透明白字应介于两者之间，实得 ${c}`);
    }

    {
        // 坏样例：配对 pill 的不透明度一直在闪（祖先层淡入淡出没走完的情形）。量尺必须拒绝出数，不能拿拼凑的画面去量
        const scene = SCENES.find((s) => s.id === "window");
        await openScene(page, server, design, "input", "zh", scene);
        await page.eval(
            `(() => { const s = document.createElement("style"); s.textContent = "@keyframes fxFlicker{from{opacity:.2}to{opacity:1}} .rn-pair{animation:fxFlicker .2s linear infinite alternate}"; document.head.append(s); })()`,
        );
        let msg = "";
        try {
            await measureContrastOnPage(page);
        } catch (e) {
            msg = String(e && e.message);
        }
        run.check(
            msg.includes("量不到静止画面") && msg.includes("不透明度"),
            `坏样例 闪烁的 pill：量尺应拒绝出数并指出不透明度在变，实得 ${JSON.stringify(msg)}`,
        );
    }
    {
        // 静止判据自检：同样的两遍采样要判一致；字色 alpha 在变、不透明度在变、矩形挪位、段数变化都要判不一致
        const base = () => ({
            text: "已连接",
            sel: "rn-pair__text",
            tier: "B",
            scrolled: false,
            r: { x: 10, y: 20, w: 40, h: 12 },
            c: { r: 30, g: 120, b: 80, a: 1 },
            op: 1,
        });
        const same = itemsDrift([base()], [base()]);
        run.check(same === "", `静止判据自检：相同采样应判一致，实得 ${same}`);
        const tiny = base();
        tiny.r.x += 0.3;
        run.check(itemsDrift([base()], [tiny]) === "", "静止判据自检：亚像素抖动（0.3 px）不该判漂");
        const faded = { ...base(), op: 0.5 };
        run.check(itemsDrift([base()], [faded]).includes("不透明度"), "静止判据自检：不透明度 1 → 0.5 应判漂");
        const dim = { ...base(), c: { ...base().c, a: 0.5 } };
        run.check(itemsDrift([base()], [dim]).includes("alpha"), "静止判据自检：文字色 alpha 1 → 0.5 应判漂");
        const moved = base();
        moved.r.y += 3;
        run.check(itemsDrift([base()], [moved]).includes("矩形"), "静止判据自检：矩形下移 3 px 应判漂");
        run.check(itemsDrift([base()], []).includes("段数"), "静止判据自检：段数变化应判漂");
        const items1 = [base()];
        run.check(rectsDrift(items1, [{ ...base().r }]) === "", "静止判据自检：截图后矩形不动应判静止");
        run.check(
            rectsDrift(items1, [{ ...base().r, x: 14 }]).includes("挪了"),
            "静止判据自检：截图期间矩形挪 4 px 应判漂",
        );
        const scrolledItem = [{ ...base(), scrolled: true }];
        run.check(rectsDrift(scrolledItem, [null]) === "", "静止判据自检：滚动区内的项不参与截图后核对");
    }

    run.check(page.errors.length === 0, `页面不应有错误：${page.errors.slice(0, 3).join("；")}`);
} finally {
    await browser.close();
    await server.close();
}
run.finish();
