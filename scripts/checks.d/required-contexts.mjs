// SPDX-License-Identifier: GPL-3.0-or-later
//
// required checks 的离线核对，判据在 check-required-contexts.mjs（--live 的联网回读不在这里跑）。

import { loadRepo, selfTest, staticCheck } from "../check-required-contexts.mjs";

export default {
    name: "required-contexts",
    group: "docs",
    inputs: [],
    run(ctx) {
        const { errors, notes } = staticCheck(loadRepo(ctx.root));
        return { ok: errors.length === 0, lines: [...errors, ...notes] };
    },
    selfTest: () => selfTest(),
};
