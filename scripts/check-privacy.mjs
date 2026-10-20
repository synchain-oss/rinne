// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/ci.json
//
// 公开仓隐私门禁：privacy-needles.mjs 的通用规则加上个人针，扫三种对象。
//   node scripts/check-privacy.mjs [--root <dir>]      git 跟踪的工作树（内容与路径；UTF-16 与二进制也扫）
//   node scripts/check-privacy.mjs --commits <rev-list 参数…> [--no-messages]
//        区间内每个提交的消息、新增行与新路径。推上去的提交经 refs/pull 永久公开，
//        工作树干净不代表历史干净。--no-messages 不扫提交消息（消息归 check-commits 按档位判）。
//   node scripts/check-privacy.mjs --text <文件|->     一段公开文本（PR 标题与正文、评论草稿），只收 UTF-8
//   --needles local|ci|ci-optional  个人针来源，见 privacy-needles.mjs；默认 local
//   --self-test（规则夹具 + 端到端）  --mutation-check  --list-rules
// 只打印位置与打码片段；个人针只打印序号，路径里的命中也打码。退出码：0 干净；1 有命中、配置缺失或自测失败。

import fs from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import * as kit from "./lib/check-kit.mjs";
import * as needles from "./lib/privacy-needles.mjs";
import { makeTempDir, removeTempDir } from "./lib/safe-rm.mjs";

const { ROOT, caseResult, git, tempRepo, trackedFiles } = kit;
const { mask, redact, scan, scanBlob } = needles;
const SELF = fileURLToPath(import.meta.url);
// 本机的 diff.external 与 textconv 驱动会改写补丁内容，扫描时一律关掉
const DIFF = ["--no-renames", "--no-ext-diff", "--no-textconv"];

export const formatFinding = (f) =>
    `  ${f.where}${f.line ? `:${f.line}:${f.col}` : "（二进制）"} [${f.rule.id}] ${redact(f)}`;
const tag = (where, list) => list.map((f) => ({ ...f, where }));

/**
 * 补丁 +++ 行里的路径：含空格时 git 在末尾补一个 TAB；含引号、反斜杠、控制字符时整段写成
 * C 风格的带引号字符串（八进制转义按 UTF-8 字节还原）。返回去掉 b/ 前缀的仓内路径。
 */
export function patchPath(s) {
    let p = s.replace(/\t$/, "");
    if (p.startsWith('"') && p.endsWith('"')) {
        const bytes = [];
        const esc = { a: 7, b: 8, t: 9, n: 10, v: 11, f: 12, r: 13 };
        for (let i = 1; i < p.length - 1; i++) {
            if (p[i] !== "\\") {
                bytes.push(...Buffer.from(p[i]));
            } else if (/[0-7]/.test(p[i + 1])) {
                bytes.push(parseInt(p.slice(i + 1, i + 4), 8));
                i += 3;
            } else {
                bytes.push(esc[p[i + 1]] ?? p.charCodeAt(i + 1));
                i++;
            }
        }
        p = Buffer.from(bytes).toString("utf8");
    }
    return p.replace(/^b\//, "");
}

export function scanTree(root, personal) {
    const out = [];
    for (const rel of trackedFiles(root)) {
        const shown = mask(rel, personal);
        out.push(...tag(`${shown}（路径）`, scan(rel, {}, { personal })));
        let buf;
        try {
            buf = fs.readFileSync(path.join(root, rel));
        } catch {
            continue; // 已删除、子模块等
        }
        out.push(...tag(shown, scanBlob(buf, { path: rel }, { personal })));
    }
    return out;
}

/**
 * 逐提交扫消息（可关）、新路径与新内容。内容取提交后的整份文件来扫（这样 C/C++ 的块注释、
 * 同文件的章节标题都在上下文里），只保留落在新增行上的命中；二进制与 UTF-16 文件没有行级补丁，
 * 整份报告。合并提交看相对第一父提交带进来的全部内容。
 */
export function scanCommits(revArgs, cwd, personal, { messages = true } = {}) {
    const out = [];
    for (const sha of git(["rev-list", ...revArgs], { cwd })
        .split("\n")
        .filter(Boolean)) {
        const s7 = sha.slice(0, 7);
        const [parents, msg] = git(["log", "-1", "--format=%P%x00%B", sha], { cwd }).split("\0");
        if (messages) out.push(...tag(`${s7} 提交消息`, scan(msg, {}, { personal })));
        const diff = (...opts) =>
            parents.trim().includes(" ")
                ? git(["diff", ...DIFF, ...opts, `${sha}^1`, sha], { cwd })
                : git(["show", ...DIFF, "--format=", ...opts, sha], { cwd });
        const added = new Map(); // 路径 → 新增行号
        let file = null;
        let header = false;
        for (const line of diff("--no-color", "-U0", "--src-prefix=a/", "--dst-prefix=b/").split("\n")) {
            if (line.startsWith("diff --git ")) {
                header = true;
                file = null;
            } else if (header && line.startsWith("+++ ")) {
                file = line === "+++ /dev/null" ? null : patchPath(line.slice(4));
                if (file) added.set(file, new Set());
            } else if (line.startsWith("@@")) {
                header = false;
                const [, start, count = "1"] = /\+(\d+)(?:,(\d+))?/.exec(line) || [];
                for (let k = 0; file && k < Number(count); k++) added.get(file).add(Number(start) + k);
            }
        }
        // 路径与是否二进制取 --numstat -z（路径不转义）；补丁里找不到的路径按整份文件报告
        for (const entry of diff("--numstat", "-z", "--diff-filter=AMT").split("\0")) {
            const [plus, , ...rest] = entry.replace(/^\n+/, "").split("\t");
            const rel = rest.join("\t");
            if (!rel) continue;
            const shown = mask(rel, personal);
            out.push(...tag(`${s7} ${shown}（路径）`, scan(rel, {}, { personal })));
            const buf = git(["cat-file", "blob", `${sha}:${rel}`], { cwd, raw: true, allowFail: true });
            if (!buf) continue; // 子模块之类不是 blob 的条目
            const lines = plus === "-" ? null : added.get(rel);
            const hits = scanBlob(buf, { path: rel }, { personal });
            // 二进制视图的命中（line 0）没有行号可对，一律保留
            const kept = lines ? hits.filter((f) => f.line === 0 || lines.has(f.line)) : hits;
            out.push(...tag(`${s7} ${shown}`, kept));
        }
    }
    return out;
}

function report(findings, notes) {
    for (const n of notes) console.log(n);
    if (!findings.length) {
        console.log("check-privacy 通过：零命中");
        return 0;
    }
    console.error(`check-privacy 失败：${findings.length} 处命中`);
    for (const f of findings) console.error(formatFinding(f));
    console.error("规则见 --list-rules。确属误报就改写句子；逐字第三方目录只豁免邮箱与编号，个人针没有豁免。");
    return 1;
}

/** 规则夹具，加上端到端：假针藏进临时仓的各种地方，必须判红且输出里不出现针的明文。 */
export function selfTest() {
    const cases = needles.runCases();
    const N = needles.FAKE_NEEDLE;
    const repo = tempRepo();
    const aux = makeTempDir("rinne-selftest-aux-");
    const file = (name, data) => {
        const p = path.join(aux, name);
        fs.writeFileSync(p, data);
        return p;
    };
    const utf16 = (s) => Buffer.concat([Buffer.from([0xff, 0xfe]), Buffer.from(s, "utf16le")]);
    const base = kit.isolatedEnv();
    for (const k of ["RINNE_PRIVACY_LOCAL", "PRIVACY_NEEDLES", "GITHUB_ACTIONS", "CI"]) delete base[k];
    const run = (args, env = {}, self = SELF) => {
        const r = spawnSync(process.execPath, [self, ...args], {
            cwd: repo.dir,
            env: { ...base, ...env },
            encoding: "utf8",
        });
        return { code: r.status, out: `${r.stdout}${r.stderr}` };
    };
    const check = (label, bad, ok) => cases.push({ label: `端到端：${label}`, bad, passed: !!ok });
    const commit = (msg) => {
        repo.git(["add", "-A"]);
        repo.git(["commit", "-q", "-m", msg]);
    };
    try {
        const nf = file("needles.txt", `# 自测\n${N}\n`);
        const local = { RINNE_PRIVACY_LOCAL: nf };
        const tree = (env, ...extra) => run(["--root", repo.dir, ...extra], env);
        repo.write("docs/clean.md", "干净的文本\n");
        commit("ci(test): 干净");
        check("干净的树通过", false, tree(local).code === 0);
        // 个人针的来源必须显式给出，不看环境变量猜
        check("本机未配置个人针判红", true, tree({}).code === 1);
        check("本机模式不因 CI=true 放行", true, tree({ CI: "true", GITHUB_ACTIONS: "true" }).code === 1);
        check("个人针文件缺失判红", true, tree({ RINNE_PRIVACY_LOCAL: `${nf}.missing` }).code === 1);
        check("同仓 PR 缺 secret 判红", true, tree({}, "--needles", "ci").code === 1);
        const fork = tree({}, "--needles", "ci-optional");
        check("fork PR 缺 secret：注记并只跑通用规则", false, fork.code === 0 && fork.out.includes("PRIVACY_NEEDLES"));
        check("UTF-16 针表判红", true, tree({ RINNE_PRIVACY_LOCAL: file("n16.txt", utf16(`${N}\n`)) }).code === 1);
        check("针表含 NUL 判红", true, tree({ RINNE_PRIVACY_LOCAL: file("nnul.txt", `${N}\0\n`) }).code === 1);
        check("--text 拒绝 UTF-16", true, run(["--text", file("t16.md", utf16("干净\n"))], local).code === 1);
        const nul = run(["--text", file("tnul.md", "干净\0\n")], local);
        check("--text 拒绝 NUL 并报出位置", true, nul.code === 1 && nul.out.includes("字节偏移 6"));
        // 经 junction / 符号链接调用时 CLI 照常运行，不会静默什么都不做
        const link = path.join(aux, "scripts-link");
        fs.symlinkSync(path.dirname(SELF), link, process.platform === "win32" ? "junction" : "dir");
        const card = file("card.md", `${needles.CASES[0][1]}\n`);
        check(
            "经链接调用照样判红",
            true,
            run(["--text", card], local, path.join(link, "check-privacy.mjs")).code === 1,
        );
        // 各种藏法：普通文件、UTF-16 文件、二进制文本块、文件名；删掉之后历史里仍要抓到
        const png = Buffer.concat([Buffer.from([0x89, 0x50, 0x4e, 0x47, 0, 0]), Buffer.from(`tEXtAuthor\0${N}\0`)]);
        // 第 8000 字节之后才有 NUL：git 与这里都当文本；gitattributes 让 git 按文本 diff 的二进制：
        // git 给行号，这里只有二进制视图（line 0），两种情形都不能漏
        const lateNul = Buffer.concat([Buffer.from(`说明 ${N}\n${"x".repeat(8100)}`), Buffer.from([0, 10])]);
        const forced = Buffer.concat([Buffer.from([0, 1, 0]), Buffer.from(` ${N} `), Buffer.from([0])]);
        repo.write(".gitattributes", "*.dat diff\n");
        commit("ci(test): 属性");
        const leaks = [
            ["普通文件", "docs/leak.md", `说明 ${N}\n`],
            ["UTF-16 文件", "docs/log16.txt", utf16(`路径 ${N}\n`)],
            ["二进制文本块", "docs/shot.png", png],
            ["文件名", `docs/notes-${N}.md`, "x\n"],
            ["8000 字节后才有 NUL", "docs/late.txt", lateNul],
            ["按文本 diff 的二进制", "docs/blob.dat", forced],
        ];
        for (const [label, rel, data] of leaks) {
            repo.write(rel, data);
            commit("ci(test): 夹带");
            const r = tree(local);
            check(`${label}：工作树判红且不回显`, true, r.code === 1 && !r.out.includes(N));
            repo.git(["rm", "-q", rel]);
            commit("ci(test): 删掉");
            const h = run(["--root", repo.dir, "--commits", "HEAD~2..HEAD"], local);
            check(`${label}：删掉后历史里判红且不回显`, true, h.code === 1 && !h.out.includes(N));
        }
        check("全部删掉后工作树通过", false, tree(local).code === 0);
        // 行号映射：路径带空格时 git 在 +++ 行末补 TAB，也只看新增行
        const old = `${needles.CASES[0][1]}\n`;
        repo.write("docs/a b.md", old);
        commit("ci(test): 旧内容");
        repo.write("docs/a b.md", `${old}干净的新行\n`);
        commit("ci(test): 只加一行");
        check(
            "带空格的路径只看新增行",
            false,
            run(["--root", repo.dir, "--commits", "HEAD~1..HEAD"], local).code === 0,
        );
        repo.git(["commit", "-q", "--allow-empty", "-m", `ci(test): ${N}`]);
        const range = ["--root", repo.dir, "--commits", "HEAD~1..HEAD"];
        check("提交消息里的针判红", true, run(range, local).code === 1);
        check("--no-messages 不扫消息", false, run([...range, "--no-messages"], local).code === 0);
        const t = (label, input, want) => check(label, false, patchPath(input) === want);
        t("补丁路径：去掉末尾 TAB", "b/a b.md\t", "a b.md");
        t("补丁路径：C 风格引号", '"b/q\\"x.md"', 'q"x.md');
        t("补丁路径：八进制转义", '"b/\\346\\226\\207.md"', "文.md");
    } finally {
        repo.cleanup();
        removeTempDir(aux);
    }
    return caseResult(cases);
}

const OWN = { "--root": 1, "--needles": 1, "--text": 1, "--no-messages": 0, "--commits": 0 };

function main(argv) {
    kit.assertNodeVersion();
    if (argv.includes("--list-rules")) {
        for (const r of needles.RULES) {
            const flags = [r.verbatimOk && "逐字目录豁免", r.commentsOnly && "C/C++ 只看注释", r.binary && "扫二进制"];
            console.log(`  ${r.id}${flags.some(Boolean) ? `（${flags.filter(Boolean).join("、")}）` : ""}`);
        }
        console.log(`  personal（个人针，无豁免，扫二进制）\n逐字第三方目录：${needles.VERBATIM_DIRS.join(" ")}`);
        return 0;
    }
    if (argv.includes("--self-test")) {
        const r = selfTest();
        for (const l of r.lines) console.error(l);
        console.log(`check-privacy --self-test：${r.ok ? "通过" : "失败"}（坏样例 ${r.bad}、正例 ${r.good}）`);
        return r.ok ? 0 : 1;
    }
    if (argv.includes("--mutation-check")) {
        const r = needles.mutationCheck();
        for (const s of r.survived) console.error(`  变异未被任何夹具抓到：${s}`);
        console.log(`check-privacy --mutation-check：${r.total} 个变异，存活 ${r.survived.length}`);
        return r.survived.length ? 1 : 0;
    }
    const opts = {};
    const free = [];
    for (let i = 0; i < argv.length; i++) {
        if (argv[i] in OWN) opts[argv[i]] = OWN[argv[i]] ? argv[++i] : true;
        else free.push(argv[i]);
    }
    if (free.length && !opts["--commits"]) throw new Error(`无法识别的参数：${free.join(" ")}`);
    const root = opts["--root"] ? path.resolve(opts["--root"]) : ROOT;
    const personal = needles.loadPersonalNeedles({ cwd: root, mode: needles.needlesMode(argv) });
    if (opts["--commits"]) {
        const found = scanCommits(free, root, personal.needles, { messages: !opts["--no-messages"] });
        return report(found, personal.notes);
    }
    if (opts["--text"]) {
        const src = opts["--text"];
        const text = needles.plainText(fs.readFileSync(src === "-" ? 0 : src), "--text 的输入");
        return report(tag("文本", scan(text, {}, { personal: personal.needles })), personal.notes);
    }
    return report(scanTree(root, personal.needles), personal.notes);
}

if (kit.isMain(import.meta.url)) {
    try {
        process.exitCode = main(process.argv.slice(2));
    } catch (e) {
        console.error(`check-privacy：${e.message}`);
        process.exitCode = 1;
    }
}
