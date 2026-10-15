// SPDX-License-Identifier: GPL-3.0-or-later
//
// HTML 注入口门禁，判据在 check-no-html-sinks.mjs。

import { checkTree, selfTest } from "../check-no-html-sinks.mjs";

export default {
    name: "html-sinks",
    group: "docs",
    inputs: ["web"],
    run() {
        const r = checkTree();
        return { ok: r.problems.length === 0, lines: r.problems };
    },
    selfTest: () => selfTest(),
};
