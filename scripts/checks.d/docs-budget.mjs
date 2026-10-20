// SPDX-License-Identifier: GPL-3.0-or-later
//
// 文档与脚本体量预算（docs-truth 的一部分），数据取 scripts/lib/rules.mjs 的 BUDGETS：
//   files：逐个文件的 UTF-8 字节数；
//   patterns：glob 命中的文件，目前只有 ADR（docs/adr/[0-9][0-9][0-9][0-9]-*.md），每份 4 KiB；
//   changelogSectionBytes：CHANGELOG.md 每个二级标题节的字节数；
//   publicMarkdownBytes：全部跟踪 Markdown 的合计，third_party/ 与 LICENSES/ 除外；
//   scriptLines：scripts/ 下本仓新写脚本的总行数与单文件行数，upstream/*.json 里登记的复制脚本不计。
// ADR 超限判红（CLAUDE.md 把 ADR 4 KB 定为硬上限），其余条目超限只报 WARN。
// growthPerMinor 不实现：仓里没有可比的上一小版本基线，增长比例无从计算。
// 判据在 budgetProblems，只吃路径与文本；glob 用本文件里的最小匹配，不引依赖。

import path from "node:path";
import { BUDGETS } from "../lib/rules.mjs";
import { caseResult, readText, trackedFiles } from "../lib/check-kit.mjs";

const CHANGELOG = "CHANGELOG.md";
const PUBLIC_MARKDOWN = /\.md$/i;
const NOT_PUBLIC = /^(?:third_party|LICENSES)\//;
const SCRIPT = /^scripts\/.*\.(?:mjs|ps1|psm1|py|sh)$/;
const UPSTREAM_JSON = /^upstream\/[^/]+\.json$/;

const bytesOf = (text) => Buffer.byteLength(text, "utf8");

/** 行数：按 LF / CRLF 分行，末尾换行不多算一行；空文件是 0 行。 */
function lineCount(text) {
    const lines = text.split(/\r?\n/);
    if (lines.at(-1) === "") lines.pop();
    return lines.length;
}

/** 最小 glob → 正则：`*` 不跨 `/`，`?` 是一个非 `/` 字符，`[...]` 是字符组。 */
function globToRegExp(glob) {
    let out = "";
    for (let i = 0; i < glob.length; i++) {
        const c = glob[i];
        if (c === "*") out += "[^/]*";
        else if (c === "?") out += "[^/]";
        else if (c === "[") {
            const end = glob.indexOf("]", i + 1);
            if (end < 0) {
                out += "\\[";
                continue;
            }
            let body = glob.slice(i + 1, end);
            if (body.startsWith("!")) body = `^${body.slice(1)}`;
            out += body && body !== "^" ? `[${body.replace(/\\/g, "\\\\")}]` : "\\[\\]";
            i = end;
        } else out += c.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
    }
    return new RegExp(`^${out}$`);
}

/** CHANGELOG 的二级标题节：从 `## ` 行到下一个二级标题之前；围栏代码里的 `## ` 不算。 */
function h2Sections(text) {
    const out = [];
    let cur = null;
    let fence = false;
    for (const raw of text.match(/[^\n]*\n|[^\n]+$/g) ?? []) {
        const line = raw.replace(/\r?\n$/, "");
        if (/^\s*(```|~~~)/.test(line)) fence = !fence;
        else if (!fence && /^## /.test(line)) {
            if (cur) out.push(cur);
            cur = { title: line.slice(3).trim(), text: raw };
            continue;
        }
        if (cur) cur.text += raw;
    }
    if (cur) out.push(cur);
    return out.map((s) => ({ title: s.title || "(无标题)", text: s.text }));
}

/** upstream/*.json 里登记为本仓复制来源的路径；坏 JSON 由 upstream 检查报，这里跳过。 */
function upstreamCopies(paths, texts) {
    const copies = new Set();
    for (const p of paths) {
        if (!UPSTREAM_JSON.test(p) || typeof texts[p] !== "string") continue;
        try {
            const data = JSON.parse(texts[p]);
            for (const f of Array.isArray(data?.files) ? data.files : []) {
                if (typeof f?.path === "string") copies.add(f.path);
            }
        } catch {
            // 片段不是合法 JSON 时留给 upstream 检查
        }
    }
    return copies;
}

/**
 * paths：全部跟踪路径；texts：{ 路径: 文本 }，含预算条目、CHANGELOG.md、公开 Markdown、
 * scripts/ 下的脚本与 upstream/*.json。返回 { red, warn }，只做计算，不碰文件系统。
 */
export function budgetProblems({ paths, texts }) {
    return judge({ paths, texts }, BUDGETS);
}

/** 判据本体。budgets 只供变异检查放宽阈值，运行时一律是 BUDGETS。 */
function judge({ paths, texts }, budgets) {
    const red = [];
    const warn = [];
    const present = (p) => typeof texts[p] === "string";
    const sorted = [...paths].sort();

    // 预算文件缺失时不在这里报：存在性由别的检查负责。
    for (const { path: file, bytes: limit } of budgets.files) {
        if (!present(file)) continue;
        const bytes = bytesOf(texts[file]);
        if (bytes > limit) warn.push(`${file} 有 ${bytes} 字节，超过 ${limit} 字节的预算`);
    }

    for (const pattern of budgets.patterns) {
        const re = globToRegExp(pattern.glob);
        for (const p of sorted) {
            if (!re.test(p) || !present(p)) continue;
            const bytes = bytesOf(texts[p]);
            if (bytes > pattern.bytes) {
                red.push(`${p} 有 ${bytes} 字节，超过 ADR 每份 ${pattern.bytes} 字节的硬上限`);
            }
        }
    }

    if (present(CHANGELOG)) {
        for (const section of h2Sections(texts[CHANGELOG])) {
            const bytes = bytesOf(section.text);
            if (bytes > budgets.changelogSectionBytes) {
                warn.push(
                    `${CHANGELOG} 的「${section.title}」一节有 ${bytes} 字节，` +
                        `超过 ${budgets.changelogSectionBytes} 字节的预算`,
                );
            }
        }
    }

    const publicBytes = sorted
        .filter((p) => PUBLIC_MARKDOWN.test(p) && !NOT_PUBLIC.test(p) && present(p))
        .reduce((sum, p) => sum + bytesOf(texts[p]), 0);
    if (publicBytes > budgets.publicMarkdownBytes) {
        warn.push(`公开 Markdown 合计 ${publicBytes} 字节，超过 ${budgets.publicMarkdownBytes} 字节的预算`);
    }

    const copies = upstreamCopies(paths, texts);
    const scripts = sorted.filter((p) => SCRIPT.test(p) && !copies.has(p) && present(p));
    let totalLines = 0;
    for (const p of scripts) {
        const lines = lineCount(texts[p]);
        totalLines += lines;
        if (lines > budgets.scriptLines.perFile) {
            warn.push(`${p} 有 ${lines} 行，超过单文件 ${budgets.scriptLines.perFile} 行的预算`);
        }
    }
    if (totalLines > budgets.scriptLines.total) {
        warn.push(`scripts/ 下本仓新写脚本合计 ${totalLines} 行，超过 ${budgets.scriptLines.total} 行的预算`);
    }

    return { red: red.sort(), warn: warn.sort() };
}

export function selfTest() {
    const adrLimit = BUDGETS.patterns[0].bytes;
    const fileLimit = BUDGETS.files.find((f) => f.path === "CLAUDE.md").bytes;
    const sectionLimit = BUDGETS.changelogSectionBytes;
    const mdLimit = BUDGETS.publicMarkdownBytes;
    const { total, perFile } = BUDGETS.scriptLines;
    const adr = "docs/adr/9999-demo.md";
    const one = (p, text) => ({ paths: [p], texts: { [p]: text } });
    const lines = (n) => Array.from({ length: n }, (_, i) => `line ${i}`).join("\n");
    const manyScripts = () => {
        const count = Math.ceil((total + 1) / perFile);
        const each = Math.ceil((total + 1) / count);
        const paths = [];
        const texts = {};
        for (let i = 0; i < count; i++) {
            const p = `scripts/demo${i}.mjs`;
            paths.push(p);
            texts[p] = lines(each);
        }
        return { paths, texts };
    };
    const changelog = `## [1.0.0]\n${"x".repeat(sectionLimit + 1)}\n## [0.9.0]\nok`;
    const cases = [
        [
            "ADR 恰好 4096 字节不判红",
            false,
            one(adr, "x".repeat(adrLimit)),
            (r) => r.red.length === 0 && r.warn.length === 0,
        ],
        [
            "ADR 4097 字节判红",
            true,
            one(adr, "x".repeat(adrLimit + 1)),
            (r) => r.red.length === 1 && r.warn.length === 0,
        ],
        [
            "CJK 多字节按字节而非字符算",
            true,
            one(adr, "中".repeat(Math.ceil((adrLimit + 1) / 3))),
            (r) => r.red.length === 1 && r.warn.length === 0,
        ],
        [
            "CLAUDE.md 超限只 WARN 不判红",
            true,
            one("CLAUDE.md", "x".repeat(fileLimit + 1)),
            (r) => r.red.length === 0 && r.warn.length === 1,
        ],
        [
            "CLAUDE.md 恰好到预算不报",
            false,
            one("CLAUDE.md", "x".repeat(fileLimit)),
            (r) => r.red.length === 0 && r.warn.length === 0,
        ],
        [
            "changelog 某节超限 WARN",
            true,
            one(CHANGELOG, changelog),
            (r) => r.red.length === 0 && r.warn.length === 1 && r.warn[0].includes(CHANGELOG),
        ],
        [
            "公开 Markdown 合计超限 WARN",
            true,
            one("docs/big.md", "x".repeat(mdLimit + 1)),
            (r) => r.red.length === 0 && r.warn.length === 1 && r.warn[0].includes("公开 Markdown"),
        ],
        [
            "third_party 与 LICENSES 不计入公开 Markdown",
            false,
            {
                paths: ["third_party/big.md", "LICENSES/big.md", "docs/small.md"],
                texts: {
                    "third_party/big.md": "x".repeat(mdLimit + 1),
                    "LICENSES/big.md": "x".repeat(mdLimit + 1),
                    "docs/small.md": "ok",
                },
            },
            (r) => r.red.length === 0 && r.warn.length === 0,
        ],
        [
            "脚本单文件 401 行 WARN",
            true,
            one("scripts/demo.mjs", lines(perFile + 1)),
            (r) => r.red.length === 0 && r.warn.length === 1 && r.warn[0].includes("单文件"),
        ],
        [
            "脚本单文件 400 行不报",
            false,
            one("scripts/demo.mjs", lines(perFile)),
            (r) => r.red.length === 0 && r.warn.length === 0,
        ],
        [
            "upstream 登记的脚本不计行数",
            false,
            {
                paths: ["scripts/copied.mjs", "upstream/demo.json"],
                texts: {
                    "scripts/copied.mjs": lines(perFile + 100),
                    "upstream/demo.json": JSON.stringify({ files: [{ path: "scripts/copied.mjs" }] }),
                },
            },
            (r) => r.red.length === 0 && r.warn.length === 0,
        ],
        [
            "scripts 合计超限 WARN",
            true,
            manyScripts(),
            (r) => r.red.length === 0 && r.warn.length === 1 && r.warn[0].includes("合计"),
        ],
        [
            "scripts 之外的脚本与别的扩展名不计",
            false,
            {
                paths: ["scripts/notes.txt", "web-preview/big.mjs"],
                texts: {
                    "scripts/notes.txt": lines(perFile + 100),
                    "web-preview/big.mjs": lines(perFile + 100),
                },
            },
            (r) => r.red.length === 0 && r.warn.length === 0,
        ],
        [
            "ADR glob 只认四位数字开头的 Markdown",
            false,
            one("docs/adr/12-demo.md", "x".repeat(adrLimit + 1)),
            (r) => r.red.length === 0 && r.warn.length === 0,
        ],
        ["预算文件缺失不在这里报", false, { paths: [], texts: {} }, (r) => r.red.length === 0 && r.warn.length === 0],
    ].map(([label, bad, input, expect]) => ({ label, bad, passed: expect(budgetProblems(input)) }));
    return caseResult(cases);
}

export default {
    name: "docs-budget",
    group: "docs",
    inputs: [],
    run(ctx) {
        const paths = trackedFiles(ctx.root);
        const budgeted = new Set(BUDGETS.files.map((f) => f.path));
        const wanted = (p) =>
            budgeted.has(p) ||
            p === CHANGELOG ||
            UPSTREAM_JSON.test(p) ||
            (PUBLIC_MARKDOWN.test(p) && !NOT_PUBLIC.test(p)) ||
            SCRIPT.test(p);
        const texts = {};
        for (const p of paths) {
            if (!wanted(p)) continue;
            const text = readText(path.join(ctx.root, p));
            if (text !== null) texts[p] = text;
        }
        const { red, warn } = budgetProblems({ paths, texts });
        return { ok: red.length === 0, lines: [...red, ...warn.map((w) => `WARN ${w}`)] };
    },
    selfTest,
    /** 放宽阈值后，原本判红的 ADR 超限样例必须不再红；脚本单文件超限的 WARN 同理消失。 */
    mutationCheck() {
        const lines = [];
        const adr = "docs/adr/9999-demo.md";
        const adrInput = { paths: [adr], texts: { [adr]: "x".repeat(BUDGETS.patterns[0].bytes + 1) } };
        if (judge(adrInput, BUDGETS).red.length !== 1) lines.push("基准：ADR 超限没有判红");
        const wideAdr = structuredClone(BUDGETS);
        for (const p of wideAdr.patterns) p.bytes *= 2;
        if (judge(adrInput, wideAdr).red.length !== 0) lines.push("ADR 阈值翻倍后，超限样例仍然判红");

        const script = "scripts/demo.mjs";
        const body = Array.from({ length: BUDGETS.scriptLines.perFile + 1 }, (_, i) => `line ${i}`).join("\n");
        const scriptInput = { paths: [script], texts: { [script]: body } };
        if (!judge(scriptInput, BUDGETS).warn.some((w) => w.includes("单文件"))) {
            lines.push("基准：脚本单文件超限没有报 WARN");
        }
        const wideScript = structuredClone(BUDGETS);
        wideScript.scriptLines.perFile *= 2;
        if (judge(scriptInput, wideScript).warn.some((w) => w.includes("单文件"))) {
            lines.push("单文件行数上限翻倍后，超限样例仍然报 WARN");
        }
        return { ok: lines.length === 0, lines };
    },
};
