// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/web-preview.json
//
// 嵌入资源可解析性回归（Node，无 DOM）。守的是「插件窗口空白，看门狗超时，落到兜底面板」这类故障的静态面。
//
// 背景：插件里的 web 资源不是按目录服务的，构建把 web/ 下的文件扁平编进二进制数据，资源提供者再按 basename 反查。
// 于是目录层级不进包，页面里的 ../shared/x.js 靠 URL 归一加 basename 命中；但只要有一个引用的 basename 不在打包集合里，
// 插件里就取不到那个文件，而浏览器预览（按真实目录服务）照样全绿。本套把这条差异搬到静态检查上。
//
// 断言：
//   ① 每个角色的打包集合内 basename 全局唯一（重名会让其中一个永远取不到）；
//   ② 从 index.html 出发，脚本、样式、字体、模块图的每个引用都能按 basename 命中；
//   ③ 同一个文件不会被两个不同的路径取到（ES module 按 URL 定身份，会被实例化两次）；
//   ④ index.html 里的 boot 守卫存在，是普通 script 且只用 ES5，排在所有资源之前；事件名与 C++ 侧一致；
//   ⑥ 根元素底色恰好一条、内联、排在外链样式之前，是 linear-gradient 字面量，且与 tokens.css 的 --page-gradient
//      逐项同色标，也与 C++ 侧的占位色标数组相同（开窗白闪的第三段）。这一族是占位：遮挡闸期间铺满窗口的那块底；
//   ⑥b tokens.css 的 --page-backdrop 是外圈色，与占位是两个角色：必须不等于占位的任何一个色标；
//   ⑥c 占位的 C++ 真源与 --page-gradient 整张色标表逐项相同（tokens 是真源，C++ 数组与内联字面量照抄它）；
//   ⑦ 「首帧已绘」上行信号在场：事件名与 C++ 一致，武装是嵌套两层 requestAnimationFrame 且由 paint 记录接线，
//      回落路与保险定时器都在，保险的回调直接发信号、不绕 rAF，撤网（sent 置位与清保险）排在 postMessage 之后。
//   ⑧ 打包与接线：cmake/RinneWebAssets.cmake 的 glob 与这里的 packagedFiles 同口径（逐条解析对拍）；每个角色的
//      src/<角色>/sources-webview.cmake 以对的角色目录、命名空间调用 rinne_add_web_assets，嵌入根目录的 LICENSE，
//      并把生成的目标链进该插件；插件入口文件包含同名头、把四个成员原样交给编辑器工厂；工厂把资源、角色、钩子原样转给网页编辑器。
//      这几处都在插件目标里编，宿主测试编不到，是「资源没进包」一类空白窗口故障的静态面，只能靠这里的源码判据守。
// C++ 侧文件（WebViewHost.h、PlatformWebView.h）必须在库里：缺文件、或事件名、字段名、角度与色标表抽不出来，一律判红。
// 本套没有跳过项：⑥ ⑥c ⑦ 的对拍全是真判据。
//
// 用法：node web-preview/tests/smoke-embedded-resources.mjs [仓库根]
// 退出码：0 全绿；1 有失败项。

import fs from "node:fs";
import path from "node:path";
import { createRun, repoRoot } from "./lib/harness.mjs";

const ROOT = repoRoot();
const run = createRun("嵌入资源");
const rel = (p) => p.split(path.sep).join("/");
const read = (p) => fs.readFileSync(path.join(ROOT, p), "utf8");
const exists = (p) => fs.existsSync(path.join(ROOT, p));

/** 列目录下匹配后缀的文件（相对仓库根的 POSIX 路径）。 */
function listFiles(dir, exts, recursive = false) {
    const out = [];
    const walk = (d) => {
        for (const name of fs.readdirSync(d)) {
            const p = path.join(d, name);
            if (fs.statSync(p).isDirectory()) {
                if (recursive) walk(p);
            } else if (exts.some((e) => name.endsWith(e))) out.push(rel(path.relative(ROOT, p)));
        }
    };
    if (fs.existsSync(path.join(ROOT, dir))) walk(path.join(ROOT, dir));
    return out;
}

/**
 * 打包集合的口径表：cmake/RinneWebAssets.cmake 的 glob 与这里必须逐条相同（见 ⑧）。
 * dir 里的 "role" 代表本角色目录；recursive 对应 GLOB_RECURSE；exts 不含点。
 */
const PACKAGING = [
    { dir: "role", recursive: true, exts: ["html", "js", "css"] },
    { dir: "shared", recursive: false, exts: ["js", "css", "png", "json"] },
    { dir: "i18n", recursive: false, exts: ["js"] },
    { dir: "js/juce", recursive: false, exts: ["js"] },
    { dir: "fonts", recursive: false, exts: ["woff2"] },
];
/** 随包嵌入的许可证文本（仓库根的文件）。它的文件名同样参与唯一性断言。 */
const LICENSE_FILE = "LICENSE";

/** 某个角色的打包集合。构建脚本的 glob 要与这里同口径：角色目录递归，其余目录不递归。 */
function packagedFiles(role) {
    return [
        ...PACKAGING.flatMap((r) =>
            listFiles(
                `web/${r.dir === "role" ? role : r.dir}`,
                r.exts.map((e) => `.${e}`),
                r.recursive,
            ),
        ),
        ...(exists(LICENSE_FILE) ? [LICENSE_FILE] : []),
    ];
}

/** 抽出一个文件里的全部资源引用（纯文本扫描）。 */
function referencesIn(file) {
    const text = read(file);
    const refs = [];
    const push = (re, g = 1) => {
        for (const m of text.matchAll(re)) refs.push(m[g]);
    };
    if (file.endsWith(".js")) {
        push(/\bfrom\s+"([^"]+)"/g);
        push(/\bimport\s+"([^"]+)"/g);
        push(/\bimport\(\s*"([^"]+)"\s*\)/g);
    } else if (file.endsWith(".html")) {
        push(/<(?:script|img)\b[^>]*\bsrc="([^"]+)"/g);
        push(/<link\b[^>]*\bhref="([^"]+)"/g);
    } else if (file.endsWith(".css")) {
        push(/url\(\s*"?([^")]+)"?\s*\)/g);
    }
    return refs.filter((r) => !r.startsWith("data:") && !r.startsWith("#"));
}

const roles = ["input", "output"];
const hexOf = (s) => s.toLowerCase();

/** 解析 linear-gradient(<角度>deg, <色> <n>%, …)。 */
function parseGradient(text) {
    const m = /linear-gradient\(\s*(\d+)deg\s*,([^)]*)\)/.exec(text);
    if (!m) return null;
    const stops = [...m[2].matchAll(/(#[0-9a-fA-F]{6})\s+(\d+)%/g)].map((x) => `${hexOf(x[1])} ${x[2]}%`);
    return { angle: Number(m[1]), stops };
}

// ---------------------------------------------------------------- ① ② ③
for (const role of roles) {
    const pack = packagedFiles(role);
    const byBase = new Map();
    for (const f of pack) {
        const b = path.posix.basename(f);
        byBase.set(b, [...(byBase.get(b) || []), f]);
    }
    for (const [b, files] of byBase) {
        run.check(files.length === 1, `${role}：打包集合里 basename ${b} 重名：${files.join("、")}`);
    }

    const seen = new Set();
    const identity = new Map(); // basename -> 到达它的不同路径
    const queue = [`web/${role}/index.html`];
    while (queue.length) {
        const file = queue.shift();
        if (seen.has(file)) continue;
        seen.add(file);
        if (!exists(file)) {
            run.check(false, `${role}：引用到的文件不存在：${file}`);
            continue;
        }
        for (const ref of referencesIn(file)) {
            if (/^[a-z]+:\/\//i.test(ref) || ref.startsWith("//")) {
                run.check(false, `${role}：${file} 引用了外链 ${ref}`);
                continue;
            }
            const clean = ref.split(/[?#]/)[0];
            const target = path.posix.normalize(path.posix.join(path.posix.dirname(file), clean));
            const base = path.posix.basename(target);
            run.check(pack.includes(target), `${role}：${file} 引用的 ${ref} 不在打包集合里（插件里取不到）`);
            const hit = byBase.get(base);
            run.check(
                Boolean(hit) && hit[0] === target,
                `${role}：${file} 引用的 ${ref} 按 basename 反查命不中同一个文件`,
            );
            identity.set(base, new Set([...(identity.get(base) || []), target]));
            if (/\.(js|css)$/.test(target)) queue.push(target);
        }
    }
    for (const [base, paths] of identity) {
        run.check(paths.size === 1, `${role}：同一个文件 ${base} 被不同路径取到：${[...paths].join("、")}`);
    }
    run.log(`${role}：可达文件 ${seen.size} 个，打包集合 ${pack.length} 个`);
}

// ---------------------------------------------------------------- C++ 真源
// 两个 C++ 文件都必须在库里，五个名字都必须抽得出来：缺文件或抽不出来就判红，没有跳过的路。
// 抽取前先去掉注释（含被注释掉的旧声明），声明按「名字 = 值」「名字[] = {…};」的形态认，
// 注释或函数体里提到同名标识符不会被当成声明。
const CPP_FILES = ["src/plugin-common/WebViewHost.h", "src/plugin-common/PlatformWebView.h"];

/** 去掉 C++ 的行注释与块注释，字符串字面量里的 // 与 /* 原样保留。 */
function stripCppComments(text) {
    let out = "";
    let i = 0;
    while (i < text.length) {
        const c = text[i];
        const n = text[i + 1];
        if (c === "/" && n === "/") {
            while (i < text.length && text[i] !== "\n") i++;
        } else if (c === "/" && n === "*") {
            const end = text.indexOf("*/", i + 2);
            i = end < 0 ? text.length : end + 2;
            out += " ";
        } else if (c === '"' || c === "'") {
            out += c;
            i++;
            while (i < text.length && text[i] !== c) {
                if (text[i] === "\\") {
                    out += text[i++];
                    if (i >= text.length) break;
                }
                out += text[i++];
            }
            if (i < text.length) out += text[i++];
        } else {
            out += c;
            i++;
        }
    }
    return out;
}

/** 色标表的每一项必须是 {0xRRGGBB, 位置}：rgb 在前，位置在后。返回「#rrggbb n%」列表与读不懂的项数。 */
function parseCppStopEntries(body) {
    const stops = [];
    let malformed = 0;
    for (const m of body.matchAll(/\{([^{}]*)\}/g)) {
        const e = /^\s*(0x[0-9a-fA-F]{6})\s*,\s*(\d+(?:\.\d+)?)f?\s*$/.exec(m[1]);
        if (!e) {
            malformed++;
            continue;
        }
        const pos = Number(e[2]);
        stops.push(`#${e[1].slice(2).toLowerCase()} ${Math.round(pos <= 1 ? pos * 100 : pos)}%`);
    }
    return { stops, malformed };
}

/** 从 C++ 文本抽出对拍用的五个事实；抽不到的是 null。 */
function extractCppFacts(rawText) {
    const text = stripCppComments(rawText);
    const constant = (name) => new RegExp(`\\b${name}\\s*=\\s*"([^"]+)"`).exec(text)?.[1] ?? null;
    const table = /\bkShellBackdropStops\s*\[\s*\]\s*=\s*\{([\s\S]*?)\}\s*;/.exec(text)?.[1] ?? null;
    return {
        boot: constant("kBootErrorEventId"),
        first: constant("kFirstFrameEventId"),
        delta: constant("kPaintDeltaField"),
        angle: /\bkShellBackdropAngleDeg\s*=\s*(\d+)/.exec(text)?.[1] ?? null,
        stops: table === null ? null : parseCppStopEntries(table),
    };
}

const cppExpected = {
    boot: "kBootErrorEventId",
    first: "kFirstFrameEventId",
    delta: "kPaintDeltaField",
    angle: "kShellBackdropAngleDeg",
    stops: "kShellBackdropStops",
};

/** 判红的对拍项：缺任何一个 C++ 文件时五项全红（没有可抽的东西），否则只有抽不到的那几项。 */
function failedCppFacts(missingFileCount, facts) {
    if (missingFileCount > 0) return Object.values(cppExpected);
    return Object.keys(cppExpected)
        .filter((k) => facts[k] === null)
        .map((k) => cppExpected[k]);
}

// 抽取器自测：好例抽得出，坏例抽不出或读不懂，注释和同名引用不会冒充声明。
{
    const good = extractCppFacts(`
        // constexpr const char* kBootErrorEventId = "stale";
        inline constexpr const char* kBootErrorEventId = "evt.boot";
        /* kFirstFrameEventId = "stale" */
        inline constexpr const char* kFirstFrameEventId = "evt.first";
        inline constexpr const char* kPaintDeltaField = "delta";
        inline constexpr float kShellBackdropAngleDeg = 157.0f;
        // 色标表 kShellBackdropStops 在下面，注释里 {0xaaaaaa, 0.5f} 不算
        inline constexpr Stop kShellBackdropStops[] = {
            {0xb5acc9, 0.00f},   // 起点色，0.5 不是色标
            {0xfde8ed, 1.00f},
        };
        auto f() { return sizeof(kShellBackdropStops); }
    `);
    run.eq(good.boot, "evt.boot", "抽取器自测：被注释掉的旧声明不能盖过真声明");
    run.eq(good.first, "evt.first", "抽取器自测：块注释里的声明不算");
    run.eq(good.delta, "delta", "抽取器自测：字段名");
    run.eq(good.angle, "157", "抽取器自测：角度");
    run.eq(
        good.stops,
        { stops: ["#b5acc9 0%", "#fde8ed 100%"], malformed: 0 },
        "抽取器自测：色标表按声明抽，行尾注释里的数字不进表",
    );

    const scvbStyle = extractCppFacts(
        "constexpr Stop kShellBackdropStops[] = { {0.00f, 0xffb5acc9}, {1.00f, 0xfffde8ed} };",
    );
    run.eq(scvbStyle.stops, { stops: [], malformed: 2 }, "抽取器自测：位置在前、ARGB 的写法必须读不懂（判红）");

    const refOnly = extractCppFacts("auto n = sizeof(kShellBackdropStops); const char* kBootErrorEventId;");
    run.eq(refOnly.stops, null, "抽取器自测：只有引用没有声明，色标表抽不到");
    run.eq(refOnly.boot, null, "抽取器自测：没有赋值的声明抽不到事件名");

    const urlInString = extractCppFacts(
        `const char* u = "https://x.y/z"; inline constexpr const char* kBootErrorEventId = "e";`,
    );
    run.eq(urlInString.boot, "e", "抽取器自测：字符串里的 // 不能当成注释吞掉后面的声明");

    // 声明之前已有函数体引用了同名标识符，其后还有花括号：只认声明形态，不能从第一次出现处起算
    const earlyRef = extractCppFacts(
        "inline int f() { auto n = sizeof(kShellBackdropStops); if (n) { g(); } else { h(); } return 0; }\n" +
            "inline constexpr Stop kShellBackdropStops[] = { {0xb5acc9, 0.00f}, {0xfde8ed, 1.00f} };",
    );
    run.eq(
        earlyRef.stops,
        { stops: ["#b5acc9 0%", "#fde8ed 100%"], malformed: 0 },
        "抽取器自测：声明之前的同名引用不能被当成表的起点",
    );

    // 每一项只许 {rgb, 位置} 两个字段，多出来的字段算读不懂
    const extraField = extractCppFacts("constexpr Stop kShellBackdropStops[] = { {0xb5acc9, 0.00f, 0x112233} };");
    run.eq(extraField.stops, { stops: [], malformed: 1 }, "抽取器自测：多出字段的项必须读不懂");

    // 对拍项判红：缺文件或抽不到都红，没有跳过的路
    const allNull = { boot: null, first: null, delta: null, angle: null, stops: null };
    const names = Object.values(cppExpected);
    run.eq(failedCppFacts(2, allNull), names, "判红自测：C++ 文件都不在时五项全红，不是跳过");
    run.eq(failedCppFacts(1, good), names, "判红自测：缺一个文件时，即使另一个抽得到也五项全红");
    run.eq(failedCppFacts(0, allNull), names, "判红自测：文件在但全抽不到时全部判红");
    run.eq(failedCppFacts(0, { ...good, delta: null }), ["kPaintDeltaField"], "判红自测：只有抽不到的那一项判红");
    run.eq(failedCppFacts(0, good), [], "判红自测：文件齐全且都抽得到时没有红项");
}

const cppFile = CPP_FILES.filter(exists);
const cpp = extractCppFacts(cppFile.map(read).join("\n"));
for (const f of CPP_FILES) run.check(exists(f), `C++ 真源文件 ${f} 不在库里（⑥ ⑥c ⑦ 的对拍没有可比的东西）`);
const failedFacts = failedCppFacts(CPP_FILES.length - cppFile.length, cpp);
for (const name of Object.values(cppExpected)) {
    run.check(
        !failedFacts.includes(name),
        `C++ 的 ${name}：${CPP_FILES.join("、")} 里抽不到它（文件缺失，或名字、赋值形态、表声明被改了？）`,
    );
}

// ---------------------------------------------------------------- ⑥b ⑥c 色标
const tokens = read("web/shared/tokens.css");
const pageGradient = parseGradient(/--page-gradient:\s*([^;]+);/.exec(tokens)?.[1] ?? "");
run.check(
    pageGradient !== null && pageGradient.stops.length >= 2,
    "tokens.css 读不到 --page-gradient 的 linear-gradient 字面量",
);
const backdrop = /--page-backdrop:\s*(#[0-9a-fA-F]{6})\s*;/.exec(tokens)?.[1];
run.check(Boolean(backdrop), "tokens.css 读不到 --page-backdrop");
if (pageGradient && backdrop) {
    const stopColors = pageGradient.stops.map((s) => s.split(" ")[0]);
    run.check(
        !stopColors.includes(hexOf(backdrop)),
        "⑥b：--page-backdrop（外圈色）与占位渐变的某个色标相同，两个角色被写成了同值",
    );
}
if (cpp.stops !== null && pageGradient) {
    run.eq(
        cpp.stops.malformed,
        0,
        "⑥c：C++ 的 kShellBackdropStops 有读不懂的项（每项必须是 {0xRRGGBB, 位置}，rgb 在前）",
    );
    run.eq(
        cpp.stops.stops,
        pageGradient.stops,
        "⑥c：C++ 的 kShellBackdropStops 与 tokens.css 的 --page-gradient 色标不同",
    );
}
if (cpp.angle !== null && pageGradient) {
    run.eq(Number(cpp.angle), pageGradient.angle, "⑥c：C++ 的占位角度与 --page-gradient 不同");
}

// ---------------------------------------------------------------- ④ ⑥ ⑦ 逐个 index.html
const ES5_BANNED = [/=>/, /\bconst\b/, /\blet\b/, /`/, /\basync\b/, /\bawait\b/, /\?\./, /\?\?/, /\bclass\b/, /\.\.\./];

for (const role of roles) {
    const file = `web/${role}/index.html`;
    const html = read(file);
    const head = html.slice(0, html.indexOf("</head>"));
    const scripts = [...head.matchAll(/<script\b([^>]*)>([\s\S]*?)<\/script>/g)].map((m) => ({
        attrs: m[1],
        body: m[2],
        at: m.index,
    }));
    const firstResource = Math.min(
        ...[...head.matchAll(/<link\b[^>]*\brel="stylesheet"|<script\b[^>]*\bsrc=/g)].map((m) => m.index),
        head.length,
    );

    // ④ boot 守卫
    const boot = scripts.find((s) => s.body.includes("__rinne__bootError"));
    run.check(Boolean(boot), `${role}：④ 找不到 boot 守卫（事件 __rinne__bootError）`);
    if (boot) {
        run.check(
            !/type\s*=\s*"module"/.test(boot.attrs) && !/\bsrc=/.test(boot.attrs),
            `${role}：④ boot 守卫必须是内联的普通 script`,
        );
        run.check(boot.at < firstResource, `${role}：④ boot 守卫必须排在所有外链资源之前`);
        for (const re of ES5_BANNED)
            run.check(!re.test(boot.body.replace(/\/\/.*$/gm, "")), `${role}：⑤ boot 守卫用了非 ES5 语法 ${re}`);
        if (cpp.boot !== null) run.eq("__rinne__bootError", cpp.boot, `${role}：④ boot 事件名与 C++ 不一致`);
    }

    // ⑥ 占位底色
    const styleBlocks = [...head.matchAll(/<style\b[^>]*>([\s\S]*?)<\/style>/g)].filter((m) => m.index < firstResource);
    const gradients = styleBlocks.flatMap((m) => [...m[1].matchAll(/linear-gradient\([^)]*\)/g)]);
    run.eq(gradients.length, 1, `${role}：⑥ 外链样式之前的内联样式里必须恰好一条 linear-gradient 占位底`);
    const inline = gradients.length ? parseGradient(gradients[0][0]) : null;
    run.check(
        inline !== null && inline.stops.length >= 2,
        `${role}：⑥ 占位底必须是「linear-gradient(<角>deg, #rrggbb n%, …)」字面量`,
    );
    if (inline && pageGradient) {
        run.eq(inline.stops, pageGradient.stops, `${role}：⑥c 内联占位色标与 tokens.css 的 --page-gradient 不同`);
        run.eq(inline.angle, pageGradient.angle, `${role}：⑥c 内联占位角度与 --page-gradient 不同`);
    }
    run.check(
        /min-height:\s*100%/.test(styleBlocks.map((m) => m[1]).join("")),
        `${role}：⑥ 占位样式缺 min-height: 100%（外链缺席时渐变会塌）`,
    );
    run.check(
        /background-attachment:\s*fixed/.test(styleBlocks.map((m) => m[1]).join("")),
        `${role}：⑥ 占位样式缺 background-attachment: fixed`,
    );

    // ⑦ 首帧信号
    const ff = scripts.find((s) => s.body.includes("__rinne__firstFrame"));
    run.check(Boolean(ff), `${role}：⑦ 找不到首帧信号（事件 __rinne__firstFrame）`);
    if (ff) {
        const body = ff.body.replace(/\/\/.*$/gm, "");
        if (cpp.first !== null) run.eq("__rinne__firstFrame", cpp.first, `${role}：⑦ 首帧事件名与 C++ 不一致`);
        run.check(
            /requestAnimationFrame\(function\s*\(\)\s*\{\s*window\.requestAnimationFrame\(signal\)/.test(body),
            `${role}：⑦ 武装必须是嵌套两层 requestAnimationFrame`,
        );
        run.check(
            /observe\(\{\s*type:\s*"paint"/.test(body) && /armOnce\(\)/.test(body),
            `${role}：⑦ 武装必须由 paint 记录接线`,
        );
        run.check(
            /DOMContentLoaded/.test(body),
            `${role}：⑦ 缺回落路（没有 PerformanceObserver 时回落到 DOMContentLoaded）`,
        );
        const guard = /guard\s*=\s*setTimeout\(\s*function\s*\(\)\s*\{([\s\S]*?)\}\s*,\s*(\d+)\s*\)/.exec(body);
        run.check(Boolean(guard), `${role}：⑦ 缺保险定时器`);
        if (guard) {
            run.check(/^\s*signal\(\);\s*$/.test(guard[1]), `${role}：⑦ 保险的回调必须直接发信号，不能绕两层 rAF`);
            run.check(Number(guard[2]) === 2500, `${role}：⑦ 保险定时器应为 2500 ms，实得 ${guard[2]}`);
        }
        const post = body.indexOf("postMessage(");
        const sent = body.indexOf("sent = true");
        const clear = body.indexOf("clearTimeout(guard)");
        run.check(
            post >= 0 && sent > post && clear > post,
            `${role}：⑦ 撤网（sent 置位、清保险）必须排在 postMessage 之后`,
        );
        if (cpp.delta !== null) {
            run.check(body.includes(cpp.delta), `${role}：⑦ 载荷字段名与 C++ 的 ${cpp.delta} 不一致`);
        }
    }
}

// ---------------------------------------------------------------- ⑧ 打包与接线
// 嵌入、入口、工厂这三处都在插件目标里编，宿主测试编不到，写错了的症状是空白窗口加看门狗超时，
// 而页面预览（按真实目录服务）照样全绿。所以这里把它们当文本读，逐项断言。

/** 去掉 CMake 的 # 注释（本仓的 cmake 里没有引号内的 #）。 */
function stripCmakeComments(text) {
    return text.replace(/#.*$/gm, "");
}

/**
 * 从 RinneWebAssets.cmake 解析出打包 glob 的规则：键是「r|f:目录」（r 递归，f 不递归），值是排序后的扩展名。
 * 目录里的 ${ARG_ROLE_DIR} 记作 role；EXTRA_DIRS 的那一条（目录是变量 ${d}）不在这里，由调用点的 EXTRA_DIRS 另算。
 */
function parseCmakeGlobRules(cmakeText) {
    const rules = {};
    const globs = stripCmakeComments(cmakeText).matchAll(
        /file\(\s*GLOB(_RECURSE)?\s+(\w+)\s+CONFIGURE_DEPENDS([^)]*)\)/g,
    );
    for (const m of globs) {
        for (const g of m[3].matchAll(/"\$\{web_root\}\/([^"]+?)\/\*\.(\w+)"/g)) {
            if (g[1].includes("${d}")) continue;
            const dir = g[1] === "${ARG_ROLE_DIR}" ? "role" : g[1];
            const key = `${m[1] ? "r" : "f"}:${dir}`;
            rules[key] = [...(rules[key] || []), g[2]].sort();
        }
    }
    return rules;
}

/** 脚本里的口径表（PACKAGING）换成同一种形态。 */
function tableRules(table) {
    const rules = {};
    for (const r of table) rules[`${r.recursive ? "r" : "f"}:${r.dir}`] = [...r.exts].sort();
    return rules;
}

/** 解析 sources-webview.cmake 里对 rinne_add_web_assets 的调用；没有调用返回 null。 */
function parseAddWebAssetsCall(cmakeText) {
    const m = /\brinne_add_web_assets\(([^)]*)\)/.exec(stripCmakeComments(cmakeText));
    if (!m) return null;
    const words = m[1].match(/"[^"]*"|\S+/g) ?? [];
    const keys = new Set(["TARGET", "NAMESPACE", "ROLE_DIR", "LICENSE_FILE", "EXTRA_DIRS"]);
    const call = { TARGET: null, NAMESPACE: null, ROLE_DIR: null, LICENSE_FILE: null, EXTRA_DIRS: [] };
    let key = null;
    for (const w of words) {
        if (keys.has(w)) key = w;
        else if (key === "EXTRA_DIRS") call.EXTRA_DIRS.push(w);
        else if (key) call[key] = w.replace(/^"|"$/g, "");
    }
    return call;
}

/** 一个角色的 sources-webview.cmake 该满足的条件；返回问题列表（空表示没问题）。 */
function checkRoleCmake(text, { role, plugin, namespace }) {
    const problems = [];
    const call = parseAddWebAssetsCall(text);
    if (!call) return ["没有调用 rinne_add_web_assets"];
    if (call.ROLE_DIR !== role) problems.push(`ROLE_DIR 应为 ${role}，实得 ${call.ROLE_DIR}`);
    if (call.NAMESPACE !== namespace) problems.push(`NAMESPACE 应为 ${namespace}，实得 ${call.NAMESPACE}`);
    if (call.LICENSE_FILE !== "${CMAKE_SOURCE_DIR}/" + LICENSE_FILE) {
        problems.push(`LICENSE_FILE 应为 \${CMAKE_SOURCE_DIR}/${LICENSE_FILE}，实得 ${call.LICENSE_FILE}`);
    }
    if (!call.TARGET) {
        problems.push("缺 TARGET");
    } else {
        const link = new RegExp(`target_link_libraries\\(\\s*${plugin}\\s+PRIVATE\\s+${call.TARGET}\\s*\\)`);
        if (!link.test(stripCmakeComments(text))) problems.push(`没有把 ${call.TARGET} 以 PRIVATE 链进 ${plugin}`);
    }
    return problems;
}

/**
 * 插件目录的 CMakeLists.txt 该满足的条件：juce_add_plugin 之后按 sources-*.cmake 收集并 include（去注释后判；
 * 注释里提到同名模式不算）。sources-webview.cmake 要对插件目标做链接，目标必须已经建好。
 */
function checkDirCmake(text) {
    const code = stripCmakeComments(text);
    const problems = [];
    const plugin = code.indexOf("juce_add_plugin(");
    const glob =
        /file\(\s*GLOB\s+(\w+)\s+CONFIGURE_DEPENDS\s+"\$\{CMAKE_CURRENT_SOURCE_DIR\}\/sources-\*\.cmake"\s*\)/.exec(
            code,
        );
    if (plugin < 0) problems.push("没有 juce_add_plugin");
    if (!glob) {
        problems.push("没有按 sources-*.cmake 收集增量文件（嵌入资源的那一份不会被 include）");
    } else {
        if (glob.index < plugin) problems.push("收集 sources-*.cmake 排在 juce_add_plugin 之前（那时插件目标还没建）");
        const loop = new RegExp(String.raw`foreach\(\s*\w+\s+IN\s+LISTS\s+${glob[1]}\s*\)[^]*?include\(`);
        if (!loop.test(code.slice(glob.index))) problems.push("收集到的文件没有被 include");
    }
    return problems;
}

/** 插件入口文件该满足的条件（去注释后的文本）；返回问题列表。 */
function checkEntrySource(text, { role, namespace }) {
    const problems = [];
    const code = stripCppComments(text);
    if (!code.includes(`#include <${namespace}.h>`)) problems.push(`没有包含 <${namespace}.h>`);
    for (const member of ["namedResourceListSize", "originalFilenames", "namedResourceList", "getNamedResource"]) {
        if (!new RegExp(`\\b${namespace}::${member}\\b`).test(code))
            problems.push(`没有把 ${namespace}::${member} 交给工厂`);
    }
    if (!new RegExp(`\\boptions\\.role\\s*=\\s*"${role}"\\s*;`).test(code))
        problems.push(`options.role 不是 "${role}"`);
    if (!/\boptions\.probeEditorFactory\s*=[^;]*\bcreateProbeEditor\s*\(\s*\)/.test(code)) {
        problems.push("探针编辑器没有经 options.probeEditorFactory 注入");
    }
    if (!/\beditor::createEditorOrGeneric\s*\(\s*\*this\s*,\s*options\s*\)/.test(code)) {
        problems.push("createEditor 没有调编辑器工厂的入口 createEditorOrGeneric");
    }
    if (/GenericAudioProcessorEditor/.test(code)) problems.push("入口自己造了通用编辑器（回退规则只该在工厂里）");
    const other = namespace === "RinneInputWebData" ? "RinneOutputWebData" : "RinneInputWebData";
    if (code.includes(other)) problems.push(`引用了另一个角色的命名空间 ${other}`);
    return problems;
}

/** 工厂把 Options 原样转给网页编辑器的三条赋值；返回缺的那几条。 */
function checkFactoryForwarding(text) {
    const code = stripCppComments(text);
    const fields = [
        ["role", "role"],
        ["resourceSource", "resources"],
        ["controls", "controls"],
    ];
    return fields
        .filter(([to, from]) => !new RegExp(`\\bconfig\\.${to}\\s*=\\s*options\\.${from}\\s*;`).test(code))
        .map(([to]) => `config.${to} 没有取自 options`);
}

// 自测：好例零问题，每种坏例各报出对应的问题。
{
    const goodGlobs = `
        file(GLOB_RECURSE role_files CONFIGURE_DEPENDS
          "\${web_root}/\${ARG_ROLE_DIR}/*.html"
          "\${web_root}/\${ARG_ROLE_DIR}/*.js")
        file(GLOB shared_files CONFIGURE_DEPENDS "\${web_root}/shared/*.png" "\${web_root}/shared/*.js")
        # file(GLOB old CONFIGURE_DEPENDS "\${web_root}/old/*.js")
        foreach(d IN LISTS ARG_EXTRA_DIRS)
          file(GLOB d_files CONFIGURE_DEPENDS "\${web_root}/\${d}/*.js")
        endforeach()`;
    run.eq(
        parseCmakeGlobRules(goodGlobs),
        { "r:role": ["html", "js"], "f:shared": ["js", "png"] },
        "⑧ 自测：glob 规则按递归与否分键，注释掉的和 EXTRA_DIRS 的不算",
    );
    run.eq(
        tableRules([{ dir: "x", recursive: true, exts: ["js", "css"] }]),
        { "r:x": ["css", "js"] },
        "⑧ 自测：口径表与 cmake 解析是同一种形态",
    );

    const goodCmake = `
        # 说明
        rinne_add_web_assets(
          TARGET RinneInputWebAssets
          NAMESPACE RinneInputWebData
          ROLE_DIR input
          LICENSE_FILE "\${CMAKE_SOURCE_DIR}/LICENSE")
        target_link_libraries(RinneInput PRIVATE RinneInputWebAssets)`;
    const want = { role: "input", plugin: "RinneInput", namespace: "RinneInputWebData" };
    const cmakeProblems = (text) => checkRoleCmake(text, want);
    run.eq(cmakeProblems(goodCmake), [], "⑧ 自测：好的 sources-webview.cmake 零问题");
    run.eq(cmakeProblems("# 只有注释"), ["没有调用 rinne_add_web_assets"], "⑧ 自测：没有调用要报");
    run.eq(
        cmakeProblems(goodCmake.replace("ROLE_DIR input", "ROLE_DIR output")),
        ["ROLE_DIR 应为 input，实得 output"],
        "⑧ 自测：角色目录写错要报",
    );
    run.eq(
        cmakeProblems(goodCmake.replace("NAMESPACE RinneInputWebData", "NAMESPACE RinneOutputWebData")),
        ["NAMESPACE 应为 RinneInputWebData，实得 RinneOutputWebData"],
        "⑧ 自测：命名空间写错要报",
    );
    run.check(
        cmakeProblems(goodCmake.replace(/\s*LICENSE_FILE[^)]*\)/, ")")).some((p) => p.startsWith("LICENSE_FILE")),
        "⑧ 自测：漏掉 LICENSE_FILE 要报",
    );
    run.check(
        cmakeProblems(goodCmake.replace("PRIVATE RinneInputWebAssets", "PRIVATE Other")).some((p) =>
            p.startsWith("没有把"),
        ),
        "⑧ 自测：没把资源目标链进插件要报",
    );
    run.check(
        cmakeProblems(
            goodCmake.replace("target_link_libraries(RinneInput ", "# target_link_libraries(RinneInput "),
        ).some((p) => p.startsWith("没有把")),
        "⑧ 自测：注释掉的链接不算",
    );
    run.eq(
        parseAddWebAssetsCall("rinne_add_web_assets(TARGET T NAMESPACE N ROLE_DIR r EXTRA_DIRS a b)")?.EXTRA_DIRS,
        ["a", "b"],
        "⑧ 自测：EXTRA_DIRS 收齐",
    );

    const goodDir = [
        "juce_add_plugin(RinneInput PRODUCT_NAME x)",
        "# 说明里提到 sources-*.cmake 不算",
        'file(GLOB _parts CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/sources-*.cmake")',
        "foreach(_part IN LISTS _parts)",
        '  include("${_part}")',
        "endforeach()",
    ].join(String.fromCharCode(10));
    run.eq(checkDirCmake(goodDir), [], "⑧ 自测：好的目录 CMakeLists 零问题");
    run.check(
        checkDirCmake(goodDir.replace("/sources-*.cmake", "/source-*.cmake")).some((p) =>
            p.startsWith("没有按 sources"),
        ),
        "⑧ 自测：注释里还留着 sources-*.cmake 也不能让改坏的收集通过",
    );
    run.check(
        checkDirCmake(goodDir.replace("  include(", "  message(")).includes("收集到的文件没有被 include"),
        "⑧ 自测：收集了不 include 要报",
    );
    run.check(
        checkDirCmake(
            [
                "juce_add_plugin(RinneInput PRODUCT_NAME x)",
                '# file(GLOB _parts CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/sources-*.cmake")',
            ].join(String.fromCharCode(10)),
        ).some((p) => p.startsWith("没有按 sources")),
        "⑧ 自测：被注释掉的收集不算",
    );
    run.check(
        checkDirCmake(
            ["file(GLOB _parts CONFIGURE_DEPENDS " + '"${CMAKE_CURRENT_SOURCE_DIR}/sources-*.cmake")', goodDir].join(
                String.fromCharCode(10),
            ),
        ).some((p) => p.includes("juce_add_plugin 之前")),
        "⑧ 自测：收集排在 juce_add_plugin 之前要报",
    );

    const goodEntry = `
        #include <RinneInputWebData.h> // 注释
        void f() {
            editor::Options options;
            options.role = "input";
            options.resources = {RinneInputWebData::namedResourceListSize, RinneInputWebData::originalFilenames,
                                 RinneInputWebData::namedResourceList, &RinneInputWebData::getNamedResource};
            options.probeEditorFactory = [this] { return createProbeEditor(); };
            return editor::createEditorOrGeneric(*this, options);
        }`;
    const wantEntry = { role: "input", namespace: "RinneInputWebData" };
    const entryProblems = (text) => checkEntrySource(text, wantEntry);
    run.eq(entryProblems(goodEntry), [], "⑧ 自测：好的入口文件零问题");
    run.check(
        entryProblems(
            goodEntry.replace("#include <RinneInputWebData.h>", "// #include <RinneInputWebData.h>"),
        ).includes("没有包含 <RinneInputWebData.h>"),
        "⑧ 自测：注释掉的包含不算",
    );
    run.check(
        entryProblems(goodEntry.replace("&RinneInputWebData::getNamedResource", "nullptr")).includes(
            "没有把 RinneInputWebData::getNamedResource 交给工厂",
        ),
        "⑧ 自测：漏交一个成员要报",
    );
    run.check(
        entryProblems(goodEntry.replace('"input"', '"output"')).includes('options.role 不是 "input"'),
        "⑧ 自测：角色写错要报",
    );
    run.check(
        entryProblems(goodEntry.replace("options.probeEditorFactory = ", "auto unused = ")).includes(
            "探针编辑器没有经 options.probeEditorFactory 注入",
        ),
        "⑧ 自测：探针没有注入要报",
    );
    run.check(
        entryProblems(
            goodEntry.replace(
                "editor::createEditorOrGeneric(*this, options)",
                "new juce::GenericAudioProcessorEditor(*this)",
            ),
        ).length >= 2,
        "⑧ 自测：入口绕过工厂自己造通用编辑器要报",
    );
    run.check(
        entryProblems(goodEntry + "\nauto x = RinneOutputWebData::namedResourceList;").some((p) =>
            p.includes("另一个角色"),
        ),
        "⑧ 自测：串到另一个角色的命名空间要报",
    );

    const goodFactory = [
        "config.role = options.role;",
        "config.resourceSource = options.resources;",
        "config.controls = options.controls;",
    ].join(String.fromCharCode(10));
    run.eq(checkFactoryForwarding(goodFactory), [], "⑧ 自测：好的工厂转发零问题");
    run.eq(
        checkFactoryForwarding(
            goodFactory.replace(
                "config.resourceSource = options.resources;",
                "// config.resourceSource = options.resources;",
            ),
        ),
        ["config.resourceSource 没有取自 options"],
        "⑧ 自测：资源没转给网页编辑器要报（空 Source 会让窗口空白）",
    );
    run.eq(
        checkFactoryForwarding(goodFactory.replace("config.controls = options.controls;", "")),
        ["config.controls 没有取自 options"],
        "⑧ 自测：钩子没转要报",
    );
}

// ⑧-1 cmake 的打包 glob 与本脚本的口径表逐条相同
const webAssetsCmake = read("cmake/RinneWebAssets.cmake");
run.eq(
    parseCmakeGlobRules(webAssetsCmake),
    tableRules(PACKAGING),
    "⑧ cmake/RinneWebAssets.cmake 的打包 glob 与 packagedFiles 的口径不同（改一处要同批改另一处）",
);
run.check(
    /HEADER_NAME\s+\$\{ARG_NAMESPACE\}\.h\b/.test(stripCmakeComments(webAssetsCmake)),
    "⑧ juce_add_binary_data 的头文件名应为 <NAMESPACE>.h（插件入口按这个名字包含）",
);

// ⑧-2 根 CMakeLists：先 include 资源函数，再进插件目录
const rootCmake = stripCmakeComments(read("CMakeLists.txt"));
const includeAt = rootCmake.indexOf("include(RinneWebAssets");
run.check(
    includeAt >= 0 && includeAt < rootCmake.indexOf("add_subdirectory(src/input)"),
    "⑧ 根 CMakeLists 必须在 add_subdirectory(src/input) 之前 include(RinneWebAssets)",
);

// ⑧-3 两个角色：cmake 调用、入口文件
for (const role of roles) {
    const cap = role[0].toUpperCase() + role.slice(1);
    const plugin = `Rinne${cap}`;
    const namespace = `${plugin}WebData`;

    for (const p of checkDirCmake(read(`src/${role}/CMakeLists.txt`))) {
        run.check(false, `${role}：⑧ src/${role}/CMakeLists.txt：${p}`);
    }
    const cmakeFile = `src/${role}/sources-webview.cmake`;
    run.check(exists(cmakeFile), `${role}：⑧ 缺 ${cmakeFile}（页面资源没有嵌进插件）`);
    if (exists(cmakeFile)) {
        const text = read(cmakeFile);
        for (const p of checkRoleCmake(text, { role, plugin, namespace }))
            run.check(false, `${role}：⑧ ${cmakeFile}：${p}`);
        // 调用点带了 EXTRA_DIRS 时，口径表里要有对应的行，否则 packagedFiles 与实际嵌入不同
        for (const d of parseAddWebAssetsCall(text)?.EXTRA_DIRS ?? []) {
            run.check(
                PACKAGING.some((r) => r.dir === d),
                `${role}：⑧ ${cmakeFile} 的 EXTRA_DIRS 有 ${d}，packagedFiles 的口径表里没有`,
            );
        }
    }
    const entryFile = `src/${role}/${cap}PluginEntry.cpp`;
    run.check(exists(entryFile), `${role}：⑧ 缺 ${entryFile}`);
    if (exists(entryFile)) {
        for (const p of checkEntrySource(read(entryFile), { role, namespace }))
            run.check(false, `${role}：⑧ ${entryFile}：${p}`);
    }
}
run.check(exists(LICENSE_FILE), `⑧ 仓库根没有 ${LICENSE_FILE}（随包嵌入的许可证文本）`);

// ⑧-4 工厂把 Options 原样转给网页编辑器
for (const p of checkFactoryForwarding(read("src/plugin-common/RinneEditorFactory.cpp"))) {
    run.check(false, `⑧ src/plugin-common/RinneEditorFactory.cpp：${p}`);
}

run.finish();
