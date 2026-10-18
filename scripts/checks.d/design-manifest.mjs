// SPDX-License-Identifier: GPL-3.0-or-later
//
// 设计稿回收的机检，判据在 check-design-manifest.mjs：无头浏览器逐场景检查覆盖、文案、颜色、安全红字与外链。
// 设计稿（docs/design/）没入库或没有浏览器时跳过。自测只跑纯分析部分，不开浏览器。

import { locateDesign } from "../check-scenes.mjs";
import { renderReport, runAgainstDesign, selfTest } from "../check-design-manifest.mjs";

export default {
    name: "design-manifest",
    group: "docs",
    // 场景表与浏览器库是必需的：缺失由 run() 判红；设计稿或浏览器缺失才跳过
    inputs: [],
    async run(ctx) {
        const file = locateDesign(ctx.root);
        if (!file) return { ok: true, skip: "设计稿还没有入库（docs/design/ 不存在）" };
        const r = await runAgainstDesign(ctx.root, file);
        if (r.skip) return { ok: true, skip: r.skip };
        const red = r.findings.filter((f) => f.severity === "red");
        return { ok: red.length === 0, lines: red.length ? renderReport(r.findings).split("\n") : [] };
    },
    selfTest: () => selfTest(),
};
