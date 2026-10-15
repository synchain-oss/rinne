// SPDX-License-Identifier: GPL-3.0-or-later
//
// HTML 注入口门禁：web/ 下的 js、mjs、html 去掉注释后，不得出现 innerHTML、outerHTML、insertAdjacentHTML、
// document.write。界面文本只经 textContent 与 DOM API 写入，词条里没有 HTML；这样即使词条或桥送来的
// 字符串里有标记，也不可能变成可执行内容。白名单为空：没有任何文件可以豁免，需要时改写调用方。
// 只豁免逐字收录的 JUCE 辅助脚本（web/js/juce/）。
//   node scripts/check-no-html-sinks.mjs [--self-test]

import fs from "node:fs";
import path from "node:path";
import { ROOT, assertNodeVersion, caseResult, isMain, j, readText } from "./lib/check-kit.mjs";

const SINKS = new RegExp(
    j("\\b(?:inner", "HTML|outer", "HTML|insertAdjacent", "HTML)\\b|\\bdocument\\s*\\.\\s*wr", "ite\\s*\\("),
);
const SKIP = ["web/js/juce", "web/fonts"];
// 正则字面量可以出现在这些字符之后；其余情况下的 / 是除号
const REGEX_PREV = new Set("(,=:[!&|?{};+-*%~^<>\n".split(""));
const REGEX_WORDS = new Set([
    "return",
    "typeof",
    "instanceof",
    "in",
    "of",
    "new",
    "delete",
    "void",
    "case",
    "do",
    "else",
    "yield",
    "await",
]);

/** 把 JS 里的注释换成空格（保留换行，行号不变）。感知字符串、模板串和正则字面量。 */
export function blankJsComments(src) {
    let out = "";
    let i = 0;
    const n = src.length;
    let prev = "\n"; // 上一个有效（非空白）字符
    let prevWord = "";
    const blank = (s) => s.replace(/[^\n]/g, " ");
    while (i < n) {
        const c = src[i];
        const d = src[i + 1];
        if (c === "/" && d === "/") {
            const e = src.indexOf("\n", i);
            const end = e < 0 ? n : e;
            out += blank(src.slice(i, end));
            i = end;
        } else if (c === "/" && d === "*") {
            const e = src.indexOf("*/", i + 2);
            const end = e < 0 ? n : e + 2;
            out += blank(src.slice(i, end));
            i = end;
        } else if (c === '"' || c === "'" || c === "`") {
            let k = i + 1;
            while (k < n && src[k] !== c && !(c !== "`" && src[k] === "\n")) k += src[k] === "\\" ? 2 : 1;
            out += src.slice(i, k + 1);
            i = k + 1;
            prev = c;
            prevWord = "";
        } else if (c === "/" && (REGEX_PREV.has(prev) || REGEX_WORDS.has(prevWord))) {
            let k = i + 1;
            let inClass = false;
            while (k < n && src[k] !== "\n") {
                if (src[k] === "\\") k += 2;
                else {
                    if (src[k] === "[") inClass = true;
                    else if (src[k] === "]") inClass = false;
                    else if (src[k] === "/" && !inClass) break;
                    k++;
                }
            }
            out += src.slice(i, k + 1);
            i = k + 1;
            prev = "/";
            prevWord = "";
        } else {
            out += c;
            if (c === "\n") {
                prev = "\n";
                prevWord = "";
            } else if (!/\s/.test(c)) {
                prev = c;
                prevWord = /\w/.test(c) ? prevWord + c : "";
            }
            i++;
        }
    }
    return out;
}

/** HTML：去掉 <!-- -->，并对 <script> 的内容去 JS 注释。 */
export function blankHtmlComments(src) {
    const blank = (s) => s.replace(/[^\n]/g, " ");
    let t = src.replace(/<!--[\s\S]*?-->/g, blank);
    t = t.replace(/(<script\b[^>]*>)([\s\S]*?)(<\/script>)/gi, (_, a, body, z) => a + blankJsComments(body) + z);
    return t;
}

export function findSinks(text, file) {
    const body = /\.html?$/.test(file) ? blankHtmlComments(text) : blankJsComments(text);
    const out = [];
    body.split("\n").forEach((line, i) => {
        const m = SINKS.exec(line);
        if (m) out.push({ line: i + 1, match: m[0] });
    });
    return out;
}

function walk(dir, acc) {
    for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
        const p = path.join(dir, e.name);
        const rel = path.relative(ROOT, p).replace(/\\/g, "/");
        if (SKIP.some((s) => rel === s)) continue;
        if (e.isDirectory()) walk(p, acc);
        else if (/\.(?:js|mjs|html)$/.test(e.name)) acc.push(p);
    }
    return acc;
}

export function checkTree(root = ROOT) {
    const dir = path.join(root, "web");
    const files = fs.existsSync(dir) ? walk(dir, []) : [];
    const problems = [];
    for (const f of files) {
        const text = readText(f);
        if (text === null) continue;
        for (const h of findSinks(text, f))
            problems.push(`${path.relative(root, f).replace(/\\/g, "/")}:${h.line} ${h.match}`);
    }
    return { count: files.length, problems };
}

export function selfTest() {
    const sink = j("el.inner", "HTML = x;");
    const cases = [
        ["代码里的注入口", true, findSinks(`const a = 1;\n${sink}\n`, "a.js")],
        ["insertAdjacent 系", true, findSinks(j("el.insertAdjacent", "HTML('beforeend', s);"), "a.js")],
        ["document.write", true, findSinks(j("document.wr", "ite(s);"), "a.js")],
        ["字符串里的名字也算", true, findSinks(j("el['outer", "HTML'] = s;"), "a.js")],
        ["行注释里的名字不算", false, findSinks(`// 不要用 ${sink}\nlet a;\n`, "a.js")],
        ["块注释里的名字不算", false, findSinks(`/* ${sink} */ let a;\n`, "a.js")],
        ["HTML 注释里的名字不算", false, findSinks(`<p></p><!-- ${sink} -->\n`, "a.html")],
        ["内联脚本的注释不算", false, findSinks(`<script>// ${sink}\nlet a;</script>\n`, "a.html")],
        ["内联脚本的代码照判", true, findSinks(`<script>${sink}</script>\n`, "a.html")],
        ["正则字面量里的双斜杠不吞掉后面的代码", true, findSinks(`const r = /a\\/\\//; ${sink}\n`, "a.js")],
        ["除号不被当成正则", true, findSinks(`const q = a / b; ${sink}\n`, "a.js")],
    ].map(([label, bad, found]) => ({ label, bad, passed: bad ? found.length > 0 : found.length === 0 }));
    const live = checkTree(ROOT);
    cases.push({ label: "仓内 web/ 干净", bad: false, passed: live.problems.length === 0 && live.count > 0 });
    return caseResult(cases);
}

function main(argv) {
    assertNodeVersion();
    if (argv.includes("--self-test")) {
        const r = selfTest();
        for (const l of r.lines) console.error(l);
        console.log(`check-no-html-sinks --self-test：${r.ok ? "通过" : "失败"}（坏样例 ${r.bad}、正例 ${r.good}）`);
        return r.ok ? 0 : 1;
    }
    const r = checkTree();
    if (!r.problems.length) {
        console.log(`check-no-html-sinks 通过：${r.count} 个文件`);
        return 0;
    }
    console.error(`check-no-html-sinks 失败：${r.problems.length} 处`);
    for (const p of r.problems) console.error(`  ${p}`);
    return 1;
}

if (isMain(import.meta.url)) process.exitCode = main(process.argv.slice(2));
