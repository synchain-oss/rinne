// SPDX-License-Identifier: GPL-3.0-or-later
//
// 文档生成器与文档检查共用的小工具：标记区块的定位与替换、中文真源节的解析、译文哈希、占位符替换。
// 全部是纯函数或只读文件的函数，不依赖 git。

import crypto from "node:crypto";
import fs from "node:fs";
import path from "node:path";

export const SOURCE_FILE = "docs/USER_GUIDE.zh-CN.md";
export const SOURCE_HEADING = "使用前必读";
export const I18N_FILE = "docs/hard-rules.i18n.json";
export const INSTALL_FILE = "docs/INSTALL.md";

export const sha256 = (text) => crypto.createHash("sha256").update(text, "utf8").digest("hex");

/** 读仓内文件，统一成 LF。缺文件时返回 null。 */
export function readRepoText(root, rel) {
    try {
        return fs.readFileSync(path.join(root, rel), "utf8").replace(/\r\n/g, "\n");
    } catch {
        return null;
    }
}

const beginTag = (name) => `<!-- BEGIN ${name} -->`;
const endTag = (name) => `<!-- END ${name} -->`;

/**
 * 找标记区块 <!-- BEGIN name --> … <!-- END name -->。
 * 返回 null（没有标记）或 { bodyStart, bodyEnd, body }；标记不成对或重复时抛错。
 * 正文是 BEGIN 行行尾换行之后、END 行行首之前的部分。
 */
export function findRegion(text, name) {
    const b = beginTag(name);
    const e = endTag(name);
    const bi = text.indexOf(b);
    const ei = text.indexOf(e);
    if (bi < 0 && ei < 0) return null;
    if (bi < 0 || ei < 0) throw new Error(`标记 ${name} 只有${bi < 0 ? "结束" : "开始"}一端`);
    if (ei < bi) throw new Error(`标记 ${name} 的结束出现在开始之前`);
    if (text.indexOf(b, bi + 1) >= 0 || text.indexOf(e, ei + 1) >= 0) throw new Error(`标记 ${name} 出现了不止一次`);
    const bodyStart = text.indexOf("\n", bi) + 1;
    if (bodyStart === 0 || bodyStart > ei) throw new Error(`标记 ${name} 的开始与结束要各占一行`);
    return { bodyStart, bodyEnd: ei, body: text.slice(bodyStart, ei) };
}

/** 文本里所有 <!-- BEGIN gen:… --> 的名字。 */
export const generatedMarkerNames = (text) => [...text.matchAll(/<!-- BEGIN (gen:[^\s>]+) -->/g)].map((m) => m[1]);

/** 把区块正文换成 body（末尾补一个换行）。区块必须存在。 */
export function replaceRegion(text, name, body) {
    const r = findRegion(text, name);
    if (!r) throw new Error(`没有标记 ${name}`);
    return text.slice(0, r.bodyStart) + body.replace(/\n*$/, "\n") + text.slice(r.bodyEnd);
}

/** 二级标题 `## heading` 下的内容，到下一个二级标题或文末；围栏代码块里的 `## ` 不算标题。 */
export function sectionOf(markdown, heading) {
    const lines = markdown.split("\n");
    let fence = false;
    let from = -1;
    let to = lines.length;
    for (let i = 0; i < lines.length; i++) {
        if (/^\s*(```|~~~)/.test(lines[i])) fence = !fence;
        if (fence || !/^## /.test(lines[i])) continue;
        if (from < 0) {
            if (lines[i].slice(3).trim() === heading) from = i + 1;
        } else {
            to = i;
            break;
        }
    }
    return from < 0 ? null : lines.slice(from, to).join("\n");
}

const RULE_LINE = /^(\d+)\. \*\*(.+?)\*\*：(.+)$/;

/**
 * 解析中文真源节：导语一段，后接 `N. **引导词**：正文` 的编号列表（N 从 1 起连续）。
 * 返回 { lead, rules: [{ lead, body }] }，不合格式时抛错。条数从真源读，不写死。
 */
export function parseSource(markdown) {
    const section = sectionOf(markdown, SOURCE_HEADING);
    if (section === null) throw new Error(`${SOURCE_FILE} 没有 "## ${SOURCE_HEADING}"`);
    const leadLines = [];
    const rules = [];
    for (const raw of section.split("\n")) {
        const line = raw.trimEnd();
        if (!line) continue;
        const m = RULE_LINE.exec(line);
        if (m) {
            if (Number(m[1]) !== rules.length + 1) throw new Error(`规则编号应为 ${rules.length + 1}，实际是 ${m[1]}`);
            rules.push({ lead: m[2], body: m[3] });
        } else if (rules.length) {
            throw new Error(`规则列表之后出现了不是 "N. **引导词**：正文" 的行：${line.slice(0, 30)}`);
        } else {
            leadLines.push(line);
        }
    }
    if (!leadLines.length) throw new Error("缺导语段");
    if (!rules.length) throw new Error("没有规则");
    return { lead: leadLines.join("\n"), rules };
}

/** 各段中文的哈希。规则的哈希覆盖引导词与正文。combined 是全部中文文本的合并哈希（审校记录用）。 */
export function zhHashes(src) {
    const lead = sha256(src.lead);
    const rules = src.rules.map((r) => sha256(`${r.lead}\n${r.body}`));
    return { lead, rules, combined: sha256([lead, ...rules].join("\n")) };
}

export function loadSource(root) {
    const text = readRepoText(root, SOURCE_FILE);
    if (text === null) throw new Error(`缺 ${SOURCE_FILE}`);
    return parseSource(text);
}

export function loadI18n(root) {
    const text = readRepoText(root, I18N_FILE);
    if (text === null) throw new Error(`缺 ${I18N_FILE}`);
    return JSON.parse(text);
}

export const REQUIRED_INSTALL_TXT_IDS = ["placement", "zero", "panic"];

/** 真源与译文文件的一致性问题清单：条数、译文缺项、译文过期、installTxt 必选项、frReview 形状。 */
export function sourceProblems(src, i18n) {
    const out = [];
    const h = zhHashes(src);
    if (!i18n.rules || i18n.rules.length !== src.rules.length) {
        out.push(`译文有 ${i18n.rules?.length ?? 0} 条规则，中文真源有 ${src.rules.length} 条`);
        return out;
    }
    const need = (obj, where) => {
        for (const lang of ["en", "fr"]) if (!obj?.[lang]) out.push(`${where} 缺 ${lang} 译文`);
    };
    need(i18n.lead, "导语");
    if (i18n.lead?.zhSha256 !== h.lead) out.push("译文过期：导语的中文已改动，en / fr 译文与 zhSha256 还没跟上");
    src.rules.forEach((r, i) => {
        const t = i18n.rules[i];
        for (const lang of ["en", "fr"]) {
            if (!t[lang]?.lead || !t[lang]?.body) out.push(`规则 ${i + 1} 缺 ${lang} 译文`);
        }
        if (t.zhSha256 !== h.rules[i]) {
            out.push(`译文过期：规则 ${i + 1}「${r.lead}」的中文已改动，en / fr 译文与 zhSha256 还没跟上`);
        }
    });
    const flagged = new Map(i18n.rules.map((t) => [t.id, t.installTxt === true]));
    for (const id of REQUIRED_INSTALL_TXT_IDS) {
        if (!flagged.has(id)) out.push(`译文里没有 id 为 ${id} 的规则`);
        else if (!flagged.get(id)) out.push(`规则 ${id} 必须标 installTxt: true`);
    }
    const fr = i18n.frReview;
    if (!fr || !["pending", "reviewed"].includes(fr.status)) out.push("frReview.status 应为 pending 或 reviewed");
    else if (fr.status === "reviewed" && fr.reviewedZhSha256 !== h.combined) {
        out.push("frReview 已标 reviewed，但 reviewedZhSha256 不等于当前中文的合并哈希：审校记录已过期");
    }
    return out;
}

/** 把 {{NAME}} 换成 vars[NAME]；vars 里没有的名字抛错。 */
export function fill(text, vars) {
    return text.replace(/\{\{([A-Z0-9_]+)\}\}/g, (_, k) => {
        if (!(k in vars)) throw new Error(`缺变量 ${k}`);
        return String(vars[k]);
    });
}

/** 引导词与正文之间的分隔：中文全角冒号，英文冒号加空格，法文在冒号前加不间断空格。 */
export const SEP = { zh: "：", en: ": ", fr: " : " };

/**
 * 合并中文真源与译文，返回 { lead: {zh,en,fr}, rules: [{ id, installTxt, zh, en, fr }] }，
 * 其中每条规则的 zh / en / fr 都是 { lead, body }。真源或译文有问题时抛错。
 */
export function loadRuleSet(root) {
    const src = loadSource(root);
    const i18n = loadI18n(root);
    const problems = sourceProblems(src, i18n);
    if (problems.length) throw new Error(problems.join("；"));
    return {
        lead: { zh: src.lead, en: i18n.lead.en, fr: i18n.lead.fr },
        rules: src.rules.map((zh, i) => ({
            id: i18n.rules[i].id,
            installTxt: i18n.rules[i].installTxt === true,
            zh,
            en: i18n.rules[i].en,
            fr: i18n.rules[i].fr,
        })),
    };
}

/** 一条规则的 Markdown 列表项。 */
export const ruleItem = (rule, lang, n) => `${n}. **${rule[lang].lead}**${SEP[lang]}${rule[lang].body}`;
