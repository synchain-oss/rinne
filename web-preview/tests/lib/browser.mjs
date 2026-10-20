// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/web-preview.json
//
// 无头浏览器 + DevTools 协议，零 npm 依赖：靠两件 Node 内置，fetch（取协议目标列表）和全局 WebSocket（Node 22 起有）。
// 不引入 puppeteer：仓库不带运行时依赖。起的是独立 user-data-dir 的无头实例，不碰用户正在用的浏览器，也不用任何扩展。
// 无头浏览器的光栅路径和真机不同，颜色、字体和亚像素可能有极小差异；本库用来回归和「有没有画出来」，设计验收以真机为准。
// 本机同时只能有一个浏览器冒烟：调用方在 gates 里持 gui 锁；手动跑 run-all 时自己别并行。

import { spawn } from "node:child_process";
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
 * 启动无头浏览器。返回 { newPage(), close() }。
 * @param {object} [o] width、height：初始视口。
 */
export async function launchBrowser({ width = 1280, height = 900 } = {}) {
    const exe = findBrowser();
    if (!exe) throw new Error("找不到 Chromium 系浏览器（可用环境变量 RINNE_BROWSER 指定）");
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
    let port = 0;
    const portFile = path.join(profile, "DevToolsActivePort");
    for (let i = 0; i < 100 && !port; i++) {
        await sleep(100);
        try {
            port = Number(fs.readFileSync(portFile, "utf8").split("\n")[0]);
        } catch {
            // 还没写出来
        }
    }
    const cleanup = async () => {
        try {
            proc.kill();
        } catch {
            // 已经退出
        }
        await sleep(300);
        for (let i = 0; i < 5; i++) {
            if (removeTempDir(profile) === null) return;
            await sleep(300); // 浏览器进程还握着文件：等一等再删
        }
    };
    if (!port) {
        await cleanup();
        throw new Error("浏览器没有在 10 秒内开出调试端口");
    }
    const pages = [];
    return {
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
