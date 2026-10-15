// SPDX-License-Identifier: GPL-3.0-or-later
//
// 复制来的前端文件的注释清单：列出注释里命中隐私规则或「内部口吻」的位置，供人逐处重写。
// 本脚本只读不改；注释必须由人改写成自含的理由，不能自动删改（自动替换会留下读不通的半句话）。
//   node scripts/scrub-internal-ids.mjs --report [路径…]   路径可以是文件或目录，默认 web/
//   node scripts/scrub-internal-ids.mjs --self-test
// 判据 = privacy-needles.mjs 的通用规则（只对注释文字运行）+ 下面的「内部口吻」词表。
// 清单为空不代表可以入库：入库前仍以 check-privacy 为准，本脚本只是更早、更细地指出该改哪里。

import fs from "node:fs";
import path from "node:path";
import { ROOT, assertNodeVersion, caseResult, isMain, j, readText } from "./lib/check-kit.mjs";
import { RULES, scan } from "./lib/privacy-needles.mjs";

const EXT = /\.(?:js|mjs|css|html|py)$/;

// 复制来源里常见的内部口吻：决策、评审、回流这类流程词，以及指向来源仓内位置的说法。
// 规则表里的编号针管不到这些（它们没有编号形态），所以单独列。词表只增不减。
export const VOICE = [
    ["decision-word", new RegExp(j("裁", "定|终", "审|统", "筹|回", "流|差异", "清单|回验|用户拍板"))],
    ["source-row", /(?:设计稿|研究|调研|原文)\s*\d+\s*(?:[-–]\s*\d+\s*)?行|第\s*\d+\s*行/],
    ["source-name", new RegExp(j("masterPl", "an|design-rec", "on|\\bBrid", "ge\\b|\\bSC", "VB\\b"))],
    ["round-tag", /\bR[1-9]\b|\bPR\s?#\d+|#\d{2,4}\b/],
];

/** 抽出文本里的注释，返回 [{ line, text }]（行号为注释起始行；JS 与 CSS 感知字符串，HTML 只认 <!-- -->）。 */
export function comments(text, file) {
    const out = [];
    const html = /\.html$/.test(file);
    const hash = /\.py$/.test(file);
    const css = /\.css$/.test(file);
    let i = 0;
    let line = 1;
    const n = text.length;
    const adv = (s) => {
        for (const c of s) if (c === "\n") line++;
    };
    while (i < n) {
        const c = text[i];
        const rest = text.slice(i, i + 4);
        if (html && rest === "<!--") {
            const e = text.indexOf("-->", i + 4);
            const end = e < 0 ? n : e + 3;
            out.push({ line, text: text.slice(i, end) });
            adv(text.slice(i, end));
            i = end;
        } else if (!html && !hash && rest.startsWith("/*")) {
            const e = text.indexOf("*/", i + 2);
            const end = e < 0 ? n : e + 2;
            out.push({ line, text: text.slice(i, end) });
            adv(text.slice(i, end));
            i = end;
        } else if (!html && !css && !hash && rest.startsWith("//")) {
            const e = text.indexOf("\n", i);
            const end = e < 0 ? n : e;
            out.push({ line, text: text.slice(i, end) });
            i = end;
        } else if (hash && c === "#") {
            const e = text.indexOf("\n", i);
            const end = e < 0 ? n : e;
            out.push({ line, text: text.slice(i, end) });
            i = end;
        } else if (!html && (c === '"' || c === "'" || c === "`")) {
            // 跳过字符串；CSS 里的 url("…") 与 content 同理
            let k = i + 1;
            while (k < n && text[k] !== c && !(c !== "`" && text[k] === "\n")) k += text[k] === "\\" ? 2 : 1;
            adv(text.slice(i, k + 1));
            i = k + 1;
        } else {
            if (c === "\n") line++;
            i++;
        }
    }
    return out;
}

/** 一个文件的命中：[{ line, why, text }]，text 是注释的第一行。 */
export function findingsOf(text, file) {
    const out = [];
    for (const cm of comments(text, file)) {
        const why = [];
        for (const f of scan(cm.text, {}, { rules: RULES.filter((r) => !r.group) })) why.push(f.rule.id);
        for (const [id, re] of VOICE) if (re.test(cm.text)) why.push(id);
        if (why.length)
            out.push({ line: cm.line, why: [...new Set(why)], text: cm.text.split("\n")[0].trim().slice(0, 100) });
    }
    return out;
}

function walk(p, acc) {
    const st = fs.statSync(p);
    if (st.isDirectory()) {
        if (/(?:^|[\\/])(?:node_modules|js[\\/]juce|fonts)$/.test(p)) return acc;
        for (const f of fs.readdirSync(p)) walk(path.join(p, f), acc);
    } else if (EXT.test(p)) {
        acc.push(p);
    }
    return acc;
}

export function selfTest() {
    const cases = [
        ["编号在块注释里", true, findingsOf(`a{}\n/* ${j("[SL", "-377]")} 改回 */\n`, "x.css")],
        ["流程词在行注释里", true, findingsOf(`// ${j("统", "筹裁", "定")}维持\nlet a;\n`, "x.js")],
        ["来源行号", true, findingsOf(`// ${j("设计稿 ", "16 行")}的值\n`, "x.js")],
        ["字符串里的编号不是注释", false, findingsOf(`const s = "${j("[SL", "-377]")}";\n`, "x.js")],
        ["干净的注释", false, findingsOf("/* 顶缘高光线 */\n.a{}\n", "x.css")],
        ["HTML 注释", true, findingsOf(`<p></p><!-- ${j("统", "筹裁", "定")} -->\n`, "x.html")],
    ].map(([label, bad, found]) => ({ label, bad, passed: bad ? found.length > 0 : found.length === 0 }));
    return caseResult(cases);
}

function main(argv) {
    assertNodeVersion();
    if (argv.includes("--self-test")) {
        const r = selfTest();
        for (const l of r.lines) console.error(l);
        console.log(`scrub-internal-ids --self-test：${r.ok ? "通过" : "失败"}（坏样例 ${r.bad}、正例 ${r.good}）`);
        return r.ok ? 0 : 1;
    }
    if (!argv.includes("--report")) {
        console.error("用法：node scripts/scrub-internal-ids.mjs --report [路径…] | --self-test");
        return 1;
    }
    const targets = argv.filter((a) => !a.startsWith("--"));
    const files = (targets.length ? targets : ["web"]).flatMap((t) => walk(path.resolve(ROOT, t), []));
    let total = 0;
    for (const f of files.sort()) {
        const text = readText(f);
        if (text === null) continue;
        const found = findingsOf(text, f);
        for (const x of found)
            console.log(`${path.relative(ROOT, f).replace(/\\/g, "/")}:${x.line} [${x.why.join(",")}] ${x.text}`);
        total += found.length;
    }
    console.log(`scrub-internal-ids：${total} 处注释需要人工重写`);
    return 0;
}

if (isMain(import.meta.url)) process.exitCode = main(process.argv.slice(2));
