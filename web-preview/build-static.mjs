// SPDX-License-Identifier: GPL-3.0-or-later
//
// 把预览导出成一棵可以丢到任意静态服务器上的目录：web/ 与 web-preview/ 的运行时文件，保持仓库里的相对布局，
// 外加一个根 index.html 跳到 Input 预览页。用于不方便跑 serve.ps1 的场合（别人本机看、挂在静态托管上看）。
// 纯拷贝，不改任何文件内容，所以页面与 mock 仍是纯本地：不发任何网络请求，不依赖任何框架。
// 导出之后自检这棵树：任何 http(s) 外链、任何网络调用（fetch、XMLHttpRequest、WebSocket、sendBeacon、EventSource）都判红，
// 放行的只有两种字面量：SVG 命名空间和 JUCE 辅助脚本的宿主伪源（都不是网络请求）。
//   node web-preview/build-static.mjs [--out <目录>] [--check]
// --out 缺省为 build/web-preview-static（build/ 已被忽略）。--check 只自检不写文件（自检对象是仓库里的源树）。
// 退出码：0 通过；1 有外链或网络调用，或拷贝失败。

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const argv = process.argv.slice(2);
const outDir = path.resolve(
    root,
    argv.includes("--out") ? argv[argv.indexOf("--out") + 1] : "build/web-preview-static",
);

/** 进入导出的目录与文件：web/ 全部，web-preview/ 去掉测试、截图、脚本。 */
const INCLUDE = [
    { dir: "web", skip: () => false },
    {
        dir: "web-preview",
        skip: (rel) => /^(tests|\.shots)(\/|$)/.test(rel) || /\.(ps1|mjs)$/.test(rel) || rel === "README.md",
    },
];

/** 可以出现 http(s) 字面量的两种情形：SVG 命名空间（标识符，不是请求）；JUCE 前端辅助脚本里宿主资源的伪源（原生侧拦截，不出网）。 */
const ALLOWED_URL = /^https?:\/\/(?:www\.w3\.org\/2000\/svg|juce\.backend\/?)$/;
const NETWORK_CALLS = /\b(?:fetch|sendBeacon)\s*\(|\bXMLHttpRequest\b|\bnew\s+(?:WebSocket|EventSource)\b/;

function* walk(dirAbs, relBase) {
    for (const name of fs.readdirSync(dirAbs).sort()) {
        const abs = path.join(dirAbs, name);
        const rel = `${relBase}/${name}`;
        if (fs.statSync(abs).isDirectory()) yield* walk(abs, rel);
        else yield { abs, rel };
    }
}

/** 一段文本里的问题。 */
export function textProblems(rel, text) {
    const out = [];
    const bare = text
        .replace(/<!--[\s\S]*?-->/g, "")
        .replace(/\/\*[\s\S]*?\*\//g, "")
        .replace(/^\s*\/\/.*$/gm, "");
    for (const m of bare.matchAll(/https?:\/\/[^\s"'`)<>]+/g)) {
        if (!ALLOWED_URL.test(m[0])) out.push(`${rel}：外链 ${m[0]}`);
    }
    if (/\.(js|mjs|html)$/.test(rel) && !rel.startsWith("web/js/juce/") && NETWORK_CALLS.test(bare)) {
        out.push(`${rel}：出现网络调用`);
    }
    return out;
}

/** 自检一组 { rel, abs }，返回问题数组。 */
export function verify(files) {
    const out = [];
    for (const f of files) {
        if (!/\.(html|js|css)$/.test(f.rel)) continue;
        out.push(...textProblems(f.rel, fs.readFileSync(f.abs, "utf8")));
    }
    return out;
}

const files = [];
for (const { dir, skip } of INCLUDE) {
    for (const f of walk(path.join(root, dir), dir)) {
        if (!skip(path.relative(path.join(root, dir), f.abs).split(path.sep).join("/"))) files.push(f);
    }
}

const problems = verify(files);
if (!argv.includes("--check")) {
    fs.rmSync(outDir, { recursive: true, force: true });
    for (const f of files) {
        const dest = path.join(outDir, f.rel);
        fs.mkdirSync(path.dirname(dest), { recursive: true });
        fs.copyFileSync(f.abs, dest);
    }
    fs.writeFileSync(
        path.join(outDir, "index.html"),
        '<!doctype html>\n<meta charset="utf-8" />\n<meta http-equiv="refresh" content="0; url=web-preview/input.html" />\n<a href="web-preview/input.html">Rinne Input 预览</a>\n',
    );
    const bytes = files.reduce((n, f) => n + fs.statSync(f.abs).size, 0);
    console.log(
        `导出 ${files.length} 个文件（${(bytes / 1024 / 1024).toFixed(1)} MB）到 ${path.relative(root, outDir) || "."}`,
    );
}
for (const p of problems) console.error(`  ${p}`);
console.log(
    `build-static：${problems.length ? `${problems.length} 处外链或网络调用` : "自检通过，纯本地、无网络请求"}`,
);
process.exitCode = problems.length ? 1 : 0;
