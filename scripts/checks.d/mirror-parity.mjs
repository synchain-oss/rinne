// SPDX-License-Identifier: GPL-3.0-or-later
//
// 中英镜像对拍（docs-truth 的一部分）。对拍对象是 scripts/lib/rules.mjs 的 MIRROR_PAIRS：
// README.md ↔ README.zh-CN.md、docs/USER_GUIDE.md ↔ docs/USER_GUIDE.zh-CN.md。
// 跨语言只比结构，不比散文：中英散文必然不同，比散文没有意义。
//
// 规范化规则：
//   a 每一对的两个文件的第一行整行排除（语言切换行）；换行统一成 LF，CRLF 的文件与 LF 的文件一样判；
//   b 链接目标：X.md 与 X.zh-CN.md 视同同一目标；锚点按「第 k 个标题」对应（标题序列本来就强制一致）。
//     锚点相对链接所在的文件解析（页内锚点 #x 指向本文件，docs/ 下的相对链接以 docs/ 为基准）；
//     两侧必须同时有或同时没有锚点；目标文件在、锚点却找不到，或两侧锚点对应的标题序号不同，都判红；
//     目标文件不存在由 links 检查管，这里不重复报；外链与非 Markdown 目标的锚点按字面比对；
//   c 只比 info string ∈ {powershell,pwsh,sh,bash,zsh,console} 的代码块，比较前去掉注释行；其余围栏只比数量与位置；
//   d 真源节 ↔ 生成块：真源节取 rules.mjs 的 SOURCE_BLOCKS，生成块区域从 gen-docs 各模块的 surfaces 推导。
//
// 判据名见 RULES，报错行以 [名字] 开头。自测的每个坏样例写明它必须触发哪几条判据，并核对每条判据都至少被一个坏样例
// 点名：删掉任何一条判据，点名它的坏样例就变红。--self-test 里有一对正确的中英夹具作正例。
// 放置图与契约表两个生成块模块的自测也在这里跑：它们的落地面就是这些镜像文档与契约文档。

import fs from "node:fs";
import path from "node:path";
import { pathToFileURL } from "node:url";
import { ROOT, caseResult, readText } from "../lib/check-kit.mjs";
import { MIRROR_PAIRS, SOURCE_BLOCKS } from "../lib/rules.mjs";
import { selfTestCases as contractTablesCases } from "../gen-docs.d/contract-tables.mjs";
import { selfTestCases as placementCases } from "../gen-docs.d/placement.mjs";

const { default: GithubSlugger } = await import(
    pathToFileURL(path.join(ROOT, "third_party", "github-slugger", "index.js")).href
);

/** 判据名 → 说明。 */
export const RULES = {
    headings: "两侧的标题层级序列相同",
    links: "两侧的链接目标序列相同（X.md 与 X.zh-CN.md 视同同一目标，锚点按标题序号对应）",
    "code-blocks": "可比代码块（info string 在集合里）的数量与去注释后的内容相同",
    fences: "全部围栏代码块的数量与所在位置相同",
    lists: "顶层列表项条数按节对应相同",
    tables: "表格的行列数按节对应相同",
    "generated-blocks": "生成块在两侧同时存在或同时不存在，落位对应",
    "source-section": "真源节在中文侧存在，英文侧有对应的生成块",
};

/** 可比代码块的 info string。 */
const COMPARABLE = new Set(["powershell", "pwsh", "sh", "bash", "zsh", "console"]);

const HEADING_RE = /^ {0,3}(#{1,6})[ \t]+/;
const LIST_RE = /^ {0,3}(?:[-*+]|\d+[.)])[ \t]+/;
const LINK_RE = /!?\[[^\]]*\]\([ \t]*(<[^>]*>|[^)\s]*)(?:[ \t]+"[^"]*")?[ \t]*\)/g;
const REF_RE = /^ {0,3}\[[^\]]+\]:[ \t]*(\S+)/gm;
const GEN_RE = /<!--[ \t]*BEGIN[ \t]+(gen:[^\s>]+)[ \t]*-->/g;
const FENCE_RE = /^[ \t]*(?:```|~~~)[ \t]*(.*)$/;

/** 链接目标归一：去掉锚点、去掉开头的 ./、把 X.zh-CN.md 视同 X.md。 */
export function normalizeTarget(target) {
    const p = target.replace(/^<|>$/g, "").split("#")[0].replace(/^\.\//, "");
    return p.replace(/\.zh-CN\.md$/i, ".md");
}

/** 链接拆成 { path, anchor, external }；path 不含锚点，外链（带协议或 //）的 path 原样保留。 */
export function splitTarget(target) {
    const t = target.replace(/^<|>$/g, "");
    const hash = t.indexOf("#");
    const p = hash < 0 ? t : t.slice(0, hash);
    return { path: p, anchor: hash < 0 ? "" : t.slice(hash + 1), external: /^(?:[a-z][a-z0-9+.-]*:|\/\/)/i.test(p) };
}

/**
 * 链接目标相对「链接所在的文件」解析成仓根相对路径：空路径是文件自己，开头的 / 是仓根，其余相对文件所在目录。
 * 走出仓根返回 null。
 */
export function resolveFrom(file, linkPath) {
    if (!linkPath) return file;
    const joined = linkPath.startsWith("/") ? linkPath.slice(1) : path.posix.join(path.posix.dirname(file), linkPath);
    const norm = path.posix.normalize(joined);
    return norm.startsWith("../") || norm === ".." ? null : norm;
}

/** 换行统一成 LF。 */
const lf = (text) => text.replace(/\r\n?/g, "\n");

/**
 * 把一份 Markdown 拆成结构。规则 a：调用方已经把第一行去掉。
 * 返回 { headings, links, blocks, generated, sections }；section 的 path 是标题的序号路径（如 "1.2"）。
 */
export function parseDoc(text) {
    const headings = [];
    const links = [];
    const blocks = [];
    const generated = [];
    const sections = [{ path: "", lists: 0, tableRows: 0, tableCols: [] }];
    let cur = sections[0];
    let counters = [];
    let fence = null;
    for (const line of lf(text).split("\n")) {
        const f = FENCE_RE.exec(line);
        if (f) {
            if (!fence) fence = { info: f[1].trim(), body: [], path: counters.join(".") };
            else {
                blocks.push({ info: fence.info, body: fence.body.join("\n"), path: fence.path });
                fence = null;
            }
            continue;
        }
        if (fence) {
            fence.body.push(line);
            continue;
        }
        const h = HEADING_RE.exec(line);
        if (h) {
            const level = h[1].length;
            const prev = counters[level - 1] ?? 0;
            counters = counters.slice(0, level - 1);
            counters[level - 1] = prev + 1;
            headings.push(level);
            cur = { path: counters.join("."), lists: 0, tableRows: 0, tableCols: [] };
            sections.push(cur);
            continue;
        }
        for (const m of line.matchAll(LINK_RE)) links.push(m[1]);
        for (const m of line.matchAll(REF_RE)) links.push(m[1]);
        for (const m of line.matchAll(GEN_RE)) generated.push({ name: m[1], path: counters.join(".") });
        if (LIST_RE.test(line)) cur.lists++;
        if (/^[ \t]*\|/.test(line)) {
            cur.tableRows++;
            cur.tableCols.push(line.split("|").length - 2);
        }
    }
    return { headings, links, blocks, generated, sections };
}

/** 代码块去注释：整行是 # 或 // 开头的注释行去掉，首尾空白去掉。 */
export function stripComments(body) {
    return body
        .split("\n")
        .filter((l) => !/^[ \t]*(?:#|\/\/)/.test(l))
        .map((l) => l.replace(/[ \t]+$/, ""))
        .join("\n")
        .trim();
}

const same = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);

/** 一份文档里某个生成块标记在不在。 */
const hasGenerated = (doc, name) => doc.generated.some((g) => g.name === name);

/**
 * 对拍一对文件。en / zh 是 { path, text }；surfaces 是模块名 → 落地面数组；
 * sourceBlocks 是 SOURCE_BLOCKS；resolveAnchor(仓根相对路径, 锚点) 返回 { state, index }：state 为 ok（index 是标题序号）、
 * missing-file（目标文件不存在）或 no-anchor（文件在、没有这个锚点）。
 * 返回问题数组（每行以 [判据名] 开头）。
 */
export function mirrorProblems({
    en,
    zh,
    surfaces = {},
    sourceBlocks = [],
    resolveAnchor = () => ({ state: "missing-file" }),
}) {
    const out = [];
    const add = (rule, message) => {
        if (!Object.hasOwn(RULES, rule)) throw new Error(`未登记的判据 ${rule}`);
        out.push(`[${rule}] ${message}`);
    };
    en = { ...en, text: lf(en.text) };
    zh = { ...zh, text: lf(zh.text) };
    const a = parseDoc(en.text.split("\n").slice(1).join("\n"));
    const b = parseDoc(zh.text.split("\n").slice(1).join("\n"));

    // 标题层级序列
    if (!same(a.headings, b.headings))
        add("headings", `${en.path} 与 ${zh.path} 的标题层级序列不同（${a.headings} / ${b.headings}）`);

    // 链接目标序列（按 b 归一）
    const na = a.links.map(normalizeTarget);
    const nb = b.links.map(normalizeTarget);
    if (!same(na, nb)) add("links", `${en.path} 与 ${zh.path} 的链接目标序列不同（${na.length} / ${nb.length} 个）`);
    else
        for (let i = 0; i < na.length; i++) {
            const ta = splitTarget(a.links[i]);
            const tb = splitTarget(b.links[i]);
            const where = `${en.path} 与 ${zh.path} 的第 ${i + 1} 个链接`;
            if (!ta.anchor && !tb.anchor) continue;
            if (!ta.anchor || !tb.anchor) {
                add("links", `${where}只有一侧写了锚点（${ta.anchor || "无"} / ${tb.anchor || "无"}）`);
                continue;
            }
            // 外链和非 Markdown 目标（如 LICENSE#L3）的锚点不是本仓标题，按字面比对
            const notMd = (t, file) => !/\.md$/i.test(t.path || file);
            if (ta.external || tb.external || notMd(ta, en.path) || notMd(tb, zh.path)) {
                if (ta.anchor !== tb.anchor) add("links", `${where}的锚点字面不同（#${ta.anchor} / #${tb.anchor}）`);
                continue;
            }
            const pa = resolveFrom(en.path, ta.path);
            const pb = resolveFrom(zh.path, tb.path);
            if (pa === null || pb === null) {
                add("links", `${where}走出了仓根`);
                continue;
            }
            const ra = resolveAnchor(pa, ta.anchor);
            const rb = resolveAnchor(pb, tb.anchor);
            if (ra.state === "missing-file" || rb.state === "missing-file") continue; // 目标文件不存在由 links 检查管
            if (ra.state === "no-anchor") add("links", `${where}的锚点 ${pa}#${ta.anchor} 找不到对应的标题`);
            if (rb.state === "no-anchor") add("links", `${where}的锚点 ${pb}#${tb.anchor} 找不到对应的标题`);
            if (ra.state === "ok" && rb.state === "ok" && ra.index !== rb.index)
                add("links", `${where}的锚点对应的标题序号不同（${ra.index} / ${rb.index}）`);
        }

    // 可比代码块
    const ca = a.blocks.filter((x) => COMPARABLE.has(x.info.toLowerCase().split(/\s+/)[0]));
    const cb = b.blocks.filter((x) => COMPARABLE.has(x.info.toLowerCase().split(/\s+/)[0]));
    if (ca.length !== cb.length)
        add("code-blocks", `${en.path} 与 ${zh.path} 的可比代码块数量不同（${ca.length} / ${cb.length}）`);
    else
        for (let i = 0; i < ca.length; i++)
            if (stripComments(ca[i].body) !== stripComments(cb[i].body))
                add("code-blocks", `${en.path} 与 ${zh.path} 的第 ${i + 1} 个可比代码块内容不同`);

    // 全部围栏：数量与位置
    if (a.blocks.length !== b.blocks.length)
        add("fences", `${en.path} 与 ${zh.path} 的围栏代码块数量不同（${a.blocks.length} / ${b.blocks.length}）`);
    else
        for (let i = 0; i < a.blocks.length; i++)
            if (a.blocks[i].path !== b.blocks[i].path)
                add("fences", `${en.path} 与 ${zh.path} 的第 ${i + 1} 个围栏代码块位置不同`);

    // 顶层列表项条数与表格行列：按节对应
    const n = Math.min(a.sections.length, b.sections.length);
    for (let i = 0; i < n; i++) {
        if (a.sections[i].lists !== b.sections[i].lists)
            add(
                "lists",
                `${en.path} 与 ${zh.path} 的第 ${i + 1} 节顶层列表项条数不同（${a.sections[i].lists} / ${b.sections[i].lists}）`,
            );
        if (a.sections[i].tableRows !== b.sections[i].tableRows)
            add(
                "tables",
                `${en.path} 与 ${zh.path} 的第 ${i + 1} 节表格行数不同（${a.sections[i].tableRows} / ${b.sections[i].tableRows}）`,
            );
        if (!same(a.sections[i].tableCols, b.sections[i].tableCols))
            add("tables", `${en.path} 与 ${zh.path} 的第 ${i + 1} 节表格列数不同`);
    }

    // 生成块：两侧同时存在或同时不存在，且落位对应
    for (const [module, list_] of Object.entries(surfaces)) {
        const enSurf = list_.find((s) => s.file === en.path);
        const zhSurf = list_.find((s) => s.file === zh.path);
        if (!enSurf || !zhSurf) continue; // 只有一侧声明落地面时无法对拍，交给 source-section
        const enName = `gen:${module}:${enSurf.id}`;
        const zhName = `gen:${module}:${zhSurf.id}`;
        const he = hasGenerated(a, enName);
        const hz = hasGenerated(b, zhName);
        if (he !== hz)
            add("generated-blocks", `${en.path} 与 ${zh.path} 的生成块 ${he ? enName : zhName} 只有一侧存在`);
        else if (he) {
            const pe = a.generated.find((g) => g.name === enName).path;
            const pz = b.generated.find((g) => g.name === zhName).path;
            if (pe !== pz) add("generated-blocks", `${en.path} 与 ${zh.path} 的生成块 ${enName} 落位不同`);
        }
    }

    // 真源节 ↔ 生成块
    for (const s of sourceBlocks) {
        const [file, heading] = s.source.split("#");
        if (file !== zh.path && file !== en.path) continue;
        const side = file === zh.path ? zh : en;
        const other = file === zh.path ? en : zh;
        const doc = file === zh.path ? a : b;
        const escaped = heading.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
        if (!new RegExp(`^ {0,3}#{1,6}[ \\t]+${escaped}[ \\t]*$`, "m").test(side.text))
            add("source-section", `${side.path} 缺真源节「${heading}」`);
        for (const blk of s.blocks)
            if (!hasGenerated(doc, `gen:${blk}`))
                add("source-section", `${other.path} 缺真源节对应的生成块 gen:${blk}`);
    }
    return out;
}

/** 读出 gen-docs 各模块声明的落地面；模块加载失败就当没有落地面。 */
async function loadSurfaces() {
    const dir = path.join(ROOT, "scripts", "gen-docs.d");
    const out = {};
    let files = [];
    try {
        files = fs.readdirSync(dir).filter((f) => f.endsWith(".mjs"));
    } catch {
        return out;
    }
    for (const file of files) {
        try {
            const m = (await import(pathToFileURL(path.join(dir, file)).href)).default;
            if (m && typeof m.name === "string" && Array.isArray(m.surfaces)) out[m.name] = m.surfaces;
        } catch {
            // 模块加载不了时按「没有落地面」处理；gen-docs 检查会另外报它
        }
    }
    return out;
}

/** 一份文档里的标题（围栏里的不算）与它们的 GitHub 锚点。 */
export function anchorsOf(markdown) {
    const slugger = new GithubSlugger();
    const out = [];
    let fence = false;
    for (const line of lf(markdown ?? "").split("\n")) {
        if (/^[ \t]*(?:```|~~~)/.test(line)) {
            fence = !fence;
            continue;
        }
        if (fence) continue;
        const h = /^ {0,3}#{1,6}[ \t]+(.+?)[ \t]*#*[ \t]*$/.exec(line);
        if (h) out.push([h[1], slugger.slug(h[1])]);
    }
    return out;
}

/**
 * 仓根相对路径 + 锚点 → { state, index }：state 是 ok（index 为标题序号，1 起）、
 * missing-file（目标文件不存在）、no-anchor（文件在、没有这个锚点）。
 */
function makeResolver(root) {
    const cache = new Map();
    const headings = (file) => {
        if (!cache.has(file)) {
            const text = readText(path.join(root, file));
            cache.set(file, text === null ? null : anchorsOf(text));
        }
        return cache.get(file);
    };
    return (file, anchor) => {
        const list = headings(file);
        if (list === null) return { state: "missing-file" };
        const i = list.findIndex(([, slug]) => slug === anchor);
        return i < 0 ? { state: "no-anchor" } : { state: "ok", index: i + 1 };
    };
}

/** 一对文件的正文与路径；文件不在就返回 null。 */
function loadPair(root, pair) {
    const enText = readText(path.join(root, pair.en));
    const zhText = readText(path.join(root, pair.zh));
    if (enText === null || zhText === null) return null;
    return { en: { path: pair.en, text: enText }, zh: { path: pair.zh, text: zhText } };
}

export async function selfTest(root = ROOT) {
    const surfaces = {
        placement: [
            { file: "README.md", id: "en" },
            { file: "README.zh-CN.md", id: "zh" },
        ],
        "safety-warning": [
            { file: "README.md", id: "en" },
            { file: "README.zh-CN.md", id: "zh" },
            { file: "docs/USER_GUIDE.md", id: "en" },
        ],
        "hard-rules": [
            { file: "README.md", id: "en" },
            { file: "README.zh-CN.md", id: "zh" },
            { file: "docs/USER_GUIDE.md", id: "en" },
        ],
    };
    const sourceBlocks = [
        { source: "docs/USER_GUIDE.zh-CN.md#使用前必读", blocks: ["safety-warning:en", "hard-rules:en"] },
    ];

    const readmeEn = [
        "**English** | [简体中文](README.zh-CN.md)",
        "",
        "# Synchain Rinne",
        "",
        "## What it does",
        "",
        "Rinne is two plugins. Jump to [install](#install).",
        "",
        "- Input first",
        "- Output last",
        "",
        "## Where the plugins go",
        "",
        "<!-- BEGIN gen:placement:en -->",
        "",
        "```",
        "Insert slot 1",
        "```",
        "",
        "<!-- END gen:placement:en -->",
        "",
        "See the [guide](docs/USER_GUIDE.md#read-this-first) and the [license](LICENSE).",
        "",
        "| host | tier |",
        "|---|---|",
        "| Cubase | 1 |",
        "",
        "## Install",
        "",
        "```powershell",
        "# a comment",
        "Get-FileHash .\\zip",
        "```",
        "",
    ].join("\n");
    const readmeZh = [
        "[English](README.md) | **简体中文**",
        "",
        "# Synchain Rinne",
        "",
        "## 功能",
        "",
        "Rinne 是两个插件。跳到[安装](#安装)。",
        "",
        "- Input 第一格",
        "- Output 最后一格",
        "",
        "## 插件放在哪里",
        "",
        "<!-- BEGIN gen:placement:zh -->",
        "",
        "```",
        "Insert slot 1",
        "```",
        "",
        "<!-- END gen:placement:zh -->",
        "",
        "见[用户指南](docs/USER_GUIDE.zh-CN.md#使用前必读)与[许可](LICENSE)。",
        "",
        "| 宿主 | 等级 |",
        "|---|---|",
        "| Cubase | 1 |",
        "",
        "## 安装",
        "",
        "```powershell",
        "Get-FileHash .\\zip",
        "```",
        "",
    ].join("\n");
    const guideEn = [
        "# Rinne user guide",
        "",
        "## Read this first",
        "",
        "<!-- BEGIN gen:safety-warning:en -->",
        "",
        "> [!WARNING]",
        "",
        "<!-- END gen:safety-warning:en -->",
        "",
        "<!-- BEGIN gen:hard-rules:en -->",
        "",
        "1. **Placement**: text",
        "",
        "<!-- END gen:hard-rules:en -->",
        "",
        "## Another",
        "",
        "Text. Back to the [readme](../README.md#install).",
        "",
    ].join("\n");
    const guideZh = [
        "# Rinne 用户指南",
        "",
        "## 使用前必读",
        "",
        "反馈可能在一瞬间变得很响。",
        "",
        "1. **放置位置**：Rinne Input 插在第一格。",
        "",
        "## 另一节",
        "",
        "正文。回到[自述](../README.zh-CN.md#安装)。",
        "",
    ].join("\n");

    const docs = {
        "README.md": readmeEn,
        "README.zh-CN.md": readmeZh,
        "docs/USER_GUIDE.md": guideEn,
        "docs/USER_GUIDE.zh-CN.md": guideZh,
    };
    const base = () => ({
        pairs: [
            { en: { path: "README.md", text: readmeEn }, zh: { path: "README.zh-CN.md", text: readmeZh } },
            {
                en: { path: "docs/USER_GUIDE.md", text: guideEn },
                zh: { path: "docs/USER_GUIDE.zh-CN.md", text: guideZh },
            },
        ],
        surfaces,
        sourceBlocks,
        resolveAnchor: (file, anchor) => {
            if (!(file in docs)) return { state: "missing-file" };
            const i = anchorsOf(docs[file]).findIndex(([, slug]) => slug === anchor);
            return i < 0 ? { state: "no-anchor" } : { state: "ok", index: i + 1 };
        },
    });
    /** 改一份文件的内容（side 是 "en" 或 "zh"）。 */
    const edit = (over, side, path_, fn) => ({
        ...over,
        pairs: over.pairs.map((p) => {
            const key = side === "en" ? "en" : "zh";
            return p[key].path === path_ ? { ...p, [key]: { ...p[key], text: fn(p[key].text) } } : p;
        }),
    });
    const drop = (over, side, path_, marker) => edit(over, side, path_, (t) => t.replace(marker, ""));
    /** 所有文件的换行改成 CRLF（Windows 工具新写的文件）。 */
    const toCrlf = (over) => ({
        ...over,
        pairs: over.pairs.map((p) => ({
            en: { ...p.en, text: p.en.text.replace(/\n/g, "\r\n") },
            zh: { ...p.zh, text: p.zh.text.replace(/\n/g, "\r\n") },
        })),
    });

    const cases = [
        ["一对正确的中英文档", [], base()],
        [
            "英文侧少一个二级标题",
            ["headings"],
            edit(base(), "en", "README.md", (t) => t.replace("## Install\n\n```powershell", "```powershell")),
        ],
        [
            "中英标题层级序列不同",
            ["headings"],
            edit(base(), "en", "README.md", (t) => t.replace("## Install", "### Install")),
        ],
        [
            "英文侧多一个链接",
            ["links"],
            edit(base(), "en", "README.md", (t) => `${t}\nSee also [the site](https://example.com/x).\n`),
        ],
        [
            "两侧代码块内容不同",
            ["code-blocks"],
            edit(base(), "en", "README.md", (t) => t.replace("Get-FileHash .\\zip", "Get-FileHash .\\other")),
        ],
        ["英文侧少一个列表项", ["lists"], edit(base(), "en", "README.md", (t) => t.replace("- Output last\n", ""))],
        [
            "表格列数不同",
            ["tables"],
            edit(base(), "en", "README.md", (t) => t.replace("| host | tier |", "| host | tier | extra |")),
        ],
        [
            "英文侧少一个不可比围栏",
            ["fences"],
            edit(base(), "en", "README.md", (t) => t.replace("```\nInsert slot 1\n```\n\n", "")),
        ],
        [
            "中文侧的真源节被删",
            ["source-section"],
            edit(base(), "zh", "docs/USER_GUIDE.zh-CN.md", (t) => t.replace("## 使用前必读", "## 别的小节")),
        ],
        [
            "英文侧缺真源对应的生成块",
            ["source-section"],
            drop(base(), "en", "docs/USER_GUIDE.md", "<!-- BEGIN gen:hard-rules:en -->"),
        ],
        [
            "生成块只有一侧存在",
            ["generated-blocks"],
            drop(base(), "zh", "README.zh-CN.md", "<!-- BEGIN gen:placement:zh -->"),
        ],
        [
            "生成块落位不同",
            ["generated-blocks"],
            edit(base(), "en", "README.md", (t) =>
                t.replace(
                    "## What it does\n\nRinne is two plugins. Jump to [install](#install).\n\n- Input first\n- Output last\n\n## Where the plugins go\n",
                    "## Where the plugins go\n",
                ),
            ),
        ],
        [
            "页内锚点两侧对应的标题序号不同",
            ["links"],
            edit(base(), "zh", "README.zh-CN.md", (t) => t.replace("(#安装)", "(#功能)")),
        ],
        [
            "docs 下的相对链接锚点两侧对应的标题序号不同",
            ["links"],
            edit(base(), "zh", "docs/USER_GUIDE.zh-CN.md", (t) => t.replace("#安装", "#功能")),
        ],
        [
            "一侧写了锚点、另一侧没写",
            ["links"],
            edit(base(), "zh", "docs/USER_GUIDE.zh-CN.md", (t) => t.replace("README.zh-CN.md#安装", "README.zh-CN.md")),
        ],
        [
            "锚点在目标文件里找不到标题",
            ["links"],
            edit(base(), "zh", "README.zh-CN.md", (t) => t.replace("(#安装)", "(#不存在)")),
        ],
        [
            "英文侧的页内锚点找不到标题",
            ["links"],
            edit(base(), "en", "README.md", (t) => t.replace("(#install)", "(#nothing)")),
        ],
        [
            "链接走出了仓根",
            ["links"],
            edit(
                edit(base(), "en", "README.md", (t) => `${t}\nSee [o](../outside.md#a).\n`),
                "zh",
                "README.zh-CN.md",
                (t) => `${t}\n见[外](../outside.md#a)。\n`,
            ),
        ],
        [
            "目标文件不存在时不在这里报（交给 links 检查）",
            [],
            edit(
                edit(base(), "en", "README.md", (t) => `${t}\nSee [n](docs/NOPE.md#a).\n`),
                "zh",
                "README.zh-CN.md",
                (t) => `${t}\n见[无](docs/NOPE.zh-CN.md#a)。\n`,
            ),
        ],
        [
            "英文侧的命令块不再标 powershell",
            ["code-blocks"],
            edit(base(), "en", "README.md", (t) => t.replace("```powershell", "```text")),
        ],
        [
            "围栏所在的节不同",
            ["fences"],
            edit(base(), "en", "README.md", (t) =>
                t.replace(
                    "<!-- BEGIN gen:placement:en -->\n\n```",
                    "<!-- BEGIN gen:placement:en -->\n\n### Sub\n\n```",
                ),
            ),
        ],
        [
            "外链的锚点两侧字面不同",
            ["links"],
            edit(
                edit(base(), "en", "README.md", (t) => `${t}\nSee [ext](https://example.com/p#a).\n`),
                "zh",
                "README.zh-CN.md",
                (t) => `${t}\n见[外部](https://example.com/p#b)。\n`,
            ),
        ],
        [
            "CRLF 的文件里两侧代码块内容不同",
            ["code-blocks"],
            toCrlf(edit(base(), "en", "README.md", (t) => t.replace("Get-FileHash .\\zip", "Get-FileHash .\\other"))),
        ],
        ["两侧都是 CRLF 的正确文档", [], toCrlf(base())],
        [
            "外链两侧锚点字面相同",
            [],
            edit(
                edit(base(), "en", "README.md", (t) => `${t}\nSee [ext](https://example.com/p#a).\n`),
                "zh",
                "README.zh-CN.md",
                (t) => `${t}\n见[外部](https://example.com/p#a)。\n`,
            ),
        ],
        // 不得误报的三条
        ["X.md 写成 X.zh-CN.md 视同同一目标", [], base()],
        ["语言切换行内容不同", [], base()],
        ["中英散文完全不同", [], base()],
    ].map(([label, expect, input]) => {
        const errors = input.pairs.flatMap((p) => mirrorProblems({ ...input, ...p }));
        const missed = expect.filter((r) => !errors.some((e) => e.startsWith(`[${r}] `)));
        const passed = expect.length ? missed.length === 0 : errors.length === 0;
        return {
            label: missed.length ? `${label}（没有触发 ${missed.join("、")}）` : label,
            bad: !!expect.length,
            passed,
            expect,
        };
    });
    const unnamed = Object.keys(RULES).filter((r) => !cases.some((c) => c.expect.includes(r)));
    cases.push({
        label: `每条判据都有坏样例点名${unnamed.length ? `（没有：${unnamed.join("、")}）` : ""}`,
        bad: false,
        passed: unnamed.length === 0,
        expect: [],
    });
    cases.push(...placementCases().map((c) => ({ ...c, label: `placement 块：${c.label}` })));
    cases.push(...contractTablesCases().map((c) => ({ ...c, label: `contract-tables 块：${c.label}` })));
    return caseResult(cases);
}

export default {
    name: "mirror-parity",
    group: "docs",
    inputs: [],
    async run(ctx) {
        const surfaces = await loadSurfaces();
        const resolveAnchor = makeResolver(ctx.root);
        const out = [];
        for (const pair of MIRROR_PAIRS) {
            const p = loadPair(ctx.root, pair);
            if (!p) continue; // 一对里有一份不存在就整对跳过
            out.push(
                ...mirrorProblems({
                    en: p.en,
                    zh: p.zh,
                    surfaces,
                    sourceBlocks: SOURCE_BLOCKS,
                    resolveAnchor,
                }),
            );
        }
        return { ok: out.length === 0, lines: out };
    },
    selfTest: (ctx) => selfTest(ctx.root),
};
