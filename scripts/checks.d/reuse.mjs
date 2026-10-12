// SPDX-License-Identifier: GPL-3.0-or-later
//
// REUSE 合规：先做离线的结构检查（小节标记一个不少、兜底块在最前、LICENSES/ 里没有没人引用的全文），
// 再跑钉版的 reuse lint（RINNE_REUSE、git config rinne.reuse 或 PATH 上的 reuse）。
// 本机找不到 reuse 时 SKIP 并写明；CI 上找不到直接判红。

import fs from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { caseResult, inCI, localSetting } from "../lib/check-kit.mjs";

/** REUSE.toml 一次建好的全部小节；后续工作只往自己那一节里加块。 */
export const SECTIONS = [
    "base",
    "untagged",
    "juce-web-js",
    "juce-patches",
    "fonts",
    "third-party-notices",
    "third-party-src",
];

export function checkReuseToml(text, licenseFiles) {
    const errors = [];
    const marks = [...text.matchAll(/^# --- section:([a-z0-9-]+) ---$/gm)].map((m) => m[1]);
    if (marks.join() !== SECTIONS.join()) errors.push(`小节标记应依次为 ${SECTIONS.join("、")}`);
    const blocks = text.split(/^\[\[annotations\]\]\s*$/m).slice(1);
    const field = (b, k) => new RegExp(`^${k}\\s*=\\s*"([^"]*)"`, "m").exec(b)?.[1];
    const lic = (b) => field(b, "SPDX-License-Identifier");
    if (!blocks.length || field(blocks[0], "path") !== "**" || lic(blocks[0]) !== "GPL-3.0-or-later") {
        errors.push('第一个块必须是 path = "**" 的 GPL-3.0-or-later 兜底块');
    }
    const used = new Set(blocks.flatMap((b) => (lic(b) || "").split(/\s+(?:AND|OR|WITH)\s+|[()]/)));
    for (const f of licenseFiles) {
        if (!used.has(f.replace(/\.txt$/, ""))) errors.push(`LICENSES/${f} 没有被任何块引用`);
    }
    return errors;
}

const BASE = ["[[annotations]]", 'path = "**"', 'SPDX-License-Identifier = "GPL-3.0-or-later"'];
const GOOD = ["version = 1", ...SECTIONS.flatMap((s, i) => [`# --- section:${s} ---`, ...(i ? [] : BASE)])].join("\n");

export default {
    name: "reuse",
    group: "compliance",
    inputs: ["REUSE.toml", "LICENSES"],
    run(ctx) {
        const toml = fs.readFileSync(path.join(ctx.root, "REUSE.toml"), "utf8");
        const errors = checkReuseToml(toml, fs.readdirSync(path.join(ctx.root, "LICENSES")));
        if (errors.length) return { ok: false, lines: errors };
        const bin = localSetting("RINNE_REUSE", "rinne.reuse", { env: ctx.env, cwd: ctx.root }) || "reuse";
        const r = spawnSync(bin, ["--root", ctx.root, "lint"], { encoding: "utf8" });
        if (r.error) {
            if (inCI(ctx.env)) return { ok: false, lines: ["CI 上找不到 reuse"] };
            return { skip: "结构检查通过；本机找不到 reuse，lint 未跑（设置 RINNE_REUSE）" };
        }
        if (r.status === 0) return { ok: true };
        return { ok: false, lines: `${r.stdout}${r.stderr}`.trim().split("\n").slice(-25) };
    },
    selfTest() {
        const cases = [
            ["完整的小节与兜底块", false, checkReuseToml(GOOD, ["GPL-3.0-or-later.txt"])],
            ["缺一个小节", true, checkReuseToml(GOOD.replace("# --- section:fonts ---", ""), [])],
            ["兜底块不在最前", true, checkReuseToml(GOOD.replace('path = "**"', 'path = "src/**"'), [])],
            ["没人引用的许可全文", true, checkReuseToml(GOOD, ["GPL-3.0-or-later.txt", "MIT.txt"])],
        ].map(([label, bad, errors]) => ({ label, bad, passed: bad ? errors.length > 0 : errors.length === 0 }));
        return caseResult(cases);
    },
};
