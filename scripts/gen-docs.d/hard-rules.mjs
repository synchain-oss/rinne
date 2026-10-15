// SPDX-License-Identifier: GPL-3.0-or-later
//
// 硬约束：真源是 docs/USER_GUIDE.zh-CN.md 的「使用前必读」编号列表（条数从真源读），en / fr 译文与每条的 zhSha256、
// installTxt 标记在 docs/hard-rules.i18n.json。这个块产出文档里的规则列表；INSTALL.txt 由 install-txt 块用同一份数据渲染。
// 中文改了而译文没跟上时，validate 报「译文过期」。

import {
    I18N_FILE,
    SOURCE_FILE,
    loadI18n,
    loadRuleSet,
    loadSource,
    ruleItem,
    sourceProblems,
} from "../lib/docs-kit.mjs";

export default {
    name: "hard-rules",
    sources: [SOURCE_FILE, I18N_FILE],
    surfaces: [
        { file: "README.md", id: "en" },
        { file: "README.zh-CN.md", id: "zh" },
        { file: "docs/USER_GUIDE.md", id: "en" },
    ],
    validate: (ctx) => sourceProblems(loadSource(ctx.root), loadI18n(ctx.root)),
    render: (ctx, id) =>
        loadRuleSet(ctx.root)
            .rules.map((r, i) => ruleItem(r, id, i + 1))
            .join("\n"),
};
