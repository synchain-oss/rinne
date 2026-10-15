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

/**
 * 文档里没有出现或写错的规格项：行类型的列名与列序（按表格行逐列对拍）、H 的枚举取值、事件、case、variant、guard、
 * 阈值、取值上界、退出码。事件参数的类型与含义不在这里对拍。
 */
export function docProblems(spec, doc) {
    const missing = [];
    const need = (label, token) => {
        if (!doc.includes(token)) missing.push(`${label} ${token}`);
    };
    const tick = (t) => `\`${t}\``;
    const lines = doc.split("\n");
    need("魔数", spec.magic);
    for (const [kind, cols] of Object.entries(spec.rows)) {
        const line = lines.find((l) => l.startsWith(`| ${tick(kind)} |`));
        if (!line) {
            missing.push(`行类型 ${tick(kind)}`);
            continue;
        }
        const cells = line.split("|").map((c) => c.trim());
        const got = [...(cells[cells.length - 2] ?? "").matchAll(/`([^`]+)`/g)].map((m) => m[1]);
        if (got.join(",") !== cols.join(",")) missing.push(`${kind} 行的列名与列序应为 ${cols.join(" ")}`);
    }
    for (const [name, vals] of Object.entries(spec.h_enums)) for (const v of vals) need(`H.${name}`, tick(v));
    for (const e of spec.events) need("事件", tick(e));
    for (const v of spec.variants) need("variant", tick(v));
    for (const g of spec.guards) need("guard", tick(g));
    for (const c of spec.cases) need("case", tick(c));
    need("比对表头", spec.compare_head);
    const t = spec.thresholds;
    const nums = [t.min_steady_blocks, t.min_toggles, t.min_slice_blocks, t.residual_limit_dbfs];
    for (const n of nums) need("阈值", String(n));
    for (const ms of [t.slice_ms, t.stall_ms]) need("阈值", `${ms / 1000} s`);
    for (const [k, v] of Object.entries(spec.limits)) need(`上界 ${k}`, String(v));
    for (const code of Object.keys(spec.exit_codes)) need("退出码", `| ${code} |`);
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
    // 分析器本身不放进 inputs：它被删掉时本检查要变红，而不是 SKIP
    inputs: [],
    run(ctx) {
        if (!fs.existsSync(path.join(ctx.root, SCRIPT))) return { ok: false, lines: [`缺 ${SCRIPT}`] };
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
            rows: { H: ["role", "format"], B: ["seq", "n"] },
            h_enums: { role: ["I", "O"] },
            events: ["ev_a"],
            variants: ["-"],
            guards: ["off"],
            cases: ["CB-01", "CB-02", "LU-01", "LU-09"],
            compare_head: "h1,h2",
            thresholds: {
                min_steady_blocks: 32,
                min_toggles: 10,
                min_slice_blocks: 8,
                slice_ms: 2000,
                stall_ms: 5000,
                residual_limit_dbfs: -120,
            },
            limits: { buf: 65536 },
            exit_codes: { 0: "ok", 3: "incomplete" },
        };
        const full = [
            "m,1 `ev_a` `-` `off` `CB-01` `CB-02` `LU-01` `LU-09` h1,h2 `I` `O`",
            "| `H` | 实例头 | `role` `format` |",
            "| `B` | 逐块 | `seq` `n` |",
            "32 10 8 2 s 5 s -120 65536",
            "| 0 | 全过 |",
            "| 3 | 数据不全 |",
        ].join("\n");
        const tpl = `${TEMPLATE_NAME} ${Array.from({ length: 9 }, (_, i) => `A${i + 1}`).join(" ")} F1 F2 F3 F4`;
        const cases = [
            ["文档齐全", false, docProblems(spec, full).length === 0],
            ["文档缺一个事件", true, docProblems(spec, full.replace("`ev_a`", "")).length > 0],
            ["文档缺魔数", true, docProblems(spec, full.replace("m,1", "")).length > 0],
            ["文档缺 case 编号", true, docProblems(spec, full.replace("`CB-02`", "")).length > 0],
            ["文档里列序写反", true, docProblems(spec, full.replace("`seq` `n`", "`n` `seq`")).length > 0],
            ["文档缺一个列名", true, docProblems(spec, full.replace("`format`", "")).length > 0],
            ["文档缺 H 枚举取值", true, docProblems(spec, full.replace("`O`", "")).length > 0],
            ["文档缺阈值", true, docProblems(spec, full.replace("-120", "")).length > 0],
            ["文档缺取值上界", true, docProblems(spec, full.replace("65536", "")).length > 0],
            ["文档缺退出码", true, docProblems(spec, full.replace("| 3 | 数据不全 |", "")).length > 0],
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
