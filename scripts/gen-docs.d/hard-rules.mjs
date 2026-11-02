// SPDX-License-Identifier: GPL-3.0-or-later
//
// 硬约束：真源是 docs/USER_GUIDE.zh-CN.md 的「使用前必读」编号列表（条数从真源读），en / fr 译文与每条的 zhSha256、
// installTxt 标记、规则版本 rulesVersion 在 docs/hard-rules.i18n.json。这个块产出四类落地面：
//   en / zh  文档里的规则列表（README、英文指南）；INSTALL.txt 由 install-txt 块用同一份数据渲染；
//   web      web/shared/hard-rules.gen.js 整个文件：RULES_VERSION 与三语的引导词、正文，首启判据与规则页读它；
//   guide    web/i18n/guide.js 里的生成段：三语的 rules.r<N>.lead / body 词条，规则页按词条上屏。
// 中文改了而译文没跟上时，validate 报「译文过期」。
//
// 规则版本：用户在规则页确认过的版本号存在本机全局位里，版本变了新实例才重弹规则页。版本只在规则含义变化时加 1，
// 但「含义变没变」机器判断不了，所以 rulesVersionZhSha256 记下定版本时中文的合并哈希：中文一改哈希就对不上，
// validate 判红，逼改的人当场决定是加版本，还是确认只是措辞并把哈希更新成当前值（--print-hashes 的 combined）。
//
// web 落地面是整个文件都由这里生成：标记之外只允许开头一行 BEGIN 与结尾一行 END，validate 逐字节核对这两段，
// 所以文件里任何一个字节被手改都会判红。JS 里放标记的写法：BEGIN 在行注释里；生成内容的最后一行开一个块注释，
// END 标记在下一行行首、随后合上这个块注释（引擎要求 END 在行首，而 ES 模块里不能出现裸的 <!--）。

import {
    I18N_FILE,
    SOURCE_FILE,
    findRegion,
    loadI18n,
    loadRuleSet,
    loadSource,
    readRepoText,
    ruleItem,
    sourceProblems,
    zhHashes,
} from "../lib/docs-kit.mjs";

export const WEB_FILE = "web/shared/hard-rules.gen.js";
export const GUIDE_FILE = "web/i18n/guide.js";
const LANGS = ["zh", "en", "fr"];

const beginLine = (id) => `<!-- BEGIN gen:hard-rules:${id} -->`;
const endLine = (id) => `<!-- END gen:hard-rules:${id} -->`;
/** web 落地面标记之外的两段：文件开头与结尾，一字不差。 */
export const WEB_HEAD = `// ${beginLine("web")}\n`;
export const WEB_TAIL = `${endLine("web")} */\n`;
/** 生成内容的收尾行：开一个块注释，给下一行的 END 标记栖身。 */
const CLOSER = "/* 生成内容到此为止，由 scripts/gen-docs.mjs 的 hard-rules 块写出，不要手改。";

const str = (s) => JSON.stringify(s);

/** 规则版本的问题：不是正整数，或中文改过而版本记录没跟上。 */
export function versionProblems(src, i18n) {
    const out = [];
    const v = i18n.rulesVersion;
    if (!Number.isInteger(v) || v < 1) out.push("rulesVersion 应为正整数（0 留给「从没确认过」）");
    if (i18n.rulesVersionZhSha256 !== zhHashes(src).combined)
        out.push(
            "规则中文改过了，rulesVersionZhSha256 还是旧的：含义变了就把 rulesVersion 加 1；只改措辞也要确认后" +
                "把 rulesVersionZhSha256 换成 --print-hashes 的 combined",
        );
    return out;
}

/** web 落地面标记之外的字节：文件存在就必须恰好是 WEB_HEAD + 区块 + WEB_TAIL。 */
export function webShapeProblems(text) {
    if (text === null) return [];
    if (!text.startsWith(WEB_HEAD)) return [`${WEB_FILE} 第一行必须是生成标记 ${beginLine("web")}`];
    if (!text.endsWith(`\n${WEB_TAIL}`))
        return [`${WEB_FILE} 必须以生成标记 ${endLine("web")} */ 结尾，后面不能再有内容`];
    return [];
}

/** 已经接入的 guide 落地面丢了标记：文件在、标记不在。 */
function guideShapeProblems(text) {
    if (text === null) return [];
    try {
        if (findRegion(text, "gen:hard-rules:guide")) return [];
    } catch (e) {
        return [`${GUIDE_FILE}：${e.message}`];
    }
    return [`${GUIDE_FILE} 没有生成段 gen:hard-rules:guide 的标记`];
}

function renderWeb(set, version) {
    const lines = [
        // 分开拼：完整的标识行写在源码里会被 reuse 当成本文件的许可声明
        "// SPDX-License-" + "Identifier: GPL-3.0-or-later",
        "//",
        "// 硬约束的界面数据（生成物）：真源是 docs/USER_GUIDE.zh-CN.md 的「使用前必读」与 docs/hard-rules.i18n.json，",
        "// 由 scripts/gen-docs.mjs 的 hard-rules 块整份写出；改真源后运行 node scripts/gen-docs.mjs --write。",
        "// 这个文件的任何一个字节被手改，gen-docs --check 都会判红。",
        "",
        "/** 规则页的版本：用户确认过的版本号存本机全局位，与它不等时新实例重弹规则页。 */",
        `export const RULES_VERSION = ${version};`,
        "",
        "/** 规则按编号排列：id 与三语的引导词、正文。 */",
        "export const RULES = [",
    ];
    for (const r of set.rules) {
        lines.push("    {", `        id: ${str(r.id)},`);
        for (const lang of LANGS)
            lines.push(
                `        ${lang}: {`,
                `            lead: ${str(r[lang].lead)},`,
                `            body: ${str(r[lang].body)},`,
                "        },",
            );
        lines.push("    },");
    }
    lines.push("];", CLOSER);
    return lines.join("\n");
}

// 两个落地面都只用 lead、body 这样的短键：prettier 不在短键后面折行，生成内容不必模仿它的折行规则也能过格式检查。
function renderGuide(set) {
    const lines = [
        "/** 规则页的引导词与正文（生成段）：按规则编号排列，文件下方摊成 rules.r<N>.lead / body 词条。 */",
        "const RULE_TEXT = {",
    ];
    for (const lang of LANGS) {
        lines.push(`    ${lang}: [`);
        for (const r of set.rules)
            lines.push(
                "        {",
                `            lead: ${str(r[lang].lead)},`,
                `            body: ${str(r[lang].body)},`,
                "        },",
            );
        lines.push("    ],");
    }
    lines.push("};", CLOSER);
    return lines.join("\n");
}

export default {
    name: "hard-rules",
    sources: [SOURCE_FILE, I18N_FILE],
    surfaces: [
        { file: "README.md", id: "en" },
        { file: "README.zh-CN.md", id: "zh" },
        { file: "docs/USER_GUIDE.md", id: "en" },
        { file: WEB_FILE, id: "web" },
        { file: GUIDE_FILE, id: "guide" },
    ],
    validate: (ctx) => {
        const src = loadSource(ctx.root);
        const i18n = loadI18n(ctx.root);
        return [
            ...sourceProblems(src, i18n),
            ...versionProblems(src, i18n),
            ...webShapeProblems(readRepoText(ctx.root, WEB_FILE)),
            ...guideShapeProblems(readRepoText(ctx.root, GUIDE_FILE)),
        ];
    },
    render: (ctx, id) => {
        const set = loadRuleSet(ctx.root);
        if (id === "web") return renderWeb(set, loadI18n(ctx.root).rulesVersion);
        if (id === "guide") return renderGuide(set);
        return set.rules.map((r, i) => ruleItem(r, id, i + 1)).join("\n");
    },
};
