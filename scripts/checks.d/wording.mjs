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
//   unverified-support「官方认证」「官方合作」「works with Logic」一类
//   naming            产品名：Rinne Send / Rinne Return / 回环输入 / 回环输出 / Loopback
//   doctrine          「业力」「转世」「涅槃」「karma」一类
//   competitor        「better than」「比……更好」
//   coming-soon       「coming soon」「即将推出」「敬请期待」
//   fader-after       「推子后」，以及英文里 Rinne Output 与 after the fader / post-fader 同句
//   whole-channel     「整条通道」「the whole channel」
//   auto-pairing      「自动配对」「auto-pairing」「automatically pair」
// 另有三条不属于禁用词家族的判据：
//   scope-closed      仓内跟踪的文本文件必须登记在扫描范围或排除清单里，新增的用户面文档不会悄悄漏扫
//   scope-parse       扫描范围里的 JSON / JS 词条文件读不出来，就没法按字符串值扫描，直接判红
//   scope-data        范围表 docs/wording-scope.json 自己的形状：没有未知字段、路径不重复、同一路径不既在扫描范围又在排除清单、
//                     排除清单每组写明理由、核心用户面文件（两份 README、中文用户指南、硬规则词条）不得挪出扫描范围、
//                     排除清单不得盖住词条目录 web/i18n/（它下面的 .js 一律扫，用引号与否定放行合法的反例）
//
// 扫描范围与排除清单是数据，放在 docs/wording-scope.json，不放在本文件里：本文件在基础设施路径之内，
// 如果范围表写在这里，以后每新增一份面向用户的文档都要改本文件、都不能自合。数据文件不在基础设施清单里，
// 新增文档只需要在它里面登记一行；排除清单的每一组都要写明理由，改动会出现在 PR 的 diff 里被评审看到。
//
// 扫描单位是「文本里真正会显示给用户的字符串」，按文件类型取：
//   Markdown / 文本  整个文件
//   JSON             每个字符串值（键不算，键是标识符）
//   JS 词条文件      每个字符串字面量的内部文本（注释不算）
// 取出来再按散文扫描，所以引号放行只看字符串内部的引号：JSON 与双引号 JS 词条的整行值都被引号包着，
// 如果在原文上判引号，这些文件里的每一处命中都会被放过。
//
// 两条放行规则，任一命中即放过：
//   1. 命中片段在引号里（「」 “” ‘’ ""）——引用禁用词本身的句子；
//   2. 命中片段所在的分句里、紧挨它之前出现登记过的否定或免责短语（不能保证、no software can、do not……），
//      或它的紧后面直接跟着「is not」「是不可能的」这类否定。分句以逗号、冒号、分号、顿号、破折号、括号为界。
//      故意不认同一句里远处的单字「不」「无」「no」：「永不啸叫，不用担心」「works with Logic, no extra setup」
//      这类肯定式强断言，随手一个「不用」「no」不能把它放过。
// 排除清单里的文件不扫（它们合法地引用禁用词本身）。
//
// 判据名见 RULES。自测的每个坏样例写明它必须触发哪一条，并核对每一条、每个子模式都至少被一个坏样例命中：
// 删掉任何一条判据或任何一个子模式，点名它的坏样例就变红。

import fs from "node:fs";
import path from "node:path";
import { ROOT, caseResult, readText, tempRepo, trackedFiles } from "../lib/check-kit.mjs";

/** 判据名 → 说明。 */
export const RULES = {
    "absolute-support": "不写「支持所有 DAW」「兼容所有宿主」这类断言",
    "all-daw-zh": "不写中文形态的「支持所有 DAW」",
    "zero-latency": "不写英文形态的 zero latency",
    "zero-latency-zh": "不写中文形态的零延迟",
    "safety-promise": "不写永不啸叫、绝对安全、100% safe 这类承诺",
    "first-only": "不写唯一、首个、the first open-source feedback plugin 这类说法",
    "signed-claims": "不写已签名、已公证、is notarized 这类说法",
    "unverified-support": "不写官方认证、官方合作、works with Logic 这类未验证的支持声明",
    naming: "不用 Rinne Send、Rinne Return、回环输入、回环输出、Loopback 当产品名",
    doctrine: "不写业力、转世、涅槃、karma 这类说法",
    competitor: "不写比别家更好、replaces Connector 这类比较",
    "coming-soon": "不写 coming soon、即将推出、敬请期待",
    "fader-after": "不把 Rinne Output 说成在推子后",
    "whole-channel": "不写整条通道、the whole channel",
    "auto-pairing": "不写自动配对、auto-pairing、automatically pair",
    "scope-closed": "仓内跟踪的文本文件必须在扫描范围或排除清单里，扫描范围里至少有一个文件存在",
    "scope-parse": "扫描范围里的 JSON / JS 词条文件必须读得出来",
    "scope-data":
        "docs/wording-scope.json 的形状：字段齐全且没有未知字段，路径合法且不重复，同一路径不既在扫描范围又在排除清单，排除每组有理由，核心用户面文件留在扫描范围，排除清单不盖住词条目录 web/i18n/",
};

/** 范围表：扫描范围与排除清单的数据文件（相对仓根）。 */
export const SCOPE_FILE = "docs/wording-scope.json";

/**
 * 核心用户面文件（底线）：仓里已经存在的所有公开用户面文档——两份 README、更新日志、安全策略、贡献指南、安装说明、
 * 中文用户指南与硬规则词条。任何改动都不许把它们挪出扫描范围（范围表不在基础设施清单里，这是它的底线）。
 * 以实际存在为准：英文版用户指南 docs/USER_GUIDE.md、docs/DAW_COMPATIBILITY.md、docs/KNOWN_ISSUES.md 现在都不存在，
 * 它们落地的同一个 PR 要把路径加进这张清单（范围表里已经先登记了它们的路径，文件一出现就被扫；
 * 加进这里是为了此后不许挪出去）。
 */
export const REQUIRED_SCOPE = [
    "README.md",
    "README.zh-CN.md",
    "CHANGELOG.md",
    "SECURITY.md",
    "CONTRIBUTING.md",
    "docs/INSTALL.md",
    "docs/USER_GUIDE.zh-CN.md",
    "docs/hard-rules.i18n.json",
];

/**
 * 总是要扫的目录：scopedFiles 读这里的全部 .js 词条文件，新增词条文件不用登记。
 * 排除清单不许盖住它们（否则界面文字悄悄整体漏扫）；词条里合法的反例靠引号与否定放行，不靠排除。
 */
export const SCANNED_DIRS = ["web/i18n/"];

const TOP_KEYS = ["version", "scope", "excluded"];
const GROUP_KEYS = ["reason", "paths"];

/** 一个路径是否合法：仓内相对路径、正斜杠、不含通配与上级目录；目录以 / 结尾。返回问题说明或 null。 */
function pathProblem(p, { dirOk }) {
    if (typeof p !== "string" || !p.trim()) return "不是非空字符串";
    if (p.includes("\\")) return "含反斜杠，路径一律写正斜杠";
    if (/^(?:\/|[A-Za-z]:|\.\/)/.test(p)) return "不是相对仓根的路径";
    if (p.split("/").includes("..") || p.includes("//")) return "含上级目录或空段";
    if (/[*?[\]{}]/.test(p)) return "含通配符：范围表只认逐字路径，通配会悄悄什么都匹配不到";
    if (p.endsWith("/") && !dirOk) return "扫描范围里只能写文件，不能写目录";
    return null;
}

/**
 * 范围表的形状问题；返回问题字符串数组，每条以 [scope-data] 开头。
 * 逐项核对：顶层与排除组里没有未知字段；路径合法；路径在全表里不重复；同一路径不既在扫描范围又在排除清单；
 * 扫描范围里的文件不落在排除目录之下（会被悄悄跳过）；排除每组有理由；核心用户面文件留在扫描范围；
 * 排除清单不盖住总是要扫的词条目录 web/i18n/。
 */
export function scopeTableProblems(data) {
    const out = [];
    const bad = (m) => out.push(`[scope-data] ${SCOPE_FILE} ${m}`);
    if (!data || typeof data !== "object" || Array.isArray(data)) {
        bad("的内容不是对象");
        return out;
    }
    for (const k of Object.keys(data)) if (!TOP_KEYS.includes(k)) bad(`有未知字段「${k}」`);
    if (data.version !== 1) bad(`的 version 应为 1（现为 ${JSON.stringify(data.version)}）`);
    const scope = [];
    if (!Array.isArray(data.scope) || !data.scope.length) bad("缺非空的 scope 数组");
    else
        data.scope.forEach((p, i) => {
            const why = pathProblem(p, { dirOk: false });
            if (why) bad(`的 scope 第 ${i + 1} 项「${p}」${why}`);
            else scope.push(p);
        });
    const excluded = [];
    if (!Array.isArray(data.excluded)) bad("缺 excluded 数组");
    else
        data.excluded.forEach((g, i) => {
            const where = `的 excluded 第 ${i + 1} 组`;
            if (!g || typeof g !== "object" || Array.isArray(g)) return bad(`${where}不是对象`);
            for (const k of Object.keys(g)) if (!GROUP_KEYS.includes(k)) bad(`${where}有未知字段「${k}」`);
            if (typeof g.reason !== "string" || !g.reason.trim()) bad(`${where}缺理由 reason`);
            if (!Array.isArray(g.paths) || !g.paths.length) return bad(`${where}缺非空的 paths 数组`);
            g.paths.forEach((p) => {
                const why = pathProblem(p, { dirOk: true });
                if (why) bad(`${where}的路径「${p}」${why}`);
                else excluded.push(p);
            });
        });
    const seen = new Set();
    for (const p of scope) {
        if (seen.has(p)) bad(`的 scope 里「${p}」重复`);
        seen.add(p);
    }
    const seenEx = new Set();
    for (const p of excluded) {
        if (seenEx.has(p)) bad(`的 excluded 里「${p}」重复`);
        seenEx.add(p);
    }
    for (const p of seen) if (seenEx.has(p)) bad(`里「${p}」既在 scope 又在 excluded，该扫还是不该扫要二选一`);
    for (const p of seen) {
        const dir = excluded.find((e) => e.endsWith("/") && p.startsWith(e));
        if (dir) bad(`的 scope 里「${p}」落在排除目录「${dir}」之下，会被悄悄跳过`);
    }
    for (const p of REQUIRED_SCOPE) if (!seen.has(p)) bad(`的 scope 里缺核心用户面文件「${p}」，它们不许挪出扫描范围`);
    for (const e of seenEx)
        for (const d of SCANNED_DIRS)
            if (e.endsWith("/") ? d.startsWith(e) || e.startsWith(d) : e.startsWith(d))
                bad(`的 excluded 里「${e}」盖住了总是要扫的词条目录「${d}」，界面文字不许整体排除`);
    return out;
}

/**
 * 解析范围表文本。返回 { scope, excluded, problems }：形状有问题时 scope 与 excluded 仍尽量给出合法的部分，
 * 调用方看到 problems 非空就该判红，不要在残缺的表上继续扫。
 */
export function parseScopeTable(text) {
    let data;
    try {
        data = JSON.parse(text);
    } catch (e) {
        return { scope: [], excluded: [], problems: [`[scope-data] ${SCOPE_FILE} 不是合法 JSON：${e.message}`] };
    }
    const problems = scopeTableProblems(data);
    const scope = Array.isArray(data?.scope) ? data.scope.filter((p) => typeof p === "string") : [];
    const excluded = Array.isArray(data?.excluded)
        ? data.excluded.flatMap((g) => (Array.isArray(g?.paths) ? g.paths.filter((p) => typeof p === "string") : []))
        : [];
    return { scope, excluded, problems };
}

/** 读仓根下的范围表；文件不在时 problems 里写明。 */
export function loadScopeTable(root = ROOT) {
    const text = readText(path.join(root, SCOPE_FILE));
    if (text === null) return { scope: [], excluded: [], problems: [`[scope-data] 缺 ${SCOPE_FILE}`] };
    return parseScopeTable(text);
}

const TABLE = loadScopeTable(ROOT);
/** 扫描范围：用户面文本。web/i18n/ 下的词条文件在运行时扫全部 .js（见 scopedFiles）。数据在 docs/wording-scope.json。 */
export const SCOPE = TABLE.scope;
/**
 * 排除清单：这些文件里有合法的命中，或者根本不是写给用户看的文字。目录以 / 结尾，按前缀匹配。
 * 范围闭合判据（scope-closed）要求每个跟踪的文本文件不在 SCOPE 就在这里。数据在 docs/wording-scope.json。
 */
export const EXCLUDED = TABLE.excluded;

/** 文件在排除清单里（目录按前缀匹配）。 */
export function isExcluded(file, excluded = EXCLUDED) {
    return excluded.some((e) => (e.endsWith("/") ? file.startsWith(e) : file === e));
}

/** 词条文件：web/i18n/ 下的 .js。 */
const isI18nFile = (file) => /^web\/i18n\/[^/]+\.js$/.test(file);

// ---------------------------------------------------------------------------------------------
// 文本单位
// ---------------------------------------------------------------------------------------------

/** JS 字符串字面量里一个转义序列；返回 [解出的字符, 消耗的字符数]（含反斜杠）。 */
function unescapeAt(text, i) {
    const c = text[i + 1];
    if (c === undefined) return ["", 1];
    if (c === "n") return ["\n", 2];
    if (c === "t") return ["\t", 2];
    if (c === "r") return ["\r", 2];
    if (c === "\n") return ["", 2]; // 行续接
    if (c === "u") {
        const brace = /^\{([0-9a-fA-F]+)\}/.exec(text.slice(i + 2, i + 12));
        if (brace) return [String.fromCodePoint(parseInt(brace[1], 16)), 2 + brace[0].length];
        const hex = /^[0-9a-fA-F]{4}/.exec(text.slice(i + 2, i + 6));
        if (hex) return [String.fromCharCode(parseInt(hex[0], 16)), 6];
    }
    if (c === "x") {
        const hex = /^[0-9a-fA-F]{2}/.exec(text.slice(i + 2, i + 4));
        if (hex) return [String.fromCharCode(parseInt(hex[0], 16)), 4];
    }
    return [c, 2];
}

/**
 * JS 源码里的字符串字面量内部文本（单引号、双引号、反引号），按出现顺序。注释（// 与 块注释）不算。
 * 词条文件是纯数据，不会有正则字面量，所以不处理它们。
 */
export function jsStrings(text) {
    const out = [];
    const n = text.length;
    let i = 0;
    while (i < n) {
        const c = text[i];
        const d = text[i + 1];
        if (c === "/" && d === "/") {
            while (i < n && text[i] !== "\n") i++;
            continue;
        }
        if (c === "/" && d === "*") {
            const end = text.indexOf("*/", i + 2);
            i = end < 0 ? n : end + 2;
            continue;
        }
        if (c === '"' || c === "'" || c === "`") {
            let buf = "";
            let j = i + 1;
            while (j < n && text[j] !== c) {
                if (text[j] === "\\") {
                    const [ch, used] = unescapeAt(text, j);
                    buf += ch;
                    j += used;
                    continue;
                }
                if (text[j] === "\n" && c !== "`") break; // 没收口的字面量到行尾为止
                buf += text[j++];
            }
            out.push(buf);
            i = j + 1;
            continue;
        }
        i++;
    }
    return out;
}

/** JSON 里的全部字符串值（不含键）。 */
function jsonStrings(value, out = []) {
    if (typeof value === "string") out.push(value);
    else if (Array.isArray(value)) for (const v of value) jsonStrings(v, out);
    else if (value && typeof value === "object") for (const v of Object.values(value)) jsonStrings(v, out);
    return out;
}

/**
 * 一个文件里要按散文扫描的文本单位。JSON 与 JS 取字符串值（引号放行只看值内部），其余整个文件一个单位。
 * JSON 不合法时抛错，由调用方归到 scope-parse。
 */
export function proseUnits(file, text) {
    if (file.endsWith(".json")) return jsonStrings(JSON.parse(text));
    if (file.endsWith(".js")) return jsStrings(text);
    return [text];
}

// ---------------------------------------------------------------------------------------------
// 句子、引号、否定
// ---------------------------------------------------------------------------------------------

/** 引号：短语在引号里就放过。 */
const QUOTE_RE = /「[^」]*」|“[^”]*”|‘[^’]*’|"[^"\n]*"/g;

/** 句子切分：句号、问号、感叹号、分号、换行，以及英文的句点加空白。 */
export function sentences(text) {
    return text
        .split(/(?<=[。！？!?;；\n])|(?<=\.)[ \t]+/)
        .map((s) => s.trim())
        .filter(Boolean);
}

/** 一句话里引号圈起来的区间。 */
export function quotedSpans(sentence) {
    const out = [];
    for (const m of sentence.matchAll(QUOTE_RE)) out.push([m.index, m.index + m[0].length]);
    return out;
}

/** 分句界：逗号、冒号、分号、顿号、破折号、括号与带空格的连字符。否定只认同一分句里命中片段之前的那一段。 */
const CLAUSE_BREAK = /[,，;；:：、—–()（）[\]]|\s-\s/g;
/** 命中片段之前最多看多少个字符。 */
const WINDOW = 40;

/**
 * 登记过的否定与免责短语，只在命中片段之前（同一分句、最近 40 个字符）里找。
 * 故意不收单字「不」「无」「未」与 \bno\b：它们在同一句的别处出现不说明这个命中被否定了。
 */
export const NEGATION_BEFORE = [
    /\b(?:not|never|cannot|without)\b/i,
    /n['’]t\b/i,
    /\bno (?:software|plug-?in|tool|product|one|guarantee|promise)\b/i,
    /(?:不能|无法|没法|不会|不可|不得|不要|不应|禁止|勿|并非|不是|没有|尚未|未能|不保证|不承诺|不声称|不宣称|绝不|并不|从不|从未)/,
    /[不无未非没别]\s*$/, // 紧挨着命中片段的单字否定：「不支持所有 DAW」
];

/** 命中片段紧后面直接跟着的否定：「zero latency is not claimed」「零延迟是不可能的」。 */
export const NEGATION_AFTER = [
    /^\s*(?:is|are|was|were|will be|can be|should be|must be)\s+(?:not|never)\b/i,
    /^\s*(?:isn['’]t|aren['’]t|cannot|can['’]t|won['’]t)\b/i,
    /^[的是]?(?:不对|不成立|不可信|不可能|做不到|无法|不能|并非|不是)/,
];

/** 命中片段之前同一分句里、最近 WINDOW 个字符。 */
function clauseBefore(sentence, index) {
    const before = sentence.slice(0, index);
    let cut = 0;
    for (const m of before.matchAll(CLAUSE_BREAK)) cut = m.index + m[0].length;
    return before.slice(cut).slice(-WINDOW);
}

/** 一个命中（在 sentence 里从 index 起、长 len）是否被登记过的否定或免责放过。 */
export function negatedAt(sentence, index, len) {
    const before = clauseBefore(sentence, index);
    if (NEGATION_BEFORE.some((re) => re.test(before))) return true;
    const after = sentence.slice(index + len);
    return NEGATION_AFTER.some((re) => re.test(after));
}

// ---------------------------------------------------------------------------------------------
// 禁用词家族
// ---------------------------------------------------------------------------------------------

/**
 * 禁用词家族。patterns 每项是 { re, negation }：negation 为假时，否定标记不放过它
 * （safety-promise 的英文 never howls 靠正则里的否定后顾，豁免写在自己的形态里）。
 * 自测按 `家族#序号` 核对每个子模式都被某个坏样例命中；custom 记为 `家族#custom`。
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
            { re: /绝对安全/ },
            // 永不啸叫、永远不啸叫、永不会啸叫；带「无法保证」一类免责前缀的由否定窗口放过
            { re: /永(?:远)?不(?:会)?啸叫/ },
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
            // 「is signed off」是提交的 sign-off，不是代码签名
            { re: /\b(?:is|are|now|fully|officially)\s+(?:code[- ])?(?:signed(?!\s+off\b)|notarized)\b/i },
            { re: /\bsigned and notarized\b/i },
            { re: /已签名/ },
            { re: /已公证/ },
        ],
    },
    {
        id: "unverified-support",
        note: "官方认证、官方合作、works with Logic、supports Intel",
        patterns: [
            { re: /\b(?:Steinberg|Apple|Universal Audio)[- ](?:approved|certified)\b/i },
            { re: /\b(?:Steinberg|Apple|Universal Audio)\s*官方(?:合作|推荐)/i },
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
        // 英文形态：同一句里既说 Rinne Output，又说 after the fader / post-fader。命中位置取放置短语本身，
        // 这样「Rinne Output must not be placed post-fader」的否定落在命中之前，能被放过
        custom: (sentence) => {
            if (!/\bRinne Output\b/i.test(sentence)) return null;
            const m = /\b(?:after the fader|post-fader)\b/i.exec(sentence);
            return m ? { index: m.index, text: m[0] } : null;
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

/** matchAll 要求全局正则；表里的正则写成非全局的，这里补上 g。 */
const global = (re) => (re.global ? re : new RegExp(re.source, re.flags.includes("g") ? re.flags : `${re.flags}g`));

/**
 * 逐命中扫描一份文本单位。返回 [{ family, key, hit, passed }]，passed 为真表示被引号或否定放过。
 * key 是 `家族#序号` 或 `家族#custom`，自测用它核对子模式覆盖。
 */
export function scanUnit(text) {
    const out = [];
    for (const family of FAMILIES) {
        if (!Object.hasOwn(RULES, family.id)) throw new Error(`未登记的判据 ${family.id}`);
        for (const sentence of sentences(text ?? "")) {
            const spans = quotedSpans(sentence);
            const inQuote = (i) => spans.some(([a, b]) => i >= a && i < b);
            (family.patterns ?? []).forEach((p, k) => {
                for (const m of sentence.matchAll(global(p.re))) {
                    const passed =
                        inQuote(m.index) || (p.negation !== false && negatedAt(sentence, m.index, m[0].length));
                    out.push({ family: family.id, key: `${family.id}#${k}`, hit: m[0], passed });
                }
            });
            if (family.custom) {
                const hit = family.custom(sentence);
                if (hit)
                    out.push({
                        family: family.id,
                        key: `${family.id}#custom`,
                        hit: hit.text,
                        passed: inQuote(hit.index) || negatedAt(sentence, hit.index, hit.text.length),
                    });
            }
        }
    }
    return out;
}

/** 每个家族的子模式键（含 custom）。 */
export const PATTERN_KEYS = FAMILIES.flatMap((f) => [
    ...(f.patterns ?? []).map((_, k) => `${f.id}#${k}`),
    ...(f.custom ? [`${f.id}#custom`] : []),
]);

/**
 * 判据的实现。files 是 { 路径: 文本 }，excluded 是排除清单（默认取范围表）。返回问题数组（每行以 [家族名] 开头）。
 */
export function wordingProblems({ files, excluded = EXCLUDED }) {
    const out = [];
    for (const [file, text] of Object.entries(files)) {
        if (isExcluded(file, excluded)) continue;
        let units;
        try {
            units = proseUnits(file, text ?? "");
        } catch (e) {
            out.push(`[scope-parse] ${file}：读不出来，没法按字符串值扫描（${e.message}）`);
            continue;
        }
        for (const unit of units)
            for (const h of scanUnit(unit)) {
                if (h.passed) continue;
                const family = FAMILIES.find((f) => f.id === h.family);
                out.push(`[${h.family}] ${file}：${family.note}（命中「${h.hit}」）`);
            }
    }
    return out;
}

/** 候选文件：需要登记进 SCOPE 或 EXCLUDED 的跟踪文件（文本文档、docs 下的 JSON、web/i18n 词条）。 */
const isCandidate = (file) =>
    /\.(?:md|txt)$/.test(file) || (file.startsWith("docs/") && file.endsWith(".json")) || isI18nFile(file);

/**
 * 范围闭合：tracked 是仓内跟踪的文件，existing 是扫描范围里实际存在的文件数。
 * 每个候选文件不在扫描范围就得在排除清单；扫描范围里一个文件都没读到也判红（等于什么都没扫）。
 */
export function scopeProblems({ tracked, scope = SCOPE, excluded = EXCLUDED, existing }) {
    const out = [];
    for (const f of tracked)
        if (isCandidate(f) && !scope.includes(f) && !isI18nFile(f) && !isExcluded(f, excluded))
            out.push(
                `[scope-closed] ${f} 既不在扫描范围也不在排除清单：面向用户的文字登记进 ${SCOPE_FILE} 的 scope，否则登记进 excluded 并写明理由`,
            );
    if (existing === 0) out.push("[scope-closed] 扫描范围里一个文件都不存在，等于什么都没扫");
    return out;
}

/** 扫描范围内的用户面文本；web/i18n/ 下的词条文件全部扫。 */
export function scopedFiles(root) {
    const files = {};
    for (const rel of loadScopeTable(root).scope) {
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
    const ruleOfLine = (l) => /^\[([^\]]+)\]/.exec(l)?.[1];
    // 自测用固定的排除清单，不依赖真实范围表的内容：真实表以后增删条目不该让自测变红
    const TEST_SCOPE = ["docs/media/placement.txt", ...REQUIRED_SCOPE];
    const TEST_EXCLUDED = ["docs/ARCHITECTURE.md", "docs/only-in-test.md", "scripts/", "src/"];
    const lint = (text, file = "README.md") => wordingProblems({ files: { [file]: text }, excluded: TEST_EXCLUDED });
    const fires = (text, rule, file) => lint(text, file).some((l) => ruleOfLine(l) === rule);
    /** 每个禁用家族、每个子模式一个坏样例；[文本, 家族, 文件?]。 */
    const samples = [
        ["It supports all DAWs and hosts.", "absolute-support"],
        ["支持所有 DAW", "all-daw-zh"],
        ["本插件兼容所有宿主。", "all-daw-zh"],
        ["The loop is zero latency.", "zero-latency"],
        ["这条链是零延迟的。", "zero-latency-zh"],
        ["Rinne never howls.", "safety-promise"],
        ["Rinne is 100% safe.", "safety-promise"],
        ["Rinne protects your ears.", "safety-promise"],
        ["本插件绝对安全。", "safety-promise"],
        ["本插件永不啸叫。", "safety-promise"],
        ["本插件永远不啸叫。", "safety-promise"],
        ["本插件不会爆音。", "safety-promise"],
        ["Rinne is the first open-source feedback plugin.", "first-only"],
        ["它是唯一一款开源插件。", "first-only"],
        ["The build is now notarized.", "signed-claims"],
        ["Builds ship signed and notarized.", "signed-claims"],
        ["这些构建已签名。", "signed-claims"],
        ["这些构建已公证。", "signed-claims"],
        ["Steinberg-certified plug-ins.", "unverified-support"],
        ["Rinne is an Apple Universal Audio-approved plug-in.", "unverified-support"],
        ["Steinberg官方合作伙伴。", "unverified-support"],
        ["通过官方认证。", "unverified-support"],
        ["It works with Logic Pro.", "unverified-support"],
        ["Intel Macs are supported.", "unverified-support"],
        ["支持 Intel Mac。", "unverified-support"],
        ["Rinne Send is the sending end.", "naming"],
        ["这个插件叫回环输入。", "naming"],
        ["发送端叫回环输出。", "naming"],
        ["The product used to be called Loopback.", "naming"],
        ["It follows the doctrine of karma.", "doctrine"],
        ["这是业力。", "doctrine"],
        ["轮回即转世。", "doctrine"],
        ["达到涅槃。", "doctrine"],
        ["得到解脱。", "doctrine"],
        ["Rinne is better than the other one.", "competitor"],
        ["Rinne replaces Connector.", "competitor"],
        ["Rinne 比别家更好。", "competitor"],
        ["Rinne is coming soon.", "coming-soon"],
        ["功能即将推出。", "coming-soon"],
        ["AU 即将支持。", "coming-soon"],
        ["敬请期待。", "coming-soon"],
        ["Rinne Output 放在推子后。", "fader-after"],
        ["Put Rinne Output after the fader divider.", "fader-after"],
        ["它处理整条通道。", "whole-channel"],
        ["It processes the whole channel.", "whole-channel"],
        ["两端自动配对。", "auto-pairing"],
        ["The two plug-ins auto-pairing happens by itself.", "auto-pairing"],
        ["Rinne automatically pairs the two ends.", "auto-pairing"],
        ["The two ends pairs automatically.", "auto-pairing"],
        // 同句里远处的单字否定不能放过肯定式强断言
        ["本插件永不啸叫，不用担心。", "safety-promise"],
        ["本插件绝对安全，不需要担心。", "safety-promise"],
        ["It is the first open-source feedback plugin, not a toy.", "first-only"],
        ["Works with Logic Pro, no extra setup.", "unverified-support"],
        ["Rinne supports all DAWs, no exceptions.", "absolute-support"],
        ["本插件支持所有 DAW，无论什么宿主。", "all-daw-zh"],
        ["Rinne is zero latency, no kidding.", "zero-latency"],
        ["Rinne is not a toy, it supports all DAWs.", "absolute-support"],
        [
            "Rinne does not do many things in this section of the manual and then it supports all DAWs.",
            "absolute-support",
        ],
        // JSON 与 JS 词条的字符串值整个被引号包着，必须按值内部扫描
        ['{"en":{"body":"Rinne is zero latency."}}', "zero-latency", "docs/hard-rules.i18n.json"],
        ['export default { en: { "x.y": "Rinne is zero latency and 100% safe." } };', "zero-latency", "web/i18n/x.js"],
        ["export default { en: { 'x.y': 'Rinne is zero latency.' } };", "zero-latency", "web/i18n/x.js"],
        ["export default { zh: { 'x.y': '本插件绝对安全。' } };", "safety-promise", "web/i18n/x.js"],
        ["export default { en: { x: `Rinne is 100% safe.` } };", "safety-promise", "web/i18n/x.js"],
        ["{bad json", "scope-parse", "docs/hard-rules.i18n.json"],
    ];
    /** 必须放行的写法。 */
    const allowed = [
        ["Rinne reports no plugin latency to the host."],
        ["向宿主报告 0 延迟。"],
        ["Put Rinne Input in the first insert slot."],
        ["放在第一格。"],
        ["The builds are not signed with an Apple Developer ID and not notarized."],
        ["The builds are unsigned."],
        ["Every commit must be signed off."],
        ["Every commit is signed off."],
        ["Rinne was tested in two hosts."],
        ["Logic Pro is not tested."],
        ["使用微软官方安装程序。"],
        ["No software can guarantee a loop will never howl."],
        ["无法保证环路永远不啸叫。"],
        ["我们不能保证本插件不会爆音。"],
        ["Put Rinne Output above the fader divider."],
        ["Rinne Output must not be placed post-fader."],
        ["Intel Macs are not supported."],
        ["It is in the backlog, no date."],
        ["Rinne does not claim to support all DAWs."],
        ["We cannot promise zero latency."],
        ["We don't promise zero latency."],
        ["We ship it without any zero latency promise."],
        ["No plugin can promise 100% safe."],
        ["Never describe Rinne as the first open-source feedback plugin."],
        ["Zero latency is not claimed."],
        ["Zero latency isn't something we promise."],
        ["零延迟是不可能的。"],
        ["Rinne 不支持所有 DAW。"],
        ["「支持所有 DAW」这种写法不写。"],
        ['The words "zero latency" are not used.'],
        ["Rinne Input（返回端）与 Rinne Output（发送端）。"],
        ["輪廻（循环）这个名字的意思是循环。"],
        // JSON 与 JS：值内用「」圈起来的禁用词放行；注释与 JSON 键不是用户面文字
        ['{"zh":{"body":"「零延迟」这种写法不写。"}}', "docs/hard-rules.i18n.json"],
        ['export default { zh: { "x.y": "说明：「支持所有 DAW」不写。" } };', "web/i18n/x.js"],
        ["export default { en: { 'x.y': 'Say \\u201czero latency\\u201d? No.' } };", "web/i18n/x.js"],
        ["// zero latency 只出现在注释里\nexport default { en: { 'x.y': 'ok' } };", "web/i18n/x.js"],
        ["/* 100% safe 只出现在块注释里 */\nexport default { en: { x: 'ok' } };", "web/i18n/x.js"],
        ['{"zero latency":"ok"}', "docs/hard-rules.i18n.json"],
        // 排除清单里的文件不判红
        ["干声随整条通道进环。", "docs/ARCHITECTURE.md"],
        ["探针描述推子后。", "scripts/probe/x.md"],
    ];

    const cases = [];
    const fired = new Set();
    for (const [text, rule, file] of samples) {
        if (rule !== "scope-parse") for (const h of unitsOf(text, file)) if (!h.passed) fired.add(h.key);
        cases.push({ label: `坏样例：${text}`, bad: true, passed: fires(text, rule, file), expect: [rule] });
    }
    for (const [text, file] of allowed)
        cases.push({ label: `放行：${text}`, bad: false, passed: lint(text, file).length === 0, expect: [] });

    // 范围闭合
    const tracked = [
        "README.md",
        "docs/media/placement.txt",
        "docs/ARCHITECTURE.md",
        "src/core/CMakeLists.txt",
        "a.png",
    ];
    cases.push({
        label: "新增的用户面文档没登记：范围闭合判红",
        bad: true,
        passed: scopeProblems({
            tracked: [...tracked, "docs/FAQ.md"],
            scope: TEST_SCOPE,
            excluded: TEST_EXCLUDED,
            existing: 3,
        }).some((l) => ruleOfLine(l) === "scope-closed" && l.includes("docs/FAQ.md")),
        expect: ["scope-closed"],
    });
    cases.push({
        label: "扫描范围里一个文件都不存在：判红",
        bad: true,
        passed: scopeProblems({ tracked, scope: TEST_SCOPE, excluded: TEST_EXCLUDED, existing: 0 }).some(
            (l) => ruleOfLine(l) === "scope-closed" && l.includes("什么都没扫"),
        ),
        expect: ["scope-closed"],
    });
    cases.push({
        label: "登记过的与排除清单里的文件：范围闭合放行",
        bad: false,
        passed: scopeProblems({ tracked, scope: TEST_SCOPE, excluded: TEST_EXCLUDED, existing: 3 }).length === 0,
        expect: [],
    });

    // 范围表自己的形状（scope-data）：每个子判据一个坏样例，外加正例
    const goodTable = () => ({
        version: 1,
        scope: [...TEST_SCOPE],
        excluded: [{ reason: "不是用户面文字", paths: ["docs/ARCHITECTURE.md", "scripts/"] }],
    });
    const edited = (edit) => {
        const t = goodTable();
        edit(t);
        return t;
    };
    const badTable = (label, data, fragment) =>
        cases.push({
            label: `范围表：${label}`,
            bad: true,
            passed: scopeTableProblems(data).some((l) => ruleOfLine(l) === "scope-data" && l.includes(fragment)),
            expect: ["scope-data"],
        });
    cases.push({
        label: "范围表：形状齐全的表放行",
        bad: false,
        passed: scopeTableProblems(goodTable()).length === 0,
        expect: [],
    });
    cases.push({
        label: "范围表：解析出的扫描范围与排除清单逐项对得上",
        bad: false,
        passed: (() => {
            const t = parseScopeTable(JSON.stringify(goodTable()));
            return (
                t.problems.length === 0 &&
                t.scope.join() === TEST_SCOPE.join() &&
                t.excluded.join() === "docs/ARCHITECTURE.md,scripts/"
            );
        })(),
        expect: [],
    });
    badTable("内容不是对象", [], "内容不是对象");
    badTable(
        "顶层有未知字段",
        edited((t) => (t.extra = 1)),
        "未知字段「extra」",
    );
    badTable(
        "排除组里有未知字段",
        edited((t) => (t.excluded[0].note = "x")),
        "未知字段「note」",
    );
    badTable(
        "version 不是 1",
        edited((t) => (t.version = 2)),
        "version 应为 1",
    );
    badTable(
        "scope 不是数组",
        edited((t) => (t.scope = "README.md")),
        "缺非空的 scope 数组",
    );
    badTable(
        "excluded 不是数组",
        edited((t) => (t.excluded = {})),
        "缺 excluded 数组",
    );
    badTable(
        "scope 里的路径重复",
        edited((t) => t.scope.push("README.md")),
        "scope 里「README.md」重复",
    );
    badTable(
        "excluded 里的路径跨组重复",
        edited((t) => t.excluded.push({ reason: "又一组", paths: ["scripts/"] })),
        "excluded 里「scripts/」重复",
    );
    badTable(
        "同一路径既在 scope 又在 excluded",
        edited((t) => t.excluded[0].paths.push("docs/media/placement.txt")),
        "既在 scope 又在 excluded",
    );
    badTable(
        "scope 里的文件落在排除目录之下",
        edited((t) => t.excluded[0].paths.push("docs/media/")),
        "落在排除目录「docs/media/」之下",
    );
    badTable(
        "排除组没有理由",
        edited((t) => (t.excluded[0].reason = "  ")),
        "缺理由",
    );
    badTable(
        "排除清单里的一组不是对象",
        edited((t) => t.excluded.push("docs/x.md")),
        "第 2 组不是对象",
    );
    badTable(
        "排除清单里的路径含上级目录",
        edited((t) => t.excluded[0].paths.push("../x.md")),
        "的路径「../x.md」",
    );
    badTable(
        "排除组没有 paths",
        edited((t) => (t.excluded[0].paths = [])),
        "缺非空的 paths",
    );
    badTable(
        "scope 里写了目录",
        edited((t) => t.scope.push("docs/media/")),
        "不能写目录",
    );
    badTable(
        "路径含上级目录",
        edited((t) => t.scope.push("../x.md")),
        "上级目录",
    );
    badTable(
        "路径含反斜杠",
        edited((t) => t.scope.push("docs\\x.md")),
        "反斜杠",
    );
    badTable(
        "路径是绝对路径",
        edited((t) => t.scope.push("/etc/x.md")),
        "不是相对仓根",
    );
    badTable(
        "路径含通配符",
        edited((t) => t.scope.push("docs/*.md")),
        "通配符",
    );
    // 底线清单里的每一个文件：挪出 scope 判红（单纯删掉，或者挪进排除清单，两种写法都拦）
    for (const required of REQUIRED_SCOPE) {
        badTable(
            `核心用户面文件 ${required} 被挪出 scope`,
            edited((t) => (t.scope = t.scope.filter((p) => p !== required))),
            `缺核心用户面文件「${required}」`,
        );
        badTable(
            `核心用户面文件 ${required} 被挪进排除清单`,
            edited((t) => {
                t.scope = t.scope.filter((p) => p !== required);
                t.excluded[0].paths.push(required);
            }),
            `缺核心用户面文件「${required}」`,
        );
    }
    cases.push({
        label: "核心用户面文件清单覆盖仓里已有的公开用户面文档（两份 README、更新日志、安全策略、贡献指南、安装说明、中文用户指南、硬规则词条）",
        bad: false,
        passed: [
            "README.md",
            "README.zh-CN.md",
            "CHANGELOG.md",
            "SECURITY.md",
            "CONTRIBUTING.md",
            "docs/INSTALL.md",
            "docs/USER_GUIDE.zh-CN.md",
            "docs/hard-rules.i18n.json",
        ].every((p) => REQUIRED_SCOPE.includes(p)),
        expect: [],
    });
    badTable(
        "排除清单盖住词条目录 web/i18n/",
        edited((t) => t.excluded[0].paths.push("web/i18n/")),
        "盖住了总是要扫的词条目录「web/i18n/」",
    );
    badTable(
        "排除清单盖住词条目录的上级 web/",
        edited((t) => t.excluded[0].paths.push("web/")),
        "盖住了总是要扫的词条目录「web/i18n/」",
    );
    badTable(
        "排除清单排除了一个词条文件",
        edited((t) => t.excluded[0].paths.push("web/i18n/common.js")),
        "盖住了总是要扫的词条目录「web/i18n/」",
    );
    cases.push({
        label: "范围表：排除 web/js/ 与 web-preview/ 不算盖住词条目录",
        bad: false,
        passed:
            scopeTableProblems(edited((t) => t.excluded[0].paths.push("web/js/", "web/fonts/", "web-preview/")))
                .length === 0,
        expect: [],
    });
    cases.push({
        label: "范围表：不是合法 JSON",
        bad: true,
        passed: parseScopeTable("{bad").problems.some((l) => ruleOfLine(l) === "scope-data" && l.includes("合法 JSON")),
        expect: ["scope-data"],
    });
    cases.push({
        label: "范围表：文件不在",
        bad: true,
        passed: loadScopeTable(path.join(ROOT, "no-such-dir-for-selftest")).problems.some(
            (l) => ruleOfLine(l) === "scope-data" && l.includes(`缺 ${SCOPE_FILE}`),
        ),
        expect: ["scope-data"],
    });

    // 排除清单与扫描范围取自参数，不取模块里的默认值：只在传入的清单里排除的文件不扫，
    // 只在真实范围表里排除的文件照样扫、照样要求登记
    cases.push({
        label: "排除清单取自参数：只在传入清单里排除的文件放行",
        bad: false,
        passed: lint("干声随整条通道进环。", "docs/only-in-test.md").length === 0,
        expect: [],
    });
    cases.push({
        label: "排除清单取自参数：传入清单里没有的文件照样扫",
        bad: true,
        passed: fires("它处理整条通道。", "whole-channel", "docs/BUILDING.md"),
        expect: ["whole-channel"],
    });
    cases.push({
        label: "范围闭合取自参数：只在传入清单里排除的文件不要求登记",
        bad: false,
        passed:
            scopeProblems({
                tracked: ["docs/only-in-test.md"],
                scope: TEST_SCOPE,
                excluded: TEST_EXCLUDED,
                existing: 3,
            }).length === 0,
        expect: [],
    });
    cases.push({
        label: "范围闭合取自参数：传入清单里没有的文件要求登记",
        bad: true,
        passed: scopeProblems({
            tracked: ["docs/BUILDING.md"],
            scope: TEST_SCOPE,
            excluded: TEST_EXCLUDED,
            existing: 3,
        }).some((l) => ruleOfLine(l) === "scope-closed" && l.includes("docs/BUILDING.md")),
        expect: ["scope-closed"],
    });

    // 整条流程（读范围表 → 扫范围 → 范围闭合）在临时仓里走一遍
    {
        const goodText = JSON.stringify({
            version: 1,
            scope: [...REQUIRED_SCOPE],
            excluded: [{ reason: "不是用户面文字", paths: ["docs/wording-scope.json", "docs/BUILDING.md"] }],
        });
        const repo = (files) => {
            const r = tempRepo();
            for (const [rel, body] of Object.entries(files)) r.write(rel, body);
            r.git(["add", "-A"]);
            return r;
        };
        const base = {
            "README.md": "Hello.\n",
            "README.zh-CN.md": "你好。\n",
            "docs/hard-rules.i18n.json": '{"en":"ok"}',
            "docs/wording-scope.json": goodText,
        };
        const flows = [];
        try {
            const runOn = (files) => {
                const r = repo(files);
                flows.push(r);
                return wordingRun(r.dir);
            };
            const ok = runOn({ ...base, "docs/BUILDING.md": "它处理整条通道。\n" });
            cases.push({
                label: "整条流程：范围表齐全、排除的文件里有禁用词、其余干净：通过",
                bad: false,
                passed: ok.ok && ok.lines.length === 0,
                expect: [],
            });
            const dirty = runOn({ ...base, "README.md": "It is zero latency.\n" });
            cases.push({
                label: "整条流程：范围内的文件里有禁用词：判红",
                bad: true,
                passed: dirty.lines.some((l) => ruleOfLine(l) === "zero-latency"),
                expect: ["zero-latency"],
            });
            // 范围取自范围表而不是模块里的默认清单：表里新登记的文件要被扫到
            const extraTable = JSON.stringify({
                ...JSON.parse(goodText),
                scope: [...JSON.parse(goodText).scope, "docs/EXTRA.md"],
            });
            const extra = runOn({
                ...base,
                "docs/wording-scope.json": extraTable,
                "docs/EXTRA.md": "It is zero latency.\n",
            });
            cases.push({
                label: "整条流程：范围表里新登记的文件被扫到：里面的禁用词判红",
                bad: true,
                passed: extra.lines.some((l) => ruleOfLine(l) === "zero-latency" && l.includes("docs/EXTRA.md")),
                expect: ["zero-latency"],
            });
            // 界面词条（web/i18n/）不用登记，总是被扫：里面的禁用词判红，用引号圈起来的反例放行
            const i18nDirty = runOn({
                ...base,
                "web/i18n/common.js": "export default { en: { 'a.b': 'Rinne is zero latency.' } };\n",
            });
            cases.push({
                label: "整条流程：界面词条里的禁用词被扫到：判红",
                bad: true,
                passed: i18nDirty.lines.some(
                    (l) => ruleOfLine(l) === "zero-latency" && l.includes("web/i18n/common.js"),
                ),
                expect: ["zero-latency"],
            });
            const i18nQuoted = runOn({
                ...base,
                "web/i18n/common.js":
                    "export default { zh: { 'a.b': '说明：「零延迟」这种写法不写。' }, en: { 'a.b': 'We do not claim “zero latency”.' } };\n",
            });
            cases.push({
                label: "整条流程：界面词条里用引号圈起来的反例放行",
                bad: false,
                passed: i18nQuoted.ok && i18nQuoted.lines.length === 0,
                expect: [],
            });
            const i18nExcluded = runOn({
                ...base,
                "docs/wording-scope.json": JSON.stringify({
                    ...JSON.parse(goodText),
                    excluded: [{ reason: "不是用户面文字", paths: ["docs/wording-scope.json", "web/i18n/"] }],
                }),
                "web/i18n/common.js": "export default { en: { 'a.b': 'Rinne is zero latency.' } };\n",
            });
            cases.push({
                label: "整条流程：范围表把 web/i18n/ 排除：范围表判红，不靠排除漏扫词条",
                bad: true,
                passed:
                    !i18nExcluded.ok &&
                    i18nExcluded.lines.length > 0 &&
                    i18nExcluded.lines.every((l) => ruleOfLine(l) === "scope-data"),
                expect: ["scope-data"],
            });
            const unreg = runOn({ ...base, "docs/FAQ.md": "Hello.\n" });
            cases.push({
                label: "整条流程：新增的文档没登记进范围表：范围闭合判红",
                bad: true,
                passed: unreg.lines.some((l) => ruleOfLine(l) === "scope-closed" && l.includes("docs/FAQ.md")),
                expect: ["scope-closed"],
            });
            const broken = runOn({
                ...base,
                "docs/wording-scope.json": JSON.stringify({ ...JSON.parse(goodText), extra: 1 }),
            });
            cases.push({
                label: "整条流程：范围表本身不成形：只报范围表，不在残缺的表上继续扫",
                bad: true,
                passed:
                    !broken.ok && broken.lines.length > 0 && broken.lines.every((l) => ruleOfLine(l) === "scope-data"),
                expect: ["scope-data"],
            });
            const { "docs/wording-scope.json": _gone, ...noTable } = base;
            const missing = runOn(noTable);
            cases.push({
                label: "整条流程：缺范围表：判红",
                bad: true,
                passed: !missing.ok && missing.lines.some((l) => ruleOfLine(l) === "scope-data" && l.includes("缺")),
                expect: ["scope-data"],
            });
        } finally {
            for (const r of flows) r.cleanup();
        }
    }

    // 每个家族的每个子模式都被某个坏样例命中
    const uncovered = PATTERN_KEYS.filter((k) => !fired.has(k));
    cases.push({
        label: `每个子模式都被坏样例命中${uncovered.length ? `（没有：${uncovered.join("、")}）` : ""}`,
        bad: false,
        passed: uncovered.length === 0,
        expect: [],
    });
    const unnamed = Object.keys(RULES).filter((r) => !cases.some((c) => c.passed && c.expect.includes(r)));
    cases.push({
        label: `每条判据都有坏样例点名${unnamed.length ? `（没有：${unnamed.join("、")}）` : ""}`,
        bad: false,
        passed: unnamed.length === 0,
        expect: [],
    });
    return caseResult(cases);
}

/** 自测用：一份文本按文件类型取出单位，逐个扫描。 */
function unitsOf(text, file = "README.md") {
    return proseUnits(file, text).flatMap((u) => scanUnit(u));
}

/**
 * 对仓根 root 跑一遍：先核对范围表，范围表不成形就只报它（不在残缺的表上继续扫，免得一串连带的 scope-closed 把真正的原因淹掉），
 * 否则扫范围内的文本并核对范围闭合。自测用临时仓调它，所以不依赖全局状态。
 */
export function wordingRun(root) {
    const table = loadScopeTable(root);
    if (table.problems.length) return { ok: false, lines: table.problems };
    const files = scopedFiles(root);
    const problems = wordingProblems({ files, excluded: table.excluded });
    let tracked = [];
    try {
        tracked = trackedFiles(root);
    } catch {
        // 不在 git 仓里时没法核对闭合，只扫已有的范围
    }
    problems.push(
        ...scopeProblems({
            tracked,
            scope: table.scope,
            excluded: table.excluded,
            existing: Object.keys(files).filter((f) => table.scope.includes(f)).length,
        }),
    );
    return { ok: problems.length === 0, lines: problems };
}

export default {
    name: "wording",
    group: "docs",
    inputs: [],
    run: (ctx) => wordingRun(ctx.root),
    selfTest,
};
