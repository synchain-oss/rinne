// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/web-preview.json
//
// web-preview 截图器：无头浏览器加 DevTools 协议，零 npm 依赖。改完页面想看一眼，不该每次都占用人的眼睛：
// 本脚本起一个独立 user-data-dir 的无头实例，打开预览壳页，等 mock 注入，抓 iframe（即插件窗口）的 PNG。
// 它自己起一个临时端口的静态服务器，不需要先跑 serve.ps1；给 --base 则改用已经在跑的服务器。
// 无头浏览器的光栅路径和真机不同，设计验收以真机为准，本脚本用于快速回归和「有没有画出来」。
// 本机同时只能有一个浏览器冒烟：手动连着跑别的页面级冒烟时别同时跑它。
//
// 用法（在仓库根）：
//   node web-preview/shot.mjs --role=input --scenario=in-playing --out=in-playing.png
//   node web-preview/shot.mjs --role=output --scenario=out-fr-worst --lang=fr
//   node web-preview/shot.mjs --role=input --scale=1.25 --dpr=2
//   node web-preview/shot.mjs --role=input --all            每个场景一张，写到 web-preview/.shots/
//   node web-preview/shot.mjs --base=http://127.0.0.1:8833 --role=output
// 选项：--role=input|output（缺省 input）、--scenario、--lang、--scale、--fresh=1、--dpr、--wait=毫秒（缺省 600）、
//       --out=文件（缺省 web-preview/.shots/<角色>-<场景>.png）、--page=probe 打开夹具页、--help。

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { findBrowser, launchBrowser } from "./tests/lib/browser.mjs";
import { startStaticServer } from "./tests/lib/static-server.mjs";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const args = new Map();
for (const raw of process.argv.slice(2)) {
    const m = /^--([^=]+)(?:=([\s\S]*))?$/.exec(raw);
    if (m) args.set(m[1], m[2] === undefined ? "1" : m[2]);
}
const opt = (k, d) => (args.has(k) ? args.get(k) : d);

if (args.has("help")) {
    console.log(
        fs
            .readFileSync(fileURLToPath(import.meta.url), "utf8")
            .split("\n")
            .slice(8, 18)
            .join("\n")
            .replace(/^\/\/ ?/gm, ""),
    );
    process.exit(0);
}

const role = opt("role", "input");
if (role !== "input" && role !== "output") {
    console.error(`--role 只能是 input 或 output，实得 ${role}`);
    process.exit(1);
}
if (!findBrowser()) {
    console.error("找不到 Chromium 系浏览器（可用环境变量 RINNE_BROWSER 指定）");
    process.exit(2);
}

const { SCENES } = await import(pathToFileURL(path.join(root, "web-preview/mock/scenarios.js")).href);
const outDir = path.join(root, "web-preview/.shots");
const jobs = args.has("all")
    ? SCENES.filter((s) => s.role === role).map((s) => ({
          scenario: s.id,
          lang: s.lang,
          file: path.join(outDir, `${s.id}.png`),
      }))
    : [
          {
              scenario: opt("scenario", ""),
              lang: opt("lang", ""),
              file: path.resolve(opt("out", path.join(outDir, `${role}-${opt("scenario", "default")}.png`))),
          },
      ];

const server = args.has("base") ? null : await startStaticServer(root);
const base = opt("base", server?.url);
const browser = await launchBrowser({ width: 1700, height: 1300 });
try {
    const page = await browser.newPage();
    await page.setViewport(1700, 1300, Number(opt("dpr", "1")));
    for (const job of jobs) {
        const q = new URLSearchParams({ fresh: opt("fresh", "1") });
        if (job.scenario) q.set("scenario", job.scenario);
        if (opt("lang", "") || job.lang) q.set("lang", opt("lang", "") || job.lang);
        if (args.has("scale")) q.set("scale", opt("scale"));
        if (args.has("page")) q.set("page", opt("page"));
        await page.goto(`${base}/web-preview/${role}.html?${q}`);
        await page.waitFor(
            `(() => { const s = document.querySelector(".pv-status"); return !!s && !/初始化|正在注入/.test(s.textContent); })()`,
            {
                label: "mock 注入完成",
            },
        );
        await new Promise((r) => setTimeout(r, Number(opt("wait", "600"))));
        const rect = await page.eval(
            `(() => { const r = document.querySelector("iframe").getBoundingClientRect(); return { x: r.x, y: r.y, width: r.width, height: r.height }; })()`,
        );
        const shot = await page.send("Page.captureScreenshot", { format: "png", clip: { ...rect, scale: 1 } });
        fs.mkdirSync(path.dirname(job.file), { recursive: true });
        fs.writeFileSync(job.file, Buffer.from(shot.data, "base64"));
        console.log(
            `${job.scenario || "(默认态)"}：${Math.round(rect.width)}×${Math.round(rect.height)} -> ${path.relative(root, job.file)}`,
        );
    }
    if (page.errors.length) console.error(`页面有 ${page.errors.length} 条错误：\n  ${page.errors.join("\n  ")}`);
} finally {
    await browser.close();
    if (server) await server.close();
}
