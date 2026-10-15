// SPDX-License-Identifier: GPL-3.0-or-later
//
// 设计盒门禁，判据在 check-design-box.mjs：设计盒定稿值、生成头文件的对拍、缩放状态机的行为。

import { checkTree, selfTest } from "../check-design-box.mjs";

export default {
    name: "design-box",
    group: "docs",
    inputs: ["web/shared/design-box.js"],
    async run() {
        const problems = await checkTree();
        return { ok: problems.length === 0, lines: problems };
    },
    selfTest: () => selfTest(),
};
