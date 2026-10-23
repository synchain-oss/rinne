// SPDX-License-Identifier: GPL-3.0-or-later
//
// 文本宽度预算门禁：先在本进程里合并三语词条写到临时文件，再交给 check-text-fit.py（只用 Python 标准库）。
// 找不到 Python 时判红（本机和 CI 相同）。Python 用字体检查的 locatePython 找：RINNE_PYTHON、git config rinne.python，
// 都没配置时才退回 PATH 上的 python3、python；配置了却用不了直接判红并写明原因。

import { spawnSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { ROOT } from "../lib/check-kit.mjs";
import { cleanupTempDir, makeTempDir } from "../lib/safe-rm.mjs";
import { locatePython } from "./fonts.mjs";
import { loadDict } from "../check-i18n.mjs";

const SCRIPT = path.join(ROOT, "scripts", "check-text-fit.py");

export const findPython = (env = process.env) =>
    locatePython(env, "import sys; sys.exit(0 if sys.version_info >= (3, 8) else 3)", {
        need: "可用的 Python 3.8+",
        check: "文本宽度检查",
        tooOld: "版本低于 3.8",
    });

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
        const { python, why } = findPython(ctx.env);
        if (!python) return { ok: false, lines: [why] };
        const { T } = await loadDict(ROOT);
        const dump = {};
        for (const k of Object.keys(T.zh)) dump[k] = { zh: T.zh[k], en: T.en[k], fr: T.fr[k] };
        const dir = makeTempDir("rinne-textfit-");
        try {
            const file = path.join(dir, "strings.json");
            fs.writeFileSync(file, JSON.stringify(dump));
            return runPython(python, ["--strings", file]);
        } finally {
            cleanupTempDir(dir);
        }
    },
    selfTest(ctx) {
        // 配置了却找不到：判红、说出来源，不退回 PATH 上的 Python
        const missing = findPython({ ...(ctx?.env ?? process.env), RINNE_PYTHON: "rinne-no-such-python" });
        const finder = !missing.python && /RINNE_PYTHON 指定的 Python：找不到这个程序/.test(missing.why || "");
        const { python, why } = findPython(ctx?.env);
        if (!python) return { ok: false, bad: 1, good: 1, lines: [why] };
        const r = runPython(python, ["--self-test"]);
        // 脚本的自测内含超预算必须变红与短文本必须通过两类用例
        const lines = [
            ...(finder ? [] : ["自测未通过：配置的 Python 找不到时应判红，不退回 PATH"]),
            ...(r.ok ? [] : r.lines),
        ];
        return { ok: finder && r.ok, bad: 2, good: 1, lines };
    },
};
