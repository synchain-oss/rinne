// SPDX-License-Identifier: GPL-3.0-or-later
//
// Input 页面文字对比度冒烟（无头浏览器）· 首启部分：首启的 8 个场景（语言卡、规则页、导览 6 步）加规则页放行后的画面 × 中 / 英 / 法。
// 从 smoke-input-contrast-page.mjs 拆出来，为的是两个文件各自跑在 run-all 单项 300 秒的上限之内（合在一起实测 267 秒，没有余量），
// 量法、分档与判据一条没动：页内采样每段可见文字的矩形、计算色与各层不透明度，把页面文字改成透明后截图得到合成底，
// 逐像素把文字色合成上去，取矩形内最差的一格。分档 A（≥ 4.5）、B（≥ 3.0）、C 只记录。
// 首启画面的规则：叠层打开时下面的页面被压暗，只判叠层自己的文字，以及叠层之上没被压暗的紧急切断；
//   语言卡与规则页是模态，按 A；导览说明框（标题、正文、步号、提示、按钮）与模态同按 A；规则页的四条红字与脚注是 A 档里的安全红字，红色墨水不动；
//   规则页旁插入链示意是 SVG，字的颜色取 fill，量的时候连同它自己的描边一起藏掉（理由见 fixtures/first-run/measure.mjs）；
//   置灰的控件（规则页未放行时的两颗按钮、导览第 1 步的「上一步」）整体按 0.4 降级、本来就是不可用的画面，沿用预设面板「置灰的不判」的口径按 C 只记录；
//   规则页另摆一个「放行后」（滚到底）的画面，两颗按钮恢复可用后照 A 档判。
// 每个首启画面先断言叠层真的摆出来了（种类、导览步号、规则页的放行状态），量尺不会在一张没有叠层的普通页面上空转。
// 坏样例：
//   置灰的口径不是空设（未放行的按钮确实低于 A 档门槛）、放行后回到 A 档并过线、可用的按钮被压淡到 0.4 照判；
//   导览说明框的字改成暗灰、叠层之上的紧急切断改成浅红，都必须量出低于门槛；
//   叠层摘掉之后下面的页面文字被量到（「叠层打开时只量叠层」的断言在这样的页面上有话说）；
//   普通页面上没有叠层要报；SVG 标签的 fill 改暗要量出明显偏低、好页面上的标签不能是 1:1（藏字样式与按 fill 读色生效）。
// 浏览器缺失时整套跳过（退出码 2），不是通过。本机同时只能有一个浏览器冒烟。
//
// 用法：node web-preview/tests/smoke-input-contrast-first-run-page.mjs [仓库根]

import { createRun, repoRoot } from "./lib/harness.mjs";
import { findBrowser, launchBrowser } from "./lib/browser.mjs";
import { startStaticServer } from "./lib/static-server.mjs";
import { TIER_MIN } from "./shared-measure.mjs";
import {
    FRAMES,
    LANGS,
    SCENES,
    VIEW,
    describeFailure,
    frameTag,
    inFrame,
    isFirstRunScene,
    measure,
    openFrame,
    openInput,
    overlayProbeFailures,
    releaseRules,
} from "./smoke-input-kit-page.mjs";

const ROOT = repoRoot();
const run = createRun("Input 页面对比度（首启）");
if (!findBrowser()) run.skip("没有 Chromium 系浏览器");

const FIRST_RUN_FRAMES = FRAMES.filter((f) => isFirstRunScene(f.scene));

const server = await startStaticServer(ROOT);
const browser = await launchBrowser({ width: VIEW.w, height: VIEW.h });
try {
    const page = await browser.newPage();
    let pages = 0;
    const tally = { A: 0, B: 0, C: 0 };
    for (const lang of LANGS) {
        for (const frame of FIRST_RUN_FRAMES) {
            const tag = frameTag(frame, lang);
            await openFrame(page, server, frame, { lang });
            run.eq(await overlayProbeFailures(page, frame), [], `${tag}：首启叠层应摆出来`);
            const items = await measure(page);
            pages++;
            // 叠层打开时只量叠层自己的字与叠层之上的紧急切断；叠层的字确实量到了、紧急切断也量到了（防空转）
            run.check(items.length > 2, `${tag}：应采到足够多的文字段（防空转），实得 ${items.length}`);
            const stray = items.filter((i) => !i.inOverlay && !i.inPanic);
            run.eq(
                stray.map((i) => i.text),
                [],
                `${tag}：叠层打开时下面被压暗的页面文字不该被量`,
            );
            run.check(
                items.some((i) => i.inOverlay && i.tier === "A"),
                `${tag}：应量到叠层里 A 档的文字（防空转）`,
            );
            run.check(
                items.some((i) => i.inPanic && i.tier === "A"),
                `${tag}：叠层之上的紧急切断应按 A 档量到`,
            );
            for (const it of items) {
                tally[it.tier]++;
                const min = TIER_MIN[it.tier];
                if (!min) continue;
                const ok = it.worst.min >= min;
                run.check(
                    ok,
                    `${tag}：${it.tier} 档文字 ${JSON.stringify(it.text)}（${it.sel}）对比度 ${it.worst.min.toFixed(2)} 低于 ${min}；` +
                        (ok ? "" : await describeFailure(page, it)),
                );
            }
        }
    }
    run.log(`  共量 ${pages} 页；A 档 ${tally.A} 段、B 档 ${tally.B} 段、C 档 ${tally.C} 段（只记录）`);
    run.eq(pages, 9 * LANGS.length, "首启画面：8 个场景加规则页放行后一个，每种语言各量一遍");
    run.eq(FRAMES.length, SCENES.length + 1, "每个场景一个画面，规则页多一个放行后的画面（防新增场景漏测）");
    run.check(tally.A > 0, "A 档应采到文字（防空转）");

    // ---- 首启叠层的坏样例（量尺本身可信：每条新判据都要能被一张坏页面触发）
    const rulesBtn = /开始导览|跳过导览/;
    const tourScene = SCENES.find((s) => s.id === "in-tour-2");
    // 置灰的口径：规则页未放行时两颗按钮是真禁用，按 C 只记录；但它们的对比度确实低于 A 档门槛，
    // 所以没有这条口径，未放行画面就会判红（这一条同时证明「置灰不判」不是空设）
    await openInput(page, server, "in-rules", { lang: "zh" });
    {
        const items = (await measure(page)).filter((i) => i.inBtn && rulesBtn.test(i.text));
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
        const items = (await measure(page)).filter((i) => i.inBtn && rulesBtn.test(i.text));
        run.eq(items.length, 2, "放行后的规则页：应量到两颗按钮的文字");
        run.check(
            items.every((i) => i.tier === "A" && i.worst.min >= TIER_MIN.A),
            `放行后的规则页：两颗按钮应按 A 档并过线，实得 ${JSON.stringify(items.map((i) => [i.tier, +i.worst.min.toFixed(2)]))}`,
        );
        // 坏样例：可用的按钮被压淡到 0.4（视觉上的淡，不是 disabled），不得被当成置灰放过
        await inFrame(page, (doc) => {
            doc.querySelector('[data-part="rules.start"]').style.setProperty("opacity", "0.4", "important");
        });
        const dim = (await measure(page)).filter((i) => i.inBtn && /开始导览/.test(i.text));
        run.check(
            dim.length === 1 && dim[0].tier === "A" && dim[0].worst.min < TIER_MIN.A,
            `坏样例：可用的按钮被压淡到 0.4 仍按 A 档判且量出低于门槛，实得 ${JSON.stringify(dim.map((i) => [i.tier, +i.worst.min.toFixed(2)]))}`,
        );
    }
    // 坏样例：导览说明框里的文字（A 档）改成与底近似的颜色
    await openInput(page, server, "in-tour-2", { lang: "zh" });
    await inFrame(page, (doc) => {
        for (const e of doc.querySelectorAll(".rn-tour__callout, .rn-tour__callout *"))
            e.style.setProperty("color", "rgba(120, 120, 120, 0.45)", "important");
    });
    {
        const items = (await measure(page)).filter((i) => i.inOverlay && i.tier === "A");
        run.check(items.length > 0, "坏样例：应采到导览说明框里 A 档的文字");
        run.check(
            items.some((i) => i.worst.min < TIER_MIN.A),
            `坏样例：把说明框文字改成暗灰后对比度应低于 ${TIER_MIN.A}，实得 ${JSON.stringify(items.map((i) => +i.worst.min.toFixed(2)))}`,
        );
    }
    // 坏样例：叠层之上的紧急切断改成与底近似的颜色，叠层打开时也要被量出来（叠层打开不等于紧急切断免检）
    await openInput(page, server, "in-tour-2", { lang: "zh" });
    await inFrame(page, (doc) => {
        const e = doc.querySelector(".in-panic");
        e.style.setProperty("color", "rgba(255, 190, 190, 0.55)", "important");
        for (const c of e.querySelectorAll("*")) c.style.setProperty("color", "rgba(255, 190, 190, 0.55)", "important");
    });
    {
        const panic = (await measure(page)).filter((i) => i.inPanic && i.tier === "A");
        run.check(panic.length > 0, "坏样例：导览打开时应采到紧急切断的文字");
        run.check(
            panic.some((i) => i.worst.min < TIER_MIN.A),
            `坏样例：导览打开时把紧急切断改成浅红，对比度应低于 ${TIER_MIN.A}，实得 ${JSON.stringify(panic.map((i) => +i.worst.min.toFixed(2)))}`,
        );
    }
    // 坏样例：叠层不在时（把导览的节点摘掉）下面的页面文字就是要量的对象；「叠层打开时不量页面」的断言在这样的页面上必须有话说
    await openInput(page, server, "in-tour-2", { lang: "zh" });
    await inFrame(page, (doc) => doc.querySelector(".rn-tour").remove());
    {
        const stray = (await measure(page)).filter((i) => !i.inOverlay && !i.inPanic);
        run.check(stray.length > 10, `坏样例：叠层摘掉之后页面文字应被量到，实得 ${stray.length} 段`);
    }
    // 坏样例：量尺不能在没有叠层的普通页面上空转
    await openInput(page, server, "in-playing", { lang: "zh" });
    run.check(
        (await overlayProbeFailures(page, { scene: tourScene, sub: "" })).length > 0,
        "坏样例：普通页面上没有叠层，应判「叠层没摆出来」",
    );
    // SVG 文字（规则页旁的插入链示意）：取 fill 当字色、截图前连同自己的描边一起藏掉。两头各有一条判据：
    //   好页面上的标签对比度不能是 1:1（藏字样式没盖住 SVG，截图里还留着字，合成上去就是 1:1，或字色读成了 color 而不是 fill）；
    //   把标签的 fill 改暗之后，对比度要比好页面明显低（字色确实是按 fill 读的）。
    await openInput(page, server, "in-rules", { lang: "zh" });
    const svgOf = (items) => items.filter((i) => i.sel === "text" || i.sel === "tspan");
    const svgGood = svgOf(await measure(page));
    run.check(svgGood.length >= 3, `应采到插入链示意里的 SVG 文字，实得 ${svgGood.length} 段`);
    run.log(
        `  插入链示意的 SVG 标签对比度（C 档只记录）：${svgGood.map((i) => `${i.text} ${i.worst.min.toFixed(2)}`).join("，")}`,
    );
    run.check(
        svgGood.every((i) => i.worst.min > 1.5),
        `SVG 标签的对比度不该是 1:1（藏字样式要盖住 SVG 文字，字色要按 fill 读），实得 ${JSON.stringify(svgGood.map((i) => [i.text, +i.worst.min.toFixed(2)]))}`,
    );
    await inFrame(page, (doc) => {
        for (const t of doc.querySelectorAll(".rn-chain svg text"))
            t.style.setProperty("fill", "rgba(120, 120, 120, 0.4)", "important");
    });
    {
        const dim = svgOf(await measure(page));
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
