// SPDX-License-Identifier: GPL-3.0-or-later
//
// 参考仓来源登记的校验。复制自参考仓的文件登记在 upstream/<area>.json（规程见 upstream/README.md），本脚本把全部片段合起来核对：
//   - 片段结构：sources 里每个来源有 repo、ref 和 40 位 sha；files 里每项有 path、source、from、40 位 fromBlob 和 mode；
//   - 同一个 (path, from) 全仓只登记一次；一个文件合并自参考仓的几个文件时，同一个 path 可以有几项，但必须都在同一个片段里；
//     登记的文件必须存在；
//   - 文件头恰好一行 `Derived from synchain-oss/scvb; see upstream/<area>.json`，<area> 就是登记它的那个片段；
//   - 反过来，文件头写了这一行却没登记的文件也判红。
//   node scripts/check-upstream.mjs [--self-test]
//   node scripts/check-upstream.mjs --drift [--ref-dir <参考仓的本地克隆>]
// --drift 要联网（或读本地克隆），逐个文件把登记的 fromBlob 与来源分支现在的 blob 比对：相同就是没有漂移。
// 它只给维护者手动跑，不进 CI。

import fs from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { ROOT, assertNodeVersion, caseResult, git, isMain, j, readText, trackedFiles } from "./lib/check-kit.mjs";

const AREA = /^[a-z0-9]+(?:-[a-z0-9]+)*$/;
const SHA40 = /^[0-9a-f]{40}$/;
const MODES = ["adapted", "forked"];
const SOURCE_KEYS = ["repo", "ref", "sha"];
const FILE_KEYS = ["path", "source", "from", "fromBlob", "mode"];
/** 文件头出现的位置：前 8 行。 */
const HEADER_LINES = 8;
/** 带注释前缀的头行；<area> 在第 1 个捕获组。 */
const HEADER =
    /^\s*(?:#|\/\/|\/\*+|\*|<!--|;|--|rem\b)?\s*Derived from synchain-oss\/scvb; see upstream\/([\w.-]+)\.json\s*(?:\*\/|-->)?\s*$/;
/** 写不了注释的格式：登记照常，但不要求文件头。 */
const NO_COMMENT =
    /\.(?:json|woff2?|png|jpe?g|gif|ico|svg|txt|lock)$|(?:^|\/)\.[\w.-]*version$|(?:^|\/)\.[\w.-]*sha256$/;

const isObject = (v) => v !== null && typeof v === "object" && !Array.isArray(v);
const safeRel = (p) =>
    typeof p === "string" &&
    p !== "" &&
    !p.includes("\\") &&
    !p.startsWith("/") &&
    !/(?:^|\/)\.\.?(?:\/|$)/.test(p) &&
    !/^[A-Za-z]:/.test(p);

function unknownKeys(obj, allowed) {
    return Object.keys(obj).filter((k) => !allowed.includes(k));
}

/**
 * 核对全部片段。
 *   fragments: { <area>: 已解析的对象，或 { error: "读不出来的原因" } }
 *   read(rel): 返回文件文本；文件不存在返回 null；二进制返回 undefined
 *   files:     仓内全部被跟踪的文件（反向核对用）
 * 返回错误列表。
 */
export function checkUpstream({ fragments, read, files }) {
    const errors = [];
    const registered = new Map(); // path -> area
    const pairs = new Set(); // path + from
    const areas = Object.keys(fragments);
    if (!areas.length) errors.push("upstream/ 下没有任何 <area>.json");

    for (const area of areas) {
        const where = `upstream/${area}.json`;
        const frag = fragments[area];
        if (!AREA.test(area)) errors.push(`${where}：片段名只能是小写字母、数字和连字符`);
        if (!isObject(frag) || frag.error) {
            errors.push(`${where}：${isObject(frag) && frag.error ? frag.error : "顶层必须是对象"}`);
            continue;
        }
        for (const k of unknownKeys(frag, ["sources", "files"])) errors.push(`${where}：未知的顶层键 ${k}`);
        const sources = isObject(frag.sources) ? frag.sources : null;
        if (!sources || !Object.keys(sources).length) errors.push(`${where}：sources 必须是非空对象`);
        for (const [name, src] of Object.entries(sources ?? {})) {
            if (!isObject(src)) {
                errors.push(`${where}：来源 ${name} 必须是对象`);
                continue;
            }
            for (const k of unknownKeys(src, SOURCE_KEYS)) errors.push(`${where}：来源 ${name} 有未知的键 ${k}`);
            if (typeof src.repo !== "string" || !/^[\w.-]+\/[\w.-]+$/.test(src.repo))
                errors.push(`${where}：来源 ${name} 的 repo 须形如 owner/name`);
            if (typeof src.ref !== "string" || !/^\S+$/.test(src.ref))
                errors.push(`${where}：来源 ${name} 的 ref 不能为空或含空白`);
            if (typeof src.sha !== "string" || !SHA40.test(src.sha))
                errors.push(`${where}：来源 ${name} 的 sha 必须是 40 位小写十六进制`);
        }
        if (!Array.isArray(frag.files) || !frag.files.length) {
            errors.push(`${where}：files 必须是非空数组`);
            continue;
        }
        for (const [i, f] of frag.files.entries()) {
            const id = `${where} 第 ${i + 1} 项`;
            if (!isObject(f)) {
                errors.push(`${id}：必须是对象`);
                continue;
            }
            for (const k of unknownKeys(f, FILE_KEYS)) errors.push(`${id}：未知的键 ${k}`);
            for (const k of FILE_KEYS) if (typeof f[k] !== "string" || f[k] === "") errors.push(`${id}：缺 ${k}`);
            const label = typeof f.path === "string" ? `${where} 的 ${f.path}` : id;
            if (typeof f.path === "string" && !safeRel(f.path))
                errors.push(`${label}：path 必须是仓内相对路径，用正斜杠`);
            if (typeof f.from === "string" && !safeRel(f.from))
                errors.push(`${label}：from 必须是参考仓内的相对路径，用正斜杠`);
            if (typeof f.source === "string" && sources && !Object.hasOwn(sources, f.source))
                errors.push(`${label}：source ${f.source} 不在 sources 里`);
            if (typeof f.fromBlob === "string" && !SHA40.test(f.fromBlob))
                errors.push(`${label}：fromBlob 必须是 40 位小写十六进制`);
            if (typeof f.mode === "string" && !MODES.includes(f.mode))
                errors.push(`${label}：mode 只能是 ${MODES.join(" 或 ")}`);
            if (typeof f.path !== "string") continue;
            const pair = `${f.path}\0${f.from}`;
            if (pairs.has(pair)) {
                errors.push(`${label}：来自 ${f.from} 的这一项重复登记`);
                continue;
            }
            pairs.add(pair);
            if (registered.has(f.path)) {
                // 合并自几个参考文件的同一个 path：头只检查一次，但几项必须在同一个片段里
                if (registered.get(f.path) !== area)
                    errors.push(
                        `${label}：已在 upstream/${registered.get(f.path)}.json 登记，同一个文件不能分属两个片段`,
                    );
                continue;
            }
            registered.set(f.path, area);

            const text = read(f.path);
            if (text === null) {
                errors.push(`${label}：登记的文件不存在`);
                continue;
            }
            if (text === undefined || NO_COMMENT.test(f.path)) continue;
            const headers = text.split(/\r?\n/).flatMap((line, n) => (/Derived from/.test(line) ? [{ n, line }] : []));
            if (headers.length !== 1) {
                errors.push(
                    `${label}：文件头必须恰好一行 Derived from synchain-oss/scvb; see upstream/${area}.json（现有 ${headers.length} 行含 Derived from）`,
                );
                continue;
            }
            const m = HEADER.exec(headers[0].line);
            if (!m || headers[0].n >= HEADER_LINES) {
                errors.push(
                    `${label}：Derived from 行要在前 ${HEADER_LINES} 行内，形如「# Derived from synchain-oss/scvb; see upstream/${area}.json」`,
                );
            } else if (m[1] !== area) {
                errors.push(`${label}：文件头指向 upstream/${m[1]}.json，登记它的却是 upstream/${area}.json`);
            }
        }
    }

    // 反向：文件头写了却没登记
    for (const rel of files) {
        if (registered.has(rel) || rel.startsWith("upstream/")) continue;
        const text = read(rel);
        if (!text) continue;
        const head = text.split(/\r?\n/, HEADER_LINES);
        if (head.some((line) => HEADER.test(line)))
            errors.push(`${rel}：文件头写了 Derived from，却没有在任何 upstream/*.json 登记`);
    }
    return errors;
}

/** 读 upstream/*.json；某个片段解析失败就记成 { error }，不让一个坏文件盖住其余的错误。 */
function loadFragments(root) {
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

export function checkTree(root = ROOT) {
    const read = (rel) => {
        const abs = path.join(root, rel);
        if (!fs.existsSync(abs)) return null;
        return readText(abs) ?? undefined;
    };
    return checkUpstream({ fragments: loadFragments(root), read, files: trackedFiles(root) });
}

/**
 * 漂移：来源分支现在的 blob 与登记的 fromBlob 是否相同。blobAt(source, from) 返回 40 位 blob 或 null（上游已不存在）。
 * 返回 { drifted: [...], missing: [...], same: n }。
 */
export function compareDrift(fragments, blobAt) {
    const report = { drifted: [], missing: [], same: 0 };
    for (const [area, frag] of Object.entries(fragments)) {
        for (const f of frag.files ?? []) {
            const blob = blobAt(frag.sources[f.source], f.from);
            if (blob === null) report.missing.push(`${f.path}（${area}）：来源里已没有 ${f.from}`);
            else if (blob === f.fromBlob) report.same++;
            else
                report.drifted.push(`${f.path}（${area}）：登记 ${f.fromBlob.slice(0, 12)}，现在 ${blob.slice(0, 12)}`);
        }
    }
    return report;
}

/** 本地克隆里取 blob：先 fetch 来源分支，再按 origin/<ref>:<from> 解析。 */
function blobFromClone(dir) {
    const fetched = new Set();
    return (src, from) => {
        if (!fetched.has(src.ref)) {
            git(["-C", dir, "fetch", "--quiet", "origin", src.ref], { allowFail: false });
            fetched.add(src.ref);
        }
        const out = git(["-C", dir, "rev-parse", "--verify", "--quiet", `origin/${src.ref}:${from}`], {
            allowFail: true,
        });
        return out ? out.trim() : null;
    };
}

/** 经 GitHub 接口取 blob；认证沿用 gh 当前的登录或 GH_TOKEN，本脚本不切换账号。 */
function blobFromGitHub(src, from) {
    const url = `repos/${src.repo}/contents/${from.split("/").map(encodeURIComponent).join("/")}?ref=${encodeURIComponent(src.ref)}`;
    const r = spawnSync("gh", ["api", url, "--jq", ".sha"], { encoding: "utf8" });
    if (r.status === 0) return r.stdout.trim();
    if (/HTTP 404/.test(`${r.stderr}`)) return null;
    throw new Error(`gh api 失败：${String(r.stderr).trim() || r.error?.message}`);
}

export function selfTest() {
    const sha = "a".repeat(40);
    const blob = "b".repeat(40);
    const frag = (extra = {}) => ({
        sources: { s: { repo: "synchain-oss/scvb", ref: "dev", sha } },
        files: [{ path: "src/a.cpp", source: "s", from: "src/a.cpp", fromBlob: blob, mode: "adapted", ...extra }],
    });
    const hdr = (area = "x") =>
        `// ${j("SPDX-License-", "Identifier: GPL-3.0-or-later")}\n// Derived from synchain-oss/scvb; see upstream/${area}.json\nint a;\n`;
    const base = { fragments: { x: frag() }, texts: { "src/a.cpp": hdr() } };
    const run = (o = {}) => {
        const texts = { ...base.texts, ...(o.texts ?? {}) };
        const read = (rel) => (Object.hasOwn(texts, rel) ? texts[rel] : null);
        const fragments = o.fragments ?? base.fragments;
        return checkUpstream({ fragments, read, files: Object.keys(texts) });
    };
    const mutate = (f) => {
        const copy = structuredClone(frag());
        f(copy);
        return { x: copy };
    };
    const cases = [
        ["登记完整、文件头对得上", false, run()],
        [
            "注释前缀换成 # 也认",
            false,
            run({ texts: { "src/a.cpp": "# Derived from synchain-oss/scvb; see upstream/x.json\n" } }),
        ],
        [
            "不能写注释的格式不要求文件头",
            false,
            run({
                fragments: { x: frag({ path: "a.json", from: "a.json" }) },
                texts: { "a.json": "{}\n", "src/a.cpp": "int a;\n" },
            }),
        ],
        ["没有任何片段", true, run({ fragments: {} })],
        ["片段不是合法 JSON", true, run({ fragments: { x: { error: "不是合法的 JSON" } } })],
        ["来源的 sha 不是 40 位", true, run({ fragments: mutate((f) => (f.sources.s.sha = "a".repeat(39))) })],
        ["来源的 sha 含大写", true, run({ fragments: mutate((f) => (f.sources.s.sha = "A".repeat(40))) })],
        ["来源缺 ref", true, run({ fragments: mutate((f) => delete f.sources.s.ref) })],
        ["fromBlob 不是 40 位", true, run({ fragments: mutate((f) => (f.files[0].fromBlob = "b".repeat(8))) })],
        ["文件缺 fromBlob", true, run({ fragments: mutate((f) => delete f.files[0].fromBlob) })],
        ["mode 不在允许的取值里", true, run({ fragments: mutate((f) => (f.files[0].mode = "verbatim")) })],
        ["source 指向不存在的来源", true, run({ fragments: mutate((f) => (f.files[0].source = "nope")) })],
        ["顶层有未知的键", true, run({ fragments: mutate((f) => (f.extra = 1)) })],
        ["path 含反斜杠", true, run({ fragments: mutate((f) => (f.files[0].path = "src\\a.cpp")) })],
        ["登记的文件不存在", true, run({ fragments: mutate((f) => (f.files[0].path = "src/missing.cpp")) })],
        [
            "同一个文件在两个片段里各登记一次",
            true,
            run({ fragments: { x: frag(), y: frag() }, texts: { "src/a.cpp": hdr() } }),
        ],
        ["同一个 (path, from) 重复登记", true, run({ fragments: mutate((f) => f.files.push({ ...f.files[0] })) })],
        [
            "一个文件合并自两个参考文件（同片段）",
            false,
            run({ fragments: mutate((f) => f.files.push({ ...f.files[0], from: "src/b.cpp" })) }),
        ],
        ["文件头缺失", true, run({ texts: { "src/a.cpp": "int a;\n" } })],
        [
            "文件头出现两行",
            true,
            run({ texts: { "src/a.cpp": `${hdr()}// Derived from synchain-oss/scvb; see upstream/x.json\n` } }),
        ],
        ["文件头指向别的片段", true, run({ texts: { "src/a.cpp": hdr("y") } })],
        [
            "文件头夹带别的字",
            true,
            run({ texts: { "src/a.cpp": "// Derived from synchain-oss/scvb; see upstream/x.json (SL-1)\n" } }),
        ],
        ["文件头在第 8 行之后", true, run({ texts: { "src/a.cpp": `${"//\n".repeat(8)}${hdr()}` } })],
        ["有文件头却没登记", true, run({ texts: { "src/b.cpp": hdr() } })],
    ].map(([label, bad, errors]) => ({ label, bad, passed: bad ? errors.length > 0 : errors.length === 0 }));

    const same = compareDrift({ x: frag() }, () => blob);
    const moved = compareDrift({ x: frag() }, () => "c".repeat(40));
    const gone = compareDrift({ x: frag() }, () => null);
    cases.push(
        {
            label: "漂移：blob 相同不报",
            bad: false,
            passed: same.same === 1 && !same.drifted.length && !same.missing.length,
        },
        { label: "漂移：blob 不同要报", bad: true, passed: moved.drifted.length === 1 },
        { label: "漂移：上游已删除要报", bad: true, passed: gone.missing.length === 1 },
    );
    return caseResult(cases);
}

function main(argv) {
    assertNodeVersion();
    if (argv.includes("--self-test")) {
        const r = selfTest();
        for (const l of r.lines) console.error(l);
        console.log(`check-upstream --self-test：${r.ok ? "通过" : "失败"}（坏样例 ${r.bad}、正例 ${r.good}）`);
        return r.ok ? 0 : 1;
    }
    if (argv.includes("--drift")) {
        const i = argv.indexOf("--ref-dir");
        const blobAt = i >= 0 ? blobFromClone(path.resolve(argv[i + 1] ?? "")) : blobFromGitHub;
        const report = compareDrift(loadFragments(ROOT), blobAt);
        for (const l of report.drifted) console.log(`漂移：${l}`);
        for (const l of report.missing) console.log(`上游已无：${l}`);
        console.log(
            `check-upstream --drift：${report.same} 个文件与来源分支一致，${report.drifted.length} 个漂移，${report.missing.length} 个上游已无`,
        );
        return report.drifted.length || report.missing.length ? 1 : 0;
    }
    const errors = checkTree();
    if (!errors.length) {
        console.log("check-upstream 通过");
        return 0;
    }
    console.error(`check-upstream 失败：${errors.length} 处`);
    for (const e of errors) console.error(`  ${e}`);
    return 1;
}

if (isMain(import.meta.url)) process.exitCode = main(process.argv.slice(2));
