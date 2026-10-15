// SPDX-License-Identifier: GPL-3.0-or-later
//
// 文档与仓库结构（docs-truth 的一部分），三条判据：
//   1. docs/INSTALL.md 每个平台的中英两个标记块里，代码块逐个逐字相同（命令不能一边改了一边没改）；
//   2. 仓根的跟踪条目都在 scripts/lib/rules.mjs 的 TOP_LEVEL 白名单里（仓根的截图、草稿、临时导出一律判红）；
//   3. ADR、契约与变更文档的第一个二级标题是 `## Summary (EN)`。
// 判据在 structureProblems，只吃路径与文本。

import path from "node:path";
import { TOP_LEVEL } from "../lib/rules.mjs";
import { caseResult, readText, trackedFiles } from "../lib/check-kit.mjs";
import { INSTALL_FILE, findRegion } from "../lib/docs-kit.mjs";

const PLATFORMS = ["win", "mac"];
const SUMMARY_SCOPE = /^docs\/(?:adr|contracts|contract-changes)\/[^/]+\.md$/;

/** 围栏代码块的内容列表（按出现顺序，去掉首尾空白）。 */
function codeBlocks(text) {
    const blocks = [];
    let cur = null;
    for (const line of text.split("\n")) {
        if (/^\s*(```|~~~)/.test(line)) {
            if (cur === null) cur = [];
            else (blocks.push(cur.join("\n").trim()), (cur = null));
        } else if (cur) cur.push(line.trim());
    }
    return blocks;
}

const firstH2 = (text) => {
    let fence = false;
    for (const line of text.split("\n")) {
        if (/^\s*(```|~~~)/.test(line)) fence = !fence;
        else if (!fence && /^## /.test(line)) return line.trim();
    }
    return null;
};

/** paths：全部跟踪路径；files：{ 路径: 文本 }，含 INSTALL.md 与要查 Summary 的文档。 */
export function structureProblems({ paths, files }) {
    const out = [];
    const install = files[INSTALL_FILE];
    if (install === undefined) out.push(`缺 ${INSTALL_FILE}`);
    else {
        for (const p of PLATFORMS) {
            const [en, zh] = ["en", "zh"].map((lang) => {
                try {
                    return findRegion(install, `install:${p}:${lang}`);
                } catch (e) {
                    out.push(`${INSTALL_FILE}：${e.message}`);
                    return undefined;
                }
            });
            if (en === undefined || zh === undefined) continue;
            if (!en || !zh) out.push(`${INSTALL_FILE} 缺标记块 install:${p}:${en ? "zh" : "en"}`);
            else if (JSON.stringify(codeBlocks(en.body)) !== JSON.stringify(codeBlocks(zh.body))) {
                out.push(`${INSTALL_FILE} 的 ${p} 平台中英两块里的代码块不相同`);
            }
        }
    }
    for (const top of new Set(paths.map((f) => f.split("/")[0]))) {
        const isDir = paths.some((f) => f.startsWith(`${top}/`));
        if (!(isDir ? TOP_LEVEL.dirs : TOP_LEVEL.files).includes(top))
            out.push(`仓根的 ${top}${isDir ? "/" : ""} 不在白名单里（rules.mjs 的 TOP_LEVEL）`);
    }
    for (const [file, text] of Object.entries(files)) {
        if (SUMMARY_SCOPE.test(file) && firstH2(text) !== "## Summary (EN)")
            out.push(`${file} 的第一个二级标题应为 ## Summary (EN)`);
    }
    return out;
}

export function selfTest() {
    const block = (lang, cmd) =>
        `<!-- BEGIN install:${lang} -->\n\n1. 步骤\n\n   \`\`\`sh\n   ${cmd}\n   \`\`\`\n\n<!-- END install:${lang} -->\n`;
    const installOf = (winZh = "echo a", macZh = "echo b") =>
        [block("win:en", "echo a"), block("win:zh", winZh), block("mac:en", "echo b"), block("mac:zh", macZh)].join(
            "\n",
        );
    const base = {
        paths: ["README.md", "docs/INSTALL.md", "scripts/x.mjs", ".github/a.yml", "docs/adr/0001-a.md"],
        files: { [INSTALL_FILE]: installOf(), "docs/adr/0001-a.md": "# ADR\n\n## Summary (EN)\n\nx\n\n## 背景\n" },
    };
    const mod = (patch) => ({ paths: patch.paths ?? base.paths, files: { ...base.files, ...patch.files } });
    const cases = [
        ["全部合规", false, base],
        [
            "中英两块的说明文字不同但代码相同",
            false,
            mod({ files: { [INSTALL_FILE]: installOf().replace("1. 步骤", "1. 另一种说法") } }),
        ],
        ["Windows 中文块的命令不同", true, mod({ files: { [INSTALL_FILE]: installOf("echo changed") } })],
        ["macOS 中文块的命令不同", true, mod({ files: { [INSTALL_FILE]: installOf("echo a", "echo changed") } })],
        [
            "缺一个标记块",
            true,
            mod({ files: { [INSTALL_FILE]: installOf().replace("<!-- BEGIN install:mac:zh -->", "") } }),
        ],
        ["仓根多一个文件", true, mod({ paths: [...base.paths, "draft.png"] })],
        ["仓根多一个目录", true, mod({ paths: [...base.paths, "tmp/x.txt"] })],
        ["ADR 缺 Summary", true, mod({ files: { "docs/adr/0001-a.md": "# ADR\n\n## 背景\n" } })],
        [
            "Summary 不在第一个二级标题",
            true,
            mod({ files: { "docs/adr/0001-a.md": "# ADR\n\n## 背景\n\n## Summary (EN)\n" } }),
        ],
        ["变更文档缺 Summary", true, mod({ files: { "docs/contract-changes/20260101-x.md": "# x\n" } })],
        [
            "围栏里的二级标题不算",
            false,
            mod({ files: { "docs/adr/0001-a.md": "# ADR\n\n```md\n## 背景\n```\n\n## Summary (EN)\n" } }),
        ],
    ].map(([label, bad, input]) => ({
        label,
        bad,
        passed: bad ? structureProblems(input).length > 0 : structureProblems(input).length === 0,
    }));
    return caseResult(cases);
}

export default {
    name: "structure",
    group: "docs",
    inputs: [],
    run(ctx) {
        const paths = trackedFiles(ctx.root);
        const files = {};
        for (const p of paths.filter((f) => f === INSTALL_FILE || SUMMARY_SCOPE.test(f)))
            files[p] = readText(path.join(ctx.root, p)) ?? "";
        const problems = structureProblems({ paths, files });
        return { ok: problems.length === 0, lines: problems };
    },
    selfTest,
};
