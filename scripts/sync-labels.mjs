// SPDX-License-Identifier: GPL-3.0-or-later
//
// 把 GitHub 上的标签同步成 .github/labels.json（唯一真源，格式与引用由 scripts/checks.d/labels.mjs 核对）。
// 以维护者身份调 gh：token 取自 `gh auth token --user DLsnows`，先核对它确实解析为 DLsnows。
//   node scripts/sync-labels.mjs --dry-run [--repo synchain-oss/rinne]
//        只读：列出要新建的、要改的（颜色、说明，或只差大小写的改名）、真源里没有的标签
//   node scripts/sync-labels.mjs --apply [--prune] [--repo synchain-oss/rinne]
//        执行新建与修改，再回读核对一遍；--prune 才删除真源里没有的标签（删除会把它从所有 issue / PR 上摘掉）
// 名字按大小写不敏感匹配，与 GitHub 的口径一致。

import fs from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { ROOT, assertNodeVersion, isMain } from "./lib/check-kit.mjs";
import { planLabels, validateLabels } from "./checks.d/labels.mjs";

const LOGIN = "DLsnows";
const REPO = "synchain-oss/rinne";

function maintainerToken() {
    const r = spawnSync("gh", ["auth", "token", "--user", LOGIN], { encoding: "utf8" });
    const token = (r.stdout || "").trim();
    if (r.status !== 0 || !token) throw new Error(`取不到 ${LOGIN} 的 gh 凭据（先 gh auth login）`);
    return token;
}

function gh(args, token) {
    const r = spawnSync("gh", args, {
        encoding: "utf8",
        env: { ...process.env, GH_TOKEN: token },
        maxBuffer: 64 * 1024 * 1024,
    });
    if (r.status !== 0)
        throw new Error(`gh ${args.slice(0, 3).join(" ")} 失败：${(r.stderr || "").trim().split("\n")[0]}`);
    return r.stdout;
}

const current = (repo, token) =>
    gh(["api", "--paginate", `repos/${repo}/labels?per_page=100`, "--jq", ".[] | @json"], token)
        .split("\n")
        .filter((l) => l.trim())
        .map((l) => JSON.parse(l))
        .map(({ name, color, description }) => ({ name, color, description: description || "" }));

const show = (l) => `${l.name}（#${l.color}）${l.description ? ` ${l.description}` : ""}`;

function printPlan(plan, prune) {
    for (const l of plan.create) console.log(`新建  ${show(l)}`);
    for (const { from, to } of plan.update) {
        const what = [
            from.name !== to.name && `改名 ${from.name} → ${to.name}`,
            from.color.toLowerCase() !== to.color && `颜色 #${from.color} → #${to.color}`,
            (from.description || "") !== to.description && `说明「${from.description}」→「${to.description}」`,
        ].filter(Boolean);
        console.log(`修改  ${from.name}：${what.join("；")}`);
    }
    for (const l of plan.extra)
        console.log(`${prune ? "删除" : "多余"}  ${show(l)}${prune ? "" : "（加 --prune 才删）"}`);
    if (!plan.create.length && !plan.update.length && !plan.extra.length) console.log("已与 .github/labels.json 一致");
}

function main(argv) {
    assertNodeVersion();
    const has = (k) => argv.includes(k);
    const repo = has("--repo") ? argv[argv.indexOf("--repo") + 1] : REPO;
    const apply = has("--apply");
    if (apply === has("--dry-run")) throw new Error("用法：--dry-run | --apply [--prune]，二选一");
    if (has("--prune") && !apply) throw new Error("--prune 只能和 --apply 一起用");
    if (!/^[\w.-]+\/[\w.-]+$/.test(repo || "")) throw new Error("--repo 要写成 owner/name");
    const json = JSON.parse(fs.readFileSync(path.join(ROOT, ".github", "labels.json"), "utf8"));
    const errors = validateLabels(json);
    if (errors.length) throw new Error(`.github/labels.json 有问题：\n${errors.join("\n")}`);
    const token = maintainerToken();
    const login = gh(["api", "user", "--jq", ".login"], token).trim();
    if (login !== LOGIN) throw new Error(`gh 凭据解析为 ${login}，不是 ${LOGIN}`);
    const plan = planLabels(json.labels, current(repo, token));
    console.log(`${repo}：真源 ${json.labels.length} 个标签${apply ? "" : "（dry-run，不改任何东西）"}`);
    printPlan(plan, has("--prune"));
    if (!apply) return 0;
    const field = (k, v) => ["-f", `${k}=${v}`];
    for (const l of plan.create) {
        const f = [...field("name", l.name), ...field("color", l.color), ...field("description", l.description)];
        gh(["api", "-X", "POST", `repos/${repo}/labels`, ...f, "--silent"], token);
    }
    for (const { from, to } of plan.update) {
        const f = [...field("new_name", to.name), ...field("color", to.color), ...field("description", to.description)];
        gh(["api", "-X", "PATCH", `repos/${repo}/labels/${encodeURIComponent(from.name)}`, ...f, "--silent"], token);
    }
    if (has("--prune")) {
        for (const l of plan.extra) {
            gh(["api", "-X", "DELETE", `repos/${repo}/labels/${encodeURIComponent(l.name)}`, "--silent"], token);
        }
    }
    // 回读：新建与修改都要落地；--prune 时多余的也要没了
    const after = planLabels(json.labels, current(repo, token));
    const left = after.create.length + after.update.length + (has("--prune") ? after.extra.length : 0);
    if (left) {
        console.error("回读后仍不一致：");
        printPlan(after, has("--prune"));
        return 1;
    }
    console.log(
        `回读一致：${json.labels.length} 个标签${after.extra.length ? `，另有 ${after.extra.length} 个真源外的标签未删` : ""}`,
    );
    return 0;
}

if (isMain(import.meta.url)) {
    try {
        process.exitCode = main(process.argv.slice(2));
    } catch (e) {
        console.error(`sync-labels：${e.message}`);
        process.exitCode = 1;
    }
}
