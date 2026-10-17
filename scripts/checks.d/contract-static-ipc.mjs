// SPDX-License-Identifier: GPL-3.0-or-later
//
// 共享内存契约的静态对拍：不编译，直接读 src/core/ipc/SegmentLayout.h 的字段表、结构表与常量，和 Naming.h 的前缀族、
// 根 CMakeLists.txt 的三个段名前缀，与 tests/golden/ipc-layout.json 逐项比对。编译后的那一遍在 rinne_core_tests 里
// （同一张字段表展开成 offsetof），两遍都要绿。比对的内容：
//   - 字段：结构、字段名、偏移、字节数、类型、是否永久冻结，顺序也要一致；同一结构的字段首尾相接、不重叠不留缝，
//     最后一项收在结构末尾；u64 字段的偏移是 8 的倍数，u32 是 4 的倍数；
//   - 结构的大小与对齐；常量（头文件里每个 inline constexpr 都要进 golden，值只能写字面量或 util::fourcc("ABCD")）；
//   - 永久冻结面（本文件写死，不随 golden 改）：会合段头的前 16 字节，端点槽 version 的绝对位置与它依赖的
//     组块偏移、步长，会合段的 magic；
//   - 段名：三族前缀 × 9 段 × 2 平台 × {正式, 测试} 由本文件按规则重新拼一遍，与 golden 逐条相等，每条不超过 31 字符；
//     前缀族与根 CMakeLists.txt 允许的三个值一一对应；会合段名不带 abi；
//   - golden 的排版是规范形：每条记录一行、键序固定（不是规范形就判红，免得两行之间藏一处手改）；
//   - 发布代码不读环境变量（src/** 里出现 getenv 一族判红，注释与字符串里的不算）；IPC 的测试钩子只出现在
//     声明它们的文件里，插件与其他 core 代码不许调用。
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
const TAG = "<tag8>";

/** 永久冻结面：连 abi 升级都不许动。端点槽 version 的绝对位置 = 1024·G + {0, 256} + 56。 */
const PERMANENT = [
    { struct: "RvHeader", field: "magic", offset: 0, size: 4, type: "u32" },
    { struct: "RvHeader", field: "abi", offset: 4, size: 4, type: "u32" },
    { struct: "RvHeader", field: "generation", offset: 8, size: 4, type: "u32" },
    { struct: "RvHeader", field: "layout_bytes", offset: 12, size: 4, type: "u32" },
    { struct: "RoleSlot", field: "version", offset: 56, size: 4, type: "u32" },
];
const VERSION_ANCHORS = [
    { struct: "RvSegment", field: "groups", offset: 1024 },
    { struct: "GroupBlock", field: "input", offset: 0 },
    { struct: "GroupBlock", field: "output", offset: 256 },
];
const PERMANENT_CONSTANTS = { kRvMagic: fourcc("RNRV"), kGroupStride: 1024, kRoleSlotBytes: 256 };

const KEYS = {
    constants: ["constant", "value"],
    structs: ["struct", "size", "align"],
    fields: ["struct", "field", "offset", "size", "type", "permanent"],
    prefixes: ["win32", "posix"],
    names: ["platform", "scope", "prefix", "segment", "name", "length"],
};
const TOP = ["abi", "maxNameLength", ...Object.keys(KEYS)];

export function fourcc(code) {
    return [...code].reduce((v, c, i) => (v + c.charCodeAt(0) * 2 ** (8 * i)) >>> 0, 0);
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

/** 解析 SegmentLayout.h：字段表、结构表、常量名单与常量值。 */
export function parseLayout(text) {
    const errors = [];
    const code = stripCpp(text);
    const body = (name) => {
        const b = macroBody(code, name);
        if (b === null) errors.push(`${LAYOUT} 里找不到宏 ${name}`);
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
        else errors.push(`${LAYOUT}：常量 ${m[1]} 只能写字面量或 util::fourcc("ABCD")，现在是 ${raw}`);
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

/** 段名的真实长度：占位符按它代表的 8 个字符算。 */
export const nameLength = (name) => name.replaceAll(UID, "u".repeat(8)).replaceAll(TAG, "t".repeat(8)).length;

/** golden 的规范排版：每条记录一行，键序按 KEYS。 */
export function renderGolden(g) {
    const rec = (obj, keys) => `{ ${keys.map((k) => `${JSON.stringify(k)}: ${JSON.stringify(obj[k])}`).join(", ")} }`;
    const parts = [`  "abi": ${JSON.stringify(g.abi)}`, `  "maxNameLength": ${JSON.stringify(g.maxNameLength)}`];
    for (const [section, keys] of Object.entries(KEYS)) {
        const rows = (g[section] ?? []).map((r) => `    ${rec(r, keys)}`);
        parts.push(`  "${section}": [\n${rows.join(",\n")}\n  ]`);
    }
    return `{\n${parts.join(",\n")}\n}\n`;
}

const ENV_READ =
    /\b(?:std::)?(?:getenv|_wgetenv|getenv_s|_wgetenv_s|_dupenv_s|_wdupenv_s|secure_getenv|GetEnvironmentVariable[AW]?|ExpandEnvironmentStrings[AW]?|getEnvironmentVariable|_?w?environ)\b|processInfo\s*\]\s*environment/;
const SOURCE_EXT = /\.(?:c|cc|cpp|cxx|h|hh|hpp|m|mm)$/;
/** IPC 的测试钩子，与允许出现它们的文件（声明与定义处）。 */
const TEST_HOOKS = /\b(?:forTesting|randomForTesting|LockDirForTesting|resetAllForTesting)\b/;
const HOOK_HOMES = new Set(
    ["Naming", "SegmentBackendPosix", "SegmentBackendInProcess"].flatMap((f) =>
        ["h", "cpp"].map((e) => `src/core/ipc/${f}.${e}`),
    ),
);

/** 发布代码的扫描：环境变量与测试钩子。sources = [{ path, text }]，路径相对仓根。 */
export function scanSources(sources) {
    const errors = [];
    for (const { path: p, text } of sources) {
        if (!p.startsWith("src/") || !SOURCE_EXT.test(p)) continue;
        const lines = stripCpp(text, { keepStrings: false }).split("\n");
        lines.forEach((line, i) => {
            if (ENV_READ.test(line)) errors.push(`${p}:${i + 1}：发布代码不许读环境变量（${line.trim()}）`);
            const hook = HOOK_HOMES.has(p) ? null : TEST_HOOKS.exec(line);
            if (hook) errors.push(`${p}:${i + 1}：IPC 的测试钩子 ${hook[0]} 只能出现在声明它的文件里`);
        });
    }
    return errors;
}

function checkFields(fields, structs) {
    const errors = [];
    const byStruct = new Map();
    for (const f of fields) {
        if (!byStruct.has(f.struct)) byStruct.set(f.struct, []);
        byStruct.get(f.struct).push(f);
        const align = { u64: 8, u32: 4 }[f.type];
        if (align && f.offset % align !== 0) errors.push(`${f.struct}.${f.field}：${f.type} 的偏移 ${f.offset} 没对齐`);
        if (align && f.size !== align) errors.push(`${f.struct}.${f.field}：${f.type} 的大小应为 ${align}`);
    }
    for (const s of structs) {
        const list = byStruct.get(s.struct) ?? [];
        if (!list.length) errors.push(`结构 ${s.struct} 在字段表里没有字段`);
        let at = 0;
        for (const f of list) {
            if (f.offset !== at)
                errors.push(`${s.struct}.${f.field} 的偏移是 ${f.offset}，前一项收在 ${at}（不许重叠或留缝）`);
            at = f.offset + f.size;
        }
        if (list.length && at !== s.size) errors.push(`${s.struct} 的字段收在 ${at}，结构大小是 ${s.size}`);
    }
    for (const name of byStruct.keys())
        if (!structs.some((s) => s.struct === name)) errors.push(`字段表里的结构 ${name} 不在结构表里`);
    return errors;
}

function sameList(label, a, b, keys) {
    const errors = [];
    const key = (r) => JSON.stringify(keys.map((k) => r[k]));
    const n = Math.max(a.length, b.length);
    for (let i = 0; i < n; i++) {
        if (!a[i]) errors.push(`${label}：golden 多出第 ${i + 1} 项 ${key(b[i])}`);
        else if (!b[i]) errors.push(`${label}：golden 缺第 ${i + 1} 项 ${key(a[i])}`);
        else if (key(a[i]) !== key(b[i]))
            errors.push(`${label} 第 ${i + 1} 项：头文件 ${key(a[i])}，golden ${key(b[i])}`);
    }
    return errors;
}

/**
 * 全部对拍。layoutText / namingText / cmakeText 是三份源文件的文本，goldenText 是 golden 原文，sources 是要扫描的
 * 发布代码（[{ path, text }]）。返回错误列表。
 */
export function checkIpc({ layoutText, namingText, cmakeText, goldenText, sources }) {
    const errors = [];
    if (layoutText === null) return [`缺 ${LAYOUT}`];
    if (namingText === null) return [`缺 ${NAMING}`];
    if (goldenText === null) return [`缺 ${GOLDEN}`];
    let golden;
    try {
        golden = JSON.parse(goldenText);
    } catch (e) {
        return [`${GOLDEN} 不是合法的 JSON：${e.message}`];
    }
    const keys = Object.keys(golden);
    if (keys.join() !== TOP.join()) errors.push(`${GOLDEN} 的顶层键应依次为 ${TOP.join(", ")}`);
    for (const [section, want] of Object.entries(KEYS))
        for (const r of golden[section] ?? [])
            if (Object.keys(r).join() !== want.join())
                errors.push(`${GOLDEN} 的 ${section} 记录键序应为 ${want.join(", ")}`);
    if (!errors.length && renderGolden(golden) !== goldenText.replace(/\r\n/g, "\n"))
        errors.push(`${GOLDEN} 不是规范排版（每条记录一行、两格缩进、键序固定、文件末尾一个换行）`);

    const layout = parseLayout(layoutText);
    errors.push(...layout.errors);
    errors.push(...checkFields(layout.fields, layout.structs).map((e) => `${LAYOUT}：${e}`));
    errors.push(...checkFields(golden.fields ?? [], golden.structs ?? []).map((e) => `${GOLDEN}：${e}`));
    errors.push(...sameList("字段", layout.fields, golden.fields ?? [], KEYS.fields));
    errors.push(...sameList("结构", layout.structs, golden.structs ?? [], KEYS.structs));

    const declared = Object.keys(layout.constants);
    const listed = layout.constantNames;
    for (const n of declared) if (!listed.includes(n)) errors.push(`常量 ${n} 不在 RINNE_IPC_LAYOUT_CONSTANTS 里`);
    for (const n of listed) if (!declared.includes(n)) errors.push(`RINNE_IPC_LAYOUT_CONSTANTS 里的 ${n} 没有声明`);
    errors.push(
        ...sameList(
            "常量",
            listed.map((n) => ({ constant: n, value: layout.constants[n] })),
            golden.constants ?? [],
            KEYS.constants,
        ),
    );
    if (golden.abi !== layout.constants.kIpcAbi) errors.push(`golden 的 abi ${golden.abi} ≠ kIpcAbi`);
    if (golden.maxNameLength !== layout.constants.kMaxSegmentNameLength || golden.maxNameLength !== 31)
        errors.push("golden 的 maxNameLength 必须等于 kMaxSegmentNameLength，且是 31（macOS 的 POSIX 段名上限）");

    // 永久冻结面：对头文件与 golden 各核一遍
    for (const [where, fields, constants] of [
        [LAYOUT, layout.fields, layout.constants],
        [GOLDEN, golden.fields ?? [], Object.fromEntries((golden.constants ?? []).map((c) => [c.constant, c.value]))],
    ]) {
        for (const p of PERMANENT) {
            const f = fields.find((x) => x.struct === p.struct && x.field === p.field);
            if (!f || f.offset !== p.offset || f.size !== p.size || f.type !== p.type || !f.permanent)
                errors.push(
                    `${where}：永久冻结的 ${p.struct}.${p.field} 必须在偏移 ${p.offset}、${p.type}，并标为永久`,
                );
        }
        for (const f of fields)
            if (f.permanent && !PERMANENT.some((p) => p.struct === f.struct && p.field === f.field))
                errors.push(
                    `${where}：${f.struct}.${f.field} 标了永久冻结，永久冻结面只有会合段头前 16 字节与端点槽 version`,
                );
        for (const a of VERSION_ANCHORS) {
            const f = fields.find((x) => x.struct === a.struct && x.field === a.field);
            if (!f || f.offset !== a.offset)
                errors.push(`${where}：${a.struct}.${a.field} 必须在 ${a.offset}（端点槽 version 的绝对位置依赖它）`);
        }
        for (const [n, v] of Object.entries(PERMANENT_CONSTANTS))
            if (constants[n] !== v) errors.push(`${where}：${n} 永久是 ${v}`);
    }

    // 段名
    const families = parseFamilies(namingText);
    const cmake = cmakeText === null ? null : parseCmakePrefixes(cmakeText);
    if (!families) errors.push(`${NAMING} 里找不到 kPrefixFamilies`);
    if (!cmake) errors.push(`${ROOT_CMAKE} 里找不到 RINNE_SHM_PREFIX_FORMAL 与 _rinne_shm_values`);
    if (families && cmake) {
        if (families.map((f) => f.win32).join() !== cmake.join())
            errors.push(
                `${NAMING} 的前缀族 ${families.map((f) => f.win32).join("、")} 与 CMake 允许的 ${cmake.join("、")} 不一致`,
            );
        errors.push(...sameList("前缀族", families, golden.prefixes ?? [], KEYS.prefixes));
        const expected = [];
        for (const family of families)
            for (const platform of PLATFORMS)
                for (const scope of SCOPES)
                    for (const segment of SEGMENTS) {
                        const name = composeName(platform, family, scope, segment, layout.constants.kIpcAbi);
                        const length = nameLength(name);
                        expected.push({ platform, scope, prefix: family.win32, segment, name, length });
                        if (length > 31) errors.push(`段名 ${name} 有 ${length} 个字符，超过 31`);
                        if (segment === "rv" && /\.v\d+\./.test(name)) errors.push(`会合段名 ${name} 带了 abi`);
                    }
        errors.push(...sameList("段名", expected, golden.names ?? [], KEYS.names));
    }
    for (const n of golden.names ?? [])
        if (typeof n.name !== "string" || n.length !== nameLength(n.name) || n.length > golden.maxNameLength)
            errors.push(`golden 的段名 ${n.name} 长度记为 ${n.length}，上限 ${golden.maxNameLength}`);

    errors.push(...scanSources(sources));
    return errors;
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

/** 在 golden 里改一个字段的偏移（同一结构里随后的字段一起挪，保持首尾相接）。 */
function shiftGoldenField(goldenText, struct, field, delta) {
    const g = JSON.parse(goldenText);
    const f = g.fields.find((x) => x.struct === struct && x.field === field);
    if (!f) throw new Error(`golden 里没有 ${struct}.${field}`);
    f.offset += delta;
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
    const withSource = (p, text) => ({ sources: [...base.sources, { path: p, text }] });
    const L = base.layoutText;
    const cases = [
        ["真实文件与 golden 一致", false, run({})],
        [
            "golden 里改一个字段的偏移",
            true,
            run({ goldenText: shiftGoldenField(base.goldenText, "RoleSlot", "pid", 4) }),
        ],
        [
            "头文件与 golden 一起把字段挪出 8 字节对齐",
            true,
            run({
                layoutText: edit(L, "X(RoleSlot, heartbeat_ms,      16,", "X(RoleSlot, heartbeat_ms,      20,"),
                goldenText: shiftGoldenField(base.goldenText, "RoleSlot", "heartbeat_ms", 4),
            }),
        ],
        ["golden 少一个字段", true, run({ goldenText: goldenWith((g) => g.fields.splice(3, 1)) })],
        ["golden 改了字段类型", true, run({ goldenText: goldenWith((g) => (g.fields[0].type = "u64")) })],
        ["golden 改了结构大小", true, run({ goldenText: goldenWith((g) => (g.structs[1].size = 512)) })],
        ["golden 改了常量值", true, run({ goldenText: goldenWith((g) => (g.constants[0].value = 2)) })],
        [
            "golden 不是规范排版",
            true,
            run({ goldenText: base.goldenText.replace('{ "constant"', '{\n      "constant"') }),
        ],
        [
            "golden 键序变了",
            true,
            run({
                goldenText: base.goldenText.replace(
                    /\{ "constant": ("\w+"), "value": (\d+) \}/,
                    '{ "value": $2, "constant": $1 }',
                ),
            }),
        ],
        [
            "头文件多一个没进 golden 的常量",
            true,
            run({
                layoutText: edit(
                    L,
                    "inline constexpr u32 kIpcAbi = 1;",
                    "inline constexpr u32 kIpcAbi = 1;\ninline constexpr u32 kExtra = 7;",
                ),
            }),
        ],
        [
            "常量写成表达式",
            true,
            run({ layoutText: edit(L, "kRingBytes = 540672;", "kRingBytes = kRingHeaderBytes + 524288;") }),
        ],
        [
            "头文件与 golden 一起移动永久冻结的 version",
            true,
            run({
                layoutText: edit(
                    edit(L, "X(RoleSlot, version,           56,", "X(RoleSlot, version,           60,"),
                    "X(RoleSlot, cfg_flags,         60,",
                    "X(RoleSlot, cfg_flags,         56,",
                ),
                goldenText: goldenWith((g) => {
                    g.fields.find((f) => f.field === "version").offset = 60;
                    g.fields.find((f) => f.field === "cfg_flags").offset = 56;
                }),
            }),
        ],
        [
            "去掉 version 的永久标记",
            true,
            run({
                layoutText: edit(
                    L,
                    '"u32",        true)  \\\n    X(RoleSlot, cfg_flags',
                    '"u32",        false) \\\n    X(RoleSlot, cfg_flags',
                ),
                goldenText: goldenWith((g) => (g.fields.find((f) => f.field === "version").permanent = false)),
            }),
        ],
        [
            "golden 的段名改了一个字符",
            true,
            run({ goldenText: goldenWith((g) => (g.names[0].name = g.names[0].name.replace("Rinne", "Rinme"))) }),
        ],
        [
            "会合段名带上 abi",
            true,
            run({ goldenText: goldenWith((g) => (g.names[0].name = g.names[0].name.replace(".rv", ".v1.rv"))) }),
        ],
        [
            "注入超长前缀（Naming.h、CMake、golden 一起改）",
            true,
            run({
                namingText: edit(
                    base.namingText,
                    '{"SynchainRinneD", "SnchRinneD"}',
                    '{"SynchainRinneDevelop", "SnchRinneDevelop"}',
                ),
                cmakeText: edit(base.cmakeText, '"${RINNE_SHM_PREFIX_FORMAL}D"', '"${RINNE_SHM_PREFIX_FORMAL}Develop"'),
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
            "Naming.h 的前缀族与 CMake 不一致",
            true,
            run({
                namingText: edit(
                    base.namingText,
                    '{"SynchainRinneP", "SnchRinneP"}',
                    '{"SynchainRinneQ", "SnchRinneQ"}',
                ),
            }),
        ],
        [
            "发布代码读环境变量",
            true,
            run(withSource("src/core/ipc/Extra.cpp", 'auto d = std::getenv("RINNE_IPC_LOCK_DIR");\n')),
        ],
        [
            "发布代码经 JUCE 读环境变量",
            true,
            run(
                withSource("src/plugin-common/X.cpp", 'auto d = juce::SystemStats::getEnvironmentVariable("X", {});\n'),
            ),
        ],
        [
            "注释与字符串里提到 getenv 不算",
            false,
            run(withSource("src/core/ipc/Extra.cpp", '// 不用 getenv\nconst char* s = "getenv";\n')),
        ],
        [
            "插件代码调用测试作用域",
            true,
            run(withSource("src/plugin-common/Wiring.cpp", "auto s = ipc::NameScope::randomForTesting();\n")),
        ],
        [
            "core 的其他文件换测试锁目录",
            true,
            run(
                withSource(
                    "src/core/ipc/Rendezvous.cpp",
                    "SegmentBackendPosix b{SegmentBackendPosix::LockDirForTesting{d}};\n",
                ),
            ),
        ],
        [
            "tests 与 tools 里用测试钩子不算",
            false,
            run(withSource("tools/peer.cpp", 'auto s = ipc::NameScope::randomForTesting(); getenv("X");\n')),
        ],
    ].map(([label, bad, errors]) => ({ label, bad, passed: bad ? errors.length > 0 : errors.length === 0 }));
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
