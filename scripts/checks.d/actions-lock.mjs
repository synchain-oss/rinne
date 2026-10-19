// SPDX-License-Identifier: GPL-3.0-or-later
//
// 第三方 action 的离线锁检查（docs-truth 的一部分）。.github/actions-lock.json 由 scripts/lock-actions.mjs
// 联网生成，记录每个钉死的远程 action 与它 action.yml 里的嵌套 uses。这里不联网，只核对：
//   - workflow 里每个远程 uses 都在锁里，行尾注明的版本与锁一致；
//   - 锁里每个嵌套 uses 都钉 40 位 SHA、在 actions-allowlist.json 里，而且自己也有锁条目
//     （例如 claude-code-action 在 action.yml 里再 uses 一个 setup-bun：缺了它，job 会在 Set up 阶段被拒）；
//   - 锁里没有任何地方都不引用的条目。

import fs from "node:fs";
import path from "node:path";
import { caseResult } from "../lib/check-kit.mjs";
import { allowed, readJson, remoteUses, workflowFiles } from "./workflows.mjs";

/** uses：[{ ref, version, where }]；lock：{ actions: { ref: { version, using, nested: [ref…] } } }。 */
export function checkLock(uses, lock, allowlist) {
    const errors = [];
    const entries = lock.actions || {};
    const seen = new Set();
    for (const u of uses) {
        const e = entries[u.ref];
        if (!e) errors.push(`${u.where} ${u.ref} 不在 actions-lock.json 里（跑 node scripts/lock-actions.mjs）`);
        else if (u.version && e.version !== u.version)
            errors.push(`${u.where} 注明的版本 ${u.version} 与锁里的 ${e.version} 不一致`);
        seen.add(u.ref);
    }
    // 从 workflow 直接引用的条目出发，沿嵌套 uses 走一遍
    const queue = [...seen];
    while (queue.length) {
        const ref = queue.shift();
        for (const n of entries[ref]?.nested || []) {
            const at = n.lastIndexOf("@");
            const target = n.slice(0, at < 0 ? n.length : at);
            if (at < 0 || !/^[0-9a-f]{40}$/.test(n.slice(at + 1))) errors.push(`${ref} 嵌套的 ${n} 没有钉 40 位 SHA`);
            if (!allowed(target, allowlist)) errors.push(`${ref} 嵌套的 ${target} 不在 actions-allowlist.json 里`);
            if (!entries[n]) {
                errors.push(`${ref} 嵌套的 ${n} 在锁里没有自己的条目`);
            } else if (!seen.has(n)) {
                seen.add(n);
                queue.push(n);
            }
        }
    }
    for (const ref of Object.keys(entries)) if (!seen.has(ref)) errors.push(`锁里的 ${ref} 没有任何地方引用`);
    return errors;
}

const SHA = (c) => c.repeat(40);
const ACTION = `anthropics/claude-code-action@${SHA("a")}`;
const BUN = `oven-sh/setup-bun@${SHA("b")}`;
const ALLOW = {
    github_owned_allowed: true,
    patterns_allowed: ["anthropics/claude-code-action@*", "oven-sh/setup-bun@*"],
};
const LOCK = {
    actions: {
        [ACTION]: { version: "v1.0.0", using: "composite", nested: [BUN] },
        [BUN]: { version: "", using: "node20", nested: [] },
    },
};

export default {
    name: "actions-lock",
    group: "docs",
    inputs: [],
    run(ctx) {
        const uses = workflowFiles(ctx.root).flatMap((f) =>
            remoteUses(fs.readFileSync(path.join(ctx.root, f), "utf8")).map((u) => ({ ...u, where: `${f}:${u.no}` })),
        );
        const lock = readJson(ctx.root, ".github/actions-lock.json");
        const errors = checkLock(uses, lock, readJson(ctx.root, ".github/actions-allowlist.json"));
        return { ok: errors.length === 0, lines: errors.length ? errors : [`${uses.length} 处远程 uses 全部在锁里`] };
    },
    selfTest() {
        const use = [{ ref: ACTION, version: "v1.0.0", where: "f.yml:1" }];
        const without = (ref) => ({
            actions: Object.fromEntries(Object.entries(LOCK.actions).filter(([k]) => k !== ref)),
        });
        const nested = (n) => ({ actions: { ...LOCK.actions, [ACTION]: { ...LOCK.actions[ACTION], nested: [n] } } });
        const cases = [
            ["锁齐全", false, checkLock(use, LOCK, ALLOW)],
            ["workflow 引用了锁里没有的 action", true, checkLock(use, without(ACTION), ALLOW)],
            ["缺嵌套的 setup-bun 条目", true, checkLock(use, without(BUN), ALLOW)],
            ["嵌套 uses 没钉 SHA", true, checkLock(use, nested("oven-sh/setup-bun@v2"), ALLOW)],
            ["嵌套 uses 不在白名单", true, checkLock(use, nested(`softprops/action-gh-release@${SHA("c")}`), ALLOW)],
            ["版本注释与锁不一致", true, checkLock([{ ...use[0], version: "v2.0.0" }], LOCK, ALLOW)],
            ["锁里有没人引用的条目", true, checkLock([], LOCK, ALLOW)],
        ].map(([label, bad, errors]) => ({ label, bad, passed: bad ? errors.length > 0 : errors.length === 0 }));
        return caseResult(cases);
    },
};
