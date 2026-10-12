// SPDX-License-Identifier: GPL-3.0-or-later
//
// 提交与 PR 卫生：判据在 check-commits.mjs。真正的检查要 PR 或推送的上下文，由 CI 与 hook 调它的子命令；
// 这里把它的自测接进注册表，run-checks --self-test 一条命令就能跑全。

import { selfTest } from "../check-commits.mjs";

export default {
    name: "commits",
    group: "pr",
    inputs: [],
    run: () => ({ skip: "要有 PR 或推送的上下文：由 check-commits 的子命令与 hook 运行" }),
    selfTest: () => selfTest(),
};
