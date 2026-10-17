// SPDX-License-Identifier: GPL-3.0-or-later
//
// 静态导出的输出目录守卫（Node）：build-static 导出前会整个删掉输出目录重建，
// 所以 --out 指向仓库根、上级目录、真源 web/ 或 web-preview/ 本身时必须拒绝，并且什么都不能被删。
//
// 用法：node web-preview/tests/smoke-embedded-resources-outdir.mjs [仓库根]

import fs from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { createRun, repoRoot } from "./lib/harness.mjs";

const ROOT = repoRoot();
const run = createRun("静态导出输出目录守卫");
const script = path.join(ROOT, "web-preview", "build-static.mjs");
const marker = path.join(ROOT, "web", "shared", "bridge.js");

for (const out of [".", "..", "web", "web-preview", "build", path.parse(ROOT).root]) {
    const r = spawnSync(process.execPath, [script, "--out", out], { cwd: ROOT, encoding: "utf8" });
    run.check(r.status === 1, `--out ${out}：应当被拒绝（退出码 1），实得 ${r.status}`);
    run.check(fs.existsSync(marker), `--out ${out}：被拒之后真源文件仍在`);
}
const ok = path.join(ROOT, "build", `outdir-smoke-${process.pid}`);
const r = spawnSync(process.execPath, [script, "--out", ok], { cwd: ROOT, encoding: "utf8" });
run.check(r.status === 0 && fs.existsSync(path.join(ok, "index.html")), "build/ 之下的目录应当可以导出");
fs.rmSync(ok, { recursive: true, force: true });
run.finish();
