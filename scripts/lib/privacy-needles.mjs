// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/ci.json
//
// 公开文本的隐私与卫生规则：全仓只有这一张表。check-privacy（工作树、提交区间、PR 文本）、
// check-commits（提交消息、PR 文本、分支名）和两个 git hook 都从这里取规则；豁免与自测夹具也在这里。
//
// 表里只放通用规则。个人信息针（个人邮箱、本机用户名与路径片段、其他账号名）不进公开仓：
// 本机从 RINNE_PRIVACY_LOCAL（或 git config rinne.privacyLocal）指向的仓外文件读，
// CI 从 secret 注入的环境变量 PRIVACY_NEEDLES 读（每行一条），见 loadPersonalNeedles()。
//
// 针和夹具一律用 j() 由片段拼出：本文件自己也在被扫范围内，源码里一旦出现完整形态，
// 就只能给自己开豁免，而豁免正是藏东西的地方。夹具与规则各拼各的，互不引用，
// 这样把规则改坏时夹具不会跟着一起变（否则自测永远是绿的）。

import fs from "node:fs";
import { j, inCI, localSetting } from "./check-kit.mjs";

const BS = "\\\\"; // 正则源码里的一个反斜杠
const SEP = `(?:${BS}${BS}|${BS}|/)`; // JSON 转义的双反斜杠、手写的单反斜杠、CMake 的正斜杠
const DRIVE = `(?:[A-Za-z]:${SEP}|/(?:mnt/)?[A-Za-z]/)`; // 盘符，以及 MSYS、WSL 的挂载前缀
const NAME = "([^\\\\/\\s\"'`|;*?]+)"; // 路径里的用户名段
const MAIL_DOMAINS = "gmail|qq|163|126|outlook|hotmail|yahoo|foxmail|icloud|sina";

/** 逐字收录的第三方文本：上游作者的邮箱与编号属于原文，不算泄漏。 */
export const VERBATIM_DIRS = ["third_party/", "LICENSES/", "web/js/juce/"];
/** 代码文件只对注释部分套用括号编号等规则，免得滤波器系数、CSS 类名一类的代码标识符误伤。 */
export const CODE_FILE = /\.(?:c|cc|cpp|h|hpp|mm|js|mjs|cjs|ts|css|html|py|ps1|psm1|sh|cmake)$|(?:^|\/)CMakeLists\.txt$/;
const COMMENT_START = /\/\/|\/\*|<!--|#|^\s*\*/;

const placeholder = (u) => /^[<%$]/.test(u) || u === "...";
// 章节号只在许可证条款里放行（「GPLv3 」加章节号这类），或者同一文件有以该章节号开头的标题。
const licenseSection = (m, line, ctx) =>
    /GPL|OFL|Apache|section/i.test(line.slice(Math.max(0, m.index - 12), m.index)) ||
    !!ctx.headings?.has(m[0].replace(/\D*(\d+).*/, "$1"));

const rule = (id, src, flags, extra = {}) => ({ id, re: new RegExp(src, `g${flags}`), ...extra });
const ID = { verbatimOk: true }; // 编号类规则：逐字第三方文本里豁免

export const RULES = [
    // 任务卡号：字母 R、N 加两位数字，带或不带连字符；条件卡单独一条
    rule("card-id", j("\\bR", "N-?\\d{2}[a-z]?\\b"), "i", ID),
    rule("card-k", j("\\bK", "8\\b"), "i", ID),
    rule("serial", j("\\b(?:KD|S", "L|LB)-\\d{1,4}\\b"), "", ID),
    // 复制来源的内部编号：裁决号、任务号与里程碑号、三位决策号；四位的公开 ADR 编号放行。
    // 任务号与里程碑号后面紧跟数字时不算（SVG 路径数据里满是这种形态）。
    rule("upstream-j", j("\\bJ", "\\d{2,3}[a-z]?\\b"), "", ID),
    rule("upstream-tm", j("\\b[TM]", "\\d{2}\\b(?![\\s,.]*-?\\d)"), "", ID),
    rule("upstream-adr", j("\\bAD", "R-?\\d{3}\\b"), "", ID),
    // 括号里的私有编号：一个大写字母加 1–3 位数字
    rule("bracket-id", j("[\\[(（][^\\])）\\n]{0,10}?\\b[A-Z]\\d{1,3}", "[a-z]?(?=[\\s、,，/–\\-\\])）])"), "", {
        ...ID,
        commentsOnly: true,
    }),
    rule("section", j("§", "\\s?\\d+(?:\\.\\d+)*"), "", { ...ID, allow: licenseSection }),
    rule("plan-section", j("\\b\\d{2}\\s?", "§\\s?\\d"), "", ID),
    rule("decision", j("(?:用户", "决策|裁", "决)\\s*[A-Z]?\\d"), "", ID),
    // 私有计划文件名；前面紧跟「数字-」的是日期（docs/validation/2026-11-05-probe.md 这类），不算
    rule("plan-file", j("(?<!\\d-)\\b0\\d-[a-z-]+", "\\.md\\b"), "", ID),
    rule("gap", j("\\bga", "p-\\d{2}\\b"), "i", { ...ID, commentsOnly: true }),
    // 私有目录与私有仓名
    rule("private-name/scratch", j("\\b_scr", "atch\\b"), "i", ID),
    rule("private-name/ref", j("\\b_re", "f/"), "i", ID),
    rule("private-name/plan-repo", j("\\bmaster", "-?plan\\b"), "i", ID),
    rule("private-name/site-repo", j("DLsnows/Syn", "chain\\b"), "i", ID),
    rule("private-name/workspace", j("\\bloop", "back/(?:docs|plan|wt|_\\w+)"), "i", ID),
    // 本机路径与主机名（逐字第三方文本里也不豁免）
    rule("win-user-path", j(DRIVE, "Us", "ers", SEP, NAME), "i", { allow: (m) => placeholder(m[1]) }),
    rule("mac-user-path", j("(?<![\\w:])/Us", "ers/", NAME), "", {
        allow: (m) => placeholder(m[1]) || m[1] === "runner" || m[1] === "Shared",
    }),
    rule("personal-email", j("[A-Za-z0-9._%+-]+", "@(?:", MAIL_DOMAINS, ")\\.(?:com|cn|net)"), "i", ID),
    rule("hostname", j("\\b(?:DESK", "TOP|LAP", "TOP)-(?=[A-Z0-9]*\\d)[A-Z0-9]{5,}\\b"), "i"),
    rule("mac-host", j("\\b[\\w-]*(?:Mac", "Book|iMac|Mac-?mini|Mac-?Studio)[\\w-]*\\.loc", "al\\b"), "i"),
    // AI 署名
    rule("ai-coauthor", j("^co-authored", "-by:.*(?:cla", "ude|anthr", "opic)"), "i", { group: "ai" }),
    rule("ai-session", j("^cla", "ude-session:"), "i", { group: "ai" }),
    rule("ai-generated", j("generated with \\[?cla", "ude"), "i", { group: "ai" }),
    rule("ai-email", j("noreply@anthr", "opic\\.com"), "i", { group: "ai" }),
    rule("ai-url", j("cla", "ude\\.ai/code"), "i", { group: "ai" }),
    rule("ai-robot", "\\u{1F916}", "u", { group: "ai" }),
    // 写错的许可证标识：JUCE 部分是 AGPLv3 only
    rule("wrong-license", j("AGPL-3\\.0", "-or-later"), "i"),
];

const escapeRe = (s) => s.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
const personalRule = (needles) => ({
    id: "personal",
    re: new RegExp(needles.map(escapeRe).join("|"), "gi"),
    needles,
});

const commentPart = (line) => {
    const m = COMMENT_START.exec(line);
    return m ? line.slice(m.index) : "";
};

/** 以章节号开头的 Markdown 标题里的号码：同一文件内引用这些章节时放行。 */
export const headingsOf = (text) =>
    new Set([...text.matchAll(new RegExp(j("^#{1,6}\\s*", "§\\s?(\\d+)"), "gm"))].map((m) => m[1]));

/**
 * 扫一段文本。ctx.path 给出时按文件处理（逐字目录豁免、代码文件只看注释），
 * 否则按 PR / 提交文本处理，没有任何路径豁免。返回 [{ rule, line, col, match }]。
 */
export function scan(text, ctx = {}, opts = {}) {
    const { rules = RULES, personal = [], verbatimDirs = VERBATIM_DIRS, codeFile = CODE_FILE } = opts;
    const p = ctx.path || "";
    const verbatim = !!ctx.path && verbatimDirs.some((d) => p.startsWith(d));
    const code = !!ctx.path && codeFile.test(p);
    const all = personal.length ? [...rules, personalRule(personal)] : rules;
    const out = [];
    text.split(/\r?\n/).forEach((line, i) => {
        for (const r of all) {
            if (r.verbatimOk && verbatim) continue;
            const src = r.commentsOnly && code ? commentPart(line) : line;
            for (const m of src.matchAll(r.re)) {
                if (r.allow?.(m, src, ctx)) continue;
                out.push({ rule: r, line: i + 1, col: line.length - src.length + m.index + 1, match: m[0] });
            }
        }
    });
    return out;
}

/** 打码：个人针只给序号；其余只留前两个字符。CI 日志公开且长期保留，回显原文等于替它发布一次。 */
export function redact(f) {
    if (f.rule.needles) {
        const n = f.rule.needles.findIndex((x) => x.toLowerCase() === f.match.toLowerCase());
        return `个人针 #${n + 1}`;
    }
    return f.match.slice(0, 2) + "*".repeat(Math.max(0, f.match.length - 2));
}

const parseNeedles = (s) =>
    s
        .split(/\r?\n/)
        .map((l) => l.trim())
        .filter((l) => l && !l.startsWith("#"));

/**
 * 个人针：本机文件与 CI 变量取并集。本机未配置、配置了却读不到、文件里没有针，都直接抛错；
 * 只有在 CI 上两者都缺（fork PR 读不到 secret）时，才打印注记并只跑通用规则。
 */
export function loadPersonalNeedles({ env = process.env, cwd = process.cwd() } = {}) {
    const file = localSetting("RINNE_PRIVACY_LOCAL", "rinne.privacyLocal", { env, cwd });
    const needles = [];
    const notes = [];
    if (file) {
        if (!fs.existsSync(file)) throw new Error("RINNE_PRIVACY_LOCAL 指向的个人针文件不存在");
        const own = parseNeedles(fs.readFileSync(file, "utf8"));
        if (!own.length) throw new Error("个人针文件里没有任何针");
        needles.push(...own);
    }
    if (env.PRIVACY_NEEDLES) needles.push(...parseNeedles(env.PRIVACY_NEEDLES));
    if (!needles.length) {
        if (!inCI(env)) throw new Error("未配置个人针：设置 RINNE_PRIVACY_LOCAL 或 git config rinne.privacyLocal");
        notes.push("::notice::PRIVACY_NEEDLES 不可用（fork PR 读不到 secret），本次只跑通用规则");
    }
    return { needles, notes };
}

// ---- 自测夹具 ------------------------------------------------------------------------
// [应命中的规则 id（null = 必须完全干净）, 文本, 路径（省略 = PR 或提交文本）]

/** 只用于自测的假针。真实的个人针永远不进仓库。 */
export const FAKE_NEEDLE = "rinne-selftest-fake-needle-7c1e";
const ROBOT = String.fromCodePoint(0x1f916);

export const CASES = [
    ["card-id", j("修好 R", "N-12 的回归")],
    ["card-id", j("见 R", "N05")],
    ["card-id", j("feat/r", "n-01a-pairing")],
    ["card-k", j("条件卡 K", "8 待定")],
    [null, "部署到 K8s 集群"],
    ["serial", j("S", "L-446 已修")],
    ["upstream-j", j("沿用 J", "12 的约定")],
    ["upstream-tm", j("T", "19 卡的修复")],
    ["upstream-tm", j("里程碑 M", "04 之后")],
    [null, '<path d="M12 2L4 6v6c0 5 8 10 8 10M10,10">', "web/shared/icons.js"],
    ["upstream-adr", j("AD", "R-011 定的")],
    [null, "决策记录见 ADR-0007"],
    ["bracket-id", j("说明 (J", "45)")],
    ["bracket-id", j("（G", "1）")],
    ["bracket-id", j("（用户决策 D", "4）")],
    ["bracket-id", j("[B 线 M", "10]")],
    ["bracket-id", j("见（G", "1）"), "docs/USER_GUIDE.md"],
    ["bracket-id", j("// 失控保护（G", "1）"), "src/core/dsp/Guard.h"],
    [null, "格式 (VST3)、架构 (x64)、已知问题 (KI-6)、(ADR-0007)、(#12)"],
    [null, "return biquad(B0, B1, A1);", "src/core/dsp/Biquad.h"],
    ["section", j("详见 §", "4.2 的说明")],
    [null, "按 GPLv3 §6(d) 提供对应源码；JUCE 部分适用 AGPLv3（GPLv3 §13）"],
    [null, j("## §", "3 分支\n见 §", "3 的说明"), "CLAUDE.md"],
    ["plan-section", j("见 0", "6 §", "3")],
    ["decision", j("用户", "决策 D4 已定")],
    ["plan-file", j("见 0", "4-execution.md")],
    [null, "docs/validation/2026-11-05-probe-luna-macos.md"],
    ["gap", j("ga", "p-07")],
    [null, '<div class="row gap-12">', "web/output/index.html"],
    ["private-name/scratch", j("_scr", "atch/notes")],
    ["private-name/ref", j("_re", "f/upstream")],
    ["private-name/plan-repo", j("scvb-master", "plan")],
    ["private-name/site-repo", j("DLsnows/Syn", "chain")],
    ["private-name/workspace", j("loop", "back/plan/x")],
    [null, "audio loopback device"],
    ["win-user-path", j("D:", "\\", "Us", "ers", "\\", "someone", "\\", "proj")],
    ["win-user-path", j("c:/", "us", "ers/", "someone")],
    ["win-user-path", j("C:", "\\\\", "Us", "ers", "\\\\", "someone")],
    ["win-user-path", j("/c/", "Us", "ers/", "someone")],
    ["win-user-path", j("/mnt/c/", "Us", "ers/", "someone")],
    [null, j("C:", "\\", "Us", "ers", "\\", "<user>", "\\", "x 与 C:/", "Us", "ers/%USERNAME%/x")],
    [null, "C:\\Program Files\\Common Files\\VST3"],
    ["mac-user-path", j("/Us", "ers/someone/Music")],
    [null, j("/Us", "ers/runner/work 与 /Us", "ers/Shared/x")],
    ["personal-email", j("someone", "@", "gmail", ".com"), "src/core/X.cpp"],
    ["personal-email", j("someone", "@", "qq", ".com")],
    [null, j("Author: Someone <a", "@", "qq", ".com>"), "third_party/notices/x.txt"],
    [null, "联系 contact@synchain.ca"],
    ["hostname", j("DESK", "TOP-AB12CD3")],
    ["hostname", j("lap", "top-z9y8x7w")],
    [null, "desktop-friendly layout"],
    ["mac-host", j("someones-Mac", "Book-Pro.loc", "al")],
    [null, "settings.local.json"],
    ["ai-coauthor", j("Co-Authored", "-By: Cla", "ude <x@example.com>")],
    ["ai-session", j("Cla", "ude-Session: abc")],
    ["ai-generated", j("Generated with [Cla", "ude Code]")],
    ["ai-email", j("noreply@anthr", "opic.com")],
    ["ai-url", j("https://cla", "ude.ai/code")],
    ["ai-robot", `${ROBOT} done`],
    ["wrong-license", j("AGPL-3.0", "-or-later")],
    [null, "JUCE 部分：AGPL-3.0-only"],
    [null, "【重要】修复 D 合约越界；【红旗】已清零；【建议】挂账"],
    ["personal", `日志里出现 ${FAKE_NEEDLE.toUpperCase()}`],
];

/** 跑夹具；opts 可以替换规则或豁免（变异检查用）。返回每条夹具的结果。 */
export function runCases(opts = {}) {
    return CASES.map(([want, text, path]) => {
        const ids = scan(text, path ? { path, headings: headingsOf(text) } : {}, {
            personal: [FAKE_NEEDLE],
            ...opts,
        }).map((f) => f.rule.id);
        const passed = want ? ids.includes(want) : ids.length === 0;
        return { label: `${want || "干净"}：${JSON.stringify(text).slice(0, 48)}`, bad: !!want, passed };
    });
}

/**
 * 变异检查：逐条屏蔽判据（规则失效、豁免放宽到全放、豁免收紧到不放、逐字目录与注释范围的两头），
 * 每个变异都必须让至少一条夹具变红，否则说明那条判据没有夹具守着。
 */
export function mutationCheck() {
    const never = /(?!)/g;
    const swap = (id, patch) => RULES.map((r) => (r.id === id ? { ...r, ...patch } : r));
    const mutants = RULES.flatMap((r) => [
        [`${r.id}：判据失效`, { rules: swap(r.id, { re: never }) }],
        ...(r.allow
            ? [
                  [`${r.id}：豁免全放`, { rules: swap(r.id, { allow: () => true }) }],
                  [`${r.id}：豁免全不放`, { rules: swap(r.id, { allow: () => false }) }],
              ]
            : []),
    ]);
    mutants.push(
        ["个人针：未合并", { personal: [] }],
        ["逐字目录：豁免一切路径", { verbatimDirs: [""] }],
        ["逐字目录：不豁免", { verbatimDirs: [] }],
        ["代码注释范围：所有文件都当代码", { codeFile: /./ }],
        ["代码注释范围：不区分代码", { codeFile: /(?!)/ }],
    );
    const survived = mutants.filter(([, opts]) => runCases(opts).every((c) => c.passed)).map(([name]) => name);
    return { total: mutants.length, survived };
}
