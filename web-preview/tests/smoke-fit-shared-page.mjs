// SPDX-License-Identifier: GPL-3.0-or-later
//
// 共享件的适配冒烟（无头浏览器）：Input / Output × 中 / 英 / 法 × 整窗、关于面板、缩放确认、轻提示与各状态目录。
// 逐页量三件事：
//   ① 所有可见文字不越出所在的 data-gb 件、不越出外壳、不被 overflow 裁掉、互相不压住；data-gb 件不越出外壳；
//   ② 夹具登记的「最坏串」（最长的消息、带占位符的最长取值、各状态的标签）确实上屏了，防止量了个空；
//   ③ 界面上不得出现裸词条键、没替换的占位符、null / undefined（文字与 title、aria-label 都查）；
//   ④ 坏样例：夹具的 ?defect=overflow / clip / rawkey / placeholder 必须被上面各项抓出来（量尺本身可信）。
// 浏览器缺失时整套跳过（退出码 2），不是通过。本机同时只能有一个浏览器冒烟。
//
// 用法：node web-preview/tests/smoke-fit-shared-page.mjs [仓库根]

import { createRun, repoRoot } from "./lib/harness.mjs";
import { findBrowser, launchBrowser } from "./lib/browser.mjs";
import { startStaticServer } from "./lib/static-server.mjs";
import {
    ROLES,
    LANGS,
    SCENES,
    loadDesign,
    openScene,
    FIT_PROBE,
    EXPECTED_PROBE,
    missingExpected,
    findLeaks,
} from "./shared-measure.mjs";

const ROOT = repoRoot();
const run = createRun("共享件适配");
if (!findBrowser()) run.skip("没有 Chromium 系浏览器");

const design = await loadDesign(ROOT);
const server = await startStaticServer(ROOT);
const browser = await launchBrowser();

try {
    const page = await browser.newPage();
    const requests = await page.recordRequests();
    let pages = 0;
    let texts = 0;

    for (const role of ROLES) {
        for (const lang of LANGS) {
            for (const scene of SCENES) {
                const tag = `${role}/${lang}/${scene.id}`;
                await openScene(page, server, design, role, lang, scene);
                const probe = await page.eval(FIT_PROBE);
                const expected = await page.eval(EXPECTED_PROBE);
                pages++;
                texts += probe.texts;
                run.check(probe.texts > 0, `${tag}：页上应有可见文字（防空转）`);
                const bad = probe.problems.slice(0, 4).map((p) => `${p.kind}：${p.detail}`);
                run.check(
                    probe.problems.length === 0,
                    `${tag}：${bad.join("；")}${probe.problems.length > 4 ? "…" : ""}`,
                );
                const lost = missingExpected(expected, probe);
                run.check(lost.length === 0, `${tag}：这些最坏串没上屏 ${JSON.stringify(lost.map((e) => e.text))}`);
                run.check(expected.length > 0, `${tag}：夹具应登记要上屏的文字（防空转）`);
                const leaks = findLeaks(`${probe.joined} ${probe.attrs.join(" ")}`);
                run.check(
                    leaks.length === 0,
                    `${tag}：界面上出现了裸键、占位符或 null：${JSON.stringify(leaks.slice(0, 5))}`,
                );
            }
        }
    }
    run.log(`  共量 ${pages} 页、${texts} 段可见文字`);

    // ---- 坏样例：量尺必须抓得出 ----
    const defects = [
        {
            defect: "overflow",
            scene: SCENES.find((s) => s.id === "pair"),
            kinds: ["outside", "clipped"],
            role: "output",
        },
        { defect: "clip", scene: SCENES.find((s) => s.id === "bands"), kinds: ["clipped", "outside"], role: "output" },
        {
            defect: "clip",
            scene: SCENES.find((s) => s.id === "window-danger"),
            kinds: ["clipped", "outside"],
            role: "input",
        },
        // Input 一行槽里的长文字：必须是越出槽被抓到，而不是悄悄折成两行塞进 30 px
        {
            defect: "longmsg",
            scene: SCENES.find((s) => s.id === "window-danger"),
            kinds: ["outside"],
            never: ["wrapped"],
            role: "input",
        },
    ];
    for (const d of defects) {
        await openScene(page, server, design, d.role, "en", d.scene, `&defect=${d.defect}`);
        const probe = await page.eval(FIT_PROBE);
        const got = new Set(probe.problems.map((p) => p.kind));
        run.check(
            d.kinds.some((k) => got.has(k)),
            `坏样例 ${d.defect}（${d.role}/${d.scene.id}）应被量出 ${d.kinds.join(" 或 ")}，实得 ${JSON.stringify([...got])}`,
        );
        if (d.never)
            run.check(
                !d.never.some((k) => got.has(k)),
                `坏样例 ${d.defect}（${d.role}/${d.scene.id}）不应出现 ${d.never.join("、")}，实得 ${JSON.stringify([...got])}`,
            );
    }
    // 裸键、没替换的占位符：必须被扫出来
    for (const [defect, want] of [
        ["rawkey", "band.in.neverDefined"],
        ["placeholder", "{g}"],
    ]) {
        await openScene(
            page,
            server,
            design,
            "input",
            "zh",
            SCENES.find((x) => x.id === "bands"),
            `&defect=${defect}`,
        );
        const probe = await page.eval(FIT_PROBE);
        const leaks = findLeaks(`${probe.joined} ${probe.attrs.join(" ")}`);
        run.check(leaks.includes(want), `坏样例 ${defect}：应扫出 ${want}，实得 ${JSON.stringify(leaks)}`);
    }
    // 最坏串不上屏的坏样例：把页上一段登记过的文字改掉，missingExpected 必须报缺
    await openScene(page, server, design, "input", "zh", SCENES[0]);
    await page.eval(
        `(() => { const e = document.querySelector(".rn-footer__hintText") || document.querySelector(".rn-footer__hint"); e.textContent = "占位"; })()`,
    );
    {
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
