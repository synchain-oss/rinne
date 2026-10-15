// SPDX-License-Identifier: GPL-3.0-or-later
//
// 工具钉版文件：每个文件恰好一行版本号；同一工具在多处出现时版本必须一致
// （.clang-format 头注释、requirements-tools.txt 与 .clang-format-version 对拍）。
// 可选的钉版文件存在时才查格式。

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
};
// 构建用的钉版：构建骨架入库之前没有，缺失不算错
const OPTIONAL = {
    ".juce-commit": /^[0-9a-f]{40}$/,
    ".juce-version": SEMVER,
    ".pluginval-version": /^v?\d+\.\d+\.\d+$/,
    ".sccache-version": SEMVER,
    ".sccache-sha256": /^[0-9a-f]{64}$/,
};

/** read(rel) 返回文件文本或 null。 */
export function checkPins(read) {
    const errors = [];
    const value = {};
    for (const [file, re] of [...Object.entries(PINS), ...Object.entries(OPTIONAL)]) {
        const text = read(file);
        if (text === null) {
            if (file in PINS) errors.push(`缺 ${file}`);
            continue;
        }
        value[file] = text.trim();
        if (!/^[^\r\n]+\n?$/.test(text) || !re.test(value[file])) errors.push(`${file} 必须恰好一行：${re.source}`);
    }
    const cf = value[".clang-format-version"];
    if (!new RegExp(`^# clang-format ${cf?.replace(/\./g, "\\.")}\\b`, "m").test(read(".clang-format") || "")) {
        errors.push(".clang-format 头注释里的版本与 .clang-format-version 不一致");
    }
    // 带哈希的写法是「名==版本 \」加若干行「--hash=…」：去掉续行符与哈希，只看「名==版本」
    const req = (read("requirements-tools.txt") || "").split(/\r?\n/).map((l) =>
        l
            .replace(/\s*\\$/, "")
            .replace(/\s*--hash=\S+/g, "")
            .trim(),
    );
    if (!req.includes(`clang-format==${cf}`)) errors.push("requirements-tools.txt 的 clang-format 版本不一致");
    const reuse = /^reuse(?:\[[\w,-]+\])?==\d+\.\d+\.\d+$/;
    if (!req.some((l) => reuse.test(l))) errors.push("requirements-tools.txt 须钉住 reuse 的版本");
    return errors;
}

const GOOD = {
    ".gitleaks-version": "8.30.1\n",
    ".clang-format-version": "18.1.8\n",
    ".prettier-version": "3.9.6\n",
    ".actionlint-version": "1.7.12\n",
    ".node-version": "22.23.3\n",
    ".clang-format": "# SPDX\n# clang-format 18.1.8：说明\nBasedOnStyle: LLVM\n",
    "requirements-tools.txt": "clang-format==18.1.8\nreuse==6.2.0\n",
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
        const req = (cf, reuse) => ({ "requirements-tools.txt": `clang-format==${cf}\n${reuse}\n` });
        const cases = [
            ["齐全且一致", false, with_({})],
            ["可选的 JUCE 提交号合法", false, with_({ ".juce-commit": `${"d6181bde".repeat(5)}\n` })],
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
            [
                "构建钉版齐全且合法",
                false,
                with_({
                    ".juce-version": "8.0.8\n",
                    ".pluginval-version": "v1.0.4\n",
                    ".sccache-version": "0.17.0\n",
                    ".sccache-sha256": `${"e9".repeat(32)}\n`,
                }),
            ],
            ["JUCE 提交号是大写", true, with_({ ".juce-commit": `${"D6181BDE".repeat(5)}\n` })],
            ["JUCE 版本不是 x.y.z", true, with_({ ".juce-version": "8.0\n" })],
            ["pluginval 版本不是 x.y.z", true, with_({ ".pluginval-version": "latest\n" })],
            ["sccache 版本带 v 前缀", true, with_({ ".sccache-version": "v0.17.0\n" })],
            ["sccache 的 sha256 是大写", true, with_({ ".sccache-sha256": `${"E9".repeat(32)}\n` })],
            ["sccache 的 sha256 少一位", true, with_({ ".sccache-sha256": `${"e".repeat(63)}\n` })],
        ].map(([label, bad, errors]) => ({ label, bad, passed: bad ? errors.length > 0 : errors.length === 0 }));
        return caseResult(cases);
    },
};
