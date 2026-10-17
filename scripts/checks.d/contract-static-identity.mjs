// SPDX-License-Identifier: GPL-3.0-or-later
//
// 插件身份的静态对拍：不编译，直接读 src/{input,output}/CMakeLists.txt 里 juce_add_plugin 的身份字段和根 CMakeLists.txt 里的
// 几个 JUCE 宏，与 tests/golden/identity.json 逐项比对。身份在首个对外发出的构建产物里就永久冻结，改一个字母，已装的插件
// 在宿主里就是另一个插件，保存的工程也找不到它。比对的内容：
//   - 正式版与探针版两套 PRODUCT_NAME / PLUGIN_CODE / BUNDLE_ID（身份字段恰好这三项，多一项少一项都判红）；
//   - 两个插件共用的 PLUGIN_MANUFACTURER_CODE、FORMATS、VST3_CATEGORIES、AU_MAIN_TYPE、IS_SYNTH、三个 MIDI 开关；
//   - 目标名，以及目标上确实挂了 ${RINNE_PLUGIN_DEFINITIONS}（宏不生效，参数 ID 就是另一套）；
//   - 根 CMakeLists.txt 的格式清单（VST3，Apple 上加 AU）；
//   - JUCE_VST3_CAN_REPLACE_VST2、JUCE_FORCE_USE_LEGACY_PARAM_IDS、JUCE_USE_STUDIO_ONE_COMPATIBLE_PARAMETERS 三个宏：
//     决定宿主工程里保存的参数 ID。每个宏在全部 CMake 文件里只许出现一次、值与 golden 一致，免得别处用后写覆盖。
// 白名单式：身份敏感的关键字（IDENTITY_KEYWORDS）与 JUCE 的身份属性，只许出现在 golden 认可的 juce_add_plugin 调用和
// 两个身份 set(…) 里；CMake 树里任何其他地方出现一律判红（含 cmake/*.cmake、set_target_properties、add_compile_options、
// -D / /D 形式的宏）。身份变量 _rinne_<名>_identity 在全部 CMake 文件里恰好出现三次（两处 set、一处使用）。
// 构建之后，两个 .vst3 的 moduleinfo.json（CID、名称、厂商、子类、版本）还要与 golden 对拍：
//   node scripts/checks.d/contract-static-identity.mjs --moduleinfo <构建目录> [--probe]
// 解析器只认本仓手写 CMake 的写法（关键字全大写、值一个个排开、探针分支写成 if(RINNE_PROBE_BUILD) … else() … endif()）。
// 有意改身份是兼容性事件：按 CLAUDE.md 的「冻结契约」一节走变更文档，并同一个 PR 更新 golden。
//   node scripts/run-checks.mjs --only contract-static-identity [--self-test | --mutation-check]

import fs from "node:fs";
import path from "node:path";
import { caseResult, isMain } from "../lib/check-kit.mjs";

const GOLDEN = "tests/golden/identity.json";
const ROOT_CMAKE = "CMakeLists.txt";
const IDENTITY_KEYS = ["PRODUCT_NAME", "PLUGIN_CODE", "BUNDLE_ID"];
/** 身份敏感的 juce_add_plugin 关键字：值等于 golden，或者根本不出现。check-commits.mjs 的契约守卫复用这一份。 */
export const IDENTITY_KEYWORDS = [
    "PRODUCT_NAME",
    "PLUGIN_CODE",
    "BUNDLE_ID",
    "PLUGIN_MANUFACTURER_CODE",
    "PLUGIN_NAME",
    "USE_LEGACY_COMPATIBILITY_PLUGIN_CODE",
    "AU_EXPORT_PREFIX",
    "AU_SANDBOX_SAFE",
    "AU_MAIN_TYPE",
    "LV2URI",
    "AAX_IDENTIFIER",
    "VST2_CATEGORY",
    "VST3_CATEGORIES",
    "FORMATS",
    "IS_SYNTH",
    "NEEDS_MIDI_INPUT",
    "NEEDS_MIDI_OUTPUT",
    "IS_MIDI_EFFECT",
];
/** 其他经 target 属性、变量改身份的写法。 */
const IDENTITY_PATTERNS = [
    "JUCE_(?:PLUGIN_CODE|PLUGIN_MANUFACTURER_CODE|BUNDLE_ID|PLUGIN_NAME|PRODUCT_NAME|AU_\\w+|USE_LEGACY_COMPATIBILITY_PLUGIN_CODE)",
    "RINNE_PLUGIN_FORMATS",
    "RINNE_PLUGIN_DEFINITIONS",
    "_rinne_\\w+_identity",
    "JUCE_VST3_CAN_REPLACE_VST2",
    "JUCE_FORCE_USE_LEGACY_PARAM_IDS",
    "JUCE_USE_STUDIO_ONE_COMPATIBLE_PARAMETERS",
];
const WORD = (alt) => `(?:(?<![\\w])|(?<=[-/]D))(?:${alt})(?![\\w])`;
/** 契约守卫按行判 CMake 改动时用：一行里出现任何身份敏感的词。 */
export const IDENTITY_LINE_PATTERN = new RegExp(WORD([...IDENTITY_KEYWORDS, ...IDENTITY_PATTERNS].join("|")));
/** 全大写但是是取值而不是关键字的词。 */
const VALUES = new Set(["TRUE", "FALSE", "ON", "OFF", "YES", "NO", "VST3", "VST", "AU", "AUV3", "AAX", "LV2", "UNITY"]);

/** 去掉 # 行注释与 #[[ ]] 块注释；引号里的 # 不动。 */
function stripComments(text) {
    const noBlock = text.replace(/#\[(=*)\[[\s\S]*?\]\1\]/g, "");
    return noBlock
        .split(/\r?\n/)
        .map((line) => {
            let quote = false;
            for (let i = 0; i < line.length; i++) {
                if (line[i] === '"') quote = !quote;
                else if (line[i] === "#" && !quote) return line.slice(0, i);
            }
            return line;
        })
        .join("\n");
}

/** 按空白拆词，双引号里的算一个词（引号去掉）。 */
function tokens(text) {
    return [...text.matchAll(/"([^"]*)"|([^\s"]+)/g)].map((m) => (m[1] !== undefined ? m[1] : m[2]));
}

/** 取 name( … ) 括号里的原文（配对括号，引号里的括号不算）；每次调用返回全部出现。 */
function calls(code, name) {
    const out = [];
    const re = new RegExp(`(?<![\\w])${name}\\s*\\(`, "gi");
    for (let m; (m = re.exec(code));) {
        let depth = 1;
        let quote = false;
        let i = re.lastIndex;
        for (; i < code.length && depth > 0; i++) {
            const c = code[i];
            if (c === '"') quote = !quote;
            else if (!quote && c === "(") depth++;
            else if (!quote && c === ")") depth--;
        }
        out.push(code.slice(re.lastIndex, i - 1));
    }
    return out;
}

/** 关键字后面跟一串值；关键字是全大写且不在取值表里的词。 */
function keywordMap(toks) {
    const map = {};
    let key = null;
    for (const t of toks) {
        if (/^[A-Z][A-Z0-9_]+$/.test(t) && !VALUES.has(t)) {
            key = t;
            map[key] = map[key] ?? [];
        } else if (key) map[key].push(t);
    }
    return map;
}

const same = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);

function compareMap(label, got, want, errors, { exact }) {
    for (const [k, v] of Object.entries(want)) {
        if (!got[k]) errors.push(`${label}：缺 ${k}（golden 是 ${JSON.stringify(v)}）`);
        else if (!same(got[k], v))
            errors.push(`${label}：${k} 现在是 ${JSON.stringify(got[k])}，golden 是 ${JSON.stringify(v)}`);
    }
    if (exact)
        for (const k of Object.keys(got))
            if (!(k in want)) errors.push(`${label}：多了 ${k}，身份字段只有 ${Object.keys(want).join("、")}`);
}

/** 一个插件目录的 CMake：返回 { target, identity: {formal, probe}, shared, errors }。 */
function parsePlugin(text, label) {
    const errors = [];
    const code = stripComments(text);
    const plugin = calls(code, "juce_add_plugin");
    if (plugin.length !== 1) return { errors: [`${label}：juce_add_plugin 应当恰好一处，现在 ${plugin.length} 处`] };
    const toks = tokens(plugin[0]);
    const target = toks[0];
    const rest = toks.slice(1);

    // ${_rinne_xxx_identity} 展开成探针分支与正式分支各自的 set(...)
    const ref = rest.find((t) => /^\$\{_rinne_\w+_identity\}$/.test(t));
    const direct = keywordMap(rest.filter((t) => t !== ref));
    const identity = {};
    const allowedRegions = [plugin[0]];
    for (const a of calls(code, "target_compile_definitions"))
        if (/^\s*\S+\s+PUBLIC\s+\$\{RINNE_PLUGIN_DEFINITIONS\}\s*$/.test(a) && tokens(a)[0] === target)
            allowedRegions.push(a);
    if (ref) {
        const name = ref.slice(2, -1);
        const branch = /if\s*\(\s*RINNE_PROBE_BUILD\s*\)([\s\S]*?)\belse\s*\(\s*\)([\s\S]*?)\bendif\s*\(\s*\)/i.exec(
            code,
        );
        if (!branch) errors.push(`${label}：找不到 if(RINNE_PROBE_BUILD) … else() … endif()`);
        else {
            for (const [flavor, body] of [
                ["probe", branch[1]],
                ["formal", branch[2]],
            ]) {
                const sets = calls(body, "set").filter((a) => tokens(a)[0] === name);
                if (sets.length !== 1)
                    errors.push(`${label}：${flavor} 分支里应当恰好一处 set(${name} …)，现在 ${sets.length} 处`);
                else {
                    identity[flavor] = keywordMap(tokens(sets[0]).slice(1));
                    allowedRegions.push(sets[0]);
                }
            }
        }
    } else {
        errors.push(`${label}：juce_add_plugin 没有引用 \${_rinne_*_identity}，身份字段应当按正式版与探针版两套分开写`);
    }
    // 直接写在 juce_add_plugin 里的身份字段（不分正式与探针）也算身份，但不该有
    for (const k of IDENTITY_KEYS)
        if (k in direct) errors.push(`${label}：${k} 不能直接写在 juce_add_plugin 里，须在正式与探针两套里分别写`);
    return {
        target,
        identity,
        shared: direct,
        code,
        errors,
        allowedRegions,
        identityVar: ref ? ref.slice(2, -1) : null,
    };
}

/**
 * 根 CMakeLists 里认可的写法挖掉：set(RINNE_PLUGIN_FORMATS …)、if(APPLE) 里的 list(APPEND RINNE_PLUGIN_FORMATS …)、
 * set(RINNE_PLUGIN_DEFINITIONS …) 与 list(APPEND RINNE_PLUGIN_DEFINITIONS …)。别处（if(WIN32) 里、if 之外）
 * 追加格式或改写这两张清单，剩下的文本里仍有这些词，就判越界。
 */
function cleanRoot(code) {
    let out = code;
    for (const inner of calls(code, "set")) {
        const first = tokens(inner)[0];
        if (first === "RINNE_PLUGIN_FORMATS" || first === "RINNE_PLUGIN_DEFINITIONS") out = out.replace(inner, " ");
    }
    for (const inner of calls(code, "list")) {
        const t = tokens(inner);
        if (t[0] === "APPEND" && t[1] === "RINNE_PLUGIN_DEFINITIONS") out = out.replace(inner, " ");
    }
    return out.replace(/if\s*\(\s*APPLE\s*\)[\s\S]*?endif\s*\(\s*\)/gi, (block) => {
        let cleaned = block;
        for (const inner of calls(block, "list")) {
            const t = tokens(inner);
            if (t[0] === "APPEND" && t[1] === "RINNE_PLUGIN_FORMATS") cleaned = cleaned.replace(inner, " ");
        }
        return cleaned;
    });
}

/** 根 CMakeLists 的格式清单与全部 CMake 文件里的宏。 */
function parseFormats(rootText) {
    const code = stripComments(rootText);
    const base = calls(code, "set")
        .map(tokens)
        .filter((t) => t[0] === "RINNE_PLUGIN_FORMATS")
        .map((t) => t.slice(1));
    const apple = /if\s*\(\s*APPLE\s*\)([\s\S]*?)endif\s*\(\s*\)/gi;
    const added = [];
    for (let m; (m = apple.exec(code));) {
        for (const a of calls(m[1], "list")) {
            const t = tokens(a);
            if (t[0] === "APPEND" && t[1] === "RINNE_PLUGIN_FORMATS") added.push(...t.slice(2));
        }
    }
    return { base: base.length === 1 ? base[0] : null, apple: added };
}

/**
 * golden：identity.json 的内容；read(rel) 返回文本或 null；cmakeFiles：要扫宏的全部 CMake 文件（相对路径）。
 * 返回错误列表。
 */
export function checkIdentity({ golden, read, cmakeFiles }) {
    const errors = [];

    const rootText = read(ROOT_CMAKE);
    if (rootText === null) return [`缺 ${ROOT_CMAKE}`];
    const formats = parseFormats(rootText);
    if (!formats.base) errors.push(`${ROOT_CMAKE}：RINNE_PLUGIN_FORMATS 应当恰好一处 set(…)`);
    else if (!same(formats.base, golden.formats.base))
        errors.push(
            `${ROOT_CMAKE}：基础格式是 ${JSON.stringify(formats.base)}，golden 是 ${JSON.stringify(golden.formats.base)}`,
        );
    if (!same(formats.apple, golden.formats.apple))
        errors.push(
            `${ROOT_CMAKE}：Apple 上追加的格式是 ${JSON.stringify(formats.apple)}，golden 是 ${JSON.stringify(golden.formats.apple)}`,
        );

    // 参数 ID 的宏：全部 CMake 文件里每个宏恰好一处、值对
    const texts = cmakeFiles.map((f) => ({ f, code: stripComments(read(f) ?? "") }));
    for (const [macro, want] of Object.entries(golden.definitions)) {
        const hits = texts.flatMap(({ f, code }) =>
            [...code.matchAll(new RegExp(`${WORD(macro)}(?:\\s*=\\s*([^\\s)"]+))?`, "g"))].map((m) => ({
                f,
                value: m[1],
            })),
        );
        if (hits.length !== 1)
            errors.push(
                `${macro} 在 CMake 文件里出现 ${hits.length} 处（${hits.map((h) => h.f).join("、") || "无"}），应当恰好一处`,
            );
        else if (hits[0].value !== want)
            errors.push(`${hits[0].f}：${macro} 的值是 ${hits[0].value ?? "(空)"}，golden 是 ${want}`);
    }

    // 白名单：认可的区域（juce_add_plugin 调用、两个身份 set）从文本里挖掉，剩下的地方出现敏感词就是越界
    const allowed = new Map(); // 文件 -> 要挖掉的片段
    const identityVars = [];
    for (const [name, spec] of Object.entries(golden.plugins)) {
        const text = read(spec.file);
        if (text === null) {
            errors.push(`缺 ${spec.file}`);
            continue;
        }
        const label = spec.file;
        const p = parsePlugin(text, label);
        errors.push(...p.errors);
        if (!p.code) continue;
        allowed.set(spec.file, p.allowedRegions);
        if (p.identityVar) identityVars.push(p.identityVar);
        if (p.target !== spec.target) errors.push(`${label}：目标名是 ${p.target}，golden 是 ${spec.target}`);
        for (const flavor of ["formal", "probe"]) {
            if (!p.identity[flavor]) continue;
            compareMap(`${label}（${flavor}）`, p.identity[flavor], spec.identity[flavor], errors, { exact: true });
        }
        compareMap(`${label}`, p.shared, spec.shared, errors, { exact: false });
        // juce_add_plugin 里身份敏感的关键字，golden 没列的也不许有
        for (const k of IDENTITY_KEYWORDS)
            if (k in p.shared && !(k in spec.shared)) errors.push(`${label}：多了身份相关的 ${k}，golden 里没有`);
        // 宏要真的挂到插件目标上，并且是 PUBLIC：包装层的编译单元也要看到
        const attach = new RegExp(
            `target_compile_definitions\\s*\\(\\s*${spec.target}\\s+PUBLIC\\s+\\$\\{RINNE_PLUGIN_DEFINITIONS\\}\\s*\\)`,
        );
        if (!attach.test(p.code))
            errors.push(
                `${label}：${spec.target} 上没有 target_compile_definitions(${spec.target} PUBLIC \${RINNE_PLUGIN_DEFINITIONS})`,
            );
        if (!name) errors.push("golden 里有空的插件名");
    }

    const sensitive = new RegExp(WORD([...IDENTITY_KEYWORDS, ...IDENTITY_PATTERNS].join("|")), "g");
    for (const { f, code } of texts) {
        let rest = f === ROOT_CMAKE ? cleanRoot(code) : code;
        for (const region of allowed.get(f) ?? []) rest = rest.replace(region, " ");
        const found = [...new Set([...rest.matchAll(sensitive)].map((m) => m[0]))];
        // 根 CMakeLists：格式清单与宏清单的认可写法已在 cleanRoot 里挖掉，剩下的才算越界
        const stray = found;
        if (stray.length)
            errors.push(`${f}：身份相关的 ${stray.join("、")} 只许出现在 golden 认可的 juce_add_plugin 与身份 set 里`);
    }
    // 其他文件里除了 golden 认可的位置，也不许再用这些词来引用身份变量、格式清单与宏清单
    for (const v of identityVars) {
        const n = texts.reduce((sum, { code }) => sum + [...code.matchAll(new RegExp(WORD(v), "g"))].length, 0);
        if (n !== 3) errors.push(`${v} 在 CMake 文件里出现 ${n} 次，应当恰好三次（两处 set、一处使用）`);
    }
    return errors;
}

// ---- 自测 ----

const FIXTURE_ROOT = `
cmake_minimum_required(VERSION 3.22)
project(Rinne VERSION 0.9.0)
set(RINNE_PLUGIN_FORMATS VST3)
if(APPLE)
  list(APPEND RINNE_PLUGIN_FORMATS AU)
endif()
# JUCE_VST3_CAN_REPLACE_VST2=1 只是注释，不算
set(RINNE_PLUGIN_DEFINITIONS
  JUCE_VST3_CAN_REPLACE_VST2=0
  JUCE_FORCE_USE_LEGACY_PARAM_IDS=0
  JUCE_USE_STUDIO_ONE_COMPATIBLE_PARAMETERS=1
  JUCE_WEB_BROWSER=1
)
`;

const fixturePlugin = (target, product, code, probeCode, bundle) => `
if(RINNE_PROBE_BUILD)
  set(_rinne_${target}_identity
    PRODUCT_NAME "${product} Probe"
    PLUGIN_CODE ${probeCode}
    BUNDLE_ID ${bundle}.probe)
else()
  set(_rinne_${target}_identity
    PRODUCT_NAME "${product}"
    PLUGIN_CODE ${code}
    BUNDLE_ID ${bundle})
endif()
juce_add_plugin(${target}
  \${_rinne_${target}_identity}
  PLUGIN_MANUFACTURER_CODE Snch
  COMPANY_NAME "Synchain"
  FORMATS \${RINNE_PLUGIN_FORMATS}
  VST3_CATEGORIES Fx Delay
  AU_MAIN_TYPE kAudioUnitType_Effect # 注释
  AU_SANDBOX_SAFE FALSE
  IS_SYNTH FALSE
  NEEDS_MIDI_INPUT FALSE
  NEEDS_MIDI_OUTPUT FALSE
  IS_MIDI_EFFECT FALSE)
target_compile_definitions(${target} PUBLIC \${RINNE_PLUGIN_DEFINITIONS})
`;

const shared = (extra = {}) => ({
    PLUGIN_MANUFACTURER_CODE: ["Snch"],
    FORMATS: ["${RINNE_PLUGIN_FORMATS}"],
    VST3_CATEGORIES: ["Fx", "Delay"],
    AU_MAIN_TYPE: ["kAudioUnitType_Effect"],
    AU_SANDBOX_SAFE: ["FALSE"],
    IS_SYNTH: ["FALSE"],
    NEEDS_MIDI_INPUT: ["FALSE"],
    NEEDS_MIDI_OUTPUT: ["FALSE"],
    IS_MIDI_EFFECT: ["FALSE"],
    ...extra,
});

const FIXTURE_GOLDEN = {
    moduleinfo: {
        vendor: "Synchain",
        cidPrefix: "ABCDEF01",
        classes: {
            "Audio Module Class": "9182FAEB",
            "Component Controller Class": "1234ABCD",
            "Plugin Compatibility Class": "C0DEF00D",
        },
    },
    formats: { base: ["VST3"], apple: ["AU"] },
    definitions: {
        JUCE_VST3_CAN_REPLACE_VST2: "0",
        JUCE_FORCE_USE_LEGACY_PARAM_IDS: "0",
        JUCE_USE_STUDIO_ONE_COMPATIBLE_PARAMETERS: "1",
    },
    plugins: {
        input: {
            file: "src/input/CMakeLists.txt",
            target: "RinneInput",
            identity: {
                formal: {
                    PRODUCT_NAME: ["Rinne Input"],
                    PLUGIN_CODE: ["Rnni"],
                    BUNDLE_ID: ["com.synchain.rinne.input"],
                },
                probe: {
                    PRODUCT_NAME: ["Rinne Input Probe"],
                    PLUGIN_CODE: ["Rnpi"],
                    BUNDLE_ID: ["com.synchain.rinne.input.probe"],
                },
            },
            shared: shared(),
        },
        output: {
            file: "src/output/CMakeLists.txt",
            target: "RinneOutput",
            identity: {
                formal: {
                    PRODUCT_NAME: ["Rinne Output"],
                    PLUGIN_CODE: ["Rnno"],
                    BUNDLE_ID: ["com.synchain.rinne.output"],
                },
                probe: {
                    PRODUCT_NAME: ["Rinne Output Probe"],
                    PLUGIN_CODE: ["Rnpo"],
                    BUNDLE_ID: ["com.synchain.rinne.output.probe"],
                },
            },
            shared: shared(),
        },
    },
};

const FIXTURE_FILES = () => ({
    [ROOT_CMAKE]: FIXTURE_ROOT,
    "src/input/CMakeLists.txt": fixturePlugin("RinneInput", "Rinne Input", "Rnni", "Rnpi", "com.synchain.rinne.input"),
    "src/output/CMakeLists.txt": fixturePlugin(
        "RinneOutput",
        "Rinne Output",
        "Rnno",
        "Rnpo",
        "com.synchain.rinne.output",
    ),
});

/** 把 files 里 rel 这个文件的第一处 from 换成 to；没换到就抛错，免得自测因夹具变了而空转。 */
function edit(files, rel, from, to) {
    if (!files[rel].includes(from)) throw new Error(`自测夹具里 ${rel} 没有「${from}」`);
    return { ...files, [rel]: files[rel].replace(from, to) };
}

const cmakeList = (files) => Object.keys(files);

export function selfTest() {
    const run = (files, golden = FIXTURE_GOLDEN) =>
        checkIdentity({ golden, read: (r) => files[r] ?? null, cmakeFiles: cmakeList(files) });
    const base = FIXTURE_FILES();
    const IN = "src/input/CMakeLists.txt";
    const OUT = "src/output/CMakeLists.txt";
    const goldenWith = (mutate) => {
        const g = structuredClone(FIXTURE_GOLDEN);
        mutate(g);
        return g;
    };
    const cases = [
        ["夹具与 golden 一致", false, run(base)],
        [
            "改注释、公司名、键顺序和空白不算",
            false,
            run({
                ...base,
                [IN]: `# 新注释\n${base[IN].replace('COMPANY_NAME "Synchain"', 'COMPANY_NAME "Other"').replace("IS_SYNTH FALSE\n  NEEDS_MIDI_INPUT FALSE", "NEEDS_MIDI_INPUT FALSE   IS_SYNTH FALSE")}`,
            }),
        ],
        [
            "Input 的 PLUGIN_CODE 改一个字母（正式版）",
            true,
            run(edit(base, IN, "PLUGIN_CODE Rnni", "PLUGIN_CODE Rnnj")),
        ],
        ["Input 的 PLUGIN_CODE 改一个字母（探针）", true, run(edit(base, IN, "PLUGIN_CODE Rnpi", "PLUGIN_CODE Rnpj"))],
        ["Output 的 PLUGIN_CODE 改一个字母", true, run(edit(base, OUT, "PLUGIN_CODE Rnno", "PLUGIN_CODE Rnnp"))],
        ["PLUGIN_CODE 大小写变了", true, run(edit(base, IN, "PLUGIN_CODE Rnni", "PLUGIN_CODE RNNI"))],
        ["PRODUCT_NAME 改了", true, run(edit(base, OUT, 'PRODUCT_NAME "Rinne Output"', 'PRODUCT_NAME "Rinne Send"'))],
        [
            "BUNDLE_ID 改了",
            true,
            run(edit(base, IN, "BUNDLE_ID com.synchain.rinne.input)", "BUNDLE_ID com.synchain.rinne.in)")),
        ],
        ["厂商码改了", true, run(edit(base, IN, "PLUGIN_MANUFACTURER_CODE Snch", "PLUGIN_MANUFACTURER_CODE Snc2"))],
        ["AU 类型改了", true, run(edit(base, OUT, "kAudioUnitType_Effect", "kAudioUnitType_MusicEffect"))],
        ["IS_SYNTH 改成 TRUE", true, run(edit(base, IN, "IS_SYNTH FALSE", "IS_SYNTH TRUE"))],
        ["MIDI 输入打开", true, run(edit(base, OUT, "NEEDS_MIDI_INPUT FALSE", "NEEDS_MIDI_INPUT TRUE"))],
        ["VST3 分类改了", true, run(edit(base, IN, "VST3_CATEGORIES Fx Delay", "VST3_CATEGORIES Fx"))],
        ["身份里多出一个字段", true, run(edit(base, IN, "PLUGIN_CODE Rnni", "PLUGIN_CODE Rnni\n    PLUGIN_NAME x"))],
        [
            "身份字段直接写进 juce_add_plugin",
            true,
            run(edit(base, IN, "PLUGIN_MANUFACTURER_CODE Snch", "PRODUCT_NAME x PLUGIN_MANUFACTURER_CODE Snch")),
        ],
        [
            "目标改名",
            true,
            run(
                edit(
                    edit(base, IN, "juce_add_plugin(RinneInput", "juce_add_plugin(RinneIn"),
                    IN,
                    "target_compile_definitions(RinneInput",
                    "target_compile_definitions(RinneIn",
                ),
            ),
        ],
        [
            "探针分支被删",
            true,
            run({
                ...base,
                [IN]: base[IN].replace(/if\(RINNE_PROBE_BUILD\)[\s\S]*?else\(\)/, "").replace("endif()", ""),
            }),
        ],
        [
            "JUCE_VST3_CAN_REPLACE_VST2 翻成 1",
            true,
            run(edit(base, ROOT_CMAKE, "JUCE_VST3_CAN_REPLACE_VST2=0", "JUCE_VST3_CAN_REPLACE_VST2=1")),
        ],
        [
            "JUCE_FORCE_USE_LEGACY_PARAM_IDS 翻成 1",
            true,
            run(edit(base, ROOT_CMAKE, "JUCE_FORCE_USE_LEGACY_PARAM_IDS=0", "JUCE_FORCE_USE_LEGACY_PARAM_IDS=1")),
        ],
        [
            "JUCE_USE_STUDIO_ONE_COMPATIBLE_PARAMETERS 翻成 0",
            true,
            run(
                edit(
                    base,
                    ROOT_CMAKE,
                    "JUCE_USE_STUDIO_ONE_COMPATIBLE_PARAMETERS=1",
                    "JUCE_USE_STUDIO_ONE_COMPATIBLE_PARAMETERS=0",
                ),
            ),
        ],
        ["参数 ID 的宏被删掉", true, run(edit(base, ROOT_CMAKE, "  JUCE_FORCE_USE_LEGACY_PARAM_IDS=0\n", ""))],
        [
            "别处用另一个值再定义一次同名宏",
            true,
            run({ ...base, "cmake/Extra.cmake": "add_compile_definitions(JUCE_FORCE_USE_LEGACY_PARAM_IDS=1)\n" }),
        ],
        [
            "前缀相同的别的宏名不算同一个宏",
            false,
            run({ ...base, "cmake/Extra.cmake": "add_compile_definitions(JUCE_FORCE_USE_LEGACY_PARAM_IDS_EXTRA=1)\n" }),
        ],
        [
            "宏没有挂到插件目标上",
            true,
            run(edit(base, OUT, "target_compile_definitions(RinneOutput PUBLIC ${RINNE_PLUGIN_DEFINITIONS})", "")),
        ],
        ["宏改挂成 PRIVATE", true, run(edit(base, IN, "RinneInput PUBLIC", "RinneInput PRIVATE"))],
        [
            "基础格式多了 AAX",
            true,
            run(edit(base, ROOT_CMAKE, "set(RINNE_PLUGIN_FORMATS VST3)", "set(RINNE_PLUGIN_FORMATS VST3 AAX)")),
        ],
        ["Apple 上不再追加 AU", true, run(edit(base, ROOT_CMAKE, "list(APPEND RINNE_PLUGIN_FORMATS AU)", ""))],
        ["插件的 CMake 文件不存在", true, run({ [ROOT_CMAKE]: base[ROOT_CMAKE], [IN]: base[IN] })],
        [
            "golden 被改（正式版插件码）",
            true,
            run(
                base,
                goldenWith((g) => (g.plugins.input.identity.formal.PLUGIN_CODE = ["Rnnx"])),
            ),
        ],
        [
            "golden 被改（宏的值）",
            true,
            run(
                base,
                goldenWith((g) => (g.definitions.JUCE_VST3_CAN_REPLACE_VST2 = "1")),
            ),
        ],
        ["AU_SANDBOX_SAFE 改成 TRUE", true, run(edit(base, IN, "AU_SANDBOX_SAFE FALSE", "AU_SANDBOX_SAFE TRUE"))],
        [
            "加 USE_LEGACY_COMPATIBILITY_PLUGIN_CODE",
            true,
            run(edit(base, IN, "IS_SYNTH FALSE", "IS_SYNTH FALSE USE_LEGACY_COMPATIBILITY_PLUGIN_CODE TRUE")),
        ],
        ["加 PLUGIN_NAME", true, run(edit(base, OUT, "IS_SYNTH FALSE", 'IS_SYNTH FALSE PLUGIN_NAME "Rinne Send"'))],
        ["加 AU_EXPORT_PREFIX", true, run(edit(base, IN, "IS_SYNTH FALSE", "IS_SYNTH FALSE AU_EXPORT_PREFIX X"))],
        [
            "sources-*.cmake 里 set_target_properties 改 JUCE 身份属性",
            true,
            run({
                ...base,
                "src/input/sources-x.cmake": "set_target_properties(RinneInput PROPERTIES JUCE_BUNDLE_ID com.x)\n",
            }),
        ],
        ["别的 cmake 文件里写身份关键字", true, run({ ...base, "cmake/X.cmake": "set(PLUGIN_CODE Abcd)\n" })],
        [
            "endif 之后再 set 一次身份变量",
            true,
            run(
                edit(
                    base,
                    IN,
                    "target_compile_definitions(RinneInput",
                    "set(_rinne_RinneInput_identity PRODUCT_NAME x PLUGIN_CODE Rnnx BUNDLE_ID y)\ntarget_compile_definitions(RinneInput",
                ),
            ),
        ],
        [
            "list(TRANSFORM) 改身份变量",
            true,
            run(
                edit(
                    base,
                    IN,
                    "target_compile_definitions(RinneInput",
                    "list(TRANSFORM _rinne_RinneInput_identity REPLACE Rnni Rnnx)\ntarget_compile_definitions(RinneInput",
                ),
            ),
        ],
        [
            "add_compile_options 用 -D 改参数 ID 宏",
            true,
            run({ ...base, "cmake/X.cmake": "add_compile_options(-DJUCE_FORCE_USE_LEGACY_PARAM_IDS=1)\n" }),
        ],
        [
            "add_compile_options 用 /D 改参数 ID 宏",
            true,
            run({ ...base, "cmake/X.cmake": "add_compile_options(/DJUCE_FORCE_USE_LEGACY_PARAM_IDS=1)\n" }),
        ],
        [
            "target_compile_definitions 里 -D 写参数 ID 宏",
            true,
            run({
                ...base,
                "src/input/sources-x.cmake":
                    "target_compile_definitions(RinneInput PUBLIC -DJUCE_VST3_CAN_REPLACE_VST2=1)\n",
            }),
        ],
        [
            "根 CMakeLists 的 if(WIN32) 里追加格式",
            true,
            run(
                edit(
                    base,
                    ROOT_CMAKE,
                    "set(RINNE_PLUGIN_DEFINITIONS",
                    "if(WIN32)\n  list(APPEND RINNE_PLUGIN_FORMATS LV2)\nendif()\nset(RINNE_PLUGIN_DEFINITIONS",
                ),
            ),
        ],
        [
            "根 CMakeLists 的 if 之外追加格式",
            true,
            run(
                edit(
                    base,
                    ROOT_CMAKE,
                    "set(RINNE_PLUGIN_DEFINITIONS",
                    "list(APPEND RINNE_PLUGIN_FORMATS LV2)\nset(RINNE_PLUGIN_DEFINITIONS",
                ),
            ),
        ],
        [
            "RINNE_PLUGIN_FORMATS 在别处被改写",
            true,
            run({ ...base, "cmake/X.cmake": "list(APPEND RINNE_PLUGIN_FORMATS AAX)\n" }),
        ],
    ].map(([label, bad, errors]) => ({ label, bad, passed: bad ? errors.length > 0 : errors.length === 0 }));
    // moduleinfo 对拍：用 golden 夹具拼出一份正确的 moduleinfo（带尾逗号，与 JUCE 生成的写法一致），再逐项改坏
    const miGood = (plugin = "input", flavor = "formal") => {
        const spec = FIXTURE_GOLDEN.plugins[plugin];
        const id = spec.identity[flavor];
        const mi = FIXTURE_GOLDEN.moduleinfo;
        const tail = hex(spec.shared.PLUGIN_MANUFACTURER_CODE[0]) + hex(id.PLUGIN_CODE[0]);
        const cls = (category, middle, subs) => ({
            CID: mi.cidPrefix + middle + tail,
            Category: category,
            Name: id.PRODUCT_NAME[0],
            Vendor: mi.vendor,
            Version: "0.9.0",
            SDKVersion: "VST 3.7.12",
            ...(subs ? { "Sub Categories": subs } : {}),
        });
        return {
            Name: id.PRODUCT_NAME[0],
            Version: "0.9.0",
            "Factory Info": { Vendor: mi.vendor },
            Classes: [
                cls("Audio Module Class", mi.classes["Audio Module Class"], ["Fx", "Delay"]),
                cls("Component Controller Class", mi.classes["Component Controller Class"], ["Fx", "Delay"]),
                cls("Plugin Compatibility Class", mi.classes["Plugin Compatibility Class"]),
            ],
        };
    };
    const miRun = (mutate, plugin = "input", flavor = "formal") => {
        const info = miGood(plugin, flavor);
        mutate?.(info);
        // 经文本往返一次，顺带验证尾逗号的宽松解析
        const text = JSON.stringify(info, null, 2).replace(/([}\]"\d])(\s*\n\s*[}\]])/g, "$1,$2");
        return checkModuleInfo({
            golden: FIXTURE_GOLDEN,
            plugin,
            flavor,
            info: parseModuleInfo(text),
            version: "0.9.0",
        });
    };
    for (const [label, bad, errors] of [
        ["moduleinfo：正式版 Input 一致", false, miRun()],
        ["moduleinfo：探针版 Output 一致", false, miRun(undefined, "output", "probe")],
        ["moduleinfo：CID 改一位", true, miRun((i) => (i.Classes[0].CID = i.Classes[0].CID.replace(/.$/, "0")))],
        [
            "moduleinfo：插件码对应的 CID 尾段换成别的",
            true,
            miRun((i) => i.Classes.forEach((c) => (c.CID = c.CID.slice(0, -8) + "526E6E6A"))),
        ],
        [
            "moduleinfo：厂商码对应的 CID 段换了",
            true,
            miRun((i) => (i.Classes[1].CID = "ABCDEF011234ABCD536E6369526E6E69")),
        ],
        ["moduleinfo：名称改了", true, miRun((i) => (i.Name = "Rinne Send"))],
        ["moduleinfo：类里的名称改了", true, miRun((i) => (i.Classes[2].Name = "x"))],
        ["moduleinfo：厂商改了", true, miRun((i) => (i["Factory Info"].Vendor = "Other"))],
        ["moduleinfo：版本与工程版本不一致", true, miRun((i) => (i.Version = "0.9.1"))],
        ["moduleinfo：子类改了", true, miRun((i) => (i.Classes[0]["Sub Categories"] = ["Fx"]))],
        ["moduleinfo：缺一个类", true, miRun((i) => i.Classes.pop())],
        ["moduleinfo：多一个类", true, miRun((i) => i.Classes.push({ ...i.Classes[2] }))],
    ])
        cases.push({ label, bad, passed: bad ? errors.length > 0 : errors.length === 0 });
    return caseResult(cases);
}

// ---- 构建产物：VST3 的 moduleinfo.json ----

/** moduleinfo.json 允许尾逗号，标准 JSON 解析器不认：先去掉再解析。 */
export function parseModuleInfo(text) {
    return JSON.parse(text.replace(/,(\s*[}\]])/g, "$1"));
}

const hex = (s) => Buffer.from(s, "latin1").toString("hex").toUpperCase();

/**
 * 与 golden 对拍一份 moduleinfo：名称、厂商、版本、每个类的 CID 与子类（SDK 版本随 JUCE 自带的 VST3 SDK 走，不属于插件身份，不对拍）。
 * CID = 前缀 + 类别固定段 + 厂商码 + 插件码（十六进制），由 golden 里的身份字段算出来，所以插件码改了这里一定对不上。
 * flavor 是 "formal" 或 "probe"；version 是根 CMakeLists 的 project 版本。
 */
export function checkModuleInfo({ golden, plugin, flavor, info, version }) {
    const spec = golden.plugins[plugin];
    const id = spec.identity[flavor];
    const mi = golden.moduleinfo;
    const label = `${spec.target}（${flavor}）moduleinfo`;
    const errors = [];
    const want = (what, got, expected) => {
        if (JSON.stringify(got) !== JSON.stringify(expected))
            errors.push(`${label}：${what} 是 ${JSON.stringify(got)}，应为 ${JSON.stringify(expected)}`);
    };
    want("Name", info.Name, id.PRODUCT_NAME[0]);
    want("Version", info.Version, version);
    want("厂商", info["Factory Info"]?.Vendor, mi.vendor);
    const classes = Array.isArray(info.Classes) ? info.Classes : [];
    want("类数量", classes.length, Object.keys(mi.classes).length);
    const tail = hex(spec.shared.PLUGIN_MANUFACTURER_CODE[0]) + hex(id.PLUGIN_CODE[0]);
    for (const [category, middle] of Object.entries(mi.classes)) {
        const c = classes.find((x) => x.Category === category);
        if (!c) {
            errors.push(`${label}：缺类 ${category}`);
            continue;
        }
        want(`${category} 的 CID`, c.CID, mi.cidPrefix + middle + tail);
        want(`${category} 的名称`, c.Name, id.PRODUCT_NAME[0]);
        want(`${category} 的厂商`, c.Vendor, mi.vendor);
        want(`${category} 的版本`, c.Version, version);
        // 插件兼容类不带子类
        if (category !== "Plugin Compatibility Class")
            want(`${category} 的子类`, c["Sub Categories"], spec.shared.VST3_CATEGORIES);
    }
    return errors;
}

/** 在构建目录里找两个插件的 moduleinfo.json 并对拍；返回错误列表。 */
export function checkBuiltModuleInfo(root, buildDir, flavor) {
    const golden = JSON.parse(fs.readFileSync(path.join(root, GOLDEN), "utf8"));
    const rootText = fs.readFileSync(path.join(root, ROOT_CMAKE), "utf8");
    const version = /project\s*\(\s*Rinne\s+VERSION\s+([\d.]+)/i.exec(stripComments(rootText))?.[1];
    if (!version) return [`${ROOT_CMAKE}：读不出 project(Rinne VERSION …)`];
    const errors = [];
    for (const [plugin, spec] of Object.entries(golden.plugins)) {
        const product = spec.identity[flavor].PRODUCT_NAME[0];
        const dir = path.join(buildDir, path.dirname(spec.file), `${spec.target}_artefacts`);
        const found = fs.existsSync(dir)
            ? fs
                  .readdirSync(dir, { withFileTypes: true })
                  .filter((e) => e.isDirectory())
                  .map((e) =>
                      path.join(dir, e.name, "VST3", `${product}.vst3`, "Contents", "Resources", "moduleinfo.json"),
                  )
                  .filter((f) => fs.existsSync(f))
            : [];
        if (!found.length) {
            errors.push(`${spec.target}（${flavor}）：在 ${dir} 下找不到「${product}.vst3」的 moduleinfo.json`);
            continue;
        }
        for (const f of found) {
            let info;
            try {
                info = parseModuleInfo(fs.readFileSync(f, "utf8"));
            } catch (e) {
                errors.push(`${f}：解析失败：${e.message}`);
                continue;
            }
            errors.push(...checkModuleInfo({ golden, plugin, flavor, info, version }));
        }
    }
    return errors;
}

// ---- 注册 ----

const cmakeFilesOf = (root) => {
    const out = [ROOT_CMAKE];
    const walk = (dir) => {
        for (const e of fs.readdirSync(path.join(root, dir), { withFileTypes: true })) {
            const rel = `${dir}/${e.name}`;
            if (e.isDirectory()) walk(rel);
            else if (e.name === "CMakeLists.txt" || e.name.endsWith(".cmake")) out.push(rel);
        }
    };
    for (const d of ["cmake", "src", "tests", "tools"]) if (fs.existsSync(path.join(root, d))) walk(d);
    return out;
};

const readIn = (root) => (rel) =>
    fs.existsSync(path.join(root, rel)) ? fs.readFileSync(path.join(root, rel), "utf8") : null;

export default {
    name: "contract-static-identity",
    group: "docs",
    inputs: [],
    run(ctx) {
        const read = readIn(ctx.root);
        const golden = read(GOLDEN);
        if (golden === null) return { ok: false, lines: [`缺 ${GOLDEN}`] };
        const errors = checkIdentity({ golden: JSON.parse(golden), read, cmakeFiles: cmakeFilesOf(ctx.root) });
        return { ok: errors.length === 0, lines: errors };
    },
    selfTest: () => selfTest(),
    /** 在真实文件上把 PLUGIN_CODE 改一个字母，门禁必须变红。 */
    mutationCheck(ctx) {
        const read = readIn(ctx.root);
        const golden = JSON.parse(read(GOLDEN));
        const lines = [];
        for (const [name, spec] of Object.entries(golden.plugins)) {
            const code = spec.identity.formal.PLUGIN_CODE[0];
            const text = read(spec.file);
            const mutated = text.replace(
                `PLUGIN_CODE ${code}`,
                `PLUGIN_CODE ${code.slice(0, -1)}${code.endsWith("z") ? "y" : "z"}`,
            );
            if (mutated === text) {
                lines.push(`${name}：真实文件里找不到 PLUGIN_CODE ${code}，无法做变异`);
                continue;
            }
            const errors = checkIdentity({
                golden,
                read: (rel) => (rel === spec.file ? mutated : read(rel)),
                cmakeFiles: cmakeFilesOf(ctx.root),
            });
            if (!errors.length) lines.push(`${name}：PLUGIN_CODE 改了一个字母，门禁没有变红`);
        }
        return { ok: lines.length === 0, lines };
    },
};

if (isMain(import.meta.url)) {
    const argv = process.argv.slice(2);
    const i = argv.indexOf("--moduleinfo");
    if (i < 0) {
        console.error("用法：node scripts/checks.d/contract-static-identity.mjs --moduleinfo <构建目录> [--probe]");
        process.exitCode = 2;
    } else {
        const root = path.resolve(
            path.dirname(new URL(import.meta.url).pathname.replace(/^\/(?=[A-Za-z]:)/, "")),
            "..",
            "..",
        );
        const flavor = argv.includes("--probe") ? "probe" : "formal";
        const errors = checkBuiltModuleInfo(root, path.resolve(argv[i + 1] ?? ""), flavor);
        for (const e of errors) console.error(e);
        console.log(
            errors.length
                ? `moduleinfo 对拍失败（${flavor}）：${errors.length} 处`
                : `moduleinfo 与 golden 一致（${flavor}）`,
        );
        process.exitCode = errors.length ? 1 : 0;
    }
}
