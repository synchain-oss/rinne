// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/web-preview.json
//
// 首帧信号与 boot 守卫，页面级冒烟（无头浏览器）。静态面（事件名、结构）由 smoke-embedded-resources 钉住，
// 这里钉行为：在每个角色的真源页面里装一个假的 window.__JUCE__.postMessage 收信号，
//   ① 正常路径：页面画出第一帧之后恰好发一次 __rinne__firstFrame，载荷带非负的 paintDeltaMs，随后 __rinneBooted 置位，
//      期间没有 boot 错误，也没有控制台错误（缺字体、缺样式都会在这里露出来）；
//   ② 没有 PerformanceObserver：回落到 DOMContentLoaded 触发，不许变成永不放行；
//   ③ 没有 requestAnimationFrame：直接发信号；
//   ④ rAF 永不回调（合成停摆，窗口最小化或被完全遮挡的情形）：2.5 s 的保险发信号，且只发一次。放行时刻的窗口 [2200, 4500] ms
//      量的是「从导航开始到页面把信号交给宿主」，用页内时钟（performance.now，起点是 performance.timeOrigin）：假宿主收消息时
//      当场打上时间戳（__at）。不在测试进程里用墙钟量：那样会把 CDP 开页、加载等待的耗时也算进窗口，本机忙时能多出几秒；
//   ⑤ 页面脚本报错：boot 守卫在首帧之前上报 __rinne__bootError，并且只报第一条；
//   ⑥ 只有 postMessage 的假 __JUCE__、没有 mock（纯浏览器直接打开、真宿主桥没接上的情形）：页面要么干净地起来
//      （首帧恰好一次、__rinneBooted 置位、没有 boot 错误），要么响亮地失败（boot 错误上报），不许既没起来也不吭声；
//   ⑦ 主模块一上来就抛错（注入一个残缺的 __RINNE_MOCK__，createBridge 会抛）：boot 守卫上报恰好一条，__rinneBooted 不置位。
//      占位页没有主模块，这一项自动跳过，页面接入主模块后自动生效。
// ①–⑤ 的启动环境是「假宿主 + 一个最小的 __RINNE_MOCK__」：真页面要调 createBridge()，没有桥就抛错，这几项测的是首帧信号，
// 不该因为页面没桥而红；没有 mock 的那条路由 ⑥ 单独覆盖。
// 浏览器缺失时整套跳过（退出码 2）。本机同时只能有一个浏览器冒烟，gates 里持 gui 锁。
// 浏览器开调试端口有界重试（lib/browser.mjs 的 withLaunchRetry）：这套曾因「10 秒内没开出调试端口」偶发失败。开头先不起浏览器，
// 用假的启动与假时钟核对重试的次数、退避、总时长上界与「重试完仍失败照常抛错」；这几条没有浏览器也照跑，失败时不被整套跳过盖掉。
// 起浏览器之后把「重试了几次」记进输出，便于与 CI 的偶发对拍。
//
// 用法：node web-preview/tests/smoke-first-frame-page.mjs [仓库根]

import fs from "node:fs";
import path from "node:path";
import { createRun, repoRoot } from "./lib/harness.mjs";
import { cleanupTempDir, makeTempDir } from "../../scripts/lib/safe-rm.mjs";
import { findBrowser, launchBrowser, LAUNCH_POLICY, withLaunchRetry } from "./lib/browser.mjs";
import { startStaticServer } from "./lib/static-server.mjs";

const ROOT = repoRoot();
const run = createRun("首帧信号（页面）");

// 有界重试：attempt 第 okAt 次才「起来」（null 表示这次没开出端口），每次没起来按 waitMs 推进假时钟
async function retryCase(okAt, policy) {
    let t = 0;
    const calls = [];
    const pauses = [];
    const attempt = async (k) => {
        calls.push(k);
        if (k === okAt) return 9222;
        t += policy.waitMs;
        return null;
    };
    const clock = { now: () => t, wait: async (ms) => void (pauses.push(ms), (t += ms)) };
    try {
        return { ok: await withLaunchRetry(attempt, policy, clock), calls, pauses };
    } catch (e) {
        return { err: e, calls, pauses };
    }
}
{
    // 下面三条用的就是真在用的 LAUNCH_POLICY：先钉住它是「重试 2 次、退避 1 秒与 2 秒、每次等 10 秒、总时长 45 秒封顶」
    const policy = LAUNCH_POLICY;
    let unitOk = run.eq(
        [policy.attempts, policy.waitMs, policy.backoffMs, policy.totalMs],
        [3, 10000, [1000, 2000], 45000],
        "有界重试：LAUNCH_POLICY 是起 3 次、每次等 10 秒、退避 1 秒与 2 秒、总时长 45 秒封顶",
    );
    const first = await retryCase(1, policy);
    unitOk =
        run.eq(
            [first.ok?.retries, first.calls, first.pauses],
            [0, [1], []],
            "有界重试：第一次就起来，不重试、不退避",
        ) && unitOk;
    const third = await retryCase(3, policy);
    unitOk =
        run.eq(
            [third.ok?.value, third.ok?.retries, third.calls, third.pauses],
            [9222, 2, [1, 2, 3], [1000, 2000]],
            "有界重试：第三次才起来，重试 2 次、两次退避逐次加长",
        ) && unitOk;
    const never = await retryCase(0, policy);
    unitOk =
        run.check(
            never.err &&
                never.err.launchAttempts === 3 &&
                /重试 2 次/.test(never.err.message) &&
                never.calls.length === 3,
            `有界重试：一直没起来，起满 3 次后抛错（写明重试了几次），不返回、不跳过；实得 ${never.err?.message ?? "没有抛错"}`,
        ) && unitOk;
    const capped = await retryCase(0, { attempts: 10, waitMs: 10000, backoffMs: [1000], totalMs: 25000 });
    unitOk =
        run.check(
            capped.err && capped.calls.length === 2,
            `有界重试：总时长上界 25 秒、每次等 10 秒时只起 2 次，不会起满 10 次；实得起了 ${capped.calls.length} 次`,
        ) && unitOk;
    if (!findBrowser()) {
        if (!unitOk) {
            run.finish();
            process.exit();
        }
        run.skip("没有 Chromium 系浏览器");
    }
}

/** 假的 JUCE 宿主：只收 postMessage，记到 window.__posted；每条带上收到时的页内时钟 __at（performance.now()，起点是 performance.timeOrigin）。 */
const FAKE_HOST = `
    window.__posted = [];
    window.__JUCE__ = { postMessage(text) { window.__posted.push(Object.assign(JSON.parse(text), { __at: performance.now() })); } };
`;

/** 最小的 mock 后端：addEventListener 是空操作，其余任何名字都是回「没准备好」的异步函数（createBridge 只校名，不校行为）。 */
const MOCK = `
    window.__RINNE_MOCK__ = new Proxy({ addEventListener() {} }, {
        get: (target, key) => (key in target ? target[key] : async () => ({ ok: false, reason: "not-ready" })),
    });
`;

const server = await startStaticServer(ROOT);
let browser;
try {
    browser = await launchBrowser();
} catch (e) {
    await server.close();
    throw e; // 重试完仍没起来：照常判红（退出码 1），不跳过
}
run.log(
    `  浏览器调试端口：第 ${browser.launch.attempts} 次启动开出（重试 ${browser.launch.retries} 次，${(browser.launch.ms / 1000).toFixed(1)} 秒）`,
);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/** 开一个页面：在文档创建前注入假宿主（可选再加最小 mock）与 preamble，再打开 URL。 */
async function open(url, preamble, { mock = true } = {}) {
    const page = await browser.newPage();
    await page.send("Page.addScriptToEvaluateOnNewDocument", { source: FAKE_HOST + (mock ? MOCK : "") + preamble });
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
            const page = await open(url, "window.requestAnimationFrame = function () { return 1; };");
            await sleep(1200);
            run.eq((await posted(page, "__rinne__firstFrame")).length, 0, `${role}（rAF 停摆）：保险到点之前不应放行`);
            await page.waitFor(`window.__posted.some((m) => m.eventId === "__rinne__firstFrame")`, {
                timeout: 4000,
                label: `${role}：保险放行`,
            });
            // 放行时刻取页内时钟（导航开始起算），不含测试进程开页、等加载的耗时；窗口数值不随量法改
            const dt = (await posted(page, "__rinne__firstFrame"))[0]?.__at;
            run.check(
                Number.isFinite(dt) && dt >= 2200 && dt <= 4500,
                `${role}（rAF 停摆）：保险应在约 2.5 s 放行（页内时钟，自导航开始起），实得 ${dt} ms`,
            );
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

        // ⑥ 只有 postMessage 的假 __JUCE__、没有 mock：要么干净起来，要么响亮失败，不许悄无声息
        {
            const page = await open(url, "", { mock: false });
            await page
                .waitFor(
                    `window.__rinneBooted === true || window.__posted.some((m) => m.eventId === "__rinne__bootError")`,
                    { timeout: 4000, label: `${role}：没有 mock 时页面既没起来也没上报` },
                )
                .catch(() => {});
            await sleep(300);
            const booted = await page.eval("window.__rinneBooted === true");
            const errs = await posted(page, "__rinne__bootError");
            const firsts = await posted(page, "__rinne__firstFrame");
            run.check(
                (booted && errs.length === 0 && firsts.length === 1) || errs.length >= 1,
                `${role}（没有 mock）：要么干净起来（首帧一次、__rinneBooted 置位、无 boot 错误），要么上报 boot 错误；实得 booted=${booted}、boot 错误 ${errs.length} 条、首帧 ${firsts.length} 次`,
            );
            await page.close();
        }

        // ⑦ 主模块一上来就抛错：残缺的 __RINNE_MOCK__（没有 addEventListener）让 createBridge 抛错
        {
            const html = fs.readFileSync(path.join(ROOT, "web", role, "index.html"), "utf8");
            if (!/<script[^>]+type="module"/.test(html)) {
                // 占位页没有主模块，没有可抛的东西；这里只打一行记录（不用跳过项：套内跳过会让 gates 的 G9-web 变 SKIP），页面接入主模块后这一项自动生效
                run.log(`  ${role}（主模块抛错）：占位页没有主模块，本项不适用`);
            } else {
                const page = await open(url, "window.__RINNE_MOCK__ = {};", { mock: false });
                await page
                    .waitFor(`window.__posted.some((m) => m.eventId === "__rinne__bootError")`, {
                        timeout: 4000,
                        label: `${role}：主模块抛错应被 boot 守卫上报`,
                    })
                    .catch(() => {});
                await sleep(300);
                const errs = await posted(page, "__rinne__bootError");
                run.eq(errs.length, 1, `${role}（主模块抛错）：boot 守卫恰好上报一条`);
                run.eq(
                    await page.eval("window.__rinneBooted === true"),
                    false,
                    `${role}（主模块抛错）：__rinneBooted 不应置位`,
                );
                await page.close();
            }
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
