// SPDX-License-Identifier: GPL-3.0-or-later
//
// 桥契约四方对拍：docs/UI_BRIDGE.md、src/{input,output}/*BridgeApi.h、web/shared/bridge.js 的名表、
// web-preview/mock 逐名相等。比的是名字集合：函数、事件、Input 参数 ID、配对状态、诊断字段白名单，
// 另外对拍参数默认值与可自动化标记、配对状态的颜色分组与产生者（仅 Output 的状态）、诊断字段的角色（仅 Input 的节）
// 与顺序，以及 delay_note 的 16 档顺序（文档、mock、src/input/ParamTable.h 三方逐档相同，且按拍数严格升序）。
//   - 文档里带 `<!-- names:<类> -->` 标记的表是机读真源（类：fn、ev、param、pair、diag），第一列的反引号名字就是名字；
//   - 头文件按变量名前缀分类：kFn 函数、kEv 事件、kParam 参数、kPair 配对状态；变量名与字面量也要对得上；
//   - bridge.js 读 BRIDGE_FUNCTIONS 与 BRIDGE_EVENTS 两张名表（纯字面量，不执行）；
//   - mock 读 bridge-{input,output}.js 的 FUNCTIONS / EVENTS 与 contract.js 的常量，另外实例化一遍后端，
//     查每个函数真的存在、每个事件在初始状态请求之后真的会发；两个角色的诊断文本真的只用文档名字表里的名字
//     （Output 是子集），Input 的预览场景真的不会把配对态设成只属于 Output 的状态。
// 诊断名字是「节名.键名」，另有 rinne-diag 与 events 两个整行名字，写法见 UI_BRIDGE.md 的诊断一节。
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
    paramTable: "src/input/ParamTable.h",
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
        diagInputOnly: arrayLiteral(text, "DIAG_INPUT_ONLY_SECTIONS"),
        pairOutputOnly: arrayLiteral(text, "PAIR_OUTPUT_ONLY"),
        colors,
    };
}

/** ParamTable.h：delay_note 的档位 ID，按索引排好；一条都读不到或索引不是 0..n-1 连续时返回 null。 */
export function parseNoteTable(text) {
    const rows = [...text.matchAll(/^\s*RINNE_CHOICE\(\s*delay_note\s*,\s*(\d+)\s*,\s*(\w+)\s*,/gm)].map((m) => [
        Number(m[1]),
        m[2],
    ]);
    if (rows.length === 0) return null;
    rows.sort((a, b) => a[0] - b[0]);
    return rows.every(([index], k) => index === k) ? rows.map((r) => r[1]) : null;
}

/** 文档参数表里 delay_note 一行写的档位顺序；没有这一行返回 null。 */
function docNoteOrder(rows) {
    const row = rows.find((r) => /^`delay_note`$/.test(r[0] || ""));
    return row ? [...(row[1] || "").matchAll(/`(n\d+[td]?)`/g)].map((m) => m[1]) : null;
}

/** 档位 ID 的拍数：n<N> 是 1/N 音符，t 三连音乘 2/3，d 附点乘 3/2；一拍是四分音符。 */
function noteBeats(id) {
    const m = /^n(\d+)([td]?)$/.exec(id);
    if (!m) return Number.NaN;
    return (4 / Number(m[1])) * (m[2] === "t" ? 2 / 3 : m[2] === "d" ? 1.5 : 1);
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
const nameOf = (cell) => /^`([^`]+)`$/.exec(cell || "")?.[1];

/** delay_note 的 16 档顺序：文档、mock、ParamTable.h 三方逐档相同，且每一方都按拍数严格升序。 */
export function delayNoteProblems(doc, mock, table) {
    const out = [];
    const lists = [
        ["UI_BRIDGE.md", doc],
        ["mock PARAM_DEFS", mock],
        ["ParamTable.h", table],
    ];
    for (const [name, list] of lists) {
        if (!list || list.length === 0) {
            out.push(`delay_note 档位顺序：读不到 ${name} 里的档位表`);
            continue;
        }
        for (let i = 1; i < list.length; i++) {
            if (!(noteBeats(list[i - 1]) < noteBeats(list[i]))) {
                out.push(
                    `delay_note 档位顺序：${name} 的第 ${i} 档 ${list[i - 1]} 与第 ${i + 1} 档 ${list[i]} 不是按拍数严格升序`,
                );
                break;
            }
        }
        if (table && list !== table && list.join() !== table.join()) {
            out.push(`delay_note 档位顺序：${name} 是 ${list.join("、")}，ParamTable.h 是 ${table.join("、")}`);
        }
    }
    return out;
}

/** 配对状态表的「产生者」栏：只能写「两端」或「仅 Output」，写「仅 Output」的集合要等于 mock 的 PAIR_OUTPUT_ONLY。 */
export function pairProducerProblems(rows, mockOutputOnly, out) {
    const docOutputOnly = [];
    for (const r of rows) {
        const id = nameOf(r[0]);
        if (!id) continue;
        if (r[2] === "仅 Output") docOutputOnly.push(id);
        else if (r[2] !== "两端")
            out.push(`配对状态 ${id} 的产生者栏要写「两端」或「仅 Output」，实得 ${r[2] ?? "（空）"}`);
    }
    diffSets("仅 Output 的配对状态", docOutputOnly, "UI_BRIDGE.md", mockOutputOnly, "mock/contract.js", out);
}

const DIAG_NAME = /^(?:rinne-diag|events|[a-z]+\.[a-z][a-z0-9_]*)$/;
const diagSection = (name) => (name.includes(".") ? name.slice(0, name.indexOf(".")) : null);

/**
 * 诊断名字表的写法与角色栏：名字写成「节名.键名」（整行名字只有 rinne-diag 与 events）；角色栏只能写「两端」或「仅 Input」，
 * 写「仅 Input」的集合要等于 mock 里属于 DIAG_INPUT_ONLY_SECTIONS 各节的名字；两边的顺序（文本里的出现顺序）相同。
 */
export function diagDocProblems(rows, mockNames, mockInputOnlySections, out) {
    const docNames = [];
    const docInputOnly = [];
    for (const r of rows) {
        const id = nameOf(r[0]);
        if (!id) continue;
        docNames.push(id);
        if (!DIAG_NAME.test(id))
            out.push(`诊断字段 ${id} 不是「节名.键名」的写法（整行名字只有 rinne-diag 与 events）`);
        if (r[1] === "仅 Input") docInputOnly.push(id);
        else if (r[1] !== "两端")
            out.push(`诊断字段 ${id} 的角色栏要写「两端」或「仅 Input」，实得 ${r[1] ?? "（空）"}`);
    }
    if (!mockInputOnlySections) out.push("诊断字段：读不到 mock/contract.js 里的 DIAG_INPUT_ONLY_SECTIONS");
    if (!mockNames || !mockInputOnlySections) return;
    diffSets(
        "仅 Input 的诊断字段",
        docInputOnly,
        "UI_BRIDGE.md",
        mockNames.filter((n) => mockInputOnlySections.includes(diagSection(n))),
        "mock/contract.js（DIAG_INPUT_ONLY_SECTIONS 各节）",
        out,
    );
    if (
        docNames.length === mockNames.length &&
        sorted(docNames).join() === sorted(mockNames).join() &&
        docNames.join() !== mockNames.join()
    ) {
        out.push("诊断字段：UI_BRIDGE.md 与 mock/contract.js 的名字相同，顺序不同（顺序即文本里的出现顺序）");
    }
}

/**
 * 一份 mock 诊断文本相对文档名字表的问题。namesOf 是 bridge-common.js 的 diagNamesOf。
 * 文本里出现表外的名字、Output 出现仅 Input 的名字、（有对端的状态下）缺了本角色该有的名字，
 * 或不是以 rinne-diag v1 开头、以 ui 段收尾，都算问题。
 */
export function diagTextProblems(role, text, rows, namesOf, label = `mock ${role} 后端`) {
    const out = [];
    const want = rows.filter((r) => nameOf(r[0]) && (role === "input" || r[1] !== "仅 Input")).map((r) => nameOf(r[0]));
    const got = namesOf(text);
    for (const n of uniq(got)) if (!want.includes(n)) out.push(`${label}的诊断文本里有名字表之外的 ${n}`);
    for (const n of want) if (!got.includes(n)) out.push(`${label}的诊断文本（有对端时）缺 ${n}`);
    if (!text.startsWith("rinne-diag v1\n")) out.push(`${label}的诊断文本第一行不是 rinne-diag v1`);
    if (!/(?:^|\n)ui: [^\n]*\n$/.test(text)) out.push(`${label}的诊断文本末行不是 ui 段`);
    return out;
}

/**
 * Input 的预览场景不得把配对态设成只属于 Output 的状态。items：{ id, pair } 数组（pair 是场景套用后的配对态）。
 * 没有可查的对象（一个 Input 场景都没有，或 mock 没导出 PAIR_OUTPUT_ONLY 数组）也算问题，免得判据空跑通过。
 */
export function outputOnlyPairProblems(items, outputOnly) {
    const out = [];
    if (!items.length) out.push("预览场景里没有一个 Input 场景，Output 专有配对态的检查没有对象");
    if (!Array.isArray(outputOnly)) {
        out.push("mock/contract.js 没有导出 PAIR_OUTPUT_ONLY 数组");
        return out;
    }
    for (const { id, pair } of items) {
        if (outputOnly.includes(pair)) out.push(`Input 场景 ${id} 的配对态是只属于 Output 的 ${pair}`);
    }
    return out;
}

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
    diagDocProblems(get("common", "diag").rows, contract.diag, contract.diagInputOnly, out);
    pairProducerProblems(get("common", "pair").rows, contract.pairOutputOnly, out);
    out.push(
        ...delayNoteProblems(
            docNoteOrder(get("input", "param").rows),
            defs?.delay_note?.values ?? null,
            parseNoteTable(src.paramTable),
        ),
    );
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
    const { PARAM_DEFS, PARAM_IDS, PAIR_OUTPUT_ONLY } = await import(url(FILES.contract));
    const { createPreviewSession } = await import(url("web-preview/mock/state-driver.js"));
    const { diagNamesOf } = await import(url("web-preview/mock/bridge-common.js"));
    const { SCENES } = await import(url("web-preview/mock/scenarios.js"));
    const diagRows = parseDoc(fs.readFileSync(path.join(root, FILES.doc), "utf8")).common?.diag?.rows ?? [];

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
        // 诊断文本：有对端的状态下，名字与文档名字表里本角色的名字逐项相同（Output 是子集）
        b.model.pair.state = "connected";
        out.push(...diagTextProblems(role, b.mock.copyDiagnostics(false).text, diagRows, diagNamesOf));
    }
    // Input 的预览场景套用后，配对态不得是只属于 Output 的状态
    const inputScenes = SCENES.filter((sc) => sc.role === "input").map((sc) => {
        const sess = createPreviewSession({ role: "input", params: `scenario=${sc.id}` });
        return { id: sc.id, pair: sess.model.pair.state };
    });
    out.push(...outputOnlyPairProblems(inputScenes, PAIR_OUTPUT_ONLY));
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

const swap = (list, i, j) => {
    const out = [...list];
    [out[i], out[j]] = [out[j], out[i]];
    return out;
};

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
    add("文档删一个诊断字段", true, (s) => dropRow(s, "product\\.os"));
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
    add("mock 少一个诊断字段", true, (s) => (s.contract = drop(s.contract, '"ui.webview",')));
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

    // ---- 诊断名字表：写法、角色栏、顺序 ----
    const docRow = (s, name) => new RegExp(`\\| \`${name}\` \\|[^\\n]*\\n`).exec(s.doc)[0];
    add("文档多一个诊断字段", true, (s) => {
        s.doc = s.doc.replace(docRow(s, "track\\.hash"), (r) => `${r}| \`track.extra\` | 两端 | 多出来的。 |\n`);
    });
    // 文档与 mock 同改：名字集合仍然相等，只有写法判据能查出来
    add("文档与 mock 里诊断名字都没有节名", true, (s) => {
        s.doc = s.doc.replace("| `track.hash` |", "| `track_hash` |");
        s.contract = s.contract.replace('"track.hash",', '"track_hash",');
    });
    add("文档与 mock 里整行名字 events 都改成别的无点名字", true, (s) => {
        s.doc = s.doc.replace("| `events` |", "| `event` |");
        s.contract = s.contract.replace('"events",', '"event",');
    });
    add("文档里 loop.mode 的角色栏改成两端", true, (s) => {
        s.doc = s.doc.replace(/(\| `loop\.mode` \| )仅 Input/, "$1两端");
    });
    add("文档里 track.hash 的角色栏改成仅 Input", true, (s) => {
        s.doc = s.doc.replace(/(\| `track\.hash` \| )两端/, "$1仅 Input");
    });
    add("文档里诊断角色栏写了别的", true, (s) => {
        s.doc = s.doc.replace(/(\| `track\.hash` \| )两端/, "$1仅 Output");
    });
    add("文档里诊断名字顺序与 mock 不同", true, (s) => {
        const a = docRow(s, "track\\.relation");
        const b = docRow(s, "track\\.hash");
        s.doc = s.doc.replace(a + b, b + a);
    });
    add("mock 的仅 Input 节表少一节", true, (s) => (s.contract = drop(s.contract, '"order", ')));
    add("mock 的仅 Input 节表多一节", true, (s) => {
        s.contract = s.contract.replace(
            '["loop", "sync", "order", "safety"]',
            '["loop", "sync", "order", "safety", "peer"]',
        );
    });

    // ---- 配对状态的产生者 ----
    add("文档里 bypassed 的产生者改成两端", true, (s) => {
        s.doc = s.doc.replace("| `bypassed` | gray | 仅 Output |", "| `bypassed` | gray | 两端 |");
    });
    add("文档里 connected 的产生者改成仅 Output", true, (s) => {
        s.doc = s.doc.replace("| `connected` | green | 两端 |", "| `connected` | green | 仅 Output |");
    });
    add("文档里配对状态的产生者栏写了别的", true, (s) => {
        s.doc = s.doc.replace("| `connected` | green | 两端 |", "| `connected` | green | Input |");
    });
    add("mock 的 PAIR_OUTPUT_ONLY 空了", true, (s) => {
        s.contract = s.contract.replace('PAIR_OUTPUT_ONLY = ["bypassed"]', "PAIR_OUTPUT_ONLY = []");
    });

    // ---- delay_note 的 16 档顺序 ----
    const OLD_NOTES = "n64,n32t,n32,n16t,n16,n16d,n8t,n8,n8d,n4t,n4,n4d,n2t,n2,n2d,n1".split(",");
    const NEW_NOTES = PARAM_DEFS.delay_note.values;
    const docOrderOf = (list) => list.map((n) => `\`${n}\``).join("、");
    add("三方顺序一致且按拍数升序", false, () => {});
    add("文档的 delay_note 换回音符族分组顺序", true, (s) => {
        s.doc = s.doc.replace(docOrderOf(NEW_NOTES), docOrderOf(OLD_NOTES));
    });
    add("文档的 delay_note 少一档", true, (s) => {
        s.doc = s.doc.replace(docOrderOf(NEW_NOTES), docOrderOf(NEW_NOTES.slice(0, -1)));
    });
    add("mock 的 delay_note 换回音符族分组顺序", true, () => {}, {
        ...PARAM_DEFS,
        delay_note: { ...PARAM_DEFS.delay_note, values: OLD_NOTES },
    });
    add("mock 的 delay_note 两档对调", true, () => {}, {
        ...PARAM_DEFS,
        delay_note: { ...PARAM_DEFS.delay_note, values: swap(NEW_NOTES, 5, 6) },
    });
    add("ParamTable.h 的 delay_note 两档对调", true, (s) => {
        s.paramTable = s.paramTable
            .replace("RINNE_CHOICE(delay_note, 5, n8t,", "RINNE_CHOICE(delay_note, 5, n16d,")
            .replace("RINNE_CHOICE(delay_note, 6, n16d,", "RINNE_CHOICE(delay_note, 6, n8t,");
    });
    add("ParamTable.h 里读不到 delay_note", true, (s) => {
        s.paramTable = s.paramTable.replace(/RINNE_CHOICE\(delay_note,/g, "RINNE_CHOICE(delay_notes,");
    });
    add("ParamTable.h 的 delay_note 索引有空档（末档的索引 15 改成 16，顺序不变）", true, (s) => {
        s.paramTable = s.paramTable.replace("RINNE_CHOICE(delay_note, 15, n1,", "RINNE_CHOICE(delay_note, 16, n1,");
    });
    add(
        "三方都改回音符族分组顺序（彼此一致，但不是按拍数严格升序）",
        true,
        (s) => {
            s.doc = s.doc.replace(docOrderOf(NEW_NOTES), docOrderOf(OLD_NOTES));
            let k = 0;
            s.paramTable = s.paramTable.replace(
                /RINNE_CHOICE\(delay_note, (\d+), (\w+),/g,
                (_m, i) => `RINNE_CHOICE(delay_note, ${i}, ${OLD_NOTES[k++]},`,
            );
        },
        { ...PARAM_DEFS, delay_note: { ...PARAM_DEFS.delay_note, values: OLD_NOTES } },
    );

    // ---- 运行期的两个纯判据：诊断文本、Input 场景的配对态 ----
    const { diagNamesOf } = await import(pathToFileURL(path.join(root, "web-preview/mock/bridge-common.js")).href);
    const { createInputBackend } = await import(pathToFileURL(path.join(root, FILES.mockInput)).href);
    const { createOutputBackend } = await import(pathToFileURL(path.join(root, FILES.mockOutput)).href);
    const diagRows = parseDoc(src.doc).common.diag.rows;
    const textOf = (make) => {
        const b = make({ opts: { autoInitial: false } });
        b.model.pair.state = "connected";
        return b.mock.copyDiagnostics(false).text;
    };
    const inText = textOf(createInputBackend);
    const outText = textOf(createOutputBackend);
    const addText = (label, bad, role, text) => {
        const problems = diagTextProblems(role, text, diagRows, diagNamesOf);
        cases.push({ label, bad, passed: bad ? problems.length > 0 : problems.length === 0 });
    };
    addText("Input 的诊断文本与名字表一致", false, "input", inText);
    addText("Output 的诊断文本与名字表一致（子集）", false, "output", outText);
    addText("Input 的诊断文本多一行表外的节", true, "input", inText.replace("\nui: ", "\nextra: x=1\nui: "));
    addText("Input 的诊断文本多一个表外的键", true, "input", inText.replace(" hash=none", " hash=none user=x"));
    addText("Input 的诊断文本少一节", true, "input", inText.replace(/\nsafety: [^\n]*/, ""));
    addText(
        "Output 的诊断文本里出现 loop 节",
        true,
        "output",
        outText.replace("\nprepare: ", "\nloop: d_user_ms=100.000\nprepare: "),
    );
    // 名字一个不少，只是位置不对：格式标识不在第一行、ui 段不在末行
    addText(
        "诊断文本的格式标识不在第一行",
        true,
        "input",
        inText.replace(/^rinne-diag v1\n([^\n]*\n)/, "$1rinne-diag v1\n"),
    );
    addText("诊断文本缺格式标识", true, "input", inText.replace("rinne-diag v1\n", ""));
    addText(
        "诊断文本的 ui 段不在末行",
        true,
        "input",
        inText.replace(/(ui: [^\n]*\n)$/, "").replace("events(", `${/ui: [^\n]*\n$/.exec(inText)[0]}events(`),
    );
    addText("诊断文本缺 ui 段", true, "input", inText.replace(/ui: [^\n]*\n$/, ""));
    addText("诊断文本把事件段标题写成别的样子", true, "input", inText.replace("events(last 4 of 4):", "events:"));
    // opts.outputOnly 显式给 undefined 表示「没导出」，所以按键是否存在取默认值，不用解构默认值
    const sceneCase = (label, bad, pair, opts = {}) => {
        const list = opts.items ?? [
            { id: "in-a", pair: "connected" },
            { id: "in-b", pair },
        ];
        const problems = outputOnlyPairProblems(list, "outputOnly" in opts ? opts.outputOnly : ["bypassed"]);
        cases.push({ label, bad, passed: bad ? problems.length > 0 : problems.length === 0 });
    };
    sceneCase("Input 场景的配对态都不是 Output 专有的", false, "connectedStill");
    sceneCase("Input 场景的配对态是 bypassed", true, "bypassed");
    sceneCase("一个 Input 场景都没有", true, null, { items: [] });
    sceneCase("mock 没导出 PAIR_OUTPUT_ONLY", true, "connectedStill", { outputOnly: undefined });
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
