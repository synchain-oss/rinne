// SPDX-License-Identifier: GPL-3.0-or-later
//
// 维护者包装脚本（scripts/maintainer/*.ps1）调用的 Node 判据。这里不另写规则，只把已有的判据接到 PowerShell 上：
// 隐私规则表与个人针（scripts/lib/privacy-needles.mjs）、提交消息判据（scripts/check-commits.mjs）、
// 机读尾注、条数核对与上一轮 / 处置汇总的认定（scripts/review-bot.mjs）。输入是文件，输出是一行 JSON。
//   node scripts/maintainer/lib/maint.mjs text --file <f> [--needles local|ci|ci-optional]
//        公开文本扫描：通用规则、个人针、AI 署名与任何署名尾注行、卡号；命中只给打码后的片段
//   node scripts/maintainer/lib/maint.mjs texts --file <字符串 JSON 数组> [--needles …]   同上，一次扫多段
//   node scripts/maintainer/lib/maint.mjs message --file <f> [--needles …]
//        squash 提交消息：subject 格式、DCO（恰好一行维护者签名）、只许 Signed-off-by 一种尾注、隐私
//   node scripts/maintainer/lib/maint.mjs review-state --file <issue comments JSON> --writers <login,…> [--bot claude]
//        最近一条 bot 审查总结（head、运行链接、尾注与条数核对）与它之后的处置汇总（作者须在 writers 里）；
//        另列出全部 bot 总结（尾注、运行编号、逐条意见、created_at / updated_at 与正文 sha256）与全部处置汇总
//        引用的运行编号，供「每条 bot 总结都有回应」「总结贴出之后没被改过」核对
//   node scripts/maintainer/lib/maint.mjs infra --file <路径 JSON 数组> [--list <glob JSON 数组>]
//        其中碰到基础设施路径的那些。清单与匹配规则都取自 scripts/lib/infra-paths.mjs（唯一真源）；
//        --list 只给自测换一份 glob，匹配规则不变
//   node scripts/maintainer/lib/maint.mjs signoff        维护者的签名行（squash 消息的最后一行）
//   node scripts/maintainer/lib/maint.mjs --self-test    本文件的判据：首行 head 标记（CRLF、BOM、正文引用）、发帖记录行与正文哈希

import fs from "node:fs";
import { createHash } from "node:crypto";
import { SIGNOFF, checkMessage } from "../../check-commits.mjs";
import { caseResult, isMain, j } from "../../lib/check-kit.mjs";
import { loadPersonalNeedles, needlesMode, plainText, redact, scan } from "../../lib/privacy-needles.mjs";
import { FOOTER, findPrevious, headMark, validateSummary } from "../../review-bot.mjs";

/** 本仓 Actions 运行页的链接：bot 总结第二行的「运行记录」。 */
const RUN_LINK = /\/actions\/runs\/(\d+)/;
/** agent 隐藏标记：首行，格式见 RinneGh.psm1 的 New-AgentMarker。 */
const AGENT_MARK =
    /^<!-- rinne-agent v1 role=([a-z]+) kind=([a-z-]+) round=(\d+) head=([0-9a-f]{7,40})((?: [a-z-]+=[^\s>]+)*) -->$/;
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
 * review-bot 发评论后写的那一行（GITHUB_STEP_SUMMARY 与运行日志里各一份）：
 * `rinne-review-posted id=<评论编号> sha256=<正文的 sha256>`。拿不到返回 null。
 */
export const POSTED_LINE = /^rinne-review-posted id=(\d+) sha256=([0-9a-f]{64})$/;

/** 一段文本里记下的发帖行 { id, sha256 }；没有返回 null。 */
export function postedLine(text) {
    for (const l of (text || "").split(/\r?\n/)) {
        const m = POSTED_LINE.exec(l.trim());
        if (m) return { id: m[1], sha256: m[2] };
    }
    return null;
}

/** 评论正文按 review-bot 的口径取 sha256：先把 CRLF 归一成 LF，再对 UTF-8 字节算。 */
export function bodySha256(body) {
    return createHash("sha256")
        .update(Buffer.from((body || "").replace(/\r\n/g, "\n"), "utf8"))
        .digest("hex");
}

/**
 * 贴出去的 bot 审查总结能不能用：机读尾注恰好一行且在最后一行（缺失或多于一行 → bot 结论不可用），
 * 三个数与正文三节的条数一致（review-bot 的 validateSummary）。另取首行 head 标记与开头的运行链接。
 */
export function summaryFacts(body) {
    const lines = body.replace(/\r\n/g, "\n").trimEnd().split("\n");
    const footers = lines.filter((l) => FOOTER.test(l.trim()));
    const base = {
        head: headMark(body)?.sha || null,
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
    // 只认首行的 head 标记（review-bot 的 headMark）：pr-agent 与手动 DeepSeek 审查的评论也出自 github-actions[bot]
    const isSummary = (c) => c.user?.login === "github-actions[bot]" && headMark(c.body)?.bot === bot;
    const summaries = comments.filter(isSummary).map((c) => ({
        id: c.id,
        createdAt: c.created_at,
        // review-bot 只建不改：两者不同说明总结在贴出之后被改过，判据在 pr-merge
        updatedAt: c.updated_at ?? c.created_at,
        // 正文的 sha256（CRLF 归一成 LF）：review-bot 发帖时也记下同一个值，pr-merge 用它核对评论没被改过
        sha256: bodySha256(c.body || ""),
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

/**
 * 自测：首行 head 标记的三种往返形态（CRLF、BOM、正文里引用标记行不算）与发帖记录行。返回 { ok; bad; good; lines }。
 * 「标记不在首行就不是总结」这条不能放宽：改回多行正则时「正文里引用了标记行」会变红。
 */
export function selfTest() {
    const cases = [];
    const t = (label, bad, flagged) => cases.push({ label, bad, passed: bad ? flagged : !flagged });
    const sha = "a".repeat(40);
    const mark = `<!-- rinne-review-head sha=${sha} bot=claude -->`;
    const body = `${mark}\n_审查对象：\`aaaaaaa\`_\n\n## 结论\n无\n`;
    const found = (text) => headMark(text)?.sha === sha;
    const summaries = (text) =>
        reviewState(
            [
                {
                    id: 1,
                    user: { login: "github-actions[bot]" },
                    created_at: "2026-10-12T04:00:00Z",
                    updated_at: "2026-10-12T04:00:00Z",
                    body: text,
                },
            ],
            [],
        );
    t("首行是 head 标记时认得出", false, !found(body));
    t("CRLF：首行以 \\r\\n 结尾仍认得出", false, !found(body.replace(/\n/g, "\r\n")));
    t("BOM：首行前有 U+FEFF 仍认得出", false, !found(`\uFEFF${body}`));
    t("标记不在首行（正文里引用了标记行）不算总结", false, found(`## 引用\n${mark}\n`));
    t("只出现在第二行的标记不算", false, found(`\n${mark}\n`));
    t("reviewState：首行 head 标记算总结", false, summaries(body).summaries.length !== 1);
    t("reviewState：标记不在首行不算总结", false, summaries(`## 引用\n${mark}\n`).summaries.length !== 0);
    t("reviewState：CRLF 仍算总结", false, summaries(body.replace(/\n/g, "\r\n")).summaries.length !== 1);
    t("reviewState：BOM 仍算总结", false, summaries(`\uFEFF${body}`).summaries.length !== 1);
    t(
        "reviewState：带出 updated_at 与正文 sha256",
        false,
        (() => {
            const s = reviewState(
                [
                    {
                        id: 1,
                        user: { login: "github-actions[bot]" },
                        created_at: "2026-10-12T04:00:00Z",
                        updated_at: "2026-10-12T05:00:00Z",
                        body,
                    },
                ],
                [],
            ).summary;
            return !(s && s.updatedAt === "2026-10-12T05:00:00Z" && s.sha256 === bodySha256(body));
        })(),
    );
    t("summaryFacts.head：CRLF 首行仍取到", false, summaryFacts(body.replace(/\n/g, "\r\n")).head !== sha);
    t("summaryFacts.head：BOM 首行仍取到", false, summaryFacts(`\uFEFF${body}`).head !== sha);
    t("summaryFacts.head：标记不在首行为空", false, summaryFacts(`引用\n${mark}\n`).head !== null);
    // 发帖记录行与正文哈希
    const line = `rinne-review-posted id=123456 sha256=${"0".repeat(64)}`;
    const got = postedLine(`日志行\n${line}\n`);
    t("发帖记录行：取到编号与 sha256", false, !(got && got.id === "123456" && got.sha256 === "0".repeat(64)));
    t("发帖记录行：没有这一行时取不到", true, postedLine("别的日志\n") === null);
    t("发帖记录行：编号不是数字时不认", true, postedLine(line.replace("id=123456", "id=x")) === null);
    t("发帖记录行：sha256 不是 64 位十六进制时不认", true, postedLine(line.replace("0".repeat(64), "zz")) === null);
    const lf = "<!-- rinne-review-head sha=" + sha + " -->\n## 结论\n无\n";
    t("正文哈希：CRLF 与 LF 归一后相同", false, bodySha256(lf) !== bodySha256(lf.replace(/\n/g, "\r\n")));
    t("正文哈希：正文变了哈希就变", true, bodySha256(lf) !== bodySha256(`${lf}补一句\n`));
    t(
        "正文哈希：与 review-bot 记录的口径一致（对 UTF-8 字节算）",
        false,
        bodySha256(lf) !== createHash("sha256").update(Buffer.from(lf, "utf8")).digest("hex"),
    );
    return caseResult(cases);
}

async function main(argv) {
    const [cmd, ...rest] = argv;
    const opt = (k) => (rest.includes(k) ? rest[rest.indexOf(k) + 1] : undefined);
    const print = (v) => process.stdout.write(`${JSON.stringify(v)}\n`);
    if (cmd === "--self-test") {
        const r = selfTest();
        for (const l of r.lines) console.error(l);
        console.log(`maint --self-test：${r.ok ? "通过" : "失败"}（坏样例 ${r.bad}、正例 ${r.good}）`);
        return r.ok ? 0 : 1;
    }
    if (cmd === "signoff") {
        print({ signoff: SIGNOFF });
        return 0;
    }
    const file = opt("--file");
    if (!file) throw new Error("需要 --file");
    const needles = () => loadPersonalNeedles({ mode: needlesMode(rest) }).needles;
    if (cmd === "text") {
        print({ findings: textFindings(plainText(fs.readFileSync(file), "文本"), needles()) });
    } else if (cmd === "texts") {
        // 一次扫多段文本（合并核对要复扫 PR 正文与我方全部评论），针只读一次
        const list = needles();
        const texts = JSON.parse(fs.readFileSync(file, "utf8"));
        print({
            results: texts.map((t, i) => ({ findings: textFindings(plainText(String(t), `文本 ${i + 1}`), list) })),
        });
    } else if (cmd === "message") {
        const text = plainText(fs.readFileSync(file), "提交消息");
        print(checkMessage(text, { tier: "same", exactlyOne: true, personal: needles() }));
    } else if (cmd === "review-state") {
        const comments = JSON.parse(fs.readFileSync(file, "utf8"));
        const writers = (opt("--writers") || "").split(",").filter(Boolean);
        print(reviewState(comments, writers, opt("--bot") || "claude"));
    } else if (cmd === "infra") {
        const paths = JSON.parse(fs.readFileSync(file, "utf8"));
        const m = await import("../../lib/infra-paths.mjs").catch((e) => {
            const missing = e.code === "ERR_MODULE_NOT_FOUND" && /infra-paths\.mjs/.test(e.message);
            throw new Error(
                missing
                    ? "缺 scripts/lib/infra-paths.mjs（基础设施路径的唯一清单），判不了要不要交维护者批准"
                    : `加载 scripts/lib/infra-paths.mjs 失败：${e.message}`,
            );
        });
        const list = opt("--list");
        if (typeof m.infraMatcher !== "function" || typeof m.isInfraPath !== "function") {
            throw new Error("scripts/lib/infra-paths.mjs 没有导出 infraMatcher / isInfraPath");
        }
        const match = list ? m.infraMatcher(JSON.parse(fs.readFileSync(list, "utf8"))) : m.isInfraPath;
        if (typeof match !== "function") throw new Error("infraMatcher 没有返回判定函数");
        print({ hits: paths.filter((p) => match(p)) });
    } else {
        throw new Error("用法见文件头注释：text | texts | message | review-state | infra | signoff");
    }
    return 0;
}

if (isMain(import.meta.url)) {
    main(process.argv.slice(2)).then(
        (code) => {
            process.exitCode = code;
        },
        (e) => {
            process.stdout.write(`${JSON.stringify({ error: e.message })}\n`);
            process.exitCode = 2;
        },
    );
}
