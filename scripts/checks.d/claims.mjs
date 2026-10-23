// SPDX-License-Identifier: GPL-3.0-or-later
//
// 强声明登记与证据（docs-truth 的一部分）。数据是 docs/claims.json：每条声明登记它的 strong 短语（肯定式的强断言，
// 写成完整的断言，不写功能名这类宽词）、hedge 短语（免责或「按设计」这类限定语）、evidence（证据）、pending（证据产物
// 尚未到位），以及宿主兼容类声明的开关 hedgeWhilePending / hedgeExemptReason（见下面的「宿主兼容类」）。
//
// 判据：
//   claims-shape        claims.json 是合法 JSON；每条有 id / strong / hedge / evidence；id 唯一且为小写连字符；strong 非空；
//                       strong / hedge 的每个短语都是能编译的、不会匹配任意句子的正则；hedge 与 strong 不自相匹配
//                       （hedge 的典型命中本身含 strong，或 strong 的典型命中本身满足 hedge，免责就形同虚设）；
//                       hedge 不是另一个正面断言（典型命中里每一个都要含 DISCLAIMER_TERMS 里的免责词）；
//                       hedgeWhilePending 是布尔值；写 false 必须同时写非空的 hedgeExemptReason，hedgeExemptReason 也
//                       只能跟 false 一起出现；pending 期间要求 hedge 的声明（见下）仍在 pending 时 hedge 非空；
//                       scope 里的每一项都是扫描范围内的文件
//   host-claim-evidence 宿主兼容类声明的 evidence 只认真机的 validation 记录：构建 CI 通过证明不了宿主可用。
//                       豁免 hedge 不豁免这一条
//   evidence-kind       每条 evidence / pending 的类型是 validation: / issue: / ci: / golden: 之一，形态可解析；
//                       validation 的文件名通配（*）只允许写在 pending 里（记录的日期要等上机才知道）；
//                       validation 的路径必须是仓内的规范相对路径（repoPathProblem：拒绝 .. 段、绝对路径、反斜杠、盘符，
//                       规范化后、符号链接与 junction 解开后仍在仓根之内），文件名必须以 -win.md / -macos.md / -linux.md 之一结尾
//                       （recordNameTail：tiers 检查核对记录文件名的系统后缀用的是同一个函数）
//   evidence-satisfied  evidence 里的每一条都成立：validation 文件存在；issue 是 https URL；ci 是仓内存在的 job 名；
//                       golden 的谓词为真（目标文件不存在时按 pending 处理，不在这里判红）
//   pending-listed      证据产物尚不存在时允许直述，并把待办列出来（INFO 行，不判红）
//   strong-needs-hedge  一条声明没有成立的 evidence，又没有 pending（或有 pending 但属于要求 hedge 的宿主兼容类）时，
//                       它的每一处 strong 命中，所在的分句里都要有 hedge 短语（分句规则见 clauseSpans）；
//                       引文放行：命中整个落在行内代码里，或整个落在处在引用语境里的引号中（紧跟在写成 / 不要写 / 例如 /
//                       such as / e.g. / 冒号之后，或括号里举例，见 citedSpans）；引号前面没有引用语境的引号
//                       （单独成句的 "Rinne works in LUNA."）不是引文，照判
//   unregistered-absolute 用户面文本里出现绝对化强断言句式、又没有被任何已登记声明的 strong 覆盖时判红；
//                       覆盖按命中片段算：声明的 strong 命中要与绝对化句式的命中片段重叠，同一句里别处另有 strong 不算
//
// 证据待到位的口径：pending 里的产物还没进仓时，直述允许、只列待办；产物已在仓内但谓词为假，说明声明被证据反驳，判红。
// 这样今天不会因为证据产物还没造出来就判红，产物一落地又立刻被它自己的数据约束。
//
// 宿主兼容类：pending / evidence 里有 validation 真机记录的声明，或写了 hedgeWhilePending: true 的声明。
// 真机记录还没到位前，它的 strong 不能直述，所在分句要带 hedge；记录到位后把它从 pending 移到 evidence，才解除这条要求。
// 这是默认口径，省掉字段躲不开。唯一的豁免是同时写 "hedgeWhilePending": false 与非空的 "hedgeExemptReason"
// （说明为什么暂不要求），理由随数据进评审的视线；只写 false 不写理由、或只写理由不写 false，都判形状错误。
//
// 判据名见 RULES，报错行以 [名字] 开头。自测的每个坏样例写明它必须触发哪几条判据，并核对每条判据都至少被一个坏样例
// 点名：删掉任何一条判据，点名它的坏样例就变红。
//
// golden 的两种形状都认：映射形（顶层键就是参数 id）与数组形（{"params":[{"id":…}]}）。
// 同一条谓词路径（如 feedback.default）在两种形状下必须取到同一个值，否则参数真源换个形状，
// 同一条强声明就会无端判红。params-table 块模块认的是同一套形状。
//
// 分句规则的已知限制：分句靠标点与转折连词机械地切，不做语法分析，下面几种写法不切，免责语会和 strong 算在同一个分句里而放行：
//   `/`；作连词的 yet（「works in LUNA yet not tested elsewhere」）；so；except；括号里给别的主语的免责
//   （「works in LUNA (but Pro Tools is not yet tested)」：括号里的内容算括号所在分句的一部分）。
// 这几种写法靠文案评审挡，机检不承诺；要机检拦下，就把对应的标点或连词加进 CLAUSE_BREAK_SOURCES 并补坏样例。
// 引文放行只用在 strong-needs-hedge 上；unregistered-absolute 仍与 wording 一样，整个落在引号里的命中放行。
//
// 句子切分、否定与免责、引号的识别、扫描范围都和 wording 检查共用同一份实现（从 wording.mjs 引入），不各抄一份；
// 只有 strong-needs-hedge 的引文放行（引号前要有引用语境，citedSpans）是本检查自己的规则，比 wording 的引号放行窄。
// 参数表块模块（gen-docs.d/params-table.mjs）的自测也在这里跑：它认的参数形状就是上面谓词路径认的形状。

import fs from "node:fs";
import path from "node:path";
import { ROOT, caseResult, readText } from "../lib/check-kit.mjs";
import { cleanupTempDir, makeTempDir } from "../lib/safe-rm.mjs";
import { selfTestCases as paramsTableCases, paramsOf } from "../gen-docs.d/params-table.mjs";
import {
    SCOPE,
    loadScopeTable,
    negatedAt,
    proseUnits,
    quotedSpans,
    scopedFiles,
    sentences as plainSentences,
} from "./wording.mjs";
import { parseWorkflow } from "./workflows.mjs";

export { SCOPE, scopedFiles };
export const CLAIMS_FILE = "docs/claims.json";

/** 判据名 → 说明。 */
export const RULES = {
    "claims-shape":
        "claims.json 的形状：id 唯一且为小写连字符，每条有 id / strong / hedge / evidence，strong 非空，短语是可用的正则，hedge 与 strong 不自相匹配，hedge 不是不含免责词的正面断言，hedgeWhilePending 是布尔值，写 false 必须带非空的 hedgeExemptReason（理由也只能跟 false 一起写），要求 hedge 的声明仍在 pending 时 hedge 非空，scope 在扫描范围内",
    "host-claim-evidence":
        "宿主兼容类声明（pending / evidence 里有 validation 记录，或声明了 hedgeWhilePending）的 evidence 只认 validation 真机记录，ci 与 golden 证明不了宿主可用；豁免 hedge 也不豁免这条",
    "evidence-kind":
        "每条 evidence / pending 的类型与形态可解析（validation: / issue: / ci: / golden:），通配只允许 pending；validation 路径必须是仓内规范相对路径（无 .. 段、绝对路径、反斜杠、盘符），文件名以 -win.md / -macos.md / -linux.md 之一结尾",
    "evidence-satisfied": "evidence 里的每一条都成立",
    "pending-listed": "证据产物尚未到位时列出待办（INFO，不判红）",
    "strong-needs-hedge":
        "没有成立 evidence 的声明，若也没有 pending（或属于要求 hedge 的宿主兼容类），每一处 strong 命中所在的分句里都要有 hedge 短语；行内代码和紧跟在引用语境之后的引号里的引文除外",
    "unregistered-absolute": "绝对化强断言必须被某条已登记声明的 strong 命中片段覆盖，同一句里别处的 strong 不算",
};

/**
 * 绝对化强断言的句式（只拦肯定式；含否定与免责的句子放行）。
 * 「零延迟」「支持所有 DAW」这类由 wording 检查直接禁用，不可能被登记成声明，所以不在这里重复。
 */
export const ABSOLUTE_PATTERNS = [
    /\bdoes not change with\b/i,
    /\bnever changes\b/i,
    /\bis identical to\b/i,
    /\bidentical output\b/i,
    /\balways the same output\b/i,
    /\bguarantees\b/i,
    /不随[^。\n]{0,20}变化/,
    /完全一致/,
    /始终一致/,
    /保证[^。\n]{0,20}(?:不变|一致)/,
];

/** 句子切分：先去掉围栏代码块（命令不是声明），再按 wording 的口径切。 */
export function sentences(text) {
    return plainSentences(text.replace(/```[\s\S]*?```/g, " "));
}

/** 声明的 id 形态：小写连字符。 */
const ID_RE = /^[a-z0-9]+(-[a-z0-9]+)*$/;

// ---------------------------------------------------------------------------------------------
// 验证记录的路径与文件名（claims 与 tiers 共用）
// ---------------------------------------------------------------------------------------------

/**
 * 验证记录文件名里的系统后缀，只有这一种写法：daw-tiers.json 的 os 值 windows / macos / linux
 * 对应 win / macos / linux，win 与 windows、mac 与 macos 不混用。claims 的 validation 路径与 tiers 核对记录文件名
 * 都从这里取（recordNameTail），不各写各的。
 */
export const RECORD_OS_SUFFIX = Object.freeze({ windows: "win", macos: "macos", linux: "linux" });

/** 一个系统取值（daw-tiers.json 的 os）对应的后缀：已登记的系统一个，any 取全部，不认识的系统返回空数组。 */
export function recordOsSuffixes(osName) {
    const key = String(osName ?? "").toLowerCase();
    if (key === "any") return Object.values(RECORD_OS_SUFFIX);
    return Object.hasOwn(RECORD_OS_SUFFIX, key) ? [RECORD_OS_SUFFIX[key]] : [];
}

/**
 * 记录文件名结尾的正则源码：-<系统后缀>.md。osName 省掉就是 any（接受登记的全部后缀，claims 用）；
 * tiers 传这一行的 os，只接受这一行系统对应的后缀。系统不认识时返回 null。
 */
export function recordNameTail(osName = "any") {
    const suffixes = recordOsSuffixes(osName);
    return suffixes.length ? `-(?:${suffixes.join("|")})\\.md` : null;
}

/**
 * 自测用：在 link 处建一个指向目录 target 的链接。Windows 上建 junction（不需要特权），别处是普通符号链接。
 * 建成返回 true；建不了（没有权限、文件系统不支持）返回 false，由调用方在用例名里写明跳过的条件。
 */
export function tryMakeDirLink(target, link) {
    try {
        fs.symlinkSync(target, link, "junction");
        return true;
    } catch {
        return false;
    }
}

/** 自测收尾：只摘掉链接本身，不碰它指向的目录；链接已经不在或摘不掉都不抛。 */
export function dropDirLink(link) {
    try {
        fs.unlinkSync(link);
    } catch {
        try {
            fs.rmdirSync(link);
        } catch {
            // 摘不掉就留给临时目录的收尾；那一步按真实路径核对，递归删除不会进链接
        }
    }
}

/** target 相对 base 是否在 base 之外（或就是 base 本身）；纯词法，两个参数都要是已解析的绝对路径。 */
function lexicallyOutside(base, target) {
    const back = path.relative(base, target);
    return back === "" || back === ".." || back.startsWith(`..${path.sep}`) || path.isAbsolute(back);
}

/** 真实路径（符号链接、junction 都解开）；路径不存在或读不出来返回 null。 */
function realPathOrNull(p) {
    try {
        return fs.realpathSync.native(p);
    } catch {
        return null;
    }
}

/**
 * rel 按 root 解析、规范化之后是否落在仓根之外（或就是仓根本身）。词法判断先行；词法上在仓内、而且文件存在时，
 * 再按真实路径判一次（仓根也取真实路径）：仓内若有指向仓外的符号链接或 junction（如 docs/validation/ext → 仓外），
 * 经它读到的文件不算仓内文件。文件不存在时只有词法判断（读不到，也就谈不上被它当成证据）。
 */
export function escapesRoot(root, rel) {
    const base = path.resolve(root);
    const target = path.resolve(base, rel);
    if (lexicallyOutside(base, target)) return true;
    const realTarget = realPathOrNull(target);
    if (realTarget === null) return false;
    return lexicallyOutside(realPathOrNull(base) ?? base, realTarget);
}

/**
 * 仓内相对路径的问题；没问题返回 null。validation 证据的路径只准是仓内的规范相对路径：
 * 不许反斜杠、盘符、绝对路径、空段与 . 段、.. 段；给了仓根时，规范化之后还要落在仓根之内（escapesRoot），
 * 文件存在时按真实路径判（仓内指向仓外的符号链接或 junction 不算仓内）。
 * claims 的 parseEvidence / validationExists 与 tiers 的 recordProblem 都用这一个函数。
 */
export function repoPathProblem(rel, root) {
    if (typeof rel !== "string" || !rel) return "不是非空字符串";
    if (rel.includes("\\")) return "含反斜杠，路径一律写正斜杠";
    if (rel.includes("\0")) return "含空字符";
    if (/^[A-Za-z]:/.test(rel)) return "带盘符";
    if (rel.startsWith("/")) return "是绝对路径，要写相对仓根的路径";
    const segments = rel.split("/");
    if (segments.includes("..")) return "含上级目录段 ..";
    if (segments.some((s) => s === "" || s === ".")) return "含空段或 . 段，路径要写成规范形式";
    if (root !== undefined && escapesRoot(root, rel)) return "规范化后（符号链接与 junction 解开后）落在仓根之外";
    return null;
}

/** validation 记录路径的形态问题（不含是否存在）；没问题返回 null。glob 为真时文件名允许用 *。 */
export function validationShapeProblem(body, { glob = false } = {}) {
    const why = repoPathProblem(body);
    if (why) return `路径${why}`;
    if (!/^(?:docs|tests)\//.test(body)) return "路径要在 docs/ 或 tests/ 之下";
    const slash = body.lastIndexOf("/");
    const dir = body.slice(0, slash);
    const last = body.slice(slash + 1);
    if (body.includes("*")) {
        if (!glob) return "通配只允许写在 pending 里";
        if (dir.includes("*") || !last.includes("*") || last.includes("**")) return "通配只允许出现在最后一段文件名里";
    }
    if (!new RegExp(`${recordNameTail()}$`).test(last))
        return `文件名要以 ${Object.values(RECORD_OS_SUFFIX)
            .map((s) => `-${s}.md`)
            .join(" / ")} 之一结尾（系统后缀只有这一种写法）`;
    return null;
}

// ---------------------------------------------------------------------------------------------
// 分句：hedge 要落在 strong 所在的分句里
// ---------------------------------------------------------------------------------------------

/** 括号：括号里的内容算括号所在分句的一部分，里面的逗号与连词不切分。 */
const OPEN_BRACKETS = "(（[【";
const CLOSE_BRACKETS = ")）]】";

/**
 * 分句界（在括号之外）：
 *   标点  英文逗号与冒号（只有两侧都是数字的才不切：千分位 10,000、时刻 12:30；LUNA 3, not yet tested 里逗号左边是数字，
 *         右边是空格，照切）、中文 ，：（；已经是句子界）、破折号 —、带空格的 –、带空格的连字符 ` - ` 与 `--`
 *         （wording 的分句规则也把 ` - ` 当分句界）
 *   转折连词  while / whereas / but / however / although / though；但（含但是）/ 不过 / 可是 / 而（含然而；而已、而且除外）
 * 不切：and / 和 / 与 / 以及 / 而且 这类并列，以及 not yet 里的 yet（并列留在同一分句里）。
 * 所以「works in LUNA, while Pro Tools is not yet tested」「在 LUNA 可用，而 Cubase 尚未测试」的免责语在另一个分句里，
 * 不算 LUNA 那一句的 hedge；要写成「works in LUNA (host testing pending)」或把 hedge 放进同一个分句。
 */
const CLAUSE_BREAK_SOURCES = {
    // 两侧都是数字才不切（千分位、时刻）；左边是数字、右边不是（LUNA 3, / LUNA 3:），或反过来，都要切
    comma: "(?<!\\d),|,(?!\\d)",
    colon: "(?<!\\d):|:(?!\\d)",
    "zh-punct": "[，：]",
    dash: "—|\\s–\\s",
    hyphen: "\\s-\\s|--",
    while: "\\bwhile\\b",
    whereas: "\\bwhereas\\b",
    but: "\\bbut\\b",
    however: "\\bhowever\\b",
    although: "\\balthough\\b",
    though: "\\bthough\\b",
    但: "但",
    不过: "不过",
    可是: "可是",
    而: "而(?![已且])",
};
const CLAUSE_BREAK_RE = new RegExp(Object.values(CLAUSE_BREAK_SOURCES).join("|"), "gi");

/**
 * 一句话切成的分句区间 [起, 止)。括号配平时括号里的内容遮掉再找分句界；括号不配平（只开不关）时按没有括号处理，
 * 免得一个落单的括号把后面所有的分句界都吞了。
 */
export function clauseSpans(sentence) {
    let masked = "";
    let depth = 0;
    for (const ch of sentence) {
        if (OPEN_BRACKETS.includes(ch)) depth++;
        masked += depth > 0 ? "\u0001".repeat(ch.length) : ch;
        if (CLOSE_BRACKETS.includes(ch) && depth > 0) depth--;
    }
    const text = depth === 0 ? masked : sentence;
    const spans = [];
    let from = 0;
    for (const m of text.matchAll(CLAUSE_BREAK_RE)) {
        spans.push([from, m.index]);
        from = m.index + m[0].length;
    }
    spans.push([from, sentence.length]);
    return spans;
}

/** strong 的一处命中 [a, b) 所在的分句合起来的区间（命中跨过分句界时，跨到的每个分句都算）。 */
export function clauseAround(sentence, a, b) {
    const hit = clauseSpans(sentence).filter(([from, to]) => from < b && a < to);
    if (!hit.length) return [0, sentence.length];
    return [hit[0][0], hit[hit.length - 1][1]];
}

// ---------------------------------------------------------------------------------------------
// 引文：什么样的引号才算「在引用这句话」
// ---------------------------------------------------------------------------------------------

/**
 * 引用语境：紧挨在引号之前（中间只隔空白与开括号）的词。命中就说明引号里的话是被提到的，不是被断言的：
 *   英文  such as / e.g. / i.e. / for example / for instance / like / write(s) / writing / written / wrote /
 *         say(s) / saying / said / avoid(s) / avoiding / quote(s) / quoted / quoting / phrase(s) / wording / word(s) / term(s)
 *   中文  写成 / 写作 / 写 / 说成 / 说 / 避免 / 如 / 用语 / 措辞 / 字样；例如、比如、譬如、诸如都以「如」结尾，由「如」管着，
 *         不另列（另列了，删掉它们没有任何输出变化）；不如、假如的「如」不算
 * 冒号引出的引号另算（见 COLON_LEAD）。
 * 引号前面什么语境都没有（单独成句、或夹在断言中间的 "Rinne works in LUNA."）不是引文，照常判。
 */
const CITE_LEAD_IN = new RegExp(
    "(?:\\b(?:such as|e\\.g\\.|i\\.e\\.|for example|for instance|like|writes?|writing|written|wrote|says?|saying|said|" +
        "avoids?|avoiding|quotes?|quoted|quoting|phrases?|wording|words?|terms?)|" +
        "写成|写作|写|说成|说|避免|(?<![不假])如|用语|措辞|字样)$",
    "i",
);
/** 冒号引出：「Never write: "works in LUNA"」「注意：「在 LUNA 可用」」。 */
const COLON_LEAD = /[:：]$/;
/** 引号后面直接跟着的指称：「「在 LUNA 可用」这种写法不写」。 */
const CITE_LEAD_OUT = /^\s*(?:这种|这样|这类|此类|这句)(?:写法|说法|话|措辞|用语|字样|表述)/;
/** 括号里举例：「Strong claims about behaviour ("does not change with...")」，括号之前的话在谈声明或措辞。 */
const CLAIM_TALK = /\b(?:claims?|phrases?|wording|statements?)\b|声明|说法|措辞|写法|用语|字样|短语/i;
/** 两个引号之间只有列举连接：`"a", "b"`、`"a" or "b"`、`「a」、「b」`。 */
const QUOTE_LIST_GAP = /^\s*(?:[,，、]\s*)?(?:(?:or|and)\s+|或者?|和)?$/i;

/**
 * 一句话里「引文」的区间：行内代码，以及处在引用语境里的引号。
 * 引号的判定（任一成立）：紧跟在引用语境之后（CITE_LEAD_IN 或冒号）；引号之后紧跟「这种写法」一类指称；
 * 紧跟在开括号之后、且括号之前的话在谈声明或措辞；与前一个已算引文的引号用列举连接。
 * 其余引号（单独成句的 "Rinne works in LUNA."、夹在断言里的引号）不是引文，里面的 strong 照判。
 */
export function citedSpans(sentence) {
    const out = [];
    for (const m of sentence.matchAll(/`[^`\n]+`/g)) out.push([m.index, m.index + m[0].length]);
    let prev = null; // 前一个引号：{ end, cited }
    for (const [a, b] of quotedSpans(sentence)) {
        const before = sentence.slice(0, a);
        const open = /[(（]\s*$/.test(before);
        const lead = before.replace(/[\s(（]+$/, "");
        const cited =
            CITE_LEAD_IN.test(lead) ||
            COLON_LEAD.test(lead) ||
            CITE_LEAD_OUT.test(sentence.slice(b)) ||
            (open && CLAIM_TALK.test(lead.slice(-80))) ||
            (prev !== null && prev.cited && QUOTE_LIST_GAP.test(sentence.slice(prev.end, a)));
        if (cited) out.push([a, b]);
        prev = { end: b, cited };
    }
    return out;
}

/** 短语按正则匹配，大小写不敏感。写坏的正则直接抛错——悄悄退回逐字匹配会让一条声明永远不命中。 */
export function matchPhrase(phrase, text) {
    if (typeof phrase !== "string" || !phrase) throw new Error("短语不是非空字符串");
    return new RegExp(phrase, "i").test(text);
}

/** 用来测「过于宽泛」的探针句：任何正常的 strong / hedge 短语都不该匹配它们。 */
const PROBES = ["a", "x y", "The quick brown fox.", "这是一个句子。"];

/** 一个 strong / hedge 短语的问题；没问题返回 null。 */
export function phraseProblem(phrase) {
    if (typeof phrase !== "string" || !phrase.trim()) return "不是非空字符串";
    let re;
    try {
        re = new RegExp(phrase, "i");
    } catch (e) {
        return `不是能编译的正则（${e.message}）`;
    }
    if (re.test("")) return "能匹配空串，会吞掉所有句子";
    if (PROBES.some((t) => re.test(t))) return "过于宽泛，能匹配任意句子";
    return null;
}

/**
 * 从短语（正则源码）生成几个能匹配它的样例串，用来测 hedge 与 strong 是否自相匹配。
 * 只认本检查的短语会用到的子集：字面、转义、字符类、分组、分支、量词（取最少与多一次两种）；
 * 生成的串必须真的匹配短语本身才保留，认不出的写法产不出样例，等于不参与自相匹配检查。
 */
export function phraseSamples(source, cap = 24) {
    let i = 0;
    const n = source.length;
    const cross = (a, b) => a.flatMap((x) => b.map((y) => x + y)).slice(0, cap);
    /** 字符类 [...]：取一个真的落在类里的字符。 */
    const classSample = () => {
        let j = i + 1;
        while (j < n && source[j] !== "]") j += source[j] === "\\" ? 2 : 1;
        const body = source.slice(i, j + 1);
        i = j + 1;
        let re;
        try {
            re = new RegExp(body);
        } catch {
            return [];
        }
        const literal = [...body.slice(1, -1).replace(/\\(.)/g, "$1")].filter((c) => c !== "^" && c !== "-");
        const hit = [...literal, "a", "0", " ", "x", "-"].find((c) => re.test(c));
        return hit === undefined ? [] : [hit];
    };
    const atom = () => {
        const c = source[i];
        if (c === "(") {
            i++;
            const look = /^\?(?:<[=!]|[=!])/.exec(source.slice(i, i + 4));
            if (look) i += look[0].length;
            else if (source.startsWith("?:", i)) i += 2;
            else if (source[i] === "?") i = source.indexOf(">", i) + 1;
            const inner = alt();
            if (source[i] === ")") i++;
            return look ? [""] : inner;
        }
        if (c === "[") return classSample();
        if (c === "\\") {
            const e = source[i + 1];
            i += 2;
            if (e === "b" || e === "B") return [""];
            if (e === "d") return ["0"];
            if (e === "w" || e === "S" || e === "D") return ["a"];
            if (e === "s") return [" "];
            if (e === "W") return ["-"];
            if (e === "n") return ["\n"];
            if (e === "t") return ["\t"];
            return [e ?? ""];
        }
        i++;
        if (c === "^" || c === "$") return [""];
        if (c === ".") return ["a"];
        return [c];
    };
    /** 量词：返回要试的重复次数（最少一次与多一次），没有量词返回 null。 */
    const quant = () => {
        const c = source[i];
        let range = null;
        if (c === "?" || c === "*") range = [0, 1];
        else if (c === "+") range = [1, 2];
        else if (c === "{") {
            const m = /^\{(\d+)(?:(,)(\d*))?\}/.exec(source.slice(i));
            if (!m) return null;
            const lo = Number(m[1]);
            const hi = m[2] ? (m[3] === "" ? lo + 1 : Number(m[3])) : lo;
            i += m[0].length - 1;
            range = [lo, Math.min(hi, lo + 1)];
        }
        if (!range) return null;
        i++;
        if (source[i] === "?") i++; // 惰性量词
        return range;
    };
    const seq = () => {
        let acc = [""];
        while (i < n && source[i] !== "|" && source[i] !== ")") {
            const a = atom();
            const r = quant();
            const reps = r ? [...new Set(r)] : [1];
            acc = cross(
                acc,
                reps.flatMap((k) => a.map((x) => x.repeat(k))),
            );
        }
        return acc;
    };
    function alt() {
        const out = [...seq()];
        while (source[i] === "|") {
            i++;
            out.push(...seq());
        }
        return out.slice(0, cap);
    }
    try {
        const re = new RegExp(source, "i");
        return alt().filter((t) => re.test(t));
    } catch {
        return [];
    }
}

/** hedge 与 strong 是否自相匹配：一边的典型命中本身就满足另一边。返回说明；没有问题返回 null。 */
export function selfMatchProblem(strong, hedge) {
    const s = new RegExp(strong, "i");
    const h = new RegExp(hedge, "i");
    const fromHedge = phraseSamples(hedge).find((t) => s.test(t));
    if (fromHedge !== undefined) return `hedge「${hedge}」的典型命中「${fromHedge}」本身就含有 strong「${strong}」`;
    const fromStrong = phraseSamples(strong).find((t) => h.test(t));
    if (fromStrong !== undefined) return `strong「${strong}」的典型命中「${fromStrong}」本身就满足 hedge「${hedge}」`;
    return null;
}

/**
 * 免责词表：hedge 短语的每个典型命中至少含其中一个，否则它不是限定或免责，只是另一个正面断言
 * （「passes auval and pluginval」放在 works in LUNA 的 hedge 里，同一句「works in LUNA and passes auval and pluginval」
 * 就被放行了）。词表按用途分三类，英文词带词界，中文不带：
 *   没验证过   not yet / untested / pending / awaiting / 尚未 / 未验证 / 待测、待验证、待确认、待上机、待实测
 *   按设计     designed / by design / intended / 按设计 / 设计上 / 旨在
 *   限定范围   built for / between / except / unless / until / 面向 / 之间 / 除非，
 *              以及带限定搭配的 only 与 if、仅（见下）
 * 单词太宽的不收：单字「待」会把期待、对待、待机放进来，只认「待」后面跟 测 / 验证 / 确认 / 上机 / 实测；
 * 裸的 only 与 if 是普通词（「works in LUNA only」没有说明任何限制），只认限定搭配——
 * only 后面跟 if / when / once / after / on / for（only on macOS 11+、only if tested），
 * if 前面不用 only 时只认 if tested / verified / confirmed / validated；中文的仅只认仅在 / 仅限 / 仅当 / 仅适用 / 仅用于
 * （不仅、仅仅不算）。要加词就改这里，评审看得见；词不能宽到匹配任意句子（自测拿探针句核对）。
 */
export const DISCLAIMER_TERMS = [
    "\\bnot yet\\b",
    "\\byet to\\b",
    "\\bnot (?:been )?(?:tested|verified|confirmed|validated)\\b",
    "\\bun(?:tested|verified|confirmed|validated)\\b",
    "\\bpending\\b",
    "\\bawait(?:s|ing|ed)?\\b",
    "\\bto be (?:tested|verified|confirmed|validated)\\b",
    "尚未",
    "未(?:经|能)?(?:测试|验证|确认)",
    "没有(?:测试|验证|确认)",
    "待(?:测|验证|确认|上机|实测)",
    "\\bdesigned\\b",
    "\\bby design\\b",
    "\\bintended\\b",
    "按设计",
    "设计上",
    "旨在",
    "\\bbuilt for\\b",
    "\\bbetween\\b",
    "\\bonly (?:if|when|once|after|on|for)\\b",
    "\\bexcept\\b",
    "\\bunless\\b",
    "\\buntil\\b",
    "\\bif (?:tested|verified|confirmed|validated)\\b",
    "面向",
    "之间",
    "(?<!不)仅(?:在|限|当|适用|用于)",
    "除非",
];
const DISCLAIMER_RE = new RegExp(DISCLAIMER_TERMS.join("|"), "i");

/** 免责词表是否宽到匹配任意句子（自测用）。 */
export const disclaimerTooBroad = () => PROBES.some((t) => DISCLAIMER_RE.test(t));

/**
 * hedge 短语是不是另一个正面断言：它的典型命中里有不含任何免责词的，就是。返回说明；没有问题返回 null。
 * 典型命中用 phraseSamples 生成（含「可选的免责」那一份不出现的写法）；认不出的写法产不出样例，
 * 退回看短语的字面文字。
 */
export function disclaimerProblem(hedge) {
    const samples = phraseSamples(hedge);
    const pool = samples.length ? samples : [hedge.replace(/\\(.)/g, "$1")];
    const bare = pool.find((t) => !DISCLAIMER_RE.test(t));
    return bare === undefined
        ? null
        : `hedge「${hedge}」的典型命中「${bare}」不含任何免责词（尚未 / pending / 按设计 / built for 等），它是另一个正面断言`;
}

/** 一个短语在文本里的全部命中区间 [起, 止)。 */
export function phraseSpans(phrase, text) {
    return [...text.matchAll(new RegExp(phrase, "gi"))]
        .filter((m) => m[0].length > 0)
        .map((m) => [m.index, m.index + m[0].length]);
}

/** scope 里的一项是否在扫描范围内（范围表里的文件，或 web/i18n/ 下的词条文件）。 */
const inScope = (f, scope = SCOPE) => typeof f === "string" && (scope.includes(f) || /^web\/i18n\/[^/]+\.js$/.test(f));

/**
 * 把一条 evidence / pending 拆成 { kind, body }；形态不对时返回 null（原因用 evidenceShapeProblem 取）。
 * glob 为真时 validation 的文件名允许用 *（只在最后一段里、不跨目录），给还不知道日期的上机记录用。
 * validation 与 golden 的路径只准是仓内的规范相对路径（repoPathProblem），validation 的文件名要以登记的系统后缀结尾。
 */
export function parseEvidence(item, { glob = false } = {}) {
    if (typeof item !== "string") return null;
    const i = item.indexOf(":");
    if (i < 0) return null;
    const kind = item.slice(0, i);
    const body = item.slice(i + 1);
    if (!body) return null;
    if (kind === "validation") {
        if (validationShapeProblem(body, { glob })) return null;
        return body.includes("*") ? { kind, body, glob: true } : { kind, body };
    }
    if (kind === "issue") return /^https:\/\/\S+$/.test(body) ? { kind, body } : null;
    if (kind === "ci") return /^[a-z0-9][a-z0-9_-]*$/.test(body) ? { kind, body } : null;
    if (kind === "golden") {
        if (!/^[^#\s]+\.json#\S+\s+(?:==|!=|>=|<=|>|<)\s+\S+$/.test(body)) return null;
        return repoPathProblem(body.split("#")[0]) ? null : { kind, body };
    }
    return null;
}

/** 一条 evidence / pending 形态不对的原因（说给人看）；形态没问题返回 null。 */
export function evidenceShapeProblem(item, { glob = false } = {}) {
    if (parseEvidence(item, { glob })) return null;
    if (typeof item === "string" && item.startsWith("validation:"))
        return validationShapeProblem(item.slice("validation:".length), { glob });
    if (typeof item === "string" && item.startsWith("golden:")) {
        const file = item.slice("golden:".length).split("#")[0];
        const why = repoPathProblem(file);
        return why ? `golden 目标文件路径${why}` : null;
    }
    return null;
}

/**
 * validation 记录是否存在；带 * 的按最后一段的文件名通配。
 * 路径先过 repoPathProblem（带仓根，规范化后、符号链接与 junction 解开后必须仍在仓根之内），不合格就当不存在，不去碰仓外的文件。
 */
export function validationExists(root, body, glob) {
    if (repoPathProblem(body, root)) return false;
    if (!glob) return fs.existsSync(path.join(root, body));
    const slash = body.lastIndexOf("/");
    const re = new RegExp(
        `^${body
            .slice(slash + 1)
            .split("*")
            .map((s) => s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&"))
            .join("[^/]*")}$`,
    );
    const dir = body.slice(0, slash);
    try {
        // 目录本身与匹配到的每一项都过一遍 repoPathProblem：目录是指向仓外的链接、或匹配项是指向仓外的链接，都不算
        return (
            !repoPathProblem(dir, root) &&
            fs.readdirSync(path.join(root, dir)).some((f) => re.test(f) && !repoPathProblem(`${dir}/${f}`, root))
        );
    } catch {
        return false;
    }
}

/** golden 谓词求值：body 是 "<file>#<path> <op> <value>"。返回 true / false / null（文件不在）。 */
export function evalGolden(root, body) {
    const [file, rest] = body.split("#");
    const m = /^(.*?)\s+(==|!=|>=|<=|>|<)\s+(\S+)$/.exec(rest ?? "");
    if (!m) return null;
    const [, pathExpr, op, raw] = m;
    if (repoPathProblem(file, root)) return null; // 仓外的文件不读
    const text = readText(path.join(root, file));
    if (text === null) return null;
    let data;
    try {
        data = JSON.parse(text);
    } catch {
        return false;
    }
    const value = resolvePath(data, pathExpr) ?? resolveParamPath(data, pathExpr);
    if (value === undefined) return false;
    const want = coerce(raw, value);
    switch (op) {
        case "==":
            return value === want;
        case "!=":
            return value !== want;
        case ">":
            return typeof value === "number" && typeof want === "number" && value > want;
        case "<":
            return typeof value === "number" && typeof want === "number" && value < want;
        case ">=":
            return typeof value === "number" && typeof want === "number" && value >= want;
        case "<=":
            return typeof value === "number" && typeof want === "number" && value <= want;
        default:
            return false;
    }
}
/**
 * 点路径取值：a.b[0].c 与 a.b.0.c 都支持；a.b[name=x].c 在数组 a.b 里选 name 等于 x 的那一项
 * （按字段值选，不依赖项在数组里的顺序，数组加一项、换个顺序都不会让谓词指到别的项）。
 */
function resolvePath(data, expr) {
    const parts = [];
    for (const m of expr.matchAll(/\[(\d+)\]|\[([^\]=\s]+)=([^\]\s]+)\]|([^.[\]]+)/g))
        parts.push(m[4] !== undefined ? m[4] : m[1] !== undefined ? m[1] : { field: m[2], value: m[3] });
    let cur = data;
    for (const p of parts) {
        if (cur === null || cur === undefined || typeof cur !== "object") return undefined;
        if (typeof p === "string") cur = cur[p];
        else if (Array.isArray(cur))
            cur = cur.find((x) => x && typeof x === "object" && String(x[p.field]) === p.value);
        else return undefined;
    }
    return cur;
}

/**
 * 参数集合里按 id 取一条参数。
 * golden 有两种形状：映射形（顶层键就是参数 id）与数组形（{"params":[{"id":…}]}）。
 * 两种形状下同一条谓词路径（如 feedback.default）都要取到同一个值，
 * 否则真源换个形状，同一条强声明就会无端判红。
 */
export function findParam(data, id) {
    if (!data || typeof data !== "object") return null;
    const direct = data[id];
    if (direct && typeof direct === "object" && !Array.isArray(direct)) return direct;
    const list = Array.isArray(data.params)
        ? data.params
        : Array.isArray(data.parameters)
          ? data.parameters
          : Object.entries(data)
                .filter(([, v]) => v && typeof v === "object" && !Array.isArray(v))
                // 键最后写、以键为准：条目里自己带的 id 字段不能盖掉键，与 params-table 的 paramsOf 同一个口径
                .map(([k, v]) => ({ ...v, id: k }));
    const hit = list.find((p) => p && typeof p === "object" && p.id === id);
    return hit ?? null;
}

/** 谓词路径的第一段是参数 id 时，改走参数集合（两种 golden 形状都认）。 */
function resolveParamPath(data, expr) {
    const dot = expr.indexOf(".");
    if (dot <= 0) return undefined;
    const id = expr.slice(0, dot);
    const rest = expr.slice(dot + 1);
    const param = findParam(data, id);
    if (!param) return undefined;
    return resolvePath(param, rest);
}

/**
 * 把字面量转成与实际值同类的值。布尔值认 true / false 与 1 / 0：
 * 参数表把「开」存成 true 还是 1，同一条谓词（== 1）都要成立。
 */
function coerce(raw, sample) {
    if (typeof sample === "number") {
        const n = Number(raw);
        return Number.isNaN(n) ? raw : n;
    }
    if (typeof sample === "boolean") {
        if (raw === "true" || raw === "1") return true;
        if (raw === "false" || raw === "0") return false;
        return raw;
    }
    return raw;
}

/** 仓内 .github/workflows 里的 job 名（用 workflows 检查的解析器，口径与它一致）。 */
export function ciJobs(root) {
    const dir = path.join(root, ".github", "workflows");
    const jobs = new Set();
    let files = [];
    try {
        files = fs.readdirSync(dir).filter((f) => f.endsWith(".yml"));
    } catch {
        return jobs;
    }
    for (const file of files) {
        const text = readText(path.join(dir, file));
        if (text === null) continue;
        for (const job of parseWorkflow(text).jobs) jobs.add(job.id);
    }
    return jobs;
}

/**
 * 一条 evidence 是否成立。返回 "ok" / "missing"（产物不在）/ "false"（谓词不成立）/ "malformed"。
 * glob 为真时 validation 允许文件名通配（只给 pending 用）。
 */
export function evidenceState(root, item, jobs, { glob = false } = {}) {
    const e = parseEvidence(item, { glob });
    if (!e) return "malformed";
    if (e.kind === "validation") return validationExists(root, e.body, e.glob) ? "ok" : "missing";
    if (e.kind === "issue") return "ok";
    if (e.kind === "ci") return jobs.has(e.body) ? "ok" : "false";
    if (e.kind === "golden") {
        const v = evalGolden(root, e.body);
        if (v === null) return "missing";
        return v ? "ok" : "false";
    }
    return "malformed";
}

const isRecord = (x) => typeof x === "string" && x.startsWith("validation:");

/** 宿主兼容类声明：pending / evidence 里有 validation 真机记录，或写了 hedgeWhilePending: true。不看 false，也不看字段有没有。 */
export function isHostClaim(c) {
    const items = [...(Array.isArray(c?.evidence) ? c.evidence : []), ...(Array.isArray(c?.pending) ? c.pending : [])];
    return c?.hedgeWhilePending === true || items.some(isRecord);
}

/** 显式豁免 hedge：hedgeWhilePending 写的是 false，并且同时写了非空的 hedgeExemptReason。 */
export function hedgeExempt(c) {
    return (
        c?.hedgeWhilePending === false && typeof c?.hedgeExemptReason === "string" && c.hedgeExemptReason.trim() !== ""
    );
}

/** 真机记录到位前是否要求 hedge：宿主兼容类，且没有显式豁免。 */
export const hedgeRequiredWhilePending = (c) => isHostClaim(c) && !hedgeExempt(c);

/**
 * 判据的实现。data 是 claims.json 解析后的对象；files 是 { 路径: 文本 }；root 是仓根（golden 与 ci 查表用）；
 * scope 是扫描范围（范围表里的文件清单，默认取仓里的范围表）。
 * 返回 { errors, infos }；errors 判红，infos 只列出待办。
 */
export function claimProblems({ data, files, root, jobs = ciJobs(root), scope: scanScope = SCOPE }) {
    const errors = [];
    const infos = [];
    const add = (rule, message) => {
        if (!Object.hasOwn(RULES, rule)) throw new Error(`未登记的判据 ${rule}`);
        errors.push(`[${rule}] ${message}`);
    };
    const note = (rule, message) => {
        if (!Object.hasOwn(RULES, rule)) throw new Error(`未登记的判据 ${rule}`);
        infos.push(`[${rule}] ${message}`);
    };

    if (!data || typeof data !== "object" || !Array.isArray(data.claims)) {
        add("claims-shape", `${CLAIMS_FILE} 缺 claims 数组`);
        return { errors, infos };
    }
    const ids = new Set();
    const unusable = new Set(); // 短语写坏的声明不参与后面的匹配，免得把正则错误当成扫描结果
    for (const c of data.claims) {
        const where = c?.id ?? "(没有 id)";
        if (typeof c?.id !== "string" || !ID_RE.test(c.id))
            add("claims-shape", `${CLAIMS_FILE} 的条目 id「${where}」不是小写连字符`);
        else if (ids.has(c.id)) add("claims-shape", `${CLAIMS_FILE} 的条目 id「${c.id}」重复`);
        else ids.add(c.id);
        if (!Array.isArray(c?.strong) || !c.strong.length)
            add("claims-shape", `${CLAIMS_FILE} 的条目「${where}」缺 strong 短语`);
        if (!Array.isArray(c?.hedge)) add("claims-shape", `${CLAIMS_FILE} 的条目「${where}」缺 hedge 短语`);
        if (!Array.isArray(c?.evidence)) add("claims-shape", `${CLAIMS_FILE} 的条目「${where}」缺 evidence 数组`);
        for (const [field, list] of [
            ["strong", c?.strong],
            ["hedge", c?.hedge],
        ])
            for (const phrase of Array.isArray(list) ? list : []) {
                const why = phraseProblem(phrase);
                if (why) {
                    add("claims-shape", `${CLAIMS_FILE} 的条目「${where}」的 ${field} 短语「${phrase}」${why}`);
                    unusable.add(c);
                }
            }
        // hedge 与 strong 自相匹配：免责句式本身含有强断言（或强断言本身满足免责），hedge 就不起作用
        for (const strong of Array.isArray(c?.strong) ? c.strong : [])
            for (const hedge of Array.isArray(c?.hedge) ? c.hedge : []) {
                if (phraseProblem(strong) || phraseProblem(hedge)) continue;
                const why = selfMatchProblem(strong, hedge);
                if (why) add("claims-shape", `${CLAIMS_FILE} 的条目「${where}」的${why}，hedge 形同虚设`);
            }
        // hedge 要是免责或限定语：不含任何免责词的正面断言放进 hedge，同一句里再写别的强断言就被放行了
        for (const hedge of Array.isArray(c?.hedge) ? c.hedge : []) {
            if (phraseProblem(hedge)) continue; // 写坏的短语上面已经报过
            const why = disclaimerProblem(hedge);
            if (why) add("claims-shape", `${CLAIMS_FILE} 的条目「${where}」的${why}`);
        }
        // hedgeWhilePending: false 是显式豁免，必须同时写非空的 hedgeExemptReason 说明为什么暂不要求；理由也只能跟 false 一起写
        const flag = c?.hedgeWhilePending;
        const reason = c?.hedgeExemptReason;
        if (flag !== undefined && typeof flag !== "boolean")
            add("claims-shape", `${CLAIMS_FILE} 的条目「${where}」的 hedgeWhilePending 不是布尔值`);
        const reasonOk = typeof reason === "string" && reason.trim() !== "";
        if (flag === false && !reasonOk)
            add(
                "claims-shape",
                `${CLAIMS_FILE} 的条目「${where}」写了 hedgeWhilePending: false（豁免 hedge）却没有非空的 hedgeExemptReason，` +
                    "豁免要写明为什么暂不要求",
            );
        else if (reason !== undefined && flag !== false)
            add(
                "claims-shape",
                `${CLAIMS_FILE} 的条目「${where}」写了 hedgeExemptReason 却没有 "hedgeWhilePending": false，理由没有对应的豁免`,
            );
        // 宿主兼容类声明：evidence 只认 validation 真机记录（不看开关，豁免 hedge 也不豁免这一条）。
        // 构建通过证明不了宿主可用；真机记录到位前放在 pending 里
        if (isHostClaim(c))
            for (const item of Array.isArray(c?.evidence) ? c.evidence : [])
                if (!isRecord(item))
                    add(
                        "host-claim-evidence",
                        `${CLAIMS_FILE} 的条目「${where}」是宿主兼容类声明（pending / evidence 里有 validation 记录，或写了 hedgeWhilePending），` +
                            `evidence「${item}」不是 validation 真机记录：构建 CI 与 golden 证明不了宿主可用，真机记录到位前放在 pending 里`,
                    );
        if (c?.scope !== undefined) {
            if (!Array.isArray(c.scope)) add("claims-shape", `${CLAIMS_FILE} 的条目「${where}」的 scope 不是数组`);
            else
                for (const f of c.scope)
                    if (!inScope(f, scanScope))
                        add(
                            "claims-shape",
                            `${CLAIMS_FILE} 的条目「${where}」的 scope「${f}」不在扫描范围内，写错了文件名？`,
                        );
        }
    }

    // 证据形态与成立情况
    const shapeWhy = (item, opts) => {
        const why = evidenceShapeProblem(item, opts);
        return why ? `：${why}` : "";
    };
    const covered = [];
    for (const c of data.claims) {
        const id = c?.id ?? "(没有 id)";
        if (!Array.isArray(c?.strong) || !Array.isArray(c?.evidence)) continue;
        let hasProof = false;
        for (const item of c.evidence) {
            const state = evidenceState(root, item, jobs);
            if (state === "malformed")
                add("evidence-kind", `${CLAIMS_FILE} 的条目「${id}」的 evidence「${item}」形态不认识${shapeWhy(item)}`);
            else if (state === "missing")
                add("evidence-satisfied", `${CLAIMS_FILE} 的条目「${id}」的 evidence「${item}」指向的产物不在仓内`);
            else if (state === "false")
                add(
                    "evidence-satisfied",
                    `${CLAIMS_FILE} 的条目「${id}」的 evidence「${item}」不成立，` +
                        `对应的强声明「${c.strong.join("」「")}」判红`,
                );
            else hasProof = true;
        }
        const pendings = Array.isArray(c.pending) ? c.pending : [];
        for (const item of pendings) {
            const state = evidenceState(root, item, jobs, { glob: true });
            if (state === "malformed")
                add(
                    "evidence-kind",
                    `${CLAIMS_FILE} 的条目「${id}」的 pending「${item}」形态不认识${shapeWhy(item, { glob: true })}`,
                );
            else if (state === "missing") note("pending-listed", `待办：${id} 的证据「${item}」尚未到位`);
            else if (state === "false")
                // 产物已经在仓内但谓词不成立：这条声明被证据反驳，判红
                add(
                    "evidence-satisfied",
                    `${CLAIMS_FILE} 的条目「${id}」的 pending「${item}」指向的产物已在仓内但不成立，` +
                        `对应的强声明「${c.strong.join("」「")}」判红`,
                );
            else note("pending-listed", `待办：${id} 的 pending「${item}」已经成立，请把它移到 evidence`);
        }
        // 要不要 hedge：有成立的 evidence 就不要；只有 pending 时直述允许，宿主兼容类（没有显式豁免的）除外
        const pendingOnly = !hasProof && pendings.length > 0;
        const holdsHedge = pendingOnly && hedgeRequiredWhilePending(c);
        if (holdsHedge && !(Array.isArray(c.hedge) && c.hedge.length))
            add(
                "claims-shape",
                `${CLAIMS_FILE} 的条目「${id}」是宿主兼容类声明，真机记录到位前要求 hedge，却没有 hedge 短语，没有句子能满足它；` +
                    "确有理由暂不要求的，写 hedgeWhilePending: false 并附 hedgeExemptReason",
            );
        if (unusable.has(c)) continue;
        covered.push({
            id,
            phrases: c.strong,
            hedges: c.hedge ?? [],
            needsHedge: !hasProof && (!pendings.length || holdsHedge),
            pendingOnly,
            c,
        });
    }

    /** 一个文件里的全部句子（JSON / JS 词条先取出字符串值）。解析失败的文件由 wording 的 scope-parse 报，这里按原文扫。 */
    const sentencesOf = (file, text) => {
        let units;
        try {
            units = proseUnits(file, text);
        } catch {
            units = [text];
        }
        return units.flatMap((u) => sentences(u));
    };

    // strong-needs-hedge：没有成立 evidence 又没有 pending 的声明，以及证据还在 pending 的宿主兼容类声明（没有显式豁免的）。
    // strong 的每一处命中，所在的分句（clauseSpans 的规则）里都要有 hedge 短语；同一句里别的分句有 hedge 不算
    for (const { id, phrases, hedges, needsHedge, pendingOnly, c } of covered) {
        if (!needsHedge) continue;
        const scope = c?.scope;
        for (const [file, text] of Object.entries(files)) {
            if (Array.isArray(scope) && scope.length && !scope.includes(file)) continue;
            for (const s of sentencesOf(file, text)) {
                // 引文不是断言：整个落在行内代码、或处在引用语境里的引号（写成 / 不要写 / 例如 / such as / e.g. / 冒号 /
                // 括号里举例，见 citedSpans）里的命中放行。引号前面没有引用语境的引号（单独成句的 "Rinne works in LUNA."）照判
                const cited = citedSpans(s);
                const quotes = quotedSpans(s);
                for (const phrase of phrases) {
                    const found = phraseSpans(phrase, s)
                        .filter(([a, b]) => !cited.some(([ca, cb]) => a >= ca && b <= cb))
                        .map(([a, b]) => {
                            const [from, to] = clauseAround(s, a, b);
                            return { a, b, clause: s.slice(from, to).trim() };
                        })
                        .find(({ clause }) => !hedges.some((h) => matchPhrase(h, clause)));
                    if (found === undefined) continue;
                    const bare = found.clause;
                    const unciting = quotes.some(([qa, qb]) => found.a >= qa && found.b <= qb)
                        ? "（它在引号里，但引号前没有引用语境，不算引文：引文要紧跟在写成 / 不要写 / 例如 / such as / e.g. / 冒号之后，或写在行内代码里）"
                        : "";
                    add(
                        "strong-needs-hedge",
                        pendingOnly
                            ? `${file} 里的强声明「${phrase}」（${id}）的真机记录还没有到位（pending），它所在的分句「${bare}」里要有 hedge 短语${unciting}；` +
                                  "记录到位后把它从 pending 移到 evidence"
                            : `${file} 里的强声明「${phrase}」（${id}）既没有成立的 evidence 也没有 pending，它所在的分句「${bare}」里要有 hedge 短语${unciting}`,
                    );
                }
            }
        }
    }

    // unregistered-absolute：绝对化句式没有被任何声明的 strong 覆盖
    for (const [file, text] of Object.entries(files)) {
        for (const s of sentencesOf(file, text)) {
            const spans = quotedSpans(s);
            for (const re of ABSOLUTE_PATTERNS) {
                for (const m of s.matchAll(new RegExp(re.source, re.flags.includes("g") ? re.flags : `${re.flags}g`))) {
                    // 引号里引用的句式、紧挨命中之前或之后的否定与免责：放行（口径与 wording 检查相同）
                    if (spans.some(([a, b]) => m.index >= a && m.index < b)) continue;
                    if (negatedAt(s, m.index, m[0].length)) continue;
                    // 声明的 scope 限定它管哪些文件；scope 外的文件不算这条声明覆盖到了
                    const covering = covered.filter((c) => {
                        const sc = c.c?.scope;
                        return !(Array.isArray(sc) && sc.length) || sc.includes(file);
                    });
                    // 覆盖按命中片段算：声明的 strong 命中要与这处绝对化句式重叠。
                    // 同一句里别处出现了某条声明的 strong，不能把这一处也当成已登记
                    const [from, to] = [m.index, m.index + m[0].length];
                    const covers = (c) => c.phrases.some((p) => phraseSpans(p, s).some(([a, b]) => a < to && b > from));
                    if (!covering.some(covers))
                        add("unregistered-absolute", `${file} 里的绝对化强断言「${m[0]}」没有登记在 ${CLAIMS_FILE}`);
                }
            }
        }
    }
    return { errors, infos };
}

export async function selfTest(root = ROOT) {
    // 临时目录的建与删都走 scripts/lib/safe-rm.mjs：只删本进程建出来的目录，删之前按真实路径重新核对
    const temps = [];
    const mkTemp = () => {
        const d = makeTempDir("rinne-claims-");
        temps.push(d);
        return d;
    };
    // 自测里建的链接：收尾时先摘链接，再删临时目录，删除不会经链接碰到链接指向的目录
    const links = [];
    const mkLink = (target, link) => {
        const ok = tryMakeDirLink(target, link);
        if (ok) links.push(link);
        return ok;
    };
    /**
     * 一个带 golden 的临时目录：golden 里的 feedback.default 可改。
     * shape 是 "map"（顶层键就是参数 id）或 "list"（{"params":[{"id":…}]}）——
     * 两种形状都要让同一条谓词路径取到同一个值，否则真源换个形状强声明就会无端判红。
     * limiter 是另一个参数，用来测布尔值与 1 / 0 的两种写法。
     */
    const withGolden = (value, shape = "map", limiter = true) => {
        const dir = mkTemp();
        fs.mkdirSync(path.join(dir, "tests", "golden"), { recursive: true });
        const body =
            shape === "list"
                ? {
                      params: [
                          { id: "feedback", default: value, min: 0, max: 100 },
                          { id: "limiter", default: limiter },
                      ],
                  }
                : { feedback: { default: value }, limiter: { default: limiter } };
        fs.writeFileSync(path.join(dir, "tests", "golden", "params-input.json"), JSON.stringify(body, null, 2));
        return dir;
    };
    const baseClaims = {
        version: 1,
        claims: [
            {
                id: "safe-defaults",
                scope: ["README.md"],
                strong: ["starts with feedback at zero"],
                hedge: ["designed so"],
                evidence: [],
                pending: ["golden:tests/golden/params-input.json#feedback.default == 0"],
            },
            {
                id: "auto-protection",
                scope: ["README.md"],
                strong: ["auto-protection"],
                hedge: ["designed so"],
                evidence: ["ci:core-sanitizers"],
                pending: [],
            },
        ],
    };
    const files = {
        "README.md": "Rinne starts with feedback at zero and has a limiter.\n",
        "README.zh-CN.md": "Rinne 默认反馈为 0。\n",
    };
    const jobs = new Set(["core-sanitizers"]);
    const goldenPred = "golden:tests/golden/params-input.json#feedback.default == 0";
    const withClaim = (over) => ({ ...baseClaims, claims: [{ ...baseClaims.claims[0], ...over }] });
    const onlyAuto = { version: 1, claims: [baseClaims.claims[1]] };

    const run = (over = {}) =>
        claimProblems({
            data: over.data ?? baseClaims,
            files: over.files ?? files,
            root: over.root ?? root,
            jobs: over.jobs ?? jobs,
            scope: over.scope,
        });
    /** 规则名 → 触发它的行（errors 与 infos 合在一起看）。 */
    const fired = (result) => [...result.errors, ...result.infos];

    let cases;
    try {
        const golden05 = withGolden(0.5);
        const golden0 = withGolden(0);
        const golden05List = withGolden(0.5, "list");
        const golden0List = withGolden(0, "list");
        const goldenMissing = mkTemp();
        const goldenLimiterBool = withGolden(0, "map", true);
        const goldenLimiterOff = withGolden(0, "list", false);
        // 映射形的条目里自带了和键不一样的 id：以键为准。ghost 只出现在条目的 id 字段里，没有叫 ghost 的键
        const withIdGolden = (body) => {
            const dir = mkTemp();
            fs.mkdirSync(path.join(dir, "tests", "golden"), { recursive: true });
            fs.writeFileSync(path.join(dir, "tests", "golden", "params-input.json"), JSON.stringify(body));
            return dir;
        };
        const idGhost = { feedback: { id: "ghost", default: 0 } };
        const idSwap = { feedback: { id: "limiter", default: 0.5 }, limiter: { default: 1 } };
        const goldenIdGhost = withIdGolden(idGhost);
        const goldenIdSwap = withIdGolden(idSwap);
        // 带一份 validation 记录的仓，用来测文件名通配
        const withRecord = mkTemp();
        fs.mkdirSync(path.join(withRecord, "docs", "validation"), { recursive: true });
        fs.writeFileSync(path.join(withRecord, "docs", "validation", "2026-11-02-cubase15-win.md"), "x\n");
        // 带 LUNA 真机记录的仓：记录到位后宿主兼容类声明才算有了证据
        const withLuna = mkTemp();
        fs.mkdirSync(path.join(withLuna, "docs", "validation"), { recursive: true });
        fs.writeFileSync(path.join(withLuna, "docs", "validation", "2026-11-03-luna-macos.md"), "x\n");
        const lunaRecord = "validation:docs/validation/2026-11-03-luna-macos.md";
        // 仓根之外有一份文件名合规的记录：用 .. 绕出去的路径在旧写法下会读到它、算作成立的证据
        const escapeBase = mkTemp();
        const escapeRepo = path.join(escapeBase, "repo");
        fs.mkdirSync(path.join(escapeRepo, "docs", "validation"), { recursive: true });
        fs.writeFileSync(path.join(escapeBase, "2026-11-03-luna-macos.md"), "x\n");
        fs.writeFileSync(path.join(escapeBase, "outside.json"), JSON.stringify({ a: 1 }));
        const escapedRecord = "validation:docs/../../2026-11-03-luna-macos.md";
        // 仓内的 junction（或符号链接）指向仓外：词法上 docs/validation/ext/… 在仓内，真实路径在仓外
        const linkOutside = mkTemp();
        fs.writeFileSync(path.join(linkOutside, "2026-11-03-luna-macos.md"), "x\n");
        fs.writeFileSync(path.join(linkOutside, "outside.json"), JSON.stringify({ a: 1 }));
        const outsideLink = path.join(escapeRepo, "docs", "validation", "ext");
        const outsideLinked = mkLink(linkOutside, outsideLink);
        // 仓内的链接指向仓内的另一个目录：真实路径仍在仓内，照常放行
        const insideLink = path.join(withLuna, "docs", "alias");
        const insideLinked = mkLink(path.join(withLuna, "docs", "validation"), insideLink);
        // 仓根本身是经链接到达的（工作树放在 junction 下）：仓根也取真实路径，仓内的文件不能因此被当成越界
        const rootLink = path.join(escapeBase, "repo-link");
        const rootLinked = mkLink(withLuna, rootLink);
        // state-schema 形状的 golden（cfgs.keys 是数组）：guard_on 的默认值可改，顺序可换，另有一份缺 guard_on 的
        const withCfgs = (guardDefault, { swap = false, noGuard = false } = {}) => {
            const dir = mkTemp();
            fs.mkdirSync(path.join(dir, "tests", "golden"), { recursive: true });
            const keys = [
                // 默认值故意与 guard_on 的任何取值（0 或 1）都不同：数组换了顺序时，按位置取会取到它而判错
                { key: 3, name: "reset_on_play", default: 2 },
                ...(noGuard ? [] : [{ key: 7, name: "guard_on", default: guardDefault }]),
            ];
            fs.writeFileSync(
                path.join(dir, "tests", "golden", "state-schema.json"),
                JSON.stringify({ cfgs: { keys: swap ? keys.reverse() : keys } }),
            );
            return dir;
        };
        const cfgsOn = withCfgs(1);
        const cfgsOff = withCfgs(0);
        const cfgsSwapped = withCfgs(1, { swap: true });
        const cfgsNoGuard = withCfgs(1, { noGuard: true });
        const guardPred = "golden:tests/golden/state-schema.json#cfgs.keys[name=guard_on].default == 1";
        // 宿主兼容类声明：真机记录没到位时直述也要同一句带 hedge
        const hostClaim = {
            id: "mac-hosts",
            scope: ["README.md"],
            strong: ["works in LUNA"],
            hedge: ["host testing pending"],
            hedgeWhilePending: true,
            evidence: [],
            pending: ["validation:docs/validation/*-luna-macos.md"],
        };
        const withHost = (over = {}) => ({ version: 1, claims: [{ ...hostClaim, ...over }] });
        // 显式豁免 hedge：hedgeWhilePending: false 加非空的理由，缺一不可
        const exemptReason = "真机实测前沿用现有措辞，文案在实测后统一修订";
        const exempt = { hedgeWhilePending: false, hedgeExemptReason: exemptReason };
        const zhHedge = "尚未[^，。\\n]{0,10}(?:测试|验证)";
        const withMinMacos = () =>
            withHost({
                id: "min-macos",
                strong: ["tested on macOS", "在 macOS [^。\\n]{0,20}上可用"],
                hedge: ["built for macOS 11\\+", "not yet tested", zhHedge],
            });
        // 文件名系统后缀写成 mac 的记录（文件真的存在）：后缀不是登记的写法，存在也不行
        const withMacRecord = mkTemp();
        fs.mkdirSync(path.join(withMacRecord, "docs", "validation"), { recursive: true });
        fs.writeFileSync(path.join(withMacRecord, "docs", "validation", "2026-11-03-luna-mac.md"), "x\n");
        // 每个分句界一个坏样例：strong 在界之前，免责语在界之后的另一个分句里
        const CLAUSE_BREAK_SAMPLES = [
            ...[
                ",",
                ":",
                " —",
                " –",
                " -",
                " --",
                ";",
                " while",
                " whereas",
                " but",
                " however",
                " although",
                " though",
            ].map((b) => [`英文「${b.trim()}」`, `Rinne works in LUNA${b} Pro Tools is not yet tested.\n`]),
            ...["，", "：", "但是", "但", "然而", "不过", "可是", "而"].map((b) => [
                `中文「${b}」`,
                `Rinne 在 LUNA 可用${b}Cubase 尚未测试。\n`,
            ]),
        ];
        // validation 证据的坏路径：.. 段、绝对路径、反斜杠、盘符、空段（文件名都带着合规的系统后缀，只测路径）
        const BAD_RECORD_PATHS = [
            ["docs/../../x", "validation:docs/../../x"],
            ["docs/../../x-win.md", "validation:docs/../../x-win.md"],
            ["docs/validation/../../../x-win.md", "validation:docs/validation/../../../x-win.md"],
            ["docs/validation/..", "validation:docs/validation/.."],
            ["/etc/x", "validation:/etc/x"],
            ["/etc/x-win.md", "validation:/etc/x-win.md"],
            ["/docs/validation/x-win.md", "validation:/docs/validation/x-win.md"],
            ["docs\\x", "validation:docs\\x"],
            ["docs\\validation\\x-win.md", "validation:docs\\validation\\x-win.md"],
            ["C:/x-win.md", "validation:C:/x-win.md"],
            ["C:\\x-win.md", "validation:C:\\x-win.md"],
            ["docs//validation/x-win.md", "validation:docs//validation/x-win.md"],
        ];
        // 窄的完整断言：只有整句断言才算命中，同一句里别的绝对化说法不被它放过
        const narrowClaims = {
            version: 1,
            claims: [
                {
                    id: "limiter-default-on",
                    scope: ["README.md"],
                    strong: ["limiter is on by default", "限制器默认开启"],
                    hedge: ["designed so"],
                    evidence: ["ci:core-sanitizers"],
                    pending: [],
                },
                {
                    id: "loop-delay-fixed",
                    scope: ["README.md"],
                    strong: ["delay does not change with"],
                    hedge: ["designed so"],
                    evidence: ["ci:core-sanitizers"],
                    pending: [],
                },
            ],
        };
        // 三种 workflow 写法：列 0 注释行夹在 jobs 下、job 键带行尾注释、下划线 job 名
        const wfRoot = mkTemp();
        fs.mkdirSync(path.join(wfRoot, ".github", "workflows"), { recursive: true });
        fs.writeFileSync(
            path.join(wfRoot, ".github", "workflows", "a.yml"),
            "name: x\non: push\njobs:\n  one:\n    runs-on: x\n# 列 0 的注释\n  two: # 行尾注释\n    runs-on: x\n\n  three_x:\n    runs-on: x\n",
        );
        const wfJobs = ciJobs(wfRoot);

        cases = [
            ["登记完整、证据成立", [], run()],
            [
                "golden 里的默认值改成 0.5",
                ["evidence-satisfied"],
                run({ root: golden05, data: withClaim({ pending: [], evidence: [goldenPred] }) }),
            ],
            [
                "golden 是数组形时默认值改成 0.5",
                ["evidence-satisfied"],
                run({ root: golden05List, data: withClaim({ pending: [], evidence: [goldenPred] }) }),
            ],
            [
                "golden 是数组形时默认值是 0",
                [],
                run({ root: golden0List, data: withClaim({ pending: [], evidence: [goldenPred] }) }),
            ],
            [
                "golden 里的默认值是 0",
                [],
                run({ root: golden0, data: withClaim({ pending: [], evidence: [goldenPred] }) }),
            ],
            [
                "布尔写法的 limiter 默认值满足 == 1",
                [],
                run({
                    root: goldenLimiterBool,
                    data: withClaim({
                        pending: [],
                        evidence: ["golden:tests/golden/params-input.json#limiter.default == 1"],
                    }),
                }),
            ],
            [
                "limiter 默认值是关时 == 1 不成立",
                ["evidence-satisfied"],
                run({
                    root: goldenLimiterOff,
                    data: withClaim({
                        pending: [],
                        evidence: ["golden:tests/golden/params-input.json#limiter.default == 1"],
                    }),
                }),
            ],
            ["证据产物还没进仓时列为待办", ["pending-listed"], run({ root: goldenMissing })],
            [
                "pending 的产物已在仓内但谓词不成立时判红",
                ["evidence-satisfied"],
                run({ root: golden05, data: withClaim({ pending: [goldenPred], evidence: [] }) }),
            ],
            [
                "pending 里的通配记录还没有对应文件：列为待办",
                ["pending-listed"],
                run({
                    root: goldenMissing,
                    data: withClaim({ pending: ["validation:docs/validation/*-cubase15-win.md"] }),
                }),
            ],
            [
                "pending 里的通配记录已有对应文件：提醒移到 evidence",
                ["pending-listed"],
                run({
                    root: withRecord,
                    data: withClaim({ pending: ["validation:docs/validation/*-cubase15-win.md"] }),
                }),
            ],
            [
                "evidence 里不许写通配",
                ["evidence-kind"],
                run({
                    root: withRecord,
                    data: withClaim({ pending: [], evidence: ["validation:docs/validation/*-cubase15-win.md"] }),
                }),
            ],
            [
                "claims.json 不是合法形状",
                ["claims-shape"],
                run({ data: { version: 1, claims: [{ id: "Bad Id", strong: [] }] } }),
            ],
            ["缺 claims 数组", ["claims-shape"], run({ data: { version: 1 } })],
            ["id 不是小写连字符（其余字段都合法）", ["claims-shape"], run({ data: withClaim({ id: "Bad Id" }) })],
            ["缺 strong", ["claims-shape"], run({ data: withClaim({ strong: undefined }) })],
            ["strong 是空数组", ["claims-shape"], run({ data: withClaim({ strong: [] }) })],
            ["缺 hedge", ["claims-shape"], run({ data: withClaim({ hedge: undefined }) })],
            ["缺 evidence", ["claims-shape"], run({ data: withClaim({ evidence: undefined }) })],
            ["scope 不是数组", ["claims-shape"], run({ data: withClaim({ scope: "README.md" }) })],
            [
                "evidence 指向不存在的 validation 记录",
                ["evidence-satisfied"],
                run({ data: withClaim({ pending: [], evidence: ["validation:docs/validation/nowhere-win.md"] }) }),
            ],
            ["pending 的形态不认识", ["evidence-kind"], run({ data: withClaim({ pending: ["wiki:x"] }) })],
            [
                "id 重复",
                ["claims-shape"],
                run({
                    data: { ...baseClaims, claims: [baseClaims.claims[0], { ...baseClaims.claims[0], scope: [] }] },
                }),
            ],
            [
                "strong 短语的正则没写完",
                ["claims-shape"],
                run({ data: withClaim({ strong: ["starts (with feedback"] }) }),
            ],
            ["strong 短语能匹配空串", ["claims-shape"], run({ data: withClaim({ strong: [".*"] }) })],
            ["hedge 短语宽到匹配任意句子", ["claims-shape"], run({ data: withClaim({ hedge: ["."] }) })],
            ["scope 里写错了文件名", ["claims-shape"], run({ data: withClaim({ scope: ["README.mdd"] }) })],
            [
                "scope 在 web/i18n 之内是合法的",
                [],
                run({ data: withClaim({ scope: ["README.md", "web/i18n/input.js"] }) }),
            ],
            [
                "evidence 的类型不认识",
                ["evidence-kind"],
                run({ data: { ...baseClaims, claims: [{ ...baseClaims.claims[1], evidence: ["wiki:something"] }] } }),
            ],
            [
                "ci 证据指向不存在的 job",
                ["evidence-satisfied"],
                run({ data: { ...baseClaims, claims: [{ ...baseClaims.claims[1], evidence: ["ci:no-such-job"] }] } }),
            ],
            [
                "强声明既无 evidence 也无 pending，且同句没有 hedge",
                ["strong-needs-hedge"],
                run({ data: withClaim({ pending: [], evidence: [] }) }),
            ],
            [
                "强声明既无证据也无 pending，但同句有 hedge",
                [],
                run({
                    data: withClaim({ pending: [], evidence: [] }),
                    files: { "README.md": "Rinne is designed so that it starts with feedback at zero.\n" },
                }),
            ],
            [
                "JSON 词条里的强声明同样要 hedge",
                ["strong-needs-hedge"],
                run({
                    data: withClaim({ pending: [], evidence: [], scope: ["docs/hard-rules.i18n.json"] }),
                    files: { "docs/hard-rules.i18n.json": '{"en":{"body":"Rinne starts with feedback at zero."}}' },
                }),
            ],
            [
                "绝对化强断言没有登记",
                ["unregistered-absolute"],
                run({ data: onlyAuto, files: { "README.md": "The loop delay is identical output every time.\n" } }),
            ],
            [
                "JSON 词条里的绝对化强断言没有登记",
                ["unregistered-absolute"],
                run({
                    data: onlyAuto,
                    files: { "docs/hard-rules.i18n.json": '{"en":{"body":"The delay never changes."}}' },
                }),
            ],
            [
                "声明的 scope 之外的文件不算被覆盖",
                ["unregistered-absolute"],
                run({ data: onlyAuto, files: { "docs/OTHER.md": "The loop delay is identical output every time.\n" } }),
            ],
            [
                "含否定的绝对化句式放行",
                [],
                run({ data: onlyAuto, files: { "README.md": "No software guarantees the loop will never howl.\n" } }),
            ],
            [
                "引号里引用的绝对化句式放行",
                [],
                run({ data: onlyAuto, files: { "README.md": "我们不写「完全一致」这种话。\n" } }),
            ],
            [
                "命中片段自带的否定词不算免责",
                ["unregistered-absolute"],
                run({ data: onlyAuto, files: { "README.md": "环路延迟不随缓冲大小变化。\n" } }),
            ],
            [
                "同句里远处的单字否定不放过强断言",
                ["unregistered-absolute"],
                run({ data: onlyAuto, files: { "README.md": "Rinne guarantees identical output, no exceptions.\n" } }),
            ],
            // 覆盖按命中片段算：同一句里别处出现某条声明的 strong，不能把另一处绝对化说法也放过
            [
                "同句里限制器默认开启，另一处写了「完全一致」：后者没登记，判红",
                ["unregistered-absolute"],
                run({ data: narrowClaims, files: { "README.md": "限制器默认开启，输出与播放完全一致。\n" } }),
            ],
            [
                "同句里 limiter is on by default，另一处写了 is identical to：后者没登记，判红",
                ["unregistered-absolute"],
                run({
                    data: narrowClaims,
                    files: { "README.md": "The limiter is on by default and the output is identical to playback.\n" },
                }),
            ],
            [
                "同一句里登记过一处 does not change with，另一处别的绝对化句式没登记：判红",
                ["unregistered-absolute"],
                run({
                    data: narrowClaims,
                    files: { "README.md": "The delay does not change with buffer size and the gain never changes.\n" },
                }),
            ],
            [
                "同一句里同一个句式出现两次，只有第一处登记过：第二处判红",
                ["unregistered-absolute"],
                run({
                    data: narrowClaims,
                    files: {
                        "README.md": "The delay does not change with size, and the gain does not change with level.\n",
                    },
                }),
            ],
            [
                "声明的 strong 命中紧挨着绝对化句式、没有重叠：不算覆盖",
                ["unregistered-absolute"],
                run({ data: narrowClaims, files: { "README.md": "限制器默认开启完全一致。\n" } }),
            ],
            // scope 取自参数（范围表），不取模块里的默认值
            [
                "声明的 scope 写了范围表里没有的文件：判红",
                ["claims-shape"],
                run({ data: withClaim({ scope: ["docs/INSTALL.md"] }), scope: ["README.md"] }),
            ],
            [
                "声明的 scope 写了范围表里有的文件：放行",
                [],
                run({ data: withClaim({ scope: ["docs/only-in-test.md"] }), scope: ["docs/only-in-test.md"] }),
            ],
            [
                "绝对化句式本身就是登记的 strong：覆盖成立",
                [],
                run({
                    data: narrowClaims,
                    files: { "README.md": "The loop delay does not change with buffer size.\n" },
                }),
            ],
            [
                "同一句里登记的绝对化句式与窄的完整断言各自覆盖：放行",
                [],
                run({
                    data: narrowClaims,
                    files: {
                        "README.md": "The delay does not change with buffer size and the limiter is on by default.\n",
                    },
                }),
            ],
            // 宿主兼容类声明（pending / evidence 里有 validation 真机记录）：真机记录没到位时，直述要在所在分句里带 hedge。
            // 这是默认口径，不用写 hedgeWhilePending；唯一的豁免是 hedgeWhilePending: false 加非空的 hedgeExemptReason
            [
                "hedgeWhilePending：真机记录没到位，直述判红",
                ["strong-needs-hedge"],
                run({ root: goldenMissing, data: withHost(), files: { "README.md": "Rinne works in LUNA.\n" } }),
            ],
            [
                "hedgeWhilePending：括号里带 hedge，同一分句放行",
                [],
                run({
                    root: goldenMissing,
                    data: withHost(),
                    files: { "README.md": "Rinne works in LUNA (host testing pending).\n" },
                }),
            ],
            [
                "没写 hedgeWhilePending、pending 里有 validation 记录：默认也要求 hedge，直述判红",
                ["strong-needs-hedge"],
                run({
                    root: goldenMissing,
                    data: withHost({ hedgeWhilePending: undefined }),
                    files: { "README.md": "Rinne works in LUNA.\n" },
                }),
            ],
            [
                "没写 hedgeWhilePending、pending 里有 validation 记录：同一分句带 hedge 放行",
                [],
                run({
                    root: goldenMissing,
                    data: withHost({ hedgeWhilePending: undefined }),
                    files: { "README.md": "Rinne works in LUNA (host testing pending).\n" },
                }),
            ],
            [
                "没写 hedgeWhilePending、pending 里有 validation 记录却没有 hedge 短语：形状错误",
                ["claims-shape"],
                run({ root: goldenMissing, data: withHost({ hedgeWhilePending: undefined, hedge: [] }) }),
            ],
            [
                "pending 里只有 golden 的声明不算宿主兼容类：没写 hedgeWhilePending 可以直述",
                [],
                run({
                    root: goldenMissing,
                    data: withClaim({ hedgeWhilePending: undefined }),
                    files: { "README.md": "Rinne starts with feedback at zero.\n" },
                }),
            ],
            [
                "hedgeWhilePending: false 且写了理由：显式豁免，可以直述",
                [],
                run({ root: goldenMissing, data: withHost(exempt), files: { "README.md": "Rinne works in LUNA.\n" } }),
            ],
            [
                "hedgeWhilePending: false 且写了理由、没有 hedge 短语：豁免了就不要求",
                [],
                run({
                    root: goldenMissing,
                    data: withHost({ ...exempt, hedge: [] }),
                    files: { "README.md": "Rinne works in LUNA.\n" },
                }),
            ],
            [
                "只写 hedgeWhilePending: false、不写理由：形状错误，豁免不生效，直述仍判红",
                ["claims-shape", "strong-needs-hedge"],
                run({
                    root: goldenMissing,
                    data: withHost({ hedgeWhilePending: false }),
                    files: { "README.md": "Rinne works in LUNA.\n" },
                }),
            ],
            ...[
                ["是空串", ""],
                ["只有空白", "  "],
                ["不是字符串", 1],
                ["是 null", null],
            ].map(([label, reason]) => [
                `hedgeExemptReason ${label}：形状错误，豁免不生效，直述仍判红`,
                ["claims-shape", "strong-needs-hedge"],
                run({
                    root: goldenMissing,
                    data: withHost({ hedgeWhilePending: false, hedgeExemptReason: reason }),
                    files: { "README.md": "Rinne works in LUNA.\n" },
                }),
            ]),
            [
                "只写 hedgeExemptReason、不写 hedgeWhilePending: false：形状错误，直述仍判红",
                ["claims-shape", "strong-needs-hedge"],
                run({
                    root: goldenMissing,
                    data: withHost({ hedgeWhilePending: undefined, hedgeExemptReason: exemptReason }),
                    files: { "README.md": "Rinne works in LUNA.\n" },
                }),
            ],
            [
                "hedgeWhilePending: true 又写了 hedgeExemptReason：形状错误，直述仍判红",
                ["claims-shape", "strong-needs-hedge"],
                run({
                    root: goldenMissing,
                    data: withHost({ hedgeExemptReason: exemptReason }),
                    files: { "README.md": "Rinne works in LUNA.\n" },
                }),
            ],
            [
                "hedgeWhilePending：真机记录移到 evidence 并成立后，直述放行",
                [],
                run({
                    root: withLuna,
                    data: withHost({ evidence: [lunaRecord], pending: [] }),
                    files: { "README.md": "Rinne works in LUNA.\n" },
                }),
            ],
            [
                "没写 hedgeWhilePending、真机记录在 evidence 并成立：直述放行",
                [],
                run({
                    root: withLuna,
                    data: withHost({ hedgeWhilePending: undefined, evidence: [lunaRecord], pending: [] }),
                    files: { "README.md": "Rinne works in LUNA.\n" },
                }),
            ],
            [
                "hedgeWhilePending：记录已在仓内却还留在 pending，仍要求 hedge",
                ["strong-needs-hedge", "pending-listed"],
                run({ root: withLuna, data: withHost(), files: { "README.md": "Rinne works in LUNA.\n" } }),
            ],
            [
                "宿主兼容类声明拿 ci 证据当证据：判红",
                ["host-claim-evidence"],
                run({ root: goldenMissing, data: withHost({ evidence: ["ci:core-sanitizers"] }) }),
            ],
            [
                "宿主兼容类声明拿 golden 证据当证据：判红",
                ["host-claim-evidence"],
                run({ root: cfgsOn, data: withHost({ evidence: [guardPred] }) }),
            ],
            // 宿主兼容类的认定不看 hedgeWhilePending 开关：pending / evidence 里有 validation 记录就是
            [
                "显式豁免 hedge、evidence 加回 ci 证据：仍判红（豁免不豁免证据口径）",
                ["host-claim-evidence"],
                run({ root: goldenMissing, data: withHost({ ...exempt, evidence: ["ci:core-sanitizers"] }) }),
            ],
            [
                "去掉 hedgeWhilePending 字段、evidence 加回 ci 证据：仍判红",
                ["host-claim-evidence"],
                run({
                    root: goldenMissing,
                    data: withHost({ hedgeWhilePending: undefined, evidence: ["ci:core-sanitizers"] }),
                }),
            ],
            [
                "显式豁免 hedge、evidence 加回 golden 证据：仍判红",
                ["host-claim-evidence"],
                run({ root: cfgsOn, data: withHost({ ...exempt, evidence: [guardPred] }) }),
            ],
            [
                "显式豁免 hedge、真机记录已在 evidence 却混着 ci 证据（pending 已清空）：仍判红",
                ["host-claim-evidence"],
                run({
                    root: withLuna,
                    data: withHost({ ...exempt, evidence: [lunaRecord, "ci:core-sanitizers"], pending: [] }),
                }),
            ],
            [
                "显式豁免 hedge、evidence 全是 validation 真机记录：放行",
                [],
                run({
                    root: withLuna,
                    data: withHost({ ...exempt, evidence: [lunaRecord], pending: [] }),
                    files: { "README.md": "Rinne works in LUNA.\n" },
                }),
            ],
            [
                "pending 与 evidence 里都没有 validation 记录的声明不算宿主兼容类：ci 证据照常放行",
                [],
                run({
                    root: goldenMissing,
                    data: withHost({
                        hedgeWhilePending: undefined,
                        evidence: ["ci:core-sanitizers"],
                        pending: ["golden:tests/golden/params-input.json#feedback.default == 0"],
                    }),
                }),
            ],
            ["hedgeWhilePending 不是布尔值", ["claims-shape"], run({ data: withHost({ hedgeWhilePending: "yes" }) })],
            [
                "宿主兼容类声明在真机记录到位前没有 hedge 短语",
                ["claims-shape"],
                run({ root: goldenMissing, data: withHost({ hedge: [] }) }),
            ],
            // hedge 要落在 strong 所在的分句里：别的分句里的免责语不算
            [
                "「works in LUNA, while Pro Tools is not yet tested」：免责语在 while 之后的另一个分句，判红",
                ["strong-needs-hedge"],
                run({
                    root: goldenMissing,
                    data: withHost({ hedge: ["host testing pending", "not yet tested"] }),
                    files: { "README.md": "Rinne works in LUNA, while Pro Tools is not yet tested.\n" },
                }),
            ],
            [
                "「works in LUNA, host testing pending」：免责语在逗号之后的另一个分句，判红",
                ["strong-needs-hedge"],
                run({
                    root: goldenMissing,
                    data: withHost(),
                    files: { "README.md": "Rinne works in LUNA, host testing pending.\n" },
                }),
            ],
            [
                "中文「在 LUNA 可用，而 Cubase 尚未测试」：免责语在而之后的另一个分句，判红",
                ["strong-needs-hedge"],
                run({
                    root: goldenMissing,
                    data: withHost({ strong: ["在 LUNA 可用"], hedge: [zhHedge] }),
                    files: { "README.md": "Rinne 在 LUNA 可用，而 Cubase 尚未测试。\n" },
                }),
            ],
            [
                "min-macos 同理：「tested on macOS 14, while Windows is not yet tested」判红",
                ["strong-needs-hedge"],
                run({
                    root: goldenMissing,
                    data: withMinMacos(),
                    files: { "README.md": "Rinne is tested on macOS 14, while Windows is not yet tested.\n" },
                }),
            ],
            [
                "min-macos 同理：「在 macOS 14 上可用，但 Windows 尚未验证」判红",
                ["strong-needs-hedge"],
                run({
                    root: goldenMissing,
                    data: withMinMacos(),
                    files: { "README.md": "Rinne 在 macOS 14 上可用，但 Windows 尚未验证。\n" },
                }),
            ],
            [
                "min-macos 同理：同一分句带 hedge 放行",
                [],
                run({
                    root: goldenMissing,
                    data: withMinMacos(),
                    files: {
                        "README.md":
                            "Rinne is tested on macOS 14 (built for macOS 11+), while Windows is not yet tested.\n",
                    },
                }),
            ],
            ...CLAUSE_BREAK_SAMPLES.map(([label, text, extra]) => [
                `分句界「${label}」：免责语在界之后的另一个分句，判红`,
                ["strong-needs-hedge"],
                run({
                    root: goldenMissing,
                    data: withHost({
                        strong: ["works in LUNA", "在 LUNA 可用"],
                        hedge: ["not yet tested", zhHedge],
                        ...extra,
                    }),
                    files: { "README.md": text },
                }),
            ]),
            // 逗号与冒号：只有两侧都是数字（千分位、时刻）才不切；一侧是数字照切
            [
                "min-macos：「tested on macOS 14, not yet tested on macOS 11」逗号左边是数字也切，第一处 strong 的分句没有 hedge，判红",
                ["strong-needs-hedge"],
                run({
                    root: goldenMissing,
                    data: withMinMacos(),
                    files: { "README.md": "Rinne is tested on macOS 14, not yet tested on macOS 11.\n" },
                }),
            ],
            [
                "「works in LUNA 3, not yet tested on Pro Tools」：数字后的逗号照切，免责语在另一个分句，判红",
                ["strong-needs-hedge"],
                run({
                    root: goldenMissing,
                    data: withHost({ hedge: ["host testing pending", "not yet tested"] }),
                    files: { "README.md": "Rinne works in LUNA 3, not yet tested on Pro Tools.\n" },
                }),
            ],
            [
                "「works in LUNA 3: not yet tested on Pro Tools」：数字后的冒号照切，判红",
                ["strong-needs-hedge"],
                run({
                    root: goldenMissing,
                    data: withHost({ hedge: ["host testing pending", "not yet tested"] }),
                    files: { "README.md": "Rinne works in LUNA 3: not yet tested on Pro Tools.\n" },
                }),
            ],
            [
                "「works in LUNA 3.1, host testing pending for the rest」：版本号后的逗号照切，判红",
                ["strong-needs-hedge"],
                run({
                    root: goldenMissing,
                    data: withHost(),
                    files: { "README.md": "Rinne works in LUNA 3.1, host testing pending for the rest.\n" },
                }),
            ],
            [
                "数字之间的逗号不切：「works in LUNA with 10,000 tracks not yet tested」hedge 仍在同一分句，放行",
                [],
                run({
                    root: goldenMissing,
                    data: withHost({ hedge: ["not yet tested"] }),
                    files: { "README.md": "Rinne works in LUNA with 10,000 tracks not yet tested.\n" },
                }),
            ],
            [
                "数字之间的冒号不切：「works in LUNA at 12:30 and is not yet tested」hedge 仍在同一分句，放行",
                [],
                run({
                    root: goldenMissing,
                    data: withHost({ hedge: ["not yet tested"] }),
                    files: { "README.md": "Rinne works in LUNA at 12:30 and is not yet tested.\n" },
                }),
            ],
            // 带空格的连字符与 -- 是分句界（wording 也把带空格的连字符当分句界）；词内的连字符不是
            [
                "「works in LUNA - not yet tested on Pro Tools」：带空格的连字符是分句界，判红",
                ["strong-needs-hedge"],
                run({
                    root: goldenMissing,
                    data: withHost({ hedge: ["host testing pending", "not yet tested"] }),
                    files: { "README.md": "Rinne works in LUNA - not yet tested on Pro Tools.\n" },
                }),
            ],
            [
                "「works in LUNA -- not yet tested on Pro Tools」：-- 是分句界，判红",
                ["strong-needs-hedge"],
                run({
                    root: goldenMissing,
                    data: withHost({ hedge: ["host testing pending", "not yet tested"] }),
                    files: { "README.md": "Rinne works in LUNA -- not yet tested on Pro Tools.\n" },
                }),
            ],
            [
                "词内的连字符不切：「works in LUNA with pre-release testing pending」放行",
                [],
                run({
                    root: goldenMissing,
                    data: withHost({ hedge: ["pre-release testing pending"] }),
                    files: { "README.md": "Rinne works in LUNA with pre-release testing pending.\n" },
                }),
            ],
            // 仓内的 junction / 符号链接指向仓外：词法上在仓内、真实路径在仓外的证据不算（需要本机能建 junction 或符号链接）
            ...(outsideLinked
                ? [
                      [
                          "validation 证据经仓内的 junction 指到仓外的真实文件（词法上在仓内）：不当证据（需要能建 junction）",
                          ["evidence-satisfied"],
                          run({
                              root: escapeRepo,
                              data: withHost({
                                  ...exempt,
                                  evidence: ["validation:docs/validation/ext/2026-11-03-luna-macos.md"],
                                  pending: [],
                              }),
                              files: { "README.md": "Rinne works in LUNA.\n" },
                          }),
                      ],
                  ]
                : []),
            ...(insideLinked
                ? [
                      [
                          "validation 证据经仓内的链接指到仓内另一个目录里的真实文件：照常成立（需要能建 junction）",
                          [],
                          run({
                              root: withLuna,
                              data: withHost({
                                  ...exempt,
                                  evidence: ["validation:docs/alias/2026-11-03-luna-macos.md"],
                                  pending: [],
                              }),
                              files: { "README.md": "Rinne works in LUNA.\n" },
                          }),
                      ],
                  ]
                : []),
            [
                "同一句里 strong 出现两处，只有第一处所在的分句带 hedge：第二处判红",
                ["strong-needs-hedge"],
                run({
                    root: goldenMissing,
                    data: withHost(),
                    files: { "README.md": "Rinne works in LUNA (host testing pending), and it works in LUNA.\n" },
                }),
            ],
            // 引文不是断言，但只有引号紧跟在引用语境之后才算引文；其余引号里的 strong 照判
            [
                "strong 整个在引号里（引用示例，引号前是 write）：放行",
                [],
                run({
                    root: goldenMissing,
                    data: withHost(),
                    files: { "README.md": 'Never write "works in LUNA" without a note.\n' },
                }),
            ],
            [
                "strong 整个在「」里（中文引用示例）：放行",
                [],
                run({
                    root: goldenMissing,
                    data: withHost({ strong: ["在 LUNA 可用"], hedge: [zhHedge] }),
                    files: { "README.md": "我们不写「在 LUNA 可用」这种话。\n" },
                }),
            ],
            // 引文放行只认两处：行内代码，以及紧跟在引用语境之后的引号（citedSpans）。下面每个引用语境各一条正例
            [
                "引文放行：CONTRIBUTING.md 里的写法（括号里举例，括号前在谈 claims，第二个引号用列举连接第一个）",
                [],
                run({
                    root: goldenMissing,
                    data: withHost({ strong: ["does not change with", "safe by default"], hedge: ["designed so"] }),
                    files: {
                        "README.md":
                            'Strong claims about behaviour ("does not change with...", "safe by default") need evidence registered in\ndocs/claims.json.\n',
                    },
                }),
            ],
            [
                "引文放行：只有第二个引号里有 strong，它靠列举连接前一个已算引文的引号",
                [],
                run({
                    root: goldenMissing,
                    data: withHost({ strong: ["safe by default"], hedge: ["designed so"] }),
                    files: {
                        "README.md":
                            'Strong claims about behaviour ("does not change with..." or "safe by default") need evidence.\n',
                    },
                }),
            ],
            ...[
                "such as",
                "e.g.",
                "i.e.",
                "for example",
                "for instance",
                "like",
                "write",
                "writes",
                "writing",
                "written",
                "wrote",
                "say",
                "says",
                "saying",
                "said",
                "avoid",
                "avoids",
                "avoiding",
                "quote",
                "quotes",
                "quoted",
                "quoting",
                "phrase",
                "phrases",
                "wording",
                "word",
                "words",
                "term",
                "terms",
            ].map((w) => [
                `引文放行：引号紧跟在「${w}」之后`,
                [],
                run({
                    root: goldenMissing,
                    data: withHost(),
                    files: { "README.md": `Docs ${w} "Rinne works in LUNA" must carry a note.\n` },
                }),
            ]),
            ...[
                "写成",
                "写作",
                "写",
                "说成",
                "说",
                "避免",
                "例如",
                "比如",
                "譬如",
                "诸如",
                "如",
                "用语",
                "措辞",
                "字样",
            ].map((w) => [
                `引文放行（中文）：引号紧跟在「${w}」之后`,
                [],
                run({
                    root: goldenMissing,
                    data: withHost({ strong: ["在 LUNA 可用"], hedge: [zhHedge] }),
                    files: { "README.md": `文档里${w}「在 LUNA 可用」要带说明。\n` },
                }),
            ]),
            ...[
                ["英文冒号", 'Never use this wording: "Rinne works in LUNA."', "Rinne works in LUNA"],
                ["英文冒号加弯引号", "Never use this wording: “Rinne works in LUNA.”", "Rinne works in LUNA"],
                ["中文冒号", "不要这样写：「在 LUNA 可用」。", "在 LUNA 可用"],
                [
                    "开括号之后（e.g. 在括号里）",
                    'Wording (e.g. "Rinne works in LUNA") needs a note.',
                    "Rinne works in LUNA",
                ],
                ["中文开括号之后", "比如（「在 LUNA 可用」）要带说明。", "在 LUNA 可用"],
            ].map(([label, text, strong]) => [
                `引文放行：${label}`,
                [],
                run({
                    root: goldenMissing,
                    data: withHost({ strong: [strong], hedge: [zhHedge, "host testing pending"] }),
                    files: { "README.md": `${text}\n` },
                }),
            ]),
            ...["这种", "这样", "这类", "此类", "这句"].flatMap((lead) =>
                ["写法", "说法", "话", "措辞", "用语", "字样", "表述"].map((tail) => [
                    `引文放行（中文）：引号后面直接跟着「${lead}${tail}」`,
                    [],
                    run({
                        root: goldenMissing,
                        data: withHost({ strong: ["在 LUNA 可用"], hedge: [zhHedge] }),
                        files: { "README.md": `我们认为「在 LUNA 可用」${lead}${tail}要带说明。\n` },
                    }),
                ]),
            ),
            // 括号里举例：括号前面的话在谈声明或措辞，用的词放在远处，避免被紧挨着的引用语境盖住
            ...["claim", "claims", "phrase", "phrases", "wording", "statement", "statements"].map((w) => [
                `引文放行：开括号之后，括号前在谈「${w}」`,
                [],
                run({
                    root: goldenMissing,
                    data: withHost(),
                    files: { "README.md": `Strong ${w} about behaviour ("Rinne works in LUNA") need evidence.\n` },
                }),
            ]),
            ...["声明", "说法", "措辞", "写法", "用语", "字样", "短语"].map((w) => [
                `引文放行（中文）：开括号之后，括号前在谈「${w}」`,
                [],
                run({
                    root: goldenMissing,
                    data: withHost({ strong: ["在 LUNA 可用"], hedge: [zhHedge] }),
                    files: { "README.md": `关于${w}的行为（「在 LUNA 可用」）要有证据。\n` },
                }),
            ]),
            // 列举：第二个引号靠前一个已算引文的引号带入，两个引号之间只能是列举连接
            ...[
                [", ", "英文逗号"],
                ["，", "中文逗号"],
                ["、", "顿号"],
                [" or ", "or"],
                [" and ", "and"],
                [", or ", "逗号加 or"],
                ["或", "或"],
                ["或者", "或者"],
                ["和", "和"],
            ].map(([gap, label]) => [
                `引文放行：列举里的第二个引号（${label}）由前一个引文带入`,
                [],
                run({
                    root: goldenMissing,
                    data: withHost({ strong: ["works in LUNA"], hedge: ["host testing pending"] }),
                    files: { "README.md": `Never write "Rinne is great"${gap}"Rinne works in LUNA" in the docs.\n` },
                }),
            ]),
            [
                "引文放行：行内代码里的命中（不带引号）",
                [],
                run({
                    root: goldenMissing,
                    data: withHost(),
                    files: { "README.md": "Do not ship the string `Rinne works in LUNA` before testing.\n" },
                }),
            ],
            [
                "引文放行：行内代码里的中文命中",
                [],
                run({
                    root: goldenMissing,
                    data: withHost({ strong: ["在 LUNA 可用"], hedge: [zhHedge] }),
                    files: { "README.md": "不写 `Rinne 在 LUNA 可用`。\n" },
                }),
            ],
            // 引号前面没有引用语境：不是引文，里面的 strong 照判
            [
                "引号里的强声明单独成句（前面没有引用语境）：判红，报错点明引号前没有引用语境",
                ["strong-needs-hedge"],
                (() => {
                    const r = run({
                        root: goldenMissing,
                        data: withHost(),
                        files: { "README.md": '"Rinne works in LUNA."\n' },
                    });
                    // 提示串写进了报错行：缺它就看不出为什么引号没放行
                    return r.errors.some((l) => l.includes("没有引用语境")) ? r : { errors: [], infos: [] };
                })(),
            ],
            ...[
                ['"Rinne works in LUNA."', "引号开头的整句"],
                ['Rinne "works in LUNA".', "夹在断言里的引号"],
                ['Rinne is great and "works in LUNA" too.', "and 之后的引号"],
                ['Rinne ("works in LUNA") is great.', "括号里的引号，括号之前不在谈声明或措辞"],
                ['Never write it that way, Rinne is "works in LUNA".', "引用语境离引号太远"],
                ['"Rinne is great", "Rinne works in LUNA"', "列举连接的前一个引号不是引文"],
                ['Never write "Rinne is great" but Rinne "works in LUNA".', "前一个是引文，但两个引号之间不是列举连接"],
                ['Never write "Rinne is great", so Rinne "works in LUNA".', "前一个是引文，两个引号之间隔着别的话"],
                ["Rinne is great, and “Rinne works in LUNA.”", "弯引号开头也一样"],
                ["The page says nothing. “Rinne works in LUNA.”", "上一句里的语境不管到下一句"],
                [
                    "Rinne works well e.g. in Cubase. “Rinne works in LUNA.”",
                    "e.g. 后面的句点不切，但上一句的语境仍不管到下一句",
                ],
                ["他们认为 Rinne「在 LUNA 可用」。", "夹在断言里的中文引号"],
                ["「Rinne 在 LUNA 可用。」", "中文引号开头的整句"],
                ["不如「Rinne 在 LUNA 可用」。", "不如的「如」不算引用语境"],
                ["假如「Rinne 在 LUNA 可用」。", "假如的「如」不算引用语境"],
                ["「在 LUNA 可用」这个 Rinne 很好。", "引号后面不是指称写法的话"],
            ].map(([text, label]) => [
                `引号里的强声明但引号前没有引用语境：判红（${label}）`,
                ["strong-needs-hedge"],
                run({
                    root: goldenMissing,
                    data: withHost({
                        strong: ["works in LUNA", "在 LUNA 可用"],
                        hedge: ["host testing pending", zhHedge],
                    }),
                    files: { "README.md": `${text}\n` },
                }),
            ]),
            [
                "引号在引用语境之后，但命中跨出了引号一部分（引号里只有「在 macOS 14」，「上可用」在引号外）：仍判红",
                ["strong-needs-hedge"],
                run({
                    root: goldenMissing,
                    data: withMinMacos(),
                    files: { "README.md": "例如「在 macOS 14」上可用。\n" },
                }),
            ],
            [
                "strong 只有一部分在引号里：仍判红",
                ["strong-needs-hedge"],
                run({
                    root: goldenMissing,
                    data: withMinMacos(),
                    files: { "README.md": 'Rinne 在 macOS "14 上可用"。\n' },
                }),
            ],
            [
                "同一句里引号外还有一处 strong：引号外那处仍判红",
                ["strong-needs-hedge"],
                run({
                    root: goldenMissing,
                    data: withHost(),
                    files: { "README.md": 'Rinne works in LUNA, and we never say "works in LUNA" alone.\n' },
                }),
            ],
            [
                "括号里带 hedge、括号内有逗号：仍算括号所在的分句，放行",
                [],
                run({
                    root: goldenMissing,
                    data: withHost(),
                    files: { "README.md": "Rinne works in LUNA (see the notes, host testing pending).\n" },
                }),
            ],
            [
                "中文括号里带 hedge、括号内有逗号：放行",
                [],
                run({
                    root: goldenMissing,
                    data: withHost({ strong: ["在 LUNA 可用"], hedge: [zhHedge] }),
                    files: { "README.md": "Rinne 在 LUNA 可用（见说明，尚未测试）。\n" },
                }),
            ],
            [
                "括号没配平时按没有括号处理：免责语在逗号之后，判红",
                ["strong-needs-hedge"],
                run({
                    root: goldenMissing,
                    data: withHost(),
                    files: { "README.md": "Rinne works in LUNA (see the notes, host testing pending.\n" },
                }),
            ],
            [
                "strong 与 hedge 在同一分句、转折连词在 strong 之后：放行",
                [],
                run({
                    root: goldenMissing,
                    data: withHost({ strong: ["delay does not change with"], hedge: ["designed so"] }),
                    files: {
                        "README.md":
                            "Rinne is designed so that the delay does not change with buffer size while you play.\n",
                    },
                }),
            ],
            [
                "转折连词把 strong 与前面的无关分句隔开：strong 自己的分句带 hedge 放行",
                [],
                run({
                    root: goldenMissing,
                    data: withHost({ hedge: ["not yet tested", "host testing pending"] }),
                    files: {
                        "README.md": "Pro Tools is not yet tested, but Rinne works in LUNA (host testing pending).\n",
                    },
                }),
            ],
            [
                "not yet 里的 yet 不是转折连词：同一分句放行",
                [],
                run({
                    root: goldenMissing,
                    data: withHost({ hedge: ["not yet tested"] }),
                    files: { "README.md": "Rinne works in LUNA which is not yet tested.\n" },
                }),
            ],
            [
                "并列的 and 不切分句：同一分句放行",
                [],
                run({
                    root: goldenMissing,
                    data: withHost({ hedge: ["not yet tested"] }),
                    files: { "README.md": "Rinne works in LUNA and Cubase and is not yet tested.\n" },
                }),
            ],
            [
                "中文并列的「而且」不切分句：同一分句放行",
                [],
                run({
                    root: goldenMissing,
                    data: withHost({ strong: ["在 LUNA 可用"], hedge: [zhHedge] }),
                    files: { "README.md": "Rinne 在 LUNA 可用而且尚未测试。\n" },
                }),
            ],
            // strong 的命中本身跨过分句界时，跨到的每个分句合起来算它所在的区间
            [
                "strong 的命中跨过逗号，hedge 在跨到的第二个分句里：放行",
                [],
                run({
                    root: goldenMissing,
                    data: withHost({ strong: ["不随[^。\\n]{0,20}变化"], hedge: ["按设计"] }),
                    files: { "README.md": "环路延迟不随缓冲大小，按设计也不随导出方式变化。\n" },
                }),
            ],
            [
                "strong 的命中跨过逗号，hedge 在命中之外的第三个分句里：判红",
                ["strong-needs-hedge"],
                run({
                    root: goldenMissing,
                    data: withHost({ strong: ["不随[^。\\n]{0,20}变化"], hedge: ["按设计"] }),
                    files: { "README.md": "环路延迟不随缓冲大小，导出方式变化，按设计。\n" },
                }),
            ],
            [
                "时间里的冒号不切分句：同一分句放行",
                [],
                run({
                    root: goldenMissing,
                    data: withHost(),
                    files: { "README.md": "Rinne works in LUNA since 10:30 (host testing pending).\n" },
                }),
            ],
            [
                "数字里的逗号不切分句：同一分句放行",
                [],
                run({
                    root: goldenMissing,
                    data: withHost(),
                    files: { "README.md": "Rinne works in LUNA at 1,024 samples (host testing pending).\n" },
                }),
            ],
            // validation 证据的路径只准是仓内的规范相对路径：用 .. 绕出仓根、绝对路径、反斜杠、盘符都是形状错误
            [
                "validation 证据用 .. 绕到仓根之外的真实文件：形状错误，不当证据（旧写法会读到它）",
                ["evidence-kind"],
                run({
                    root: escapeRepo,
                    data: withHost({ ...exempt, evidence: [escapedRecord], pending: [] }),
                    files: { "README.md": "Rinne works in LUNA.\n" },
                }),
            ],
            [
                "validation 的 pending 用 .. 绕出去：形状错误",
                ["evidence-kind"],
                run({
                    root: escapeRepo,
                    data: withHost({ pending: ["validation:docs/../*-luna-macos.md"] }),
                }),
            ],
            ...BAD_RECORD_PATHS.map(([label, item]) => [
                `validation 证据「${label}」：形状错误`,
                ["evidence-kind"],
                run({ root: goldenMissing, data: withHost({ ...exempt, evidence: [item], pending: [] }) }),
            ]),
            [
                "golden 证据的目标文件用 .. 绕出仓根：形状错误",
                ["evidence-kind"],
                run({
                    root: goldenMissing,
                    data: withClaim({ pending: [], evidence: ["golden:tests/../../x.json#feedback.default == 0"] }),
                }),
            ],
            [
                "validation 证据路径里的 . 段与空段不是规范形式：形状错误",
                ["evidence-kind"],
                run({
                    root: withLuna,
                    data: withHost({
                        ...exempt,
                        evidence: ["validation:docs/./validation/2026-11-03-luna-macos.md"],
                        pending: [],
                    }),
                }),
            ],
            [
                "validation 证据是仓内的规范路径、文件存在：放行",
                [],
                run({
                    root: withLuna,
                    data: withHost({ ...exempt, evidence: [lunaRecord], pending: [] }),
                    files: { "README.md": "Rinne works in LUNA.\n" },
                }),
            ],
            // 记录文件名的系统后缀只有一种写法：win / macos / linux
            ...[
                ["mac", "validation:docs/validation/*-luna-mac.md"],
                ["windows", "validation:docs/validation/*-cubase-pro-15-windows.md"],
                ["osx", "validation:docs/validation/*-luna-osx.md"],
                ["没有系统后缀", "validation:docs/validation/*-luna.md"],
                ["后缀大小写不同", "validation:docs/validation/*-luna-MacOS.md"],
            ].map(([label, item]) => [
                `pending 的记录文件名系统后缀写成「${label}」：形状错误`,
                ["evidence-kind"],
                run({ root: goldenMissing, data: withHost({ pending: [item] }) }),
            ]),
            [
                "evidence 的记录文件名系统后缀写成 mac（文件存在也不行）：形状错误",
                ["evidence-kind"],
                run({
                    root: withMacRecord,
                    data: withHost({
                        ...exempt,
                        evidence: ["validation:docs/validation/2026-11-03-luna-mac.md"],
                        pending: [],
                    }),
                }),
            ],
            ...[
                ["win", "validation:docs/validation/*-cubase-pro-15-win.md"],
                ["macos", "validation:docs/validation/*-luna-macos.md"],
                ["linux", "validation:docs/validation/*-reaper-linux.md"],
            ].map(([label, item]) => [
                `pending 的记录文件名系统后缀写成「${label}」：放行`,
                [],
                run({ root: goldenMissing, data: withHost({ pending: [item] }), files: { "README.md": "x.\n" } }),
            ]),
            // hedge 不能是不含任何免责词的正面断言
            [
                "hedge 是另一个正面断言（passes auval and pluginval）",
                ["claims-shape"],
                run({ data: withHost({ hedge: ["host testing pending", "passes auval and pluginval"] }) }),
            ],
            [
                "hedge 是另一个正面断言（中文：通过 auval 与 pluginval）",
                ["claims-shape"],
                run({ data: withHost({ hedge: ["尚未[^，。\\n]{0,10}(?:测试|验证)", "通过 auval 与 pluginval"] }) }),
            ],
            [
                "hedge 的免责是可选的（不写免责词时就是正面断言）",
                ["claims-shape"],
                run({ data: withHost({ hedge: ["(?:not yet )?passes auval"] }) }),
            ],
            [
                "hedge 的一个分支是正面断言",
                ["claims-shape"],
                run({ data: withHost({ hedge: ["not yet tested|also works in Cubase"] }) }),
            ],
            [
                "同一句「works in LUNA and passes auval and pluginval」不因 hedge 里没有正面断言而放行",
                ["strong-needs-hedge"],
                run({
                    root: goldenMissing,
                    data: withHost(),
                    files: { "README.md": "Rinne works in LUNA and passes auval and pluginval.\n" },
                }),
            ],
            [
                "同一句中文「在 LUNA 可用，通过 auval 与 pluginval」同样不放行",
                ["strong-needs-hedge"],
                run({
                    root: goldenMissing,
                    data: withHost({
                        strong: ["works in LUNA", "在 LUNA 可用"],
                        hedge: ["host testing pending", "尚未[^，。\\n]{0,10}(?:测试|验证)"],
                    }),
                    files: { "README.md": "Rinne 在 LUNA 可用，通过 auval 与 pluginval。\n" },
                }),
            ],
            [
                "hedge 都是真正的免责或限定语（尚未验证、pending、awaiting、按设计、built for）：放行",
                [],
                run({
                    data: withHost({
                        hedge: [
                            "host testing pending",
                            "not yet tested",
                            "awaiting",
                            "尚未[^，。\\n]{0,10}(?:测试|验证)",
                            "designed so",
                            "按设计",
                            "built for macOS 11\\+",
                        ],
                    }),
                }),
            ],
            // hedge 与 strong 自相匹配。两个方向的夹具都带着免责词（pending / only on），只触发自相匹配这一条，
            // 不与「hedge 不能是正面断言」混在一起
            [
                "hedge 的典型命中本身含 strong（tested on macOS [0-9]+ pending）",
                ["claims-shape"],
                run({
                    data: withHost({
                        id: "min-macos",
                        strong: ["tested on macOS"],
                        hedge: ["built for macOS 11\\+", "tested on macOS [0-9]+ pending"],
                    }),
                }),
            ],
            [
                "strong 的典型命中本身满足 hedge（works in LUNA only on macOS 满足 only on macOS）",
                ["claims-shape"],
                run({
                    data: withHost({ strong: ["works in LUNA only on macOS"], hedge: ["only on macOS"] }),
                }),
            ],
            // 免责词表收窄：单字「待」、裸的 only 与 if 不算免责，hedge 里只有它们时就是另一个正面断言
            ...[
                ["期待", "期待"],
                ["期待…可用", "期待[^。]{0,6}可用"],
                ["待机", "待机"],
                ["对待", "对待"],
                ["works in LUNA only（only 在句尾，没有限定搭配）", "available only"],
                ["裸的 if", "if you like"],
                ["不仅在（不仅不是仅在）", "不仅在"],
            ].map(([label, hedge]) => [
                `hedge「${label}」不算免责：形状错误`,
                ["claims-shape"],
                run({ data: withHost({ hedge: ["host testing pending", hedge] }) }),
            ]),
            ...[
                ["待测试", "待测试"],
                ["待验证", "待验证"],
                ["待确认", "待确认"],
                ["待上机", "待上机"],
                ["待实测", "待实测"],
                ["only on", "only on macOS 11\\+"],
                ["only if", "only if tested"],
                ["only when", "only when verified"],
                ["if tested", "if tested"],
                ["仅在", "仅在实测后"],
                ["仅限", "仅限 macOS 11"],
            ].map(([label, hedge]) => [
                `hedge「${label}」是真正的免责或限定：放行`,
                [],
                run({ data: withHost({ hedge: ["host testing pending", hedge] }) }),
            ]),
            [
                "旧写法 hedge「tested on macOS [0-9]」既自相匹配又不含免责词",
                ["claims-shape"],
                run({
                    data: withHost({
                        id: "min-macos",
                        strong: ["tested on macOS"],
                        hedge: ["built for macOS 11\\+", "tested on macOS [0-9]"],
                    }),
                }),
            ],
            [
                "hedge 是真正的免责或限定语：与 strong 不自相匹配",
                [],
                run({
                    data: withHost({
                        id: "min-macos",
                        strong: ["tested on macOS", "在 macOS [^。\\n]{0,20}上可用"],
                        hedge: ["built for macOS 11\\+", "not yet tested", "尚未[^，。\\n]{0,10}(?:测试|验证)"],
                    }),
                }),
            ],
            // golden 谓词按字段值选数组项
            [
                "按 name 选数组项：guard_on 默认值成立",
                [],
                run({ root: cfgsOn, data: withClaim({ pending: [], evidence: [guardPred] }) }),
            ],
            [
                "按 name 选数组项：数组换了顺序仍指到 guard_on",
                [],
                run({ root: cfgsSwapped, data: withClaim({ pending: [], evidence: [guardPred] }) }),
            ],
            [
                "按 name 选数组项：guard_on 默认值是 0 时判红",
                ["evidence-satisfied"],
                run({ root: cfgsOff, data: withClaim({ pending: [], evidence: [guardPred] }) }),
            ],
            [
                "映射形 golden 的条目里写了别的 id：谓词按键取值，feedback.default == 0 照常成立",
                [],
                run({ root: goldenIdGhost, data: withClaim({ pending: [], evidence: [goldenPred] }) }),
            ],
            [
                "映射形 golden 的条目里写了别的 id（ghost）：按条目里的 id 取不到参数，ghost.default == 0 判红",
                ["evidence-satisfied"],
                run({
                    root: goldenIdGhost,
                    data: withClaim({
                        pending: [],
                        evidence: ["golden:tests/golden/params-input.json#ghost.default == 0"],
                    }),
                }),
            ],
            [
                "映射形 golden 的条目 id 与另一个键重名：limiter.default == 1 取的是键 limiter 自己的条目，成立",
                [],
                run({
                    root: goldenIdSwap,
                    data: withClaim({
                        pending: [],
                        evidence: ["golden:tests/golden/params-input.json#limiter.default == 1"],
                    }),
                }),
            ],
            [
                "映射形 golden 的条目 id 与另一个键重名：feedback.default == 0.5 取的是键 feedback 自己的条目，成立",
                [],
                run({
                    root: goldenIdSwap,
                    data: withClaim({
                        pending: [],
                        evidence: ["golden:tests/golden/params-input.json#feedback.default == 0.5"],
                    }),
                }),
            ],
            [
                "按 name 选数组项：数组里没有 guard_on 时判红",
                ["evidence-satisfied"],
                run({ root: cfgsNoGuard, data: withClaim({ pending: [], evidence: [guardPred] }) }),
            ],
        ].map(([label, expect, result]) => {
            const lines = fired(result);
            const missed = expect.filter((r) => !lines.some((e) => e.startsWith(`[${r}] `)));
            return {
                label: missed.length ? `${label}（没有触发 ${missed.join("、")}）` : label,
                bad: !!expect.length,
                passed: expect.length ? missed.length === 0 : result.errors.length === 0,
                expect,
            };
        });
        // 报错要指到对应的强声明
        const golden05Lines = fired(run({ root: golden05, data: withClaim({ pending: [], evidence: [goldenPred] }) }));
        cases.push({
            label: "golden 谓词不成立时报错指到对应的强声明",
            bad: false,
            passed: golden05Lines.some((l) => l.includes("starts with feedback at zero")),
            expect: [],
        });
        // 引文区间：CONTRIBUTING.md 那句的两个引号都算，单独成句或夹在断言里的引号不算，行内代码算
        cases.push({
            label: "citedSpans：括号里举例的两个引号与行内代码算引文，单独成句和夹在断言里的引号不算",
            bad: false,
            passed:
                citedSpans(
                    'Strong claims about behaviour ("does not change with...", "safe by default") need evidence.',
                ).length === 2 &&
                citedSpans('Never write "works in LUNA" and `works in LUNA`.').length === 2 &&
                citedSpans('"Rinne works in LUNA."').length === 0 &&
                citedSpans('Rinne "works in LUNA" too.').length === 0 &&
                citedSpans("Rinne works in LUNA.").length === 0,
            expect: [],
        });
        // 参数表块模块（paramsOf）与这里的 findParam 读同一份 golden：条目里自带的 id 以键为准，两边取到同一个值
        for (const [label, body] of [
            ["条目 id 是 ghost，没有叫 ghost 的键", idGhost],
            ["条目 id 与另一个键重名", idSwap],
        ])
            cases.push({
                label: `params-table 的 paramsOf 与 claims 的 findParam 读到同一个值：${label}`,
                bad: false,
                passed:
                    paramsOf(body)
                        .map((p) => p.id)
                        .join() === Object.keys(body).join() &&
                    [...Object.keys(body), "ghost"].every(
                        (id) =>
                            (findParam(body, id)?.default ?? null) ===
                            (paramsOf(body).find((p) => p.id === id)?.default ?? null),
                    ) &&
                    findParam(body, "ghost") === null,
                expect: [],
            });
        // 工具函数
        const throws = (fn) => {
            try {
                fn();
                return false;
            } catch {
                return true;
            }
        };
        cases.push({
            label: "写坏的正则不悄悄退回字面匹配：matchPhrase 直接抛错",
            bad: true,
            passed: throws(() => matchPhrase("starts (with", "starts (with")),
            expect: [],
        });
        cases.push({
            label: "弯撇号与直撇号写同一句都命中 host['’]s 这种写法",
            bad: false,
            passed: matchPhrase("host['’]s own", "the host's own") && matchPhrase("host['’]s own", "the host’s own"),
            expect: [],
        });
        cases.push({
            label: "ciJobs 认得列 0 注释、行尾注释与下划线的 job 名",
            bad: false,
            passed: ["one", "two", "three_x"].every((j) => wfJobs.has(j)) && wfJobs.size === 3,
            expect: [],
        });
        // 短语样例：字符类、量词、分支、转义、非捕获组都能出样例，且样例真的匹配短语本身；写坏的正则不抛错
        const sampled = [
            "tested on macOS [0-9]",
            "在 macOS [^。\\n]{0,20}上可用",
            "limiter (?:and x )?(?:is|are) on",
            "11\\+",
            "a|b",
        ];
        cases.push({
            label: "phraseSamples：常见写法都出得了样例，样例匹配短语本身",
            bad: false,
            passed: sampled.every((p) => {
                const ss = phraseSamples(p);
                return ss.length > 0 && ss.every((t) => new RegExp(p, "i").test(t));
            }),
            expect: [],
        });
        cases.push({
            label: "phraseSamples：量词取最少与多一次，分支各出一个",
            bad: false,
            passed:
                phraseSamples("limiter (?:and x )?is").includes("limiter is") &&
                phraseSamples("limiter (?:and x )?is").includes("limiter and x is") &&
                ["a", "b"].every((x) => phraseSamples("a|b").includes(x)),
            expect: [],
        });
        cases.push({
            label: "phraseSamples：写坏的正则返回空，不抛错",
            bad: true,
            passed: phraseSamples("starts (with").length === 0,
            expect: [],
        });
        cases.push({
            label: "免责词表不宽到匹配任意句子，也不吞掉正面断言（passes auval and pluginval 不含免责词）",
            bad: false,
            passed:
                !disclaimerTooBroad() &&
                disclaimerProblem("passes auval and pluginval") !== null &&
                disclaimerProblem("通过 auval 与 pluginval") !== null &&
                disclaimerProblem("not yet tested") === null &&
                disclaimerProblem("尚未[^，。\\n]{0,10}(?:测试|验证)") === null,
            expect: [],
        });
        // 收窄后的词表：期待 / 待机 / 对待、句尾的 only、裸的 if 不算免责；带限定搭配的才算
        const notDisclaimers = [
            "期待",
            "待机",
            "对待",
            "works in LUNA only",
            "only",
            "if",
            "if you like",
            "不仅在",
            "仅仅",
        ];
        const disclaimers = [
            "待测",
            "待测试",
            "待验证",
            "待确认",
            "待上机",
            "待实测",
            "only on macOS 11+",
            "only if tested",
            "only when verified",
            "only after testing",
            "only for tested hosts",
            "if tested",
            "仅在 LUNA 实测后",
            "仅限已验证的宿主",
        ];
        cases.push({
            label: `免责词表收窄：${notDisclaimers.join(" / ")} 不算免责`,
            bad: true,
            passed: notDisclaimers.every((p) => disclaimerProblem(p.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")) !== null),
            expect: [],
        });
        cases.push({
            label: `免责词表收窄后仍认：${disclaimers.join(" / ")}`,
            bad: false,
            passed: disclaimers.every((p) => disclaimerProblem(p.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")) === null),
            expect: [],
        });
        // 记录文件名的系统后缀：只有一种写法，生成与校验是同一个函数
        cases.push({
            label: "recordOsSuffixes / recordNameTail：windows→win、macos→macos、linux→linux，any 取全部，不认识的系统没有后缀",
            bad: false,
            passed:
                recordOsSuffixes("windows").join() === "win" &&
                recordOsSuffixes("macOS").join() === "macos" &&
                recordOsSuffixes("linux").join() === "linux" &&
                recordOsSuffixes("any").join() === "win,macos,linux" &&
                recordOsSuffixes("freebsd").length === 0 &&
                recordNameTail("freebsd") === null &&
                ["mac", "windows", "osx"].every(
                    (s) => !new RegExp(`${recordNameTail()}$`).test(`2026-11-03-x-${s}.md`),
                ) &&
                ["win", "macos", "linux"].every((s) => new RegExp(`${recordNameTail()}$`).test(`2026-11-03-x-${s}.md`)),
            expect: [],
        });
        // 仓内路径函数：坏路径各一条，规范的相对路径放行；给了仓根时规范化后还要在仓根之内
        const badPaths = [
            "docs/../../x",
            "../x",
            "docs/validation/..",
            "/etc/x",
            "docs\\x",
            "C:/x",
            "c:x",
            "docs/x\0y",
            "docs//x",
            "docs/./x",
            "docs/x/",
            "",
        ];
        cases.push({
            label: "repoPathProblem：.. 段、绝对路径、反斜杠、盘符、空段、. 段都拒绝",
            bad: true,
            passed: badPaths.every((p) => repoPathProblem(p) !== null) && repoPathProblem(undefined) !== null,
            expect: [],
        });
        cases.push({
            label: "repoPathProblem：仓内的规范相对路径放行，带仓根时规范化后在仓根之内也放行",
            bad: false,
            passed:
                repoPathProblem("docs/validation/2026-11-03-luna-macos.md") === null &&
                repoPathProblem("docs/validation/2026-11-03-luna-macos.md", escapeRepo) === null &&
                repoPathProblem("docs/..x/y.md", escapeRepo) === null,
            expect: [],
        });
        cases.push({
            label: "validationExists / evalGolden 直接拒绝仓外的文件：路径不合格就不去读，仓内的照常",
            bad: true,
            passed:
                validationExists(escapeRepo, "docs/../../2026-11-03-luna-macos.md", false) === false &&
                validationExists(escapeRepo, "docs/validation/../../*-luna-macos.md", true) === false &&
                validationExists(withLuna, "docs/validation/2026-11-03-luna-macos.md", false) === true &&
                validationExists(withLuna, "docs/validation/*-luna-macos.md", true) === true &&
                evalGolden(escapeRepo, "../outside.json#a == 1") === null &&
                evalGolden(cfgsOn, "tests/golden/state-schema.json#cfgs.keys[name=guard_on].default == 1") === true,
            expect: [],
        });
        cases.push({
            label: "escapesRoot：规范化后落在仓根之外（或就是仓根本身）的路径判为越界",
            bad: true,
            passed:
                escapesRoot(escapeRepo, "docs/../../x") &&
                escapesRoot(escapeRepo, "../x") &&
                escapesRoot(escapeRepo, "..") &&
                escapesRoot(escapeRepo, ".") &&
                !escapesRoot(escapeRepo, "docs/x") &&
                !escapesRoot(escapeRepo, "docs/..x/y"),
            expect: [],
        });
        // 真实路径：仓内的 junction / 符号链接指向仓外（建不了链接的机器上这几条跳过，用例名里写明）
        if (outsideLinked && insideLinked) {
            cases.push({
                label: "escapesRoot：文件存在时按真实路径判，经仓内 junction 指到仓外的文件越界，不存在的路径与仓内链接照旧（需要能建 junction）",
                bad: true,
                passed:
                    escapesRoot(escapeRepo, "docs/validation/ext/2026-11-03-luna-macos.md") &&
                    escapesRoot(escapeRepo, "docs/validation/ext") &&
                    !escapesRoot(escapeRepo, "docs/validation/ext/not-there.md") &&
                    !escapesRoot(withLuna, "docs/alias/2026-11-03-luna-macos.md") &&
                    repoPathProblem("docs/validation/ext/2026-11-03-luna-macos.md", escapeRepo) !== null &&
                    repoPathProblem("docs/alias/2026-11-03-luna-macos.md", withLuna) === null,
                expect: [],
            });
            cases.push({
                label: "validationExists / evalGolden 不经 junction 读仓外的文件，通配也不进指向仓外的目录；仓内的链接照常（需要能建 junction）",
                bad: true,
                passed:
                    validationExists(escapeRepo, "docs/validation/ext/2026-11-03-luna-macos.md", false) === false &&
                    validationExists(escapeRepo, "docs/validation/ext/*-luna-macos.md", true) === false &&
                    evalGolden(escapeRepo, "docs/validation/ext/outside.json#a == 1") === null &&
                    validationExists(withLuna, "docs/alias/2026-11-03-luna-macos.md", false) === true &&
                    validationExists(withLuna, "docs/alias/*-luna-macos.md", true) === true,
                expect: [],
            });
            if (rootLinked)
                cases.push({
                    label: "仓根经链接到达时，仓根与目标都取真实路径再比：仓内的文件不越界、仍然成立（需要能建 junction）",
                    bad: false,
                    passed:
                        !escapesRoot(rootLink, "docs/validation/2026-11-03-luna-macos.md") &&
                        repoPathProblem("docs/validation/2026-11-03-luna-macos.md", rootLink) === null &&
                        validationExists(rootLink, "docs/validation/2026-11-03-luna-macos.md", false) === true,
                    expect: [],
                });
        } else {
            cases.push({
                label: "仓内 junction 指向仓外的证据判红（条件：本机能建 junction 或符号链接；这次建不了，已跳过）",
                bad: true,
                passed: true,
                expect: [],
            });
        }
    } finally {
        for (const l of links) dropDirLink(l);
        for (const d of temps) cleanupTempDir(d);
    }
    const unnamed = Object.keys(RULES).filter((r) => !cases.some((c) => c.expect.includes(r)));
    cases.push({
        label: `每条判据都有坏样例点名${unnamed.length ? `（没有：${unnamed.join("、")}）` : ""}`,
        bad: false,
        passed: unnamed.length === 0,
        expect: [],
    });
    // 参数表块模块和 golden 谓词认同一套参数形状，它的自测在这里一并跑
    cases.push(...paramsTableCases().map((c) => ({ ...c, label: `params-table 块：${c.label}` })));
    return caseResult(cases);
}

export default {
    name: "claims",
    group: "docs",
    inputs: [],
    run(ctx) {
        const text = readText(path.join(ctx.root, CLAIMS_FILE));
        if (text === null) return { ok: false, lines: [`[claims-shape] 缺 ${CLAIMS_FILE}`] };
        let data;
        try {
            data = JSON.parse(text);
        } catch (e) {
            return { ok: false, lines: [`[claims-shape] ${CLAIMS_FILE} 不是合法 JSON：${e.message}`] };
        }
        // 范围表不成形时 wording 会报具体原因；这里不在残缺的范围上继续判
        const table = loadScopeTable(ctx.root);
        if (table.problems.length) return { ok: false, lines: table.problems };
        const { errors, infos } = claimProblems({
            data,
            files: scopedFiles(ctx.root),
            root: ctx.root,
            scope: table.scope,
        });
        return { ok: errors.length === 0, lines: [...errors, ...infos] };
    },
    selfTest: (ctx) => selfTest(ctx.root),
};
