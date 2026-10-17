// SPDX-License-Identifier: GPL-3.0-or-later
//
// 离线扫描的判据（Node，纯函数）：build-static 的 textProblems 要能认出各种形态的外部引用与网络调用。
// 好例必须放行（注释里的地址、SVG 命名空间、普通文字），坏例必须判红；每条判据至少一个坏例，
// 删掉任一条判据都有用例变红。真实源树必须是干净的。
//
// 用法：node web-preview/tests/smoke-embedded-resources-scan.mjs [仓库根]

import path from "node:path";
import { pathToFileURL } from "node:url";
import { createRun, repoRoot } from "./lib/harness.mjs";

const ROOT = repoRoot();
const run = createRun("离线扫描判据");
const { textProblems, verify } = await import(pathToFileURL(path.join(ROOT, "web-preview", "build-static.mjs")).href);

const bad = (label, rel, text) => run.check(textProblems(rel, text).length > 0, `坏例应判红：${label}`);
const good = (label, rel, text) =>
    run.check(textProblems(rel, text).length === 0, `好例应放行：${label}（${textProblems(rel, text).join("；")}）`);

// 坏例：http(s) 字面量
bad("https 外链", "web/a.html", '<script src="https://cdn.example.invalid/a.js"></script>');
bad("http 外链", "web/a.css", "@import url(http://x.invalid/a.css);");
// 坏例：协议相对地址
bad("script src=//", "web/a.html", '<script src="//cdn.example.invalid/a.js"></script>');
bad("link href=// 单引号", "web/a.html", "<link rel='stylesheet' href='//cdn.example.invalid/a.css'>");
bad("img src 无引号", "web/a.html", "<img src=//cdn.example.invalid/p.gif>");
bad("@import url(//)", "web/a.css", "@import url(//x.invalid/a.css);");
bad("@import 字符串 //", "web/a.css", '@import "//x.invalid/a.css";');
bad("css url(//)", "web/a.css", "a { background: url('//x.invalid/p.png'); }");
bad("js img.src = '//'", "web/a.js", "const i = new Image(); i.src = '//cdn.example.invalid/p.gif';");
bad("js 字符串里的 //host", "web/a.js", 'const u = "//cdn.example.invalid/x";');
// 坏例：网络调用
bad("fetch(", "web/a.js", "fetch('/x');");
bad("间接 fetch", "web/a.js", "const f = window.fetch; f('/x');");
bad("window['fetch']", "web/a.js", "window['fetch']('/x');");
bad("sendBeacon", "web/a.js", "navigator.sendBeacon('/x', 'y');");
bad("XMLHttpRequest", "web/a.js", "const x = new XMLHttpRequest();");
bad("WebSocket", "web/a.js", "new WebSocket('ws://x');");
bad("EventSource", "web/a.js", "new EventSource('/x');");
bad("importScripts", "web/a.js", "importScripts('a.js');");
bad("new Worker", "web/a.js", "new Worker('w.js');");
bad("html 里的内联脚本", "web/a.html", "<script>fetch('/x')</script>");

// 好例
good("注释里的地址", "web/a.js", "// see https://example.invalid/x\nconst a = 1;");
good("块注释里的地址", "web/a.css", "/* https://example.invalid/x */ a { color: red; }");
good("html 注释里的地址", "web/a.html", "<!-- https://example.invalid/x --><p>hi</p>");
good(
    "SVG 命名空间",
    "web/a.css",
    "a { background: url(\"data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg'%3E\"); }",
);
good(
    "相对路径",
    "web/a.html",
    '<script type="module" src="./a.js"></script><link href="../shared/x.css" rel="stylesheet">',
);
good("普通 import", "web/a.js", 'import { a } from "./a.js";');
good("JUCE 的宿主伪源", "web/js/juce/index.js", 'const origin = "https://juce.backend/";');
good("JUCE 副本里的网络词不查", "web/js/juce/index.js", "new XMLHttpRequest();");
good("正则里的双斜杠", "web/a.js", "const re = /a\\/\\/b/;");

// 真实源树
const fs = await import("node:fs");
const files = [];
const walk = (dir, rel) => {
    for (const name of fs.readdirSync(path.join(ROOT, dir)).sort()) {
        const r = `${rel}/${name}`;
        if (fs.statSync(path.join(ROOT, dir, name)).isDirectory()) walk(`${dir}/${name}`, r);
        else files.push({ rel: r, abs: path.join(ROOT, dir, name) });
    }
};
walk("web", "web");
run.eq(verify(files), [], "真实的 web/ 源树必须是干净的");

run.finish();
