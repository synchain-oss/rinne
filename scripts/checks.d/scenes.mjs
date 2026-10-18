// SPDX-License-Identifier: GPL-3.0-or-later
//
// 场景集合对拍，判据在 check-scenes.mjs：场景表自身一致；设计稿入库后再与它的机读清单对拍。

import { checkTree, selfTest } from "../check-scenes.mjs";

export default {
    name: "scenes",
    group: "docs",
    // 场景表是必需的：缺失由 run() 判红，不能放进 inputs 让运行器记成跳过
    inputs: [],
    async run(ctx) {
        const { problems } = await checkTree(ctx.root);
        return { ok: problems.length === 0, lines: problems };
    },
    selfTest: () => selfTest(),
};
