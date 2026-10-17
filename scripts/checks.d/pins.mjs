// SPDX-License-Identifier: GPL-3.0-or-later
//
// 工具钉版文件：每个文件恰好一行版本号；同一工具在多处出现时版本必须一致
// （.clang-format 头注释、requirements-tools.txt 与 .clang-format-version 对拍）。
// 构建用的五个钉版文件与工具钉版一样是必需的，缺一个就判红。WebView2 包的版本与 sha256 钉在 CMake 里，
// 与 THIRD-PARTY-NOTICES.md、third_party/notices/README.md 写的对拍。requirements-tools.txt 里字体检查要用的
// fontTools 与 brotli 必须在位、带版本与 sha256。

import fs from "node:fs";
import path from "node:path";
import { caseResult } from "../lib/check-kit.mjs";

const SEMVER = /^\d+\.\d+\.\d+$/;
export const PINS = {
    ".gitleaks-version": SEMVER,
    ".clang-format-version": SEMVER,
    ".prettier-version": SEMVER,
    ".actionlint-version": SEMVER,
    ".node-version": SEMVER,
    ".juce-commit": /^[0-9a-f]{40}$/,
    ".juce-version": SEMVER,
    ".pluginval-version": /^v?\d+\.\d+\.\d+$/,
    ".sccache-version": SEMVER,
    ".sccache-sha256": /^[0-9a-f]{64}$/,
};
const WEBVIEW2 = {
    cmake: "cmake/RinneDeps.cmake",
    notices: "THIRD-PARTY-NOTICES.md",
    readme: "third_party/notices/README.md",
};
const HASHED = ["fonttools", "brotli"];

/** requirements-tools.txt 的条目：续行接回一行、去掉注释行，{ name（pip 的规范名）, version, hashes }。 */
export function requirementEntries(text) {
    return text
        .replace(/\\\r?\n/g, " ")
        .split(/\r?\n/)
        .filter((l) => l.trim() && !l.trim().startsWith("#"))
        .map((l) => {
            const m = /^\s*([A-Za-z0-9._-]+)(?:\[[^\]]*\])?\s*(?:==\s*(\S+))?/.exec(l);
            const hashes = [...l.matchAll(/--hash=sha256:(\S+)/g)].map((h) => h[1]);
            return { name: (m?.[1] || "").toLowerCase().replace(/[-_.]+/g, "-"), version: m?.[2] || "", hashes };
        });
}

/** WebView2 包：CMake 里的版本与 sha256，和两份声明里写的是同一个。 */
function checkWebView2(read) {
    const cmake = read(WEBVIEW2.cmake) || "";
    const ver = /^\s*set\(WEBVIEW2_VERSION "([^"]*)"\)/m.exec(cmake)?.[1];
    const sha = /^\s*set\(WEBVIEW2_SHA256 "([^"]*)"\)/m.exec(cmake)?.[1];
    const errors = [];
    if (!/^\d+\.\d+\.\d+\.\d+$/.test(ver || "")) errors.push(`${WEBVIEW2.cmake} 里没有四段版本号的 WEBVIEW2_VERSION`);
    if (!/^[0-9a-f]{64}$/.test(sha || "")) errors.push(`${WEBVIEW2.cmake} 里没有 64 位小写十六进制的 WEBVIEW2_SHA256`);
    if (errors.length) return errors;
    // NOTICES 里每一行提到 WEBVIEW2_VERSION 的地方都写着当前版本，至少一处
    const lines = (read(WEBVIEW2.notices) || "").split(/\r?\n/).filter((l) => l.includes("WEBVIEW2_VERSION"));
    const seen = [...new Set(lines.flatMap((l) => l.match(/\b\d+\.\d+\.\d+\.\d+\b/g) || []))];
    if (!seen.length || seen.some((v) => v !== ver)) {
        errors.push(`${WEBVIEW2.notices} 写的 WebView2 版本（${seen.join("、") || "没有"}）与 CMake 的 ${ver} 不一致`);
    }
    const row = (read(WEBVIEW2.readme) || "").split(/\r?\n/).find((l) => l.includes("`Microsoft.Web.WebView2`")) || "";
    const rowVer = /`Microsoft\.Web\.WebView2`\s+(\S+)/.exec(row)?.[1];
    const rowSha = /sha256 `([0-9a-f]{64})`/.exec(row)?.[1];
    if (rowVer !== ver || rowSha !== sha)
        errors.push(`${WEBVIEW2.readme} 里 WebView2 包的版本或 sha256 与 CMake 不一致`);
    return errors;
}

/** read(rel) 返回文件文本或 null。 */
export function checkPins(read) {
    const errors = [];
    const value = {};
    for (const [file, re] of Object.entries(PINS)) {
        const text = read(file);
        if (text === null) {
            errors.push(`缺 ${file}`);
            continue;
        }
        value[file] = text.trim();
        if (!/^[^\r\n]+\n?$/.test(text) || !re.test(value[file])) errors.push(`${file} 必须恰好一行：${re.source}`);
    }
    const cf = value[".clang-format-version"];
    if (!new RegExp(`^# clang-format ${cf?.replace(/\./g, "\\.")}\\b`, "m").test(read(".clang-format") || "")) {
        errors.push(".clang-format 头注释里的版本与 .clang-format-version 不一致");
    }
    const req = requirementEntries(read("requirements-tools.txt") || "");
    const entry = (name) => req.find((e) => e.name === name);
    if (entry("clang-format")?.version !== cf) errors.push("requirements-tools.txt 的 clang-format 版本不一致");
    if (!SEMVER.test(entry("reuse")?.version || "")) errors.push("requirements-tools.txt 须钉住 reuse 的版本");
    for (const name of HASHED) {
        const e = entry(name);
        if (!e || !SEMVER.test(e.version)) errors.push(`requirements-tools.txt 须钉住 ${name} 的版本（字体检查要用）`);
        else if (!e.hashes.length || e.hashes.some((h) => !/^[0-9a-f]{64}$/.test(h))) {
            errors.push(`requirements-tools.txt 的 ${name} 须带 sha256 哈希（64 位小写十六进制）`);
        }
    }
    errors.push(...checkWebView2(read));
    return errors;
}

const H = (c) => c.repeat(64);
const FONT_REQ = `fonttools==4.65.0 \\\n    --hash=sha256:${H("4")}\nbrotli==1.2.0 \\\n    --hash=sha256:${H("e")} \\\n    --hash=sha256:${H("0")}\n`;
const GOOD = {
    ".gitleaks-version": "8.30.1\n",
    ".clang-format-version": "18.1.8\n",
    ".prettier-version": "3.9.6\n",
    ".actionlint-version": "1.7.12\n",
    ".node-version": "22.23.3\n",
    ".juce-commit": `${"d6181bde".repeat(5)}\n`,
    ".juce-version": "8.0.8\n",
    ".pluginval-version": "v1.0.4\n",
    ".sccache-version": "0.17.0\n",
    ".sccache-sha256": `${"e9".repeat(32)}\n`,
    ".clang-format": "# SPDX\n# clang-format 18.1.8：说明\nBasedOnStyle: LLVM\n",
    "requirements-tools.txt": `clang-format==18.1.8\nreuse==6.2.0\n${FONT_REQ}`,
    [WEBVIEW2.cmake]: `if(WIN32)\n  set(WEBVIEW2_VERSION "1.0.2957.106")\n  set(WEBVIEW2_SHA256 "${"4c".repeat(32)}")\n`,
    [WEBVIEW2.notices]:
        "| WebView2 loader | `WEBVIEW2_VERSION` in CMake (1.0.2957.106) |\n(version `WEBVIEW2_VERSION` in CMake, currently 1.0.2957.106).\n",
    [WEBVIEW2.readme]: `| \`webview2.LICENSE.txt\` | \`LICENSE.txt\` in the NuGet package \`Microsoft.Web.WebView2\` 1.0.2957.106 | package sha256 \`${"4c".repeat(32)}\` |\n`,
};

export default {
    name: "pins",
    group: "docs",
    inputs: [], // 钉版文件缺失在 checkPins 里直接判红，不能变成 SKIP
    run(ctx) {
        const read = (rel) => {
            const abs = path.join(ctx.root, rel);
            return fs.existsSync(abs) ? fs.readFileSync(abs, "utf8") : null;
        };
        const errors = checkPins(read);
        return { ok: errors.length === 0, lines: errors };
    },
    selfTest() {
        const with_ = (patch) => checkPins((f) => (f in patch ? patch[f] : (GOOD[f] ?? null)));
        const req = (cf, reuse, fonts = FONT_REQ) => ({
            "requirements-tools.txt": `clang-format==${cf}\n${reuse}\n${fonts}`,
        });
        const edit = (file, from, to) => ({ [file]: GOOD[file].replaceAll(from, to) });
        const cases = [
            ["齐全且一致", false, with_({})],
            ["版本号带 v 前缀", true, with_({ ".gitleaks-version": "v8.30.1\n" })],
            ["多出一行", true, with_({ ".node-version": "22.23.3\n22.0.0\n" })],
            ["缺钉版文件", true, with_({ ".actionlint-version": null })],
            ["clang-format 两处版本不一致", true, with_(req("18.1.7", "reuse==6.2.0"))],
            ["reuse 未钉版", true, with_(req("18.1.8", "reuse"))],
            [
                "带哈希的写法",
                false,
                with_(
                    req(
                        "18.1.8 \\\n    --hash=sha256:aa \\\n    --hash=sha256:bb",
                        "reuse==6.2.0 \\\n    --hash=sha256:cc",
                    ),
                ),
            ],
            ["带哈希时 clang-format 版本不一致", true, with_(req("18.1.7 \\\n    --hash=sha256:aa", "reuse==6.2.0"))],
            ["JUCE 提交号不是 40 位", true, with_({ ".juce-commit": "d6181bde\n" })],
            ["JUCE 提交号是大写", true, with_({ ".juce-commit": `${"D6181BDE".repeat(5)}\n` })],
            ["JUCE 版本不是 x.y.z", true, with_({ ".juce-version": "8.0\n" })],
            ["pluginval 版本不是 x.y.z", true, with_({ ".pluginval-version": "latest\n" })],
            ["sccache 版本带 v 前缀", true, with_({ ".sccache-version": "v0.17.0\n" })],
            ["sccache 的 sha256 是大写", true, with_({ ".sccache-sha256": `${"E9".repeat(32)}\n` })],
            ["sccache 的 sha256 少一位", true, with_({ ".sccache-sha256": `${"e".repeat(63)}\n` })],
            // 构建钉版是必需的：逐个删掉都要红
            ...[".juce-commit", ".juce-version", ".pluginval-version", ".sccache-version", ".sccache-sha256"].map(
                (f) => [`缺构建钉版 ${f}`, true, with_({ [f]: null })],
            ),
            // fontTools 与 brotli：在位、钉版本、带 sha256
            [
                "缺 fonttools",
                true,
                with_(req("18.1.8", "reuse==6.2.0", FONT_REQ.replace(/^fonttools[^\n]*\n[^\n]*\n/, ""))),
            ],
            [
                "brotli 没有哈希",
                true,
                with_(req("18.1.8", "reuse==6.2.0", FONT_REQ.replace(/brotli==1\.2\.0[\s\S]*$/, "brotli==1.2.0\n"))),
            ],
            [
                "fonttools 没钉版本",
                true,
                with_(req("18.1.8", "reuse==6.2.0", FONT_REQ.replace("fonttools==4.65.0", "fonttools"))),
            ],
            [
                "brotli 的哈希不是 64 位",
                true,
                with_(req("18.1.8", "reuse==6.2.0", FONT_REQ.replace(H("e"), "e".repeat(63)))),
            ],
            [
                "包名大小写与连字符照 pip 的规范比",
                false,
                with_(req("18.1.8", "reuse==6.2.0", FONT_REQ.replace("fonttools==", "FontTools=="))),
            ],
            // WebView2：CMake 是真源，两份声明逐字对拍
            ["CMake 里没有 WEBVIEW2_SHA256", true, with_(edit(WEBVIEW2.cmake, "WEBVIEW2_SHA256", "WEBVIEW2_HASH"))],
            ["CMake 里的 WebView2 版本不是四段", true, with_(edit(WEBVIEW2.cmake, '"1.0.2957.106"', '"1.0.2957"'))],
            [
                "NOTICES 有一处还写着旧版本",
                true,
                with_({
                    [WEBVIEW2.notices]: GOOD[WEBVIEW2.notices].replace(
                        "currently 1.0.2957.106",
                        "currently 1.0.2903.40",
                    ),
                }),
            ],
            ["NOTICES 没提 WEBVIEW2_VERSION", true, with_({ [WEBVIEW2.notices]: "WebView2 loader\n" })],
            [
                "notices/README 的包 sha256 与 CMake 不同",
                true,
                with_(edit(WEBVIEW2.readme, "4c".repeat(32), "4d".repeat(32))),
            ],
            [
                "notices/README 的包版本与 CMake 不同",
                true,
                with_(edit(WEBVIEW2.readme, "1.0.2957.106", "1.0.2957.107")),
            ],
            ["CMake 升了版本、两份声明都没跟", true, with_(edit(WEBVIEW2.cmake, "1.0.2957.106", "1.0.3000.1"))],
        ].map(([label, bad, errors]) => ({ label, bad, passed: bad ? errors.length > 0 : errors.length === 0 }));
        return caseResult(cases);
    },
};
