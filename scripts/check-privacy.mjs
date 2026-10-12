// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/ci.json
//
// 公开仓隐私门禁：privacy-needles.mjs 的通用规则加上仓外个人针，扫三种对象。
//   node scripts/check-privacy.mjs [--root <dir>]      git 跟踪的工作树（内容与路径）
//   node scripts/check-privacy.mjs --commits <rev-list 参数…>
//        区间内每个提交的消息、新增行与路径。推上去的提交经 refs/pull 永久公开，
//        工作树干净不代表历史干净，所以推送前与 PR 上都要逐提交扫。
//   node scripts/check-privacy.mjs --text <文件|->     一段公开文本（PR 标题与正文、评论草稿）
//   --self-test（规则夹具 + 端到端假针）  --mutation-check  --list-rules
// 只打印位置与打码片段，个人针只打印序号。退出码：0 干净；1 有命中、配置缺失或自测失败。

import fs from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import * as kit from "./lib/check-kit.mjs";
import * as needles from "./lib/privacy-needles.mjs";

const { ROOT, caseResult, git, readText, tempRepo, trackedFiles } = kit;

const { scan, redact, headingsOf } = needles;
const SELF = fileURLToPath(import.meta.url);

export const formatFinding = (f) => `  ${f.where}:${f.line}:${f.col} [${f.rule.id}] ${redact(f)}`;
const tag = (where, list) => list.map((f) => ({ ...f, where }));

export function scanTree(root, personal) {
    const out = [];
    for (const rel of trackedFiles(root)) {
        out.push(...tag(`${rel}（路径）`, scan(rel, {}, { personal })));
        const text = readText(path.join(root, rel));
        if (text !== null) out.push(...tag(rel, scan(text, { path: rel, headings: headingsOf(text) }, { personal })));
    }
    return out;
}

/** 逐提交扫消息（可关）、新增行与新路径；合并提交看相对第一父提交带进来的全部内容。 */
export function scanCommits(revArgs, cwd, personal, { messages = true } = {}) {
    const out = [];
    for (const sha of git(["rev-list", ...revArgs], { cwd }).split("\n").filter(Boolean)) {
        const s7 = sha.slice(0, 7);
        const [parents, msg] = git(["log", "-1", "--format=%P%x00%B", sha], { cwd }).split("\0");
        if (messages) out.push(...tag(`${s7} 提交消息`, scan(msg, {}, { personal })));
        // 合并提交用 diff 对第一父提交，普通提交用 show（根提交也适用）
        const diff = (...opts) =>
            parents.trim().includes(" ")
                ? git(["diff", "--no-renames", ...opts, `${sha}^1`, sha], { cwd })
                : git(["show", "--no-renames", "--format=", ...opts, sha], { cwd });
        // 路径单独列：二进制文件和空文件在补丁里没有 +++ 行
        for (const file of diff("--name-only", "-z", "--diff-filter=AMT").split("\0").filter(Boolean)) {
            out.push(...tag(`${s7} ${file}（路径）`, scan(file, {}, { personal })));
        }
        const patch = diff("--no-color", "-U0", "--src-prefix=a/", "--dst-prefix=b/");
        let ctx = null;
        let header = false;
        let lineNo = 0;
        for (const line of patch.split("\n")) {
            if (line.startsWith("diff --git ")) {
                header = true;
                ctx = null;
            } else if (header && line.startsWith("+++ ")) {
                const file = line === "+++ /dev/null" ? null : line.slice(6);
                ctx = file && {
                    path: file,
                    get headings() {
                        return (this.h ??= headingsOf(git(["show", `${sha}:${file}`], { cwd, allowFail: true }) || ""));
                    },
                };
            } else if (line.startsWith("@@")) {
                header = false;
                lineNo = Number(/\+(\d+)/.exec(line)?.[1] ?? 0);
            } else if (ctx && !header && line.startsWith("+")) {
                const hits = scan(line.slice(1), ctx, { personal }).map((f) => ({ ...f, line: lineNo }));
                out.push(...tag(`${s7} ${ctx.path}`, hits));
                lineNo++;
            }
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
    console.error("规则见 --list-rules。确属误报就改写句子；只有逐字第三方目录有豁免，个人针没有豁免。");
    return 1;
}

/** 规则夹具，加上端到端：假针写进临时仓的跟踪文件，必须判红且输出里不出现针的明文。 */
export function selfTest() {
    const cases = needles.runCases();
    const repo = tempRepo();
    const nf = path.join(repo.dir, "..", `${path.basename(repo.dir)}-needles.txt`);
    const base = kit.isolatedEnv();
    for (const k of ["RINNE_PRIVACY_LOCAL", "PRIVACY_NEEDLES", "GITHUB_ACTIONS", "CI"]) delete base[k];
    const run = (args, env) => {
        const r = spawnSync(process.execPath, [SELF, "--root", repo.dir, ...args], {
            cwd: repo.dir,
            env: { ...base, ...env },
            encoding: "utf8",
        });
        return { code: r.status, out: `${r.stdout}${r.stderr}` };
    };
    const check = (label, bad, ok) => cases.push({ label, bad, passed: !!ok });
    try {
        fs.writeFileSync(nf, `# 自测\n${needles.FAKE_NEEDLE}\n`);
        repo.write("docs/clean.md", "干净的文本\n");
        repo.git(["add", "."]);
        repo.git(["commit", "-q", "-m", "ci(test): 干净"]);
        let r = run([], { RINNE_PRIVACY_LOCAL: nf });
        check("端到端：干净的树通过", false, r.code === 0);
        r = run([], { GITHUB_ACTIONS: "true" });
        check("端到端：CI 缺 secret 时注记并只跑通用规则", false, r.code === 0 && r.out.includes("PRIVACY_NEEDLES"));
        r = run([], {});
        check("端到端：本机未配置个人针判红", true, r.code === 1);
        r = run([], { RINNE_PRIVACY_LOCAL: `${nf}.missing` });
        check("端到端：个人针文件缺失判红", true, r.code === 1);
        repo.write("docs/leak.md", `说明 ${needles.FAKE_NEEDLE}\n`);
        repo.git(["add", "."]);
        repo.git(["commit", "-q", "-m", "ci(test): 夹带"]);
        r = run([], { RINNE_PRIVACY_LOCAL: nf });
        check("端到端：跟踪文件含假针判红且不回显", true, r.code === 1 && !r.out.includes(needles.FAKE_NEEDLE));
        r = run([], { GITHUB_ACTIONS: "true", PRIVACY_NEEDLES: `x-unused\n${needles.FAKE_NEEDLE}` });
        check("端到端：CI 经 PRIVACY_NEEDLES 判红", true, r.code === 1);
        repo.git(["rm", "-q", "docs/leak.md"]);
        repo.git(["commit", "-q", "-m", "ci(test): 删掉"]);
        r = run([], { RINNE_PRIVACY_LOCAL: nf });
        check("端到端：删掉后工作树通过", false, r.code === 0);
        r = run(["--commits", "HEAD"], { RINNE_PRIVACY_LOCAL: nf });
        check("端到端：历史里仍有假针，--commits 判红", true, r.code === 1);
    } finally {
        repo.cleanup();
        fs.rmSync(nf, { force: true });
    }
    return caseResult(cases);
}

function main(argv) {
    kit.assertNodeVersion();
    const at = (flag) => argv.indexOf(flag);
    if (argv.includes("--list-rules")) {
        for (const r of needles.RULES) console.log(`  ${r.id}${r.verbatimOk ? "（逐字第三方目录豁免）" : ""}`);
        console.log(`  personal（仓外个人针，无豁免）\n逐字第三方目录：${needles.VERBATIM_DIRS.join(" ")}`);
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
    const root = at("--root") >= 0 ? path.resolve(argv[at("--root") + 1]) : ROOT;
    let personal;
    try {
        personal = needles.loadPersonalNeedles({ cwd: root });
    } catch (e) {
        console.error(`check-privacy：${e.message}`);
        return 1;
    }
    if (at("--commits") >= 0) {
        return report(scanCommits(argv.slice(at("--commits") + 1), root, personal.needles), personal.notes);
    }
    if (at("--text") >= 0) {
        const src = argv[at("--text") + 1];
        const text = fs.readFileSync(src === "-" ? 0 : src, "utf8");
        return report(tag("文本", scan(text, {}, { personal: personal.needles })), personal.notes);
    }
    return report(scanTree(root, personal.needles), personal.notes);
}

if (kit.isMain(import.meta.url)) process.exitCode = main(process.argv.slice(2));
