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
// C++ 侧文件（WebViewHost.h、PlatformWebView.h）都还没入库时，对拍项记「跳过」并说明原因；只要有一个入库，
// 事件名、字段名、角度与色标表就必须抽得出来，抽不出来判红，不静默跳过。其余照常断言。
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

/** 某个角色的打包集合。构建脚本的 glob 要与这里同口径：角色目录递归，其余目录不递归。 */
function packagedFiles(role) {
    return [
        ...listFiles(`web/${role}`, [".html", ".js", ".css"], true),
        ...listFiles("web/shared", [".js", ".css", ".png", ".json"]),
        ...listFiles("web/i18n", [".js"]),
        ...listFiles("web/js/juce", [".js"]),
        ...listFiles("web/fonts", [".woff2"]),
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
// 两个 C++ 文件都还没入库：对拍项记「跳过」并说明原因。只要有一个入库，名字就必须抽得出来：
// 抽不出来就判红，不静默跳过。抽取前先去掉注释（含被注释掉的旧声明），声明按「名字 = 值」「名字[] = {…};」的形态认，
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

/** 对拍项的归属：C++ 文件都没入库就全部记跳过；只要有一个入库，抽不到的名字就必须判红。 */
function classifyCppFacts(fileCount, facts) {
    if (fileCount === 0) return { skip: Object.values(cppExpected), fail: [] };
    return {
        skip: [],
        fail: Object.keys(cppExpected)
            .filter((k) => facts[k] === null)
            .map((k) => cppExpected[k]),
    };
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

    // 对拍项归属：没有 C++ 文件才跳过；有文件就必须抽得到
    const allNull = { boot: null, first: null, delta: null, angle: null, stops: null };
    const names = Object.values(cppExpected);
    run.eq(classifyCppFacts(0, allNull), { skip: names, fail: [] }, "归属自测：C++ 文件都不在时只记跳过");
    run.eq(classifyCppFacts(1, allNull), { skip: [], fail: names }, "归属自测：文件在但全抽不到时全部判红");
    run.eq(
        classifyCppFacts(2, { ...good, delta: null }).fail,
        ["kPaintDeltaField"],
        "归属自测：只有抽不到的那一项判红",
    );
}

const cppFile = CPP_FILES.filter(exists);
const cpp = extractCppFacts(cppFile.map(read).join("\n"));
const verdict = classifyCppFacts(cppFile.length, cpp);
const skipped = new Set(verdict.skip.map((name) => `C++ 的 ${name}`));
for (const name of Object.values(cppExpected)) {
    run.check(
        !verdict.fail.includes(name),
        `C++ 的 ${name}：${cppFile.join("、")} 已入库，但抽不到它（名字、赋值形态或表声明被改了？）`,
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

for (const s of skipped) run.skipItem(s, "C++ 侧文件还没入库；入库后这些项必须抽得到，抽不到判红");
run.finish();
