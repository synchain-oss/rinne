// SPDX-License-Identifier: GPL-3.0-or-later
//
// 安全警告导语：真源是 docs/USER_GUIDE.zh-CN.md 的「使用前必读」导语段，英文译文在 docs/hard-rules.i18n.json。
// 落地面按语言各一份，用 GitHub 的 [!WARNING] 提示框写出；README 与英文指南接入时在文件里放上标记即可。

import { I18N_FILE, SOURCE_FILE, loadRuleSet, sourceProblems, loadI18n, loadSource } from "../lib/docs-kit.mjs";

export default {
    name: "safety-warning",
    sources: [SOURCE_FILE, I18N_FILE],
    surfaces: [
        { file: "README.md", id: "en" },
        { file: "README.zh-CN.md", id: "zh" },
        { file: "docs/USER_GUIDE.md", id: "en" },
    ],
    validate: (ctx) => sourceProblems(loadSource(ctx.root), loadI18n(ctx.root)),
    render: (ctx, id) => `> [!WARNING]\n> ${loadRuleSet(ctx.root).lead[id]}`,
};
