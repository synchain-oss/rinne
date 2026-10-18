// SPDX-License-Identifier: GPL-3.0-or-later
//
// 桥契约四方对拍，判据在 check-bridge-parity.mjs：UI_BRIDGE.md、两个 BridgeApi.h、bridge.js 名表、web-preview 的 mock。

import { checkTree, selfTest } from "../check-bridge-parity.mjs";

export default {
    name: "bridge-parity",
    group: "docs",
    // 四方文件都是必需的：缺任何一个由 run() 判红，不能放进 inputs 让运行器记成跳过
    inputs: [],
    async run(ctx) {
        const problems = await checkTree(ctx.root);
        return { ok: problems.length === 0, lines: problems };
    },
    selfTest: (ctx) => selfTest(ctx.root),
};
