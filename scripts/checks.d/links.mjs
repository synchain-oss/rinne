// SPDX-License-Identifier: GPL-3.0-or-later
//
// 文档链接（docs-truth 的一部分）：
//   1. 跟踪的 Markdown（third_party/ 与 LICENSES/ 除外）里的相对链接：目标文件存在；带锚点时，锚点必须是目标 Markdown
//      里某个标题的 GitHub 锚点，算法用 third_party/github-slugger 的规则（重名标题带 -1、-2 后缀；围栏代码与行内代码里的不算）；
//   2. docs/stable-links.txt 锁定的路径与锚点都还在，一行一条 `路径` 或 `路径#锚点`，`#` 开头的行是注释；
//   3. web/、src/、scripts/package*、docs/INSTALL.md 里指向本仓 blob 的链接必须写 blob/prod/，且路径与锚点登记在
//      stable-links 里（web/shared/links.js 是拼接 tag 链接的唯一例外）。
// 判据全在 linkProblems，只吃文本，不碰文件系统。

import path from "node:path";
import { pathToFileURL } from "node:url";
import { ROOT, caseResult, readText, trackedFiles } from "../lib/check-kit.mjs";

const { default: GithubSlugger } = await import(
    pathToFileURL(path.join(ROOT, "third_party", "github-slugger", "index.js")).href
);

const STABLE = "docs/stable-links.txt";
const BLOB_SCOPE = /^(?:web\/|src\/|scripts\/package|docs\/INSTALL\.md$)/;
const BLOB_EXEMPT = new Set(["web/shared/links.js"]);
const BLOB_RE = /github\.com\/synchain-oss\/rinne\/blob\/([^/\s)"'`]+)(\/[^\s)"'`#>]*)?(#[^\s)"'`>]*)?/g;

/** 去掉围栏代码、HTML 注释与行内代码，返回 { text, headings }，headings 是标题原文。 */
function strip(markdown) {
    let fence = false;
    const headings = [];
    const kept = [];
    for (const line of markdown.replace(/<!--[\s\S]*?-->/g, "").split("\n")) {
        if (/^\s*(```|~~~)/.test(line)) {
            fence = !fence;
            continue;
        }
        if (fence) continue;
        const h = /^ {0,3}#{1,6}[ \t]+(.+?)[ \t]*#*[ \t]*$/.exec(line);
        if (h) headings.push(h[1]);
        kept.push(line.replace(/`[^`\n]*`/g, ""));
    }
    return { text: kept.join("\n"), headings };
}

/** 一份 Markdown 里可用的锚点：标题的 GitHub 锚点，加显式的 id / name 属性。 */
function anchorsOf(markdown) {
    const slugger = new GithubSlugger();
    const { text, headings } = strip(markdown);
    const set = new Set(
        headings.map((h) =>
            slugger.slug(
                h
                    .replace(/!?\[([^\]]*)\]\([^)]*\)/g, "$1")
                    .replace(/<[^>]+>/g, "")
                    .replace(/`/g, ""),
            ),
        ),
    );
    for (const m of text.matchAll(/<a\s[^>]*?(?:id|name)="([^"]+)"/g)) set.add(m[1]);
    return set;
}

// 链接文本允许含一层 [..](..)，即徽章写法 [![alt](img)](target)
const LINK = /!?\[((?:[^[\]\\]|\\.|!?\[[^\]]*\]\([^)]*\))*)\]\(\s*(<[^>]*>|[^)\s]*)(?:\s+"[^"]*")?\s*\)/g;

function linksOf(markdown) {
    const { text } = strip(markdown);
    const out = [];
    for (const m of text.matchAll(LINK)) {
        out.push(m[2]);
        if (m[1].includes("](")) out.push(...linksOf(m[1])); // 里层的目标也要查
    }
    for (const m of text.matchAll(/^ {0,3}\[[^\]]+\]:\s*(\S+)/gm)) out.push(m[1]);
    return out.map((t) => t.replace(/^<|>$/g, "")).filter((t) => t && !/^(?:[a-z][a-z0-9+.-]*:|\/\/)/i.test(t));
}

const safeDecode = (s) => {
    try {
        return decodeURIComponent(s);
    } catch {
        return s;
    }
};

/**
 * md：{ 路径: 文本 }，只含要查的 Markdown；all：全部跟踪路径；stable：stable-links.txt 的文本；
 * scan：{ 路径: 文本 }，要扫 blob 链接的文件。返回问题数组。
 */
export function linkProblems({ md, all, stable, scan }) {
    const out = [];
    const tracked = new Set(all);
    const dirs = new Set(
        all.flatMap((p) =>
            p
                .split("/")
                .slice(0, -1)
                .map((_, i, a) => a.slice(0, i + 1).join("/")),
        ),
    );
    const anchors = new Map();
    const anchorsFor = (p) => {
        if (!anchors.has(p)) anchors.set(p, anchorsOf(md[p] ?? ""));
        return anchors.get(p);
    };
    const exists = (p) => tracked.has(p) || dirs.has(p);
    const hasAnchor = (p, a) => !(p in md) || anchorsFor(p).has(safeDecode(a));

    for (const [file, text] of Object.entries(md)) {
        for (const target of linksOf(text)) {
            const [rawPath, ...rest] = target.split("#");
            const anchor = rest.join("#");
            const rel = rawPath
                ? path.posix
                      .normalize(path.posix.join(path.posix.dirname(file), safeDecode(rawPath)))
                      .replace(/(.)\/$/, "$1")
                : file;
            if (rel.startsWith("..")) out.push(`${file}：链接 ${target} 指到仓外`);
            else if (!exists(rel)) out.push(`${file}：链接 ${target} 的目标 ${rel} 不存在`);
            else if (anchor && rel.endsWith(".md") && !hasAnchor(rel, anchor))
                out.push(`${file}：链接 ${target} 的锚点在 ${rel} 里找不到`);
        }
    }

    const locked = new Set();
    for (const raw of stable.split("\n")) {
        const line = raw.trim();
        if (!line || line.startsWith("#")) continue;
        locked.add(line);
        const [p, ...rest] = line.split("#");
        const anchor = rest.join("#");
        if (!tracked.has(p)) out.push(`${STABLE}：锁定的 ${p} 已不存在`);
        else if (anchor && !(p.endsWith(".md") && hasAnchor(p, anchor)))
            out.push(`${STABLE}：锁定的锚点 ${line} 已不存在`);
    }

    for (const [file, text] of Object.entries(scan)) {
        if (BLOB_EXEMPT.has(file)) continue;
        for (const m of text.matchAll(BLOB_RE)) {
            const key = `${(m[2] ?? "").replace(/^\//, "")}${m[3] ?? ""}`;
            if (m[1] !== "prod") out.push(`${file}：blob 链接要写 blob/prod/，现在是 blob/${m[1]}/`);
            else if (!locked.has(safeDecode(key)))
                out.push(`${file}：blob/prod 链接 ${key || "（根）"} 没有登记在 ${STABLE}`);
        }
    }
    return out;
}

export function selfTest() {
    const doc = "# 标题 One\n\n## 同名\n\n## 同名\n\n## `code` and [link](b.md)\n\n```md\n## 围栏里的\n```\n";
    const base = {
        md: { "docs/a.md": doc, "docs/b.md": "# B\n" },
        all: ["docs/a.md", "docs/b.md", "web/app.js", "web/shared/links.js", "docs/INSTALL.md"],
        stable: "# 注释\ndocs/a.md#同名\n",
        scan: {},
    };
    const withLink = (link, extra = {}) => ({
        ...base,
        ...extra,
        md: { ...base.md, "docs/b.md": `[x](${link})\n`, ...extra.md },
    });
    const blob = (url, extra = {}) => ({ ...base, ...extra, scan: { "web/app.js": url } });
    const cases = [
        ["相对链接和 CJK 锚点", false, withLink("a.md#标题-one")],
        ["重名标题的 -1 后缀", false, withLink("a.md#同名-1")],
        ["含行内代码与链接的标题", false, withLink("a.md#code-and-link")],
        [
            "徽章式嵌套链接两层都查",
            false,
            { ...base, md: { ...base.md, "docs/b.md": "[![x](b.md)](a.md#标题-one)\n" } },
        ],
        ["外链与 mailto 不查", false, withLink("https://example.com/x#y")],
        ["同文件锚点", false, { ...base, md: { ...base.md, "docs/b.md": "# B\n\n[s](#b)\n" } }],
        [
            "代码里的链接不查",
            false,
            { ...base, md: { ...base.md, "docs/b.md": "`[x](nope.md)`\n\n```\n[y](nope.md)\n```\n" } },
        ],
        ["已登记的 blob/prod 链接", false, blob("https://github.com/synchain-oss/rinne/blob/prod/docs/a.md#同名")],
        [
            "links.js 不受 blob 限制",
            false,
            { ...base, scan: { "web/shared/links.js": "github.com/synchain-oss/rinne/blob/dev/x" } },
        ],
        ["嵌套链接的外层目标不存在", true, { ...base, md: { ...base.md, "docs/b.md": "[![x](a.md)](nope.md)\n" } }],
        ["嵌套链接的里层目标不存在", true, { ...base, md: { ...base.md, "docs/b.md": "[![x](nope.png)](a.md)\n" } }],
        ["链接目标文件不存在", true, withLink("nope.md")],
        ["锚点不存在", true, withLink("a.md#没有这个")],
        ["围栏代码里的标题不算锚点", true, withLink("a.md#围栏里的")],
        ["同文件锚点不存在", true, { ...base, md: { ...base.md, "docs/b.md": "# B\n\n[s](#c)\n" } }],
        ["锁定的锚点被删", true, { ...base, md: { ...base.md, "docs/a.md": "# 标题 One\n" } }],
        ["锁定的文件被删", true, { ...base, stable: "docs/gone.md\n" }],
        ["blob/dev 链接", true, blob("https://github.com/synchain-oss/rinne/blob/dev/docs/a.md#同名")],
        ["blob/prod 但没登记", true, blob("https://github.com/synchain-oss/rinne/blob/prod/docs/b.md")],
    ].map(([label, bad, input]) => ({
        label,
        bad,
        passed: bad ? linkProblems(input).length > 0 : linkProblems(input).length === 0,
    }));
    return caseResult(cases);
}

export default {
    name: "links",
    group: "docs",
    inputs: [],
    run(ctx) {
        const all = trackedFiles(ctx.root);
        const read = (p) => readText(path.join(ctx.root, p));
        const md = {};
        for (const p of all.filter((f) => /\.md$/i.test(f) && !/^(?:third_party|LICENSES)\//.test(f)))
            md[p] = read(p) ?? "";
        const scan = {};
        for (const p of all.filter((f) => BLOB_SCOPE.test(f))) scan[p] = read(p) ?? "";
        const problems = linkProblems({ md, all, stable: read(STABLE) ?? "", scan });
        if (read(STABLE) === null) problems.push(`缺 ${STABLE}`);
        return { ok: problems.length === 0, lines: problems };
    },
    selfTest,
};
