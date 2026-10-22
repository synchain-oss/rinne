// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/web-preview.json
//
// 首帧信号与 boot 守卫，页面级冒烟（无头浏览器）。静态面（事件名、结构）由 smoke-embedded-resources 钉住，
// 这里钉行为：在每个角色的真源页面里装一个假的 window.__JUCE__.postMessage 收信号，
//   ① 正常路径：页面画出第一帧之后恰好发一次 __rinne__firstFrame，载荷带非负的 paintDeltaMs，随后 __rinneBooted 置位，
//      期间没有 boot 错误，也没有控制台错误（缺字体、缺样式都会在这里露出来）；
//   ② 没有 PerformanceObserver：回落到 DOMContentLoaded 触发，不许变成永不放行；
//   ③ 没有 requestAnimationFrame：直接发信号；
//   ④ rAF 永不回调（合成停摆，窗口最小化或被完全遮挡的情形）：2.5 s 的保险发信号，且只发一次；
//   ⑤ 页面脚本报错：boot 守卫在首帧之前上报 __rinne__bootError，并且只报第一条。
// 浏览器缺失时整套跳过（退出码 2）。本机同时只能有一个浏览器冒烟，gates 里持 gui 锁。
//
// 用法：node web-preview/tests/smoke-first-frame-page.mjs [仓库根]

import fs from "node:fs";
import path from "node:path";
import { createRun, repoRoot } from "./lib/harness.mjs";
import { cleanupTempDir, makeTempDir } from "../../scripts/lib/safe-rm.mjs";
import { findBrowser, launchBrowser } from "./lib/browser.mjs";
import { startStaticServer } from "./lib/static-server.mjs";

const ROOT = repoRoot();
const run = createRun("首帧信号（页面）");
if (!findBrowser()) run.skip("没有 Chromium 系浏览器");

/** 假的 JUCE 宿主：只收 postMessage，记到 window.__posted。 */
const FAKE_HOST = `
    window.__posted = [];
    window.__JUCE__ = { postMessage(text) { window.__posted.push(JSON.parse(text)); } };
`;

const server = await startStaticServer(ROOT);
const browser = await launchBrowser();
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/** 开一个页面：在文档创建前注入 preamble，再打开 URL。 */
async function open(url, preamble) {
    const page = await browser.newPage();
    await page.send("Page.addScriptToEvaluateOnNewDocument", { source: FAKE_HOST + preamble });
    page.requestLog = await page.recordRequests();
    await page.goto(url);
    return page;
}

/** 页面发出的请求里不同源的那些（data:、blob:、about: 不算网络请求）。 */
const foreignRequests = (page, origin) =>
    page.requestLog.filter((u) => !/^(data|blob|about):/.test(u) && new URL(u).origin !== origin);
const posted = (page, id) => page.eval(`window.__posted.filter((m) => m.eventId === ${JSON.stringify(id)})`);

try {
    for (const role of ["input", "output"]) {
        const url = `${server.url}/web/${role}/index.html`;

        // ① 正常路径
        {
            const page = await open(url, "");
            await page.waitFor(`window.__posted.some((m) => m.eventId === "__rinne__firstFrame")`, {
                label: `${role}：首帧信号`,
            });
            await sleep(300); // 给「重复发」留时间暴露
            const ff = await posted(page, "__rinne__firstFrame");
            run.eq(ff.length, 1, `${role}（正常）：首帧信号恰好一次`);
            run.check(
                Number.isFinite(ff[0]?.payload?.paintDeltaMs) && ff[0].payload.paintDeltaMs >= 0,
                `${role}（正常）：载荷带非负的 paintDeltaMs`,
            );
            run.eq((await posted(page, "__rinne__bootError")).length, 0, `${role}（正常）：不应有 boot 错误`);
            run.eq(
                await page.eval("window.__rinneBooted === true"),
                true,
                `${role}（正常）：首帧发出后 __rinneBooted 置位`,
            );
            run.check(page.errors.length === 0, `${role}（正常）：控制台不应有错误：${page.errors.join("；")}`);
            run.check(page.requestLog.length > 0, `${role}（正常）：应当记录到页面自己的请求（防空转）`);
            run.eq(
                foreignRequests(page, server.url),
                [],
                `${role}（正常）：页面发出的所有请求都必须与服务器同源（本地，无外部请求）`,
            );
            await page.close();
        }

        // ② 没有 PerformanceObserver
        {
            const page = await open(url, "delete window.PerformanceObserver;");
            await page.waitFor(`window.__posted.some((m) => m.eventId === "__rinne__firstFrame")`, {
                label: `${role}：无 PerformanceObserver 时的首帧信号`,
            });
            await sleep(200);
            run.eq(
                (await posted(page, "__rinne__firstFrame")).length,
                1,
                `${role}（无 PerformanceObserver）：回落路发一次`,
            );
            await page.close();
        }

        // ③ 没有 requestAnimationFrame
        {
            const page = await open(url, "window.requestAnimationFrame = undefined;");
            await page.waitFor(`window.__posted.some((m) => m.eventId === "__rinne__firstFrame")`, {
                label: `${role}：无 rAF 时的首帧信号`,
            });
            await sleep(200);
            run.eq((await posted(page, "__rinne__firstFrame")).length, 1, `${role}（无 rAF）：直接发一次`);
            await page.close();
        }

        // ④ rAF 永不回调：只有 2.5 s 的保险能放行
        {
            const t0 = Date.now();
            const page = await open(url, "window.requestAnimationFrame = function () { return 1; };");
            await sleep(1200);
            run.eq((await posted(page, "__rinne__firstFrame")).length, 0, `${role}（rAF 停摆）：保险到点之前不应放行`);
            await page.waitFor(`window.__posted.some((m) => m.eventId === "__rinne__firstFrame")`, {
                timeout: 4000,
                label: `${role}：保险放行`,
            });
            const dt = Date.now() - t0;
            run.check(dt >= 2200 && dt <= 4500, `${role}（rAF 停摆）：保险应在约 2.5 s 放行，实得 ${dt} ms`);
            await sleep(600);
            run.eq((await posted(page, "__rinne__firstFrame")).length, 1, `${role}（rAF 停摆）：保险只发一次`);
            await page.close();
        }

        // ⑤ 页面脚本报错：首帧之前上报，只报第一条
        {
            const page = await open(
                url,
                `window.requestAnimationFrame = function () { return 1; };
                 document.addEventListener("DOMContentLoaded", function () { setTimeout(function () { throw new Error("first-boom"); }, 0); setTimeout(function () { throw new Error("second-boom"); }, 0); });`,
            );
            await page.waitFor(`window.__posted.some((m) => m.eventId === "__rinne__bootError")`, {
                label: `${role}：boot 错误上报`,
            });
            await sleep(200);
            const errs = await posted(page, "__rinne__bootError");
            run.eq(errs.length, 1, `${role}（脚本报错）：只报第一条`);
            run.check(
                errs[0]?.payload?.stage === "script" && String(errs[0].payload.detail).includes("first-boom"),
                `${role}（脚本报错）：阶段与细节应来自第一条错误`,
            );
            await page.close();
        }
    }

    // 坏例：一个会发外部请求的页面，必须被同源断言抓到。页面放在 mkdtemp 出来的临时目录里，请求发往本机一个无人监听的端口，
    // 不会真的出网；同源断言要把它认作外部请求。
    const tmp = makeTempDir("rinne-badpage-");
    try {
        fs.mkdirSync(path.join(tmp, "web-preview"));
        fs.writeFileSync(
            path.join(tmp, "web-preview", "bad.html"),
            // 页面加载完之后才发外部请求：不拖慢本测试等待页面加载完成
            '<!doctype html><meta charset="utf-8"><p>bad</p><script>addEventListener("load", function () { new Image().src = "http://127.0.0.1:9/p.gif"; var s = document.createElement("script"); s.src = "http://127.0.0.1:9/external.js"; document.head.appendChild(s); });</script>',
        );
        const badServer = await startStaticServer(tmp);
        try {
            const page = await open(`${badServer.url}/web-preview/bad.html`, "");
            await sleep(300);
            const offenders = foreignRequests(page, badServer.url);
            run.check(offenders.length >= 2, `坏例：同源断言应当抓到外部请求，实得 ${JSON.stringify(offenders)}`);
            await page.close();
        } finally {
            await badServer.close();
        }
    } finally {
        cleanupTempDir(tmp);
    }
} finally {
    await browser.close();
    await server.close();
}
run.finish();
