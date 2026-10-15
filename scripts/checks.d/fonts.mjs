// SPDX-License-Identifier: GPL-3.0-or-later
//
// 字体门禁：字符覆盖（check-font-coverage.py）与名称、署名（check-font-names.py）。
// 两个脚本都要 fontTools 与 brotli。缺依赖一律判红（本机和 CI 相同）：缺依赖判绿是假绿。
// CI 的 docs-truth 任务按 requirements-tools.txt 的哈希装好这两个包，并用 RINNE_PYTHON 指给本检查。

import { spawnSync } from "node:child_process";
import path from "node:path";
import { ROOT } from "../lib/check-kit.mjs";

const SCRIPTS = ["check-font-coverage.py", "check-font-names.py"];

/** 找一个装了 fontTools 与 brotli 的 Python：环境变量 RINNE_PYTHON，其次 python3、python。 */
function findPython(env = process.env) {
    for (const cand of [env.RINNE_PYTHON, "python3", "python"].filter(Boolean)) {
        const r = spawnSync(cand, ["-I", "-c", "import fontTools, brotli"], { encoding: "utf8" });
        if (r.status === 0) return cand;
    }
    return null;
}

function run(python, script, args) {
    const r = spawnSync(python, ["-I", path.join(ROOT, "scripts", script), ...args], { cwd: ROOT, encoding: "utf8" });
    return { ok: r.status === 0, lines: `${r.stdout || ""}${r.stderr || ""}`.split(/\r?\n/).filter(Boolean) };
}

export default {
    name: "fonts",
    group: "docs",
    inputs: ["web/fonts/RinneSans.woff2"],
    run(ctx) {
        const python = findPython(ctx.env);
        if (!python) {
            const why = "没有装 fontTools 与 brotli 的 Python 3（pip install fonttools brotli），字体检查没有运行";
            return { ok: false, lines: [why] };
        }
        const results = SCRIPTS.map((s) => run(python, s, []));
        return { ok: results.every((r) => r.ok), lines: results.flatMap((r) => (r.ok ? [] : r.lines)) };
    },
    selfTest(ctx) {
        const python = findPython(ctx?.env);
        if (!python) {
            const why = "没有装 fontTools 与 brotli 的 Python 3（pip install fonttools brotli），字体自测没有运行";
            return { ok: false, bad: 1, good: 1, lines: [why] };
        }
        const results = SCRIPTS.map((s) => run(python, s, ["--self-test"]));
        return {
            ok: results.every((r) => r.ok),
            bad: 1,
            good: 1,
            lines: results.flatMap((r) => (r.ok ? [] : r.lines)),
        };
    },
};
