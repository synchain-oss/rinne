// SPDX-License-Identifier: GPL-3.0-or-later
//
// 离线：插件不联网。src/ 的 C/C++/Objective-C 源码里不得出现网络 API，也不得让 WebView 打开外部页面
// （goToURL 只能指向本地资源提供器）。判据按名字认会取数的接口，
// 所以 juce::URL 本身不判：用户点击后用 launchInDefaultBrowser 把链接交给系统浏览器是唯一的正当用法，
// 同一行再取数照样判红。web/ 里不得加载外部 http(s) 资源，
// 也不得在 WebView 里直接打开外链（外链一律经桥交给 C++ 打开浏览器）。JUCE 资源提供器的地址
// juce.backend 是本地的，放行。判据只看代码：C/C++ 去掉注释，HTML 去掉 <!-- -->，CSS 与 JS 去掉块注释。

import fs from "node:fs";
import path from "node:path";
import { caseResult, trackedFiles } from "../lib/check-kit.mjs";

const SRC_FILE = /^src\/.*\.(?:c|cc|cpp|cxx|h|hpp|m|mm)$/;
const WEB_FILE = /^web\/.*\.(?:html?|css|js|mjs|svg)$/;
const LOCAL_HOSTS = ["juce.backend"];

// 外部地址：http(s)、ws(s) 或协议相对的 //host，主机不是本地资源提供器（主机名后面可以直接是行尾）
const LOCAL = LOCAL_HOSTS.map((h) => h.replace(/\./g, "\\.")).join("|");
const EXT = `(?:(?:https?|wss?):)?\\/\\/(?!(?:${LOCAL})(?:[/:"'\`\\s)]|$))`;

/** C/C++ 里的网络 API：JUCE 的网络类与 URL 取数方法、WebView 打开外部页面、系统的 HTTP 与 socket 接口。 */
const NATIVE = [
    [
        "JUCE URL 取数",
        /\b(?:InputStreamOptions|WebInputStream|DownloadTask)\b|\.(?:readEntire(?:Text|Binary|Xml)Stream|downloadToFile)\s*\(/,
    ],
    ["WebView 打开外部页面", new RegExp(`\\bgoToURL\\s*\\([^;]*?"${EXT}`)],
    ["JUCE socket", /\b(?:StreamingSocket|DatagramSocket)\b|\b(?:connectToSocket|beginWaitingForSocket)\s*\(/],
    ["本机网络信息", /\b(?:IPAddress|MACAddress)::/],
    [
        "Windows 网络接口",
        /\b(?:WinHttp[A-Z]\w*|Internet(?:Open|Connect|ReadFile|OpenUrl)\w*|URLDownloadTo\w+|WSAStartup)\b/,
    ],
    [
        "网络头文件",
        /#\s*include\s*[<"](?:winsock2?|ws2tcpip|winhttp|wininet|urlmon|sys\/socket|netdb|arpa\/inet|curl\/curl)\.h[>"]/,
    ],
    ["macOS 网络接口", /\b(?:NSURLSession|NSURLConnection|CFHTTP\w+|CFStreamCreatePairWithSocket\w*)\b/],
    ["其他网络接口", /\b(?:curl_easy_\w+|getaddrinfo|gethostbyname)\b/],
];

/** web/ 里会让 WebView 自己去取外部内容或离开插件页面的写法。 */
const WEB = [
    [
        "HTML 资源属性",
        new RegExp(
            `<[A-Za-z][^>]*?\\s(?:src|srcset|poster|data|action|formaction|href|xlink:href)\\s*=\\s*["']?\\s*${EXT}`,
            "i",
        ),
    ],
    ["JS 元素属性", new RegExp(`\\.(?:src|srcset|href|action|data)\\s*=\\s*["'\`]${EXT}`)],
    ["CSS 外部资源", new RegExp(`(?:\\burl\\(|@import)\\s*["']?\\s*${EXT}`, "i")],
    [
        "JS 外部请求",
        new RegExp(`\\b(?:fetch|import|importScripts|EventSource|WebSocket|sendBeacon|open)\\s*\\(\\s*["'\`]${EXT}`),
    ],
    ["XHR 外部请求", new RegExp(`\\.open\\s*\\(\\s*["'\`][A-Za-z]+["'\`]\\s*,\\s*["'\`]${EXT}`)],
    [
        "HTML 自动跳转",
        new RegExp(`<meta\\b[^>]*http-equiv\\s*=\\s*["']?refresh[^>]*\\burl\\s*=\\s*["']?\\s*${EXT}`, "i"),
    ],
    ["JS 外部模块", new RegExp(`\\bfrom\\s*["']${EXT}`)],
    [
        "JS 页面跳转",
        new RegExp(
            `\\blocation(?:\\.href)?\\s*=\\s*["'\`]${EXT}|\\blocation\\.(?:assign|replace)\\s*\\(\\s*["'\`]${EXT}`,
        ),
    ],
    ["WebSocket", /\bnew\s+WebSocket\b/],
];

/** C/C++：注释换成空格（换行保留），代码与字符串原样；字符串里的 // 不当注释。 */
function stripC(t) {
    let out = "";
    let state = "code";
    for (let i = 0; i < t.length; i++) {
        const c = t[i];
        const two = t.slice(i, i + 2);
        if (state === "line") {
            if (c === "\n") state = "code";
            out += c === "\n" ? c : " ";
        } else if (state === "block") {
            if (two === "*/") {
                state = "code";
                out += "  ";
                i++;
            } else out += c === "\n" ? c : " ";
        } else if (state === "str" || state === "chr") {
            out += c;
            if (c === "\\") out += t[++i] ?? "";
            else if (c === (state === "str" ? '"' : "'") || c === "\n") state = "code";
        } else if (two === "//" || two === "/*") {
            state = two === "//" ? "line" : "block";
            out += "  ";
            i++;
        } else {
            if (c === '"') state = "str";
            else if (c === "'" && !/[0-9A-Za-z_]/.test(t[i - 1] || "")) state = "chr";
            out += c;
        }
    }
    return out;
}
const stripWeb = (t) =>
    t
        .replace(/<!--[\s\S]*?-->/g, (m) => m.replace(/[^\n]/g, " "))
        .replace(/\/\*[\s\S]*?\*\//g, (m) => m.replace(/[^\n]/g, " "));

/** files: [{ path, text }]。返回「路径:行 类别」形式的命中列表。 */
export function checkOffline(files) {
    const hits = [];
    for (const f of files) {
        const native = SRC_FILE.test(f.path);
        const rules = native ? NATIVE : WEB_FILE.test(f.path) ? WEB : null;
        if (!rules) continue;
        (native ? stripC(f.text) : stripWeb(f.text)).split(/\r?\n/).forEach((line, i) => {
            for (const [label, re] of rules) if (re.test(line)) hits.push(`${f.path}:${i + 1} ${label}`);
        });
    }
    return hits;
}

export function selfTest() {
    const cpp = (text) => [{ path: "src/plugin-common/About.cpp", text }];
    const web = (text, p = "web/input/index.html") => [{ path: p, text }];
    const cases = [
        ["系统浏览器打开链接", false, cpp('juce::URL ("https://synchain.ca").launchInDefaultBrowser();')],
        [
            "打开浏览器的同一行又取数",
            true,
            cpp("juce::URL (u).launchInDefaultBrowser(); auto s = u.readEntireTextStream();"),
        ],
        ["本地文件流", false, cpp("auto in = file.createInputStream();\nauto s = in->readEntireStreamAsString();")],
        [
            "注释里提到 WebInputStream",
            false,
            cpp("// 不用 WebInputStream：插件不联网\n/* StreamingSocket */ int x = 0;"),
        ],
        ["URL 取数", true, cpp("auto s = url.createInputStream (juce::URL::InputStreamOptions (p));")],
        ["字符串里的 // 之后的代码照扫", true, cpp('auto s = juce::URL ("https://x.example").readEntireTextStream();')],
        ["读整段文本", true, cpp("auto s = url.readEntireTextStream();")],
        ["读整段 XML", true, cpp("auto xml = url.readEntireXmlStream();")],
        ["WebView 打开外部页面", true, cpp('browser.goToURL ("https://example.com/ui");')],
        ["WebView 打开本地资源提供器", false, cpp('browser.goToURL ("https://juce.backend/index.html");')],
        ["socket", true, cpp("juce::StreamingSocket sock;")],
        ["读网卡地址", true, cpp("auto all = juce::MACAddress::getAllAddresses();")],
        ["WinHTTP", true, cpp('auto h = WinHttpOpen (L"x", 0, nullptr, nullptr, 0);')],
        ["网络头文件", true, cpp("#include <winsock2.h>")],
        ["libcurl 与域名解析", true, cpp("curl_easy_perform (h);\ngetaddrinfo (host, nullptr, &hints, &res);")],
        ["NSURLSession", true, [{ path: "src/plugin-common/Mac.mm", text: "[NSURLSession sharedSession];" }]],
        ["不在 src/ 下的文件不扫", false, [{ path: "tools/fetch.cpp", text: "WinHttpOpen();" }]],
        [
            "本地资源与相对路径",
            false,
            web('<link rel="stylesheet" href="../shared/base.css"><script src="app.js"></script>'),
        ],
        ["JUCE 资源提供器", false, web('fetch("https://juce.backend/state.json");', "web/shared/bridge.js")],
        ["本地资源提供器的主机名写到行尾", false, web("<script src=https://juce.backend\n></script>")],
        [
            "只是字符串里的地址",
            false,
            web('const SOURCE = "https://github.com/synchain-oss/rinne";', "web/shared/links.js"),
        ],
        ["变量名恰好叫 data", false, web('const data = { href: "https://synchain.ca" };', "web/shared/links.js")],
        ["JS 设元素的外部 src", true, web('img.src = "https://cdn.example.com/a.png";', "web/shared/x.js")],
        [
            "JS 模板里的外部脚本",
            true,
            web('el.innerHTML = `<script src="https://cdn.example.com/x.js"></script>`;', "web/shared/x.js"),
        ],
        ["HTML 注释里的外链", false, web('<!-- <script src="https://cdn.example.com/x.js"></script> -->')],
        ["外部脚本", true, web('<script src="https://cdn.example.com/x.js"></script>')],
        ["外部样式表", true, web('<link rel="stylesheet" href="//fonts.example.com/css">')],
        ["WebView 里直接打开外链", true, web('<a href="https://synchain.ca">site</a>')],
        [
            "CSS 外部字体",
            true,
            web("@font-face { src: url(https://fonts.example.com/a.woff2); }", "web/shared/tokens.css"),
        ],
        ["CSS @import", true, web('@import "https://fonts.example.com/css";', "web/shared/base.css")],
        ["JS fetch 外部", true, web('fetch("https://api.example.com/v1");', "web/shared/x.js")],
        ["JS 外部模块", true, web('import x from "https://esm.example.com/x.js";', "web/shared/x.js")],
        ["JS 页面跳转", true, web('location.href = "https://synchain.ca";', "web/shared/x.js")],
        ["location.assign 外部地址", true, web("window.location.assign('https://synchain.ca');", "web/shared/x.js")],
        ["location.replace 外部地址", true, web('location.replace("https://synchain.ca");', "web/shared/x.js")],
        ["XHR 外部请求", true, web("xhr.open('GET', 'https://api.example.com/v1');", "web/shared/x.js")],
        ["XHR 本地请求", false, web("xhr.open('GET', 'https://juce.backend/state.json');", "web/shared/x.js")],
        ["meta 自动跳转", true, web('<meta http-equiv="refresh" content="0;url=https://synchain.ca">')],
        ["WebSocket", true, web("const ws = new WebSocket(url);", "web/shared/x.js")],
    ].map(([label, bad, files]) => {
        const hits = checkOffline(files);
        return { label, bad, passed: bad ? hits.length > 0 : hits.length === 0 };
    });
    return caseResult(cases);
}

export default {
    name: "offline",
    group: "compliance",
    inputs: [],
    run(ctx) {
        const files = trackedFiles(ctx.root)
            .filter((p) => (SRC_FILE.test(p) || WEB_FILE.test(p)) && fs.existsSync(path.join(ctx.root, p)))
            .map((p) => ({ path: p, text: fs.readFileSync(path.join(ctx.root, p), "utf8") }));
        if (!files.length) return { skip: "尚无 src/ 与 web/ 的源文件" };
        const hits = checkOffline(files);
        return { ok: hits.length === 0, lines: hits };
    },
    selfTest,
};
