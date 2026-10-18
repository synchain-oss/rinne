// SPDX-License-Identifier: GPL-3.0-or-later
//
// 基础设施路径的唯一机读清单。改到这些路径的 PR 不自合，review 干净后交维护者批准；pr-merge 读这一份，
// CLAUDE.md 只写类别并指向这里。清单覆盖合并判据本身：CI 配置与 CI 调用的脚本、评审 bot、检查脚本与它们共用的库、
// hooks、gates 与它装工具用的引导脚本、合并时运行的构建 / 压测 / 打包脚本、格式与规范化配置、合规配置与工具钉版。
// 本文件在 scripts/lib/** 之内，所以改它也要维护者批准。
// glob 写法：** 跨目录，* 不跨 /，路径相对仓根。匹配不区分大小写：Windows 与 mac 的文件系统不区分大小写，
// Scripts/hooks/x 检出后就是 scripts/hooks/x；只差大小写的跟踪路径另由 path-case 检查拒绝。

export const INFRA_PATHS = [
    ".github/**",
    "scripts/maintainer/**",
    "scripts/hooks/**",
    "scripts/gates.ps1",
    "scripts/gates.d/**",
    // 工具的 sha256 清单在这里；gates 的预检也用它的 -CheckHooks 核对 hooks
    "scripts/bootstrap-tools.ps1",
    "scripts/lib/**",
    "scripts/checks.d/**",
    "scripts/run-checks.mjs",
    // 检查脚本不只 .mjs：还有 PowerShell 与 Python 写的（ctest 上界、发布 tag、字体）
    "scripts/check-*",
    // ctest 的 TIMEOUT 门禁就是这个文件里的两条 add_test，Catch2 的钉版也在这里：删掉它们不该由自合的 PR 完成
    "tests/CMakeLists.txt",
    // sanitizer 金丝雀：run.sh 由 CI 调用，被改成恒过，core-sanitizers 的绿就不可信
    "tests/sanitizer-canary/**",
    "scripts/review-bot.mjs",
    "scripts/lock-actions.mjs",
    "scripts/sync-labels.mjs",
    // 合并前在集成树上构建、压测与打包时跑的是 PR 自己的这几个脚本
    "scripts/build.ps1",
    "scripts/package*",
    "scripts/stress.ps1",
    // CI workflow 调用的脚本等于 workflow 的一部分
    "scripts/ci/**",
    // 格式检查读的配置，以及决定文件怎样被检查、怎样规范化的仓库配置
    ".clang-format",
    ".prettierrc.json",
    ".prettierignore",
    ".editorconfig",
    ".gitattributes",
    ".gitleaks.toml",
    "requirements-tools.txt",
    ".*-version",
    ".juce-commit",
    ".sccache-sha256",
    "REUSE.toml",
    "CLAUDE.md",
];

const globRe = (g, flags) =>
    new RegExp(
        `^${g
            .replace(/[.+^${}()|[\]\\]/g, "\\$&")
            .replace(/\*\*/g, "\0")
            .replace(/\*/g, "[^/]*")
            .replace(/\0/g, ".*")}$`,
        flags,
    );
/** 由一份 glob 清单得到判定函数：仓内相对路径（正斜杠）是否命中其中之一。ignoreCase 只给自测关掉。 */
export function infraMatcher(globs, { ignoreCase = true } = {}) {
    const res = globs.map((g) => globRe(g, ignoreCase ? "i" : ""));
    return (p) => res.some((re) => re.test(p));
}

export const isInfraPath = infraMatcher(INFRA_PATHS);
