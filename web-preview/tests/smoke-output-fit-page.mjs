// SPDX-License-Identifier: GPL-3.0-or-later
//
// Output 页面的适配冒烟（无头浏览器）：真页面（夹具 web-preview/fixtures/output）× 中 / 英 / 法 × 全部场景与全部消息。
// 量四件事：
//   ① 所有可见文字不越出所在件、不越出外壳、不被 overflow 裁掉（含 line-clamp 截断）、互相不压住；data-gb 件不越出外壳；
//   ② 登记的「必须上屏」文字（红字、状态灯、副行、消息、页脚提示、发送行、轨道名）确实上屏，防止量了个空；
//   ③ 界面上没有裸词条键、没替换的占位符、null / undefined（文字与 title、aria-label 都查）；
//   ④ 行数：常驻红字 ≤ 2 行（设计盒里只留了两行的位置）、副行 ≤ 2 行、发送行与页脚提示 1 行、消息位文字 ≤ 3 行。
// 场景之外再造最坏组合：14 条消息逐条上屏（三语）、超长轨道名、多条消息同时在场（「+n」）、单声道与采样率最宽的写法。
// 末尾是坏样例：夹具的 ?defect=overflow / place3 / rawkey 必须被上面各项抓出来（量尺本身可信）。
// 浏览器缺失时整套跳过（退出码 2）。
//
// 用法：node web-preview/tests/smoke-output-fit-page.mjs [仓库根]

import { createRun, repoRoot } from "./lib/harness.mjs";
import { findBrowser, launchBrowser } from "./lib/browser.mjs";
import { startStaticServer } from "./lib/static-server.mjs";
import { FIT_PROBE, EXPECTED_PROBE, missingExpected, findLeaks } from "./shared-measure.mjs";
import {
    LANGS,
    MESSAGE_CASES,
    loadDesign,
    loadOutputScenes,
    openOutput,
    setModel,
    lineCount,
    textOf,
} from "../fixtures/output/lib.mjs";

const ROOT = repoRoot();
const run = createRun("Output 页面适配");
if (!findBrowser()) run.skip("没有 Chromium 系浏览器");

const box = await loadDesign(ROOT);
const scenes = await loadOutputScenes(ROOT);
const server = await startStaticServer(ROOT);
const browser = await launchBrowser();

/** 量一遍当前页：带标签地登记失败。返回探针结果。 */
async function measure(page, tag, allow = null) {
    const probe = await page.eval(FIT_PROBE);
    // allow：设计上就该被省略号截断的那几处（超长轨道名），量尺会把它们报成 clipped / outside，这里按名字放行，其余照报
    if (allow) probe.problems = probe.problems.filter((p) => !allow.test(p.detail));
    const expected = await page.eval(EXPECTED_PROBE);
    const bad = probe.problems.slice(0, 4).map((p) => `${p.kind}：${p.detail}`);
    run.check(probe.texts > 0, `${tag}：页上应有可见文字（防空转）`);
    run.check(probe.problems.length === 0, `${tag}：${bad.join("；")}${probe.problems.length > 4 ? "…" : ""}`);
    const lost = missingExpected(expected, probe);
    run.check(lost.length === 0, `${tag}：这些文字没上屏 ${JSON.stringify(lost.map((e) => e.text))}`);
    run.check(expected.length > 0, `${tag}：夹具应登记要上屏的文字（防空转）`);
    const leaks = findLeaks(`${probe.joined} ${probe.attrs.join(" ")}`);
    run.check(leaks.length === 0, `${tag}：界面上出现了裸键、占位符或 null：${JSON.stringify(leaks.slice(0, 5))}`);
    return probe;
}

/** 行数断言：红字 ≤ 2、副行 ≤ 2、发送行与页脚提示 1、消息位文字 ≤ 3。 */
async function checkLines(page, tag) {
    const place = await lineCount(page, ".rn-place__text");
    run.check(place >= 1 && place <= 2, `${tag}：常驻红字应在 2 行以内，实得 ${place} 行`);
    const sub = await lineCount(page, '[data-part="pair.sub"]');
    run.check(sub >= 1 && sub <= 2, `${tag}：配对卡副行应在 2 行以内，实得 ${sub} 行`);
    const send = await lineCount(page, '[data-gb="output.send"]');
    run.eq(send, 1, `${tag}：发送行应是 1 行`);
    const hint = await lineCount(page, '[data-gb="output.footer.hint"]');
    run.check(hint <= 1, `${tag}：页脚提示应是 1 行，实得 ${hint} 行`);
    const band = await lineCount(
        page,
        '[data-gb="output.band"] .rn-banner__text, [data-gb="output.band"] .rn-banner__idle',
    );
    run.check(band >= 1 && band <= 3, `${tag}：消息位文字应在 3 行以内，实得 ${band} 行`);
}

try {
    const page = await browser.newPage();
    const requests = await page.recordRequests();
    let pages = 0;
    let texts = 0;

    // ---- 全部场景 × 三语 ----
    for (const lang of LANGS) {
        for (const sc of scenes) {
            const tag = `${lang}/${sc.id}`;
            await openOutput(page, server, box, lang, sc.id);
            const probe = await measure(page, tag);
            pages++;
            texts += probe.texts;
            await checkLines(page, tag);
        }
    }

    // ---- 逐条消息 × 三语（同一张页反复改模型，省得重开） ----
    for (const lang of LANGS) {
        await openOutput(page, server, box, lang, "");
        for (const [id, [notices, state, peer]] of Object.entries(MESSAGE_CASES)) {
            const tag = `${lang}/消息 ${id}`;
            const group = state === "unassigned" ? null : "A";
            await setModel(page, { notices, state, peer: peer.version || peer.sampleRate ? peer : {}, group });
            run.check(
                (await page.eval(`document.querySelector('[data-gb="output.band"]').getAttribute("data-msg")`)) ===
                    `band.out.${id}`,
                `${tag}：消息位应显示这一条`,
            );
            const probe = await measure(page, tag);
            pages++;
            texts += probe.texts;
            await checkLines(page, tag);
        }

        // ---- 最坏组合 ----
        // 超长轨道名（宿主给的轨道名不受控）：两行都省略，不撑破配对卡
        const long = "Chœurs doublés – refrain 2 (bus stéréo) – prise alternative très longue 浓缩人声总线 final mix";
        await setModel(page, {
            state: "crossTrack",
            verify: "diff",
            peer: { name: long, channels: 1 },
            self: { name: long },
        });
        {
            const tag = `${lang}/超长轨道名`;
            const probe = await measure(page, tag, /track.(this|input).name|Chœurs doublés/);
            pages++;
            texts += probe.texts;
            await checkLines(page, tag);
            const cut = await page.eval(
                `[...document.querySelectorAll(".rn-track__name")].filter((e) => !e.hidden).map((e) => ({ ellipsis: getComputedStyle(e).textOverflow === "ellipsis", truncated: e.scrollWidth > e.clientWidth, inCard: e.getBoundingClientRect().right <= e.closest('[data-gb="output.pair"]').getBoundingClientRect().right }))`,
            );
            run.eq(
                cut,
                [1, 2].map(() => ({ ellipsis: true, truncated: true, inCard: true })),
                `${tag}：两行轨道名都应被省略号截断且不越出配对卡`,
            );
            // 省略号截断后真正看得见的那一段不能压住右边的核验徽标与声道徽标（量尺按整段文字比，这里按可见框比）
            const gap = await page.eval(
                `(() => { const row = document.querySelector('[data-part="track.input"]'); const n = row.querySelector(".rn-track__name").getBoundingClientRect(); const end = row.querySelector(".rn-track__end").getBoundingClientRect(); return { endShown: end.width > 0, gap: end.left - n.right }; })()`,
            );
            run.check(
                gap.endShown && gap.gap >= -0.5,
                `${tag}：轨道名的可见框不得压住右边的徽标，实得 ${JSON.stringify(gap)}`,
            );
            const titled = await page.eval(
                `[...document.querySelectorAll(".rn-track__name")].filter((e) => !e.hidden).map((e) => e.getAttribute("title") || "")`,
            );
            run.check(
                titled.length === 2 && titled.every((s) => s.length > 40),
                `${tag}：被省略的轨道名要在 title 里留全名，实得 ${JSON.stringify(titled.map((s) => s.length))}`,
            );
        }
        // 多条消息同时在场：「+n」chip、红色优先
        await setModel(page, {
            notices: [{ id: "ipcSandbox" }, { id: "newerProject", a: "0.11.0", b: "0.10.12" }, { id: "outOfOrder" }],
            state: "abiMismatch",
            verify: "diff",
            peer: { version: "0.10.12" },
        });
        {
            const tag = `${lang}/多条同时在场`;
            const probe = await measure(page, tag);
            pages++;
            texts += probe.texts;
            await checkLines(page, tag);
            run.eq(
                await textOf(page, '[data-gb="output.band"] [data-part="band.more"]'),
                "+4",
                `${tag}：五条消息显示「+4」`,
            );
        }
        // 单声道发送 + 44.1 kHz；对端立体声：声道徽标变成不同色的「Input→Output」
        await setModel(page, {
            state: "connected",
            send: { channels: "MONO", sampleRate: 44100 },
            peer: { channels: 2 },
        });
        {
            const tag = `${lang}/声道不同`;
            const probe = await measure(page, tag);
            pages++;
            texts += probe.texts;
            await checkLines(page, tag);
        }
    }
    run.log(`  共量 ${pages} 页、${texts} 段可见文字`);

    // ---- 坏样例：量尺必须抓得出 ----
    {
        await openOutput(page, server, box, "zh", "out-connected", "&defect=overflow");
        const probe = await page.eval(FIT_PROBE);
        const got = new Set(probe.problems.map((p) => p.kind));
        run.check(
            got.has("outside") || got.has("clipped"),
            `坏样例 overflow：应被量出 outside 或 clipped，实得 ${JSON.stringify([...got])}`,
        );
    }
    {
        await openOutput(page, server, box, "zh", "out-connected", "&defect=place3");
        const n = await lineCount(page, ".rn-place__text");
        run.check(n > 2, `坏样例 place3：红字撑到 3 行以上时行数应 > 2（行数量尺可信），实得 ${n}`);
        const probe = await page.eval(FIT_PROBE);
        run.check(
            probe.problems.length > 0,
            "坏样例 place3：红字撑出两行位置时，越界/裁切量尺也应报出来（红字不能被悄悄裁掉）",
        );
    }
    {
        await openOutput(page, server, box, "zh", "out-connected", "&defect=rawkey");
        const probe = await page.eval(FIT_PROBE);
        const leaks = findLeaks(`${probe.joined} ${probe.attrs.join(" ")}`);
        run.check(
            leaks.includes("out.neverDefined"),
            `坏样例 rawkey：应扫出 out.neverDefined，实得 ${JSON.stringify(leaks)}`,
        );
    }
    {
        // 登记的文字不上屏：把页脚提示改掉，必须被报缺
        await openOutput(page, server, box, "zh", "out-connected");
        await page.eval(`document.querySelector('[data-gb="output.footer.hint"]').textContent = "占位"`);
        const probe = await page.eval(FIT_PROBE);
        const expected = await page.eval(EXPECTED_PROBE);
        run.check(missingExpected(expected, probe).length > 0, "坏样例：改掉页脚提示后，登记的上屏文字应被报缺");
    }

    run.check(page.errors.length === 0, `页面不应有错误：${page.errors.slice(0, 3).join("；")}`);
    const foreign = requests.filter((u) => !/^(data|blob|about):/.test(u) && new URL(u).origin !== server.url);
    run.check(foreign.length === 0, `夹具页的请求都必须同源，外部请求：${JSON.stringify(foreign)}`);
} finally {
    await browser.close();
    await server.close();
}
run.finish();
