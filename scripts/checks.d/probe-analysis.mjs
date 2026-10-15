// SPDX-License-Identifier: GPL-3.0-or-later
//
// 探针分析工具：analyze-probe.py 的自测（合成夹具的通过 / 不通过 / 数据不全、严格格式、脱敏）、
// LOG-FORMAT.md 与分析器规格的对拍，以及 validation 模板的栏目。
// 变异检查会逐条屏蔽 A1-A9 的判据、两道脱敏防线和关键阈值，自测都必须因此变红。

import fs from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { caseResult, inCI } from "../lib/check-kit.mjs";

const SCRIPT = "scripts/probe/analyze-probe.py";
const FORMAT_DOC = "scripts/probe/LOG-FORMAT.md";
const TEMPLATE = "docs/validation/TEMPLATE.md";
const TEMPLATE_NAME = "<YYYY-MM-DD>-<host>-<os>.md";

let cachedPython;

/** 找一个能跑的 Python 3：python3、python、py -3。找不到返回 null。 */
function findPython() {
    if (cachedPython !== undefined) return cachedPython;
    cachedPython = null;
    for (const cmd of [["python3"], ["python"], ["py", "-3"]]) {
        const r = spawnSync(cmd[0], [
            ...cmd.slice(1),
            "-c",
            "import sys; sys.exit(0 if sys.version_info >= (3, 9) else 1)",
        ]);
        if (r.status === 0) {
            cachedPython = cmd;
            break;
        }
    }
    return cachedPython;
}

function py(root, args) {
    const cmd = findPython();
    const r = spawnSync(cmd[0], [...cmd.slice(1), "-B", path.join(root, SCRIPT), ...args], {
        encoding: "utf8",
        env: { ...process.env, PYTHONDONTWRITEBYTECODE: "1", PYTHONIOENCODING: "utf-8" },
        maxBuffer: 64 * 1024 * 1024,
    });
    return r;
}

/** 文档里没有出现的规格项。events / kinds / variants 逐个要求带反引号；case 只要求两端的编号出现。 */
export function docProblems(spec, doc) {
    const missing = [];
    const need = (label, token) => {
        if (!doc.includes(token)) missing.push(`${label} ${token}`);
    };
    need("魔数", spec.magic);
    for (const [kind, cols] of Object.entries(spec.rows)) {
        need("行类型", `\`${kind}\``);
        for (const c of cols) need(`${kind} 行的列`, `\`${c}\``);
    }
    for (const e of spec.events) need("事件", `\`${e}\``);
    for (const v of spec.variants) need("variant", `\`${v}\``);
    for (const g of spec.guards) need("guard", `\`${g}\``);
    for (const prefix of ["CB", "LU"]) {
        const ids = spec.cases.filter((c) => c.startsWith(prefix));
        need("case", `\`${ids[0]}\``);
        need("case", `\`${ids[ids.length - 1]}\``);
    }
    need("比对表头", spec.compare_head);
    return missing;
}

/** validation 模板必须有的栏目：A1-A9、F1-F4 和命名规则。 */
export function templateProblems(text) {
    const missing = [];
    const has = (id) => new RegExp(`(^|[^A-Za-z0-9])${id}($|[^A-Za-z0-9])`).test(text);
    for (let i = 1; i <= 9; i++) if (!has(`A${i}`)) missing.push(`A${i}`);
    for (let i = 1; i <= 4; i++) if (!has(`F${i}`)) missing.push(`F${i}`);
    if (!text.includes(TEMPLATE_NAME)) missing.push("命名规则");
    return missing;
}

function readSpec(root) {
    const r = py(root, ["--spec"]);
    if (r.status !== 0) throw new Error(`analyze-probe --spec 失败（退出码 ${r.status}）`);
    return JSON.parse(r.stdout);
}

export default {
    name: "probe-analysis",
    group: "docs",
    inputs: [SCRIPT],
    run(ctx) {
        if (!findPython()) {
            return inCI(ctx.env)
                ? { ok: false, lines: ["找不到 Python 3（python3 / python / py -3）"] }
                : { skip: "本机没有 Python 3" };
        }
        const lines = [];
        const st = py(ctx.root, ["--self-test", "--json"]);
        if (st.status !== 0) lines.push(`analyze-probe 自测失败（退出码 ${st.status}）：${(st.stderr || "").trim()}`);
        const spec = readSpec(ctx.root);
        const doc = fs.readFileSync(path.join(ctx.root, FORMAT_DOC), "utf8");
        for (const m of docProblems(spec, doc)) lines.push(`${FORMAT_DOC} 缺：${m}`);
        const tpl = path.join(ctx.root, TEMPLATE);
        if (!fs.existsSync(tpl)) lines.push(`缺 ${TEMPLATE}`);
        else for (const m of templateProblems(fs.readFileSync(tpl, "utf8"))) lines.push(`${TEMPLATE} 缺栏目：${m}`);
        return { ok: lines.length === 0, lines };
    },
    selfTest(ctx) {
        // Python 一侧的自测由 run() 跑；这里证明两道文档对拍本身会变红，并并入 Python 的计数
        const spec = {
            magic: "m,1",
            rows: { H: ["role"] },
            events: ["ev_a"],
            variants: ["-"],
            guards: ["off"],
            cases: ["CB-01", "CB-02", "LU-01", "LU-09"],
            compare_head: "h1,h2",
        };
        const full = "m,1 `H` `role` `ev_a` `-` `off` `CB-01` `CB-02` `LU-01` `LU-09` h1,h2";
        const tpl = `${TEMPLATE_NAME} ${Array.from({ length: 9 }, (_, i) => `A${i + 1}`).join(" ")} F1 F2 F3 F4`;
        const cases = [
            ["文档齐全", false, docProblems(spec, full).length === 0],
            ["文档缺一个事件", true, docProblems(spec, full.replace("`ev_a`", "")).length > 0],
            ["文档缺魔数", true, docProblems(spec, full.replace("m,1", "")).length > 0],
            ["文档缺 case 编号", true, docProblems(spec, full.replace("`LU-09`", "")).length > 0],
            ["模板齐全", false, templateProblems(tpl).length === 0],
            ["模板缺 A5", true, templateProblems(tpl.replace("A5", "")).length > 0],
            ["模板缺 F3", true, templateProblems(tpl.replace("F3", "")).length > 0],
            ["模板缺命名规则", true, templateProblems(tpl.replace(TEMPLATE_NAME, "")).length > 0],
        ].map(([label, bad, passed]) => ({ label, bad, passed }));
        const own = caseResult(cases);
        if (!findPython())
            return inCI(ctx?.env ?? process.env)
                ? { ...own, ok: false, lines: [...own.lines, "找不到 Python 3"] }
                : own;
        const r = py(ctx?.root ?? process.cwd(), ["--self-test", "--json"]);
        let sub;
        try {
            sub = JSON.parse(r.stdout);
        } catch {
            return { ...own, ok: false, lines: [...own.lines, "analyze-probe 自测没有输出 JSON"] };
        }
        return {
            ok: own.ok && sub.ok,
            bad: own.bad + sub.bad,
            good: own.good + sub.good,
            lines: [...own.lines, ...sub.lines],
        };
    },
    mutationCheck(ctx) {
        if (!findPython())
            return inCI(ctx?.env ?? process.env)
                ? { ok: false, lines: ["找不到 Python 3"] }
                : { ok: true, lines: ["本机没有 Python 3，跳过"] };
        const r = py(ctx?.root ?? process.cwd(), ["--mutation-check", "--json"]);
        try {
            return JSON.parse(r.stdout);
        } catch {
            return { ok: false, lines: ["analyze-probe 变异检查没有输出 JSON"] };
        }
    },
};
