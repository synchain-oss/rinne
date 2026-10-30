// SPDX-License-Identifier: GPL-3.0-or-later
//
// Output 页面的文字对比度冒烟（无头浏览器）：按实际合成底取最差点，不拿 token 的名义值估算。
// 做法与共享件的对比度冒烟相同（页内列文字 → 文字改透明截图 → 每个文字矩形内逐像素合成算 WCAG 比值，取最差点），
// 区别是分档加了 Output 自己的两类：
//   A 档 ≥ 4.5:1：红色与确认条的消息、模态里的全部文字、动作按钮，加上本页的常驻红字（安全信息）、配对卡副行与轨道名；
//   B 档 ≥ 3.0:1：琥珀 / 灰 / 紫消息、配对状态灯文字；
//   其余（mono 小标、发送行、页脚提示）只记录不判。不设任何豁免：一档里有一段文字不够就是失败。
// 覆盖：全部场景 × 中 / 英 / 法，再加 14 条消息逐条上屏 × 三语。每档的最差值与出处打印出来，贴进 PR 用。
// 无头浏览器的光栅路径与真机有极小差异，设计验收以真机为准。浏览器缺失时整套跳过（退出码 2）。
//
// 用法：node web-preview/tests/smoke-output-contrast-page.mjs [仓库根]

import { createRun, repoRoot } from "./lib/harness.mjs";
import { findBrowser, launchBrowser } from "./lib/browser.mjs";
import { startStaticServer } from "./lib/static-server.mjs";
import { TIER_MIN } from "./shared-measure.mjs";
import {
    LANGS,
    MESSAGE_CASES,
    loadDesign,
    loadOutputScenes,
    measureOutputContrast,
    openOutput,
    setModel,
} from "../fixtures/output/lib.mjs";

const ROOT = repoRoot();
const run = createRun("Output 页面文字对比度");
if (!findBrowser()) run.skip("没有 Chromium 系浏览器");

const box = await loadDesign(ROOT);
const scenes = await loadOutputScenes(ROOT);
const server = await startStaticServer(ROOT);
const browser = await launchBrowser();

const worstOf = { A: null, B: null, C: null };
const counts = { A: 0, B: 0, C: 0 };
/** 按出处（选择器前缀）记每一类 A / B 档文字的最差值，防止某一类被别的类的高分盖住。 */
const bySel = new Map();

function judge(items, tag) {
    for (const it of items) {
        counts[it.tier]++;
        const v = it.worst.min;
        const rec = { tag, tier: it.tier, v, text: it.text, sel: it.sel };
        if (!worstOf[it.tier] || v < worstOf[it.tier].v) worstOf[it.tier] = rec;
        if (it.tier !== "C") {
            const key = `${it.tier} ${it.sel}`;
            if (!bySel.has(key) || v < bySel.get(key).v) bySel.set(key, rec);
        }
        if (it.tier !== "C" && v < TIER_MIN[it.tier]) {
            run.check(
                false,
                `${tag}：${it.tier} 档文字 ${JSON.stringify(it.text)}（${it.sel}）对比度 ${v.toFixed(2)}:1 < ${TIER_MIN[it.tier]}:1，` +
                    `底 rgb(${it.worst.bg}) 字 rgb(${it.worst.fg}) 文字色 ${JSON.stringify(it.c)} 各层不透明度 ${it.op} 矩形 ${JSON.stringify(it.r)}`,
            );
        }
    }
}

try {
    const page = await browser.newPage();

    for (const lang of LANGS) {
        for (const sc of scenes) {
            const tag = `${lang}/${sc.id}`;
            await openOutput(page, server, box, lang, sc.id);
            judge(await measureOutputContrast(page), tag);
        }
        await openOutput(page, server, box, lang, "");
        for (const [id, [notices, state, peer]] of Object.entries(MESSAGE_CASES)) {
            await setModel(page, {
                notices,
                state,
                peer: peer.version || peer.sampleRate ? peer : {},
                group: state === "unassigned" ? null : "A",
            });
            judge(await measureOutputContrast(page), `${lang}/消息 ${id}`);
        }

        // 宿主旁路时成立的消息不降级：红字消息仍按 A 档判（旁路的其余内容按设计降到 .55，单列 C 档只记录）
        for (const [name, model] of [
            ["conflict", { state: "conflict" }],
            ["ipcSandbox", { state: "searching", notices: [{ id: "ipcSandbox" }] }],
        ]) {
            await setModel(page, { ...model, send: { bypassed: true } });
            const items = await measureOutputContrast(page);
            judge(items, `${lang}/旁路 + ${name}`);
            run.check(
                items.some((i) => i.tier === "A" && i.sel.startsWith("rn-banner__text")),
                `${lang}/旁路 + ${name}：消息位的红字应按 A 档量到（旁路时不降级、不被划进 C 档）`,
            );
        }
    }

    // 防空转：各档都量到了足够多的文字，A 档里必须有常驻红字与副行
    run.check(counts.A > 100, `A 档应量到足够多的文字（防空转），实得 ${counts.A}`);
    run.check(counts.B > 40, `B 档应量到足够多的文字（防空转），实得 ${counts.B}`);
    for (const need of ["A rn-place__text", "A rn-paircard__sub", "A rn-track__name"]) {
        run.check(bySel.has(need), `A 档里应量到 ${need}（分档生效），已量到：${[...bySel.keys()].join("、")}`);
    }
    for (const tier of ["A", "B"]) {
        const w = worstOf[tier];
        run.log(
            `  ${tier} 档：${counts[tier]} 段，最差 ${w ? w.v.toFixed(2) : "—"}:1（${w ? `${w.tag}，${JSON.stringify(w.text)}，${w.sel}` : "无"}）门限 ${TIER_MIN[tier]}:1`,
        );
    }
    run.log(`  C 档（只记录）：${counts.C} 段，最差 ${worstOf.C ? worstOf.C.v.toFixed(2) : "—"}:1`);
    for (const [key, rec] of [...bySel].sort((a, b) => a[1].v - b[1].v).slice(0, 8)) {
        run.log(`    ${key}：最差 ${rec.v.toFixed(2)}:1（${rec.tag}）`);
    }

    // ---- 坏样例：把常驻红字压到对比度不够，量尺必须报出 ----
    {
        await openOutput(page, server, box, "zh", "out-connected", "&defect=lowcontrast");
        const items = await measureOutputContrast(page);
        const place = items.filter((i) => i.sel.startsWith("rn-place__text"));
        run.check(place.length > 0, "坏样例 lowcontrast：应量到常驻红字（分档生效）");
        run.check(
            place.some((i) => i.tier === "A" && i.worst.min < TIER_MIN.A),
            `坏样例 lowcontrast：压低字色后常驻红字应低于 ${TIER_MIN.A}:1，实得 ${JSON.stringify(place.map((i) => i.worst.min.toFixed(2)))}`,
        );
    }

    run.check(page.errors.length === 0, `页面不应有错误：${page.errors.slice(0, 3).join("；")}`);
} finally {
    await browser.close();
    await server.close();
}
run.finish();
