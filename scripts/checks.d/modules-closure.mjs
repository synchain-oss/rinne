// SPDX-License-Identifier: GPL-3.0-or-later
//
// 模块闭包：CMake 引入的每个第三方来源都登记在 lib/rules.mjs 的 THIRD_PARTY_BY_MODULE，登记表点名的组件
// 都在 THIRD-PARTY-NOTICES.md 对应的表里有一行。CMake 里认的写法：
//   juce::juce_<模块>（target_link_libraries 等处）；juce_add_plugin(…)，隐含 juce::juce_audio_plugin_client；
//   NEEDS_WEBVIEW2 TRUE，隐含 nuget:Microsoft.Web.WebView2；提到 nuget 的文件里的 install <包名>；
//   FetchContent_Declare(<名字> …)。
// 注释（# 行注释与 #[[ ]] 块注释）不算。还没有 CMake 文件时只核对登记表与 NOTICES。

import fs from "node:fs";
import path from "node:path";
import { caseResult, trackedFiles } from "../lib/check-kit.mjs";
import { THIRD_PARTY_BY_MODULE } from "../lib/rules.mjs";
import { NOTICE_SECTIONS, NOTICES, parseNotices } from "./licenses-closure.mjs";

const CMAKE_FILE = /(?:^|\/)CMakeLists\.txt$|\.cmake$/;
const KEY = /^(?:juce::juce_\w+|nuget:[A-Za-z][\w.-]*|fetch:[\w.+-]+)$/;

/** 一个 CMake 文件引入的第三方来源（THIRD_PARTY_BY_MODULE 的键）。 */
export function cmakeRefs(text) {
    const code = text.replace(/#\[(=*)\[[\s\S]*?\]\1\]/g, "").replace(/#.*$/gm, "");
    const refs = new Set(code.match(/\bjuce::juce_\w+/g) || []);
    if (/\bjuce_add_plugin\s*\(/i.test(code)) refs.add("juce::juce_audio_plugin_client");
    if (/\bNEEDS_WEBVIEW2\s+(?:TRUE|ON|YES|1)\b/i.test(code)) refs.add("nuget:Microsoft.Web.WebView2");
    if (/\bnuget\b/i.test(code)) {
        for (const m of code.matchAll(/\binstall\s+([A-Za-z][\w-]*(?:\.[\w-]+)+)/g)) refs.add(`nuget:${m[1]}`);
    }
    for (const m of code.matchAll(/\bFetchContent_Declare\s*\(\s*([\w.+-]+)/gi)) refs.add(`fetch:${m[1]}`);
    return refs;
}

/** files: [{ path, text }]；registry 同 THIRD_PARTY_BY_MODULE；notices 是 NOTICES 全文。返回错误列表。 */
export function checkModules({ files, registry, notices }) {
    const { errors, shipped, build } = parseNotices(notices);
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
    }
    for (const f of files) {
        for (const ref of cmakeRefs(f.text)) {
            if (!Object.hasOwn(registry, ref)) errors.push(`${f.path} 引入了 ${ref}，THIRD_PARTY_BY_MODULE 没有登记`);
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
const REGISTRY = {
    "juce::juce_core": { table: "shipped", components: ["JUCE", "zlib"] },
    "juce::juce_gui_extra": { table: "shipped", components: ["JUCE"] },
    "juce::juce_audio_plugin_client": { table: "shipped", components: ["JUCE", "VST3 SDK"] },
    "juce::juce_recommended_warning_flags": { table: "shipped", components: [] },
    "nuget:Microsoft.Web.WebView2": { table: "shipped", components: ["WebView2 loader"] },
    "fetch:Catch2": { table: "build", components: ["Catch2"] },
};
const CMAKE = [
    "juce_add_plugin(RinneInput FORMATS VST3 NEEDS_WEBVIEW2 TRUE)",
    "target_link_libraries(rinne_plugin_common INTERFACE juce::juce_core juce::juce_gui_extra",
    "    juce::juce_recommended_warning_flags) # juce::juce_opengl 只是注释",
    "#[[ 块注释里的 juce::juce_dsp 不算 ]]",
    "FetchContent_Declare(Catch2 GIT_REPOSITORY https://github.com/catchorg/Catch2.git GIT_TAG v3.5.4)",
    'execute_process(COMMAND "${NUGET_EXE}" install Microsoft.Web.WebView2 -Version 1.0.2957.106)',
].join("\n");

export function selfTest() {
    const without = (key) => Object.fromEntries(Object.entries(REGISTRY).filter(([k]) => k !== key));
    const cases = [
        ["登记齐全", false, {}],
        ["还没有 CMake 文件", false, { files: [] }],
        [
            "植入未登记的 juce::juce_audio_formats",
            true,
            { cmake: `${CMAKE}\ntarget_link_libraries(x juce::juce_audio_formats)` },
        ],
        ["未登记的 FetchContent 项", true, { cmake: `${CMAKE}\nFetchContent_Declare(libebur128 GIT_TAG v1.2.6)` }],
        ["未登记的 NuGet 包", true, { cmake: `${CMAKE}\nexecute_process(COMMAND nuget install Some.Other.Package)` }],
        ["juce_add_plugin 隐含的插件封装没登记", true, { registry: without("juce::juce_audio_plugin_client") }],
        ["NEEDS_WEBVIEW2 隐含的 NuGet 包没登记", true, { registry: without("nuget:Microsoft.Web.WebView2") }],
        [
            "登记的组件不在 NOTICES 里",
            true,
            { registry: { ...REGISTRY, "juce::juce_graphics": { table: "shipped", components: ["libpng"] } } },
        ],
        [
            "组件登记在错的表",
            true,
            { registry: { ...REGISTRY, "fetch:Catch2": { table: "shipped", components: ["Catch2"] } } },
        ],
        ["table 取值不对", true, { registry: { ...REGISTRY, "fetch:Catch2": { table: "test", components: [] } } }],
        ["键的写法不对", true, { registry: { ...REGISTRY, juce_core: { table: "shipped", components: [] } } }],
    ].map(([label, bad, o]) => {
        const files = o.files ?? [{ path: "src/input/CMakeLists.txt", text: o.cmake ?? CMAKE }];
        const errors = checkModules({ files, registry: o.registry ?? REGISTRY, notices: NOTICES_FIXTURE });
        return { label, bad, passed: bad ? errors.length > 0 : errors.length === 0 };
    });
    return caseResult(cases);
}

export default {
    name: "modules-closure",
    group: "compliance",
    inputs: [],
    run(ctx) {
        const abs = path.join(ctx.root, NOTICES);
        if (!fs.existsSync(abs)) return { ok: false, lines: [`缺 ${NOTICES}`] };
        const files = trackedFiles(ctx.root)
            .filter((p) => CMAKE_FILE.test(p) && !p.startsWith("third_party/") && fs.existsSync(path.join(ctx.root, p)))
            .map((p) => ({ path: p, text: fs.readFileSync(path.join(ctx.root, p), "utf8") }));
        const errors = checkModules({ files, registry: THIRD_PARTY_BY_MODULE, notices: fs.readFileSync(abs, "utf8") });
        const note = files.length ? [] : ["尚无 CMake 文件：只核对了登记表与 NOTICES"];
        return { ok: errors.length === 0, lines: [...note, ...errors] };
    },
    selfTest,
};
