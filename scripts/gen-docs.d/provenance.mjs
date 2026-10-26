// SPDX-License-Identifier: GPL-3.0-or-later
//
// 逐文件来源表：发行包里那份 THIRD-PARTY-NOTICES.md 的「本仓文件从哪来」一节由它渲染。
//
// 真源是 upstream/*.json（来源登记）：每个片段写清复制自参考仓的哪个提交、哪个文件的哪个 blob。
// 这张表把登记摊平成一行一个文件：本仓路径、上游仓@上游提交、上游路径、改写方式。
//
// 判据（纯函数，吃登记与文件列表）：
//   1. 每个登记的文件在本仓里都要存在：登记指向不存在的文件，渲染时抛错。
//   2. 登记与文件头必须一致：带 `Derived from` 头的文件必须在某个片段里登记，登记过的文件必须恰好带一行头。
//      这一条与 scripts/check-upstream.mjs 的判据一致，两处不重复实现第三套口径。
//   3. 表里按本仓路径排序（序数），同一份输入渲染出同一段文本：包里两个平台比哈希才有意义。
//   4. 渲染进表里的每个字段都先校验：来源的 repo 形如 owner/name、ref 非空不含空白，path 与 from 是安全的相对路径；
//      同一个 (path, from) 只登记一次，同一个 path 不能分属两个片段。否则表里会出现 undefined 或重复行，而打包
//      并不依赖 check-upstream，没人拦。
//   文件头与 NO_COMMENT 两个常量是 check-upstream.mjs 里那两个的副本（它没有导出）；自测会对照那份源码，
//   两边一旦不同就红，漂移有机制拦。
//
// 它只被 `--emit provenance` 调用；发行包里的那份 NOTICES 由打包脚本渲染后放进 zip。

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { caseResult, isMain } from "../lib/check-kit.mjs";
import { readRepoText } from "../lib/docs-kit.mjs";

/** 上游片段里登记的文件头（与 check-upstream 同一行形态）。 */
const HEADER =
    /^\s*(?:#|\/\/|\/\*+|\*|<!--|;|--|rem\b)?\s*Derived from synchain-oss\/scvb; see upstream\/([\w.-]+)\.json\s*(?:\*\/|-->)?\s*$/;
/** 文件头出现的位置：前 8 行（与 check-upstream 同一口径）。 */
const HEADER_LINES = 8;
/** 写不了注释的格式：登记照常，但不要求文件头（与 check-upstream 的 NO_COMMENT 同一份口径）。 */
const NO_COMMENT =
    /\.(?:json|woff2?|png|jpe?g|gif|ico|svg|lock)$|(?<!CMakeLists)\.txt$|(?:^|\/)\.[\w.-]*version$|(?:^|\/)\.[\w.-]*sha256$/;

/** 路径要是仓内（参考仓内）的安全相对路径：不绝对、不含反斜杠、没有 . 与 .. 段、不带盘符（与 check-upstream 同一口径）。 */
const safeRel = (p) =>
    typeof p === "string" &&
    p !== "" &&
    !p.includes("\\") &&
    !p.startsWith("/") &&
    !/(?:^|\/)\.\.?(?:\/|$)/.test(p) &&
    !/^[A-Za-z]:/.test(p);

/** 改写方式的显示名：登记里的 mode 是机读值，表里写成人话。 */
const MODE_LABEL = { adapted: "adapted", forked: "forked" };

/**
 * 读出 upstream/ 下的全部片段。
 * fragments: { <area>: { sources, files } }；解析失败的那个记成 { error }，不让一个坏文件盖住其余。
 */
export function loadFragments(root) {
    const dir = path.join(root, "upstream");
    const out = {};
    if (!fs.existsSync(dir)) return out;
    for (const file of fs
        .readdirSync(dir)
        .filter((f) => f.endsWith(".json"))
        .sort()) {
        const area = file.slice(0, -5);
        try {
            out[area] = JSON.parse(fs.readFileSync(path.join(dir, file), "utf8"));
        } catch (e) {
            out[area] = { error: `不是合法的 JSON：${e.message}` };
        }
    }
    return out;
}

/**
 * 把全部片段摊平成一张表：每行 { path, repo, ref, sha, from, fromBlob, mode, area }。
 * 判据：片段结构不对、文件头缺失或指向别的片段，都在这里报错。
 * read(rel) 返回文件文本或 null；exists(rel) 返回文件在不在。
 */
export function provenanceRows({ fragments, read, exists }) {
    const errors = [];
    const rows = [];
    const pairs = new Set();
    const registered = new Map();
    for (const [area, frag] of Object.entries(fragments)) {
        if (!frag || frag.error) {
            errors.push(`upstream/${area}.json：${frag?.error ?? "不是对象"}`);
            continue;
        }
        const sources = frag.sources && typeof frag.sources === "object" ? frag.sources : null;
        if (!sources || !Object.keys(sources).length) {
            errors.push(`upstream/${area}.json：sources 必须是非空对象`);
            continue;
        }
        if (!Array.isArray(frag.files) || !frag.files.length) {
            errors.push(`upstream/${area}.json：files 必须是非空数组`);
            continue;
        }
        // 来源的 repo 与 ref 会原样拼进表里（`repo@sha12 (ref)`）：形态不对就是 undefined 或乱码。
        let sourcesOk = true;
        for (const [name, src] of Object.entries(sources)) {
            if (typeof src?.repo !== "string" || !/^[\w.-]+\/[\w.-]+$/.test(src.repo)) {
                errors.push(`upstream/${area}.json：来源 ${name} 的 repo 须形如 owner/name`);
                sourcesOk = false;
            }
            if (typeof src?.ref !== "string" || !/^\S+$/.test(src.ref)) {
                errors.push(`upstream/${area}.json：来源 ${name} 的 ref 不能为空或含空白`);
                sourcesOk = false;
            }
        }
        if (!sourcesOk) continue;
        for (const f of frag.files) {
            const where = `upstream/${area}.json 的 ${f?.path ?? "(没有 path)"}`;
            if (typeof f?.path !== "string" || typeof f?.from !== "string") {
                errors.push(`${where}：缺 path 或 from`);
                continue;
            }
            if (!safeRel(f.path) || !safeRel(f.from)) {
                errors.push(`${where}：path 与 from 必须是相对路径，用正斜杠，没有 .. 段`);
                continue;
            }
            // 同一个 (path, from) 只登记一次；同一个 path 可以合并自几个上游文件，但必须都在同一个片段里。
            const pair = `${f.path}\0${f.from}`;
            if (pairs.has(pair)) {
                errors.push(`${where}：来自 ${f.from} 的这一项重复登记`);
                continue;
            }
            pairs.add(pair);
            if (registered.has(f.path) && registered.get(f.path) !== area) {
                errors.push(`${where}：已在 upstream/${registered.get(f.path)}.json 登记，同一个文件不能分属两个片段`);
                continue;
            }
            registered.set(f.path, area);
            const src = sources[f.source];
            if (!src) {
                errors.push(`${where}：source「${f.source}」不在 sources 里`);
                continue;
            }
            if (!/^[0-9a-f]{40}$/.test(src.sha ?? "")) {
                errors.push(`${where}：来源 ${f.source} 的 sha 不是 40 位小写十六进制`);
                continue;
            }
            if (!/^[0-9a-f]{40}$/.test(f.fromBlob ?? "")) {
                errors.push(`${where}：fromBlob 不是 40 位小写十六进制`);
                continue;
            }
            if (!Object.hasOwn(MODE_LABEL, f.mode)) {
                errors.push(`${where}：mode 只能是 adapted 或 forked`);
                continue;
            }
            // 登记的文件必须在仓里存在：表里写一个不存在的文件，等于给用户一条断链。
            if (!exists(f.path)) {
                errors.push(`${where}：登记的文件不存在`);
                continue;
            }
            // 文件头必须恰好一行，且指向登记它的那个片段。
            // 写不了注释的格式（.json、图片、钉版文件）不要求头：与 check-upstream 同一份口径。
            const text = read(f.path);
            if (text === null) {
                errors.push(`${where}：读不到登记的文件`);
                continue;
            }
            if (NO_COMMENT.test(f.path)) {
                rows.push({
                    path: f.path,
                    repo: src.repo,
                    ref: src.ref,
                    sha: src.sha,
                    from: f.from,
                    fromBlob: f.fromBlob,
                    mode: f.mode,
                    area,
                });
                continue;
            }
            const headers = text
                .split(/\r?\n/)
                .map((line, n) => ({ n, line }))
                .filter((h) => /Derived from/.test(h.line));
            if (headers.length !== 1) {
                errors.push(
                    `${where}：文件头必须恰好一行 Derived from synchain-oss/scvb; see upstream/${area}.json` +
                        `（现有 ${headers.length} 行含 Derived from）`,
                );
                continue;
            }
            const m = HEADER.exec(headers[0].line);
            if (!m || headers[0].n >= HEADER_LINES) {
                errors.push(`${where}：Derived from 行要在前 ${HEADER_LINES} 行内`);
                continue;
            }
            if (m[1] !== area) {
                errors.push(`${where}：文件头指向 upstream/${m[1]}.json，登记它的却是 upstream/${area}.json`);
                continue;
            }
            rows.push({
                path: f.path,
                repo: src.repo,
                ref: src.ref,
                sha: src.sha,
                from: f.from,
                fromBlob: f.fromBlob,
                mode: f.mode,
                area,
            });
        }
    }
    return { rows, errors };
}

/**
 * 渲染逐文件来源表（Markdown）。
 * rows 来自 provenanceRows；表按本仓路径排序，序数比较，同一份输入渲染出同一段文本。
 */
export function renderTable(rows) {
    const sorted = [...rows].sort((a, b) => (a.path < b.path ? -1 : a.path > b.path ? 1 : 0));
    const lines = ["| 本仓文件 | 上游 | 上游路径 | 改写方式 |", "| --- | --- | --- | --- |"];
    for (const r of sorted) {
        const upstream = `${r.repo}@${r.sha.slice(0, 12)} (${r.ref})`;
        lines.push(`| \`${r.path}\` | ${upstream} | \`${r.from}\` | ${MODE_LABEL[r.mode]} |`);
    }
    return lines.join("\n");
}

/** 从仓根渲染：读 upstream/*.json，摊平成表。判据不过就抛错（不渲染半张表）。 */
export function renderFromRoot(root) {
    const fragments = loadFragments(root);
    const read = (rel) => readRepoText(root, rel);
    const exists = (rel) => fs.existsSync(path.join(root, rel));
    const { rows, errors } = provenanceRows({ fragments, read, exists });
    if (errors.length) throw new Error(`来源表渲染不了：\n  ${errors.join("\n  ")}`);
    if (!rows.length) throw new Error("upstream/ 下没有任何登记：来源表是空的");
    return renderTable(rows);
}

export default {
    name: "provenance",
    sources: ["upstream/README.md"],
    surfaces: [],
    render: (ctx) => renderFromRoot(ctx.root),
    emit: (ctx) => renderFromRoot(ctx.root) + "\n",
    validate(ctx) {
        const fragments = loadFragments(ctx.root);
        const read = (rel) => readRepoText(ctx.root, rel);
        const exists = (rel) => fs.existsSync(path.join(ctx.root, rel));
        const { errors } = provenanceRows({ fragments, read, exists });
        return errors;
    },
};

/** 自测：坏样例与正例都要有。 */
export function selfTest() {
    const SHA = "a".repeat(40);
    const BLOB = "b".repeat(40);
    const frag = (area = "release", extra = {}) => ({
        sources: { "scvb-dev": { repo: "synchain-oss/scvb", ref: "dev", sha: SHA } },
        files: [
            {
                path: "scripts/a.ps1",
                source: "scvb-dev",
                from: "scripts/a.ps1",
                fromBlob: BLOB,
                mode: "adapted",
                ...extra,
            },
        ],
    });
    const header = (area = "release") =>
        // 标识串拆开写：REUSE 逐行扫源码，整行字面量会被它当成本文件的第二个许可表达式。
        `# SPDX${"-License-Identifier"}: GPL-3.0-or-later\n# Derived from synchain-oss/scvb; see upstream/${area}.json\nint a;\n`;
    /** 跑一遍判据：fragments 与 files。 */
    const run = (fragments, files = { "scripts/a.ps1": header() }) => {
        const read = (rel) => (Object.hasOwn(files, rel) ? files[rel] : null);
        const exists = (rel) => Object.hasOwn(files, rel);
        return provenanceRows({ fragments, read, exists });
    };
    const cases = [
        ["登记齐全、文件头对得上", false, run({ release: frag() })],
        ["片段不是合法 JSON", true, run({ release: { error: "不是合法的 JSON" } })],
        [
            "来源的 sha 不是 40 位",
            true,
            run({
                release: {
                    sources: { "scvb-dev": { repo: "synchain-oss/scvb", ref: "dev", sha: "a".repeat(39) } },
                    files: frag().files,
                },
            }),
        ],
        ["fromBlob 不是 40 位", true, run({ release: frag("release", { fromBlob: "b".repeat(8) }) })],
        ["mode 不在允许的取值里", true, run({ release: frag("release", { mode: "verbatim" }) })],
        ["登记的文件不存在", true, run({ release: frag() }, {})],
        ["文件头缺失", true, run({ release: frag() }, { "scripts/a.ps1": "int a;\n" })],
        [
            "文件头出现两行",
            true,
            run(
                { release: frag() },
                { "scripts/a.ps1": `${header()}// Derived from synchain-oss/scvb; see upstream/release.json\n` },
            ),
        ],
        ["文件头指向别的片段", true, run({ release: frag() }, { "scripts/a.ps1": header("build") })],
        ["文件头在第 8 行之后", true, run({ release: frag() }, { "scripts/a.ps1": `${"//\n".repeat(8)}${header()}` })],
        ["source 指向不存在的来源", true, run({ release: { sources: {}, files: frag().files } })],
        [
            "来源缺 repo（表里会出现 undefined）",
            true,
            run({ release: { sources: { "scvb-dev": { ref: "dev", sha: SHA } }, files: frag().files } }),
        ],
        [
            "来源的 repo 不是 owner/name",
            true,
            run({ release: { sources: { "scvb-dev": { repo: "scvb", ref: "dev", sha: SHA } }, files: frag().files } }),
        ],
        [
            "来源缺 ref",
            true,
            run({ release: { sources: { "scvb-dev": { repo: "synchain-oss/scvb", sha: SHA } }, files: frag().files } }),
        ],
        [
            "来源的 ref 含空白",
            true,
            run({
                release: {
                    sources: { "scvb-dev": { repo: "synchain-oss/scvb", ref: "d ev", sha: SHA } },
                    files: frag().files,
                },
            }),
        ],
        ["登记缺 from", true, run({ release: frag("release", { from: undefined }) })],
        ["from 是绝对路径", true, run({ release: frag("release", { from: "/etc/passwd" }) })],
        ["path 含 .. 段", true, run({ release: frag("release", { path: "scripts/../a.ps1" }) })],
        [
            "同一个 (path, from) 重复登记",
            true,
            run({ release: { sources: frag().sources, files: [...frag().files, ...frag().files] } }),
        ],
        [
            "同一个文件分属两个片段",
            true,
            // 写不了注释的格式不查文件头，所以“分属两个片段”只能靠登记表自己的判据拦。
            run(
                {
                    release: frag("release", { path: "config/a.json", from: "config/a.json" }),
                    build: frag("build", { path: "config/a.json", from: "config/b.json" }),
                },
                { "config/a.json": "{}\n" },
            ),
        ],
        ["片段里一个文件都没登记", true, run({ release: { sources: frag().sources, files: [] } })],
    ].map(([label, bad, result]) => ({
        label,
        bad,
        passed: bad ? result.errors.length > 0 : result.errors.length === 0,
    }));
    // 合并自两个上游文件的同一个 path（同一片段里两项、from 不同）：不算重复。
    cases.push({
        label: "同一个 path 合并自两个上游文件（同一片段）：不算重复登记",
        bad: false,
        passed:
            run({
                release: {
                    sources: frag().sources,
                    files: [...frag().files, { ...frag().files[0], from: "scripts/b.ps1", fromBlob: "c".repeat(40) }],
                },
            }).errors.length === 0,
    });
    // 本文件的两个常量是 check-upstream.mjs 里那两个的副本：对照源码，不同就红。
    const upstreamSrc = fs.readFileSync(
        path.join(path.dirname(fileURLToPath(import.meta.url)), "..", "check-upstream.mjs"),
        "utf8",
    );
    cases.push({
        label: "文件头正则与 check-upstream.mjs 里的逐字相同（副本没有漂移）",
        bad: true,
        passed: upstreamSrc.includes(HEADER.toString()),
    });
    cases.push({
        label: "NO_COMMENT 正则与 check-upstream.mjs 里的逐字相同（副本没有漂移）",
        bad: true,
        passed: upstreamSrc.includes(NO_COMMENT.toString()),
    });
    cases.push({
        label: "文件头位置（前 8 行）与 check-upstream.mjs 里的相同",
        bad: true,
        passed: upstreamSrc.includes(`const HEADER_LINES = ${HEADER_LINES};`),
    });
    // 渲染：表头、行数与排序都要对。
    const rows = run({ release: frag() }).rows;
    const table = renderTable(rows);
    cases.push({
        label: "渲染：表头与一行一个文件",
        bad: false,
        passed:
            table.startsWith("| 本仓文件 | 上游 | 上游路径 | 改写方式 |") &&
            table.split("\n").length === 3 &&
            table.includes("`scripts/a.ps1`"),
    });
    // 排序：两个文件的登记顺序反过来，渲染出来必须逐字相同。
    const two = {
        sources: { s: { repo: "synchain-oss/scvb", ref: "dev", sha: SHA } },
        files: [
            { path: "scripts/z.ps1", source: "s", from: "scripts/z.ps1", fromBlob: BLOB, mode: "adapted" },
            { path: "scripts/a.ps1", source: "s", from: "scripts/a.ps1", fromBlob: BLOB, mode: "adapted" },
        ],
    };
    const files2 = { "scripts/a.ps1": header(), "scripts/z.ps1": header() };
    const read2 = (rel) => files2[rel];
    const exists2 = (rel) => Object.hasOwn(files2, rel);
    const rowsA = provenanceRows({ fragments: { release: two }, read: read2, exists: exists2 }).rows;
    const rowsB = provenanceRows({
        fragments: { release: { ...two, files: [...two.files].reverse() } },
        read: read2,
        exists: exists2,
    }).rows;
    cases.push({
        label: "渲染：登记顺序不同，渲染结果逐字相同",
        bad: false,
        passed:
            renderTable(rowsA) === renderTable(rowsB) &&
            renderTable(rowsA).indexOf("a.ps1") < renderTable(rowsA).indexOf("z.ps1"),
    });
    return caseResult(cases);
}

// 直接运行：node scripts/gen-docs.d/provenance.mjs --self-test
// 引擎只认默认导出；自测不挂在引擎上，所以留一个命令行入口，坏样例与正例都在 selfTest 里。
if (isMain(import.meta.url)) {
    if (process.argv.includes("--self-test") || process.argv.includes("-SelfTest")) {
        const r = selfTest();
        for (const l of r.lines) console.error(l);
        console.log(`provenance --self-test：${r.ok ? "通过" : "失败"}（坏样例 ${r.bad}、正例 ${r.good}）`);
        process.exitCode = r.ok ? 0 : 1;
    } else {
        console.error(
            "用法：node scripts/gen-docs.d/provenance.mjs --self-test（渲染走 node scripts/gen-docs.mjs --emit provenance）",
        );
        process.exitCode = 1;
    }
}
