// SPDX-License-Identifier: GPL-3.0-or-later
//
// 共享内存契约的静态对拍：不编译，直接读 src/core/ipc/SegmentLayout.h 的字段表、结构表与常量，和 Naming.h 的前缀族、
// 根 CMakeLists.txt 的三个段名前缀，与 tests/golden/ipc-layout.json 逐项比对。编译后的那一遍在 rinne_core_tests 里
// （同一张字段表展开成 offsetof），两遍都要绿。比对的内容：
//   - 字段：结构、字段名、偏移、字节数、类型、是否永久冻结，顺序也要一致；同一结构的字段首尾相接、不重叠不留缝，
//     最后一项收在结构末尾；u64 字段的偏移是 8 的倍数，u32 是 4 的倍数；
//   - 结构的大小与对齐；常量（头文件里每个 inline constexpr 都要进 golden，值只能写字面量或 util::fourcc("ABCD")）；
//   - 永久冻结面（本文件写死，不随 golden 改，头文件与 golden 各核一遍）：会合段头在段首、它的前 16 字节与 init_pid，
//     端点槽 version 的绝对位置与它依赖的组块偏移、组块与端点槽的结构大小、步长，会合段的 magic 与初始化标记，
//     会合段大小（kRvBytes 与 RvSegment 结构）的下限，RvSegment 结构的大小等于 kRvBytes；
//   - 段名：三族前缀 × 9 段 × 2 平台 × {正式, 测试} 由本文件按规则重新拼一遍，与 golden 逐条相等，每条不超过 31 字符；
//     abi 涨到 golden 的 nameAbiHeadroom（至少 99）时每条也不超过 31；前缀族与根 CMakeLists.txt 允许的三个值一一对应；
//     会合段名不带 abi；
//   - golden 的排版是规范形：每条记录一行、键序固定（不是规范形就判红，免得两行之间藏一处手改）；
//   - 发布代码不读环境变量（src/** 里出现 getenv 一族判红，注释与字符串里的不算）；测试钩子（名字以 ForTesting 结尾）
//     只出现在声明它们的文件里，插件与其他 core 代码不许调用。
// 每条判据有一个名字（RULES），报错行以 [名字] 开头。--self-test 的每个坏样例写明它必须触发哪几条判据，并核对每条
// 判据都至少被一个坏样例点名：删掉任何一条判据，点名它的坏样例就变红。冻结面清单与读环境写法表的每一项也各有一个
// 只动这一项的坏样例，删掉任何一项同样变红。
// 有意改布局是兼容性事件：kIpcAbi + 1，按 CLAUDE.md 的「冻结契约」一节走变更文档，并同一个 PR 更新 golden。
//   node scripts/run-checks.mjs --only contract-static-ipc [--self-test | --mutation-check]

import fs from "node:fs";
import path from "node:path";
import { caseResult } from "../lib/check-kit.mjs";

const LAYOUT = "src/core/ipc/SegmentLayout.h";
const NAMING = "src/core/ipc/Naming.h";
const GOLDEN = "tests/golden/ipc-layout.json";
const ROOT_CMAKE = "CMakeLists.txt";

const PLATFORMS = ["win32", "posix"];
const SCOPES = ["formal", "testing"];
const SEGMENTS = ["rv", "g1", "g2", "g3", "g4", "g5", "g6", "g7", "g8"];
const UID = "<uid8>";
const TAG = "<tag7>";
/** 段名给 abi 留的增长空间：abi 到这个值时所有段名仍不超过 31 字符。只许调大。 */
const MIN_ABI_HEADROOM = 99;

/** 永久冻结的字段：连 abi 升级都不许动。端点槽 version 的绝对位置 = 1024·G + {0, 256} + 56。 */
const PERMANENT = [
    { struct: "RvHeader", field: "magic", offset: 0, size: 4, type: "u32" },
    { struct: "RvHeader", field: "abi", offset: 4, size: 4, type: "u32" },
    { struct: "RvHeader", field: "generation", offset: 8, size: 4, type: "u32" },
    { struct: "RvHeader", field: "layout_bytes", offset: 12, size: 4, type: "u32" },
    { struct: "RvHeader", field: "init_pid", offset: 32, size: 4, type: "u32" },
    { struct: "RoleSlot", field: "version", offset: 56, size: 4, type: "u32" },
];
/** 永久冻结字段的绝对位置所依赖的结构内偏移。 */
const ANCHORS = [
    { struct: "RvSegment", field: "header", offset: 0 },
    { struct: "RvSegment", field: "groups", offset: 1024 },
    { struct: "GroupBlock", field: "input", offset: 0 },
    { struct: "GroupBlock", field: "output", offset: 256 },
];
/** 永久冻结的结构大小：组块的大小就是步长，端点槽的大小决定 Output 槽的位置。 */
const PERMANENT_STRUCTS = [
    { struct: "GroupBlock", size: 1024 },
    { struct: "RoleSlot", size: 256 },
];
const PERMANENT_CONSTANTS = {
    kRvMagic: fourcc("RNRV"),
    kInitMarker: fourcc("RNIN"),
    kGroupStride: 1024,
    kRoleSlotBytes: 256,
    kRvMinBytes: 16384,
};

/** 判据名 → 说明。报错行以 [名字] 开头；自测按名字核对。 */
export const RULES = {
    "input-missing": "三份源文件与 golden 都要在",
    "golden-json": "golden 是合法的 JSON",
    "golden-top": "golden 的顶层键依次固定",
    "golden-keys": "golden 每条记录的键序固定",
    "golden-canonical": "golden 是规范排版",
    "layout-macro": "头文件里有字段表、结构表、常量表三个宏",
    "constant-literal": "常量只写字面量或 util::fourcc",
    "field-align": "u32 / u64 字段的偏移对齐",
    "field-type-size": "u32 / u64 字段的字节数等于类型大小",
    "field-contiguous": "同一结构的字段首尾相接、不重叠不留缝",
    "field-fills": "同一结构的最后一个字段收在结构末尾",
    "struct-empty": "结构表里的结构在字段表里有字段",
    "struct-unlisted": "字段表里的结构都在结构表里",
    "fields-match": "头文件的字段表与 golden 逐行相同",
    "structs-match": "头文件的结构表与 golden 逐行相同",
    "constant-unlisted": "头文件声明的常量都在常量表里",
    "constant-undeclared": "常量表里的常量都有声明",
    "constants-match": "头文件的常量与 golden 逐行相同",
    "abi-match": "golden 的 abi 等于 kIpcAbi",
    "max-name-length": "golden 的 maxNameLength 等于 kMaxSegmentNameLength 且是 31",
    "name-headroom": "golden 的 nameAbiHeadroom 不小于 99",
    "permanent-field": "永久冻结的字段在固定的位置、标为永久",
    "permanent-extra": "只有永久冻结面标为永久",
    "permanent-anchor": "永久冻结字段的绝对位置依赖的偏移不动",
    "permanent-constant": "永久冻结的常量不变",
    "permanent-struct": "组块与端点槽的结构大小不变",
    "rv-floor": "会合段（kRvBytes 与 RvSegment 结构）不小于永久冻结的下限",
    "rv-struct": "RvSegment 结构的大小等于 kRvBytes",
    "families-missing": "Naming.h 里有 kPrefixFamilies",
    "cmake-missing": "根 CMakeLists.txt 里有三个段名前缀",
    "families-cmake": "前缀族与 CMake 允许的值一一对应",
    "prefixes-match": "前缀族与 golden 逐行相同",
    "name-length": "段名不超过 31 字符",
    "rv-name-abi": "会合段名不带 abi",
    "name-growth": "abi 涨到 nameAbiHeadroom 时段名仍不超过 31 字符",
    "names-match": "按规则拼出的段名与 golden 逐条相同",
    "golden-name-length": "golden 记的段名长度与名字一致、不超过上限",
    "env-read": "发布代码不读环境变量",
    "test-hook": "测试钩子只出现在声明它的文件里",
};

const KEYS = {
    constants: ["constant", "value"],
    structs: ["struct", "size", "align"],
    fields: ["struct", "field", "offset", "size", "type", "permanent"],
    prefixes: ["win32", "posix"],
    names: ["platform", "scope", "prefix", "segment", "name", "length"],
};
const TOP = ["abi", "maxNameLength", "nameAbiHeadroom", ...Object.keys(KEYS)];

export function fourcc(code) {
    return [...code].reduce((v, c, i) => (v + c.charCodeAt(0) * 2 ** (8 * i)) >>> 0, 0);
}

/** 报错的收集：每一行以判据名开头。 */
class Findings {
    constructor() {
        this.lines = [];
    }
    add(rule, message) {
        if (!Object.hasOwn(RULES, rule)) throw new Error(`未登记的判据 ${rule}`);
        this.lines.push(`[${rule}] ${message}`);
    }
    has(rule) {
        return this.lines.some((l) => l.startsWith(`[${rule}] `));
    }
}

/** 去掉 C/C++ 的注释；keepStrings 为假时字符串与字符字面量也换成空白。换行保留，行号不变。 */
export function stripCpp(text, { keepStrings = true } = {}) {
    let out = "";
    let i = 0;
    const blank = (s) => s.replace(/[^\n]/g, " ");
    while (i < text.length) {
        const two = text.slice(i, i + 2);
        if (two === "//") {
            const end = text.indexOf("\n", i);
            const stop = end < 0 ? text.length : end;
            out += blank(text.slice(i, stop));
            i = stop;
        } else if (two === "/*") {
            const end = text.indexOf("*/", i + 2);
            const stop = end < 0 ? text.length : end + 2;
            out += blank(text.slice(i, stop));
            i = stop;
        } else if (text[i] === '"' || text[i] === "'") {
            const raw = text[i] === '"' && /R$/.test(text.slice(Math.max(0, i - 1), i));
            let stop;
            if (raw) {
                const open = text.indexOf("(", i);
                const delim = text.slice(i + 1, open);
                const end = text.indexOf(`)${delim}"`, open);
                stop = end < 0 ? text.length : end + delim.length + 2;
            } else {
                let k = i + 1;
                while (k < text.length && text[k] !== text[i] && text[k] !== "\n") k += text[k] === "\\" ? 2 : 1;
                stop = Math.min(text.length, k + 1);
            }
            out += keepStrings ? text.slice(i, stop) : text[i] + blank(text.slice(i + 1, stop - 1)) + text[stop - 1];
            i = stop;
        } else {
            out += text[i++];
        }
    }
    return out;
}

/** 取一个以反斜杠续行的宏定义体（不含 #define 那一行的宏名）。 */
function macroBody(text, name) {
    const lines = text.split(/\r?\n/);
    const start = lines.findIndex((l) => new RegExp(`^\\s*#\\s*define\\s+${name}\\(X\\)`).test(l));
    if (start < 0) return null;
    const body = [];
    for (let k = start; k < lines.length; k++) {
        body.push(lines[k]);
        if (!/\\\s*$/.test(lines[k])) break;
    }
    return body.join("\n").replace(new RegExp(`^\\s*#\\s*define\\s+${name}\\(X\\)`), "");
}

/** 解析 SegmentLayout.h：字段表、结构表、常量名单与常量值。errors 是 [{ rule, message }]。 */
export function parseLayout(text) {
    const errors = [];
    const code = stripCpp(text);
    const body = (name) => {
        const b = macroBody(code, name);
        if (b === null) errors.push({ rule: "layout-macro", message: `${LAYOUT} 里找不到宏 ${name}` });
        return b ?? "";
    };
    const fields = [
        ...body("RINNE_IPC_LAYOUT_FIELDS").matchAll(
            /X\(\s*(\w+)\s*,\s*(\w+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*"([^"]+)"\s*,\s*(true|false)\s*\)/g,
        ),
    ].map((m) => ({
        struct: m[1],
        field: m[2],
        offset: Number(m[3]),
        size: Number(m[4]),
        type: m[5],
        permanent: m[6] === "true",
    }));
    const structs = [...body("RINNE_IPC_LAYOUT_STRUCTS").matchAll(/X\(\s*(\w+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)/g)].map(
        (m) => ({ struct: m[1], size: Number(m[2]), align: Number(m[3]) }),
    );
    const constantNames = [...body("RINNE_IPC_LAYOUT_CONSTANTS").matchAll(/X\(\s*(\w+)\s*\)/g)].map((m) => m[1]);
    const constants = {};
    for (const m of code.matchAll(/inline\s+constexpr\s+[\w:]+\s+(\w+)\s*=\s*([^;]+);/g)) {
        const raw = m[2].trim();
        const fc = /^util::fourcc\("(.{4})"\)$/.exec(raw);
        if (/^\d+$/.test(raw)) constants[m[1]] = Number(raw);
        else if (fc) constants[m[1]] = fourcc(fc[1]);
        else
            errors.push({
                rule: "constant-literal",
                message: `${LAYOUT}：常量 ${m[1]} 只能写字面量或 util::fourcc("ABCD")，现在是 ${raw}`,
            });
    }
    return { fields, structs, constantNames, constants, errors };
}

/** 解析 Naming.h 的 kPrefixFamilies。 */
export function parseFamilies(text) {
    const m = /kPrefixFamilies\s*\[\s*\d*\s*\]\s*=\s*\{([\s\S]*?)\};/.exec(stripCpp(text));
    if (!m) return null;
    return [...m[1].matchAll(/\{\s*"([^"]*)"\s*,\s*"([^"]*)"\s*\}/g)].map((x) => ({ win32: x[1], posix: x[2] }));
}

/** 根 CMakeLists.txt 允许的三个段名前缀：正式、带 D、带 P。 */
export function parseCmakePrefixes(text) {
    const formal = /set\(\s*RINNE_SHM_PREFIX_FORMAL\s+"([^"]+)"\s*\)/.exec(text)?.[1];
    const values = /set\(\s*_rinne_shm_values\s+([^)]*)\)/.exec(text)?.[1];
    if (!formal || !values) return null;
    return [...values.matchAll(/"([^"]*)"/g)].map((v) => v[1].replaceAll("${RINNE_SHM_PREFIX_FORMAL}", formal));
}

/** 按 Naming.cpp 的规则拼名；uid 与测试标签用占位符。 */
export function composeName(platform, family, scope, segment, abi) {
    const head = platform === "win32" ? "Local\\" : "/";
    const testing = scope === "testing";
    const prefix = platform === "win32" && !testing ? family.win32 : family.posix;
    const middle = testing ? `.${TAG}` : platform === "posix" ? `.${UID}` : "";
    const seg = segment === "rv" ? "rv" : `v${abi}.${segment}`;
    return `${head}${prefix}${middle}.${seg}`;
}

/** 段名的真实长度：占位符按它代表的字符数算（uid 8 个、测试标签 7 个）。 */
export const nameLength = (name) => name.replaceAll(UID, "u".repeat(8)).replaceAll(TAG, "t".repeat(7)).length;

/** golden 的规范排版：每条记录一行，键序按 KEYS。 */
export function renderGolden(g) {
    const rec = (obj, keys) => `{ ${keys.map((k) => `${JSON.stringify(k)}: ${JSON.stringify(obj[k])}`).join(", ")} }`;
    const parts = ["abi", "maxNameLength", "nameAbiHeadroom"].map((k) => `  "${k}": ${JSON.stringify(g[k])}`);
    for (const [section, keys] of Object.entries(KEYS)) {
        const rows = (g[section] ?? []).map((r) => `    ${rec(r, keys)}`);
        parts.push(`  "${section}": [\n${rows.join(",\n")}\n  ]`);
    }
    return `{\n${parts.join(",\n")}\n}\n`;
}

/** 发布代码里不许出现的读环境写法（整词）。自测里每一种写法各有一个坏样例，删掉任何一项都有样例变红。 */
const ENV_READ_NAMES = [
    "getenv",
    "_wgetenv",
    "getenv_s",
    "_wgetenv_s",
    "_dupenv_s",
    "_wdupenv_s",
    "secure_getenv",
    "GetEnvironmentVariable[AW]?",
    "GetEnvironmentStrings[AW]?",
    "ExpandEnvironmentStrings[AW]?",
    "getEnvironmentVariable",
    "_NSGetEnviron",
    "__environ",
    "_?w?environ",
];
const ENV_READ = new RegExp(String.raw`\b(?:std::)?(?:${ENV_READ_NAMES.join("|")})\b|processInfo\s*\]\s*environment`);
const SOURCE_EXT = /\.(?:c|cc|cpp|cxx|h|hh|hpp|m|mm)$/;
/** 测试钩子：名字以 ForTesting 结尾的函数、类型与结构；只许出现在声明它们的文件里。 */
const TEST_HOOKS = /\b\w*[Ff]orTesting\b/;
const HOOK_HOMES = new Set([
    ...["Naming", "SegmentBackendPosix", "SegmentBackendInProcess"].flatMap((f) =>
        ["h", "cpp"].map((e) => `src/core/ipc/${f}.${e}`),
    ),
    "src/core/ipc/ISegmentBackend.h",
]);

function scanInto(f, sources) {
    for (const { path: p, text } of sources) {
        if (!p.startsWith("src/") || !SOURCE_EXT.test(p)) continue;
        const lines = stripCpp(text, { keepStrings: false }).split("\n");
        lines.forEach((line, i) => {
            if (ENV_READ.test(line)) f.add("env-read", `${p}:${i + 1}：发布代码不许读环境变量（${line.trim()}）`);
            const hook = HOOK_HOMES.has(p) ? null : TEST_HOOKS.exec(line);
            if (hook) f.add("test-hook", `${p}:${i + 1}：测试钩子 ${hook[0]} 只能出现在声明它的文件里`);
        });
    }
}

/** 发布代码的扫描：环境变量与测试钩子。sources = [{ path, text }]，路径相对仓根。 */
export function scanSources(sources) {
    const f = new Findings();
    scanInto(f, sources);
    return f.lines;
}

function checkFields(f, where, fields, structs) {
    const byStruct = new Map();
    for (const x of fields) {
        if (!byStruct.has(x.struct)) byStruct.set(x.struct, []);
        byStruct.get(x.struct).push(x);
        const align = { u64: 8, u32: 4 }[x.type];
        if (align && x.offset % align !== 0)
            f.add("field-align", `${where}：${x.struct}.${x.field}：${x.type} 的偏移 ${x.offset} 没对齐`);
        if (align && x.size !== align)
            f.add("field-type-size", `${where}：${x.struct}.${x.field}：${x.type} 的大小应为 ${align}`);
    }
    for (const s of structs) {
        const list = byStruct.get(s.struct) ?? [];
        if (!list.length) f.add("struct-empty", `${where}：结构 ${s.struct} 在字段表里没有字段`);
        let at = 0;
        for (const x of list) {
            if (x.offset !== at)
                f.add(
                    "field-contiguous",
                    `${where}：${s.struct}.${x.field} 的偏移是 ${x.offset}，前一项收在 ${at}（不许重叠或留缝）`,
                );
            at = x.offset + x.size;
        }
        if (list.length && at !== s.size)
            f.add("field-fills", `${where}：${s.struct} 的字段收在 ${at}，结构大小是 ${s.size}`);
    }
    for (const name of byStruct.keys())
        if (!structs.some((s) => s.struct === name))
            f.add("struct-unlisted", `${where}：字段表里的结构 ${name} 不在结构表里`);
}

function sameList(f, rule, label, a, b, keys) {
    const key = (r) => JSON.stringify(keys.map((k) => r[k]));
    const n = Math.max(a.length, b.length);
    for (let i = 0; i < n; i++) {
        if (!a[i]) f.add(rule, `${label}：golden 多出第 ${i + 1} 项 ${key(b[i])}`);
        else if (!b[i]) f.add(rule, `${label}：golden 缺第 ${i + 1} 项 ${key(a[i])}`);
        else if (key(a[i]) !== key(b[i]))
            f.add(rule, `${label} 第 ${i + 1} 项：头文件 ${key(a[i])}，golden ${key(b[i])}`);
    }
}

/** 永久冻结面：头文件与 golden 各核一遍，取值写死在本文件里。 */
function checkPermanent(f, where, fields, structs, constants) {
    for (const p of PERMANENT) {
        const x = fields.find((y) => y.struct === p.struct && y.field === p.field);
        if (!x || x.offset !== p.offset || x.size !== p.size || x.type !== p.type || !x.permanent)
            f.add(
                "permanent-field",
                `${where}：永久冻结的 ${p.struct}.${p.field} 必须在偏移 ${p.offset}、${p.type}，并标为永久`,
            );
    }
    for (const x of fields)
        if (x.permanent && !PERMANENT.some((p) => p.struct === x.struct && p.field === x.field))
            f.add(
                "permanent-extra",
                `${where}：${x.struct}.${x.field} 标了永久冻结，永久冻结的字段只有 ${PERMANENT.map((p) => `${p.struct}.${p.field}`).join("、")}`,
            );
    for (const a of ANCHORS) {
        const x = fields.find((y) => y.struct === a.struct && y.field === a.field);
        if (!x || x.offset !== a.offset)
            f.add(
                "permanent-anchor",
                `${where}：${a.struct}.${a.field} 必须在 ${a.offset}（永久冻结字段的绝对位置依赖它）`,
            );
    }
    for (const [n, v] of Object.entries(PERMANENT_CONSTANTS))
        if (constants[n] !== v) f.add("permanent-constant", `${where}：${n} 永久是 ${v}`);
    for (const p of PERMANENT_STRUCTS) {
        const s = structs.find((y) => y.struct === p.struct);
        if (!s || s.size !== p.size)
            f.add("permanent-struct", `${where}：${p.struct} 的大小永久是 ${p.size}（version 的绝对位置依赖它）`);
    }
    const floor = PERMANENT_CONSTANTS.kRvMinBytes;
    const rv = structs.find((y) => y.struct === "RvSegment");
    if (!(constants.kRvBytes >= floor))
        f.add("rv-floor", `${where}：kRvBytes ${constants.kRvBytes} 小于永久冻结的下限 ${floor}`);
    if (!(rv?.size >= floor)) f.add("rv-floor", `${where}：RvSegment 的大小 ${rv?.size} 小于永久冻结的下限 ${floor}`);
    if (rv?.size !== constants.kRvBytes)
        f.add("rv-struct", `${where}：RvSegment 的大小 ${rv?.size} 与 kRvBytes ${constants.kRvBytes} 不符`);
}

/**
 * 全部对拍。layoutText / namingText / cmakeText 是三份源文件的文本，goldenText 是 golden 原文，sources 是要扫描的
 * 发布代码（[{ path, text }]）。返回报错行，每行以 [判据名] 开头。
 */
export function checkIpc({ layoutText, namingText, cmakeText, goldenText, sources }) {
    const f = new Findings();
    if (layoutText === null) f.add("input-missing", `缺 ${LAYOUT}`);
    if (namingText === null) f.add("input-missing", `缺 ${NAMING}`);
    if (goldenText === null) f.add("input-missing", `缺 ${GOLDEN}`);
    if (layoutText === null || namingText === null || goldenText === null) return f.lines;
    let golden;
    try {
        golden = JSON.parse(goldenText);
    } catch (e) {
        f.add("golden-json", `${GOLDEN} 不是合法的 JSON：${e.message}`);
        return f.lines;
    }
    if (Object.keys(golden).join() !== TOP.join()) f.add("golden-top", `${GOLDEN} 的顶层键应依次为 ${TOP.join(", ")}`);
    for (const [section, want] of Object.entries(KEYS))
        for (const r of golden[section] ?? [])
            if (Object.keys(r).join() !== want.join())
                f.add("golden-keys", `${GOLDEN} 的 ${section} 记录键序应为 ${want.join(", ")}`);
    if (!f.has("golden-top") && !f.has("golden-keys") && renderGolden(golden) !== goldenText.replace(/\r\n/g, "\n"))
        f.add("golden-canonical", `${GOLDEN} 不是规范排版（每条记录一行、两格缩进、键序固定、文件末尾一个换行）`);

    const layout = parseLayout(layoutText);
    for (const e of layout.errors) f.add(e.rule, e.message);
    checkFields(f, LAYOUT, layout.fields, layout.structs);
    checkFields(f, GOLDEN, golden.fields ?? [], golden.structs ?? []);
    sameList(f, "fields-match", "字段", layout.fields, golden.fields ?? [], KEYS.fields);
    sameList(f, "structs-match", "结构", layout.structs, golden.structs ?? [], KEYS.structs);

    const declared = Object.keys(layout.constants);
    const listed = layout.constantNames;
    for (const n of declared)
        if (!listed.includes(n)) f.add("constant-unlisted", `常量 ${n} 不在 RINNE_IPC_LAYOUT_CONSTANTS 里`);
    for (const n of listed)
        if (!declared.includes(n)) f.add("constant-undeclared", `RINNE_IPC_LAYOUT_CONSTANTS 里的 ${n} 没有声明`);
    sameList(
        f,
        "constants-match",
        "常量",
        listed.map((n) => ({ constant: n, value: layout.constants[n] })),
        golden.constants ?? [],
        KEYS.constants,
    );
    if (golden.abi !== layout.constants.kIpcAbi) f.add("abi-match", `golden 的 abi ${golden.abi} ≠ kIpcAbi`);
    if (golden.maxNameLength !== layout.constants.kMaxSegmentNameLength || golden.maxNameLength !== 31)
        f.add(
            "max-name-length",
            "golden 的 maxNameLength 必须等于 kMaxSegmentNameLength，且是 31（macOS 的 POSIX 段名上限）",
        );
    const headroom = golden.nameAbiHeadroom;
    if (!Number.isInteger(headroom) || headroom < MIN_ABI_HEADROOM || headroom < layout.constants.kIpcAbi)
        f.add(
            "name-headroom",
            `golden 的 nameAbiHeadroom 必须是不小于 ${MIN_ABI_HEADROOM} 的整数（段名要给 abi 留两位数的增长空间）`,
        );

    checkPermanent(f, LAYOUT, layout.fields, layout.structs, layout.constants);
    checkPermanent(
        f,
        GOLDEN,
        golden.fields ?? [],
        golden.structs ?? [],
        Object.fromEntries((golden.constants ?? []).map((c) => [c.constant, c.value])),
    );

    // 段名
    const families = parseFamilies(namingText);
    const cmake = cmakeText === null ? null : parseCmakePrefixes(cmakeText);
    if (!families) f.add("families-missing", `${NAMING} 里找不到 kPrefixFamilies`);
    if (!cmake) f.add("cmake-missing", `${ROOT_CMAKE} 里找不到 RINNE_SHM_PREFIX_FORMAL 与 _rinne_shm_values`);
    if (families && cmake && families.map((x) => x.win32).join() !== cmake.join())
        f.add(
            "families-cmake",
            `${NAMING} 的前缀族 ${families.map((x) => x.win32).join("、")} 与 CMake 允许的 ${cmake.join("、")} 不一致`,
        );
    if (families) {
        sameList(f, "prefixes-match", "前缀族", families, golden.prefixes ?? [], KEYS.prefixes);
        const expected = [];
        for (const family of families)
            for (const platform of PLATFORMS)
                for (const scope of SCOPES)
                    for (const segment of SEGMENTS) {
                        const name = composeName(platform, family, scope, segment, layout.constants.kIpcAbi);
                        const length = nameLength(name);
                        expected.push({ platform, scope, prefix: family.win32, segment, name, length });
                        if (length > 31) f.add("name-length", `段名 ${name} 有 ${length} 个字符，超过 31`);
                        if (segment === "rv" && /\.v\d+\./.test(name))
                            f.add("rv-name-abi", `会合段名 ${name} 带了 abi`);
                        const grown = composeName(platform, family, scope, segment, Math.max(headroom ?? 0, 99));
                        if (nameLength(grown) > 31)
                            f.add(
                                "name-growth",
                                `abi 涨到 ${headroom} 时段名 ${grown} 有 ${nameLength(grown)} 个字符，超过 31`,
                            );
                    }
        sameList(f, "names-match", "段名", expected, golden.names ?? [], KEYS.names);
    }
    for (const n of golden.names ?? [])
        if (typeof n.name !== "string" || n.length !== nameLength(n.name) || n.length > golden.maxNameLength)
            f.add("golden-name-length", `golden 的段名 ${n.name} 长度记为 ${n.length}，上限 ${golden.maxNameLength}`);

    scanInto(f, sources);
    return f.lines;
}

// ---- 运行、自测与变异 ----

const read = (root, rel) =>
    fs.existsSync(path.join(root, rel)) ? fs.readFileSync(path.join(root, rel), "utf8") : null;

function sourcesOf(root) {
    const out = [];
    const walk = (dir) => {
        if (!fs.existsSync(path.join(root, dir))) return;
        for (const e of fs.readdirSync(path.join(root, dir), { withFileTypes: true })) {
            const rel = `${dir}/${e.name}`;
            if (e.isDirectory()) walk(rel);
            else if (SOURCE_EXT.test(e.name))
                out.push({ path: rel, text: fs.readFileSync(path.join(root, rel), "utf8") });
        }
    };
    walk("src");
    return out;
}

function inputsOf(root) {
    return {
        layoutText: read(root, LAYOUT),
        namingText: read(root, NAMING),
        cmakeText: read(root, ROOT_CMAKE),
        goldenText: read(root, GOLDEN),
        sources: sourcesOf(root),
    };
}

/** 把 text 里的第一处 from 换成 to；没换到就抛错，免得自测与变异因真实文件变了而空转。 */
function edit(text, from, to) {
    if (!text.includes(from)) throw new Error(`夹具里没有「${from}」`);
    return text.replace(from, to);
}
const edits = (text, pairs) => pairs.reduce((t, [from, to]) => edit(t, from, to), text);

/**
 * 在 golden 里把一个字段连同同一结构里它后面的字段一起后挪 delta 字节：前面插一段名为 <字段>_pad 的保留区，
 * 结构末尾的保留区相应缩短。golden 自身仍首尾相接、收在结构末尾、对齐不变（delta 取 8 的倍数），只有与头文件的
 * 逐行比对（以及挪到了冻结面时的冻结判据）能抓到。
 */
function shiftGoldenField(goldenText, struct, field, delta) {
    const g = JSON.parse(goldenText);
    const rows = g.fields.filter((x) => x.struct === struct);
    const at = rows.findIndex((x) => x.field === field);
    const last = rows[rows.length - 1];
    if (at < 0) throw new Error(`golden 里没有 ${struct}.${field}`);
    if (delta % 8 !== 0 || !last || last.type !== "reserved" || last.size <= delta || at === rows.length - 1)
        throw new Error(`${struct}.${field} 后挪 ${delta} 需要结构末尾有更大的保留区`);
    for (const x of rows.slice(at)) x.offset += delta;
    last.size -= delta;
    const pad = { struct, field: `${field}_pad`, offset: rows[at].offset - delta, size: delta, type: "reserved" };
    g.fields.splice(g.fields.indexOf(rows[at]), 0, { ...pad, permanent: false });
    return renderGolden(g);
}

export function selfTest(root) {
    const base = inputsOf(root);
    const missing = ["layoutText", "namingText", "cmakeText", "goldenText"].filter((k) => base[k] === null);
    if (missing.length) return { ok: false, bad: 0, good: 0, lines: [`自测要读真实文件，缺 ${missing.join("、")}`] };
    const run = (over) => checkIpc({ ...base, ...over });
    const goldenWith = (mutate) => {
        const g = JSON.parse(base.goldenText);
        mutate(g);
        return renderGolden(g);
    };
    const field = (g, s, n) => g.fields.find((x) => x.struct === s && x.field === n);
    const constant = (g, n) => g.constants.find((x) => x.constant === n);
    const struct = (g, n) => g.structs.find((x) => x.struct === n);
    const withSource = (p, text) => ({ sources: [...base.sources, { path: p, text }] });
    const L = base.layoutText;
    const G = base.goldenText;
    const N = base.namingText;
    const C = base.cmakeText;
    // [说明, 必须触发的判据（空 = 正例，必须一条都不报）, 报错行]
    const cases = [
        ["真实文件与 golden 一致", [], run({})],
        ["缺头文件", ["input-missing"], run({ layoutText: null })],
        ["缺 Naming.h", ["input-missing"], run({ namingText: null })],
        ["缺 golden", ["input-missing"], run({ goldenText: null })],
        ["golden 不是 JSON", ["golden-json"], run({ goldenText: "{" })],
        [
            "golden 顶层键换了顺序",
            ["golden-top"],
            run({ goldenText: edit(G, '"abi": 1,\n  "maxNameLength": 31,', '"maxNameLength": 31,\n  "abi": 1,') }),
        ],
        [
            "golden 记录的键序变了",
            ["golden-keys"],
            run({
                goldenText: G.replace(/\{ "constant": ("\w+"), "value": (\d+) \}/, '{ "value": $2, "constant": $1 }'),
            }),
        ],
        [
            "golden 不是规范排版",
            ["golden-canonical"],
            run({ goldenText: edit(G, '{ "constant"', '{\n      "constant"') }),
        ],
        [
            "头文件少了结构表",
            ["layout-macro"],
            run({ layoutText: edit(L, "#define RINNE_IPC_LAYOUT_STRUCTS(X)", "#define RINNE_IPC_LAYOUT_SHAPES(X)") }),
        ],
        [
            "常量写成表达式",
            ["constant-literal"],
            run({ layoutText: edit(L, "kRingBytes = 540672;", "kRingBytes = kRingHeaderBytes + 524288;") }),
        ],
        [
            "头文件与 golden 一起把字段挪出 8 字节对齐",
            ["field-align"],
            run({
                layoutText: edit(L, "X(RoleSlot, heartbeat_ms,      16,", "X(RoleSlot, heartbeat_ms,      20,"),
                goldenText: goldenWith((g) => (field(g, "RoleSlot", "heartbeat_ms").offset = 20)),
            }),
        ],
        [
            "golden 里 u32 字段占 8 字节",
            ["field-type-size"],
            run({ goldenText: goldenWith((g) => (field(g, "RingHeader", "clamp_count").size = 8)) }),
        ],
        [
            "golden 里两个字段之间留了缝",
            ["field-contiguous"],
            run({ goldenText: goldenWith((g) => (field(g, "RvHeader", "groups").offset = 20)) }),
        ],
        [
            "golden 里结构末尾的保留区短了一截",
            ["field-fills"],
            run({ goldenText: goldenWith((g) => (field(g, "RingHeader", "reserved3").size -= 8)) }),
        ],
        [
            "golden 的结构表多一个没有字段的结构",
            ["struct-empty"],
            run({ goldenText: goldenWith((g) => g.structs.push({ struct: "Ghost", size: 64, align: 64 })) }),
        ],
        ["golden 的结构表少一个结构", ["struct-unlisted"], run({ goldenText: goldenWith((g) => g.structs.pop()) })],
        [
            "golden 里改一个字段的偏移（golden 自身仍然自洽）",
            ["fields-match"],
            run({ goldenText: shiftGoldenField(G, "RingHeader", "write_head", 8) }),
        ],
        [
            "golden 单独改字段名",
            ["fields-match"],
            run({ goldenText: goldenWith((g) => (field(g, "RingHeader", "clamp_count").field = "clamp_cnt")) }),
        ],
        ["golden 少一个字段", ["fields-match"], run({ goldenText: goldenWith((g) => g.fields.splice(3, 1)) })],
        [
            "golden 改了字段类型",
            ["fields-match"],
            run({ goldenText: goldenWith((g) => (field(g, "RingHeader", "ring_frames").type = "f32")) }),
        ],
        ["golden 改了结构对齐", ["structs-match"], run({ goldenText: goldenWith((g) => (g.structs[0].align = 128)) })],
        [
            "头文件多一个没进常量表的常量",
            ["constant-unlisted"],
            run({
                layoutText: edit(
                    L,
                    "inline constexpr u32 kIpcAbi = 1;",
                    "inline constexpr u32 kIpcAbi = 1;\ninline constexpr u32 kExtra = 7;",
                ),
            }),
        ],
        [
            "常量表里有没声明的常量",
            ["constant-undeclared"],
            run({ layoutText: edit(L, "X(kOutBypass) X(kOutFading)", "X(kOutBypass) X(kOutFading) X(kGhost)") }),
        ],
        [
            "golden 改了常量值",
            ["constants-match"],
            run({ goldenText: goldenWith((g) => (constant(g, "kRingFrames").value = 32768)) }),
        ],
        ["golden 的 abi 与 kIpcAbi 不同", ["abi-match"], run({ goldenText: goldenWith((g) => (g.abi = 2)) })],
        [
            "golden 的段名上限不是 31",
            ["max-name-length"],
            run({ goldenText: goldenWith((g) => (g.maxNameLength = 30)) }),
        ],
        [
            "golden 把 abi 的增长空间调小",
            ["name-headroom"],
            run({ goldenText: goldenWith((g) => (g.nameAbiHeadroom = 9)) }),
        ],
        [
            "头文件与 golden 一起移动永久冻结的 version",
            ["permanent-field"],
            run({
                layoutText: edit(
                    edit(L, "X(RoleSlot, version,           56,", "X(RoleSlot, version,           60,"),
                    "X(RoleSlot, cfg_flags,         60,",
                    "X(RoleSlot, cfg_flags,         56,",
                ),
                goldenText: goldenWith((g) => {
                    field(g, "RoleSlot", "version").offset = 60;
                    field(g, "RoleSlot", "cfg_flags").offset = 56;
                }),
            }),
        ],
        [
            "头文件与 golden 一起去掉 init_pid 的永久标记",
            ["permanent-field"],
            run({
                layoutText: edit(
                    L,
                    'X(RvHeader, init_pid,          32,     4,      "u32",        true)',
                    'X(RvHeader, init_pid,          32,     4,      "u32",        false)',
                ),
                goldenText: goldenWith((g) => (field(g, "RvHeader", "init_pid").permanent = false)),
            }),
        ],
        [
            "头文件与 golden 一起多标一个永久字段",
            ["permanent-extra"],
            run({
                layoutText: edit(
                    L,
                    'X(RvHeader, groups,            16,     4,      "u32",        false)',
                    'X(RvHeader, groups,            16,     4,      "u32",        true)',
                ),
                goldenText: goldenWith((g) => (field(g, "RvHeader", "groups").permanent = true)),
            }),
        ],
        [
            "头文件与 golden 一起把 Output 槽挪到组块的 320",
            ["permanent-anchor"],
            run({
                layoutText: edit(
                    edit(
                        L,
                        'X(GroupBlock, output,          256,    256,    "RoleSlot",   false) \\',
                        'X(GroupBlock, output_pad,      256,    64,     "reserved",   false) \\\n' +
                            '    X(GroupBlock, output,          320,    256,    "RoleSlot",   false) \\',
                    ),
                    'X(GroupBlock, panic_seq,       512,    4,      "u32",        false) \\\n' +
                        '    X(GroupBlock, reserved,        516,    508,    "reserved",   false) \\',
                    'X(GroupBlock, panic_seq,       576,    4,      "u32",        false) \\\n' +
                        '    X(GroupBlock, reserved,        580,    444,    "reserved",   false) \\',
                ),
                goldenText: shiftGoldenField(G, "GroupBlock", "output", 64),
            }),
        ],
        [
            "头文件与 golden 一起把会合段头挪到 960",
            ["permanent-anchor"],
            run({
                layoutText: edit(
                    L,
                    'X(RvSegment, header,           0,      64,     "RvHeader",   false) \\\n' +
                        '    X(RvSegment, reserved0,        64,     960,    "reserved",   false) \\',
                    'X(RvSegment, reserved0,        0,      960,    "reserved",   false) \\\n' +
                        '    X(RvSegment, header,           960,    64,     "RvHeader",   false) \\',
                ),
                goldenText: goldenWith((g) => {
                    const i = g.fields.indexOf(field(g, "RvSegment", "header"));
                    const [header, reserved] = g.fields.splice(i, 2);
                    reserved.offset = 0;
                    header.offset = 960;
                    g.fields.splice(i, 0, reserved, header);
                }),
            }),
        ],
        [
            "头文件与 golden 一起改初始化标记",
            ["permanent-constant"],
            run({
                layoutText: edit(L, 'kInitMarker = util::fourcc("RNIN");', 'kInitMarker = util::fourcc("RNIX");'),
                goldenText: goldenWith((g) => (constant(g, "kInitMarker").value = fourcc("RNIX"))),
            }),
        ],
        [
            "头文件与 golden 一起改会合段的 magic",
            ["permanent-constant"],
            run({
                layoutText: edit(L, 'kRvMagic = util::fourcc("RNRV");', 'kRvMagic = util::fourcc("RNRW");'),
                goldenText: goldenWith((g) => (constant(g, "kRvMagic").value = fourcc("RNRW"))),
            }),
        ],
        [
            "头文件与 golden 一起把会合段改小到冻结下限以下",
            ["rv-floor"],
            run({
                layoutText: edit(L, "kRvBytes = 16384;", "kRvBytes = 8192;"),
                goldenText: goldenWith((g) => (constant(g, "kRvBytes").value = 8192)),
            }),
        ],
        // 下面每个坏样例只动冻结面的一项，删掉 ANCHORS、PERMANENT_STRUCTS、PERMANENT_CONSTANTS 的任何一项或
        // checkPermanent 的任何一个比较，都有一个样例变红
        [
            "头文件与 golden 一起把 RvHeader.magic 的类型改成 f32（偏移与大小不变）",
            ["permanent-field"],
            run({
                layoutText: edit(
                    L,
                    'X(RvHeader, magic,             0,      4,      "u32",',
                    'X(RvHeader, magic,             0,      4,      "f32",',
                ),
                goldenText: goldenWith((g) => (field(g, "RvHeader", "magic").type = "f32")),
            }),
        ],
        [
            "头文件与 golden 一起把 init_pid 加宽到 8 字节（类型仍写 u32）",
            ["field-type-size", "permanent-field"],
            run({
                layoutText: edits(L, [
                    ["X(RvHeader, init_pid,          32,     4,", "X(RvHeader, init_pid,          32,     8,"],
                    ["X(RvHeader, reserved,          36,     28,", "X(RvHeader, reserved,          40,     24,"],
                ]),
                goldenText: goldenWith((g) => {
                    field(g, "RvHeader", "init_pid").size = 8;
                    Object.assign(field(g, "RvHeader", "reserved"), { offset: 40, size: 24 });
                }),
            }),
        ],
        // 冻结面在头文件与 golden 两侧各核一遍：只改一侧时，除了逐行比对，那一侧的冻结判据也要报
        [
            "只在头文件里把 RvHeader.magic 的类型改成 f32",
            ["fields-match", "permanent-field"],
            run({
                layoutText: edit(
                    L,
                    'X(RvHeader, magic,             0,      4,      "u32",',
                    'X(RvHeader, magic,             0,      4,      "f32",',
                ),
            }),
        ],
        [
            "只在 golden 里把 RvHeader.magic 的类型改成 f32",
            ["fields-match", "permanent-field"],
            run({ goldenText: goldenWith((g) => (field(g, "RvHeader", "magic").type = "f32")) }),
        ],
        [
            "头文件与 golden 一起把组块挪到会合段的 1088",
            ["permanent-anchor"],
            run({
                layoutText: edits(L, [
                    ["X(RvSegment, reserved0,        64,     960,", "X(RvSegment, reserved0,        64,     1024,"],
                    ["X(RvSegment, groups,           1024,   8192,", "X(RvSegment, groups,           1088,   8192,"],
                    ["X(RvSegment, reserved1,        9216,   7168,", "X(RvSegment, reserved1,        9280,   7104,"],
                ]),
                goldenText: goldenWith((g) => {
                    field(g, "RvSegment", "reserved0").size = 1024;
                    field(g, "RvSegment", "groups").offset = 1088;
                    Object.assign(field(g, "RvSegment", "reserved1"), { offset: 9280, size: 7104 });
                }),
            }),
        ],
        [
            "头文件与 golden 一起把 Input 槽挪到 Output 槽后面（Output 槽不动）",
            ["permanent-anchor"],
            run({
                layoutText: edits(L, [
                    [
                        'X(GroupBlock, input,           0,      256,    "RoleSlot",   false) \\',
                        'X(GroupBlock, input_pad,       0,      256,    "reserved",   false) \\',
                    ],
                    [
                        'X(GroupBlock, panic_seq,       512,    4,      "u32",        false) \\\n' +
                            '    X(GroupBlock, reserved,        516,    508,    "reserved",   false) \\',
                        'X(GroupBlock, input,           512,    256,    "RoleSlot",   false) \\\n' +
                            '    X(GroupBlock, panic_seq,       768,    4,      "u32",        false) \\\n' +
                            '    X(GroupBlock, reserved,        772,    252,    "reserved",   false) \\',
                    ],
                ]),
                goldenText: goldenWith((g) => {
                    const input = field(g, "GroupBlock", "input");
                    const at = g.fields.indexOf(input);
                    g.fields.splice(at, 1, { ...input, field: "input_pad", type: "reserved" });
                    g.fields.splice(at + 2, 0, { ...input, offset: 512 });
                    field(g, "GroupBlock", "panic_seq").offset = 768;
                    Object.assign(field(g, "GroupBlock", "reserved"), { offset: 772, size: 252 });
                }),
            }),
        ],
        [
            "头文件与 golden 一起把组块长到 1088（kGroupStride 不动）",
            ["permanent-struct"],
            run({
                layoutText: edits(L, [
                    ["X(GroupBlock, reserved,        516,    508,", "X(GroupBlock, reserved,        516,    572,"],
                    ["X(RvSegment, groups,           1024,   8192,", "X(RvSegment, groups,           1024,   8704,"],
                    ["X(RvSegment, reserved1,        9216,   7168,", "X(RvSegment, reserved1,        9728,   6656,"],
                    ["X(GroupBlock,  1024,   64)", "X(GroupBlock,  1088,   64)"],
                ]),
                goldenText: goldenWith((g) => {
                    field(g, "GroupBlock", "reserved").size = 572;
                    field(g, "RvSegment", "groups").size = 8704;
                    Object.assign(field(g, "RvSegment", "reserved1"), { offset: 9728, size: 6656 });
                    struct(g, "GroupBlock").size = 1088;
                }),
            }),
        ],
        [
            "头文件与 golden 一起把端点槽长到 320（kRoleSlotBytes 不动）",
            ["permanent-struct"],
            run({
                layoutText: edits(L, [
                    [
                        'X(RoleSlot, name_words,        144,    112,    "u32[28]",    false) \\',
                        'X(RoleSlot, name_words,        144,    112,    "u32[28]",    false) \\\n' +
                            '    X(RoleSlot, reserved1,         256,    64,     "reserved",   false) \\',
                    ],
                    ["X(RoleSlot,    256,    64)", "X(RoleSlot,    320,    64)"],
                ]),
                goldenText: goldenWith((g) => {
                    const last = field(g, "RoleSlot", "name_words");
                    g.fields.splice(g.fields.indexOf(last) + 1, 0, {
                        struct: "RoleSlot",
                        field: "reserved1",
                        offset: 256,
                        size: 64,
                        type: "reserved",
                        permanent: false,
                    });
                    struct(g, "RoleSlot").size = 320;
                }),
            }),
        ],
        [
            "头文件与 golden 一起把 RvSegment 结构缩到 12288（kRvBytes 不动）",
            ["rv-floor", "rv-struct"],
            run({
                layoutText: edits(L, [
                    ["X(RvSegment, reserved1,        9216,   7168,", "X(RvSegment, reserved1,        9216,   3072,"],
                    ["X(RvSegment,   16384,  64)", "X(RvSegment,   12288,  64)"],
                ]),
                goldenText: goldenWith((g) => {
                    field(g, "RvSegment", "reserved1").size = 3072;
                    struct(g, "RvSegment").size = 12288;
                }),
            }),
        ],
        [
            "头文件与 golden 一起把 RvSegment 结构长到 20480（kRvBytes 不动）",
            ["rv-struct"],
            run({
                layoutText: edits(L, [
                    ["X(RvSegment, reserved1,        9216,   7168,", "X(RvSegment, reserved1,        9216,   11264,"],
                    ["X(RvSegment,   16384,  64)", "X(RvSegment,   20480,  64)"],
                ]),
                goldenText: goldenWith((g) => {
                    field(g, "RvSegment", "reserved1").size = 11264;
                    struct(g, "RvSegment").size = 20480;
                }),
            }),
        ],
        ...[
            ["kGroupStride", 1024, 1088],
            ["kRoleSlotBytes", 256, 320],
            ["kRvMinBytes", 16384, 12288],
        ].map(([name, from, to]) => [
            `头文件与 golden 一起把 ${name} 改成 ${to}`,
            ["permanent-constant"],
            run({
                layoutText: edit(L, `${name} = ${from};`, `${name} = ${to};`),
                goldenText: goldenWith((g) => (constant(g, name).value = to)),
            }),
        ]),
        [
            "Naming.h 里找不到前缀族",
            ["families-missing"],
            run({ namingText: edit(N, "kPrefixFamilies[3]", "kPrefixFamilyTable[3]") }),
        ],
        ["根 CMakeLists.txt 里找不到段名前缀", ["cmake-missing"], run({ cmakeText: "project(X)\n" })],
        [
            "Naming.h 的前缀族与 CMake 不一致",
            ["families-cmake"],
            run({ namingText: edit(N, '{"SynchainRinneP", "SnchRinneP"}', '{"SynchainRinneQ", "SnchRinneQ"}') }),
        ],
        [
            "golden 的前缀族单独改了短写",
            ["prefixes-match"],
            run({ goldenText: goldenWith((g) => (g.prefixes[2].posix = "SnchRinnQ")) }),
        ],
        [
            "注入超长前缀（Naming.h、CMake、golden 一起改）",
            ["name-length"],
            run({
                namingText: edit(N, '{"SynchainRinneD", "SnchRinneD"}', '{"SynchainRinneDevelop", "SnchRinneDevelop"}'),
                cmakeText: edit(C, '"${RINNE_SHM_PREFIX_FORMAL}D"', '"${RINNE_SHM_PREFIX_FORMAL}Develop"'),
                goldenText: goldenWith((g) => {
                    g.prefixes[1] = { win32: "SynchainRinneDevelop", posix: "SnchRinneDevelop" };
                    for (const n of g.names.filter((x) => x.prefix === "SynchainRinneD")) {
                        n.prefix = "SynchainRinneDevelop";
                        n.name = n.name.replace("RinneD.", "RinneDevelop.");
                        n.length = nameLength(n.name);
                    }
                }),
            }),
        ],
        [
            "前缀族里带了 abi 形态，会合段名跟着带上",
            ["rv-name-abi"],
            run({ namingText: edit(N, '{"SynchainRinneP", "SnchRinneP"}', '{"SynchainRinneP.v1", "SnchRinneP.v1"}') }),
        ],
        [
            "短写族多一个字符：abi 1 时还放得下，abi 涨到两位数就超长",
            ["name-growth"],
            run({
                namingText: edit(N, '{"SynchainRinneD", "SnchRinneD"}', '{"SynchainRinneD", "SnchRinneDx"}'),
                goldenText: goldenWith((g) => {
                    g.prefixes[1] = { win32: "SynchainRinneD", posix: "SnchRinneDx" };
                    for (const n of g.names.filter((x) => x.name.includes("SnchRinneD."))) {
                        n.name = n.name.replace("SnchRinneD.", "SnchRinneDx.");
                        n.length = nameLength(n.name);
                    }
                }),
            }),
        ],
        [
            "golden 的段名改了一个字符",
            ["names-match"],
            run({
                goldenText: goldenWith((g) => (g.names[0].name = g.names[0].name.replace("Rinne", "Rinme"))),
            }),
        ],
        [
            "golden 的会合段名带上 abi",
            ["names-match"],
            run({ goldenText: goldenWith((g) => (g.names[0].name = g.names[0].name.replace(".rv", ".v1.rv"))) }),
        ],
        [
            "golden 记的段名长度不对",
            ["golden-name-length"],
            run({ goldenText: goldenWith((g) => (g.names[0].length += 1)) }),
        ],
        // 读环境的每一种写法各一个坏样例，每个只命中 ENV_READ 的一个分支
        ...[
            ["src/core/ipc/Extra.cpp", 'auto d = std::getenv("RINNE_IPC_LOCK_DIR");'],
            ["src/core/util/Env.cpp", 'auto d = _wgetenv(L"X");'],
            ["src/core/util/Env.cpp", 'getenv_s(&n, buf, sizeof(buf), "X");'],
            ["src/core/util/Env.cpp", '_wgetenv_s(&n, buf, 8, L"X");'],
            ["src/core/util/Env.cpp", '_dupenv_s(&p, &n, "X");'],
            ["src/core/util/Env.cpp", '_wdupenv_s(&p, &n, L"X");'],
            ["src/core/util/Env.cpp", 'auto d = secure_getenv("X");'],
            ["src/plugin-common/Env.cpp", '::GetEnvironmentVariableW(L"X", buf, 8);'],
            ["src/plugin-common/Env.cpp", "auto* block = ::GetEnvironmentStringsW();"],
            ["src/plugin-common/Env.cpp", '::ExpandEnvironmentStringsW(L"%APPDATA%", buf, 8);'],
            ["src/plugin-common/X.cpp", 'auto d = juce::SystemStats::getEnvironmentVariable("X", {});'],
            ["src/plugin-common/Env.mm", "char** env = *_NSGetEnviron();"],
            ["src/core/util/Env.cpp", "for (char** e = __environ; *e; ++e) {}"],
            ["src/core/util/Env.cpp", "extern char** environ;"],
            ["src/plugin-common/Env.mm", "NSDictionary* env = [[NSProcessInfo processInfo] environment];"],
        ].map(([p, line]) => [
            `发布代码读环境：${line}`,
            ["env-read"],
            run(
                withSource(
                    p,
                    `${line}
`,
                ),
            ),
        ]),
        [
            "注释与字符串里提到 getenv 不算",
            [],
            run(withSource("src/core/ipc/Extra.cpp", '// 不用 getenv 与 __environ\nconst char* s = "getenv";\n')),
        ],
        [
            "插件代码调用测试作用域",
            ["test-hook"],
            run(withSource("src/plugin-common/Wiring.cpp", "auto s = ipc::NameScope::randomForTesting();\n")),
        ],
        [
            "core 的其他文件换测试锁目录",
            ["test-hook"],
            run(
                withSource(
                    "src/core/ipc/Rendezvous.cpp",
                    "SegmentBackendPosix b{SegmentBackendPosix::LockDirForTesting{d}};\n",
                ),
            ),
        ],
        [
            "发布代码覆盖段头初始化的测试钩子",
            ["test-hook"],
            run(
                withSource(
                    "src/core/ipc/Rendezvous.h",
                    "void onInitStageForTesting(InitStage s, const SegmentView& v) noexcept override;\n",
                ),
            ),
        ],
        [
            "发布代码借租约的测试接缝",
            ["test-hook"],
            run(withSource("src/input/Engine.cpp", "struct LeaseSeamForTesting {};\n")),
        ],
        [
            "tests 与 tools 里用测试钩子不算",
            [],
            run(withSource("tools/peer.cpp", 'auto s = ipc::NameScope::randomForTesting(); getenv("X");\n')),
        ],
    ].map(([label, expect, errors]) => {
        const missed = expect.filter((r) => !errors.some((e) => e.startsWith(`[${r}] `)));
        const passed = expect.length ? missed.length === 0 : errors.length === 0;
        return {
            label: missed.length ? `${label}（没有触发 ${missed.join("、")}）` : label,
            bad: !!expect.length,
            passed,
            expect,
        };
    });
    const unnamed = Object.keys(RULES).filter((r) => !cases.some((c) => c.expect.includes(r)));
    cases.push({
        label: `每条判据都有坏样例点名${unnamed.length ? `（没有：${unnamed.join("、")}）` : ""}`,
        bad: false,
        passed: unnamed.length === 0,
    });
    return caseResult(cases);
}

export default {
    name: "contract-static-ipc",
    group: "docs",
    inputs: [],
    run(ctx) {
        const errors = checkIpc(inputsOf(ctx.root));
        return { ok: errors.length === 0, lines: errors };
    },
    selfTest: (ctx) => selfTest(ctx.root),
    /** 在真实文件上做三种变异，门禁都必须变红。 */
    mutationCheck(ctx) {
        const base = inputsOf(ctx.root);
        const lines = [];
        const red = (label, over) => {
            if (!checkIpc({ ...base, ...over }).length) lines.push(`${label}：门禁没有变红`);
        };
        red("golden 改一个字段的偏移", {
            goldenText: shiftGoldenField(base.goldenText, "RingHeader", "write_head", 8),
        });
        red("注入超长前缀", {
            namingText: edit(base.namingText, '"SnchRinneP"}', '"SnchRinnePrefixTooLong"}'),
        });
        red("发布代码读环境变量", {
            sources: [...base.sources, { path: "src/core/ipc/X.cpp", text: 'getenv("HOME");\n' }],
        });
        return { ok: lines.length === 0, lines };
    },
};
