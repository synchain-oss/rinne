// SPDX-License-Identifier: GPL-3.0-or-later
//
// 许可闭包：THIRD-PARTY-NOTICES.md 的两张表、LICENSES/ 与 third_party/notices/ 三向一致。判据：
//   1. NOTICES 的小节标记一个不少、按序、成对；
//   2. Shipped 表每行至少有一个 SPDX 标识和一份上游声明；表里用到的标识在 LICENSES/ 有全文
//      （只用于构建、又不带声明的行除外：它们不随二进制分发，仓里也没有文件引用那份全文）；
//   3. 表里点名的声明文件都存在；third_party/notices/ 下每个文件（来源表 README.md 除外）都被某一行点名；
//   4. LICENSES/ 下每份全文都被本仓许可或表里某一行引用；
//   5. 每份声明的许可按 REUSE 的规则取整份 REUSE.toml 里最后一个命中的块：这个块必须在 third-party-notices
//      小节里、写明 precedence = "override"（否则 REUSE 会改用原文里的版权行），许可必须等于这份声明该有的标识。行里只有一个标识时它的每份声明都是这个标识；有多个标识时，
//      声明文件与标识按顺序一一对应（VST3 SDK 那一行：GPLv3 那份在前，BSD 那份在后），个数对不上判红；
//   6. NOTICES、REUSE.toml 与许可文件名里不得出现把 JUCE 写成「或更高版本」的标识（JUCE 是 AGPLv3 only）。
// 表格解析同时导出给 modules-closure 用。

import fs from "node:fs";
import path from "node:path";
import { caseResult, j } from "../lib/check-kit.mjs";

export const NOTICES = "THIRD-PARTY-NOTICES.md";
/** NOTICES 一次建好的全部小节；后续工作只改自己那一节。 */
export const NOTICE_SECTIONS = [
    "shipped-table",
    "juce",
    "vst3-sdk",
    "au-sdk",
    "webview2",
    "fonts",
    "catch2",
    "github-slugger",
    "design-runtime",
    "scvb-derived",
];
const PROJECT_LICENSE = "GPL-3.0-or-later";
const NOTICE_DIR = "third_party/notices/";
const BUILD_HEADING = /^##\s.*\bBuild, test and CI only\b/;
const OR_LATER = new RegExp(j("AGPL-3\\.0", "-or-later"), "i");

/** 小节标记：返回 { errors, bodies }，bodies 是 小节名 → 小节内的行。 */
export function sectionsOf(text) {
    const errors = [];
    const bodies = new Map();
    const order = [];
    let open = null;
    for (const line of text.split(/\r?\n/)) {
        const start = /^<!-- section:([a-z0-9-]+) -->$/.exec(line.trim());
        const end = /^<!-- \/section:([a-z0-9-]+) -->$/.exec(line.trim());
        if (start) {
            if (open) errors.push(`小节 ${open} 还没结束就开始了 ${start[1]}`);
            open = start[1];
            order.push(open);
            bodies.set(open, []);
        } else if (end) {
            if (end[1] !== open) errors.push(`小节结束标记 ${end[1]} 与开始标记 ${open ?? "（无）"} 对不上`);
            open = null;
        } else if (open) {
            bodies.get(open).push(line);
        }
    }
    if (open) errors.push(`小节 ${open} 没有结束标记`);
    if (order.join() !== NOTICE_SECTIONS.join()) errors.push(`小节标记应依次为 ${NOTICE_SECTIONS.join("、")}`);
    return { errors, bodies };
}

const cellsOf = (line) =>
    line
        .trim()
        .replace(/^\||\|$/g, "")
        .split("|")
        .map((c) => c.trim());
const ticks = (cell) => [...cell.matchAll(/`([^`]+)`/g)].map((m) => m[1]);

/** lines 里的第一张 Markdown 表格，按表头列名取组件、SPDX 与声明三列。 */
function tableRows(lines, label, errors) {
    const at = lines.findIndex((l, i) => /^\s*\|/.test(l) && /^\s*\|[\s:|-]+\|\s*$/.test(lines[i + 1] || ""));
    if (at < 0) {
        errors.push(`${label}：找不到表格`);
        return [];
    }
    const head = cellsOf(lines[at]).map((c) => c.toLowerCase());
    const col = (name) => head.findIndex((h) => h.startsWith(name));
    const [ci, si, ni] = [col("component"), col("spdx"), col("notice")];
    if (ci < 0 || si < 0 || ni < 0) {
        errors.push(`${label}：表头须有 Component、SPDX、Notice texts 三列`);
        return [];
    }
    const rows = [];
    for (const line of lines.slice(at + 2)) {
        if (!/^\s*\|/.test(line)) break;
        const cells = cellsOf(line);
        rows.push({
            component: (cells[ci] || "").replace(/[`*]/g, "").trim(),
            spdx: ticks(cells[si] || "").flatMap((e) => e.split(/\s+(?:AND|OR|WITH)\s+|[()\s]+/).filter(Boolean)),
            notices: ticks(cells[ni] || "").filter((p) => p.startsWith(NOTICE_DIR)),
        });
    }
    return rows;
}

/** NOTICES 的两张表：shipped（小节 shipped-table 里）与 build（「Build, test and CI only」标题下）。 */
export function parseNotices(text) {
    const { errors, bodies } = sectionsOf(text);
    const shipped = bodies.has("shipped-table") ? tableRows(bodies.get("shipped-table"), "Shipped 表", errors) : [];
    const lines = text.split(/\r?\n/);
    const h = lines.findIndex((l) => BUILD_HEADING.test(l));
    let build = [];
    if (h < 0) {
        errors.push("找不到「Build, test and CI only」一节");
    } else {
        const next = lines.findIndex((l, i) => i > h && /^##\s/.test(l));
        build = tableRows(lines.slice(h + 1, next < 0 ? undefined : next), "Build 表", errors);
    }
    return { errors, shipped, build };
}

const globRe = (g) =>
    new RegExp(
        `^${g
            .replace(/[.+^${}()|[\]\\]/g, "\\$&")
            .replace(/\*\*/g, "\0")
            .replace(/\*/g, "[^/]*")
            .replace(/\?/g, "[^/]")
            .replace(/\0/g, ".*")}$`,
    );

/**
 * REUSE.toml 的全部块，按文件顺序：[{ section, paths: [正则], license }]。section 是块所在的小节
 * （第一个小节标记之前为 null）。REUSE 对一个文件取最后一个命中的块，所以判定要看整份文件。
 */
export function reuseBlocks(toml) {
    const blocks = [];
    let section = null;
    // 切在每个小节标记与每个 [[annotations]] 之前：每一段以其中之一开头（第一段是文件头）
    for (const part of toml.split(/^(?=# --- section:[a-z0-9-]+ ---$|\[\[annotations\]\]\s*$)/m)) {
        const mark = /^# --- section:([a-z0-9-]+) ---$/.exec(part.split(/\r?\n/)[0]);
        if (mark) section = mark[1];
        if (!/^\[\[annotations\]\]/.test(part)) continue;
        const raw = /^path\s*=\s*(\[[^\]]*\]|"[^"]*")/m.exec(part)?.[1] || "";
        const paths = [...raw.matchAll(/"([^"]*)"/g)].map((x) => globRe(x[1]));
        const license = /^SPDX-License-Identifier\s*=\s*"([^"]*)"/m.exec(part)?.[1] || "";
        const precedence = /^precedence\s*=\s*"([^"]*)"/m.exec(part)?.[1] || null;
        blocks.push({ section, paths, license, precedence });
    }
    return blocks;
}

/**
 * notices / reuseToml 是文件文本；licenseFiles 是 LICENSES/ 下的文件名；noticeFiles 是 third_party/notices/
 * 下的相对路径。返回错误列表。
 */
export function checkLicenses({ notices, reuseToml, licenseFiles, noticeFiles }) {
    const { errors, shipped, build } = parseNotices(notices);
    if (!shipped.length) errors.push("Shipped 表没有任何一行");
    const have = new Set(licenseFiles.filter((f) => f.endsWith(".txt")).map((f) => f.slice(0, -4)));
    const referenced = new Set([PROJECT_LICENSE]);
    const named = new Map(); // 声明文件 → [{ where, license }]：点名它的行与它该有的标识
    for (const [kind, rows] of [
        ["Shipped", shipped],
        ["Build", build],
    ]) {
        for (const r of rows) {
            const where = `${kind} 表「${r.component}」`;
            if (!r.spdx.length) errors.push(`${where} 没有 SPDX 标识`);
            if (kind === "Shipped" && !r.notices.length) errors.push(`${where} 没有点名上游声明文件`);
            if (r.spdx.length > 1 && r.notices.length && r.notices.length !== r.spdx.length) {
                errors.push(
                    `${where} 有 ${r.spdx.length} 个标识、${r.notices.length} 份声明：多个标识时两者按顺序一一对应`,
                );
            }
            r.spdx.forEach((id) => referenced.add(id));
            if (kind === "Shipped" || r.notices.length) {
                for (const id of r.spdx) if (!have.has(id)) errors.push(`${where} 用了 ${id}，LICENSES/ 里没有全文`);
            }
            r.notices.forEach((p, i) => {
                const rel = p.slice(NOTICE_DIR.length);
                if (!noticeFiles.includes(rel)) errors.push(`${where} 点名的 ${p} 不存在`);
                const license = r.spdx.length === 1 ? r.spdx[0] : r.spdx[i];
                named.set(rel, [...(named.get(rel) || []), { where, license }]);
            });
        }
    }
    for (const f of noticeFiles) {
        if (f !== "README.md" && !named.has(f)) errors.push(`${NOTICE_DIR}${f} 没有被 NOTICES 的任何一行点名`);
    }
    for (const id of have) if (!referenced.has(id)) errors.push(`LICENSES/${id}.txt 没有被本仓许可或 NOTICES 引用`);
    const blocks = reuseBlocks(reuseToml);
    if (!blocks.some((b) => b.section === "third-party-notices"))
        errors.push("REUSE.toml 的 third-party-notices 小节没有块");
    for (const [rel, uses] of named) {
        if (!noticeFiles.includes(rel)) continue;
        const file = `${NOTICE_DIR}${rel}`;
        const wants = [...new Set(uses.map((u) => u.license).filter(Boolean))];
        if (wants.length > 1) errors.push(`${file} 被几行点名，该有的标识互相矛盾：${wants.join("、")}`);
        const hit = blocks.filter((b) => b.paths.some((re) => re.test(file))).at(-1);
        if (!hit || hit.section !== "third-party-notices") {
            const by = hit ? `最后命中的是 ${hit.section ?? "小节之外"} 的块` : "没有任何块命中";
            errors.push(`${file} 的 REUSE 登记不在 third-party-notices 小节：${by}`);
        } else {
            if (hit.precedence !== "override") {
                const was = hit.precedence ? `写的是 ${hit.precedence}` : "没有写，默认是 closest";
                errors.push(`${file} 的 REUSE 块必须是 precedence = "override"（${was}）`);
            }
            if (wants.length === 1 && hit.license !== wants[0]) {
                errors.push(`${file} 的 REUSE 许可是 ${hit.license}，按 NOTICES 应为 ${wants[0]}`);
            }
        }
    }
    const orLater = [
        [NOTICES, notices],
        ["REUSE.toml", reuseToml],
        ["LICENSES/ 的文件名", licenseFiles.join("\n")],
        [`${NOTICE_DIR} 的文件名`, noticeFiles.join("\n")],
    ].filter(([, t]) => OR_LATER.test(t));
    for (const [where] of orLater)
        errors.push(`${where} 里出现了把 JUCE 写成「或更高版本」的标识（应为 AGPL-3.0-only）`);
    return errors;
}

function listFiles(dir, base = dir) {
    if (!fs.existsSync(dir)) return [];
    return fs.readdirSync(dir, { withFileTypes: true }).flatMap((e) => {
        const abs = path.join(dir, e.name);
        return e.isDirectory() ? listFiles(abs, base) : [path.relative(base, abs).split(path.sep).join("/")];
    });
}

// ---- 自测夹具 ----------------------------------------------------------------------------
const SHIPPED = [
    "| Component | Version | SPDX | Ships in | Notice texts |",
    "| --- | --- | --- | --- | --- |",
    "| JUCE | 8 | `AGPL-3.0-only` | all | `third_party/notices/juce.LICENSE.md` |",
    "| JUCE helper | 8 | `AGPL-3.0-only` | web | `third_party/notices/juce.LICENSE.md` |",
    "| VST3 SDK | 3.7 | `GPL-3.0-only`, `BSD-3-Clause` | vst3 | `third_party/notices/vst3sdk.LICENSE.txt`, `third_party/notices/vst3sdk-base.LICENSE.txt` |",
];
const BUILD = [
    "## 2. Build, test and CI only",
    "",
    "| Component | Version | SPDX | Used for | Notice texts |",
    "| --- | --- | --- | --- | --- |",
    "| Catch2 | 3 | `BSL-1.0` | tests | `third_party/notices/catch2.LICENSE.txt` |",
    "| slug rules | 1 | `ISC` | links | — |",
    "",
    "## 3. Notes",
];
// precedence 为 null 时不写这一行
const block = (p, lic, precedence = "override") =>
    [
        "[[annotations]]",
        `path = ${Array.isArray(p) ? JSON.stringify(p) : `"${p}"`}`,
        ...(precedence ? [`precedence = "${precedence}"`] : []),
        `SPDX-License-Identifier = "${lic}"`,
    ].join("\n");
const REUSE_BLOCKS = [
    ["third_party/notices/juce.*", "AGPL-3.0-only"],
    ["third_party/notices/vst3sdk.*", "GPL-3.0-only"],
    [["third_party/notices/vst3sdk-base.*"], "BSD-3-Clause"],
    ["third_party/notices/catch2.*", "BSL-1.0"],
];
// tail 是放在声明小节之后（third-party-src 小节里）的块
const reuse = (blocks = REUSE_BLOCKS, section = true, tail = []) =>
    [
        "# --- section:base ---",
        block("**", "GPL-3.0-or-later", null),
        ...(section ? ["# --- section:third-party-notices ---"] : []),
        ...blocks.map(([p, l, pr]) => block(p, l, pr)),
        "# --- section:third-party-src ---",
        ...tail.map(([p, l, pr]) => block(p, l, pr)),
    ].join("\n");
const LICS = ["GPL-3.0-or-later", "AGPL-3.0-only", "GPL-3.0-only", "BSD-3-Clause", "BSL-1.0"].map((l) => `${l}.txt`);
const NOTS = ["README.md", "juce.LICENSE.md", "vst3sdk.LICENSE.txt", "vst3sdk-base.LICENSE.txt", "catch2.LICENSE.txt"];
function fixture(o = {}) {
    const { shipped = SHIPPED, order = NOTICE_SECTIONS, extra = [], lic = LICS, not = NOTS, blocks } = o;
    const notices = [
        "# Third-Party Notices",
        ...order.flatMap((s) => [
            `<!-- section:${s} -->`,
            ...(s === "shipped-table" ? shipped : []),
            `<!-- /section:${s} -->`,
        ]),
        ...(o.drop === "build" ? [] : BUILD),
        ...extra,
    ].join("\n");
    return { notices, reuseToml: reuse(blocks, !o.noSection, o.tail), licenseFiles: lic, noticeFiles: not };
}

export function selfTest() {
    const swap = (from, to) => SHIPPED.map((l) => l.replace(from, to));
    const without = (list, x) => list.filter((f) => f !== x);
    const cases = [
        ["完整一致", false, {}],
        [
            "SPDX 写成表达式",
            false,
            { shipped: swap("`GPL-3.0-only`, `BSD-3-Clause`", "`GPL-3.0-only AND BSD-3-Clause`") },
        ],
        ["后面的 REUSE 块覆盖前面的", false, { blocks: [["third_party/notices/*", "MIT"], ...REUSE_BLOCKS] }],
        ["出现 AGPL 或更高版本的标识", true, { extra: [`JUCE is ${j("AGPL-3.0", "-or-later")}.`] }],
        ["LICENSES/ 下有未被引用的全文", true, { lic: [...LICS, "MIT.txt"] }],
        ["Shipped 行的许可没有全文", true, { lic: without(LICS, "BSD-3-Clause.txt") }],
        ["带声明的 Build 行的许可没有全文", true, { lic: without(LICS, "BSL-1.0.txt") }],
        ["notices 下有没人点名的文件", true, { not: [...NOTS, "zlib.LICENSE"] }],
        ["点名的声明文件不存在", true, { not: without(NOTS, "catch2.LICENSE.txt") }],
        ["Shipped 行没有上游声明", true, { shipped: swap("`third_party/notices/juce.LICENSE.md` |", "— |") }],
        ["行里没有 SPDX 标识", true, { shipped: swap("`AGPL-3.0-only` | web", "AGPL | web") }],
        ["Shipped 表是空的", true, { shipped: SHIPPED.slice(0, 2) }],
        ["缺 Build 表一节", true, { drop: "build" }],
        ["REUSE.toml 缺 third-party-notices 小节", true, { noSection: true }],
        ["缺一个小节", true, { order: NOTICE_SECTIONS.filter((s) => s !== "fonts") }],
        ["小节顺序不对", true, { order: ["juce", "shipped-table", ...NOTICE_SECTIONS.slice(2)] }],
        ["声明在 REUSE 里没有块", true, { blocks: REUSE_BLOCKS.filter(([p]) => p !== "third_party/notices/catch2.*") }],
        [
            "声明的 REUSE 许可与表里不符",
            true,
            { blocks: REUSE_BLOCKS.map(([p, l]) => [p, p.includes("juce") ? "GPL-3.0-or-later" : l]) },
        ],
        [
            "vst3sdk 与 vst3sdk-base 的 REUSE 许可对调",
            true,
            {
                blocks: REUSE_BLOCKS.map(([p, l]) =>
                    l === "GPL-3.0-only" ? [p, "BSD-3-Clause"] : l === "BSD-3-Clause" ? [p, "GPL-3.0-only"] : [p, l],
                ),
            },
        ],
        [
            "声明的块没写 precedence",
            true,
            { blocks: REUSE_BLOCKS.map(([p, l]) => (`${p}`.includes("juce") ? [p, l, null] : [p, l])) },
        ],
        [
            "声明的块 precedence 写成 closest",
            true,
            { blocks: REUSE_BLOCKS.map(([p, l]) => (`${p}`.includes("catch2") ? [p, l, "closest"] : [p, l])) },
        ],
        ["后面小节的通配块盖过了声明的块", true, { tail: [["third_party/**", "GPL-3.0-or-later"]] }],
        ["后面小节的块不碰声明", false, { tail: [["third_party/miniz/**", "MIT"]] }],
        [
            "多个标识但声明份数对不上",
            true,
            {
                shipped: swap(", `third_party/notices/vst3sdk-base.LICENSE.txt` |", " |"),
                not: without(NOTS, "vst3sdk-base.LICENSE.txt"),
            },
        ],
    ].map(([label, bad, opts]) => {
        const errors = checkLicenses(fixture(opts));
        return { label, bad, passed: bad ? errors.length > 0 : errors.length === 0 };
    });
    const unclosed = sectionsOf(["<!-- section:shipped-table -->", "<!-- section:juce -->"].join("\n")).errors;
    cases.push({ label: "小节没有结束标记", bad: true, passed: unclosed.length > 0 });
    return caseResult(cases);
}

export default {
    name: "licenses-closure",
    group: "compliance",
    inputs: [],
    run(ctx) {
        const read = (rel) =>
            fs.existsSync(path.join(ctx.root, rel)) ? fs.readFileSync(path.join(ctx.root, rel), "utf8") : null;
        const notices = read(NOTICES);
        const reuseToml = read("REUSE.toml");
        const missing = [
            [NOTICES, notices],
            ["REUSE.toml", reuseToml],
        ].filter(([, t]) => t === null);
        if (missing.length) return { ok: false, lines: missing.map(([f]) => `缺 ${f}`) };
        const errors = checkLicenses({
            notices,
            reuseToml,
            licenseFiles: listFiles(path.join(ctx.root, "LICENSES")),
            noticeFiles: listFiles(path.join(ctx.root, NOTICE_DIR)),
        });
        return { ok: errors.length === 0, lines: errors };
    },
    selfTest,
};
