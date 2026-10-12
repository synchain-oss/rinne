// SPDX-License-Identifier: GPL-3.0-or-later
//
// 模块闭包：CMake 引入的每个第三方来源都登记在 lib/rules.mjs 的 THIRD_PARTY_BY_MODULE，登记表点名的组件
// 都在 THIRD-PARTY-NOTICES.md 对应的表里有一行。CMake 里认的写法：
//   JUCE 目标：juce::juce_<名字>，或不带命名空间的 juce_<名字>（JUCE 的 juce:: 目标只是同名目标的别名）；
//   后面紧跟括号的是 JUCE 的 CMake 命令（juce_add_binary_data 等），路径里的模块名也不算；
//   juce_add_plugin(…) 隐含 juce::juce_audio_plugin_client；NEEDS_WEBVIEW2 TRUE 隐含 nuget:Microsoft.Web.WebView2；
//   提到 nuget 的文件里的 install <包名>；FetchContent_Declare(<名字> …)。
// JUCE 模块按 JUCE_MODULE_REQUIRES 展开传递依赖，闭包里的每个模块都必须登记：只链接 juce_dsp 也会编进
// juce_audio_formats（以及其中的 FLAC 与 Ogg Vorbis）。登记表本身也必须对依赖闭合。
// 注释（# 行注释与 #[[ ]] 块注释）不算。还没有 CMake 文件时只核对登记表、依赖表与 NOTICES。

import fs from "node:fs";
import path from "node:path";
import { caseResult, trackedFiles } from "../lib/check-kit.mjs";
import { JUCE_MODULE_REQUIRES, THIRD_PARTY_BY_MODULE } from "../lib/rules.mjs";
import { NOTICE_SECTIONS, NOTICES, parseNotices } from "./licenses-closure.mjs";

const CMAKE_FILE = /(?:^|\/)CMakeLists\.txt$|\.cmake$/;
const KEY = /^(?:juce::juce_\w+|nuget:[A-Za-z][\w.-]*|fetch:[\w.+-]+)$/;
// juce::juce_x 或裸的 juce_x；前面是路径、变量或别的标识符的一部分时不算，后面跟括号的是命令
const JUCE_TARGET = /(?<![\w:$/{.-])(?:juce::)?(juce_[a-z0-9_]+)\b(?!\s*\()/g;

/** 一个 CMake 文件引入的第三方来源（THIRD_PARTY_BY_MODULE 的键）。 */
export function cmakeRefs(text) {
    const code = text.replace(/#\[(=*)\[[\s\S]*?\]\1\]/g, "").replace(/#.*$/gm, "");
    const refs = new Set([...code.matchAll(JUCE_TARGET)].map((m) => `juce::${m[1]}`));
    if (/\bjuce_add_plugin\s*\(/i.test(code)) refs.add("juce::juce_audio_plugin_client");
    if (/\bNEEDS_WEBVIEW2\s+(?:TRUE|ON|YES|1)\b/i.test(code)) refs.add("nuget:Microsoft.Web.WebView2");
    if (/\bnuget\b/i.test(code)) {
        for (const m of code.matchAll(/\binstall\s+([A-Za-z][\w-]*(?:\.[\w-]+)+)/g)) refs.add(`nuget:${m[1]}`);
    }
    for (const m of code.matchAll(/\bFetchContent_Declare\s*\(\s*([\w.+-]+)/gi)) refs.add(`fetch:${m[1]}`);
    return refs;
}

/** 键 key 要求登记的全部键：依赖表里有的 JUCE 模块展开成依赖闭包（含自身），其余就是它自己。 */
function required(key, table) {
    const name = key.startsWith("juce::") ? key.slice(6) : null;
    if (!name || !Object.hasOwn(table, name)) return [key];
    const seen = new Set();
    const walk = (m) => {
        if (seen.has(m)) return;
        seen.add(m);
        (table[m] || []).forEach(walk);
    };
    walk(name);
    return [...seen].map((m) => `juce::${m}`);
}

/**
 * files: [{ path, text }]；registry 同 THIRD_PARTY_BY_MODULE；requires 同 JUCE_MODULE_REQUIRES；
 * notices 是 NOTICES 全文；juceVersion 是 .juce-version 的内容（文件还没有时不给）。返回错误列表。
 */
export function checkModules({ files, registry, requires, notices, juceVersion }) {
    const { errors, shipped, build } = parseNotices(notices);
    const table = requires.modules;
    if (juceVersion && juceVersion !== requires.version) {
        errors.push(
            `JUCE_MODULE_REQUIRES 是 JUCE ${requires.version} 的依赖表，.juce-version 是 ${juceVersion}：按新版本重抄`,
        );
    }
    for (const [m, deps] of Object.entries(table)) {
        for (const d of deps)
            if (!Object.hasOwn(table, d)) errors.push(`JUCE_MODULE_REQUIRES：${m} 依赖的 ${d} 不在表里`);
    }
    const rows = { shipped: new Set(shipped.map((r) => r.component)), build: new Set(build.map((r) => r.component)) };
    for (const [key, entry] of Object.entries(registry)) {
        if (!KEY.test(key)) errors.push(`THIRD_PARTY_BY_MODULE 的键 ${key} 写法不对`);
        if (!(entry?.table in rows)) {
            errors.push(`THIRD_PARTY_BY_MODULE["${key}"].table 只能是 shipped 或 build`);
            continue;
        }
        for (const c of entry.components || []) {
            if (!rows[entry.table].has(c)) errors.push(`${key} 的组件「${c}」不在 NOTICES 的 ${entry.table} 表里`);
        }
        for (const dep of required(key, table)) {
            if (!Object.hasOwn(registry, dep))
                errors.push(`THIRD_PARTY_BY_MODULE 登记了 ${key}，却没登记它依赖的 ${dep}`);
        }
    }
    for (const f of files) {
        const reported = new Set();
        for (const ref of cmakeRefs(f.text)) {
            for (const dep of required(ref, table)) {
                if (Object.hasOwn(registry, dep) || reported.has(dep)) continue;
                reported.add(dep);
                const via = dep === ref ? "" : `（经 ${ref} 的依赖带进来）`;
                errors.push(`${f.path} 引入了 ${dep}${via}，THIRD_PARTY_BY_MODULE 没有登记`);
            }
        }
    }
    return errors;
}

// ---- 自测夹具 ----------------------------------------------------------------------------
const NOTICES_FIXTURE = [
    "<!-- section:shipped-table -->",
    "| Component | Version | SPDX | Ships in | Notice texts |",
    "| --- | --- | --- | --- | --- |",
    "| JUCE | 8 | `AGPL-3.0-only` | all | `third_party/notices/juce.LICENSE.md` |",
    "| zlib | 1 | `Zlib` | all | `third_party/notices/zlib.LICENSE` |",
    "| VST3 SDK | 3 | `GPL-3.0-only` | vst3 | `third_party/notices/vst3sdk.LICENSE.txt` |",
    "| WebView2 loader | 1 | `BSD-3-Clause` | win | `third_party/notices/webview2.LICENSE.txt` |",
    "<!-- /section:shipped-table -->",
    ...NOTICE_SECTIONS.slice(1).flatMap((s) => [`<!-- section:${s} -->`, `<!-- /section:${s} -->`]),
    "## 2. Build, test and CI only",
    "| Component | Version | SPDX | Used for | Notice texts |",
    "| --- | --- | --- | --- | --- |",
    "| Catch2 | 3 | `BSL-1.0` | tests | `third_party/notices/catch2.LICENSE.txt` |",
].join("\n");
// 依赖表夹具与真表同形，只取用到的几个模块
const REQUIRES = {
    version: "8.0.8",
    modules: {
        juce_core: [],
        juce_events: ["juce_core"],
        juce_data_structures: ["juce_events"],
        juce_graphics: ["juce_events"],
        juce_gui_basics: ["juce_graphics", "juce_data_structures"],
        juce_gui_extra: ["juce_gui_basics"],
        juce_audio_basics: ["juce_core"],
        juce_audio_formats: ["juce_audio_basics"],
        juce_audio_processors: ["juce_gui_extra", "juce_audio_basics"],
        juce_audio_plugin_client: ["juce_audio_processors"],
        juce_dsp: ["juce_audio_formats"],
    },
};
const juce = (components = ["JUCE"]) => ({ table: "shipped", components });
const REGISTRY = {
    "juce::juce_core": juce(["JUCE", "zlib"]),
    "juce::juce_events": juce(),
    "juce::juce_data_structures": juce(),
    "juce::juce_graphics": juce(),
    "juce::juce_gui_basics": juce(),
    "juce::juce_gui_extra": juce(),
    "juce::juce_audio_basics": juce(),
    "juce::juce_audio_processors": juce(["JUCE", "VST3 SDK"]),
    "juce::juce_audio_plugin_client": juce(["JUCE", "VST3 SDK"]),
    "juce::juce_recommended_warning_flags": juce([]),
    "nuget:Microsoft.Web.WebView2": { table: "shipped", components: ["WebView2 loader"] },
    "fetch:Catch2": { table: "build", components: ["Catch2"] },
};
const CMAKE = [
    "juce_add_plugin(RinneInput FORMATS VST3 NEEDS_WEBVIEW2 TRUE)",
    "juce_add_binary_data(RinneWebAssets SOURCES index.html)",
    "target_link_libraries(rinne_plugin_common INTERFACE juce::juce_core juce::juce_gui_extra",
    "    juce::juce_recommended_warning_flags) # juce::juce_opengl 只是注释",
    "target_link_libraries(rinne_host_tests PRIVATE juce_audio_processors)",
    "#[[ 块注释里的 juce::juce_dsp 不算 ]]",
    "include(${JUCE_PATH}/modules/juce_dsp/x.cmake OPTIONAL)",
    "FetchContent_Declare(Catch2 GIT_REPOSITORY https://github.com/catchorg/Catch2.git GIT_TAG v3.5.4)",
    'execute_process(COMMAND "${NUGET_EXE}" install Microsoft.Web.WebView2 -Version 1.0.2957.106)',
].join("\n");

export function selfTest() {
    const without = (key) => Object.fromEntries(Object.entries(REGISTRY).filter(([k]) => k !== key));
    const link = (target) => ({ cmake: `${CMAKE}\ntarget_link_libraries(x PRIVATE ${target})` });
    const cases = [
        ["登记齐全（含裸目标、JUCE 命令与路径里的模块名）", false, {}],
        ["还没有 CMake 文件", false, { files: [] }],
        ["JUCE 版本与依赖表一致", false, { juceVersion: "8.0.8" }],
        ["植入未登记的 juce::juce_audio_formats", true, link("juce::juce_audio_formats")],
        ["不带命名空间的 juce_audio_formats 目标", true, link("juce_audio_formats")],
        [
            "只登记 juce_dsp、没登记 juce_audio_formats",
            true,
            { registry: { ...REGISTRY, "juce::juce_dsp": juce() }, ...link("juce::juce_dsp") },
        ],
        ["依赖表里没有的 JUCE 模块", true, link("juce::juce_foo")],
        ["JUCE 版本与依赖表不一致", true, { juceVersion: "8.0.11" }],
        [
            "依赖表引用了表外的模块",
            true,
            { requires: { ...REQUIRES, modules: { ...REQUIRES.modules, juce_dsp: ["juce_nothing"] } } },
        ],
        ["未登记的 FetchContent 项", true, { cmake: `${CMAKE}\nFetchContent_Declare(libebur128 GIT_TAG v1.2.6)` }],
        ["未登记的 NuGet 包", true, { cmake: `${CMAKE}\nexecute_process(COMMAND nuget install Some.Other.Package)` }],
        ["juce_add_plugin 隐含的插件封装没登记", true, { registry: without("juce::juce_audio_plugin_client") }],
        ["NEEDS_WEBVIEW2 隐含的 NuGet 包没登记", true, { registry: without("nuget:Microsoft.Web.WebView2") }],
        ["登记表对依赖不闭合", true, { files: [], registry: without("juce::juce_events") }],
        ["登记的组件不在 NOTICES 里", true, { registry: { ...REGISTRY, "juce::juce_graphics": juce(["libpng"]) } }],
        [
            "组件登记在错的表",
            true,
            { registry: { ...REGISTRY, "fetch:Catch2": { table: "shipped", components: ["Catch2"] } } },
        ],
        ["table 取值不对", true, { registry: { ...REGISTRY, "fetch:Catch2": { table: "test", components: [] } } }],
        ["键的写法不对", true, { registry: { ...REGISTRY, juce_core: juce([]) } }],
    ].map(([label, bad, o]) => {
        const files = o.files ?? [{ path: "src/input/CMakeLists.txt", text: o.cmake ?? CMAKE }];
        const errors = checkModules({
            files,
            registry: o.registry ?? REGISTRY,
            requires: o.requires ?? REQUIRES,
            notices: NOTICES_FIXTURE,
            juceVersion: o.juceVersion,
        });
        return { label, bad, passed: bad ? errors.length > 0 : errors.length === 0 };
    });
    return caseResult(cases);
}

export default {
    name: "modules-closure",
    group: "compliance",
    inputs: [],
    run(ctx) {
        const read = (rel) =>
            fs.existsSync(path.join(ctx.root, rel)) ? fs.readFileSync(path.join(ctx.root, rel), "utf8") : null;
        const notices = read(NOTICES);
        if (notices === null) return { ok: false, lines: [`缺 ${NOTICES}`] };
        const files = trackedFiles(ctx.root)
            .filter((p) => CMAKE_FILE.test(p) && !p.startsWith("third_party/") && fs.existsSync(path.join(ctx.root, p)))
            .map((p) => ({ path: p, text: read(p) }));
        const errors = checkModules({
            files,
            registry: THIRD_PARTY_BY_MODULE,
            requires: JUCE_MODULE_REQUIRES,
            notices,
            juceVersion: read(".juce-version")?.trim() || undefined,
        });
        const note = files.length ? [] : ["尚无 CMake 文件：只核对了登记表、依赖表与 NOTICES"];
        return { ok: errors.length === 0, lines: [...note, ...errors] };
    },
    selfTest,
};
