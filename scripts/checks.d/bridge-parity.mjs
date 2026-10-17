// SPDX-License-Identifier: GPL-3.0-or-later
//
// 桥契约四方对拍，判据在 check-bridge-parity.mjs：UI_BRIDGE.md、两个 BridgeApi.h、bridge.js 名表、web-preview 的 mock。

import { checkTree, selfTest } from "../check-bridge-parity.mjs";

export default {
    name: "bridge-parity",
    group: "docs",
    inputs: [
        "docs/UI_BRIDGE.md",
        "src/input/InputBridgeApi.h",
        "src/output/OutputBridgeApi.h",
        "web-preview/mock/contract.js",
    ],
    async run(ctx) {
        const problems = await checkTree(ctx.root);
        return { ok: problems.length === 0, lines: problems };
    },
    selfTest: (ctx) => selfTest(ctx.root),
};
