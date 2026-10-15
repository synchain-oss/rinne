// SPDX-License-Identifier: GPL-3.0-or-later
//
// 文本宽度预算门禁：先在本进程里合并三语词条写到临时文件，再交给 check-text-fit.py（只用 Python 标准库）。
// 找不到 Python 时跳过并写明原因，不判绿也不判红；本机 gates 有 Python，CI 的 docs-truth 任务也有 python3。

import { spawnSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { ROOT } from "../lib/check-kit.mjs";
import { loadDict } from "../check-i18n.mjs";

const SCRIPT = path.join(ROOT, "scripts", "check-text-fit.py");

/** 找一个能跑的 Python：环境变量 RINNE_PYTHON，其次 python3、python。 */
function findPython(env = process.env) {
    for (const cand of [env.RINNE_PYTHON, "python3", "python"].filter(Boolean)) {
        const r = spawnSync(cand, ["-I", "-c", "import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)"], {
            encoding: "utf8",
        });
        if (r.status === 0) return cand;
    }
    return null;
}

function runPython(python, args) {
    const r = spawnSync(python, ["-I", SCRIPT, ...args], { cwd: ROOT, encoding: "utf8" });
    const lines = `${r.stdout || ""}${r.stderr || ""}`.split(/\r?\n/).filter(Boolean);
    return { ok: r.status === 0, lines };
}

export default {
    name: "text-fit",
    group: "docs",
    inputs: ["web/shared/text-budget.json", "scripts/check-text-fit.py"],
    async run(ctx) {
        const python = findPython(ctx.env);
        if (!python) return { skip: "没有可用的 Python 3（可用 RINNE_PYTHON 指定）" };
        const { T } = await loadDict(ROOT);
        const dump = {};
        for (const k of Object.keys(T.zh)) dump[k] = { zh: T.zh[k], en: T.en[k], fr: T.fr[k] };
        const dir = fs.mkdtempSync(path.join(os.tmpdir(), "rinne-textfit-"));
        try {
            const file = path.join(dir, "strings.json");
            fs.writeFileSync(file, JSON.stringify(dump));
            return runPython(python, ["--strings", file]);
        } finally {
            fs.rmSync(dir, { recursive: true, force: true });
        }
    },
    selfTest(ctx) {
        const python = findPython(ctx?.env);
        if (!python) return { ok: true, bad: 1, good: 1, lines: ["没有可用的 Python 3，跳过 text-fit 自测"] };
        const r = runPython(python, ["--self-test"]);
        // 脚本的自测内含超预算必须变红与短文本必须通过两类用例
        return { ok: r.ok, bad: 1, good: 1, lines: r.ok ? [] : r.lines };
    },
};
