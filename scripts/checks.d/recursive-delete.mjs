// SPDX-License-Identifier: GPL-3.0-or-later
//
// 递归删除门禁：仓内脚本、测试、CMake、workflow 里新增的递归删除，必须经 scripts/lib/TempDir.psm1（PowerShell）或
// scripts/lib/safe-rm.mjs（node）的入口，要不然就在那一行（或紧挨着的上一行）带一条豁免注释：
//     rinne-delete-ok: <为什么删得是安全的：本进程新建的目录 / 仓内写死的输出目录，以及在哪里核对了路径>
// 判据按写法各一组规则（见 RULES）：Remove-Item 带 -Recurse（含 -r 一类缩写）、目录的 Delete(…, $true)、rd /s、
// fs.rm / rmSync / rmdirSync 带 recursive、shutil.rmtree、rm -r 一类（含数组形式的子进程调用）、find -delete、REMOVE_RECURSE 与
// cmake -E remove_directory，以及 git clean -f、git worktree remove（它们连未跟踪的文件一起清）。PowerShell 与 shell 的续行先拼成
// 一条再判；workflow 的 run 块里可能是 pwsh，所以 yml 同时套 PowerShell 与 shell 两组。整行注释不算，注释符按语言区分。
// 豁免：只认注释里的「rinne-delete-ok: 理由」（理由至少 10 个字符），写在同一行或紧挨的上一行；一条只豁免一处。
// 范围：脚本类文件（ps1、psm1、bat、cmd、js 系、py、sh、yml、cmake）。C / C++ 不在范围内，靠评审把关
// （tests/core/ipc/backends/TempDir.h 的 remove_all 是 mkdtemp 新建目录加路径核对，已在清单里标「合规」）。
// removeFixedDir 的调用不是一律放行：参数必须是 safe-rm.mjs 的白名单（FIXED_DIRS）里的字面量。
// 只认字面写法。不覆盖的形态：命令名或选项放进变量或拼接出来（& $cmd -Recurse、"r" + "m"、$opts = '-Recurse'）、经别名或包装函数
// 间接调用、别的语言的递归删除接口（C / C++ 的 remove_all、deleteRecursively、Java、Go 等，靠评审把关）、
// 不在脚本类文件里的删除（.md 里的命令示例不算）。
// 自测：每条规则至少一个只有它能命中的坏样例、几个正例；mutationCheck 逐条屏蔽规则，自测必须因此变红。

import path from "node:path";
import { caseResult, readText, trackedFiles } from "../lib/check-kit.mjs";
import { FIXED_DIRS, selfTestCases as safeRmCases } from "../lib/safe-rm.mjs";

/** 模块自己与本检查：删除的实现就在这里，不再要求豁免。 */
const ALLOWED = new Set([
    "scripts/lib/TempDir.psm1",
    "scripts/lib/safe-rm.mjs",
    "scripts/checks.d/recursive-delete.mjs",
]);

/**
 * 待整改、暂时放行的行：自测收尾删临时目录的那一处，整改要等并行改动该文件的变更合入之后再做（改成经 TempDir.psm1）。
 * 只放行匹配这条正则的行，文件里别的递归删除照判。整改完从这里删掉；每多一项都要写明原因。
 */
const PENDING = new Map([["scripts/build.ps1", /Remove-Item -LiteralPath \$tmp -Recurse -Force/]]);

const EXEMPT = /(#|\/\/|<#|\/\*|::)\s*rinne-delete-ok:\s*\S.{8,}/;
const PS = ["ps1", "psm1"];
const JS = ["mjs", "js", "cjs", "ts"];
const SH = ["sh", "bash", "yml", "yaml"];
const CMD = ["bat", "cmd"];

// 规则里的片段拼接，免得本文件自己的正则文字出现在别的检查的扫描里
const j = (...p) => p.join("");

// -Recurse 的任何前缀缩写（-r、-re、-rec……）
const REC = "-r(e(c(u(r(se?)?)?)?)?)?(?![\\w-])";
const RM_OPT = "-[a-zA-Z]*[rR][a-zA-Z]*";

// removeFixedDir 的调用：参数必须是 safe-rm.mjs 白名单里的字面量（rel: [".rinne-review"] 这种），变量和别的目录都不行
const FIXED_LITERALS = FIXED_DIRS.map(
    (d) =>
        new RegExp(
            "rel:\\s*\\[\\s*" +
                d.map((x) => JSON.stringify(x).replace(/[.*+?^${}()|[\]\\]/g, "\\$&")).join("\\s*,\\s*") +
                "\\s*,?\\s*\\]",
        ),
);

/** 每条规则：name、适用的扩展名、判据 (l, lines, i, ext) => boolean；l 是拼好续行的那一条语句。 */
export const RULES = [
    {
        name: "powershell-remove-then-recurse",
        exts: [...PS, ...SH, ...JS, "py"],
        test: (l) => new RegExp(j("\\bRemove-", "Item\\b.*\\s", REC), "i").test(l),
    },
    {
        name: "powershell-recurse-then-remove",
        exts: [...PS, ...SH, ...JS, "py"],
        test: (l) => new RegExp(j("\\s", REC, ".*\\bRemove-", "Item\\b"), "i").test(l),
    },
    {
        name: "powershell-alias-recurse",
        exts: [...PS, ...SH],
        test: (l) => new RegExp(j("(^|[\\s;|&({'\"])(rm|ri|rd|rmdir|del|erase)\\s(.*\\s)?", REC), "i").test(l),
    },
    {
        name: "dotnet-directory-delete-recursive",
        exts: PS,
        test: (l) => /Directory\]::Delete\([^)]*,\s*\$true\s*\)/i.test(l),
    },
    {
        name: "cmd-rd-s",
        exts: [...PS, ...CMD, ...SH, ...JS, "py"],
        test: (l) => /(^|[\s;&|"'])(rd|rmdir)\s+(\/[a-z]\s+)*\/s\b/i.test(l),
    },
    {
        name: "cmd-rd-s-array",
        exts: [...JS, "py"],
        test: (l) => /['"](rd|rmdir)['"]\s*,\s*(['"]\/[a-z]['"]\s*,\s*)*['"]\/s['"]/i.test(l),
    },
    {
        name: "node-rm-recursive",
        exts: JS,
        test: (l, lines, i) =>
            /(^|[^\w$])(rm|rmSync)\(/.test(l) && lines.slice(i, i + 4).some((x) => /\brecursive\b/.test(x)),
    },
    {
        name: "node-rmdir-recursive",
        exts: JS,
        test: (l, lines, i) =>
            /(^|[^\w$])(rmdir|rmdirSync)\(/.test(l) && lines.slice(i, i + 4).some((x) => /\brecursive\b/.test(x)),
    },
    {
        name: "fixed-dir-call-not-whitelisted",
        exts: JS,
        test: (l, lines, i) =>
            /\bremoveFixedDir\(/.test(l) &&
            !/^\s*import\b/.test(l) &&
            !FIXED_LITERALS.some((re) => re.test(lines.slice(i, i + 3).join(" "))),
    },
    { name: "node-rimraf", exts: JS, test: (l) => /\brimraf\b/.test(l) },
    { name: "python-rmtree", exts: ["py"], test: (l) => /\brmtree\s*\(/.test(l) },
    {
        name: "shell-rm-r",
        exts: [...SH, ...JS, "py", "cmake"],
        test: (l) => new RegExp(j("(^|[\\s;&|('\"/])rm(\\s+-[a-zA-Z]+)*\\s+", RM_OPT, "\\b")).test(l),
    },
    {
        name: "shell-rm-recursive-long",
        exts: [...SH, ...JS, "py", "cmake"],
        test: (l) => /(^|[\s;&|('"/])rm(\s+-[a-zA-Z]+)*\s+--recursive\b/.test(l),
    },
    {
        name: "shell-rm-array",
        exts: [...JS, "py"],
        test: (l) => new RegExp(j("['\"]rm['\"]\\s*,\\s*\\[?\\s*(['\"]-[a-zA-Z]+['\"]\\s*,\\s*)*['\"](", RM_OPT, "|--recursive)['\"]")).test(l),
    },
    { name: "find-delete", exts: [...SH, ...JS, "py"], test: (l) => /\bfind\b.*\s-delete\b/.test(l) },
    {
        name: "cmake-remove-recurse",
        exts: ["cmake"],
        test: (l) => /\bREMOVE_RECURSE\b|-E\s+remove_directory/.test(l),
    },
    {
        name: "git-clean-force-array",
        exts: [...PS, ...JS, "py"],
        test: (l) => /['"]clean['"]\s*,\s*(['"]-[^'"]*['"]\s*,\s*)*['"](-[a-zA-Z]*f[a-zA-Z]*|--force)['"]/.test(l),
    },
    {
        name: "git-clean-force-text",
        exts: [...PS, ...SH, ...JS, "py", "cmake"],
        test: (l) => /\bgit\b.*\sclean(\s+-\S+)*\s+(-[a-zA-Z]*f[a-zA-Z]*|--force)\b/.test(l),
    },
    {
        name: "git-worktree-remove-array",
        exts: [...PS, ...JS, "py"],
        test: (l) => /['"]worktree['"]\s*,\s*['"]remove/.test(l),
    },
    {
        name: "git-worktree-remove-text",
        exts: [...PS, ...SH, ...JS, "py", "cmake"],
        test: (l) => /\bgit\b.*\sworktree\s+remove\b/.test(l),
    },
];

const extOf = (rel) => (path.basename(rel) === "CMakeLists.txt" ? "cmake" : path.extname(rel).slice(1).toLowerCase());
// 注释符按语言分：shell / yml 里 case 的兜底分支 `*) …` 以 * 开头，不能当注释
const CLIKE = /^\s*(\/\/|\/\*|\*)/;
const HASH = /^\s*#/;
const COMMENT = {
    ps1: /^\s*(#|<#)/,
    psm1: /^\s*(#|<#)/,
    bat: /^\s*(rem\b|::)/i,
    cmd: /^\s*(rem\b|::)/i,
};
for (const e of JS) COMMENT[e] = CLIKE;
const isComment = (l, ext) => (COMMENT[ext] ?? HASH).test(l);

/** 续行拼成一条语句：PowerShell 行尾反引号，shell / yml 行尾反斜杠。 */
function statement(lines, i, ext) {
    const cont = PS.includes(ext) ? "`" : SH.includes(ext) ? "\\" : null;
    let s = lines[i];
    for (let k = i; cont && k < i + 5 && lines[k].trimEnd().endsWith(cont) && k + 1 < lines.length; k++) {
        s = s.trimEnd().slice(0, -1) + " " + lines[k + 1].trim();
    }
    return s;
}

/** 一份文本里的违规：[{ line, rule }]。rules 缺省是全部规则（mutationCheck 传去掉一条的）。 */
export function scanText(rel, text, rules = RULES) {
    if (ALLOWED.has(rel)) return [];
    const pending = PENDING.get(rel);
    const ext = extOf(rel);
    const use = rules.filter((r) => r.exts.includes(ext));
    if (!use.length) return [];
    const lines = text.split(/\r?\n/);
    const usedMarkers = new Set(); // 一条豁免只盖一处
    const out = [];
    let inBlock = false; // PowerShell 的 <# … #> 多行注释
    lines.forEach((l, i) => {
        if (PS.includes(ext)) {
            if (inBlock) {
                if (l.includes("#>")) inBlock = false;
                return;
            }
            if (/^s*<#/.test(l) && !l.includes("#>")) {
                inBlock = true;
                return;
            }
        }
        if (isComment(l, ext)) return;
        if (pending && pending.test(l)) return; // 只放行清单里写明的那一行，文件里新增的递归删除照判
        const stmt = statement(lines, i, ext);
        const hits = use.filter((r) => r.test(stmt, lines, i, ext));
        if (!hits.length) return;
        const marker = [i, i - 1].find((k) => k >= 0 && !usedMarkers.has(k) && EXEMPT.test(lines[k]));
        if (marker !== undefined) usedMarkers.add(marker);
        else for (const r of hits) out.push({ line: i + 1, rule: r.name });
    });
    return out;
}

// ---- 夹具：坏样例与正例都是内存里的文本，片段拼接，不落盘；每条规则至少一个只有它能命中的坏样例 ----
const rm = j("Remove-", "Item");
const fx = [
    ["powershell-remove-then-recurse", "a.ps1", `${rm} -LiteralPath $d -Recurse -Force`],
    ["powershell-remove-then-recurse", "a.ps1", `${rm} $d -r -fo`],
    ["powershell-remove-then-recurse", "a.ps1", `${rm} -LiteralPath $d \`\n    -Recurse -Force`],
    ["powershell-remove-then-recurse", "w.yml", `        shell: pwsh\n        run: ${rm} $d -Recurse`],
    ["powershell-remove-then-recurse", "a.mjs", `execSync("powershell ${rm} -Recurse " + d);`],
    ["powershell-recurse-then-remove", "a.ps1", `Get-ChildItem $d -Recurse | ${rm} -Force`],
    ["powershell-alias-recurse", "a.ps1", `rm $d -Recurse`],
    ["powershell-alias-recurse", "a.ps1", `rm -r -fo $d`],
    ["dotnet-directory-delete-recursive", "a.ps1", "[IO.Directory]::Delete($d, $true)"],
    ["cmd-rd-s", "a.cmd", "rd /s /q build"],
    ["cmd-rd-s", "a.ps1", "cmd /c rd /q /s $d"],
    ["cmd-rd-s-array", "a.mjs", 'spawnSync("cmd", ["/c", "rd", "/s", "/q", d]);'],
    ["node-rm-recursive", "a.mjs", "fs.rmSync(dir, { recursive: true, force: true });"],
    ["node-rm-recursive", "a.mjs", "fs.rmSync(dir, {\n    force: true,\n    recursive: true,\n});"],
    ["node-rm-recursive", "a.mjs", "await fsp.rm(dir, { recursive: true });"],
    ["node-rm-recursive", "a.mjs", "rmSync(dir, { recursive: true });"],
    ["node-rmdir-recursive", "a.mjs", "fs.rmdirSync(dir, { recursive: true });"],
    ["node-rmdir-recursive", "a.mjs", "await fsp.rmdir(dir, { recursive: true });"],
    ["node-rimraf", "a.js", "rimraf(dir)"],
    ["fixed-dir-call-not-whitelisted", "a.mjs", "removeFixedDir(out, { rel: parts });"],
    ["fixed-dir-call-not-whitelisted", "a.mjs", 'removeFixedDir(out, { rel: ["wt"] });'],
    ["python-rmtree", "a.py", "shutil.rmtree(tmp, ignore_errors=True)"],
    ["shell-rm-r", "a.sh", 'rm -rf "$dir"'],
    ["shell-rm-r", "a.sh", `trap 'rm -rf "$tmp"' EXIT`],
    ["shell-rm-r", "w.yml", '          rm -fr "$RUNNER_TEMP/x"'],
    ["shell-rm-r", "a.sh", 'rm -f -r "$d"'],
    ["shell-rm-r", "a.sh", '  *) rm -rf "$d" ;;'],
    ["shell-rm-r", "a.sh", '/bin/rm -rf "$d"'],
    ["shell-rm-r", "a.mjs", 'execSync("rm -rf " + d);'],
    ["shell-rm-r", "a.py", 'os.system("rm -rf " + d)'],
    ["shell-rm-r", "x.cmake", "execute_process(COMMAND rm -rf ${d})"],
    ["shell-rm-r", "x.cmake", "execute_process(COMMAND ${CMAKE_COMMAND} -E rm -f -r ${d})"],
    ["shell-rm-recursive-long", "a.sh", 'rm --recursive "$d"'],
    ["shell-rm-array", "a.py", 'subprocess.run(["rm", "-rf", d])'],
    ["shell-rm-array", "a.mjs", 'spawnSync("rm", ["-r", d]);'],
    ["find-delete", "a.sh", 'find "$d" -delete'],
    ["cmake-remove-recurse", "CMakeLists.txt", 'file(REMOVE_RECURSE "${dir}")'],
    ["cmake-remove-recurse", "x.cmake", "execute_process(COMMAND cmake -E remove_directory ${d})"],
    ["git-clean-force-array", "a.ps1", "Invoke-RinneGit @('clean', '-ffdx', '--quiet')"],
    ["git-clean-force-array", "a.ps1", "Invoke-RinneGit @('clean', '-d', '-f')"],
    ["git-clean-force-array", "a.ps1", "Invoke-RinneGit @('clean', '--force')"],
    ["git-clean-force-text", "a.sh", "git -C wt/x clean -ffdx"],
    ["git-clean-force-text", "a.sh", "git clean -d -f"],
    ["git-clean-force-text", "a.sh", "git clean --force"],
    ["git-clean-force-text", "a.ps1", "git -C $tip clean -ffdx"],
    ["git-worktree-remove-array", "a.ps1", "Invoke-RinneGit @('worktree', 'remove', $wt)"],
    ["git-worktree-remove-text", "a.sh", "git worktree remove --force wt/x"],
    ["git-worktree-remove-text", "a.ps1", "git worktree remove --force $wt"],
    ["powershell-remove-then-recurse", "scripts/build.ps1", `${rm} -LiteralPath $other -Recurse -Force`],
];
const good = [
    ["a.ps1", `${rm} -LiteralPath $f -Force`],
    ["a.ps1", "Copy-Item -LiteralPath $b -Destination $s -Recurse"],
    ["a.ps1", "Get-ChildItem -Recurse -Filter *.vst3"],
    ["a.ps1", `# ${rm} $d -Recurse：注释里的不算`],
    ["a.mjs", "fs.rmSync(file, { force: true });"],
    ["a.mjs", "fs.rmdirSync(emptyDir);"],
    ["a.mjs", "removeTempDir(dir);"],
    ["a.mjs", 'removeFixedDir(out, { rel: [".rinne-review"] });'],
    ["a.mjs", 'import { removeFixedDir } from "./lib/safe-rm.mjs";'],
    ["a.mjs", 'removeFixedDirInTemp(out, { tempRoot: tmp, rel: ["build"] });'],
    ["a.mjs", " * rm -rf 写在块注释里不算"],
    ["a.mjs", "safeRmSync(dir, { recursive: true });"],
    ["a.py", "os.remove(path)"],
    ["a.sh", 'rm -f "$file"'],
    ["a.sh", 'rm -f -v "$file"'],
    ["a.sh", "git clean -n"],
    ["a.sh", "git clean -n -d"],
    ["a.sh", "git worktree list"],
    ["a.ps1", "Invoke-RinneGit @('clean', '-n')"],
    ["a.sh", 'find "$d" -name x -print'],
    ["README.md", "rm -rf x"],
    ["scripts/build.ps1", `${rm} -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue`],
    ["a.ps1", `${rm} $d -Recurse -Force # rinne-delete-ok: 只删上面新建的目录，删前核对过真实路径`],
    ["a.ps1", `# rinne-delete-ok: 本进程新建的目录，前缀已断言\n${rm} $d -Recurse`],
    ["a.mjs", "// rinne-delete-ok: 仓内写死的输出目录，核对过真实路径\nfs.rmSync(out, { recursive: true });"],
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
    // 豁免要写理由、写在注释里、离得近、一条只盖一处
    const bad = (label, rel, text) =>
        out.push({ label: `豁免无效仍判红：${label}`, bad: true, passed: scanText(rel, text, rules).length > 0 });
    bad("没写理由", "a.mjs", "// rinne-delete-ok:\nfs.rmSync(d, { recursive: true });");
    bad("理由太短", "a.mjs", "// rinne-delete-ok: abcd\nfs.rmSync(d, { recursive: true });");
    bad("隔了一行", "a.mjs", "// rinne-delete-ok: 理由写得很清楚\n\nfs.rmSync(d, { recursive: true });");
    bad("标记在字符串里", "a.mjs", 'const s = "rinne-delete-ok: 理由写得很清楚"; fs.rmSync(d, { recursive: true });');
    bad(
        "一条只盖一处",
        "a.mjs",
        "// rinne-delete-ok: 理由写得很清楚\nfs.rmSync(a, { recursive: true });\nfs.rmSync(b, { recursive: true });",
    );
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
