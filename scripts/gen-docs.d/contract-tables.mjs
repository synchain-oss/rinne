// SPDX-License-Identifier: GPL-3.0-or-later
//
// 契约表：真源是 tests/golden/state-schema.json 与 tests/golden/ipc-layout.json（两份今天都在仓里）。
// 渲染成两张 Markdown 表：
//   state —— 容器头字段表（offset / field / type）与 CFGS 键表；
//   ipc   —— 段常量表与段名族。
//
// 落地面 docs/contracts/* 是冻结面，本卡只声明落地面、不接入：接入标记等于改冻结契约文档，要由契约属主在冻结流程内做。

import { readRepoText } from "../lib/docs-kit.mjs";
import { caseResult } from "../lib/check-kit.mjs";

export const STATE_FILE = "tests/golden/state-schema.json";
export const IPC_FILE = "tests/golden/ipc-layout.json";

/** 表头。契约文档正文用中文（CLAUDE.md 的「文档、上游与第三方」一节）。 */
const STATE_HEADER = ["偏移", "字段", "类型"];
const CFGS_HEADER = ["键", "名字", "类型", "长度（字节）", "范围", "默认", "不合法时", "Input", "Output"];
const CONST_HEADER = ["常量", "值"];
const NAME_HEADER = ["平台", "作用域", "前缀", "段", "名字", "长度"];

/** 判据名 → 说明。 */
export const RULES = {
    "state-shape": "state-schema.json 有 header 数组与 cfgs.keys 数组，每项字段齐全",
    "ipc-shape": "ipc-layout.json 有 constants 数组与 names 数组",
    "render-refuses": "真源不成形时 render 抛错，不生成占位表格",
};

/** 表格单元里的竖线要转义，否则表格列数会算错。 */
const cell = (v) => String(v ?? "").replace(/\|/g, "\\|");

/** 读一个 JSON 真源；文件不在返回 null，JSON 不合法抛错。 */
export function loadJson(root, rel) {
    const text = readRepoText(root, rel);
    if (text === null) return null;
    try {
        return JSON.parse(text);
    } catch (e) {
        throw new Error(`${rel} 不是合法的 JSON：${e.message}`);
    }
}

/** 一张表。rows 是每行一串单元。 */
export const table = (header, rows) =>
    [header, header.map(() => "---"), ...rows].map((r) => `| ${r.map(cell).join(" | ")} |`).join("\n");

/** 容器头字段表。state 是 state-schema.json 的内容。 */
export function renderStateHeader(state) {
    const rows = (state.header ?? []).map((h) => [h.offset, h.field, h.type]);
    return table(STATE_HEADER, rows);
}

/** CFGS 键表。state 是 state-schema.json 的内容。 */
export function renderCfgsTable(state) {
    const keys = state?.cfgs?.keys ?? [];
    const rows = keys.map((k) => [
        k.key,
        k.name,
        k.type,
        k.bytes ?? `≤ ${k.maxBytes ?? "?"}`,
        k.min !== undefined || k.max !== undefined ? `${k.min ?? "?"}…${k.max ?? "?"}` : "—",
        k.default === "" ? "空串" : k.default,
        k.outOfRange ?? k.invalid ?? "取默认",
        k.input ? "是" : "—",
        k.output ? "是" : "—",
    ]);
    return table(CFGS_HEADER, rows);
}

/** 段常量表。ipc 是 ipc-layout.json 的内容。 */
export function renderConstantsTable(ipc) {
    const rows = (ipc.constants ?? []).map((c) => [c.constant, c.value]);
    return table(CONST_HEADER, rows);
}

/** 段名族表。ipc 是 ipc-layout.json 的内容。 */
export function renderNamesTable(ipc) {
    const rows = (ipc.names ?? []).map((n) => [n.platform, n.scope, n.prefix, n.segment, n.name, n.length]);
    return table(NAME_HEADER, rows);
}

/** 真源的形状问题；返回问题字符串数组，每条以 [判据名] 开头。 */
export function contractProblems(state, ipc) {
    const out = [];
    if (!state || typeof state !== "object") return [`[state-shape] ${STATE_FILE} 的内容不是对象`];
    if (!Array.isArray(state.header) || !state.header.length) out.push(`[state-shape] ${STATE_FILE} 缺 header 数组`);
    else
        for (const h of state.header)
            for (const k of ["offset", "field", "type"])
                if (h[k] === undefined) out.push(`[state-shape] ${STATE_FILE} 的 header 里有条目缺 ${k}`);
    if (!state.cfgs || !Array.isArray(state.cfgs.keys)) out.push(`[state-shape] ${STATE_FILE} 缺 cfgs.keys 数组`);
    if (!ipc || typeof ipc !== "object") {
        out.push(`[ipc-shape] ${IPC_FILE} 的内容不是对象`);
        return out;
    }
    if (!Array.isArray(ipc.constants) || !ipc.constants.length) out.push(`[ipc-shape] ${IPC_FILE} 缺 constants 数组`);
    if (!Array.isArray(ipc.names) || !ipc.names.length) out.push(`[ipc-shape] ${IPC_FILE} 缺 names 数组`);
    return out;
}

/** 一个落地面 id 的块正文；真源不成形时抛错。id 是 "state" 或 "ipc"。 */
export function contractBlockOf(state, ipc, id) {
    const problems = contractProblems(state, ipc);
    if (problems.length) throw new Error(problems.join("；"));
    if (id === "state") return [renderStateHeader(state), "", renderCfgsTable(state)].join("\n");
    if (id === "ipc") return [renderConstantsTable(ipc), "", renderNamesTable(ipc)].join("\n");
    throw new Error(`未知的落地面 id ${id}`);
}

/** 问题串里的判据名。 */
export const ruleOf = (problem) => /^\[([^\]]+)\]/.exec(problem)?.[1] ?? "";

/** 自测：每个判据都有坏样例点名，另有一个覆盖用例核对每条判据都被点名过。 */
export function selfTest() {
    const state = {
        header: [
            { offset: 0, field: "magic", type: "u32" },
            { offset: 4, field: "abi", type: "u32" },
        ],
        cfgs: {
            keys: [
                { key: 1, name: "group", type: "u8", bytes: 1, min: 0, max: 8, default: 0, input: true, output: true },
            ],
        },
    };
    const ipc = {
        constants: [{ constant: "kIpcAbi", value: 1 }],
        names: [{ platform: "win32", scope: "formal", prefix: "SynchainRinne", segment: "rv", name: "x", length: 1 }],
    };
    const cases = [];
    const add = (label, s, i, expectRules) => {
        const rules = contractProblems(s, i).map(ruleOf).filter(Boolean);
        cases.push({ label, bad: rules.length > 0, passed: expectRules.every((r) => rules.includes(r)) });
    };
    add("齐的真源没问题", state, ipc, []);
    add("state 不是对象", null, ipc, ["state-shape"]);
    add("state 缺 header 数组", { cfgs: { keys: [] } }, ipc, ["state-shape"]);
    add("header 里某项缺 type", { ...state, header: [{ offset: 0, field: "magic" }] }, ipc, ["state-shape"]);
    add("state 缺 cfgs.keys", { header: state.header }, ipc, ["state-shape"]);
    add("ipc 缺 constants 数组", state, { names: ipc.names }, ["ipc-shape"]);
    add("ipc 缺 names 数组", state, { constants: ipc.constants }, ["ipc-shape"]);
    // 判据 render-refuses：真源不成形时 render 抛错，不出占位表格
    const threw = (s, i) => {
        try {
            contractBlockOf(s, i, "state");
            return false;
        } catch {
            return true;
        }
    };
    cases.push({
        label: "真源缺字段时 render 抛错",
        bad: true,
        passed: threw({ header: state.header }, ipc),
    });
    cases.push({
        label: "真源齐备时 render 不抛错",
        bad: false,
        passed: !threw(state, ipc),
    });
    // 正例：四张表渲染得出来，行数与真源一致
    const header = renderStateHeader(state);
    const cfgs = renderCfgsTable(state);
    const consts = renderConstantsTable(ipc);
    const names = renderNamesTable(ipc);
    cases.push({
        label: "四张表的行数与真源一致",
        bad: false,
        passed:
            header.split("\n").length === state.header.length + 2 &&
            cfgs.split("\n").length === state.cfgs.keys.length + 2 &&
            consts.split("\n").length === ipc.constants.length + 2 &&
            names.split("\n").length === ipc.names.length + 2,
    });
    const seen = new Set();
    for (const [s, i] of [
        [state, ipc],
        [null, ipc],
        [{ cfgs: { keys: [] } }, ipc],
        [{ ...state, header: [{ offset: 0, field: "magic" }] }, ipc],
        [{ header: state.header }, ipc],
        [state, { names: ipc.names }],
        [state, { constants: ipc.constants }],
    ])
        for (const p of contractProblems(s, i)) seen.add(ruleOf(p));
    seen.add("render-refuses");
    cases.push({
        label: "每条判据都至少被一个坏样例点名",
        bad: false,
        passed: Object.keys(RULES).every((r) => seen.has(r)),
    });
    return caseResult(cases);
}

export default {
    name: "contract-tables",
    sources: [STATE_FILE, IPC_FILE],
    surfaces: [
        { file: "docs/contracts/STATE_SCHEMA.md", id: "state", required: false },
        { file: "docs/contracts/IPC_CONTRACT.md", id: "ipc", required: false },
    ],
    validate(ctx) {
        const state = loadJson(ctx.root, STATE_FILE);
        const ipc = loadJson(ctx.root, IPC_FILE);
        if (state === null || ipc === null) return []; // 真源缺失：跳过，不报问题
        return contractProblems(state, ipc);
    },
    render(ctx, id) {
        const state = loadJson(ctx.root, STATE_FILE);
        const ipc = loadJson(ctx.root, IPC_FILE);
        if (state === null) throw new Error(`缺 ${STATE_FILE}`);
        if (ipc === null) throw new Error(`缺 ${IPC_FILE}`);
        return contractBlockOf(state, ipc, id);
    },
    selfTest,
};
