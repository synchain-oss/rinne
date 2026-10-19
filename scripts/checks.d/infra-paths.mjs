// SPDX-License-Identifier: GPL-3.0-or-later
//
// 基础设施清单（lib/infra-paths.mjs）的锚点：清单必须覆盖它自己和合并判据的每一类实现，又不能大到把普通
// 文档与源码也算进去；CLAUDE.md 必须指向这份清单，而不是另抄一份。评审 bot 能读取并重新配置自己的文件
// （.pr_agent.toml、AGENTS.md）一律不许入库，除非在 ALLOWED_BOT_CONFIG 里逐条登记豁免。

import fs from "node:fs";
import path from "node:path";
import { caseResult, trackedFiles } from "../lib/check-kit.mjs";
import {
    ALLOWED_BOT_CONFIG,
    BOT_CONFIG_PATHS,
    INFRA_PATHS,
    botConfigErrors,
    infraMatcher,
} from "../lib/infra-paths.mjs";

const SELF = "scripts/lib/infra-paths.mjs";
/** clang-format 这个 required check 直接读取的格式配置。 */
const FORMAT_CONFIGS = [".clang-format", ".prettierrc.json", ".prettierignore"];
/** 必须是基础设施的路径：每一类各取一个代表。 */
export const ANCHORS = [
    SELF,
    ".github/workflows/compliance.yml",
    ".github/rulesets/feature-lines.json",
    ".github/CODEOWNERS",
    "scripts/maintainer/pr-merge.ps1",
    "scripts/hooks/pre-push",
    "scripts/gates.ps1",
    "scripts/gates.d/10-static.ps1",
    "scripts/bootstrap-tools.ps1",
    "scripts/lib/privacy-needles.mjs",
    "scripts/lib/check-kit.mjs",
    "scripts/checks.d/workflows.mjs",
    "scripts/run-checks.mjs",
    "scripts/check-commits.mjs",
    "scripts/check-privacy.mjs",
    "scripts/check-ctest-timeouts.ps1",
    "scripts/check-font-names.py",
    "scripts/review-bot.mjs",
    "scripts/lock-actions.mjs",
    "scripts/sync-labels.mjs",
    "scripts/build.ps1",
    "scripts/package.ps1",
    "scripts/package-macos.sh",
    "scripts/stress.ps1",
    "scripts/ci/mac/build.sh",
    ".github/actions/setup-juce/action.yml",
    ".clang-format",
    ".prettierrc.json",
    ".prettierignore",
    ".editorconfig",
    ".gitattributes",
    ".gitleaks.toml",
    "requirements-tools.txt",
    ".node-version",
    ".gitleaks-version",
    "REUSE.toml",
    "CLAUDE.md",
    // 评审 bot 能从默认分支读取、据此重新配置自己的文件
    ".pr_agent.toml",
    "sub/.pr_agent.toml",
    "AGENTS.md",
    "sub/AGENTS.md",
    // 只差大小写的写法：在不区分大小写的文件系统上检出后就是上面的文件
    "Scripts/hooks/pre-push.mjs",
    ".GITHUB/workflows/x.yml",
    "CLAUDE.MD",
    "SCRIPTS/LIB/infra-paths.mjs",
];
/** 不该算基础设施的路径：清单写得太宽（例如一条 **）时这里会变红。 */
export const NOT_INFRA = [
    "README.md",
    "THIRD-PARTY-NOTICES.md",
    "docs/adr/0001-two-binaries.md",
    "src/core/engine/InputEngine.cpp",
    "web/shared/base.css",
    "tests/core/test_ring.cpp",
];

/** isInfra 是判定函数，claude 是 CLAUDE.md 全文。返回错误列表。 */
export function checkInfra({ isInfra, claude, botFiles = [], allowedBotConfig = ALLOWED_BOT_CONFIG }) {
    const errors = [];
    for (const p of ANCHORS) if (!isInfra(p)) errors.push(`基础设施清单没有覆盖 ${p}`);
    for (const p of NOT_INFRA) if (isInfra(p)) errors.push(`基础设施清单把 ${p} 也算了进去，写得太宽`);
    if (!claude.includes(SELF)) errors.push(`CLAUDE.md 没有指向 ${SELF}`);
    errors.push(...botConfigErrors(botFiles, allowedBotConfig));
    return errors;
}

export function selfTest() {
    const claude = `改到基础设施路径的 PR 不自合，唯一真源是 \`${SELF}\`。`;
    const drop = (g) => infraMatcher(INFRA_PATHS.filter((x) => x !== g));
    const dropAll = (gs) => infraMatcher(INFRA_PATHS.filter((x) => !gs.includes(x)));
    const swap = (from, to) => infraMatcher(INFRA_PATHS.map((x) => (x === from ? to : x)));
    const base = { isInfra: infraMatcher(INFRA_PATHS), claude };
    const cases = [
        ["当前清单", false, base],
        ["清单不含 scripts/lib/**（连它自己也不在）", true, { isInfra: drop("scripts/lib/**"), claude }],
        ["清单不含检查脚本目录", true, { isInfra: drop("scripts/checks.d/**"), claude }],
        ["清单不含评审 bot", true, { isInfra: drop("scripts/review-bot.mjs"), claude }],
        [
            "清单不含工具引导脚本（sha256 清单与 hooks 预检）",
            true,
            { isInfra: drop("scripts/bootstrap-tools.ps1"), claude },
        ],
        [
            "检查脚本只认 .mjs，漏掉 PowerShell 与 Python 写的",
            true,
            { isInfra: swap("scripts/check-*", "scripts/check-*.mjs"), claude },
        ],
        ["清单不含工具钉版文件", true, { isInfra: drop(".*-version"), claude }],
        ["清单不含合并时运行的构建脚本", true, { isInfra: drop("scripts/build.ps1"), claude }],
        ["清单不含打包脚本", true, { isInfra: drop("scripts/package*"), claude }],
        ["清单不含压测脚本", true, { isInfra: drop("scripts/stress.ps1"), claude }],
        ["清单不含 CI 调用的脚本", true, { isInfra: drop("scripts/ci/**"), claude }],
        ["清单不含格式配置", true, { isInfra: dropAll(FORMAT_CONFIGS), claude }],
        ["清单不含 .gitattributes", true, { isInfra: drop(".gitattributes"), claude }],
        [
            "匹配区分大小写（Scripts/hooks/ 漏掉）",
            true,
            { isInfra: infraMatcher(INFRA_PATHS, { ignoreCase: false }), claude },
        ],
        ["清单写成一条 **", true, { isInfra: infraMatcher(["**"]), claude }],
        ["CLAUDE.md 不指向清单", true, { isInfra: infraMatcher(INFRA_PATHS), claude: "不自合的路径见下表。" }],
        ["仓里没有评审 bot 的配置文件", false, { ...base, botFiles: ["src/core/engine/InputEngine.cpp"] }],
        [".pr_agent.toml 入库且没有豁免", true, { ...base, botFiles: [".pr_agent.toml"] }],
        ["子目录里的 .pr_agent.toml 同样不许入库", true, { ...base, botFiles: ["sub/.pr_agent.toml"] }],
        ["AGENTS.md 入库且没有豁免", true, { ...base, botFiles: ["AGENTS.md"] }],
        ["子目录里的 AGENTS.md 同样不许入库", true, { ...base, botFiles: ["sub/AGENTS.md"] }],
        [
            ".pr_agent.toml 已登记豁免",
            false,
            { ...base, botFiles: [".pr_agent.toml"], allowedBotConfig: [".pr_agent.toml"] },
        ],
        [
            "豁免只对登记的那一份有效",
            true,
            { ...base, botFiles: ["sub/.pr_agent.toml"], allowedBotConfig: [".pr_agent.toml"] },
        ],
    ].map(([label, bad, args]) => {
        const errors = checkInfra(args);
        return { label, bad, passed: bad ? errors.length > 0 : errors.length === 0 };
    });
    return caseResult(cases);
}

export default {
    name: "infra-paths",
    group: "docs",
    inputs: [],
    run(ctx) {
        const abs = path.join(ctx.root, "CLAUDE.md");
        if (!fs.existsSync(abs)) return { ok: false, lines: ["缺 CLAUDE.md"] };
        const errors = checkInfra({
            isInfra: infraMatcher(INFRA_PATHS),
            claude: fs.readFileSync(abs, "utf8"),
            botFiles: trackedFiles(ctx.root),
        });
        return { ok: errors.length === 0, lines: errors };
    },
    selfTest,
};
