// SPDX-License-Identifier: GPL-3.0-or-later
//
// SPDX 头门禁，判据在 check-spdx.mjs。

import { checkTree, selfTest } from "../check-spdx.mjs";

export default {
    name: "spdx",
    group: "compliance",
    inputs: [],
    run(ctx) {
        const missing = checkTree(ctx.root);
        if (missing === null) return { ok: false, lines: ["没有找到任何需要 SPDX 头的源文件"] };
        return { ok: missing.length === 0, lines: missing.map((f) => `缺 SPDX 头：${f}`) };
    },
    selfTest: () => selfTest(),
};
