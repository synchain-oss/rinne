// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/web-preview.json
//
// 前端语法下限守卫（Node，无 DOM）。守的是「旧 WebView2 上整页 SyntaxError」这一类故障。
//
// 前端跑在用户机器的 WebView2 里，版本由用户的运行时决定，不由我们决定。某个 JS 文件一旦用了比插件声明的下限
// （PlatformWebView.h 的 kMinRuntimeMajor）更新的语法，旧运行时会在解析阶段整页 SyntaxError：脚本一行都不执行，
// 桥永不就绪，症状和「加载超时」一模一样，try/catch 又接不住。所以把它挡在静态面。
// 口径：每条规则写明「特性、最低 Chromium 版本」，断言用到的特性所需的最高版本不超过声明下限。
// 想用更新的语法可以，同一批把声明下限抬上去，并想清楚要不要把一批老运行时用户挡在「请升级」面板后面；
// 这套的作用是让那个决定必须被显式做出。
// 扫描面只含会被编进插件的文件：web/{input,output,shared,i18n}。web-preview 只在浏览器里跑，web/js/juce 是官方原样副本，都不在其列。
// C++ 侧的声明下限还没入库时，只列出用到的特性与事实下限，不判红；入库后自动生效。
//
// 用法：node web-preview/tests/smoke-frontend-syntax-floor.mjs [仓库根]

import fs from "node:fs";
import path from "node:path";
import { createRun, repoRoot } from "./lib/harness.mjs";

const ROOT = repoRoot();
const run = createRun("前端语法下限");

/** 特性表：正则、名字、最低 Chromium 主版本。只列解析期就会炸的语法，以及少数在旧引擎上直接 TypeError 的内建方法。 */
const FEATURES = [
    { re: /\?\?=|\|\|=|&&=/g, name: "逻辑赋值 ??= / ||= / &&=", chromium: 85 },
    { re: /\?\?/g, name: "空值合并 ??", chromium: 80 },
    { re: /\?\.[a-zA-Z_$([]/g, name: "可选链 ?.", chromium: 80 },
    { re: /^\s*static\s*\{/gm, name: "class static 初始化块", chromium: 94 },
    { re: /^\s*#[a-zA-Z_$][\w$]*\s*[=;(]|this\.#/gm, name: "class 私有字段", chromium: 74 },
    { re: /\(\?<[=!]/g, name: "正则 lookbehind", chromium: 62 },
    { re: /\.flatMap\(/g, name: "Array.prototype.flatMap", chromium: 69 },
    { re: /\.at\(/g, name: "Array/String.prototype.at", chromium: 92 },
    { re: /\breplaceAll\(/g, name: "String.prototype.replaceAll", chromium: 85 },
    { re: /\bObject\.hasOwn\(/g, name: "Object.hasOwn", chromium: 93 },
    { re: /\.toSorted\(|\.toReversed\(|\.findLast\(/g, name: "ES2023 数组方法", chromium: 110 },
    { re: /\bstructuredClone\(/g, name: "structuredClone", chromium: 98 },
    { re: /\bPromise\.any\(/g, name: "Promise.any", chromium: 85 },
    { re: /\bWeakRef\b/g, name: "WeakRef", chromium: 84 },
    { re: /\bimport\.meta\b/g, name: "import.meta", chromium: 64 },
];
const TOP_LEVEL_AWAIT = { name: "顶层 await", chromium: 89 };

/**
 * 把注释与字符串字面量掏空（保留换行，行号不变）。必须做：注释里常出现被讨论的语法本身，不掏空就是成片误报，
 * 而误报会让这套门禁很快被当噪音关掉。逐字符状态机，不用正则。不识别正则字面量，本仓的正则里没有引号，够用。
 */
export function stripCommentsAndStrings(src) {
    let out = "";
    let i = 0;
    const n = src.length;
    while (i < n) {
        const c = src[i];
        const next = src[i + 1];
        if (c === "/" && next === "/") {
            while (i < n && src[i] !== "\n") i++;
            continue;
        }
        if (c === "/" && next === "*") {
            i += 2;
            while (i < n && !(src[i] === "*" && src[i + 1] === "/")) {
                if (src[i] === "\n") out += "\n";
                i++;
            }
            i += 2;
            continue;
        }
        if (c === '"' || c === "'" || c === "`") {
            const quote = c;
            i++;
            while (i < n && src[i] !== quote) {
                if (src[i] === "\\") i++;
                else if (src[i] === "\n") out += "\n";
                i++;
            }
            i++;
            out += '""'; // 占位：保住「这里有个表达式」，内容不参与匹配
            continue;
        }
        out += c;
        i++;
    }
    return out;
}

/** HTML 只取 script 块内容。 */
const scriptsFromHtml = (html) =>
    [...html.matchAll(/<script\b[^>]*>([\s\S]*?)<\/script>/g)].map((m) => m[1]).join("\n");

/** 一段源码用到的受限特性：返回 Map(特性名 -> 最低 Chromium)。 */
export function featuresUsed(text) {
    const used = new Map();
    for (const f of FEATURES) {
        f.re.lastIndex = 0;
        if (f.re.test(text)) used.set(f.name, f.chromium);
    }
    // 顶层 await：零缩进的行里出现 await；缩进的都在函数体里
    if (text.split("\n").some((l) => /^\S.*\bawait\s/.test(l)))
        used.set(TOP_LEVEL_AWAIT.name, TOP_LEVEL_AWAIT.chromium);
    return used;
}

function listFiles(dir, exts) {
    const out = [];
    const walk = (d) => {
        for (const name of fs.readdirSync(d)) {
            const p = path.join(d, name);
            if (fs.statSync(p).isDirectory()) walk(p);
            else if (exts.some((e) => name.endsWith(e))) out.push(p);
        }
    };
    if (fs.existsSync(path.join(ROOT, dir))) walk(path.join(ROOT, dir));
    return out;
}

// 自带反例：规则必须真的认得出新语法，否则整套是恒真式
run.check(featuresUsed(stripCommentsAndStrings("const a = b ?? c;")).has("空值合并 ??"), "规则失效：认不出 ??");
run.check(featuresUsed(stripCommentsAndStrings("x?.y")).has("可选链 ?."), "规则失效：认不出 ?.");
run.check(
    featuresUsed(stripCommentsAndStrings("const m = await load();")).has("顶层 await"),
    "规则失效：认不出顶层 await",
);
run.check(
    featuresUsed(stripCommentsAndStrings("async function f() {\n    await g();\n}")).size === 0,
    "规则误报：函数体里的 await 不是顶层",
);
run.check(
    featuresUsed(stripCommentsAndStrings('// a ?? b\nconst s = "x?.y";')).size === 0,
    "规则误报：注释与字符串里的语法不算",
);

const header = ["src/plugin-common/PlatformWebView.h", "src/plugin-common/WebViewHost.h"]
    .map((p) => path.join(ROOT, p))
    .filter((p) => fs.existsSync(p))
    .map((p) => fs.readFileSync(p, "utf8"))
    .join("\n");
const floorMatch = /kMinRuntimeMajor\s*=\s*(\d+)/.exec(header);
const floor = floorMatch ? Number(floorMatch[1]) : null;
if (floor === null) run.skipItem("声明下限对拍", "kMinRuntimeMajor 还没入库，只列事实下限，入库后自动生效");
else run.log(`声明下限 kMinRuntimeMajor = ${floor}`);

const files = ["web/input", "web/output", "web/shared", "web/i18n"].flatMap((d) => listFiles(d, [".js", ".html"]));
const used = new Map(); // 特性名 -> { chromium, at: Set }
for (const abs of files) {
    const raw = fs.readFileSync(abs, "utf8");
    const text = stripCommentsAndStrings(abs.endsWith(".html") ? scriptsFromHtml(raw) : raw);
    for (const [name, chromium] of featuresUsed(text)) {
        if (!used.has(name)) used.set(name, { chromium, at: new Set() });
        used.get(name).at.add(path.relative(ROOT, abs).split(path.sep).join("/"));
    }
}

run.log(`扫描 ${files.length} 个会被编进插件的前端文件，用到的受限特性：${used.size ? "" : "（无）"}`);
let highest = 0;
for (const [name, info] of [...used].sort((a, b) => b[1].chromium - a[1].chromium)) {
    highest = Math.max(highest, info.chromium);
    run.log(`  ${name}：需 Chromium ${info.chromium}（${[...info.at].slice(0, 3).join("、")}）`);
    if (floor !== null) {
        run.check(
            info.chromium <= floor,
            `${name} 需要 Chromium ${info.chromium}，超过声明下限 ${floor}；同批抬高 kMinRuntimeMajor，或改写成更保守的语法`,
        );
    }
}
run.log(`前端事实下限 = Chromium ${highest || "（无约束）"}`);
run.finish();
