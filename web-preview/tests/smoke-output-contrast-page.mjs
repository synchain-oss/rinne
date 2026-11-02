// SPDX-License-Identifier: GPL-3.0-or-later
//
// Output 页面的文字对比度冒烟（无头浏览器）：按实际合成底取最差点，不拿 token 的名义值估算。
// 做法与共享件的对比度冒烟相同（页内列文字 → 文字改透明截图 → 每个文字矩形内逐像素合成算 WCAG 比值，取最差点），
// 区别是分档加了 Output 自己的两类：
//   A 档 ≥ 4.5:1：红色与确认条的消息、模态里的全部文字、动作按钮，加上本页的常驻红字（安全信息）、配对卡副行与轨道名；
//   B 档 ≥ 3.0:1：琥珀 / 灰 / 紫消息、配对状态灯文字；
//   其余（mono 小标、发送行、页脚提示）只记录不判。不设任何豁免：一档里有一段文字不够就是失败。
// 覆盖：全部场景（含首启的 6 个）× 中 / 英 / 法，再加 14 条消息逐条上屏 × 三语。每档的最差值与出处打印出来，贴进 PR 用。
// 首启的 6 个场景（语言卡、规则页、导览 4 步）：叠层打开时下面的页面被压暗，只判叠层自己的文字；语言卡与规则页是模态，按 A；
//   导览说明框（标题、正文、步号、提示、按钮）与模态同按 A；规则页的四条红字与脚注是 A 档里的安全红字，红色墨水不动；
//   规则页旁插入链示意是 SVG，字的颜色取 fill，量的时候连同它自己的描边一起藏掉（理由见 fixtures/first-run/measure.mjs）；
//   置灰的控件（规则页未放行时的两颗按钮、导览第 1 步的「上一步」）整体按 0.4 降级、本来就是不可用的画面，沿用 Input 页面冒烟「置灰的不判」的口径按 C 只记录；
//   规则页另摆一个「放行后」（滚到底）的画面，两颗按钮恢复可用后照 A 档判。Output 没有紧急切断，叠层之上没有需要另量的东西。
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
    frameTag,
    isFirstRunScene,
    loadDesign,
    loadOutputScenes,
    measureOutputContrast,
    openOutput,
    openOutputFrame,
    outputFrames,
    overlayFailures,
    releaseRules,
    setModel,
} from "../fixtures/output/lib.mjs";

const ROOT = repoRoot();
const run = createRun("Output 页面文字对比度");
if (!findBrowser()) run.skip("没有 Chromium 系浏览器");

const box = await loadDesign(ROOT);
const scenes = await loadOutputScenes(ROOT);
const frames = outputFrames(scenes);
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

    let firstRunFrames = 0;
    for (const lang of LANGS) {
        for (const frame of frames) {
            const tag = frameTag(lang, frame);
            await openOutputFrame(page, server, box, lang, frame);
            run.eq(await overlayFailures(page, frame), [], `${tag}：首启叠层应摆出来`);
            const items = await measureOutputContrast(page);
            if (isFirstRunScene(frame.scene)) {
                // 叠层打开时只量叠层自己的字（下面被压暗的页面不量）；叠层里的字确实量到了（防空转）
                firstRunFrames++;
                run.eq(
                    items.filter((i) => !i.inOverlay).map((i) => i.text),
                    [],
                    `${tag}：叠层打开时下面被压暗的页面文字不该被量`,
                );
                run.check(
                    items.some((i) => i.inOverlay && i.tier === "A"),
                    `${tag}：应量到叠层里 A 档的文字（防空转）`,
                );
            }
            judge(items, tag);
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

    run.eq(firstRunFrames, 7 * LANGS.length, "首启画面：6 个场景加规则页放行后一个，每种语言各量一遍");
    run.eq(frames.length, scenes.length + 1, "每个场景一个画面，规则页多一个放行后的画面（防新增场景漏测）");

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

    // ---- 首启的坏样例（量尺本身可信：每条新判据都要能被一张坏页面触发）----
    const rulesBtn = /开始导览|跳过导览/;
    // 置灰的口径：规则页未放行时两颗按钮是真禁用，按 C 只记录；但它们的对比度确实低于 A 档门槛，没有这条口径未放行画面就会判红
    await openOutput(page, server, box, "zh", "out-rules");
    {
        const items = (await measureOutputContrast(page)).filter((i) => i.inBtn && rulesBtn.test(i.text));
        run.eq(items.length, 2, "未放行的规则页：应量到两颗按钮的文字");
        run.check(
            items.every((i) => i.tier === "C"),
            `未放行的规则页：真禁用的按钮应按 C 只记录，实得 ${JSON.stringify(items.map((i) => [i.text, i.tier]))}`,
        );
        run.check(
            items.every((i) => i.worst.min < TIER_MIN.A),
            `未放行的规则页：两颗按钮置灰后确实低于 ${TIER_MIN.A}（若不低，置灰口径就是多余的），实得 ${JSON.stringify(items.map((i) => +i.worst.min.toFixed(2)))}`,
        );
    }
    // 放行后：同样两颗按钮恢复可用，必须回到 A 档并过线
    await releaseRules(page);
    {
        const items = (await measureOutputContrast(page)).filter((i) => i.inBtn && rulesBtn.test(i.text));
        run.eq(items.length, 2, "放行后的规则页：应量到两颗按钮的文字");
        run.check(
            items.every((i) => i.tier === "A" && i.worst.min >= TIER_MIN.A),
            `放行后的规则页：两颗按钮应按 A 档并过线，实得 ${JSON.stringify(items.map((i) => [i.tier, +i.worst.min.toFixed(2)]))}`,
        );
        // 坏样例：可用的按钮被压淡到 0.4（视觉上的淡，不是 disabled），不得被当成置灰放过
        await page.eval(
            `document.querySelector('[data-part="rules.start"]').style.setProperty("opacity", "0.4", "important")`,
        );
        const dim = (await measureOutputContrast(page)).filter((i) => i.inBtn && /开始导览/.test(i.text));
        run.check(
            dim.length === 1 && dim[0].tier === "A" && dim[0].worst.min < TIER_MIN.A,
            `坏样例：可用的按钮被压淡到 0.4 仍按 A 档判且量出低于门槛，实得 ${JSON.stringify(dim.map((i) => [i.tier, +i.worst.min.toFixed(2)]))}`,
        );
    }
    // 坏样例：导览说明框里的文字（A 档）改成与底近似的颜色
    await openOutput(page, server, box, "zh", "out-tour-2");
    await page.eval(
        `(() => { for (const e of document.querySelectorAll(".rn-tour__callout, .rn-tour__callout *")) e.style.setProperty("color", "rgba(120, 120, 120, 0.45)", "important"); })()`,
    );
    {
        const items = (await measureOutputContrast(page)).filter((i) => i.inOverlay && i.tier === "A");
        run.check(items.length > 0, "坏样例：应采到导览说明框里 A 档的文字");
        run.check(
            items.some((i) => i.worst.min < TIER_MIN.A),
            `坏样例：把说明框文字改成暗灰后对比度应低于 ${TIER_MIN.A}，实得 ${JSON.stringify(items.map((i) => +i.worst.min.toFixed(2)))}`,
        );
    }
    // 坏样例：叠层不在时（把导览的节点摘掉）下面的页面文字就是要量的对象；「叠层打开时不量页面」的断言在这样的页面上必须有话说
    await openOutput(page, server, box, "zh", "out-tour-2");
    await page.eval(`document.querySelector(".rn-tour").remove()`);
    {
        const stray = (await measureOutputContrast(page)).filter((i) => !i.inOverlay);
        run.check(stray.length > 10, `坏样例：叠层摘掉之后页面文字应被量到，实得 ${stray.length} 段`);
    }
    // 坏样例：量尺不能在没有叠层的普通页面上空转
    await openOutput(page, server, box, "zh", "out-connected");
    run.check(
        (await overlayFailures(page, { scene: scenes.find((s) => s.id === "out-tour-2"), sub: "" })).length > 0,
        "坏样例：普通页面上没有叠层，应判「叠层没摆出来」",
    );
    // SVG 文字（规则页旁的插入链示意）：取 fill 当字色、截图前连同自己的描边一起藏掉。两头各有一条判据：
    //   好页面上的标签对比度不能是 1:1（藏字样式没盖住 SVG，截图里还留着字，合成上去就是 1:1，或字色读成了 color 而不是 fill）；
    //   把标签的 fill 改暗之后，对比度要比好页面明显低（字色确实是按 fill 读的）。
    await openOutput(page, server, box, "zh", "out-rules");
    const svgOf = (items) => items.filter((i) => i.sel === "text" || i.sel === "tspan");
    const svgGood = svgOf(await measureOutputContrast(page));
    run.check(svgGood.length >= 3, `应采到插入链示意里的 SVG 文字，实得 ${svgGood.length} 段`);
    run.log(
        `  插入链示意的 SVG 标签对比度（C 档只记录）：${svgGood.map((i) => `${i.text} ${i.worst.min.toFixed(2)}`).join("，")}`,
    );
    run.check(
        svgGood.every((i) => i.worst.min > 1.5),
        `SVG 标签的对比度不该是 1:1（藏字样式要盖住 SVG 文字，字色要按 fill 读），实得 ${JSON.stringify(svgGood.map((i) => [i.text, +i.worst.min.toFixed(2)]))}`,
    );
    await page.eval(
        `(() => { for (const t of document.querySelectorAll(".rn-chain svg text")) t.style.setProperty("fill", "rgba(120, 120, 120, 0.4)", "important"); })()`,
    );
    {
        const dim = svgOf(await measureOutputContrast(page));
        const goodMin = Math.min(...svgGood.map((i) => i.worst.min));
        run.check(
            dim.length === svgGood.length && dim.every((i) => i.worst.min < goodMin - 0.5),
            `坏样例：把 SVG 标签的 fill 改暗，对比度应明显低于好页面的最低 ${goodMin.toFixed(2)}，实得 ${JSON.stringify(dim.map((i) => [i.text, +i.worst.min.toFixed(2)]))}`,
        );
    }

    run.check(page.errors.length === 0, `页面不应有错误：${page.errors.slice(0, 3).join("；")}`);
} finally {
    await browser.close();
    await server.close();
}
run.finish();
