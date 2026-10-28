// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/web-preview.json
//
// 无头浏览器 + DevTools 协议，零 npm 依赖：靠两件 Node 内置，fetch（取协议目标列表）和全局 WebSocket（Node 22 起有）。
// 不引入 puppeteer：仓库不带运行时依赖。起的是独立 user-data-dir 的无头实例，不碰用户正在用的浏览器，也不用任何扩展。
// 无头浏览器的光栅路径和真机不同，颜色、字体和亚像素可能有极小差异；本库用来回归和「有没有画出来」，设计验收以真机为准。
// 本机同时只能有一个浏览器冒烟：调用方在 gates 里持 gui 锁；手动跑 run-all 时自己别并行。

import { spawn, spawnSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { makeTempDir, removeTempDir } from "../../../scripts/lib/safe-rm.mjs";

const CANDIDATES = [
    process.env.RINNE_BROWSER,
    "C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
    "C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe",
    "C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe",
    "C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe",
    "/usr/bin/google-chrome",
    "/usr/bin/chromium",
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
    "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
];

/** 找一个可用的 Chromium 系浏览器；找不到返回 null。 */
export function findBrowser() {
    return CANDIDATES.find((p) => p && fs.existsSync(p)) || null;
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

/**
 * 开调试端口的有界重试。无头浏览器偶尔在负载下 10 秒内写不出 DevToolsActivePort（CI 与本机都见过），这一次没起来不等于页面坏了，
 * 所以换一个新 profile 重起；但次数与总时长都有上界，重试完仍没起来照常抛错判红，不改成跳过：页面冒烟是 required check。
 * attempts：最多起几次（含第一次）；waitMs：每次等端口文件多久；backoffMs：第 k 次失败后等多久再起（不够长就重复最后一项）；
 * totalMs：从第一次起算的总时长上界，下一次起不完就不再起。
 */
export const LAUNCH_POLICY = Object.freeze({ attempts: 3, waitMs: 10000, backoffMs: [1000, 2000], totalMs: 45000 });

/**
 * 按 policy 有界重试 attempt(k)（k 从 1 起）：返回非 null 就算起来了，返回 null 就退避再起。返回 { value, attempts, retries, ms }；
 * 次数用完或总时长不够再起一次时抛错，错误里写明起了几次、重试了几次、用了多久（err.launchAttempts 是起的次数）。
 * onRetry(k, pauseMs) 在每次退避之前调用（打日志用）；now、wait 可以注入假时钟（自测用）。
 */
export async function withLaunchRetry(
    attempt,
    policy = LAUNCH_POLICY,
    { now = Date.now, wait = sleep, onRetry = () => {} } = {},
) {
    const t0 = now();
    for (let k = 1; ; k++) {
        const value = await attempt(k);
        if (value != null) return { value, attempts: k, retries: k - 1, ms: now() - t0 };
        const pause = policy.backoffMs[Math.min(k - 1, policy.backoffMs.length - 1)] ?? 0;
        const spent = now() - t0;
        if (k >= policy.attempts || spent + pause + policy.waitMs > policy.totalMs) {
            const err = new Error(
                `浏览器起了 ${k} 次，每次都没在 ${policy.waitMs / 1000} 秒内开出调试端口（重试 ${k - 1} 次，共 ${(spent / 1000).toFixed(1)} 秒）`,
            );
            err.launchAttempts = k;
            throw err;
        }
        onRetry(k, pause);
        await wait(pause);
    }
}

/** 一个页面：eval、goto、等条件、收集错误。 */
class Page {
    constructor(ws, targetId, port) {
        this.ws = ws;
        this.targetId = targetId;
        this.port = port;
        this.errors = [];
        this.id = 0;
        this.pending = new Map();
        ws.addEventListener("message", (ev) => {
            const m = JSON.parse(ev.data);
            if (m.method && this.requests && m.method === "Network.requestWillBeSent")
                this.requests.push(m.params.request.url);
            if (m.id && this.pending.has(m.id)) {
                this.pending.get(m.id)(m);
                this.pending.delete(m.id);
            } else if (m.method === "Runtime.exceptionThrown") {
                const d = m.params.exceptionDetails;
                this.errors.push(`异常：${d.exception?.description || d.text}`);
            } else if (m.method === "Runtime.consoleAPICalled" && m.params.type === "error") {
                this.errors.push(`console.error：${m.params.args.map((a) => a.value ?? a.description).join(" ")}`);
            } else if (m.method === "Log.entryAdded" && m.params.entry.level === "error") {
                this.errors.push(`日志：${m.params.entry.text} ${m.params.entry.url || ""}`);
            }
        });
    }

    send(method, params = {}) {
        return new Promise((resolve, reject) => {
            const id = ++this.id;
            const timer = setTimeout(() => reject(new Error(`CDP ${method} 超时`)), 30000);
            this.pending.set(id, (m) => {
                clearTimeout(timer);
                if (m.error) reject(new Error(`CDP ${method}：${m.error.message}`));
                else resolve(m.result);
            });
            this.ws.send(JSON.stringify({ id, method, params }));
        });
    }

    async init(width, height) {
        await this.send("Page.enable");
        await this.send("Runtime.enable");
        await this.send("Log.enable");
        await this.setViewport(width, height);
    }

    setViewport(width, height, deviceScaleFactor = 1) {
        return this.send("Emulation.setDeviceMetricsOverride", { width, height, deviceScaleFactor, mobile: false });
    }

    async goto(url) {
        await this.send("Page.navigate", { url });
        await this.waitFor("document.readyState === 'complete'", { timeout: 20000 });
    }

    /** 在页面里求值（可返回 Promise），按值返回；页面抛异常时这里也抛。 */
    async eval(expression) {
        const r = await this.send("Runtime.evaluate", { expression, awaitPromise: true, returnByValue: true });
        if (r.exceptionDetails) {
            throw new Error(`页面求值失败：${r.exceptionDetails.exception?.description || r.exceptionDetails.text}`);
        }
        return r.result.value;
    }

    /** 轮询到表达式为真；超时抛错，带最后一次的值。 */
    async waitFor(expression, { timeout = 10000, step = 40, label = expression } = {}) {
        const t0 = Date.now();
        let last;
        for (;;) {
            try {
                last = await this.eval(expression);
            } catch (e) {
                last = String(e.message);
            }
            if (last === true || (last && typeof last !== "string")) return last;
            if (Date.now() - t0 > timeout)
                throw new Error(`等待超时（${timeout} ms）：${label}；最后读到 ${JSON.stringify(last)}`);
            await sleep(step);
        }
    }

    /** 截图，只返回 PNG 字节；写文件由调用方负责（且只能写到它自己限定的目录）。 */
    async screenshot() {
        const r = await this.send("Page.captureScreenshot", { format: "png" });
        return Buffer.from(r.data, "base64");
    }

    /** 开始记录页面发出的所有网络请求（Network.requestWillBeSent），返回会持续增长的 URL 数组。 */
    async recordRequests() {
        this.requests = [];
        await this.send("Network.enable");
        return this.requests;
    }

    async close() {
        try {
            this.ws.close();
            await fetch(`http://127.0.0.1:${this.port}/json/close/${this.targetId}`).catch(() => {});
        } catch {
            // 已经关了
        }
    }
}

/**
 * 起一次浏览器：新的临时 profile，等 waitMs 看它写不写出调试端口。写出来了返回 { port, cleanup }；没写出来就收掉这一次的进程树与
 * profile，返回 null（由 withLaunchRetry 决定还起不起）。
 */
async function launchOnce(exe, waitMs) {
    const profile = makeTempDir("rinne-pv-");
    const proc = spawn(
        exe,
        [
            "--headless=new",
            "--disable-gpu",
            "--hide-scrollbars",
            "--no-first-run",
            "--no-default-browser-check",
            "--disable-extensions",
            "--remote-debugging-port=0",
            `--user-data-dir=${profile}`,
            "about:blank",
        ],
        { stdio: "ignore" },
    );
    // 起不起来（可执行文件坏了之类）不让未处理的 error 事件打崩进程：这一次算没开出端口，不再干等
    let spawnError = null;
    proc.once("error", (e) => {
        spawnError = e;
    });
    let port = 0;
    const portFile = path.join(profile, "DevToolsActivePort");
    for (let i = 0; i < waitMs / 100 && !port && !spawnError; i++) {
        await sleep(100);
        try {
            port = Number(fs.readFileSync(portFile, "utf8").split("\n")[0]);
        } catch {
            // 还没写出来
        }
    }
    const cleanup = async () => {
        try {
            // Windows 上 Chromium 还有 GPU / 工具子进程握着 profile 里的文件：只杀主进程，子进程一直不放，目录就删不掉；连子进程一起结束
            if (process.platform === "win32" && proc.pid)
                spawnSync("taskkill", ["/pid", String(proc.pid), "/T", "/F"], { stdio: "ignore" });
            proc.kill();
        } catch {
            // 已经退出
        }
        await sleep(300);
        let why;
        for (let i = 0; i < 20; i++) {
            why = removeTempDir(profile);
            if (why === null) return;
            await sleep(300); // 浏览器进程还握着文件：等一等再删
        }
        console.error(`浏览器的临时 profile 没清掉：${why}`);
    };
    // 主进程被强杀（超时）时 finally 不会执行；正常退出和未捕获异常时 exit 事件还能尽力清一次
    process.once("exit", () => {
        removeTempDir(profile);
    });
    if (!port) {
        await cleanup();
        if (spawnError) console.error(`[浏览器] 这一次没起来：${spawnError.message}`);
        return null;
    }
    return { port, cleanup };
}

/**
 * 启动无头浏览器。返回 { newPage(), close(), launch }；launch 是 { attempts, retries, ms }：第几次起来的、重试了几次、用了多久。
 * 开调试端口按 LAUNCH_POLICY 有界重试，每次重试与「重试之后才起来」都往标准错误写一行（run-all 会把它当作这一套的最后一行显示），
 * 便于与 CI 上的偶发对拍；重试完仍没起来照常抛错。
 * @param {object} [o] width、height：初始视口。
 */
export async function launchBrowser({ width = 1280, height = 900 } = {}) {
    const exe = findBrowser();
    if (!exe) throw new Error("找不到 Chromium 系浏览器（可用环境变量 RINNE_BROWSER 指定）");
    const got = await withLaunchRetry(() => launchOnce(exe, LAUNCH_POLICY.waitMs), LAUNCH_POLICY, {
        onRetry: (k, pause) =>
            console.error(
                `[浏览器] 第 ${k} 次启动没在 ${LAUNCH_POLICY.waitMs / 1000} 秒内开出调试端口，${pause / 1000} 秒后重起（最多起 ${LAUNCH_POLICY.attempts} 次）`,
            ),
    });
    const { port, cleanup } = got.value;
    const launch = { attempts: got.attempts, retries: got.retries, ms: got.ms };
    if (launch.retries)
        console.error(
            `[浏览器] 调试端口第 ${launch.attempts} 次启动才开出（重试 ${launch.retries} 次，共 ${(launch.ms / 1000).toFixed(1)} 秒）`,
        );
    const pages = [];
    return {
        launch,
        async newPage() {
            const t = await (await fetch(`http://127.0.0.1:${port}/json/new?about:blank`, { method: "PUT" })).json();
            const ws = new WebSocket(t.webSocketDebuggerUrl);
            await new Promise((resolve, reject) => {
                ws.addEventListener("open", resolve, { once: true });
                ws.addEventListener("error", () => reject(new Error("连不上调试端口")), { once: true });
            });
            const page = new Page(ws, t.id, port);
            await page.init(width, height);
            pages.push(page);
            return page;
        },
        async close() {
            for (const p of pages) await p.close();
            await cleanup();
        },
    };
}
