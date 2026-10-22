// SPDX-License-Identifier: GPL-3.0-or-later
//
// 用语纪律（docs-truth 的一部分）：扫仓内的用户面文本，把禁用的宣传式强断言拦下来。
// 禁用词表放在本文件里，不放在 scripts/lib/rules.mjs（那是基础设施路径，改它要维护者批准）。
//
// 家族（每族一条判据，报错行以 [家族名] 开头）：
//   absolute-support  「支持所有 DAW」「works with every host」一类
//   all-daw-zh        中文形态的「支持所有 DAW / 兼容所有宿主」
//   zero-latency      英文形态的 zero latency
//   zero-latency-zh   中文形态的零延迟
//   safety-promise    「永不啸叫」「100% safe」「protects your hearing」一类
//   first-only        「唯一」「首个」「the first open-source feedback plugin」一类
//   signed-claims     「已签名」「已公证」「is signed」一类
//   unverified-support「官方认证」「works with Logic」一类
//   naming            产品名：Rinne Send / Rinne Return / 回环输入 / 回环输出 / Loopback
//   doctrine          「业力」「转世」「涅槃」「karma」一类
//   competitor        「better than」「比……更好」
//   coming-soon       「coming soon」「即将推出」「敬请期待」
//   fader-after       「推子后」，以及英文里 Rinne Output 与 after the fader / post-fader 同句
//   whole-channel     「整条通道」「the whole channel」
//   auto-pairing      「自动配对」「auto-pairing」「automatically pair」
//
// 三条放行规则，任一命中即放过：短语在引号里；同一句里有否定或免责标记；文件在排除清单里。
//
// 判据名见 RULES。自测的每个坏样例写明它必须触发哪一条，并核对每一条都至少被一个坏样例点名：删掉任何一条判据，
// 点名它的坏样例就变红。

import fs from "node:fs";
import path from "node:path";
import { ROOT, caseResult, readText } from "../lib/check-kit.mjs";

/** 判据名 → 说明。 */
export const RULES = {
    "absolute-support": "不写「支持所有 DAW」「兼容所有宿主」这类断言",
    "all-daw-zh": "不写中文形态的「支持所有 DAW」",
    "zero-latency": "不写英文形态的 zero latency",
    "zero-latency-zh": "不写中文形态的零延迟",
    "safety-promise": "不写永不啸叫、绝对安全、100% safe 这类承诺",
    "first-only": "不写唯一、首个、the first open-source feedback plugin 这类说法",
    "signed-claims": "不写已签名、已公证、is notarized 这类说法",
    "unverified-support": "不写官方认证、works with Logic 这类未验证的支持声明",
    naming: "不用 Rinne Send、Rinne Return、回环输入、回环输出、Loopback 当产品名",
    doctrine: "不写业力、转世、涅槃、karma 这类说法",
    competitor: "不写比别家更好、replaces Connector 这类比较",
    "coming-soon": "不写 coming soon、即将推出、敬请期待",
    "fader-after": "不把 Rinne Output 说成在推子后",
    "whole-channel": "不写整条通道、the whole channel",
    "auto-pairing": "不写自动配对、auto-pairing、automatically pair",
};

/** 扫描范围：用户面文本。 */
export const SCOPE = [
    "README.md",
    "README.zh-CN.md",
    "docs/USER_GUIDE.md",
    "docs/USER_GUIDE.zh-CN.md",
    "docs/INSTALL.md",
    "docs/KNOWN_ISSUES.md",
    "docs/DAW_COMPATIBILITY.md",
    "docs/hard-rules.i18n.json",
    "docs/media/placement.txt",
];

/** 排除清单：这些文件里有合法的命中，不扫。 */
export const EXCLUDED = [
    "docs/ARCHITECTURE.md",
    "docs/UI_BRIDGE.md",
    "docs/contracts/",
    "docs/adr/",
    "docs/validation/",
    "CLAUDE.md",
    "CONTRIBUTING.md",
    "scripts/",
];

/** 文件在排除清单里（目录按前缀匹配）。 */
export function isExcluded(file) {
    return EXCLUDED.some((e) => (e.endsWith("/") ? file.startsWith(e) : file === e));
}

/** 否定与免责标记：同句里有它们的强断言不算命中。 */
const NEGATION = [
    /\bnot\b/i,
    /\bno\b/i,
    /\bnever\b/i,
    /\bcannot\b/i,
    /\bcan't\b/i,
    /\bwithout\b/i,
    /\bdo not\b/i,
    /\bdon't\b/i,
    /\bunsupported\b/i,
    /\bunsigned\b/i,
    /\bin the backlog\b/i,
    /\bpending\b/i,
    /不/,
    /不得/,
    /不要/,
    /禁止/,
    /勿/,
    /不会/,
    /没有/,
    /无/,
    /无法/,
    /尚未/,
    /未/,
];

/** 引号：短语在引号里就放过。 */
const QUOTE_RE = /「[^」]*」|“[^”]*”|‘[^’]*’|"[^"\n]*"|''[^'\n]*''/g;

/** 句子切分：句号、问号、感叹号、分号、换行，以及英文的句点加空白。 */
export function sentences(text) {
    return text
        .split(/(?<=[。！？!?;；\n])|(?<=\.)[ \t]+/)
        .map((s) => s.trim())
        .filter(Boolean);
}

/** 一句话里有没有否定或免责标记。 */
export function hasNegation(sentence) {
    return NEGATION.some((re) => re.test(sentence));
}

/** 一句话里引号圈起来的区间。 */
export function quotedSpans(sentence) {
    const out = [];
    for (const m of sentence.matchAll(QUOTE_RE)) out.push([m.index, m.index + m[0].length]);
    return out;
}

/**
 * 禁用词家族。patterns 每项是 { re, negation }：negation 为假时，同句的否定标记不放过它
 * （safety-promise 的英文 never howls 靠正则里的否定后顾，豁免写在自己的形态里）。
 */
export const FAMILIES = [
    {
        id: "absolute-support",
        note: "支持 / 兼容所有 DAW 与宿主（英文形态）",
        patterns: [
            {
                re: /\b(?:supports?|works (?:in|with)|compatible with)\s+(?:all|any|every)\s+(?:DAWs?|hosts?)\b/i,
            },
        ],
    },
    {
        id: "all-daw-zh",
        note: "支持所有 DAW 与宿主（中文形态）",
        patterns: [{ re: /支持所有\s*(?:DAW|宿主)/ }, { re: /兼容所有(?:宿主|DAW)/ }],
    },
    {
        id: "zero-latency",
        note: "零延迟（英文形态）",
        patterns: [{ re: /\bzero[- ]latency\b/i }],
    },
    {
        id: "zero-latency-zh",
        note: "零延迟（中文形态）",
        patterns: [{ re: /零延迟/ }],
    },
    {
        id: "safety-promise",
        note: "永不啸叫、绝对安全、100% safe",
        patterns: [
            {
                // 英文的免责形态写在正则的否定后顾里：「no software can guarantee a loop will never howl」不命中
                re: /(?<!(?:guarantee|cannot|can't|no software)[^.\n]{0,40})\bnever howls?\b/i,
                negation: false,
            },
            { re: /\b100\s?% safe\b/i },
            { re: /\bprotects your (?:hearing|ears|speakers)\b/i },
            // 中文形态带否定或免责前缀时放过：「无法保证本插件不会爆音」
            { re: /绝对安全/ },
            { re: /永不啸叫/ },
            { re: /不会爆音/ },
        ],
    },
    {
        id: "first-only",
        note: "唯一、首个、第一款、the first open-source feedback plugin",
        patterns: [
            {
                re: /\b(?:the )?(?:first|only)\b[^.\n]{0,40}\b(?:open[- ]source|free|feedback (?:plugin|tool)|solution)s?\b/i,
            },
            { re: /(?:唯一|首个|首款|第一款)[^。\n]{0,10}(?:开源|免费|插件|工具|方案)/ },
        ],
    },
    {
        id: "signed-claims",
        note: "已签名、已公证、is notarized",
        patterns: [
            { re: /\b(?:is|are|now|fully|officially)\s+(?:code[- ])?(?:signed|notarized)\b/i },
            { re: /\bsigned and notarized\b/i },
            { re: /已签名/ },
            { re: /已公证/ },
        ],
    },
    {
        id: "unverified-support",
        note: "官方认证、works with Logic、supports Intel",
        patterns: [
            { re: /\bSteinberg[- ](?:approved|certified)\b/i },
            { re: /官方认证/ },
            { re: /\b(?:works (?:with|in)|supports?|compatible with)\s+(?:Logic|Intel|GarageBand)\b/i },
            { re: /\bIntel Macs? (?:are |is )?supported\b/i },
            { re: /支持\s*(?:Intel|Logic|GarageBand)/ },
        ],
    },
    {
        id: "naming",
        note: "Rinne Send、Rinne Return、回环输入、回环输出、Loopback",
        patterns: [{ re: /\bRinne (?:Send|Return)\b/ }, { re: /回环输入/ }, { re: /回环输出/ }, { re: /\bLoopback\b/ }],
    },
    {
        id: "doctrine",
        note: "业力、转世、涅槃、karma",
        patterns: [
            { re: /\b(?:karma|reincarnation|nirvana|enlightenment)\b/i },
            { re: /业力/ },
            { re: /转世/ },
            { re: /涅槃/ },
            { re: /解脱/ },
        ],
    },
    {
        id: "competitor",
        note: "比别家更好、replaces Connector",
        patterns: [{ re: /\bbetter than\b/i }, { re: /\breplaces? Connector\b/i }, { re: /比[^。\n]{0,12}更好/ }],
    },
    {
        id: "coming-soon",
        note: "coming soon、即将推出、敬请期待",
        patterns: [{ re: /\bcoming soon\b/i }, { re: /即将推出/ }, { re: /即将支持/ }, { re: /敬请期待/ }],
    },
    {
        id: "fader-after",
        note: "推子后；英文里 Rinne Output 与 after the fader / post-fader 同句",
        patterns: [{ re: /推子后/ }],
        // 英文形态：同一句里既说 Rinne Output，又说 after the fader / post-fader
        custom: (sentence) => {
            if (!/\bRinne Output\b/i.test(sentence)) return null;
            const m = /\b(?:after the fader|post-fader)\b/i.exec(sentence);
            if (!m) return null;
            const out = /\bRinne Output\b/i.exec(sentence);
            return { index: out.index, text: out[0] };
        },
    },
    {
        id: "whole-channel",
        note: "整条通道、the whole channel",
        patterns: [{ re: /整条通道/ }, { re: /\bthe whole channel\b/i }],
    },
    {
        id: "auto-pairing",
        note: "自动配对、auto-pairing、automatically pair",
        patterns: [
            { re: /自动配对/ },
            { re: /\bauto-pairing\b/i },
            { re: /\bautomatically pairs?\b/i },
            { re: /\bpairs automatically\b/i },
        ],
    },
];

/**
 * 判据的实现。files 是 { 路径: 文本 }。返回问题数组（每行以 [家族名] 开头）。
 */
export function wordingProblems({ files }) {
    const out = [];
    for (const [file, text] of Object.entries(files)) {
        if (isExcluded(file)) continue;
        for (const family of FAMILIES) {
            if (!Object.hasOwn(RULES, family.id)) throw new Error(`未登记的判据 ${family.id}`);
            const report = (index, hit) => out.push(`[${family.id}] ${file}：${family.note}（命中「${hit}」）`);
            for (const sentence of sentences(text ?? "")) {
                const spans = quotedSpans(sentence);
                const inQuote = (i) => spans.some(([a, b]) => i >= a && i < b);
                // 免责标记要在这句里、且不在命中片段自己身上（命中片段自带否定词的不算免责）
                const negatedOutside = (index, len) => {
                    const rest = sentence.slice(0, index) + " " + sentence.slice(index + len);
                    return hasNegation(rest);
                };
                for (const p of family.patterns ?? []) {
                    const negation = p.negation !== false;
                    for (const m of sentence.matchAll(global(p.re))) {
                        if (inQuote(m.index)) continue;
                        if (negation && negatedOutside(m.index, m[0].length)) continue;
                        report(m.index, m[0]);
                    }
                }
                if (family.custom) {
                    const hit = family.custom(sentence);
                    if (!hit) continue;
                    if (inQuote(hit.index)) continue;
                    if (negatedOutside(hit.index, hit.text.length)) continue;
                    report(hit.index, hit.text);
                }
            }
        }
    }
    return out;
}

/** matchAll 要求全局正则；表里的正则写成非全局的，这里补上 g。 */
const global = (re) => (re.global ? re : new RegExp(re.source, re.flags.includes("g") ? re.flags : `${re.flags}g`));

/** 扫描范围内的用户面文本；web/i18n/ 下的词条文件全部扫。 */
export function scopedFiles(root) {
    const files = {};
    for (const rel of SCOPE) {
        const t = readText(path.join(root, rel));
        if (t !== null) files[rel] = t;
    }
    const i18n = path.join(root, "web", "i18n");
    try {
        for (const f of fs
            .readdirSync(i18n)
            .filter((x) => x.endsWith(".js"))
            .sort())
            files[`web/i18n/${f}`] = readText(path.join(i18n, f)) ?? "";
    } catch {
        // 没有 web/i18n 目录时只扫固定清单
    }
    return files;
}

export async function selfTest() {
    const files = {
        "README.md": "Rinne is two plugins.\n",
    };
    const one = (text) => wordingProblems({ files: { "README.md": text } });
    const bad = (text, rule) => {
        const lines = one(text);
        return lines.some((l) => l.startsWith(`[${rule}] `));
    };
    /** 每个禁用家族一个坏样例，外加几条必须放行的正例。 */
    const samples = [
        ["支持所有 DAW", "all-daw-zh"],
        ["It supports all DAWs and hosts.", "absolute-support"],
        ["The loop is zero latency.", "zero-latency"],
        ["这条链是零延迟的。", "zero-latency-zh"],
        ["Rinne never howls.", "safety-promise"],
        ["Rinne is 100% safe.", "safety-promise"],
        ["本插件不会爆音。", "safety-promise"],
        ["本插件永不啸叫。", "safety-promise"],
        ["本插件绝对安全。", "safety-promise"],
        ["Rinne is the first open-source feedback plugin.", "first-only"],
        ["它是唯一一款开源插件。", "first-only"],
        ["The build is now notarized.", "signed-claims"],
        ["这些构建已公证。", "signed-claims"],
        ["Steinberg-certified plug-ins.", "unverified-support"],
        ["It works with Logic Pro.", "unverified-support"],
        ["Rinne Send is the sending end.", "naming"],
        ["这个插件叫回环输入。", "naming"],
        ["The product used to be called Loopback.", "naming"],
        ["It follows the doctrine of karma.", "doctrine"],
        ["轮回即转世。", "doctrine"],
        ["Rinne is better than the other one.", "competitor"],
        ["Rinne is even better than nothing.", "competitor"],
        ["Rinne is coming soon.", "coming-soon"],
        ["功能即将推出。", "coming-soon"],
        ["Put Rinne Output after the fader divider.", "fader-after"],
        ["Rinne Output 放在推子后。", "fader-after"],
        ["It processes the whole channel.", "whole-channel"],
        ["它处理整条通道。", "whole-channel"],
        ["The two plug-ins auto-pairing happens by itself.", "auto-pairing"],
        ["两端自动配对。", "auto-pairing"],
    ];
    // 必须放行的写法
    const allowed = [
        "Rinne reports no plugin latency to the host.",
        "向宿主报告 0 延迟。",
        "Put Rinne Input in the first insert slot.",
        "放在第一格。",
        "The builds are not signed with an Apple Developer ID and not notarized.",
        "No software can guarantee a loop will never howl.",
        "无法保证环路永远不啸叫。",
        "我们不能保证本插件不会爆音。",
        "Put Rinne Output above the fader divider.",
        "Intel Macs are not supported.",
        "It is in the backlog, no date.",
        "「支持所有 DAW」这种写法不写。",
        "Rinne Input（返回端）与 Rinne Output（发送端）。",
        "輪廻（循环）这个名字的意思是循环。",
    ];

    const cases = [];
    for (const [text, rule] of samples) {
        cases.push({
            label: `坏样例：${text}`,
            bad: true,
            passed: bad(text, rule),
            expect: [rule],
        });
    }
    for (const text of allowed) {
        cases.push({
            label: `放行：${text}`,
            bad: false,
            passed: one(text).length === 0,
            expect: [],
        });
    }
    // 排除清单里的文件不判红
    cases.push({
        label: "排除清单里的文件不判红",
        bad: false,
        passed:
            wordingProblems({
                files: { "docs/ARCHITECTURE.md": "干声随整条通道进环。\n", "scripts/probe/x.md": "探针描述推子后。\n" },
            }).length === 0,
        expect: [],
    });
    const unnamed = Object.keys(RULES).filter((r) => !cases.some((c) => c.expect.includes(r)));
    cases.push({
        label: `每条判据都有坏样例点名${unnamed.length ? `（没有：${unnamed.join("、")}）` : ""}`,
        bad: false,
        passed: unnamed.length === 0,
    });
    return caseResult(cases);
}

export default {
    name: "wording",
    group: "docs",
    inputs: [],
    run(ctx) {
        const problems = wordingProblems({ files: scopedFiles(ctx.root) });
        return { ok: problems.length === 0, lines: problems };
    },
    selfTest,
};
