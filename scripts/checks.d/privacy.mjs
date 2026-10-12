// SPDX-License-Identifier: GPL-3.0-or-later
//
// 隐私门禁：工作树全量扫描（通用规则 + 个人针）。自测与变异检查用规则表里的夹具。

import { formatFinding, scanTree, selfTest } from "../check-privacy.mjs";
import { loadPersonalNeedles, mutationCheck } from "../lib/privacy-needles.mjs";

export default {
    name: "privacy",
    group: "compliance",
    inputs: ["scripts/lib/privacy-needles.mjs"],
    run(ctx) {
        const { needles, notes } = loadPersonalNeedles({ env: ctx.env, cwd: ctx.root });
        const hits = scanTree(ctx.root, needles);
        return { ok: hits.length === 0, lines: [...notes, ...hits.map(formatFinding)] };
    },
    selfTest: () => selfTest(),
    mutationCheck() {
        const r = mutationCheck();
        return { ok: r.survived.length === 0, lines: [`${r.total} 个变异，存活 ${r.survived.length}`, ...r.survived] };
    },
};
