// SPDX-License-Identifier: GPL-3.0-or-later
//
// 参数契约的静态对拍：不编译，读 src/input/ParamTable.h 的 RINNE_PARAM / RINNE_CHOICE 宏行，与
// tests/golden/params-input.json、docs/contracts/PARAMETERS.md 的两种表逐项比对。编译的两遍在单测里：
// rinne_core_tests 把宏表展开后与 golden 逐字节比，rinne_params_tests 真建 JUCE 参数对象取宿主 ID 比 golden。比对的内容：
//   - 宏行：一行一个调用、参数个数对、全是字面量；参数序号从 0 起按行连续，每个档位参数的档位序号也是；ID 不重名；
//     类型、刻度、单位、可自动化取值合法；范围非空、步进为正、默认在范围内，默认与上限落在步进格上；对数刻度的下限为正；
//     开关与档位的形状对，档位行只属于档位参数；
//   - golden：合法 JSON；规范排版（每个参数一行、键序固定）；顶层依次是 status、versionHint、count，再是各参数；
//     逐参数逐字段等于宏表、先后一致；vst3 / au 两列等于本文件另算的散列（JUCE 的 String::hashCode 再清最高位），
//     两列各自不重复；
//   - 契约文档：status 行读得出，且与 golden 顶层的 status 相同；参数表（<!-- table:params --> 之后那张）与每个
//     档位参数的档位表（<!-- table:choices:<id> --> 之后那张）逐行等于 golden；
//   - 冻结面（写死在本文件的 FROZEN 里，不随 golden 改，宏表与 golden 各核一遍）：11 个参数的 ID、序号与类型，
//     golden 顶层的 versionHint 是 1（逐参数的 versionHint 这里没有核：宏表与 golden 没有这一栏，真实参数树的核对
//     由参数树布局落地的那个 PR 加用例），两个档位参数已有档位的桥 id 与先后（只能在末尾追加），可自动化参数仍可自动化、
//     范围与刻度不变。参数 ID 任何时候都不改；其余冻结面正式发布之后永不改，之前要改，同一个 PR 改 FROZEN
//     （本文件是基础设施，维护者批准）并附变更文档。
//     其余字段（默认、显示名、步进、单位、不可自动化参数的范围与刻度）改了不在这里拦：冻结之后改它们由契约守卫要求
//     同一个 PR 带变更文档。
// 每条判据有一个名字（RULES），报错行以 [名字] 开头。--self-test 的每个坏样例写明它必须触发哪几条判据，并核对每条
// 判据都至少被一个坏样例点名：删掉任何一条判据，点名它的坏样例就变红。--mutation-check 在真实文件上改一个默认值、
// 改一个 ID、调换两档时值、把 golden 的 status 改得与契约文档不一致，门禁都必须变红。
//   node scripts/run-checks.mjs --only contract-static-params [--self-test | --mutation-check]

import fs from "node:fs";
import path from "node:path";
import { caseResult } from "../lib/check-kit.mjs";
import { CONTRACTS, parseStatus } from "../check-commits.mjs";
import { stripCpp } from "./contract-static-ipc.mjs";

const TABLE = "src/input/ParamTable.h";
const GOLDEN = "tests/golden/params-input.json";
const DOC = CONTRACTS.params;
const SELF = "scripts/checks.d/contract-static-params.mjs";

const KINDS = { F: "float", B: "bool", C: "choice" };
const SKEWS = { lin: "linear", log: "log" };
const UNITS = { none: "", pct: "%", ms: "ms", hz: "Hz", db: "dB" };
const TOP_KEYS = ["status", "versionHint", "count"];
const ROW_KEYS = [
    "index",
    "name",
    "kind",
    "min",
    "max",
    "default",
    "step",
    "skew",
    "automatable",
    "unit",
    "vst3",
    "au",
];
const CHOICE_KEYS = ["choices", "bridgeIds"];
const VERSION_HINT = 1;

const NOTES = [
    "n64",
    "n32t",
    "n32",
    "n16t",
    "n16",
    "n8t",
    "n16d",
    "n8",
    "n4t",
    "n8d",
    "n4",
    "n2t",
    "n4d",
    "n2",
    "n2d",
    "n1",
];
/**
 * 冻结面。参数 ID 任何时候都不改；其余各项正式发布之后永不改，之前要改，同一个 PR 改这里并附变更文档。序号就是数组下标；choices 是已有档位的桥 id（新档只能接在后面）；写了 lo / hi / skew 的是可自动化参数
 * （宿主按归一化值记自动化，范围或刻度一变，已录的自动化就走样），它们也不许改成不可自动化。
 */
const FROZEN = [
    { id: "feedback", kind: "F", lo: 0, hi: 150, skew: "lin" },
    { id: "loop_on", kind: "B", automation: true },
    { id: "mix", kind: "F", lo: 0, hi: 100, skew: "lin" },
    { id: "delay_mode", kind: "C", choices: ["time", "sync"] },
    { id: "delay_ms", kind: "F" },
    { id: "delay_note", kind: "C", choices: NOTES },
    { id: "low_cut", kind: "F", lo: 20, hi: 2000, skew: "log" },
    { id: "high_cut", kind: "F", lo: 1000, hi: 20000, skew: "log" },
    { id: "polarity", kind: "B", automation: true },
    { id: "limiter", kind: "B" },
    { id: "ceiling", kind: "F" },
];
const isAutomationFrozen = (f) => f.automation || f.skew !== undefined;

const DOC_PARAM_HEADER = [
    "序号",
    "ID",
    "中文名",
    "宿主名",
    "类型",
    "范围",
    "默认",
    "步进",
    "刻度",
    "自动化",
    "宿主参数 ID",
];
const DOC_CHOICE_HEADER = ["序号", "桥取值", "宿主文字"];
const DOC_KIND = { float: "连续", bool: "开关", choice: "档位" };
const DOC_SKEW = { linear: "线性", log: "对数" };

/** 判据名 → 说明。报错行以 [名字] 开头；自测按名字核对。 */
export const RULES = {
    "input-missing": "参数表、golden 与契约文档都要在",
    "table-call": "宏行一行一个调用、参数个数对、全是字面量",
    "table-index": "参数序号从 0 起按行连续；每个档位参数的档位序号从 0 起连续",
    "table-unique": "参数 ID 不重名；同一参数的档位桥 id 与文字不重名",
    "table-value":
        "类型、刻度、单位、可自动化取值合法；范围非空、步进为正、默认在范围内；对数刻度下限为正；开关与档位的形状对",
    "table-grid": "默认值与上限落在步进格 lo + k·step 上",
    "table-choice": "档位行只属于档位参数；档位参数至少两档，档数等于上限加一",
    "golden-json": "golden 是合法的 JSON",
    "golden-canonical": "golden 是规范排版：每个参数一行、两格缩进、键序固定、文件末尾一个换行",
    "golden-top": "golden 顶层依次是 status、versionHint、count，再是各参数",
    "golden-keys": "golden 每个参数的键序固定，档位参数另带 choices 与 bridgeIds",
    "golden-status": "golden 的 status 是 draft 或 frozen",
    "golden-count": "golden 的 count 等于参数个数",
    "params-match": "参数表与 golden 逐参数逐字段相同，先后一致",
    "host-id": "vst3 / au 两列等于参数 ID 的散列（String::hashCode 再清最高位）",
    "host-id-unique": "vst3 / au 两列各自不重复",
    "doc-status": "契约文档的 status 行读得出（draft 或 frozen）",
    "golden-status-doc": "golden 的 status 等于契约文档的 status 行",
    "doc-table": "契约文档的参数表逐行等于 golden",
    "doc-choices": "契约文档每个档位参数的档位表逐行等于 golden",
    "frozen-param": "冻结面上的参数：ID（任何时候都不改）、序号与类型不变",
    "frozen-hint": "golden 顶层的 versionHint 是 1（逐参数的 versionHint 没有这一栏，不在这里核）",
    "frozen-choice": "档位参数已有档位的桥 id 与先后不变，新档只能追加在末尾",
    "frozen-automation": "可自动化的参数仍可自动化，范围与刻度不变",
};

/** 报错的收集：每一行以判据名开头。 */
class Findings {
    constructor() {
        this.lines = [];
    }
    add(rule, message) {
        if (!Object.hasOwn(RULES, rule)) throw new Error(`未登记的判据 ${rule}`);
        this.lines.push(`[${rule}] ${message}`);
    }
}

/** 宿主参数 ID：JUCE 的 String::hashCode（乘子 31、32 位回绕，逐码点）再清最高位。参数 ID 全是 ASCII。 */
export function hostId(id) {
    let h = 0;
    for (const c of id) h = (Math.imul(h, 31) + c.codePointAt(0)) >>> 0;
    return (h & 0x7fffffff) >>> 0;
}

// ---- 宏表 ----

/** 按顶层逗号切参数，字符串字面量里的逗号不算；引号不成对返回 null。 */
function splitArgs(text) {
    const out = [];
    let cur = "";
    let quoted = false;
    for (let i = 0; i < text.length; i++) {
        const c = text[i];
        if (quoted && c === "\\") {
            cur += c + (text[i + 1] ?? "");
            i++;
        } else if (c === '"') {
            quoted = !quoted;
            cur += c;
        } else if (c === "," && !quoted) {
            out.push(cur.trim());
            cur = "";
        } else cur += c;
    }
    if (quoted) return null;
    out.push(cur.trim());
    return out;
}

const NUMBER = /^-?\d+(?:\.\d+)?$/;
const WORD = /^[A-Za-z_][A-Za-z0-9_]*$/;
const INT = /^\d+$/;
const STRING = /^"([^"\\]*)"$/;
const PARAM_SHAPE = [WORD, INT, WORD, NUMBER, NUMBER, NUMBER, NUMBER, WORD, INT, WORD, STRING];
const CHOICE_SHAPE = [WORD, INT, WORD, STRING];

/** 解析 ParamTable.h：参数行与档位行，按出现的先后。形状不对的行记 table-call，不进结果。 */
export function parseTable(text, f) {
    const params = [];
    const choices = [];
    const lines = stripCpp(text).split(/\r?\n/);
    lines.forEach((line, i) => {
        if (/^\s*#/.test(line) || !/\bRINNE_(?:PARAM|CHOICE)\b/.test(line)) return;
        const where = `${TABLE}:${i + 1}`;
        const m = /^\s*RINNE_(PARAM|CHOICE)\((.*)\)\s*$/.exec(line);
        const args = m && splitArgs(m[2]);
        const shape = /\bRINNE_PARAM\b/.test(line) ? PARAM_SHAPE : CHOICE_SHAPE;
        if (!args || args.length !== shape.length || args.some((a, k) => !shape[k].test(a))) {
            f.add("table-call", `${where} 不是一行一个、${shape.length} 个字面量参数的宏调用：${line.trim()}`);
            return;
        }
        const str = (a) => STRING.exec(a)[1];
        if (m[1] === "PARAM") {
            const [id, index, kind, lo, hi, def, step, skew, automatable, unit, name] = args;
            params.push({
                where,
                id,
                index: Number(index),
                kind,
                lo: Number(lo),
                hi: Number(hi),
                def: Number(def),
                step: Number(step),
                skew,
                automatable,
                unit,
                name: str(name),
            });
        } else {
            const [id, index, bridge, label] = args;
            choices.push({ where, id, index: Number(index), bridge, label: str(label) });
        }
    });
    return { params, choices };
}

const onGrid = (v, lo, step) => {
    const k = (v - lo) / step;
    return Math.abs(k - Math.round(k)) < 1e-6;
};

function checkTable({ params, choices }, f) {
    const ids = new Set();
    params.forEach((p, i) => {
        if (p.index !== i) f.add("table-index", `${p.where} ${p.id} 的序号是 ${p.index}，按行应为 ${i}`);
        if (ids.has(p.id)) f.add("table-unique", `${p.where} 参数 ID ${p.id} 重名`);
        ids.add(p.id);
        const bad = (msg) => f.add("table-value", `${p.where} ${p.id}：${msg}`);
        if (!/^[a-z][a-z0-9_]*$/.test(p.id)) bad("ID 只能是小写 ASCII、数字与下划线，字母开头");
        if (!Object.hasOwn(KINDS, p.kind)) bad(`类型 ${p.kind} 不认识（F、B、C）`);
        if (!Object.hasOwn(SKEWS, p.skew)) bad(`刻度 ${p.skew} 不认识（lin、log）`);
        if (!Object.hasOwn(UNITS, p.unit)) bad(`单位 ${p.unit} 不认识（${Object.keys(UNITS).join("、")}）`);
        if (p.automatable !== "0" && p.automatable !== "1") bad(`可自动化只能写 0 或 1`);
        if (!(p.lo < p.hi)) bad(`范围 ${p.lo} … ${p.hi} 为空`);
        if (!(p.step > 0)) bad(`步进 ${p.step} 不为正`);
        if (!(p.def >= p.lo && p.def <= p.hi)) bad(`默认 ${p.def} 不在范围 ${p.lo} … ${p.hi} 内`);
        if (p.skew === "log" && !(p.lo > 0)) bad("对数刻度的下限必须为正");
        if (p.kind !== "F" && (p.lo !== 0 || p.step !== 1 || p.skew !== "lin" || p.unit !== "none"))
            bad("开关与档位取 0 起、步进 1、线性、无单位");
        if (p.kind === "B" && p.hi !== 1) bad("开关的上限是 1");
        if (p.step > 0 && !(onGrid(p.def, p.lo, p.step) && onGrid(p.hi, p.lo, p.step)))
            f.add("table-grid", `${p.where} ${p.id} 的默认 ${p.def} 或上限 ${p.hi} 不在 ${p.lo} + k·${p.step} 上`);
    });
    const byId = new Map(params.map((p) => [p.id, p]));
    const perParam = new Map();
    const orphans = new Map();
    for (const c of choices) {
        const p = byId.get(c.id);
        if (!p || p.kind !== "C") {
            orphans.set(c.id, [...(orphans.get(c.id) ?? []), c.where]);
            continue;
        }
        const list = perParam.get(c.id) ?? [];
        if (c.index !== list.length)
            f.add("table-index", `${c.where} ${c.id} 的档位序号是 ${c.index}，按行应为 ${list.length}`);
        if (list.some((x) => x.bridge === c.bridge || x.label === c.label))
            f.add("table-unique", `${c.where} ${c.id} 的档位 ${c.bridge} / ${c.label} 重名`);
        list.push(c);
        perParam.set(c.id, list);
    }
    for (const [id, where] of orphans)
        f.add("table-choice", `${where[0]} 起有 ${where.length} 个档位行属于 ${id}，它不是档位参数`);
    for (const p of params.filter((x) => x.kind === "C")) {
        const n = (perParam.get(p.id) ?? []).length;
        if (n < 2 || p.hi !== n - 1)
            f.add("table-choice", `${p.where} ${p.id} 有 ${n} 档，上限是 ${p.hi}：至少两档，上限等于档数减一`);
    }
    return perParam;
}

/** 宏表的一行换成 golden 里应有的样子（choices 是这个参数的档位行）。 */
function expectedRow(p, list = []) {
    const row = {
        index: p.index,
        name: p.name,
        kind: KINDS[p.kind],
        min: p.lo,
        max: p.hi,
        default: p.def,
        step: p.step,
        skew: SKEWS[p.skew],
        automatable: p.automatable === "1",
        unit: UNITS[p.unit],
        vst3: hostId(p.id),
        au: hostId(p.id),
    };
    if (p.kind === "C") {
        row.choices = list.map((c) => c.label);
        row.bridgeIds = list.map((c) => c.bridge);
    }
    return row;
}

// ---- golden ----

const isParam = ([k, v]) => !TOP_KEYS.includes(k) && v !== null && typeof v === "object" && !Array.isArray(v);
const goldenParams = (g) => Object.entries(g).filter(isParam);
const listText = (a) => (Array.isArray(a) ? `[${a.map((s) => JSON.stringify(s)).join(", ")}]` : JSON.stringify(a));

/** golden 的规范排版：顶层三个标量各一行，每个参数一行，键序按 ROW_KEYS（档位参数再加 CHOICE_KEYS）。 */
export function renderGolden(g) {
    const top = TOP_KEYS.map((k) => `  "${k}": ${JSON.stringify(g[k])}`);
    const rows = goldenParams(g).map(([id, p]) => {
        const keys = p.kind === "choice" ? [...ROW_KEYS, ...CHOICE_KEYS] : ROW_KEYS;
        return `  ${JSON.stringify(id)}: { ${keys.map((k) => `"${k}": ${listText(p[k])}`).join(", ")} }`;
    });
    return `{\n${[...top, ...rows].join(",\n")}\n}\n`;
}

function checkGolden(text, f) {
    let g;
    try {
        g = JSON.parse(text);
    } catch (e) {
        f.add("golden-json", `${GOLDEN} 不是合法的 JSON：${e.message}`);
        return null;
    }
    if (g === null || typeof g !== "object" || Array.isArray(g)) {
        f.add("golden-json", `${GOLDEN} 顶层不是对象`);
        return null;
    }
    const keys = Object.keys(g);
    const params = goldenParams(g);
    if (keys.slice(0, TOP_KEYS.length).join() !== TOP_KEYS.join() || keys.length !== TOP_KEYS.length + params.length)
        f.add(
            "golden-top",
            `${GOLDEN} 顶层应依次是 ${TOP_KEYS.join("、")}，再是各参数（对象），实际是 ${keys.join("、")}`,
        );
    for (const [id, p] of params) {
        const want = p.kind === "choice" ? [...ROW_KEYS, ...CHOICE_KEYS] : ROW_KEYS;
        if (Object.keys(p).join() !== want.join())
            f.add("golden-keys", `${GOLDEN} 的 ${id} 键序应为 ${want.join("、")}，实际是 ${Object.keys(p).join("、")}`);
    }
    if (text.replace(/\r\n/g, "\n") !== renderGolden(g))
        f.add("golden-canonical", `${GOLDEN} 不是规范排版（每个参数一行、两格缩进、键序固定、文件末尾一个换行）`);
    if (g.status !== "draft" && g.status !== "frozen")
        f.add("golden-status", `${GOLDEN} 的 status 是 ${JSON.stringify(g.status)}，应为 draft 或 frozen`);
    if (g.count !== params.length)
        f.add("golden-count", `${GOLDEN} 的 count 是 ${g.count}，参数有 ${params.length} 个`);
    if (g.versionHint !== VERSION_HINT)
        f.add("frozen-hint", `${GOLDEN} 的 versionHint 是 ${g.versionHint}，冻结为 ${VERSION_HINT}`);
    const seen = { vst3: new Map(), au: new Map() };
    for (const [id, p] of params) {
        for (const col of ["vst3", "au"]) {
            if (p[col] !== hostId(id))
                f.add("host-id", `${GOLDEN} 的 ${id}.${col} 是 ${p[col]}，散列应为 ${hostId(id)}`);
            if (seen[col].has(p[col]))
                f.add("host-id-unique", `${GOLDEN} 的 ${id} 与 ${seen[col].get(p[col])} 的 ${col} 相同`);
            seen[col].set(p[col], id);
        }
    }
    return g;
}

function compareTableGolden(table, perParam, g, f) {
    const params = goldenParams(g);
    const order = (list) => list.join("、");
    if (order(table.params.map((p) => p.id)) !== order(params.map(([id]) => id)))
        f.add(
            "params-match",
            `参数先后不一致：${TABLE} 是 ${order(table.params.map((p) => p.id))}，${GOLDEN} 是 ${order(params.map(([id]) => id))}`,
        );
    const golden = new Map(params);
    for (const p of table.params) {
        const row = golden.get(p.id);
        if (!row) continue;
        const want = expectedRow(p, perParam.get(p.id));
        for (const [k, v] of Object.entries(want)) {
            if (k === "vst3" || k === "au") continue; // 散列由 host-id 单独核
            if (JSON.stringify(row[k]) !== JSON.stringify(v))
                f.add(
                    "params-match",
                    `${p.id}.${k}：${TABLE} 是 ${JSON.stringify(v)}，${GOLDEN} 是 ${JSON.stringify(row[k])}`,
                );
        }
    }
}

// ---- 冻结面 ----

function checkFrozen(table, perParam, g, f) {
    const sources = [
        {
            name: TABLE,
            rows: table.params.map((p) => ({
                id: p.id,
                index: p.index,
                kind: p.kind,
                lo: p.lo,
                hi: p.hi,
                skew: p.skew,
                automatable: p.automatable === "1",
                bridges: (perParam.get(p.id) ?? []).map((c) => c.bridge),
            })),
        },
    ];
    if (g)
        sources.push({
            name: GOLDEN,
            rows: goldenParams(g).map(([id, p]) => ({
                id,
                index: p.index,
                kind: Object.keys(KINDS).find((k) => KINDS[k] === p.kind),
                lo: p.min,
                hi: p.max,
                skew: Object.keys(SKEWS).find((k) => SKEWS[k] === p.skew),
                automatable: p.automatable === true,
                bridges: Array.isArray(p.bridgeIds) ? p.bridgeIds : [],
            })),
        });
    for (const { name, rows } of sources) {
        FROZEN.forEach((fz, i) => {
            const r = rows.find((x) => x.id === fz.id);
            if (!r || r.index !== i || r.kind !== fz.kind) {
                const now = r ? `现在是第 ${r.index} 个、类型 ${KINDS[r.kind] ?? r.kind}` : "现在找不到它";
                f.add("frozen-param", `${name}：${fz.id} 冻结为第 ${i} 个、类型 ${KINDS[fz.kind]}，${now}`);
                return;
            }
            if (fz.choices && fz.choices.some((b, k) => r.bridges[k] !== b))
                f.add(
                    "frozen-choice",
                    `${name}：${fz.id} 的已有档位冻结为 ${fz.choices.join("、")}，现在是 ${r.bridges.join("、")}`,
                );
            if (isAutomationFrozen(fz) && !r.automatable)
                f.add("frozen-automation", `${name}：${fz.id} 可自动化，不能改成不可自动化`);
            if (fz.skew !== undefined && (r.lo !== fz.lo || r.hi !== fz.hi || r.skew !== fz.skew))
                f.add(
                    "frozen-automation",
                    `${name}：${fz.id} 的范围与刻度冻结为 ${fz.lo} … ${fz.hi}、${fz.skew}，现在是 ${r.lo} … ${r.hi}、${r.skew}`,
                );
        });
    }
}

// ---- 契约文档 ----

/** marker 之后的第一张 Markdown 表：表头与各行的单元格（去掉首尾空白）。没有 marker 或表返回 null。 */
export function markdownTable(text, marker) {
    const lines = text.split(/\r?\n/);
    const at = lines.findIndex((l) => l.trim() === `<!-- ${marker} -->`);
    if (at < 0) return null;
    let k = at + 1;
    while (k < lines.length && !lines[k].trim()) k++;
    const rows = [];
    for (; k < lines.length && /^\s*\|/.test(lines[k]); k++)
        rows.push(
            lines[k]
                .trim()
                .replace(/^\||\|$/g, "")
                .split("|")
                .map((c) => c.trim()),
        );
    if (rows.length < 2 || !rows[1].every((c) => /^:?-+:?$/.test(c))) return null;
    return { header: rows[0], rows: rows.slice(2) };
}

const unquote = (c) => c.replace(/^`(.*)`$/, "$1");
const withUnit = (v, unit) => `${v}${unit ? ` ${unit}` : ""}`;

/** golden 的一个参数在契约文档参数表里应有的那一行（「中文名」一栏不核，给空串占位）。 */
function docRow(id, p) {
    const choice = p.kind === "choice";
    const float = p.kind === "float";
    const def = choice
        ? (p.choices?.[p.default] ?? "?")
        : p.kind === "bool"
          ? p.default
              ? "开"
              : "关"
          : withUnit(p.default, p.unit);
    return [
        String(p.index),
        id,
        "",
        p.name,
        DOC_KIND[p.kind] ?? "?",
        float ? `${p.min} … ${withUnit(p.max, p.unit)}` : choice ? `${p.choices?.length ?? 0} 档` : "关 / 开",
        def,
        float ? withUnit(p.step, p.unit) : "—",
        float ? (DOC_SKEW[p.skew] ?? "?") : "—",
        p.automatable ? "是" : "否",
        p.vst3 === p.au ? String(p.vst3) : `${p.vst3} / ${p.au}`,
    ];
}

function checkDoc(text, g, f) {
    const status = parseStatus(text);
    if (status === "invalid") f.add("doc-status", `${DOC} 的 status 行读不出 draft 或 frozen`);
    if (!g) return;
    if (status !== "invalid" && g.status !== status)
        f.add(
            "golden-status-doc",
            `${GOLDEN} 的 status 是 ${JSON.stringify(g.status)}，${DOC} 的 status 行是 ${status}：两处要一致`,
        );
    const params = goldenParams(g);
    const table = markdownTable(text, "table:params");
    if (!table || table.header.join("|") !== DOC_PARAM_HEADER.join("|")) {
        f.add(
            "doc-table",
            `${DOC} 缺参数表：<!-- table:params --> 之后要有表头为 ${DOC_PARAM_HEADER.join(" | ")} 的表`,
        );
    } else {
        if (table.rows.length !== params.length)
            f.add("doc-table", `${DOC} 的参数表有 ${table.rows.length} 行，golden 有 ${params.length} 个参数`);
        params.forEach(([id, p], i) => {
            const got = table.rows[i]?.map(unquote);
            const want = docRow(id, p);
            if (!got) return;
            want.forEach((cell, k) => {
                if (k === 2) return; // 中文名归界面文案，不核
                if (got[k] !== cell)
                    f.add(
                        "doc-table",
                        `${DOC} 参数表第 ${i + 1} 行「${DOC_PARAM_HEADER[k]}」是「${got[k] ?? ""}」，按 golden 应为「${cell}」`,
                    );
            });
        });
    }
    for (const [id, p] of params.filter(([, x]) => x.kind === "choice")) {
        const marker = `table:choices:${id}`;
        const t = markdownTable(text, marker);
        if (!t || DOC_CHOICE_HEADER.some((h, k) => t.header[k] !== h)) {
            f.add(
                "doc-choices",
                `${DOC} 缺 ${id} 的档位表：<!-- ${marker} --> 之后要有前三列为 ${DOC_CHOICE_HEADER.join(" | ")} 的表`,
            );
            continue;
        }
        const labels = p.choices ?? [];
        const bridges = p.bridgeIds ?? [];
        if (t.rows.length !== labels.length)
            f.add("doc-choices", `${DOC} 的 ${id} 档位表有 ${t.rows.length} 行，golden 有 ${labels.length} 档`);
        t.rows.forEach((r, i) => {
            const want = [String(i), bridges[i], labels[i]];
            if (want.some((c, k) => unquote(r[k] ?? "") !== c))
                f.add(
                    "doc-choices",
                    `${DOC} 的 ${id} 档位表第 ${i + 1} 行是「${r.slice(0, 3).join(" | ")}」，按 golden 应为「${want.join(" | ")}」`,
                );
        });
    }
}

/** inputs：{ tableText, goldenText, docText }，缺的为 null。返回报错行。 */
export function checkParams({ tableText, goldenText, docText }) {
    const f = new Findings();
    const missing = [
        [TABLE, tableText],
        [GOLDEN, goldenText],
        [DOC, docText],
    ].filter(([, t]) => t === null);
    for (const [p] of missing) f.add("input-missing", `缺 ${p}`);
    if (missing.length) return f.lines;
    const table = parseTable(tableText, f);
    const perParam = checkTable(table, f);
    const g = checkGolden(goldenText, f);
    if (g) compareTableGolden(table, perParam, g, f);
    checkFrozen(table, perParam, g, f);
    checkDoc(docText, g, f);
    return f.lines;
}

// ---- 运行、自测与变异 ----

const read = (root, rel) =>
    fs.existsSync(path.join(root, rel)) ? fs.readFileSync(path.join(root, rel), "utf8") : null;
const inputsOf = (root) => ({ tableText: read(root, TABLE), goldenText: read(root, GOLDEN), docText: read(root, DOC) });

/** 把 text 里的第一处 from 换成 to；没换到就抛错，免得自测与变异因真实文件变了而空转。 */
function edit(text, from, to) {
    if (!text.includes(from)) throw new Error(`夹具里没有「${from}」`);
    return text.replace(from, to);
}
const edits = (text, pairs) => pairs.reduce((t, [from, to]) => edit(t, from, to), text);

/** 宏表里某个参数那一行。 */
function paramLine(text, id) {
    const line = text.split("\n").find((l) => l.startsWith(`RINNE_PARAM(${id}, `));
    if (!line) throw new Error(`夹具里没有 ${id} 的参数行`);
    return line;
}
/** 宏表里某个参数那一行的第 k 个参数换成 value。 */
function tableArg(text, id, k, value) {
    const line = paramLine(text, id);
    const args = splitArgs(/\((.*)\)/.exec(line)[1]);
    args[k] = value;
    return edit(text, line, `RINNE_PARAM(${args.join(", ")})`);
}
/** 宏表里调换两个档位行的桥 id 与文字（序号不动）。 */
function swapChoices(text, id, a, b) {
    const prefix = (i) => `RINNE_CHOICE(${id}, ${i}, `;
    const lineOf = (i) => text.split("\n").find((l) => l.startsWith(prefix(i)));
    const [la, lb] = [lineOf(a), lineOf(b)];
    if (!la || !lb) throw new Error(`夹具里没有 ${id} 的第 ${a}、${b} 档`);
    const [ta, tb] = [la.slice(prefix(a).length), lb.slice(prefix(b).length)];
    return edit(edit(text, la, prefix(a) + tb), lb, prefix(b) + ta);
}

export function selfTest(root) {
    const base = inputsOf(root);
    const missing = Object.entries(base).filter(([, v]) => v === null);
    if (missing.length)
        return { ok: false, bad: 0, good: 0, lines: [`自测要读真实文件，缺 ${missing.map(([k]) => k).join("、")}`] };
    const run = (over) => checkParams({ ...base, ...over });
    const T = base.tableText;
    const G = base.goldenText;
    const D = base.docText;
    const goldenWith = (mutate) => {
        const g = JSON.parse(G);
        mutate(g);
        return renderGolden(g);
    };
    /** golden 里把参数 from 改名为 to（键与两列散列一起改，键序不变）。 */
    const renamed = (g, from, to) => {
        const out = {};
        for (const [k, v] of Object.entries(g))
            out[k === from ? to : k] = k === from ? { ...v, vst3: hostId(to), au: hostId(to) } : v;
        return out;
    };
    const goldenRenamed = (from, to) => renderGolden(renamed(JSON.parse(G), from, to));
    const swapNoteGolden = (g) => {
        const n = g.delay_note;
        [n.choices[5], n.choices[6]] = [n.choices[6], n.choices[5]];
        [n.bridgeIds[5], n.bridgeIds[6]] = [n.bridgeIds[6], n.bridgeIds[5]];
    };
    /** golden 顶层 status 那一行（不带行尾逗号），不假定它现在是 draft 还是 frozen。 */
    const statusLine = `  "status": ${JSON.stringify(JSON.parse(G).status)}`;
    /** 契约文档 status 行换成另一个合法值，golden 不动。 */
    const otherStatus = parseStatus(D) === "frozen" ? "draft" : "frozen";
    const noteRow = (i) => {
        const g = JSON.parse(G);
        return `| ${i} | \`${g.delay_note.bridgeIds[i]}\` | ${g.delay_note.choices[i]} |`;
    };
    // [说明, 必须触发的判据（空 = 正例，必须一条都不报）, 报错行]
    const cases = [
        ["真实文件一致", [], run({})],
        ["缺参数表", ["input-missing"], run({ tableText: null })],
        ["缺 golden", ["input-missing"], run({ goldenText: null })],
        ["缺契约文档", ["input-missing"], run({ docText: null })],
        [
            "宏调用拆成两行",
            ["table-call"],
            run({ tableText: edit(T, paramLine(T, "mix"), paramLine(T, "mix").replace("F, ", "F,\n    ")) }),
        ],
        ["宏参数写成表达式", ["table-call"], run({ tableText: tableArg(T, "mix", 5, "25.0 + 0") })],
        ["宏参数少一个", ["table-call"], run({ tableText: edit(T, ', pct, "Mix")', ', "Mix")') })],
        ["参数序号跳号", ["table-index"], run({ tableText: tableArg(T, "loop_on", 1, "7") })],
        [
            "档位序号跳号",
            ["table-index"],
            run({ tableText: edit(T, "RINNE_CHOICE(delay_note, 3, ", "RINNE_CHOICE(delay_note, 4, ") }),
        ],
        ["参数 ID 重名", ["table-unique"], run({ tableText: tableArg(T, "polarity", 0, "mix") })],
        [
            "档位桥 id 重名",
            ["table-unique"],
            run({ tableText: edit(T, "RINNE_CHOICE(delay_mode, 1, sync,", "RINNE_CHOICE(delay_mode, 1, time,") }),
        ],
        ["类型写成不认识的字母", ["table-value"], run({ tableText: tableArg(T, "mix", 2, "X") })],
        ["默认值越出范围", ["table-value"], run({ tableText: tableArg(T, "ceiling", 5, "1.0") })],
        ["对数刻度下限为 0", ["table-value"], run({ tableText: tableArg(T, "mix", 7, "log") })],
        ["开关的上限不是 1", ["table-value"], run({ tableText: tableArg(T, "polarity", 4, "2") })],
        ["默认值不在步进格上", ["table-grid"], run({ tableText: tableArg(T, "delay_ms", 5, "100.0005") })],
        ["档位行挂在连续参数上", ["table-choice"], run({ tableText: `${T}\nRINNE_CHOICE(mix, 0, a, "A")\n` })],
        ["档数与上限对不上", ["table-choice"], run({ tableText: tableArg(T, "delay_mode", 4, "2") })],
        ["golden 不是 JSON", ["golden-json"], run({ goldenText: "{" })],
        ["golden 多一个空格", ["golden-canonical"], run({ goldenText: edit(G, '"index": 2,', '"index":  2,') })],
        [
            "golden 顶层键换了顺序",
            ["golden-top"],
            run({
                goldenText: edit(G, `${statusLine},\n  "versionHint": 1,`, `  "versionHint": 1,\n${statusLine},`),
            }),
        ],
        [
            "golden 参数的键序变了",
            ["golden-keys"],
            run({ goldenText: edit(G, '{ "index": 2, "name": "Mix",', '{ "name": "Mix", "index": 2,') }),
        ],
        ["golden 的 status 拼错", ["golden-status"], run({ goldenText: goldenWith((g) => (g.status = "drafty")) })],
        ["golden 的 count 不对", ["golden-count"], run({ goldenText: goldenWith((g) => (g.count += 1)) })],
        ["golden 改了一个默认值", ["params-match"], run({ goldenText: goldenWith((g) => (g.mix.default = 30)) })],
        [
            "golden 调换两行",
            ["params-match"],
            run({ goldenText: renderGolden(swapKeys(JSON.parse(G), "loop_on", "mix")) }),
        ],
        ["golden 的 vst3 列差 1", ["host-id"], run({ goldenText: goldenWith((g) => (g.feedback.vst3 += 1)) })],
        [
            "golden 两个参数的 au 列相同",
            ["host-id-unique"],
            run({ goldenText: goldenWith((g) => (g.mix.au = g.loop_on.au)) }),
        ],
        ["契约文档的 status 行坏了", ["doc-status"], run({ docText: edit(D, "status: frozen", "status: frozzen") })],
        [
            "golden 的 status 与契约文档不一致（golden 改了）",
            ["golden-status-doc"],
            run({ goldenText: goldenWith((g) => (g.status = otherStatus)) }),
        ],
        [
            "golden 的 status 与契约文档不一致（契约文档改了）",
            ["golden-status-doc"],
            run({ docText: edit(D, `status: ${parseStatus(D)}`, `status: ${otherStatus}`) }),
        ],
        ["契约文档改了一个默认值", ["doc-table"], run({ docText: edit(D, "| 25 % |", "| 30 % |") })],
        ["契约文档少了参数表", ["doc-table"], run({ docText: edit(D, "<!-- table:params -->", "") })],
        [
            "契约文档调换两档",
            ["doc-choices"],
            run({
                docText: edits(D, [
                    [noteRow(5), "@@"],
                    [noteRow(6), noteRow(5)],
                    ["@@", noteRow(6)],
                ]),
            }),
        ],
        ["契约文档少了档位表", ["doc-choices"], run({ docText: edit(D, "<!-- table:choices:delay_mode -->", "") })],
        [
            "宏表与 golden 一起改一个 ID",
            ["frozen-param"],
            run({ tableText: tableArg(T, "mix", 0, "mix_amount"), goldenText: goldenRenamed("mix", "mix_amount") }),
        ],
        [
            "宏表与 golden 一起改一个类型",
            ["frozen-param"],
            run({
                tableText: tableArg(
                    tableArg(tableArg(tableArg(T, "ceiling", 2, "C"), "ceiling", 3, "0"), "ceiling", 6, "1"),
                    "ceiling",
                    4,
                    "1",
                ),
                goldenText: goldenWith((g) => Object.assign(g.ceiling, { kind: "choice" })),
            }),
        ],
        ["golden 的 versionHint 改成 2", ["frozen-hint"], run({ goldenText: goldenWith((g) => (g.versionHint = 2)) })],
        [
            "宏表与 golden 一起调换两档时值",
            ["frozen-choice"],
            run({ tableText: swapChoices(T, "delay_note", 5, 6), goldenText: goldenWith(swapNoteGolden) }),
        ],
        [
            "宏表与 golden 一起把可自动化参数改成不可自动化",
            ["frozen-automation"],
            run({
                tableText: tableArg(T, "polarity", 8, "0"),
                goldenText: goldenWith((g) => (g.polarity.automatable = false)),
            }),
        ],
        [
            "宏表与 golden 一起改可自动化参数的范围",
            ["frozen-automation"],
            run({ tableText: tableArg(T, "low_cut", 3, "10.0"), goldenText: goldenWith((g) => (g.low_cut.min = 10)) }),
        ],
        [
            "宏表、golden 与契约文档一起改默认值：不在这里拦，由契约守卫要变更文档",
            [],
            run({
                tableText: tableArg(T, "mix", 5, "30.0"),
                goldenText: goldenWith((g) => (g.mix.default = 30)),
                docText: edit(D, "| 25 % |", "| 30 % |"),
            }),
        ],
        [
            "宏表与 golden 一起把不可自动化参数改成可自动化：不在这里拦",
            [],
            run({
                tableText: tableArg(T, "limiter", 8, "1"),
                goldenText: goldenWith((g) => (g.limiter.automatable = true)),
                docText: edit(
                    D,
                    "| Return Limiter | 开关 | 关 / 开 | 开 | — | — | 否 |",
                    "| Return Limiter | 开关 | 关 / 开 | 开 | — | — | 是 |",
                ),
            }),
        ],
        [
            "三处一起在末尾追加一档时值：允许",
            [],
            run({
                tableText: edit(
                    tableArg(T, "delay_note", 4, "16"),
                    'RINNE_CHOICE(delay_note, 15, n1, "1/1")',
                    'RINNE_CHOICE(delay_note, 15, n1, "1/1")\nRINNE_CHOICE(delay_note, 16, n1d, "1/1D")',
                ),
                goldenText: goldenWith((g) => {
                    g.delay_note.max = 16;
                    g.delay_note.choices.push("1/1D");
                    g.delay_note.bridgeIds.push("n1d");
                }),
                docText: edits(D, [
                    ["| 档位 | 16 档 |", "| 档位 | 17 档 |"],
                    [noteRow(15), `${noteRow(15)}\n| 16 | \`n1d\` | 1/1D |`],
                ]),
            }),
        ],
    ].map(([label, expect, errors]) => {
        const missed = expect.filter((r) => !errors.some((e) => e.startsWith(`[${r}] `)));
        const passed = expect.length ? missed.length === 0 : errors.length === 0;
        const why = missed.length
            ? `（没有触发 ${missed.join("、")}）`
            : !passed
              ? `（报了：${errors.join("；")}）`
              : "";
        return { label: `${label}${why}`, bad: !!expect.length, passed, expect };
    });
    const unnamed = Object.keys(RULES).filter((r) => !cases.some((c) => c.expect.includes(r)));
    cases.push({
        label: `每条判据都有坏样例点名${unnamed.length ? `（没有：${unnamed.join("、")}）` : ""}`,
        bad: false,
        passed: unnamed.length === 0,
    });
    // 散列的已知答案：按 JUCE 的算法另行算出，不是从 golden 抄的
    cases.push({
        label: "散列的已知答案",
        bad: false,
        passed: hostId("a") === 97 && hostId("mix") === 108124 && hostId("feedback") === 1955982213,
    });
    return caseResult(cases);
}

/** golden 里调换两个参数的先后（各自的内容不变）。 */
function swapKeys(g, a, b) {
    const out = {};
    for (const k of Object.keys(g)) {
        const key = k === a ? b : k === b ? a : k;
        out[key] = g[key];
    }
    return out;
}

export default {
    name: "contract-static-params",
    group: "docs",
    inputs: [],
    run(ctx) {
        const errors = checkParams(inputsOf(ctx.root));
        if (errors.some((e) => e.startsWith("[frozen-")))
            errors.push(
                `冻结面写死在 ${SELF} 的 FROZEN 里。参数 ID 任何时候都不改；其余各项正式发布之前确要改，同一个 PR 改 FROZEN 并附变更文档，见 ${DOC} 的冻结规则`,
            );
        return { ok: errors.length === 0, lines: errors };
    },
    selfTest: (ctx) => selfTest(ctx.root),
    /** 在真实文件上做点名的三种改动（另加 golden 改默认值、契约文档改默认值、golden 的 status 与契约文档不一致三种），门禁都必须变红。 */
    mutationCheck(ctx) {
        const base = inputsOf(ctx.root);
        const lines = [];
        const red = (label, over) => {
            if (!checkParams({ ...base, ...over }).length) lines.push(`${label}：门禁没有变红`);
        };
        const T = base.tableText;
        red("参数表改一个默认值（mix 25 → 30）", { tableText: tableArg(T, "mix", 5, "30.0") });
        red("golden 改一个默认值（feedback 0 → 5）", {
            goldenText: renderGolden({
                ...JSON.parse(base.goldenText),
                feedback: { ...JSON.parse(base.goldenText).feedback, default: 5 },
            }),
        });
        red("参数表改一个 ID（delay_note → sync_note）", { tableText: tableArg(T, "delay_note", 0, "sync_note") });
        red("参数表调换两档时值（1/8T 与 1/16D）", { tableText: swapChoices(T, "delay_note", 5, 6) });
        red("契约文档改一个默认值（mix 25 % → 30 %）", { docText: edit(base.docText, "| 25 % |", "| 30 % |") });
        const golden = JSON.parse(base.goldenText);
        red(`golden 的 status 改成与契约文档不一致（${golden.status} → 另一个合法值）`, {
            goldenText: renderGolden({ ...golden, status: golden.status === "frozen" ? "draft" : "frozen" }),
        });
        return { ok: lines.length === 0, lines };
    },
};
