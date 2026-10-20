// SPDX-License-Identifier: GPL-3.0-or-later
//
// 生成块与真源一致（docs-truth 的一部分）。判据在 scripts/gen-docs.mjs：已接入的落地面逐字节比对生成结果；
// 中文真源改了而译文的 zhSha256 没跟上，报「译文过期」；INSTALL.txt 用假变量试渲染两个平台，缺警告或缺必选规则即红。
// 自测在临时目录里放一份真源的副本和两个带标记的落地面，逐项改坏。

import fs from "node:fs";
import path from "node:path";
import { checkAll, writeAll } from "../gen-docs.mjs";
import { ROOT, caseResult } from "../lib/check-kit.mjs";
import { makeTempDir, removeTempDir } from "../lib/safe-rm.mjs";
import { I18N_FILE, INSTALL_FILE, SOURCE_FILE, loadSource, zhHashes } from "../lib/docs-kit.mjs";

const SURFACES = {
    "README.md": ["safety-warning:en", "hard-rules:en"],
    "docs/USER_GUIDE.md": ["safety-warning:en", "hard-rules:en"],
};
const marked = (blocks) => blocks.map((b) => `<!-- BEGIN gen:${b} -->\n<!-- END gen:${b} -->\n`).join("\n");

/** 临时目录里搭一份已回写的夹具，让 mutate 改坏，再返回检查结果里的问题。 */
async function scenario(root, mutate) {
    const dir = makeTempDir("rinne-gendocs-");
    try {
        const put = (rel, text) => {
            fs.mkdirSync(path.dirname(path.join(dir, rel)), { recursive: true });
            fs.writeFileSync(path.join(dir, rel), text);
        };
        for (const rel of [SOURCE_FILE, I18N_FILE, INSTALL_FILE])
            put(rel, fs.readFileSync(path.join(root, rel), "utf8"));
        for (const [rel, blocks] of Object.entries(SURFACES)) put(rel, `# t\n\n${marked(blocks)}`);
        put("README.zh-CN.md", "# 没有标记的文件\n");
        await writeAll(dir);
        const edit = (rel, fn) => put(rel, fn(fs.readFileSync(path.join(dir, rel), "utf8")));
        mutate({ edit, put, dir });
        return await checkAll(dir);
    } finally {
        removeTempDir(dir);
    }
}

const reviewed = (sha) => (t) => {
    const j = JSON.parse(t);
    j.frReview = { status: "reviewed", reviewedBy: "x", reviewedZhSha256: sha, approvalUrl: "https://example.com/x" };
    return JSON.stringify(j, null, 2);
};

const swap = (from, to) => (t) => {
    if (!t.includes(from)) throw new Error(`自测夹具里找不到 ${from}`);
    return t.replace(from, to);
};

export async function selfTest(root = ROOT) {
    const cases = [
        ["回写后的夹具（含未接入、不存在的落地面）全绿", false, () => {}, (p) => p.length === 0],
        [
            "落地面的生成块里改一个字节",
            true,
            (c) => c.edit("README.md", swap("Placement", "Placemen")),
            (p) => p.some((x) => x.includes("README.md")),
        ],
        [
            "落地面的警告块被手改",
            true,
            (c) => c.edit("docs/USER_GUIDE.md", swap("> [!WARNING]", "> [!NOTE]")),
            (p) => p.length > 0,
        ],
        [
            "只改中文规则、不改译文",
            true,
            (c) => c.edit(SOURCE_FILE, swap("从 0 开始", "从零开始")),
            (p) => p.some((x) => x.includes("译文过期")),
        ],
        [
            "只改中文导语、不改译文",
            true,
            (c) => c.edit(SOURCE_FILE, swap("反馈可能", "反馈有可能")),
            (p) => p.some((x) => x.includes("译文过期")),
        ],
        [
            "标记放在没有声明的文件里",
            true,
            (c) => c.put("docs/NOTES.md", "<!-- BEGIN gen:hard-rules:en -->\n<!-- END gen:hard-rules:en -->\n"),
            (p) => p.some((x) => x.includes("docs/NOTES.md")),
        ],
        [
            "frReview 标 reviewed 且哈希对得上",
            false,
            (c) => c.edit(I18N_FILE, reviewed(zhHashes(loadSource(c.dir)).combined)),
            (p) => p.length === 0,
        ],
        [
            "frReview 标 reviewed 但中文已变",
            true,
            (c) => c.edit(I18N_FILE, reviewed("0".repeat(64))),
            (p) => p.some((x) => x.includes("审校记录已过期")),
        ],
        [
            "frReview 标 reviewed 但审校人为空",
            true,
            (c) =>
                c.edit(I18N_FILE, (t) =>
                    reviewed(zhHashes(loadSource(c.dir)).combined)(t).replace('"reviewedBy": "x"', '"reviewedBy": ""'),
                ),
            (p) => p.some((x) => x.includes("reviewedBy")),
        ],
        [
            "frReview 标 reviewed 但确认链接不是 https",
            true,
            (c) =>
                c.edit(I18N_FILE, (t) =>
                    reviewed(zhHashes(loadSource(c.dir)).combined)(t).replace(
                        "https://example.com/x",
                        "http://example.com/x",
                    ),
                ),
            (p) => p.some((x) => x.includes("approvalUrl")),
        ],
        [
            "标记拼错",
            true,
            (c) => c.edit("README.md", swap("gen:hard-rules:en", "gen:hard-rule:en")),
            (p) => p.length > 0,
        ],
        [
            "缺结束标记",
            true,
            (c) => c.edit("README.md", swap("<!-- END gen:hard-rules:en -->", "")),
            (p) => p.length > 0,
        ],
        [
            "Panic 规则没有标 installTxt",
            true,
            (c) =>
                c.edit(
                    I18N_FILE,
                    swap('"id": "panic",\n      "installTxt": true', '"id": "panic",\n      "installTxt": false'),
                ),
            (p) => p.some((x) => x.includes("installTxt")),
        ],
        ["译文少一条规则", true, (c) => c.edit(SOURCE_FILE, (t) => t.replace(/\n4\. .*\n/, "\n")), (p) => p.length > 0],
        [
            "INSTALL.md 少一个平台的标记块",
            true,
            (c) => c.edit(INSTALL_FILE, swap("<!-- BEGIN install:mac:zh -->", "")),
            (p) => p.some((x) => x.includes("INSTALL.txt")),
        ],
        [
            "INSTALL.md 里有渲染不了的占位符",
            true,
            (c) => c.edit(INSTALL_FILE, swap("Rescan plug-ins in your DAW.", "{{NOPE}}")),
            (p) => p.some((x) => x.includes("INSTALL.txt")),
        ],
    ];
    const done = [];
    for (const [label, bad, mutate, expect] of cases) {
        let passed;
        try {
            passed = expect(await scenario(root, mutate));
        } catch {
            passed = false;
        }
        done.push({ label, bad, passed });
    }
    return caseResult(done);
}

export default {
    name: "gen-docs",
    group: "docs",
    inputs: [],
    async run(ctx) {
        const problems = await checkAll(ctx.root);
        return { ok: problems.length === 0, lines: problems };
    },
    selfTest: (ctx) => selfTest(ctx.root),
};
