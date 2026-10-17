// SPDX-License-Identifier: GPL-3.0-or-later
//
// 参考仓来源登记：upstream/*.json 的结构、登记文件的文件头、反向的漏登记。判据在 check-upstream.mjs。

import { checkTree, selfTest } from "../check-upstream.mjs";

export default {
    name: "upstream",
    group: "docs",
    inputs: [],
    run(ctx) {
        const errors = checkTree(ctx.root);
        return { ok: errors.length === 0, lines: errors };
    },
    selfTest: () => selfTest(),
};
