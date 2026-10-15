// SPDX-License-Identifier: GPL-3.0-or-later
//
// 界面词条门禁，判据在 check-i18n.mjs。

import { checkTree, selfTest } from "../check-i18n.mjs";

export default {
    name: "i18n",
    group: "docs",
    inputs: ["web/i18n"],
    async run() {
        const r = await checkTree(undefined, { strictUnused: true });
        return { ok: r.problems.length === 0, lines: r.problems };
    },
    selfTest: () => selfTest(),
};
