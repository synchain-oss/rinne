// SPDX-License-Identifier: GPL-3.0-or-later
//
// 生成 .github/actions-lock.json：workflow 里每个钉死的远程 action，加上它 action.yml 里的嵌套 uses，
// 一层层往下走到底。只读联网（gh api GET，需要 DLsnows 的 GH_TOKEN），升级 action 时在本机跑；
// CI 只做离线核对（scripts/checks.d/actions-lock.mjs）。
//   node scripts/lock-actions.mjs            重写锁文件
//   node scripts/lock-actions.mjs --check    只比较，不一致退出 1

import fs from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { ROOT, assertNodeVersion, isMain } from "./lib/check-kit.mjs";
import { remoteUses, workflowFiles } from "./checks.d/workflows.mjs";

const LOCK = path.join(ROOT, ".github", "actions-lock.json");

/** 取 ref（owner/repo[/路径]@sha）对应的 action.yml；指向 .yml 文件的是可复用 workflow，取它本身。 */
function fetchAction(ref) {
    const at = ref.lastIndexOf("@");
    const [owner, repo, ...sub] = ref.slice(0, at).split("/");
    const dir = sub.join("/");
    const files = /\.ya?ml$/.test(dir) ? [dir] : ["action.yml", "action.yaml"].map((f) => (dir ? `${dir}/${f}` : f));
    for (const f of files) {
        const args = ["api", `repos/${owner}/${repo}/contents/${f}?ref=${ref.slice(at + 1)}`, "--jq", ".content"];
        const r = spawnSync("gh", args, { encoding: "utf8" });
        if (r.status === 0) return Buffer.from(r.stdout, "base64").toString("utf8");
    }
    throw new Error(`取不到 ${ref} 的 action.yml`);
}

/** 运行方式与嵌套的远程 uses（版本取行尾注释）。 */
export function describe(text) {
    const uses = remoteUses(text);
    return {
        using: /^\s*using:\s*["']?([\w-]+)/m.exec(text)?.[1] || "",
        nested: uses.map((u) => u.ref),
        versions: Object.fromEntries(uses.map((u) => [u.ref, u.version])),
    };
}

export function buildLock(root = ROOT, fetch = fetchAction) {
    const version = {};
    for (const f of workflowFiles(root)) {
        for (const u of remoteUses(fs.readFileSync(path.join(root, f), "utf8"))) version[u.ref] ??= u.version;
    }
    const actions = {};
    const queue = Object.keys(version);
    while (queue.length) {
        const ref = queue.shift();
        if (actions[ref]) continue;
        const d = describe(fetch(ref));
        actions[ref] = { version: version[ref] || "", using: d.using, nested: d.nested };
        for (const n of d.nested) {
            version[n] ??= d.versions[n];
            queue.push(n);
        }
    }
    const sorted = Object.fromEntries(
        Object.keys(actions)
            .sort()
            .map((k) => [k, actions[k]]),
    );
    return pretty({ actions: sorted });
}

/** 与 prettier 对 JSON 的排版一致：两格缩进，放得进一行（120 列）的字符串数组写成一行。 */
export function pretty(value) {
    return `${JSON.stringify(value, null, 2).replace(
        /^( *)("[^"]+": )?\[\n((?: *"[^"\n]*",?\n)+) *\]/gm,
        (all, ind, key, items) => {
            const line = `${ind}${key || ""}[${items
                .trim()
                .split(/,?\n\s*/)
                .map((s) => s.replace(/,$/, ""))
                .join(", ")}]`;
            return line.length <= 120 ? line : all;
        },
    )}\n`;
}

function main(argv) {
    assertNodeVersion();
    const text = buildLock();
    if (argv.includes("--check")) {
        const same = fs.existsSync(LOCK) && fs.readFileSync(LOCK, "utf8") === text;
        console.log(
            `lock-actions --check：${same ? "锁与 workflow 一致" : "锁已过期，重跑 node scripts/lock-actions.mjs"}`,
        );
        return same ? 0 : 1;
    }
    fs.writeFileSync(LOCK, text);
    console.log(`lock-actions：已写入 ${Object.keys(JSON.parse(text).actions).length} 个条目`);
    return 0;
}

if (isMain(import.meta.url)) {
    try {
        process.exitCode = main(process.argv.slice(2));
    } catch (e) {
        console.error(`lock-actions：${e.message}`);
        process.exitCode = 1;
    }
}
