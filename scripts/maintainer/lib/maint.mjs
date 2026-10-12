// SPDX-License-Identifier: GPL-3.0-or-later
//
// 维护者包装脚本（scripts/maintainer/*.ps1）调用的 Node 判据。这里不另写规则，只把已有的判据接到 PowerShell 上：
// 隐私规则表与个人针（scripts/lib/privacy-needles.mjs）、提交消息判据（scripts/check-commits.mjs）、
// 机读尾注、条数核对与上一轮 / 处置汇总的认定（scripts/review-bot.mjs）。输入是文件，输出是一行 JSON。
//   node scripts/maintainer/lib/maint.mjs text --file <f> [--needles local|ci|ci-optional]
//        公开文本扫描：通用规则、个人针、AI 署名与任何署名尾注行、卡号；命中只给打码后的片段
//   node scripts/maintainer/lib/maint.mjs message --file <f> [--needles …]
//        squash 提交消息：subject 格式、DCO（恰好一行维护者签名）、只许 Signed-off-by 一种尾注、隐私
//   node scripts/maintainer/lib/maint.mjs review-state --file <issue comments JSON> --writers <login,…> [--bot claude]
//        最近一条 bot 审查总结（head、运行链接、尾注与条数核对）与它之后的处置汇总（作者须在 writers 里）；
//        另列出全部 bot 总结（尾注、运行编号、逐条意见）与全部处置汇总引用的运行编号，供「每条 bot 总结都有回应」核对
//   node scripts/maintainer/lib/maint.mjs signoff        维护者的签名行（squash 消息的最后一行）

import fs from "node:fs";
import { SIGNOFF, checkMessage } from "../../check-commits.mjs";
import { isMain, j } from "../../lib/check-kit.mjs";
import { loadPersonalNeedles, needlesMode, plainText, redact, scan } from "../../lib/privacy-needles.mjs";
import { FOOTER, findPrevious, validateSummary } from "../../review-bot.mjs";

/** 本仓 Actions 运行页的链接：bot 总结第二行的「运行记录」。 */
const RUN_LINK = /\/actions\/runs\/(\d+)/;
/** agent 隐藏标记：首行，格式见 RinneGh.psm1 的 New-AgentMarker。 */
const AGENT_MARK =
    /^<!-- rinne-agent v1 role=([a-z]+) kind=([a-z-]+) round=(\d+) head=([0-9a-f]{7,40})((?: [a-z-]+=[^\s>]+)*) -->$/;
const SUMMARY_HEAD = /^<!-- rinne-review-head sha=([0-9a-f]{40}) bot=([a-z]+) -->$/m;
/**
 * 公开文本里不许出现任何署名尾注行（不论署的是谁）：提交消息由 check-commits 的同一条判据拦，
 * PR 正文与评论在这里拦。隐私规则表里的 ai-coauthor 只认 AI 的名字，不够。
 */
const TRAILER = new RegExp(j("^\\s*(?:co-authored", "-by|cla", "ude-session)\\s*:"), "i");

export function textFindings(text, personal) {
    const out = scan(text, {}, { personal }).map((f) => ({ rule: f.rule.id, line: f.line, text: redact(f) }));
    text.split(/\r?\n/).forEach((l, i) => {
        if (TRAILER.test(l)) out.push({ rule: "ai-trailer", line: i + 1, text: `${l.trim().slice(0, 2)}…` });
    });
    return out;
}

/** 首行的 agent 标记，解析成 { role, kind, round, head, extra }；没有返回 null。 */
export function agentMarker(body) {
    const m = AGENT_MARK.exec((body || "").split(/\r?\n/, 1)[0]);
    if (!m) return null;
    const extra = Object.fromEntries(
        m[5]
            .trim()
            .split(/\s+/)
            .filter(Boolean)
            .map((kv) => kv.split("=")),
    );
    return { role: m[1], kind: m[2], round: +m[3], head: m[4], extra };
}

/**
 * 贴出去的 bot 审查总结能不能用：机读尾注恰好一行且在最后一行（缺失或多于一行 → bot 结论不可用），
 * 三个数与正文三节的条数一致（review-bot 的 validateSummary）。另取首行 head 标记与开头的运行链接。
 */
export function summaryFacts(body) {
    const lines = body.replace(/\r\n/g, "\n").trimEnd().split("\n");
    const footers = lines.filter((l) => FOOTER.test(l.trim()));
    const base = {
        head: SUMMARY_HEAD.exec(body)?.[1] || null,
        runId: RUN_LINK.exec(lines.slice(0, 3).join("\n"))?.[1] || null,
    };
    if (footers.length !== 1)
        return { ...base, ok: false, reason: footers.length ? "机读尾注多于一行" : "缺少机读尾注" };
    if (!FOOTER.test(lines.at(-1).trim())) return { ...base, ok: false, reason: "机读尾注不在最后一行" };
    const v = validateSummary(body);
    if (!v.ok) return { ...base, ok: false, reason: v.reason };
    const [, red, important, suggest] = FOOTER.exec(lines.at(-1).trim());
    return { ...base, ok: true, reason: null, red: +red, important: +important, suggest: +suggest };
}

/**
 * bot 总结里逐条意见：`## 【红旗】`、`## 【重要】`、`## 【建议】` 节内顶格的列表项，口径与 review-bot 的
 * sectionCounts 相同（跳过代码块、「无」不算、三级以下标题不打断）。没开 thread 的意见靠它列出来。
 */
export function summaryItems(body) {
    const items = [];
    let level = null;
    let fence = false;
    for (const l of body.replace(/\r\n/g, "\n").split("\n")) {
        if (/^\s*(?:```|~~~)/.test(l)) {
            fence = !fence;
            continue;
        }
        if (fence) continue;
        const h = /^(#{1,6})\s/.exec(l);
        if (h && h[1].length <= 2) {
            level = /^##\s+【(红旗|重要|建议)】/.exec(l)?.[1] || null;
            continue;
        }
        const item = /^(?:[-*+]|\d+[.)])\s+(\S.*)$/.exec(l);
        if (level && item && !/^无[。.]?\s*$/.test(item[1])) items.push({ level, text: item[1] });
    }
    return items;
}

/** comments：issue comments 接口的原样数组（按时间升序）。writers：对本仓有 write 及以上权限的 login。 */
export function reviewState(comments, writers, bot = "claude") {
    const trusted = (c) => writers.includes(c.user?.login);
    const prev = findPrevious(comments, bot, trusted);
    const isSummary = (c) => c.user?.login === "github-actions[bot]" && SUMMARY_HEAD.exec(c.body || "")?.[2] === bot;
    const summaries = comments.filter(isSummary).map((c) => ({
        id: c.id,
        createdAt: c.created_at,
        ...summaryFacts(c.body || ""),
        items: summaryItems(c.body || ""),
    }));
    const dispositions = comments
        .filter((c) => trusted(c) && !/\[bot\]$/.test(c.user?.login || ""))
        .map((c) => agentMarker(c.body))
        .filter((m) => m && m.kind === "disposition");
    const dm = prev.disposition ? agentMarker(prev.disposition) : null;
    return {
        rounds: prev.rounds,
        summary: summaries.at(-1) || null,
        summaries,
        disposition: dm ? { round: dm.round, head: dm.head, botRun: dm.extra["bot-run"] || null, role: dm.role } : null,
        dispositionRuns: dispositions.map((m) => m.extra["bot-run"] || null).filter(Boolean),
    };
}

function main(argv) {
    const [cmd, ...rest] = argv;
    const opt = (k) => (rest.includes(k) ? rest[rest.indexOf(k) + 1] : undefined);
    const print = (v) => process.stdout.write(`${JSON.stringify(v)}\n`);
    if (cmd === "signoff") {
        print({ signoff: SIGNOFF });
        return 0;
    }
    const file = opt("--file");
    if (!file) throw new Error("需要 --file");
    const needles = () => loadPersonalNeedles({ mode: needlesMode(rest) }).needles;
    if (cmd === "text") {
        print({ findings: textFindings(plainText(fs.readFileSync(file), "文本"), needles()) });
    } else if (cmd === "message") {
        const text = plainText(fs.readFileSync(file), "提交消息");
        print(checkMessage(text, { tier: "same", exactlyOne: true, personal: needles() }));
    } else if (cmd === "review-state") {
        const comments = JSON.parse(fs.readFileSync(file, "utf8"));
        const writers = (opt("--writers") || "").split(",").filter(Boolean);
        print(reviewState(comments, writers, opt("--bot") || "claude"));
    } else {
        throw new Error("用法见文件头注释：text | message | review-state | signoff");
    }
    return 0;
}

if (isMain(import.meta.url)) {
    try {
        process.exitCode = main(process.argv.slice(2));
    } catch (e) {
        process.stdout.write(`${JSON.stringify({ error: e.message })}\n`);
        process.exitCode = 2;
    }
}
