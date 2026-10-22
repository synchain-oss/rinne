// SPDX-License-Identifier: GPL-3.0-or-later
//
// 契约表：真源是 tests/golden/state-schema.json 与 tests/golden/ipc-layout.json（两份今天都在仓里）。
// 渲染成两张 Markdown 表：
//   state —— 容器头字段表（offset / field / type）与 CFGS 键表；
//   ipc   —— 段常量表与段名族。
//
// 真源不成形就报错、不出占位值：每一项的必填字段都在 contractProblems 里逐项核对，缺哪个报哪个；
// 渲染函数里没有「?」「取默认」这类回退，字段缺了就抛错。
//
// 落地面 docs/contracts/* 是冻结面，本模块只声明落地面、不接入：接入标记等于改冻结契约文档，要由契约属主在冻结流程内做。

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
    "state-shape": "state-schema.json 有非空的 header 数组与 cfgs.keys 数组",
    "state-header": "header 每项有 offset（数字）、field、type",
    "state-cfgs":
        "cfgs.keys 每项有 key、name、type、default、input、output，长度（bytes 或 maxBytes）、范围（min 与 max 同在或同缺）、不合法时的处理（outOfRange 或 invalid）",
    "ipc-shape": "ipc-layout.json 有非空的 constants 数组与 names 数组",
    "ipc-constants": "constants 每项有 constant 与 value",
    "ipc-names": "names 每项有 platform、scope、prefix、segment、name 与数字的 length",
    "render-refuses": "真源不成形时 render 抛错，不生成占位表格",
};

/** 表格单元：竖线转义（否则列数会算错），换行换成 <br>（否则表格会被撕开）。 */
const cell = (v) =>
    String(v ?? "")
        .replace(/\|/g, "\\|")
        .replace(/\r?\n/g, "<br>");

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

/** 字段写了：undefined 与 null 都算没写。渲染与形状核对共用这一个判法，两边对「缺」的理解不会错开。 */
const has = (v) => v !== undefined && v !== null;

/** 取必填字段；缺了抛错，不给默认值。 */
function need(obj, field, what) {
    if (obj?.[field] === undefined || obj[field] === null) throw new Error(`${what} 缺 ${field}`);
    return obj[field];
}

/** 容器头字段表。state 是 state-schema.json 的内容。 */
export function renderStateHeader(state) {
    const rows = (state.header ?? []).map((h) => [
        need(h, "offset", "header 里有条目"),
        need(h, "field", "header 里有条目"),
        need(h, "type", "header 里有条目"),
    ]);
    return table(STATE_HEADER, rows);
}

/** CFGS 键表。state 是 state-schema.json 的内容。 */
export function renderCfgsTable(state) {
    const rows = (state?.cfgs?.keys ?? []).map((k) => {
        const what = `cfgs.keys 里的键 ${k?.key ?? "(没有 key)"}`;
        // 缺失的判法与 contractProblems 一致：undefined 与 null 都算没写，否则 null 会被渲染成「null…null」或空单元格
        const hasMin = has(k.min);
        const hasMax = has(k.max);
        if (hasMin !== hasMax) throw new Error(`${what} 的 min 与 max 只写了一个`);
        const bytes = has(k.bytes) ? k.bytes : `≤ ${need(k, "maxBytes", what)}`;
        const onBad = k.outOfRange ?? k.invalid;
        if (!has(onBad)) throw new Error(`${what} 缺 outOfRange 或 invalid`);
        return [
            need(k, "key", what),
            need(k, "name", what),
            need(k, "type", what),
            bytes,
            hasMin ? `${k.min}…${k.max}` : "—",
            k.default === "" ? "空串" : need(k, "default", what),
            onBad,
            need(k, "input", what) ? "是" : "—",
            need(k, "output", what) ? "是" : "—",
        ];
    });
    return table(CFGS_HEADER, rows);
}

/** 段常量表。ipc 是 ipc-layout.json 的内容。 */
export function renderConstantsTable(ipc) {
    const rows = (ipc.constants ?? []).map((c) => [
        need(c, "constant", "constants 里有条目"),
        need(c, "value", "constants 里有条目"),
    ]);
    return table(CONST_HEADER, rows);
}

/** 段名族表。ipc 是 ipc-layout.json 的内容。 */
export function renderNamesTable(ipc) {
    const rows = (ipc.names ?? []).map((n) => {
        const what = "names 里有条目";
        return NAME_HEADER.map((_, i) =>
            need(n, ["platform", "scope", "prefix", "segment", "name", "length"][i], what),
        );
    });
    return table(NAME_HEADER, rows);
}

const isObj = (v) => typeof v === "object" && v !== null && !Array.isArray(v);
const isStr = (v) => typeof v === "string" && v !== "";

/** 真源的形状问题；返回问题字符串数组，每条以 [判据名] 开头。逐项核对必填字段，缺哪个报哪个。 */
export function contractProblems(state, ipc) {
    const out = [];
    if (!isObj(state)) out.push(`[state-shape] ${STATE_FILE} 的内容不是对象`);
    else {
        if (!Array.isArray(state.header) || !state.header.length)
            out.push(`[state-shape] ${STATE_FILE} 缺 header 数组`);
        else
            state.header.forEach((h, i) => {
                const where = `${STATE_FILE} 的 header 第 ${i + 1} 项`;
                if (!isObj(h)) return out.push(`[state-header] ${where} 不是对象`);
                if (typeof h.offset !== "number") out.push(`[state-header] ${where} 缺数字的 offset`);
                if (!isStr(h.field)) out.push(`[state-header] ${where} 缺 field`);
                if (!isStr(h.type)) out.push(`[state-header] ${where} 缺 type`);
            });
        if (!isObj(state.cfgs) || !Array.isArray(state.cfgs.keys) || !state.cfgs.keys.length)
            out.push(`[state-shape] ${STATE_FILE} 缺 cfgs.keys 数组`);
        else
            state.cfgs.keys.forEach((k, i) => {
                const where = `${STATE_FILE} 的 cfgs.keys 第 ${i + 1} 项（键 ${k?.key ?? "?"}）`;
                if (!isObj(k)) return out.push(`[state-cfgs] ${where} 不是对象`);
                if (typeof k.key !== "number") out.push(`[state-cfgs] ${where} 缺数字的 key`);
                if (!isStr(k.name)) out.push(`[state-cfgs] ${where} 缺 name`);
                if (!isStr(k.type)) out.push(`[state-cfgs] ${where} 缺 type`);
                if (!has(k.default)) out.push(`[state-cfgs] ${where} 缺 default`);
                if (typeof k.input !== "boolean") out.push(`[state-cfgs] ${where} 的 input 不是布尔值`);
                if (typeof k.output !== "boolean") out.push(`[state-cfgs] ${where} 的 output 不是布尔值`);
                if (!has(k.bytes) && !has(k.maxBytes)) out.push(`[state-cfgs] ${where} 缺 bytes 或 maxBytes`);
                if (has(k.min) !== has(k.max)) out.push(`[state-cfgs] ${where} 的 min 与 max 只写了一个`);
                if (!has(k.outOfRange) && !has(k.invalid)) out.push(`[state-cfgs] ${where} 缺 outOfRange 或 invalid`);
            });
    }
    if (!isObj(ipc)) {
        out.push(`[ipc-shape] ${IPC_FILE} 的内容不是对象`);
        return out;
    }
    if (!Array.isArray(ipc.constants) || !ipc.constants.length) out.push(`[ipc-shape] ${IPC_FILE} 缺 constants 数组`);
    else
        ipc.constants.forEach((c, i) => {
            const where = `${IPC_FILE} 的 constants 第 ${i + 1} 项`;
            if (!isObj(c)) return out.push(`[ipc-constants] ${where} 不是对象`);
            if (!isStr(c.constant)) out.push(`[ipc-constants] ${where} 缺 constant`);
            if (!has(c.value)) out.push(`[ipc-constants] ${where} 缺 value`);
        });
    if (!Array.isArray(ipc.names) || !ipc.names.length) out.push(`[ipc-shape] ${IPC_FILE} 缺 names 数组`);
    else
        ipc.names.forEach((n, i) => {
            const where = `${IPC_FILE} 的 names 第 ${i + 1} 项`;
            if (!isObj(n)) return out.push(`[ipc-names] ${where} 不是对象`);
            for (const f of ["platform", "scope", "prefix", "segment", "name"])
                if (!isStr(n[f])) out.push(`[ipc-names] ${where} 缺 ${f}`);
            if (typeof n.length !== "number") out.push(`[ipc-names] ${where} 缺数字的 length`);
        });
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

/**
 * 自测用例：每个用例 { label, bad, passed, expect }。期望为空表示正例（一条问题都不许有），
 * 否则是坏样例（期望的判据都要被点名）。末尾的覆盖用例核对每条判据都被某个通过的坏样例点名。
 */
export function selfTestCases() {
    const key = (over = {}) => ({
        key: 1,
        name: "group",
        type: "u8",
        bytes: 1,
        min: 0,
        max: 8,
        default: 0,
        outOfRange: "default",
        input: true,
        output: true,
        ...over,
    });
    const without = (o, f) => {
        const c = { ...o };
        delete c[f];
        return c;
    };
    const state = {
        header: [
            { offset: 0, field: "magic", type: "u32" },
            { offset: 4, field: "abi", type: "u32" },
        ],
        cfgs: {
            keys: [
                key(),
                key({
                    key: 9,
                    name: "ui_lang",
                    type: "ascii",
                    bytes: undefined,
                    maxBytes: 8,
                    min: undefined,
                    max: undefined,
                    default: "",
                    outOfRange: undefined,
                    invalid: "default",
                }),
            ],
        },
    };
    const name = (over = {}) => ({
        platform: "win32",
        scope: "formal",
        prefix: "SynchainRinne",
        segment: "rv",
        name: "x",
        length: 1,
        ...over,
    });
    const ipc = {
        constants: [{ constant: "kIpcAbi", value: 1 }],
        names: [name()],
    };
    const cases = [];
    const add = (label, s, i, expect) => {
        const rules = contractProblems(s, i).map(ruleOf).filter(Boolean);
        const bad = expect.length > 0;
        cases.push({ label, bad, passed: bad ? expect.every((r) => rules.includes(r)) : rules.length === 0, expect });
    };
    const withKey = (k) => ({ ...state, cfgs: { keys: [k] } });
    add("齐的真源没问题", state, ipc, []);
    add("state 不是对象", null, ipc, ["state-shape"]);
    add("state 缺 header 数组", { cfgs: state.cfgs }, ipc, ["state-shape"]);
    add("state 缺 cfgs.keys", { header: state.header }, ipc, ["state-shape"]);
    add("header 里某项不是对象", { ...state, header: [null] }, ipc, ["state-header"]);
    add("header 里某项缺 field", { ...state, header: [{ offset: 0, type: "u32" }] }, ipc, ["state-header"]);
    add("header 里某项缺 type", { ...state, header: [{ offset: 0, field: "magic" }] }, ipc, ["state-header"]);
    add("header 里某项的 offset 不是数字", { ...state, header: [{ offset: "0", field: "magic", type: "u32" }] }, ipc, [
        "state-header",
    ]);
    add("cfgs 某项不是对象", { ...state, cfgs: { keys: [null] } }, ipc, ["state-cfgs"]);
    add("cfgs 某项的 key 不是数字", withKey(key({ key: "1" })), ipc, ["state-cfgs"]);
    add("cfgs 某项缺 name", withKey(without(key(), "name")), ipc, ["state-cfgs"]);
    add("cfgs 某项缺 type", withKey(without(key(), "type")), ipc, ["state-cfgs"]);
    add("cfgs 某项的 output 不是布尔值", withKey(key({ output: 1 })), ipc, ["state-cfgs"]);
    add("cfgs 某项缺 default", withKey(without(key(), "default")), ipc, ["state-cfgs"]);
    add("cfgs 某项缺长度", withKey(without(key(), "bytes")), ipc, ["state-cfgs"]);
    add("cfgs 某项只写了 min 没写 max", withKey(without(key(), "max")), ipc, ["state-cfgs"]);
    add("cfgs 某项缺不合法时的处理", withKey(without(key(), "outOfRange")), ipc, ["state-cfgs"]);
    add("cfgs 某项的 input 不是布尔值", withKey(key({ input: "yes" })), ipc, ["state-cfgs"]);
    add("ipc 缺 constants 数组", state, { names: ipc.names }, ["ipc-shape"]);
    add("ipc 缺 names 数组", state, { constants: ipc.constants }, ["ipc-shape"]);
    add("ipc 不是对象", state, null, ["ipc-shape"]);
    add("constants 某项不是对象", state, { ...ipc, constants: [null] }, ["ipc-constants"]);
    add("constants 某项缺 constant", state, { ...ipc, constants: [{ value: 1 }] }, ["ipc-constants"]);
    add("names 某项不是对象", state, { ...ipc, names: [null] }, ["ipc-names"]);
    add("constants 某项缺 value", state, { ...ipc, constants: [{ constant: "kIpcAbi" }] }, ["ipc-constants"]);
    add("names 某项缺 segment", state, { ...ipc, names: [without(name(), "segment")] }, ["ipc-names"]);
    add("names 某项的 length 不是数字", state, { ...ipc, names: [name({ length: "1" })] }, ["ipc-names"]);
    // 判据 render-refuses：真源不成形时 render 抛错，不出占位表格
    const throws = (fn) => {
        try {
            fn();
            return false;
        } catch {
            return true;
        }
    };
    cases.push({
        label: "真源缺字段时 render 抛错",
        bad: true,
        passed: throws(() => contractBlockOf({ header: state.header }, ipc, "state")),
        expect: ["render-refuses"],
    });
    cases.push({
        label: "直接渲染缺字段的 CFGS 项也抛错，没有「?」「取默认」回退",
        bad: true,
        passed:
            throws(() => renderCfgsTable(withKey(without(without(key(), "bytes"), "maxBytes")))) &&
            throws(() => renderCfgsTable(withKey(without(key(), "outOfRange")))) &&
            throws(() => renderCfgsTable(withKey(without(key(), "max")))),
        expect: [],
    });
    cases.push({
        label: "真源齐备时 render 不抛错",
        bad: false,
        passed: !throws(() => contractBlockOf(state, ipc, "state")),
        expect: [],
    });
    // 正例：四张表渲染得出来，行数与真源一致；字符串默认值写成「空串」，没有占位符
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
        expect: [],
    });
    cases.push({
        label: "CFGS 表没有占位符：长度、范围、不合法时都取自真源",
        bad: false,
        passed:
            !cfgs.includes("?") &&
            !cfgs.includes("取默认") &&
            cfgs.includes("| 1 | group | u8 | 1 | 0…8 | 0 | default | 是 | 是 |") &&
            cfgs.includes("| 9 | ui_lang | ascii | ≤ 8 | — | 空串 | default | 是 | 是 |"),
        expect: [],
    });
    // null 与缺字段同样对待：形状核对认 null 为没写，渲染也要一样，不能渲染出「null…null」或空单元格
    const nullKey = key({
        key: 5,
        name: "x",
        bytes: null,
        maxBytes: 8,
        min: null,
        max: null,
        outOfRange: null,
        invalid: "default",
    });
    const nullTable = renderCfgsTable(withKey(nullKey));
    cases.push({
        label: "min、max、bytes、outOfRange 写 null 等于没写：渲染成「—」「≤ 8」「default」，不出现 null",
        bad: false,
        passed:
            contractProblems(withKey(nullKey), ipc).length === 0 &&
            nullTable.includes("| 5 | x | u8 | ≤ 8 | — | 0 | default | 是 | 是 |") &&
            !nullTable.includes("null"),
        expect: [],
    });
    cases.push({
        label: "min 写 null、max 有值：形状核对判错，渲染也抛错",
        bad: true,
        passed:
            contractProblems(withKey(key({ min: null })), ipc).some((p) => ruleOf(p) === "state-cfgs") &&
            throws(() => renderCfgsTable(withKey(key({ min: null })))),
        expect: ["state-cfgs"],
    });
    cases.push({
        label: "bytes 与 maxBytes 都写 null：形状核对判错，渲染也抛错",
        bad: true,
        passed:
            contractProblems(withKey(key({ bytes: null })), ipc).some((p) => ruleOf(p) === "state-cfgs") &&
            throws(() => renderCfgsTable(withKey(key({ bytes: null })))),
        expect: ["state-cfgs"],
    });
    cases.push({
        label: "outOfRange 与 invalid 都写 null：形状核对判错，渲染也抛错",
        bad: true,
        passed:
            contractProblems(withKey(key({ outOfRange: null })), ipc).some((p) => ruleOf(p) === "state-cfgs") &&
            throws(() => renderCfgsTable(withKey(key({ outOfRange: null })))),
        expect: ["state-cfgs"],
    });
    cases.push({
        label: "只有 invalid 且写 null：形状核对判错，渲染也抛错，不渲染出 null",
        bad: true,
        passed:
            contractProblems(withKey(key({ outOfRange: undefined, invalid: null })), ipc).some(
                (p) => ruleOf(p) === "state-cfgs",
            ) && throws(() => renderCfgsTable(withKey(key({ outOfRange: undefined, invalid: null })))),
        expect: ["state-cfgs"],
    });
    const unnamed = Object.keys(RULES).filter((r) => !cases.some((c) => c.passed && c.expect.includes(r)));
    cases.push({
        label: `每条判据都有坏样例点名${unnamed.length ? `（没有：${unnamed.join("、")}）` : ""}`,
        bad: false,
        passed: unnamed.length === 0,
        expect: [],
    });
    return cases;
}

export const selfTest = () => caseResult(selfTestCases());

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
