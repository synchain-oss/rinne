// SPDX-License-Identifier: GPL-3.0-or-later
//
// 两个 git hook 的自测（含经真实 sh 外壳的端到端用例）。hook 本身在本机 commit / push 时运行，
// 这里只把自测接进注册表。CI 上找不到 gitleaks 时 pre-push 的自测判红，不跳过。

import { selfTest as commitMsg } from "../hooks/commit-msg.mjs";
import { selfTest as prePush } from "../hooks/pre-push.mjs";

export default {
    name: "hooks",
    group: "compliance",
    inputs: [],
    run: () => ({ skip: "hook 在本机 git commit / git push 时运行" }),
    selfTest() {
        const [a, b] = [commitMsg(), prePush()];
        return { ok: a.ok && b.ok, bad: a.bad + b.bad, good: a.good + b.good, lines: [...a.lines, ...b.lines] };
    },
};
