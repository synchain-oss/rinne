// SPDX-License-Identifier: GPL-3.0-or-later
//
// Input 页面无桥模式冒烟（无头浏览器）：直接打开真源页面 /web/input/index.html，既没有预览注入的 mock、也没有 JUCE 后端
// （首帧冒烟与离线扫描就是这样打开页面的）。页面必须干净地起来并显示默认状态，而不是报错或空壳：
//   ① 什么都没有：外壳在、配对 pill 是「搜索中」、反馈 0 %、Mix 25 %、延迟 100 ms、高低切 20 Hz / 20 kHz、限制器上限 −6 dBFS，页面无错误；
//   ② 假宿主只有 postMessage（没有 backend）：同样按默认状态起来；首帧信号发出后 __rinneBooted 置位（主模块也已初始化完成）；
//   ③ 坏样例：把同一套探针用在一个空页面上，必须判不过（探针不是空转）。
// 浏览器缺失时整套跳过（退出码 2），不是通过。本机同时只能有一个浏览器冒烟。
//
// 用法：node web-preview/tests/smoke-input-offline-page.mjs [仓库根]

import { createRun, repoRoot } from "./lib/harness.mjs";
import { findBrowser, launchBrowser } from "./lib/browser.mjs";
import { startStaticServer } from "./lib/static-server.mjs";

const ROOT = repoRoot();
const run = createRun("Input 页面无桥模式");
if (!findBrowser()) run.skip("没有 Chromium 系浏览器");

const MINUS = "−";
const WANT = [
    ["fb.readout", "0%"],
    ["mix.readout", "25%"],
    ["dly.readout", "100.0"],
    ["ceiling.readout", `上限 ${MINUS}6.0 dBFS`],
];

/** 页内探针：外壳与 pill 的状态，以及几处读数的文字（取第一个匹配的 data-part）。 */
const PROBE = `(() => {
    const q = (s) => document.querySelector(s);
    const text = (p) => { const e = q('[data-part="' + p + '"]'); return e ? e.textContent.trim() : null; };
    return {
        shell: !!q('[data-gb="input.shell"]'),
        pill: (q('[data-gb="input.pair.pill"]') || { getAttribute: () => null }).getAttribute("data-state"),
        texts: ${JSON.stringify(WANT.map((w) => w[0]))}.map(text),
        booted: window.__rinneBooted === true,
    };
})()`;

/** 起一个页面，装好（可选的）假宿主，打开 url，等探针里 pill 有状态再回探针结果。 */
async function open(browser, server, url, { fakeHost, expectUp = true }) {
    const page = await browser.newPage();
    if (fakeHost)
        await page.send("Page.addScriptToEvaluateOnNewDocument", {
            source: `window.__JUCE__ = { postMessage: function () {} };`,
        });
    await page.goto(`${server.url}${url}`);
    if (expectUp)
        await page.waitFor(`!!document.querySelector('[data-gb="input.pair.pill"][data-state]')`, {
            label: `${url}：配对 pill 应出现`,
        });
    // 默认快照排在微任务里，读数再等一拍
    await page.eval(`new Promise((r) => setTimeout(r, 300))`);
    return page;
}

const server = await startStaticServer(ROOT);
const browser = await launchBrowser({ width: 900, height: 700 });
try {
    const check = (tag, p, wantBooted) => {
        run.check(p.shell, `${tag}：应有外壳`);
        run.eq(p.pill, "searching", `${tag}：无桥时配对 pill 是搜索中`);
        run.eq(
            p.texts,
            WANT.map((w) => w[1]),
            `${tag}：默认读数（${WANT.map((w) => w[0]).join("、")}）`,
        );
        run.eq(p.booted, wantBooted, `${tag}：__rinneBooted`);
    };

    // ① 什么都没有
    {
        const page = await open(browser, server, "/web/input/index.html", { fakeHost: false });
        check("无 mock 无宿主", await page.eval(PROBE), false);
        run.check(page.errors.length === 0, `无 mock 无宿主：页面不应有错误：${page.errors.slice(0, 3).join("；")}`);
        await page.close();
    }

    // ② 假宿主只有 postMessage
    {
        const page = await open(browser, server, "/web/input/index.html", { fakeHost: true });
        await page.waitFor(`window.__rinneBooted === true`, {
            label: "首帧信号发出且主模块就绪后 __rinneBooted 应置位",
        });
        check("只有 postMessage 的假宿主", await page.eval(PROBE), true);
        run.check(
            page.errors.length === 0,
            `只有 postMessage 的假宿主：页面不应有错误：${page.errors.slice(0, 3).join("；")}`,
        );
        await page.close();
    }

    // ③ 坏样例：空页面过不了同一套探针
    {
        const page = await open(browser, server, "/web-preview/fixtures/input/no-bridge.html", {
            fakeHost: false,
            expectUp: false,
        });
        const p = await page.eval(PROBE);
        run.check(
            !p.shell && p.pill === null && p.texts.every((t) => t === null),
            `坏样例：空页面应探不到外壳、pill 与读数，实得 ${JSON.stringify(p)}`,
        );
        await page.close();
    }
} finally {
    await browser.close();
    await server.close();
}
run.finish();
