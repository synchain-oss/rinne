// SPDX-License-Identifier: GPL-3.0-or-later
//
// 静态检查注册表的运行器。每项检查是 scripts/checks.d/<name>.mjs，默认导出
//   { name, group, inputs, run(ctx), selfTest(ctx), mutationCheck?(ctx) }
// 文件入库即生效。inputs 只放「还没建立就不该检查」的可选输入，缺失时该项 SKIP 并写明原因；
// 必须存在的文件由 run() 自己判红。后续检查只新增文件，不改本运行器。
// run 返回 { ok, skip?, lines? }；selfTest 另须返回 bad、good 两个计数，坏样例与正例都不能为 0。
//   node scripts/run-checks.mjs [--group <组>] [--only <名>] [--needles local|ci|ci-optional]
//                               [--self-test | --mutation-check | --list]
// --needles 是个人针来源，原样交给需要它的检查（见 privacy-needles.mjs），默认 local。

import fs from "node:fs";
import path from "node:path";
import { pathToFileURL } from "node:url";
import { ROOT, assertNodeVersion } from "./lib/check-kit.mjs";
import { needlesMode } from "./lib/privacy-needles.mjs";

export const GROUPS = ["compliance", "docs", "pr", "release", "links"];
const DIR = path.join(ROOT, "scripts", "checks.d");

/** 读出全部注册项；形状不对的不会被悄悄跳过，而是作为一条 FAIL 留在结果里。 */
async function load() {
    const out = [];
    for (const file of fs
        .readdirSync(DIR)
        .filter((f) => f.endsWith(".mjs"))
        .sort()) {
        const name = file.slice(0, -4);
        try {
            const m = (await import(pathToFileURL(path.join(DIR, file)).href)).default;
            const bad = !m
                ? "没有默认导出"
                : m.name !== name
                  ? `name 应为 ${name}`
                  : !GROUPS.includes(m.group)
                    ? `group 应为 ${GROUPS.join(" / ")} 之一`
                    : !Array.isArray(m.inputs) || typeof m.run !== "function" || typeof m.selfTest !== "function"
                      ? "缺 inputs / run / selfTest"
                      : null;
            out.push(bad ? { name, broken: bad } : m);
        } catch (e) {
            out.push({ name, broken: `加载失败：${e.message}` });
        }
    }
    return out;
}

async function runOne(check, mode, ctx) {
    if (check.broken) return { status: "FAIL", lines: [check.broken] };
    try {
        if (mode === "self-test") {
            const r = await check.selfTest(ctx);
            const both = r.bad > 0 && r.good > 0;
            const lines = [...(r.lines || [])];
            if (!both) lines.push(`自测须同时有坏样例与正例（坏 ${r.bad ?? 0}、正 ${r.good ?? 0}）`);
            return { status: r.ok && both ? "PASS" : "FAIL", lines };
        }
        if (mode === "mutation-check") {
            const r = await check.mutationCheck(ctx);
            return { status: r.ok ? "PASS" : "FAIL", lines: r.lines || [] };
        }
        const missing = check.inputs.filter((p) => !fs.existsSync(path.join(ctx.root, p)));
        if (missing.length) return { status: "SKIP", lines: [`缺 ${missing.join("、")}`] };
        const r = await check.run(ctx);
        if (r.skip) return { status: "SKIP", lines: [r.skip] };
        return { status: r.ok ? "PASS" : "FAIL", lines: r.lines || [] };
    } catch (e) {
        return { status: "FAIL", lines: [e.message] };
    }
}

async function main(argv) {
    assertNodeVersion();
    const opt = (k) => (argv.includes(k) ? argv[argv.indexOf(k) + 1] : undefined);
    const group = opt("--group");
    const only = opt("--only");
    if (group && !GROUPS.includes(group)) throw new Error(`未知的组 ${group}（${GROUPS.join(" / ")}）`);
    const mode = ["self-test", "mutation-check"].find((m) => argv.includes(`--${m}`)) || "run";
    let checks = (await load()).filter((c) => (!group || c.group === group || c.broken) && (!only || c.name === only));
    if (only && !checks.length) throw new Error(`没有名为 ${only} 的检查`);
    if (mode === "mutation-check") checks = checks.filter((c) => c.broken || c.mutationCheck);
    if (argv.includes("--list")) {
        for (const c of checks) console.log(`${c.name}\t${c.group ?? "?"}\t${(c.inputs || []).join(" ")}`);
        return 0;
    }
    const ctx = { root: ROOT, env: process.env, needles: needlesMode(argv) };
    const count = { PASS: 0, FAIL: 0, SKIP: 0 };
    for (const c of checks) {
        const r = await runOne(c, mode, ctx);
        count[r.status]++;
        console.log(`${r.status}  ${c.name}${c.group ? `（${c.group}）` : ""}`);
        for (const l of r.lines) console.log(`      ${l}`);
    }
    console.log(`run-checks ${mode}：PASS ${count.PASS} / FAIL ${count.FAIL} / SKIP ${count.SKIP}`);
    return count.FAIL ? 1 : 0;
}

main(process.argv.slice(2)).then(
    (code) => (process.exitCode = code),
    (e) => {
        console.error(`run-checks：${e.message}`);
        process.exitCode = 1;
    },
);
