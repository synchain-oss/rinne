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
// C++ 侧文件还没入库时，对拍项记「跳过」并说明原因，其余照常断言。
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

// ---------------------------------------------------------------- C++ 真源（还没入库就跳过对应项）
const cppFile = ["src/plugin-common/WebViewHost.h", "src/plugin-common/PlatformWebView.h"].filter(exists);
const cppText = cppFile.map(read).join("\n");
const cppConst = (name) => new RegExp(`${name}\\s*=\\s*"([^"]+)"`).exec(cppText)?.[1] ?? null;
const cpp = {
    boot: cppConst("kBootErrorEventId"),
    first: cppConst("kFirstFrameEventId"),
    delta: cppConst("kPaintDeltaField"),
    angle: /kShellBackdropAngleDeg\s*=\s*(\d+)/.exec(cppText)?.[1] ?? null,
    stops: /kShellBackdropStops[^{]*\{([\s\S]*?)\}\s*;/.exec(cppText)?.[1] ?? null,
};
const skipped = new Set();
const needCpp = (what, v) => {
    if (v === null) skipped.add(what);
    return v !== null;
};

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
if (needCpp("⑥c C++ 占位色标数组", cpp.stops) && pageGradient) {
    const cppStops = [...cpp.stops.matchAll(/(0x[0-9a-fA-F]{6}|#[0-9a-fA-F]{6})\s*,\s*(\d+(?:\.\d+)?)/g)].map(
        (m) =>
            `#${m[1].replace(/^0x|^#/, "").toLowerCase()} ${Math.round(Number(m[2]) <= 1 ? Number(m[2]) * 100 : Number(m[2]))}%`,
    );
    run.eq(cppStops, pageGradient.stops, "⑥c：C++ 的 kShellBackdropStops 与 tokens.css 的 --page-gradient 色标不同");
    if (cpp.angle !== null) run.eq(Number(cpp.angle), pageGradient.angle, "⑥c：C++ 的占位角度与 --page-gradient 不同");
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
        if (needCpp("④ C++ 的 kBootErrorEventId", cpp.boot))
            run.eq("__rinne__bootError", cpp.boot, `${role}：④ boot 事件名与 C++ 不一致`);
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
        if (needCpp("⑦ C++ 的 kFirstFrameEventId", cpp.first))
            run.eq("__rinne__firstFrame", cpp.first, `${role}：⑦ 首帧事件名与 C++ 不一致`);
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
        if (needCpp("⑦ C++ 的 paint 差值字段名", cpp.delta)) {
            run.check(body.includes(cpp.delta), `${role}：⑦ 载荷字段名与 C++ 的 ${cpp.delta} 不一致`);
        }
    }
}

for (const s of skipped) run.log(`  [跳过] ${s}：C++ 侧文件还没入库，入库后自动生效`);
run.finish();
