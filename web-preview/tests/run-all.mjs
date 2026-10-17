// SPDX-License-Identifier: GPL-3.0-or-later
//
// 跑 web-preview/tests 下全部 smoke-*.mjs：每个脚本一个子进程，用同一个 Node。
//   node web-preview/tests/run-all.mjs [仓库根] [--no-browser] [--only <子串>]
// --no-browser 跳过名字里带 -page 的页面级冒烟（它们要无头浏览器）。
// 子进程退出码 0 通过、2 跳过、其余失败。失败的脚本打印完整输出，通过和跳过只打印最后一行。
// 本机同时只能有一个浏览器冒烟：手动跑页面级冒烟前先确认没有别的 gates 在跑，gates 里由 50-web.ps1 持 gui 锁。
// 退出码：0 无失败（可以有跳过）；1 有失败。

import fs from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { repoRoot } from "./lib/harness.mjs";

const here = path.dirname(fileURLToPath(import.meta.url));
const root = repoRoot();
const argv = process.argv.slice(2);
const noBrowser = argv.includes("--no-browser");
const only = argv.includes("--only") ? argv[argv.indexOf("--only") + 1] : null;

const scripts = fs
    .readdirSync(here)
    .filter((f) => /^smoke-.*\.mjs$/.test(f))
    .filter((f) => !(noBrowser && f.includes("-page")))
    .filter((f) => !only || f.includes(only))
    .sort();

const count = { pass: 0, skip: 0, fail: 0 };
for (const f of scripts) {
    const t0 = Date.now();
    const r = spawnSync(process.execPath, [path.join(here, f), root], { encoding: "utf8", timeout: 300000 });
    const out = `${r.stdout || ""}${r.stderr || ""}`.trimEnd();
    const status = r.status === 0 ? "通过" : r.status === 2 ? "跳过" : "失败";
    count[r.status === 0 ? "pass" : r.status === 2 ? "skip" : "fail"]++;
    const last = out.split("\n").pop();
    console.log(`[${status}] ${f}  ${((Date.now() - t0) / 1000).toFixed(1)}s  ${status === "失败" ? "" : last}`);
    if (status === "失败") console.log(out.replace(/^/gm, "    "));
}
console.log(`run-all：通过 ${count.pass} / 跳过 ${count.skip} / 失败 ${count.fail}`);
process.exitCode = count.fail ? 1 : 0;
