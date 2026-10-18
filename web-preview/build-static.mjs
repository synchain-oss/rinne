// SPDX-License-Identifier: GPL-3.0-or-later
//
// 把预览导出成一棵可以丢到任意静态服务器上的目录：web/ 与 web-preview/ 的运行时文件，保持仓库里的相对布局，
// 外加一个根 index.html 跳到 Input 预览页。用于不方便跑 serve.ps1 的场合（别人本机看、挂在静态托管上看）。
// 纯拷贝，不改任何文件内容，所以页面与 mock 仍是纯本地：不发任何网络请求，不依赖任何框架。
// 导出之后自检这棵树：任何 http(s) 外链、任何网络调用（fetch、XMLHttpRequest、WebSocket、sendBeacon、EventSource）都判红，
// 放行的只有两种字面量：SVG 命名空间和 JUCE 辅助脚本的宿主伪源（都不是网络请求）。
//   node web-preview/build-static.mjs [--check]
//
// 输出目录是写死的：<仓库根>/build/web-preview-static，仓库根由本脚本自身的路径推出；没有 --out。
// 只接受 --check，没有任何选项能改输出位置。
// 脚本绝不删除调用方给的任意路径。清空目标目录前要全部满足：
//   1. 目标的真实路径（realpath；目标不存在时先对已存在的上级取 realpath 再拼回去，符号链接与 junction 因此无处藏身）
//      恰好等于 realpath(<仓库根>)/build/web-preview-static（build/ 被换成 junction 也拒），且不是仓库根、它的上级或盘根；
//   2. 目标若已存在，必须含本脚本上次写下的标记文件 .rinne-build-static，否则拒绝（不删，退出 1）；
//   3. 目标本身不能是符号链接或 junction（lstat 判，悬空的也算）。
// 导出时先写标记文件再拷内容；源树自检不过就不导出。
// 判定是纯函数 cleanVerdict，测试只传路径字符串。删除只针对这个目标目录，不用 force 吞错。
// --check 只自检仓库里的源树，不写任何文件。
// 退出码：0 通过；1 有外链或网络调用、守卫拒绝，或拷贝失败。

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

/** 固定输出目录相对仓库根的位置，和标记文件名。 */
export const OUT_REL = ["build", "web-preview-static"];
export const MARKER = ".rinne-build-static";

const isUnder = (base, p) => {
    const rel = path.relative(base, p);
    return rel !== "" && !rel.startsWith("..") && !path.isAbsolute(rel);
};
const samePath = (a, b) => path.relative(a, b) === "";

/**
 * 清空目标目录前的判定，纯函数：只看传进来的路径字符串与两个事实，不碰文件系统。
 * @param {object} i
 * @param {string} i.rootReal 仓库根的真实路径
 * @param {string} i.baseReal 固定输出目录的真实路径（不存在时是已存在的上级的真实路径拼回去）
 * @param {string} i.targetReal 目标目录的真实路径（同上）
 * @param {boolean} i.exists 目标目录是否已存在
 * @param {boolean} i.hasMarker 目标目录里有没有标记文件
 * @param {boolean} i.targetIsLink 目标本身是不是符号链接或 junction（lstat 判，悬空的也算）
 * @returns {{ok:boolean, reason:string}}
 */
export function cleanVerdict({ rootReal, baseReal, targetReal, exists, hasMarker, targetIsLink = false }) {
    const no = (reason) => ({ ok: false, reason });
    for (const [k, v] of Object.entries({ rootReal, baseReal, targetReal })) {
        if (typeof v !== "string" || v === "" || !path.isAbsolute(v)) return no(`${k} 必须是非空的绝对路径`);
    }
    if (!samePath(baseReal, path.join(rootReal, ...OUT_REL))) {
        return no(
            `固定输出目录的真实路径 ${baseReal} 不在仓库根真实路径下的 ${OUT_REL.join("/")}（符号链接或 junction？）`,
        );
    }
    if (targetIsLink) return no("目标本身是符号链接或 junction（含悬空的），拒绝导出");
    if (!samePath(baseReal, targetReal)) {
        return no(`目标的真实路径 ${targetReal} 不是固定输出目录 ${baseReal}`);
    }
    if (
        samePath(targetReal, rootReal) ||
        isUnder(targetReal, rootReal) ||
        samePath(targetReal, path.parse(targetReal).root)
    ) {
        return no(`目标 ${targetReal} 是仓库根、它的上级或盘根`);
    }
    if (exists && !hasMarker) return no(`目标目录已存在却没有标记文件 ${MARKER}，不是本脚本导出的目录`);
    return { ok: true, reason: "" };
}

/** 对可能不存在的路径取真实路径：存在就 realpath，不存在就对上级递归后拼回去。 */
export function realpathLoose(p) {
    if (fs.existsSync(p)) return fs.realpathSync(p);
    const parent = path.dirname(p);
    if (parent === p) return p;
    return path.join(realpathLoose(parent), path.basename(p));
}

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
/**
 * 网络调用：fetch、sendBeacon 等只要出现这个词就判红（不要求带括号：`const f = window.fetch; f()` 这类间接调用也拦住），
 * 另外 XMLHttpRequest、WebSocket、EventSource、各类 Worker 与 importScripts。静态扫描挡不住运行时拼出来的名字，
 * 那一层由 first-frame-page 冒烟用 Network.requestWillBeSent 断言所有请求同源来兜。
 */
const NETWORK_CALLS = /\b(?:fetch|sendBeacon|XMLHttpRequest|WebSocket|EventSource|importScripts|Worker|SharedWorker)\b/;

/**
 * 协议相对地址（//host/…）：属性、url()、@import、srcset 多地址、meta refresh 与 JS 字符串里的形态。
 * 盲区：反斜杠写法、HTML 实体写法、运行时拼出来的名字或地址，静态扫描不处理，由页面冒烟里
 * Network.requestWillBeSent 的同源断言兜底。
 */
const PROTOCOL_RELATIVE = [
    /\b(?:src|href|action|poster|formaction|srcset|data)\s*=\s*["']?\s*\/\//i,
    /url\(\s*["']?\s*\/\//i,
    /@import\s+(?:url\(\s*)?["']?\s*\/\//i,
    // srcset 逗号后的第二个及之后的地址
    /\bsrcset\s*=\s*["'][^"']*,\s*\/\//i,
    // <meta http-equiv="refresh" content="0;url=//…">：分隔符可以是 ; 或 ,，url= 前缀可省略
    /\bcontent\s*=\s*["']\s*[\d.]*\s*[;,]?\s*(?:url\s*=\s*)?["']?\s*\/\//i,
    /["'`]\s*\/\/[A-Za-z0-9][A-Za-z0-9-]*(?:\.[A-Za-z0-9-]+)+/,
];

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
    // web/js/juce/ 是 JUCE 官方原样副本，只查上面的 http(s) 字面量
    if (rel.startsWith("web/js/juce/")) return out;
    for (const re of PROTOCOL_RELATIVE) {
        const m = re.exec(bare);
        if (m) out.push(`${rel}：协议相对地址 ${m[0].trim()}`);
    }
    if (/\.(js|mjs|html)$/.test(rel)) {
        const m = NETWORK_CALLS.exec(bare);
        if (m) out.push(`${rel}：出现网络调用 ${m[0]}`);
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

function main() {
    const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
    const argv = process.argv.slice(2);
    let check = false;
    for (let i = 0; i < argv.length; i++) {
        if (argv[i] === "--check") check = true;
        else {
            console.error(
                `build-static：不认识的参数 ${argv[i]}（没有 --out：输出目录是写死的 ${OUT_REL.join("/")}，只接受 --check）`,
            );
            process.exit(1);
        }
    }

    const files = [];
    for (const { dir, skip } of INCLUDE) {
        for (const f of walk(path.join(root, dir), dir)) {
            if (!skip(path.relative(path.join(root, dir), f.abs).split(path.sep).join("/"))) files.push(f);
        }
    }
    const problems = verify(files);

    // 源树自检不过就不导出：不往输出目录写一棵有问题的树
    if (!check && !problems.length) {
        const target = path.join(root, ...OUT_REL);
        let st = null;
        try {
            st = fs.lstatSync(target); // lstat 不跟链接：悬空的符号链接或 junction 也能看见
        } catch {
            st = null;
        }
        const exists = st !== null;
        const verdict = cleanVerdict({
            rootReal: fs.realpathSync(root),
            baseReal: realpathLoose(target),
            targetReal: realpathLoose(target),
            exists,
            hasMarker: exists && fs.existsSync(path.join(target, MARKER)),
            targetIsLink: st !== null && st.isSymbolicLink(),
        });
        if (!verdict.ok) {
            console.error(`build-static：拒绝清空输出目录：${verdict.reason}`);
            process.exit(1);
        }
        if (exists) fs.rmSync(target, { recursive: true });
        // 先写标记再拷内容：中途失败也不会留下没有标记、之后再也清不掉的目录
        fs.mkdirSync(target, { recursive: true });
        fs.writeFileSync(
            path.join(target, MARKER),
            "本目录由 web-preview/build-static.mjs 生成，可以被它整个清空重建。\n",
        );
        for (const f of files) {
            const dest = path.join(target, f.rel);
            fs.mkdirSync(path.dirname(dest), { recursive: true });
            fs.copyFileSync(f.abs, dest);
        }
        fs.writeFileSync(
            path.join(target, "index.html"),
            '<!doctype html>\n<meta charset="utf-8" />\n<meta http-equiv="refresh" content="0; url=web-preview/input.html" />\n<a href="web-preview/input.html">Rinne Input 预览</a>\n',
        );
        const bytes = files.reduce((n, f) => n + fs.statSync(f.abs).size, 0);
        console.log(
            `导出 ${files.length} 个文件（${(bytes / 1024 / 1024).toFixed(1)} MB）到 ${path.relative(root, target)}`,
        );
    }
    for (const p of problems) console.error(`  ${p}`);
    console.log(
        `build-static：${problems.length ? `${problems.length} 处外链或网络调用` : "自检通过，纯本地、无网络请求"}`,
    );
    process.exitCode = problems.length ? 1 : 0;
}

// 被 import（测试）时只导出函数，不执行
if (process.argv[1] && fs.realpathSync(process.argv[1]) === fs.realpathSync(fileURLToPath(import.meta.url))) main();
