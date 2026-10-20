// SPDX-License-Identifier: GPL-3.0-or-later
//
// 递归删除门禁：仓内脚本、测试、CMake、workflow 里新增的递归删除，必须经 scripts/lib/TempDir.psm1（PowerShell）或
// scripts/lib/safe-rm.mjs（node）的入口，要不然就在那一行（或上面三行内）带一行豁免注释：
//     rinne-delete-ok: <为什么删得是安全的：本进程新建的目录 / 仓内写死的输出目录，以及在哪里核对了路径>
// 判据按语言各一组规则（见 RULES）：Remove-Item 带 -Recurse、目录的 Delete(…, $true)、rd /s、fs.rm / rmSync / rmdirSync 带 recursive、
// shutil.rmtree、rm -r 一类、REMOVE_RECURSE 与 cmake -E remove_directory，以及 git clean -f、git worktree remove（它们连未跟踪的
// 文件一起清）。整行注释不算。豁免只认「rinne-delete-ok:」后面跟着一句话理由。
// 已知的盲区：把命令名放进变量再调用（& $cmd -Recurse）这类间接形式认不出来，靠评审把关；这里只管字面形态。
// 自测：每条规则一个坏样例、几个正例；mutationCheck 逐条屏蔽规则，自测必须因此变红。

import path from "node:path";
import { caseResult, readText, trackedFiles } from "../lib/check-kit.mjs";
import { selfTestCases as safeRmCases } from "../lib/safe-rm.mjs";

/** 模块自己与本检查：删除的实现就在这里，不再要求豁免。 */
const ALLOWED = new Set([
    "scripts/lib/TempDir.psm1",
    "scripts/lib/safe-rm.mjs",
    "scripts/checks.d/recursive-delete.mjs",
]);

/**
 * 待整改、暂时放行的文件：自测收尾删临时目录的那一处，整改要等并行改动该文件的变更合入之后再做（改成经 TempDir.psm1）。
 * 整改完从这里删掉；清单里每多一项都要写明原因。
 */
const PENDING = new Set(["scripts/build.ps1"]);

const EXEMPT = /rinne-delete-ok:\s*\S.{3,}/;
const EXEMPT_WINDOW = 3;
const PS = ["ps1", "psm1"];
const JS = ["mjs", "js", "cjs", "ts"];
const SH = ["sh", "bash", "yml", "yaml"];

// 规则里的片段拼接，免得本文件自己的正则文字出现在别的检查的扫描里
const j = (...p) => p.join("");

/** 每条规则：name、适用的扩展名、判据（regex 或 (lines, i) => boolean）。 */
export const RULES = [
    {
        name: "powershell-remove-recurse",
        exts: PS,
        test: (l) =>
            new RegExp(j("\\bRemove-", "Item\\b.*\\s-Rec\\w*"), "i").test(l) ||
            new RegExp(j("\\s-Rec\\w*\\b.*\\bRemove-", "Item\\b"), "i").test(l) ||
            /(^|[\s;|&({'"])(rm|ri|rd|rmdir|del|erase)\s.*\s-Rec\w*/i.test(l),
    },
    {
        name: "dotnet-directory-delete-recursive",
        exts: PS,
        test: (l) => /Directory\]::Delete\([^)]*,\s*\$true\s*\)/i.test(l),
    },
    { name: "cmd-rd-s", exts: [...PS, "bat", "cmd"], test: (l) => /(^|[\s;&|"'])(rd|rmdir)\s+\/s\b/i.test(l) },
    {
        name: "node-rm-recursive",
        exts: JS,
        test: (l, lines, i) =>
            /\.(rm|rmSync|rmdir|rmdirSync)\(/.test(l) && lines.slice(i, i + 4).some((x) => /\brecursive\b/.test(x)),
    },
    { name: "node-rimraf", exts: JS, test: (l) => /\brimraf\b/.test(l) },
    { name: "python-rmtree", exts: ["py"], test: (l) => /\brmtree\s*\(/.test(l) },
    { name: "shell-rm-r", exts: SH, test: (l) => /(^|[\s;&|('"])rm\s+(-[a-zA-Z]*[rR][a-zA-Z]*|--recursive)\b/.test(l) },
    {
        name: "cmake-remove-recurse",
        exts: ["cmake"],
        test: (l) => /\bREMOVE_RECURSE\b|-E\s+(remove_directory|rm\s+-[a-zA-Z]*r)/.test(l),
    },
    {
        name: "git-clean-force",
        exts: [...PS, ...JS, ...SH],
        test: (l, _lines, _i, ext) =>
            /['"]clean['"]\s*,\s*['"]-[a-zA-Z]*f/.test(l) ||
            (SH.includes(ext) && /\bgit\b.*\sclean\s+-[a-zA-Z]*f/.test(l)),
    },
    {
        name: "git-worktree-remove",
        exts: [...PS, ...JS, ...SH],
        test: (l, _lines, _i, ext) =>
            /['"]worktree['"]\s*,\s*['"]remove/.test(l) ||
            (SH.includes(ext) && /\bgit\b.*\sworktree\s+remove\b/.test(l)),
    },
];

const extOf = (rel) => (path.basename(rel) === "CMakeLists.txt" ? "cmake" : path.extname(rel).slice(1).toLowerCase());
const isComment = (l) => /^\s*(#|\/\/|\*|\/\*|<#|<!--)/.test(l);

/** 一份文本里的违规：[{ line, rule }]。rules 缺省是全部规则（mutationCheck 传去掉一条的）。 */
export function scanText(rel, text, rules = RULES) {
    if (ALLOWED.has(rel) || PENDING.has(rel)) return [];
    const ext = extOf(rel);
    const use = rules.filter((r) => r.exts.includes(ext));
    if (!use.length) return [];
    const lines = text.split(/\r?\n/);
    const out = [];
    lines.forEach((l, i) => {
        if (isComment(l)) return;
        for (const r of use) {
            if (!r.test(l, lines, i, ext)) continue;
            const exempt = lines.slice(Math.max(0, i - EXEMPT_WINDOW), i + 1).some((x) => EXEMPT.test(x));
            if (!exempt) out.push({ line: i + 1, rule: r.name });
        }
    });
    return out;
}

// ---- 夹具：坏样例与正例都是内存里的文本，片段拼接，不落盘 ----
const rm = j("Remove-", "Item");
const fx = [
    ["powershell-remove-recurse", "a.ps1", `${rm} -LiteralPath $d -Recurse -Force`],
    ["powershell-remove-recurse", "a.ps1", `${rm} -Recurse -Force -LiteralPath $d`],
    ["powershell-remove-recurse", "a.ps1", `rm $d -Recurse`],
    ["dotnet-directory-delete-recursive", "a.ps1", "[IO.Directory]::Delete($d, $true)"],
    ["cmd-rd-s", "a.cmd", "rd /s /q build"],
    ["node-rm-recursive", "a.mjs", "fs.rmSync(dir, { recursive: true, force: true });"],
    ["node-rm-recursive", "a.mjs", "fs.rmSync(dir, {\n    force: true,\n    recursive: true,\n});"],
    ["node-rm-recursive", "a.mjs", "await fsp.rm(dir, { recursive: true });"],
    ["node-rimraf", "a.js", "rimraf(dir)"],
    ["python-rmtree", "a.py", "shutil.rmtree(tmp, ignore_errors=True)"],
    ["shell-rm-r", "a.sh", 'rm -rf "$dir"'],
    ["shell-rm-r", "a.sh", `trap 'rm -rf "$tmp"' EXIT`],
    ["shell-rm-r", "w.yml", '          rm -fr "$RUNNER_TEMP/x"'],
    ["cmake-remove-recurse", "CMakeLists.txt", 'file(REMOVE_RECURSE "${dir}")'],
    ["cmake-remove-recurse", "x.cmake", "execute_process(COMMAND cmake -E remove_directory ${d})"],
    ["git-clean-force", "a.ps1", "Invoke-RinneGit @('clean', '-ffdx', '--quiet')"],
    ["git-clean-force", "a.sh", "git -C wt/x clean -ffdx"],
    ["git-worktree-remove", "a.ps1", "Invoke-RinneGit @('worktree', 'remove', $wt)"],
    ["git-worktree-remove", "a.sh", "git worktree remove --force wt/x"],
];
const good = [
    ["a.ps1", `${rm} -LiteralPath $f -Force`],
    ["a.ps1", "Copy-Item -LiteralPath $b -Destination $s -Recurse"],
    ["a.ps1", "Get-ChildItem -Recurse -Filter *.vst3"],
    ["a.ps1", `# ${rm} $d -Recurse：注释里的不算`],
    ["a.mjs", "fs.rmSync(file, { force: true });"],
    ["a.mjs", "fs.rmdirSync(emptyDir);"],
    ["a.mjs", "removeTempDir(dir);"],
    ["a.py", "os.remove(path)"],
    ["a.sh", 'rm -f "$file"'],
    ["a.sh", "git clean -n"],
    ["README.md", "rm -rf x"],
    ["a.ps1", `${rm} $d -Recurse -Force # rinne-delete-ok: 只删上面新建的目录，删前核对过真实路径`],
    ["a.ps1", `# rinne-delete-ok: 本进程新建的目录，前缀已断言\n${rm} $d -Recurse`],
    ["a.mjs", "// rinne-delete-ok: 仓内写死的输出目录，cleanVerdict 核对过\nfs.rmSync(out, { recursive: true });"],
    ["scripts/lib/TempDir.psm1", `${rm} -LiteralPath $c.Real -Recurse -Force`],
    ["scripts/lib/safe-rm.mjs", "fs.rmSync(dir, { recursive: true });"],
];

/** 对一组夹具求自测用例；rules 是被试的规则集（正常是全部，mutationCheck 里少一条）。 */
function cases(rules) {
    const out = [];
    for (const [rule, rel, text] of fx) {
        const hits = scanText(rel, text, rules);
        out.push({
            label: `坏样例判红：${rule}（${rel}：${text.split("\n")[0].slice(0, 40)}）`,
            bad: true,
            passed: hits.some((h) => h.rule === rule),
        });
    }
    for (const [rel, text] of good) {
        out.push({
            label: `正例放行：${rel}：${text.split("\n")[0].slice(0, 40)}`,
            bad: false,
            passed: scanText(rel, text, rules).length === 0,
        });
    }
    // 豁免要写理由：空的或太短的不算；离得太远的不算
    out.push({
        label: "豁免没写理由：仍判红",
        bad: true,
        passed: scanText("a.mjs", "// rinne-delete-ok:\nfs.rmSync(d, { recursive: true });", rules).length > 0,
    });
    out.push({
        label: "豁免离得太远（隔了四行）：仍判红",
        bad: true,
        passed:
            scanText("a.mjs", "// rinne-delete-ok: 理由写得很清楚\n\n\n\nfs.rmSync(d, { recursive: true });", rules)
                .length > 0,
    });
    return out;
}

export default {
    name: "recursive-delete",
    group: "compliance",
    inputs: [],
    run(ctx) {
        const lines = [];
        for (const rel of trackedFiles(ctx.root)) {
            if (!RULES.some((r) => r.exts.includes(extOf(rel)))) continue;
            const text = readText(path.join(ctx.root, rel));
            if (text === null) continue;
            for (const h of scanText(rel, text)) {
                lines.push(
                    `${rel}:${h.line} 递归删除（${h.rule}）没经 TempDir.psm1 / safe-rm.mjs，也没有「rinne-delete-ok: 理由」`,
                );
            }
        }
        return { ok: lines.length === 0, lines };
    },
    selfTest: () => {
        const r = caseResult([
            ...cases(RULES),
            ...safeRmCases().map((c) => ({ label: `safe-rm：${c.label}`, bad: c.bad, passed: c.passed })),
        ]);
        return r;
    },
    /** 逐条屏蔽规则，夹具的坏样例必须因此变红。 */
    mutationCheck: () => {
        const lines = [];
        for (const r of RULES) {
            const rest = RULES.filter((x) => x !== r);
            if (cases(rest).every((c) => c.passed)) lines.push(`屏蔽规则 ${r.name} 后自测仍是绿的`);
        }
        return { ok: lines.length === 0, lines };
    },
};
