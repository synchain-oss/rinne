// SPDX-License-Identifier: GPL-3.0-or-later
//
// 字体门禁：字符覆盖（check-font-coverage.py）与名称、署名（check-font-names.py）。
// 两个脚本都要 fontTools 与 brotli。缺依赖一律判红（本机和 CI 相同）：缺依赖判绿是假绿。
// Python 依次取 RINNE_PYTHON、git config rinne.python（bootstrap-tools.ps1 按 requirements-tools.txt 的哈希建的 venv；
// gates 把它设进 RINNE_PYTHON），都没配置时才退回 PATH 上的 python3、python。配置了却用不了直接判红并写明原因，
// 不悄悄换成别的 Python。CI 的 docs-truth 任务按哈希装好这两个包，并用 RINNE_PYTHON 指给本检查。

import { spawnSync } from "node:child_process";
import path from "node:path";
import { ROOT, localSetting } from "../lib/check-kit.mjs";

const SCRIPTS = ["check-font-coverage.py", "check-font-names.py"];
const FIX = "用 pwsh scripts/bootstrap-tools.ps1 -Tools pytools 建 venv，再按它打印的命令设 git config rinne.python";

/**
 * 找一个能跑 probe 的 Python（本检查与 text-fit 共用）：配置了 RINNE_PYTHON 或 git config rinne.python 就只试它，
 * 都没配置才试 PATH 上的 python3、python。probe 以退出码 3 表示「版本太旧」。返回 { python } 或 { why }；
 * why 只写来源与原因，不写配置的路径。
 */
export function locatePython(env, probe, { need, check, tooOld = "版本太旧", fix = "" }) {
    const configured = localSetting("RINNE_PYTHON", "rinne.python", { env, cwd: ROOT });
    const source = env.RINNE_PYTHON ? "RINNE_PYTHON" : "git config rinne.python";
    const tried = [];
    for (const cand of configured ? [configured] : ["python3", "python"]) {
        const r = spawnSync(cand, ["-I", "-c", probe], { encoding: "utf8" });
        if (r.status === 0) return { python: cand };
        const last = `${r.stderr || ""}${r.stdout || ""}`.trim().split(/\r?\n/).pop();
        const err = r.error ? "找不到这个程序" : r.status === 3 ? tooOld : last || `退出码 ${r.status}`;
        tried.push(`${configured ? `${source} 指定的 Python` : cand}：${err}`);
    }
    return { why: `没有${need}（${tried.join("；")}），${check}没有运行${fix ? `；${fix}` : ""}` };
}

export const findPython = (env = process.env) =>
    locatePython(env, "import fontTools, brotli", {
        need: "装了 fontTools 与 brotli 的 Python 3",
        check: "字体检查",
        fix: FIX,
    });

function run(python, script, args) {
    const r = spawnSync(python, ["-I", path.join(ROOT, "scripts", script), ...args], { cwd: ROOT, encoding: "utf8" });
    return { ok: r.status === 0, lines: `${r.stdout || ""}${r.stderr || ""}`.split(/\r?\n/).filter(Boolean) };
}

export default {
    name: "fonts",
    group: "docs",
    inputs: ["web/fonts/RinneSans.woff2"],
    run(ctx) {
        const { python, why } = findPython(ctx.env);
        if (!python) return { ok: false, lines: [why] };
        const results = SCRIPTS.map((s) => run(python, s, []));
        return { ok: results.every((r) => r.ok), lines: results.flatMap((r) => (r.ok ? [] : r.lines)) };
    },
    selfTest(ctx) {
        // 配置了却找不到：判红、说出来源，不退回 PATH 上的 Python
        const missing = findPython({ ...(ctx?.env ?? process.env), RINNE_PYTHON: "rinne-no-such-python" });
        const finder = !missing.python && /RINNE_PYTHON 指定的 Python：找不到这个程序/.test(missing.why || "");
        const { python, why } = findPython(ctx?.env);
        if (!python) return { ok: false, bad: 1, good: 1, lines: [why] };
        const results = SCRIPTS.map((s) => run(python, s, ["--self-test"]));
        return {
            ok: finder && results.every((r) => r.ok),
            bad: 2,
            good: 1,
            lines: [
                ...(finder ? [] : ["自测未通过：配置的 Python 找不到时应判红，不退回 PATH"]),
                ...results.flatMap((r) => (r.ok ? [] : r.lines)),
            ],
        };
    },
};
