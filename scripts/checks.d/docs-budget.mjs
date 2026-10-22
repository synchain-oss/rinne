// SPDX-License-Identifier: GPL-3.0-or-later
//
// 文档与脚本体量预算（docs-truth 的一部分），数据取 scripts/lib/rules.mjs 的 BUDGETS：
//   files：逐个文件在磁盘上的字节数；
//   patterns：glob 命中的文件，目前只有 ADR（docs/adr/[0-9][0-9][0-9][0-9]-*.md），每份 4 KiB；
//   changelogSectionBytes：CHANGELOG.md 每个二级标题节的字节数；
//   publicMarkdownBytes：全部跟踪 Markdown 的合计，third_party/ 与 LICENSES/ 除外；
//   scriptLines：scripts/ 下本仓新写脚本的总行数与单文件行数，upstream/*.json 里登记的复制脚本不计。
// patterns 里带 hard 的条目超限判红（CLAUDE.md 把 ADR 4 KB 定为硬上限），其余条目超限只报 WARN。
// growthPerMinor 不实现：仓里没有可比的上一小版本基线，增长比例无从计算。
// 判据在 budgetProblems，只吃路径、文本与字节数，不碰文件系统；glob 用本文件里的最小匹配，不引依赖。
// 字节按文件在磁盘上的原始字节算（输入里的 sizes，来自 readEntry 读到的整个缓冲区）：解码会剥掉 UTF-8 BOM，
// 按解码后的文本算会少 3 字节，恰好跨过预算线时判错。解码后的文本只用来算行数与 CHANGELOG 分节。
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { BUDGETS } from "../lib/rules.mjs";
import { caseResult, decodeText, trackedFiles } from "../lib/check-kit.mjs";

const CHANGELOG = "CHANGELOG.md";
const PUBLIC_MARKDOWN = /\.md$/i;
const NOT_PUBLIC = /^(?:third_party|LICENSES)\//;
const SCRIPT = /^scripts\/.*\.(?:mjs|ps1|psm1|py|sh)$/;
const UPSTREAM_JSON = /^upstream\/[^/]+\.json$/;

const bytesOf = (text) => Buffer.byteLength(text, "utf8");

/** 行数：按 LF / CRLF 分行，末尾换行不多算一行；空文件是 0 行。dropTrailing 只供变异检查用。 */
function lineCount(text, { dropTrailing = true } = {}) {
    const lines = text.split(/\r?\n/);
    if (dropTrailing && lines.at(-1) === "") lines.pop();
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

/**
 * CHANGELOG 的二级标题节：从 `## ` 行到下一个二级标题之前；围栏代码里的 `## ` 不算。
 * isFence 与 onlyFirst 只供变异检查换成坏版本（围栏判定失效 / 只让第一个 ## 开节）。
 */
function h2Sections(text, { isFence = (line) => /^\s*(```|~~~)/.test(line), onlyFirst = false } = {}) {
    const out = [];
    let cur = null;
    let fence = false;
    for (const raw of text.match(/[^\n]*\n|[^\n]+$/g) ?? []) {
        const line = raw.replace(/\r?\n$/, "");
        if (isFence(line)) fence = !fence;
        else if (!fence && /^## /.test(line) && !(onlyFirst && cur)) {
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
 * scripts/ 下的脚本与 upstream/*.json；sizes：{ 路径: 磁盘上的字节数 }，没有时按文本长度算。
 * 返回 { red, warn }，只做计算，不碰文件系统。
 * overrides 只供变异检查换掉一条判据（sections / count / sizeOf / isHard / patternLabel / budgets），运行时留空。
 */
export function budgetProblems({ paths, texts, sizes }, overrides = {}) {
    return judge({ paths, texts, sizes }, overrides.budgets ?? BUDGETS, overrides);
}

/**
 * 判据本体。budgets 只供变异检查换阈值与口径，运行时一律是 BUDGETS；overrides 同上。
 * 字节一律取 sizes 里记的磁盘字节数（原始字节），没有才退回解码后文本的字节数。
 */
function judge({ paths, texts, sizes }, budgets, overrides = {}) {
    const sections = overrides.sections ?? ((text) => h2Sections(text));
    const count = overrides.count ?? ((text) => lineCount(text));
    const label = overrides.patternLabel ?? ((pattern) => pattern.glob);
    const isHard = overrides.isHard ?? ((pattern) => pattern.hard === true);
    const sizeOf = (p) =>
        overrides.sizeOf
            ? overrides.sizeOf(p, texts, sizes)
            : sizes && typeof sizes[p] === "number"
              ? sizes[p]
              : bytesOf(texts[p]);
    const red = [];
    const warn = [];
    const present = (p) => typeof texts[p] === "string";
    const sorted = [...paths].sort();
    // 预算文件缺失时不在这里报：存在性由别的检查负责。
    for (const { path: file, bytes: limit } of budgets.files) {
        if (!present(file)) continue;
        const bytes = sizeOf(file);
        if (bytes > limit) warn.push(`${file} 有 ${bytes} 字节，超过 ${limit} 字节的预算`);
    }

    // patterns 里带 hard 的才判红（ADR 是硬上限），其余只报 WARN；提示文字按这一项自己的 glob 生成，不写死成 ADR。
    for (const pattern of budgets.patterns) {
        const re = globToRegExp(pattern.glob);
        for (const p of sorted) {
            if (!re.test(p) || !present(p)) continue;
            const bytes = sizeOf(p);
            if (bytes > pattern.bytes) {
                const hard = isHard(pattern);
                const msg = `${p} 有 ${bytes} 字节，超过 ${label(pattern)} 每份 ${pattern.bytes} 字节的${hard ? "硬上限" : "预算"}`;
                if (hard) red.push(msg);
                else warn.push(msg);
            }
        }
    }

    if (present(CHANGELOG)) {
        for (const section of sections(texts[CHANGELOG])) {
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
        .reduce((sum, p) => sum + sizeOf(p), 0);
    if (publicBytes > budgets.publicMarkdownBytes) {
        warn.push(`公开 Markdown 合计 ${publicBytes} 字节，超过 ${budgets.publicMarkdownBytes} 字节的预算`);
    }

    const copies = upstreamCopies(paths, texts);
    const scripts = sorted.filter((p) => SCRIPT.test(p) && !copies.has(p) && present(p));
    let totalLines = 0;
    for (const p of scripts) {
        const lines = count(texts[p]);
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

/**
 * 跑一遍自测用例。overrides 只供变异检查换掉一条判据（走判据本体的同一个入口），运行时留空。
 * 每个用例配一个只有它能命中的坏样例：删掉任一判据，用例必须变红。
 */
function runCases(overrides = {}) {
    const adrPattern = BUDGETS.patterns.find((p) => p.hard) ?? BUDGETS.patterns[0];
    const adrLimit = adrPattern.bytes;
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
    // 三个新样例：两节只一节超限、围栏里的 ## 不是节头、末尾换行不多算一行。用例名写明各自的期望。
    const twoSections = `## [9.9.9]\nsmall\n## [8.8.8]\n${"x".repeat(sectionLimit + 1)}\n`;
    const fenced = ["## [1.0.0]", "```text", "## 假节头", "```", "x".repeat(sectionLimit + 1), ""].join("\n");
    const withNewline = one("scripts/demo.mjs", `${lines(perFile + 5)}\n`);
    const withoutNewline = one("scripts/demo.mjs", lines(perFile + 5));
    // 从 WARN 文本里取判据算出的行数；只有一个 WARN 且字节数对得上；某一节自己的字节数。
    const rowsOf = (r) => Number(/有 (\d+) 行/.exec(r.warn.join("\n"))?.[1] ?? -1);
    const oneWarn = (r, bytes) => r.red.length === 0 && r.warn.length === 1 && r.warn[0].includes(`${bytes} 字节`);
    const bytesFrom = (text, marker) => Buffer.byteLength(text.slice(text.indexOf(marker)), "utf8");
    // 带 BOM 的文件：磁盘上的字节比解码后的文本多 3 字节。字节数必须按磁盘上的原始字节算。
    const bomBytes = (n) => Buffer.concat([Buffer.from([0xef, 0xbb, 0xbf]), Buffer.from("x".repeat(n))]);
    const bomOver = bomBytes(adrLimit - 2); // 3 + 4094 = 4097 字节
    const bomExact = bomBytes(adrLimit - 3); // 3 + 4093 = 4096 字节
    const bomFileOver = bomBytes(fileLimit - 2); // CLAUDE.md：磁盘上 fileLimit + 1 字节，解码后少 3 字节
    const bomMdOver = bomBytes(mdLimit - 2); // 公开 Markdown 合计：同理
    const fromDisk = (p, bytes) => {
        const e = readBackFromDisk(bytes);
        return { paths: [p], texts: { [p]: e.text }, sizes: { [p]: e.size } };
    };
    // patterns 里不带 hard 的条目只报 WARN：提示文字按这一项自己的 glob 生成。
    const softBudgets = structuredClone(BUDGETS);
    softBudgets.patterns = [{ glob: "docs/notes/[0-9][0-9]-*.md", bytes: 1024 }];
    const soft = "docs/notes/01-demo.md";
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
            "ADR 带 BOM 时按磁盘字节算：4097 字节判红",
            true,
            { paths: [adr], texts: { [adr]: decodeText(bomOver) }, sizes: { [adr]: bomOver.length } },
            (r) => r.red.length === 1 && r.warn.length === 0 && r.red[0].includes(`${bomOver.length} 字节`),
        ],
        [
            "ADR 带 BOM 时按磁盘字节算：4096 字节不判红",
            false,
            { paths: [adr], texts: { [adr]: decodeText(bomExact) }, sizes: { [adr]: bomExact.length } },
            (r) => r.red.length === 0 && r.warn.length === 0,
        ],
        [
            "ADR 真从磁盘读回来：带 BOM 的 4097 字节判红，不能少算 BOM",
            true,
            fromDisk(adr, bomOver),
            (r) => r.red.length === 1 && r.warn.length === 0 && r.red[0].includes(`${bomOver.length} 字节`),
        ],
        [
            "CLAUDE.md 带 BOM 时按磁盘字节算：超一字节报 WARN",
            true,
            {
                paths: ["CLAUDE.md"],
                texts: { "CLAUDE.md": decodeText(bomFileOver) },
                sizes: { "CLAUDE.md": bomFileOver.length },
            },
            (r) => oneWarn(r, bomFileOver.length) && r.warn[0].includes("CLAUDE.md"),
        ],
        [
            "公开 Markdown 合计按磁盘字节算：带 BOM 超一字节报 WARN",
            true,
            {
                paths: ["docs/big.md"],
                texts: { "docs/big.md": decodeText(bomMdOver) },
                sizes: { "docs/big.md": bomMdOver.length },
            },
            (r) => oneWarn(r, bomMdOver.length) && r.warn[0].includes("公开 Markdown"),
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
            "changelog 两节只报超限的那一节，不合并两节",
            true,
            one(CHANGELOG, twoSections),
            (r) =>
                oneWarn(r, bytesFrom(twoSections, "## [8.8.8]")) &&
                r.warn[0].includes("8.8.8") &&
                !r.warn[0].includes("9.9.9"),
        ],
        [
            "changelog 围栏里的 ## 不算节头",
            true,
            one(CHANGELOG, fenced),
            (r) => oneWarn(r, Buffer.byteLength(fenced, "utf8")) && !r.warn[0].includes("假节头"),
        ],
        [
            "脚本行数不因末尾换行多算一行，带与不带末尾换行算出的行数相同",
            true,
            withNewline,
            (r, ov) => {
                const a = rowsOf(r);
                return (
                    r.red.length === 0 &&
                    r.warn.length === 1 &&
                    a === rowsOf(budgetProblems(withoutNewline, ov)) &&
                    a === perFile + 5
                );
            },
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
        [
            "patterns 里不带 hard 的条目超限只 WARN，提示按该项的 glob 生成",
            true,
            one(soft, "x".repeat(1025)),
            (r) =>
                r.red.length === 0 &&
                r.warn.length === 1 &&
                r.warn[0].includes("docs/notes/[0-9][0-9]-*.md") &&
                !r.warn[0].includes("ADR"),
            { budgets: softBudgets },
        ],
        ["预算文件缺失不在这里报", false, { paths: [], texts: {} }, (r) => r.red.length === 0 && r.warn.length === 0],
    ].map(([label, bad, input, expect, caseOverrides]) => {
        const ov = { ...overrides, ...(caseOverrides ?? {}) };
        return { label, bad, passed: expect(budgetProblems(input, ov), ov) };
    });
    return caseResult(cases);
}

/** 自测：坏样例与正例都要有，删掉任一判据都要有用例变红。 */
export function selfTest() {
    return runCases();
}

/** 要读的路径：预算条目、CHANGELOG、公开 Markdown、脚本与 upstream 登记。 */
function wantedPath(p) {
    return (
        BUDGETS.files.some((f) => f.path === p) ||
        p === CHANGELOG ||
        UPSTREAM_JSON.test(p) ||
        (PUBLIC_MARKDOWN.test(p) && !NOT_PUBLIC.test(p)) ||
        SCRIPT.test(p)
    );
}

/**
 * 读一个文件：解码后的文本（算行数与分节）与磁盘上的字节数（算字节预算，就是整个缓冲区的长度，BOM 也在内）。
 * 读不到或是二进制时返回 null。
 */
function readEntry(abs) {
    let buf;
    try {
        buf = fs.readFileSync(abs);
    } catch {
        return null;
    }
    const text = decodeText(buf);
    return text === null ? null : { text, size: buf.length };
}

/** 读一遍要判的文件。 */
function readInputs(root) {
    const paths = trackedFiles(root);
    const texts = {};
    const sizes = {};
    for (const p of paths) {
        if (!wantedPath(p)) continue;
        const entry = readEntry(path.join(root, p));
        if (entry === null) continue;
        texts[p] = entry.text;
        sizes[p] = entry.size;
    }
    return { paths, texts, sizes };
}

/** 把字节写进临时目录里的一个文件，用 readEntry 读回来，再删掉（不递归：目录里只有这一个文件）。 */
function readBackFromDisk(bytes) {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "docs-budget-"));
    const file = path.join(dir, "probe.md");
    try {
        fs.writeFileSync(file, bytes);
        return readEntry(file);
    } finally {
        fs.rmSync(file, { force: true });
        fs.rmdirSync(dir);
    }
}

export default {
    name: "docs-budget",
    group: "docs",
    inputs: [],
    run(ctx) {
        const { paths, texts, sizes } = readInputs(ctx.root);
        const { red, warn } = budgetProblems({ paths, texts, sizes });
        return { ok: red.length === 0, lines: [...red, ...warn.map((w) => `WARN ${w}`)] };
    },
    selfTest,
    /**
     * 逐条换掉一条判据，自测必须因此变红；在真实文件上做变异，门禁必须因此判红。
     * 变异走判据本体的同一个入口（overrides / budgets），不是把判据从源码里删掉。
     */
    mutationCheck(ctx) {
        const lines = [];
        const root = ctx?.root ?? process.cwd();
        const { paths, texts, sizes } = readInputs(root);
        const adrPattern = BUDGETS.patterns.find((p) => p.hard) ?? BUDGETS.patterns[0];

        // 真实 ADR：取最大的那份，原样判不红；在它的内容上垫到超限，门禁必须判红；阈值放宽后同一份垫过的文件不再判红。
        const adrRe = globToRegExp(adrPattern.glob);
        const adrs = paths.filter((p) => adrRe.test(p) && typeof texts[p] === "string");
        if (!adrs.length) {
            lines.push("真实文件里没有 ADR，无法在真实文件上做变异");
        } else {
            const target = adrs.reduce((a, b) => ((sizes[b] ?? 0) > (sizes[a] ?? 0) ? b : a));
            const realInput = {
                paths: [target],
                texts: { [target]: texts[target] },
                sizes: { [target]: sizes[target] },
            };
            if (judge(realInput, BUDGETS).red.length) lines.push(`基准：真实 ADR（${target}）本来就超限，变异没有意义`);
            const padded = texts[target] + "\n" + "x".repeat(adrPattern.bytes);
            const paddedInput = { paths: [target], texts: { [target]: padded }, sizes: { [target]: bytesOf(padded) } };
            if (judge(paddedInput, BUDGETS).red.length !== 1) lines.push(`真实 ADR（${target}）垫到超限后门禁没有判红`);
            const wide = structuredClone(BUDGETS);
            for (const p of wide.patterns) p.bytes *= 2;
            if (judge(paddedInput, wide).red.length) lines.push("ADR 阈值翻倍后，真实 ADR 垫到超限仍然判红");
        }

        // 真实脚本：取最短的那份，原样不报单文件超限；在它的内容后接上 perFile + 1 个空行，门禁必须报 WARN。
        const copies = upstreamCopies(paths, texts);
        const scripts = paths.filter((p) => SCRIPT.test(p) && !copies.has(p) && typeof texts[p] === "string");
        if (!scripts.length) {
            lines.push("真实文件里没有本仓新写的脚本，无法在真实文件上做变异");
        } else {
            const target = scripts.reduce((a, b) => (texts[b].length < texts[a].length ? b : a));
            const overLimit = (text) =>
                judge({ paths: [target], texts: { [target]: text } }, BUDGETS).warn.some((w) => w.includes("单文件"));
            if (overLimit(texts[target])) lines.push(`基准：真实脚本（${target}）本来就超过单文件行数，变异没有意义`);
            if (!overLimit(texts[target] + "\n".repeat(BUDGETS.scriptLines.perFile + 1))) {
                lines.push(`真实脚本（${target}）垫到超过单文件行数后门禁没有报 WARN`);
            }
        }

        // 各条判据各变异一次：分节、围栏、末尾换行、字节取磁盘原始字节、patterns 的判红口径与提示文字。每条变异后自测都必须变红。
        const mutants = [
            ["只让第一个 ## 开节", { sections: (text) => h2Sections(text, { onlyFirst: true }) }],
            ["围栏判定失效", { sections: (text) => h2Sections(text, { isFence: () => false }) }],
            ["末尾换行多算一行", { count: (text) => lineCount(text, { dropTrailing: false }) }],
            ["字节按解码后的文本算（BOM 少算）", { sizeOf: (p, texts) => bytesOf(texts[p]) }],
            ["patterns 里每一项都判红", { isHard: () => true }],
            ["patterns 里没有一项判红", { isHard: () => false }],
            ["patterns 提示文字写死成 ADR", { patternLabel: () => "ADR" }],
        ];
        for (const [label, ov] of mutants) {
            if (runCases(ov).ok) lines.push(`变异「${label}」后自测仍然是绿的`);
        }
        return { ok: lines.length === 0, lines };
    },
};
