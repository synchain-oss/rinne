// SPDX-License-Identifier: GPL-3.0-or-later
//
// 桥契约四方对拍：docs/UI_BRIDGE.md、src/{input,output}/*BridgeApi.h、web/shared/bridge.js 的名表、
// web-preview/mock 逐名相等。比的是名字集合：函数、事件、Input 参数 ID、配对状态、诊断字段白名单，
// 另外对拍参数默认值与可自动化标记、配对状态的颜色分组。
//   - 文档里带 `<!-- names:<类> -->` 标记的表是机读真源（类：fn、ev、param、pair、diag），第一列的反引号名字就是名字；
//   - 头文件按变量名前缀分类：kFn 函数、kEv 事件、kParam 参数、kPair 配对状态；变量名与字面量也要对得上；
//   - bridge.js 读 BRIDGE_FUNCTIONS 与 BRIDGE_EVENTS 两张名表（纯字面量，不执行）；
//   - mock 读 bridge-{input,output}.js 的 FUNCTIONS / EVENTS 与 contract.js 的常量，另外实例化一遍后端，
//     查每个函数真的存在、每个事件在初始状态请求之后真的会发。
// 任何一方多一项或少一项都是差异。
//   node scripts/check-bridge-parity.mjs [--self-test]

import fs from "node:fs";
import path from "node:path";
import { pathToFileURL } from "node:url";
import { ROOT, assertNodeVersion, caseResult, isMain } from "./lib/check-kit.mjs";

const FILES = {
    doc: "docs/UI_BRIDGE.md",
    headerInput: "src/input/InputBridgeApi.h",
    headerOutput: "src/output/OutputBridgeApi.h",
    bridge: "web/shared/bridge.js",
    mockInput: "web-preview/mock/bridge-input.js",
    mockOutput: "web-preview/mock/bridge-output.js",
    contract: "web-preview/mock/contract.js",
};

const KINDS = ["fn", "ev", "param", "pair", "diag"];
const ROLES = ["input", "output"];
const sorted = (a) => [...a].sort();
const setOf = (a) => new Set(a);

// ---------------------------------------------------------------- 解析

/** 文档：按 section 标记切开，再按 names 标记抽表。返回 { section: { kind: { names, rows } } }。 */
export function parseDoc(text) {
    const out = {};
    const parts = text.split(/<!--\s*section:(\w+)\s*-->/);
    for (let i = 1; i < parts.length; i += 2) {
        const sec = parts[i];
        out[sec] = {};
        const lines = parts[i + 1].split("\n");
        for (let n = 0; n < lines.length; n++) {
            const m = /^<!--\s*names:(\w+)\s*-->\s*$/.exec(lines[n]);
            if (!m) continue;
            let k = n + 1;
            while (k < lines.length && lines[k].trim() === "") k++;
            const rows = [];
            while (k < lines.length && lines[k].startsWith("|")) {
                rows.push(
                    lines[k]
                        .split("|")
                        .slice(1, -1)
                        .map((c) => c.trim()),
                );
                k++;
            }
            const body = rows.slice(2); // 去表头与分隔行
            const names = [];
            for (const r of body) {
                const cell = /^`([^`]+)`$/.exec(r[0] || "");
                if (cell) names.push(cell[1]);
            }
            out[sec][m[1]] = { names, rows: body };
        }
    }
    return out;
}

/** 头文件：{ fn, ev, param, pair, stray }，stray 是不符合约定的 constexpr 行。 */
export function parseHeader(text) {
    const out = { fn: [], ev: [], param: [], pair: [], stray: [], mismatch: [] };
    const prefix = { kFn: "fn", kEv: "ev", kParam: "param", kPair: "pair" };
    for (const line of text.split("\n")) {
        if (line.trimStart().startsWith("//") || !/\bconstexpr\b/.test(line)) continue;
        const m = /constexpr\s+const\s+char\*\s+(k[A-Za-z0-9]+)\s*=\s*"([^"]*)"/.exec(line);
        if (!m) {
            out.stray.push(line.trim());
            continue;
        }
        const [, id, value] = m;
        const p = Object.keys(prefix).find((k) => id.startsWith(k) && /[A-Z]/.test(id[k.length] || ""));
        if (!p) {
            out.stray.push(line.trim());
            continue;
        }
        out[prefix[p]].push(value);
        const flat = (s) =>
            s
                .replace(/^rinne\./, "")
                .replace(/[._]/g, "")
                .toLowerCase();
        if (flat(id.slice(p.length)) !== flat(value)) out.mismatch.push(`${id} = "${value}"`);
    }
    return out;
}

/** 取 `<name> = [ ... ]` 的字符串数组；找不到返回 null。 */
function arrayLiteral(text, name) {
    const m = new RegExp(`${name}\\s*[=:]\\s*\\[([^\\]]*)\\]`).exec(text);
    return m ? [...m[1].matchAll(/"([^"]+)"/g)].map((x) => x[1]) : null;
}

/** bridge.js：{ fn: {input, output}, ev: {input, output} }。 */
export function parseBridge(text) {
    const out = { fn: {}, ev: {} };
    for (const [key, table] of [
        ["fn", "BRIDGE_FUNCTIONS"],
        ["ev", "BRIDGE_EVENTS"],
    ]) {
        const block = new RegExp(`${table}\\s*=\\s*\\{([\\s\\S]*?)\\n\\};`).exec(text);
        for (const role of ROLES) {
            out[key][role] = block ? arrayLiteral(block[1], role) : null;
        }
    }
    return out;
}

/** contract.js 的静态面：数组与 PAIR_COLORS。 */
export function parseContract(text) {
    const colors = {};
    const block = /PAIR_COLORS\s*=\s*\{([\s\S]*?)\n\};/.exec(text);
    if (block) for (const m of block[1].matchAll(/(\w+):\s*"(\w+)"/g)) colors[m[1]] = m[2];
    return {
        param: arrayLiteral(text, "PARAM_IDS"),
        pair: arrayLiteral(text, "PAIR_STATES"),
        diag: arrayLiteral(text, "DIAG_KEYS"),
        colors,
    };
}

// ---------------------------------------------------------------- 对拍

function diffSets(label, a, aName, b, bName, out) {
    if (a === null || b === null) {
        out.push(`${label}：读不到 ${a === null ? aName : bName} 里的名表`);
        return;
    }
    const sa = setOf(a);
    const sb = setOf(b);
    for (const x of sorted(sa)) if (!sb.has(x)) out.push(`${label}：${aName} 有 ${x}，${bName} 没有`);
    for (const x of sorted(sb)) if (!sa.has(x)) out.push(`${label}：${bName} 有 ${x}，${aName} 没有`);
    if (sa.size !== a.length) out.push(`${label}：${aName} 里有重复名字`);
    if (sb.size !== b.length) out.push(`${label}：${bName} 里有重复名字`);
}

const uniq = (a) => [...new Set(a)];

/**
 * 纯文本对拍。src：{ doc, headerInput, headerOutput, bridge, mockInput, mockOutput, contract } 都是文件内容；
 * defs：mock 的 PARAM_DEFS 对象（默认值与自动化标记）。返回问题数组。
 */
export function compare(src, defs) {
    const out = [];
    const doc = parseDoc(src.doc);
    for (const s of ["common", "input", "output"]) if (!doc[s]) out.push(`UI_BRIDGE.md 缺 ${s} 节的 section 标记`);
    if (out.length) return out;
    const get = (sec, kind) => doc[sec][kind] || { names: [], rows: [] };
    const header = { input: parseHeader(src.headerInput), output: parseHeader(src.headerOutput) };
    const bridge = parseBridge(src.bridge);
    const mock = { input: src.mockInput, output: src.mockOutput };
    const contract = parseContract(src.contract);

    for (const role of ROLES) {
        const H = header[role];
        const hname = `${role === "input" ? "InputBridgeApi.h" : "OutputBridgeApi.h"}`;
        for (const s of H.stray) out.push(`${hname}：不是桥名常量的 constexpr 行：${s}`);
        for (const s of H.mismatch) out.push(`${hname}：变量名与字面量对不上：${s}`);

        const docFn = uniq([...get("common", "fn").names, ...get(role, "fn").names]);
        const docEv = uniq([...get("common", "ev").names, ...get(role, "ev").names]);
        const mockFn = arrayLiteral(mock[role], "FUNCTIONS");
        const mockEv = arrayLiteral(mock[role], "EVENTS");
        diffSets(`${role} 函数`, docFn, "UI_BRIDGE.md", H.fn, hname, out);
        diffSets(`${role} 函数`, docFn, "UI_BRIDGE.md", bridge.fn[role], "bridge.js", out);
        diffSets(`${role} 函数`, docFn, "UI_BRIDGE.md", mockFn, `mock/bridge-${role}.js`, out);
        diffSets(`${role} 事件`, docEv, "UI_BRIDGE.md", H.ev, hname, out);
        diffSets(`${role} 事件`, docEv, "UI_BRIDGE.md", bridge.ev[role], "bridge.js", out);
        diffSets(`${role} 事件`, docEv, "UI_BRIDGE.md", mockEv, `mock/bridge-${role}.js`, out);

        const docPair = get("common", "pair").names;
        diffSets(`${role} 配对状态`, docPair, "UI_BRIDGE.md", H.pair, hname, out);

        const docParam = role === "input" ? get("input", "param").names : [];
        diffSets(`${role} 参数`, docParam, "UI_BRIDGE.md", H.param, hname, out);
    }
    if (get("output", "param").names.length) out.push("UI_BRIDGE.md：output 节不应有参数表");

    // 与角色无关的集合：配对状态、诊断字段、Input 参数
    diffSets("配对状态", get("common", "pair").names, "UI_BRIDGE.md", contract.pair, "mock/contract.js", out);
    diffSets("诊断字段", get("common", "diag").names, "UI_BRIDGE.md", contract.diag, "mock/contract.js", out);
    diffSets("Input 参数", get("input", "param").names, "UI_BRIDGE.md", contract.param, "mock/contract.js", out);
    diffSets(
        "Input 参数",
        get("input", "param").names,
        "UI_BRIDGE.md",
        Object.keys(defs || {}),
        "mock PARAM_DEFS",
        out,
    );

    // 参数默认值与自动化标记
    for (const row of get("input", "param").rows) {
        const id = /^`([^`]+)`$/.exec(row[0] || "")?.[1];
        const d = id && defs && Object.prototype.hasOwnProperty.call(defs, id) ? defs[id] : null;
        if (!d) continue;
        const docDef = /^`([^`]*)`$/.exec(row[2] || "")?.[1];
        if (docDef !== String(d.def)) out.push(`参数 ${id} 的默认值：文档 ${docDef}，mock ${String(d.def)}`);
        const docAuto = row[3] === "是";
        if (docAuto !== d.auto) out.push(`参数 ${id} 的可自动化：文档 ${row[3]}，mock ${d.auto ? "是" : "否"}`);
    }
    // 配对状态颜色
    for (const row of get("common", "pair").rows) {
        const id = /^`([^`]+)`$/.exec(row[0] || "")?.[1];
        if (id && contract.colors[id] !== row[1]) {
            out.push(`配对状态 ${id} 的颜色：文档 ${row[1]}，mock ${contract.colors[id] ?? "（无）"}`);
        }
    }
    return out;
}

// ---------------------------------------------------------------- 运行期（mock 真的实现了）

export async function runtimeProblems(root = ROOT) {
    const out = [];
    const url = (p) => pathToFileURL(path.join(root, p)).href;
    const { createInputBackend, FUNCTIONS: inFn, EVENTS: inEv, coerceParam } = await import(url(FILES.mockInput));
    const { createOutputBackend, FUNCTIONS: outFn, EVENTS: outEv } = await import(url(FILES.mockOutput));
    const { PARAM_DEFS, PARAM_IDS } = await import(url(FILES.contract));
    const { createPreviewSession } = await import(url("web-preview/mock/state-driver.js"));

    for (const [role, make, fns, evs] of [
        ["input", createInputBackend, inFn, inEv],
        ["output", createOutputBackend, outFn, outEv],
    ]) {
        const b = make();
        for (const f of fns) if (typeof b.mock[f] !== "function") out.push(`mock ${role} 后端没有实现函数 ${f}`);
        const seen = new Set();
        for (const e of evs) b.mock.addEventListener(e, () => seen.add(e));
        b.mock.requestInitialState();
        for (const e of evs) {
            if (e !== "rinne.error" && !seen.has(e)) out.push(`mock ${role} 后端在初始状态请求之后没有发 ${e}`);
        }
        if (JSON.stringify(sorted(b.core.events)) !== JSON.stringify(sorted(evs)))
            out.push(`mock ${role} 后端的事件表与导出不一致`);
    }
    for (const id of PARAM_IDS) {
        const d = PARAM_DEFS[id];
        if (!d || coerceParam(id, d.def) !== d.def) out.push(`参数 ${id} 的默认值过不了自己的夹取`);
    }
    // ?scenario= 不得走原型链
    for (const bad of ["constructor", "__proto__", "toString"]) {
        const s = createPreviewSession({ role: "input", params: `scenario=${bad}` });
        if (s.info.scenario !== null) out.push(`?scenario=${bad} 不应命中任何场景`);
    }
    return out;
}

// ---------------------------------------------------------------- 入口

export function loadSources(root = ROOT) {
    const src = {};
    for (const [k, rel] of Object.entries(FILES)) {
        const abs = path.join(root, rel);
        if (!fs.existsSync(abs)) throw new Error(`缺文件 ${rel}`);
        src[k] = fs.readFileSync(abs, "utf8");
    }
    return src;
}

export async function checkTree(root = ROOT) {
    const src = loadSources(root);
    const { PARAM_DEFS } = await import(pathToFileURL(path.join(root, FILES.contract)).href);
    return [...compare(src, PARAM_DEFS), ...(await runtimeProblems(root))];
}

/** 把某一方里的一项删掉，应该立刻被查出来。 */
function drop(text, needle) {
    const i = text.indexOf(needle);
    if (i < 0) throw new Error(`自测夹具失效：找不到 ${needle}`);
    return text.slice(0, i) + text.slice(i + needle.length);
}

export async function selfTest(root = ROOT) {
    const src = loadSources(root);
    const { PARAM_DEFS } = await import(pathToFileURL(path.join(root, FILES.contract)).href);
    const cases = [];
    const add = (label, bad, mutate, defs = PARAM_DEFS) => {
        const s = { ...src };
        mutate(s);
        const problems = compare(s, defs);
        cases.push({ label, bad, passed: bad ? problems.length > 0 : problems.length === 0 });
    };
    add("四方一致", false, () => {});
    add("文档表行顺序换了仍然一致", false, (s) => {
        s.doc = s.doc.replace(/(\| `setLang`[^\n]*\n)(\| `setUiScale`[^\n]*\n)/, "$2$1");
    });
    const dropRow = (s, name) => (s.doc = s.doc.replace(new RegExp(`\\| \`${name}\` \\|[^\\n]*\\n`), ""));
    add("文档删一个共用函数", true, (s) => dropRow(s, "setGroup"));
    add("文档删一个 Input 函数", true, (s) => dropRow(s, "panic"));
    add("文档删一个事件", true, (s) => dropRow(s, "rinne\\.pair"));
    add("文档删一个参数", true, (s) => dropRow(s, "polarity"));
    add("文档删一个配对状态", true, (s) => dropRow(s, "stale"));
    add("文档删一个诊断字段", true, (s) => dropRow(s, "diag\\.os"));
    add(
        "文档里 unassigned 的颜色改成琥珀",
        true,
        (s) => (s.doc = s.doc.replace("| `unassigned` | red |", "| `unassigned` | amber |")),
    );
    add("文档里 mix 的默认值改了", true, (s) => (s.doc = s.doc.replace(/(\| `mix` \|[^|]*\| )`25`/, "$1`50`")));
    add("Input 头文件少一个函数", true, (s) => (s.headerInput = drop(s.headerInput, 'kFnSetGroup = "setGroup"')));
    add(
        "Output 头文件少一个函数",
        true,
        (s) => (s.headerOutput = drop(s.headerOutput, 'kFnOpenExternal = "openExternal"')),
    );
    add("Input 头文件少一个事件", true, (s) => (s.headerInput = drop(s.headerInput, '"rinne.meters"')));
    add("Input 头文件少一个参数", true, (s) => (s.headerInput = drop(s.headerInput, 'kParamCeiling = "ceiling"')));
    add(
        "头文件多一个桥名以外的字符串常量",
        true,
        (s) =>
            (s.headerOutput = s.headerOutput.replace(
                "} // namespace",
                'inline constexpr const char* kLabel = "x";\n} // namespace',
            )),
    );
    add(
        "头文件变量名与字面量对不上",
        true,
        (s) => (s.headerOutput = s.headerOutput.replace('kFnSetLang = "setLang"', 'kFnSetLang = "setLanguage"')),
    );
    add("bridge.js 的 Input 名表少一个函数", true, (s) => (s.bridge = drop(s.bridge, '"deletePreset",')));
    add(
        "bridge.js 的 Output 名表少一个事件",
        true,
        (s) =>
            (s.bridge = s.bridge.replace(
                'output: ["rinne.state", "rinne.pair", "rinne.error"]',
                'output: ["rinne.state", "rinne.pair"]',
            )),
    );
    add(
        "Output 头文件少 repairPair",
        true,
        (s) => (s.headerOutput = drop(s.headerOutput, 'kFnRepairPair = "repairPair"')),
    );
    add("bridge.js 的 Input 名表少 moveToFreeGroup", true, (s) => (s.bridge = drop(s.bridge, '"moveToFreeGroup",')));
    add("文档删 resolveTakeover", true, (s) => dropRow(s, "resolveTakeover"));
    add("mock 的 Input 后端少一个函数", true, (s) => (s.mockInput = drop(s.mockInput, '"loadPreset",')));
    add("mock 的 Output 后端少一个函数", true, (s) => (s.mockOutput = drop(s.mockOutput, '"openExternal",')));
    add("mock 的 Input 后端少一个事件", true, (s) => (s.mockInput = drop(s.mockInput, '"rinne.presets"')));
    add("mock 少一个参数 ID", true, (s) => (s.contract = drop(s.contract, '"high_cut",')));
    add("mock 少一个配对状态", true, (s) => (s.contract = drop(s.contract, '"connectedStill",')));
    add("mock 少一个诊断字段", true, (s) => (s.contract = drop(s.contract, '"diag.webview",')));
    add(
        "mock 里 unassigned 的颜色改了",
        true,
        (s) => (s.contract = s.contract.replace('unassigned: "red"', 'unassigned: "amber"')),
    );
    add("mock 的参数默认值与文档不同", true, () => {}, {
        ...PARAM_DEFS,
        feedback: { ...PARAM_DEFS.feedback, def: 10 },
    });
    add("mock 的参数可自动化标记与文档不同", true, () => {}, {
        ...PARAM_DEFS,
        delay_ms: { ...PARAM_DEFS.delay_ms, auto: true },
    });
    return caseResult(cases);
}

if (isMain(import.meta.url)) {
    assertNodeVersion();
    if (process.argv.includes("--self-test")) {
        const r = await selfTest();
        for (const l of r.lines) console.error(l);
        console.log(`check-bridge-parity 自测：${r.ok ? "通过" : "失败"}（坏样例 ${r.bad}、正例 ${r.good}）`);
        process.exitCode = r.ok ? 0 : 1;
    } else {
        const problems = await checkTree();
        for (const p of problems) console.error(`  ${p}`);
        console.log(`check-bridge-parity：${problems.length ? `${problems.length} 处差异` : "四方一致"}`);
        process.exitCode = problems.length ? 1 : 0;
    }
}
