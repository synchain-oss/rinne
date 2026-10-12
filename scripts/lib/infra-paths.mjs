// SPDX-License-Identifier: GPL-3.0-or-later
//
// 基础设施路径的唯一机读清单。改到这些路径的 PR 不自合，review 干净后交维护者批准；pr-merge 读这一份，
// CLAUDE.md 只写类别并指向这里。清单覆盖合并判据本身：CI 配置、评审 bot、检查脚本与它们共用的库、
// hooks、gates、合规配置与工具钉版。本文件在 scripts/lib/** 之内，所以改它也要维护者批准。
// glob 写法：** 跨目录，* 不跨 /，路径相对仓根。

export const INFRA_PATHS = [
    ".github/**",
    "scripts/maintainer/**",
    "scripts/hooks/**",
    "scripts/gates.ps1",
    "scripts/gates.d/**",
    "scripts/lib/**",
    "scripts/checks.d/**",
    "scripts/run-checks.mjs",
    "scripts/check-*.mjs",
    "scripts/review-bot.mjs",
    "scripts/lock-actions.mjs",
    "scripts/sync-labels.mjs",
    ".gitleaks.toml",
    "requirements-tools.txt",
    ".*-version",
    ".juce-commit",
    ".sccache-sha256",
    "REUSE.toml",
    "CLAUDE.md",
];

const globRe = (g) =>
    new RegExp(
        `^${g
            .replace(/[.+^${}()|[\]\\]/g, "\\$&")
            .replace(/\*\*/g, "\0")
            .replace(/\*/g, "[^/]*")
            .replace(/\0/g, ".*")}$`,
    );
/** 由一份 glob 清单得到判定函数：仓内相对路径（正斜杠）是否命中其中之一。 */
export function infraMatcher(globs) {
    const res = globs.map(globRe);
    return (p) => res.some((re) => re.test(p));
}

export const isInfraPath = infraMatcher(INFRA_PATHS);
