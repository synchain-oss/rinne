// SPDX-License-Identifier: GPL-3.0-or-later
//
// 提交与 PR 的卫生门禁。四个子命令共用一套判据，commit-msg / pre-push 两个 hook 也调这里：
//   commits         --range <A..B> | --json <PR 提交列表>  [--tier same|fork|push]
//                   DCO、只允许 Signed-off-by 一种尾注、AI 署名、subject 格式、作者与提交者邮箱
//   pr-text         标题与正文取 --title / --body-file，缺省读环境变量 PR_TITLE / PR_BODY
//                   [--fork --author "名字 <邮箱>"]；隐私规则、AI 署名、正文末行签名
//   branch          --head <名> [--base <名>] [--fork]；按 base 分命名规则，名字里不得有私有编号
//   contract-guard  --base <ref> [--head <ref>] [--body-file <f>]；改到冻结契约面时，要求正文写
//                   contract-neutral: <理由>，或同 PR 新增 docs/contract-changes/<YYYYMMDD>-<slug>.md
//   --self-test
// 提交分三档：same 同仓（维护者身份、签名逐字一致）；fork 外部贡献（DCO 本义：有与作者一致的签名，
// AI 署名只告警）；push 推送后兜底（提交者可以是 GitHub 网页合并，作者不在白名单时按 fork 判）。

import fs from "node:fs";
import { assertNodeVersion, caseResult, git, isMain, j } from "./lib/check-kit.mjs";
import { loadPersonalNeedles, redact, scan } from "./lib/privacy-needles.mjs";

export const MAINTAINER = "DLsnows <noreply@synchain.ca>";
export const SIGNOFF = `Signed-off-by: ${MAINTAINER}`;
const SAME_AUTHORS = ["noreply@synchain.ca", "140901583+dlsnows@users.noreply.github.com"];
const COMMITTERS = ["noreply@synchain.ca", "noreply@github.com"];
const DEPENDABOT = "49699333+dependabot[bot]@users.noreply.github.com";
export const SUBJECT =
    /^(?:feat|fix|docs|test|refactor|perf|ci|build|chore|style|revert|harden|release|merge)\([a-z0-9][a-z0-9/-]*\): \S/;
const GIT_SUBJECT = /^(?:Merge (?:remote-tracking )?branch '[^']+'(?: of \S+)?(?: into \S+)?|Revert ".+")$/;
const SIGNOFF_LINE = /^signed-off-by:/i;
const TRAILER = /^[A-Za-z][A-Za-z0-9-]*:\s+\S/;
const AI_TRAILER = new RegExp(j("^(?:co-authored", "-by|cla", "ude-session):"), "i");

/** 隐私与 AI 署名：fork 档 AI 署名只告警，外部作者签名行里的个人邮箱不算泄漏。 */
function textFindings(text, { fork = false, personal = [], label = "" } = {}) {
    const errors = [];
    const warns = [];
    const lines = text.split(/\r?\n/);
    for (const f of scan(text, {}, { personal })) {
        if (fork && f.rule.id === "personal-email" && SIGNOFF_LINE.test(lines[f.line - 1])) continue;
        (fork && f.rule.group === "ai" ? warns : errors).push(`${label}第 ${f.line} 行 [${f.rule.id}] ${redact(f)}`);
    }
    return { errors, warns };
}

/** 一条提交消息。author = { name, email }；merge 时免 subject 与签名（合并提交不带新内容）。 */
export function checkMessage(message, opts = {}) {
    const { tier = "same", author, merge = false, exactlyOne = false, personal = [] } = opts;
    // 推送兜底档里，作者不在白名单的提交来自外部贡献者，按 fork 档判
    const fork = tier === "fork" || (tier === "push" && !!author && !SAME_AUTHORS.includes(author.email.toLowerCase()));
    const text = message.replace(/\r\n/g, "\n").trimEnd();
    const { errors, warns } = textFindings(text, { fork, personal, label: "消息" });
    const lines = text.split("\n");
    const subjectOk = merge || SUBJECT.test(lines[0]) || GIT_SUBJECT.test(lines[0]);
    if (!subjectOk) errors.push("subject 必须是 type(scope): 描述");
    if (lines.some((l) => AI_TRAILER.test(l))) (fork ? warns : errors).push("不允许 AI 署名尾注");
    const paras = text.split(/\n\s*\n/);
    const last = paras.at(-1).split("\n");
    if (paras.length > 1 && last.every((l) => TRAILER.test(l)) && last.some((l) => !SIGNOFF_LINE.test(l))) {
        (fork ? warns : errors).push("尾注只允许 Signed-off-by 一种");
    }
    if (merge) return { errors, warns };
    const signoffs = lines.filter((l) => SIGNOFF_LINE.test(l)).map((l) => l.trim());
    if (fork) {
        const own = author && `signed-off-by: ${author.name} <${author.email}>`.toLowerCase();
        if (!signoffs.some((s) => s.toLowerCase() === own)) errors.push("缺少与作者一致的 Signed-off-by（DCO）");
    } else if (!signoffs.length) {
        errors.push("缺少 Signed-off-by（用 git commit -s）");
    } else if (signoffs.some((s) => s !== SIGNOFF)) {
        errors.push(`每一行 Signed-off-by 都必须是「${SIGNOFF}」`);
    } else if (exactlyOne && signoffs.length !== 1) {
        errors.push("只能有一行 Signed-off-by");
    }
    return { errors, warns };
}

/** 一个提交：c = { sha, parents, author: {name, email}, committer: {name, email}, message }。 */
export function checkCommit(c, { tier = "same", personal = [] } = {}) {
    const ae = c.author.email.toLowerCase();
    if (ae === DEPENDABOT && tier !== "fork") return { errors: [], warns: [] };
    const errors = [];
    if (tier === "same" && !SAME_AUTHORS.includes(ae)) errors.push("作者邮箱不在同仓白名单");
    if (tier !== "fork" && !COMMITTERS.includes(c.committer.email.toLowerCase())) errors.push("提交者邮箱不在白名单");
    const r = checkMessage(c.message, { tier, author: c.author, merge: c.parents > 1, personal });
    return { errors: [...errors, ...r.errors], warns: r.warns };
}

export function checkPrText({ title, body, fork = false, author, personal = [] }) {
    const t = textFindings(title, { fork, personal, label: "标题" });
    const b = textFindings(body, { fork, personal, label: "正文" });
    const errors = [...t.errors, ...b.errors];
    if (!SUBJECT.test(title)) errors.push("PR 标题必须是 type(scope): 描述");
    const last = body.trimEnd().split(/\r?\n/).at(-1).trim();
    const want = fork ? `Signed-off-by: ${author}` : SIGNOFF;
    if (fork ? last.toLowerCase() !== want.toLowerCase() : last !== want) {
        errors.push(`正文最后一行必须是「${fork ? "Signed-off-by: <作者>" : SIGNOFF}」`);
    }
    return { errors, warns: [...t.warns, ...b.warns] };
}

const LONG_LIVED = /^(?:dev|prod|staging|main|master|feature\/.*)$/;
export function checkBranch({ head, base, fork = false }) {
    if (fork) return LONG_LIVED.test(head) ? ["fork 的分支不得使用长期分支名"] : [];
    const want =
        !base || base.startsWith("feature/")
            ? /^feat\/[a-z0-9][a-z0-9-]*$/
            : base === "dev"
              ? /^(?:feature\/[a-z0-9][a-z0-9.-]*|dependabot\/\S+)$/
              : base === "prod"
                ? /^dev$/
                : null;
    const errors = [];
    if (!want) errors.push("base 只能是 feature/*、dev 或 prod");
    else if (!want.test(head)) errors.push(`分支名不符合 base=${base || "feature/*"} 的命名规则`);
    for (const f of scan(head, {})) errors.push(`分支名含 [${f.rule.id}] ${redact(f)}`);
    return errors;
}

export const CONTRACTS = {
    ipc: "docs/contracts/IPC_CONTRACT.md",
    params: "docs/contracts/PARAMETERS.md",
    state: "docs/contracts/STATE_SCHEMA.md",
};
// [路径, 所属契约（* = 任一契约冻结即受守）, 只看哪些改动行]
const GUARDED = [
    [/^docs\/contracts\/IPC_CONTRACT\.md$|^src\/core\/ipc\/[^/]*Layout[^/]*\.h$/, "ipc"],
    [/^tests\/golden\/ipc-layout\.json$/, "ipc"],
    [/^docs\/contracts\/STATE_SCHEMA\.md$|^src\/core\/state\//, "state"],
    [/^tests\/golden\/state(?:-schema\.json$|\/)/, "state"],
    [/^docs\/contracts\/PARAMETERS\.md$|^tests\/golden\/(?:params-input|identity)\.json$/, "params"],
    [/^src\/input\/ParamTable\.h$/, "params", /\bRINNE_PARAM\b/],
    [/(?:^|\/)CMakeLists\.txt$|\.cmake$/, "params", /\b(?:PLUGIN_(?:MANUFACTURER_)?CODE|BUNDLE_ID|AU_[A-Z_]+)\b/],
    [/^docs\/contracts\/|^tests\/golden\//, "*"],
];
const lineFiltered = (p) => !!p && GUARDED.some(([re, , lines]) => lines && re.test(p));

/**
 * changes: [{ status: "A"|"M"|"D"|"R…", path, oldPath?, lines?: 改动行（只有按行判的路径需要） }]；
 * statusOf(契约文档) 返回 base 上该文档的 status 行（frozen / draft），文档不存在时返回 null。
 */
export function contractGuard({ changes, statusOf, body = "" }) {
    const frozen = (c) =>
        (c === "*" ? Object.values(CONTRACTS) : [CONTRACTS[c]]).some((doc) => statusOf(doc) === "frozen");
    const touched = [];
    for (const ch of changes) {
        for (const p of [ch.path, ch.oldPath].filter(Boolean)) {
            const g = GUARDED.find(([re]) => re.test(p));
            if (!g || (g[2] && !(ch.lines || []).some((l) => g[2].test(l)))) continue;
            if (frozen(g[1])) touched.push(`  ${p}`);
        }
    }
    if (!touched.length) return [];
    const neutral = /^contract-neutral:[ \t]*(?!<)\S/m.test(body);
    const doc = changes.some((c) => c.status === "A" && /^docs\/contract-changes\/\d{8}-[a-z0-9-]+\.md$/.test(c.path));
    if (neutral || doc) return [];
    const how = "PR 正文写 contract-neutral: <理由>，或同 PR 新增 docs/contract-changes/<YYYYMMDD>-<slug>.md";
    return [`改到已冻结的契约面：${how}`, ...touched];
}

function gitChanges(base, head, cwd) {
    const range = `${base}...${head}`;
    const tok = git(["diff", "--name-status", "-M", "-z", range], { cwd }).split("\0");
    const changes = [];
    for (let i = 0; i + 1 < tok.length; ) {
        const status = tok[i];
        const two = /^[RC]/.test(status);
        const ch = two ? { status, oldPath: tok[i + 1], path: tok[i + 2] } : { status, path: tok[i + 1] };
        i += two ? 3 : 2;
        if (lineFiltered(ch.path) || lineFiltered(ch.oldPath)) {
            const paths = [ch.oldPath, ch.path].filter(Boolean);
            ch.lines = git(["diff", "-U0", "--no-color", range, "--", ...paths], { cwd })
                .split("\n")
                .filter((l) => /^[+-]/.test(l) && !/^(?:\+\+\+|---) /.test(l));
        }
        changes.push(ch);
    }
    return changes;
}

function readCommits({ range, json }, cwd) {
    if (json) {
        const list = JSON.parse(fs.readFileSync(json, "utf8"));
        if (!Array.isArray(list)) throw new Error("提交列表不是 JSON 数组");
        // 列表接口最多返回 250 个提交，到顶时后面的提交根本不在列表里，只能整体判红
        if (list.length < 1 || list.length >= 250) throw new Error(`提交数 ${list.length} 不在 1–249 之内`);
        return list.map((c) => ({
            sha: c.sha,
            parents: c.parents.length,
            author: c.commit.author,
            committer: c.commit.committer,
            message: c.commit.message,
        }));
    }
    const out = git(["log", "--format=%H%x1f%P%x1f%an%x1f%ae%x1f%cn%x1f%ce%x1f%B%x1e", range], { cwd });
    return out
        .split("\x1e")
        .map((s) => s.replace(/^\n/, ""))
        .filter(Boolean)
        .map((s) => {
            const [sha, parents, an, ae, cn, ce, message] = s.split("\x1f");
            return {
                sha,
                parents: parents.split(" ").filter(Boolean).length,
                author: { name: an, email: ae },
                committer: { name: cn, email: ce },
                message,
            };
        });
}

// ---- 自测 ------------------------------------------------------------------------------
const ME = { name: "DLsnows", email: "noreply@synchain.ca" };
const OUTSIDER = { name: "Ada Lee", email: j("ada", "@", "gmail", ".com") };
const msg = (...body) => ["ci(checks): 加入检查", "", "说明一行。", "", ...body].join("\n");
const commit = (message, extra = {}) => ({ parents: 1, author: ME, committer: ME, message, ...extra });

export function selfTest() {
    const cases = [];
    const t = (label, bad, errors) => cases.push({ label, bad, passed: bad ? errors.length > 0 : errors.length === 0 });
    const c = (label, bad, x, tier) => t(label, bad, checkCommit(x, { tier }).errors);
    c("同仓：维护者身份与签名", false, commit(msg(SIGNOFF)));
    c("同仓：多行相同签名（squash 合并）", false, commit(msg(SIGNOFF, SIGNOFF)));
    c("同仓：混入别的签名", true, commit(msg(SIGNOFF, "Signed-off-by: Other <other@example.com>")));
    c("同仓：缺签名", true, commit(msg()));
    c("同仓：协作者尾注", true, commit(msg(j("Co-Authored", "-By: Someone <s@example.com>"), SIGNOFF)));
    c("同仓：会话尾注", true, commit(msg(j("Cla", "ude-Session: abc"), SIGNOFF)));
    c("同仓：生成声明", true, commit(msg(j("Generated with Cla", "ude Code"), "", SIGNOFF)));
    c("同仓：AI 邮箱", true, commit(msg(j("见 noreply@anthr", "opic.com"), "", SIGNOFF)));
    c("同仓：其他尾注", true, commit(msg("Reviewed-by: Someone <s@example.com>", SIGNOFF)));
    c("同仓：subject 格式不对", true, commit(`update stuff\n\n${SIGNOFF}`));
    c("同仓：subject 缺 scope", true, commit(`ci: 加入检查\n\n${SIGNOFF}`));
    c("同仓：subject 含卡号", true, commit(`ci(checks): 完成 ${j("R", "N-01a")}\n\n${SIGNOFF}`));
    c("同仓：作者不在白名单", true, commit(msg(SIGNOFF), { author: OUTSIDER }));
    const merge = "Merge remote-tracking branch 'origin/feature/v1' into feat/x";
    c("同仓：git 默认合并 subject", false, commit(`${merge}\n\n${SIGNOFF}`));
    c("同仓：合并提交免签名", false, commit("Merge pull request #3 from synchain-oss/feat/x", { parents: 2 }));
    c("dependabot", false, commit("ci(deps): bump x", { author: { name: "dependabot[bot]", email: DEPENDABOT } }));
    const own = `Signed-off-by: ${OUTSIDER.name} <${OUTSIDER.email}>`;
    c("fork：作者真实邮箱 + 自签名", false, commit(msg(own), { author: OUTSIDER, committer: OUTSIDER }), "fork");
    c("fork：签名与作者不一致", true, commit(msg(SIGNOFF), { author: OUTSIDER, committer: OUTSIDER }), "fork");
    const forkAi = checkCommit(commit(msg(j("Co-Authored", "-By: Bot <b@example.com>"), own), { author: OUTSIDER }), {
        tier: "fork",
    });
    t("fork：AI 尾注只告警", false, forkAi.warns.length ? forkAi.errors : ["没有告警"]);
    const webflow = { name: "GitHub", email: "noreply@github.com" };
    c("推送兜底：网页合并外部贡献", false, commit(msg(own), { author: OUTSIDER, committer: webflow }), "push");
    c("推送兜底：维护者作者却缺签名", true, commit(msg(), { committer: webflow }), "push");
    t("commit-msg：恰好一行签名", true, checkMessage(msg(SIGNOFF, SIGNOFF), { exactlyOne: true }).errors);

    const b = (label, bad, args) => t(label, bad, checkBranch(args));
    b("分支：feat/<slug>", false, { head: "feat/check-scripts", base: "feature/v1" });
    b("分支：不是 feat/", true, { head: "fix/check-scripts", base: "feature/v1" });
    b("分支：含卡号", true, { head: j("feat/r", "n-01a-checks") });
    b("分支：含条件卡号", true, { head: j("feat/k", "8-fallback") });
    b("分支：dev 到 prod", false, { head: "dev", base: "prod" });
    b("分支：主支线直接到 prod", true, { head: "feature/v1", base: "prod" });
    b("分支：主支线到 dev", false, { head: "feature/v1", base: "dev" });
    b("分支：fork 用长期分支名", true, { head: "dev", base: "dev", fork: true });
    b("分支：fork 普通分支", false, { head: "patch-1", base: "dev", fork: true });

    const body = (...l) => ["## 做了什么", "加入检查脚本。【重要】项已清零。", "", ...l].join("\n");
    const pr = (label, bad, args) => t(label, bad, checkPrText({ title: "ci(checks): 加入检查", ...args }).errors);
    pr("PR：正文末行签名", false, { body: body(SIGNOFF) });
    pr("PR：正文缺签名", true, { body: body("谢谢") });
    pr("PR：标题含卡号", true, { title: `ci(checks): ${j("R", "N05")} 收尾`, body: body(SIGNOFF) });
    pr("PR：正文含生成声明", true, { body: body(j("Generated with [Cla", "ude Code]"), SIGNOFF) });
    pr("PR：正文含私有章节号", true, { body: body(j("见 0", "6 §", "2"), SIGNOFF) });
    pr("PR：fork 作者签名", false, { fork: true, author: `${OUTSIDER.name} <${OUTSIDER.email}>`, body: body(own) });

    const frozenIpc = (doc) => (doc === CONTRACTS.ipc ? "frozen" : "draft");
    const g = (label, bad, changes, extra = {}) =>
        t(label, bad, contractGuard({ changes, statusOf: frozenIpc, body: "", ...extra }));
    const layout = { status: "M", path: "src/core/ipc/SegmentLayout.h" };
    g("契约：改冻结布局且无说明", true, [layout]);
    g("契约：contract-neutral 理由", false, [layout], { body: "contract-neutral: 只改注释" });
    g("契约：contract-neutral 占位符不算", true, [layout], { body: "contract-neutral: <理由>" });
    g("契约：同 PR 新增变更说明", false, [layout, { status: "A", path: "docs/contract-changes/20261101-ipc-v2.md" }]);
    const renamed = { status: "R100", oldPath: "x.md", path: "docs/contract-changes/20261101-a.md" };
    g("契约：改名来的说明不算", true, [layout, renamed]);
    g("契约：仍是 draft 时放行", false, [layout], { statusOf: () => "draft" });
    g("契约：文档尚未建立时放行", false, [layout], { statusOf: () => null });
    g("契约：把冻结文件改名移走", true, [{ status: "R090", oldPath: "tests/golden/ipc-layout.json", path: "x.json" }]);
    g("契约：其余 golden 在任一契约冻结时受守", true, [{ status: "M", path: "tests/golden/curve.json" }]);
    const frozenParams = { statusOf: (doc) => (doc === CONTRACTS.params ? "frozen" : null) };
    const cmake = (lines) => [{ status: "M", path: "src/input/CMakeLists.txt", lines }];
    g("契约：CMake 身份行", true, cmake(["-    PLUGIN_CODE Rnin", "+    PLUGIN_CODE Rnix"]), frozenParams);
    g("契约：CMake 非身份行", false, cmake(["+    target_sources(x)"]), frozenParams);
    const table = (lines) => [{ status: "M", path: "src/input/ParamTable.h", lines }];
    g("契约：参数表宏行", true, table(["+RINNE_PARAM(feedback, 1, 0, 1, 1, 0, pct)"]), frozenParams);
    g("契约：参数表注释行", false, table(["+// 说明"]), frozenParams);
    return caseResult(cases);
}

function main(argv) {
    assertNodeVersion();
    const [cmd, ...rest] = argv;
    const opt = (k) => (rest.includes(k) ? rest[rest.indexOf(k) + 1] : undefined);
    const has = (k) => rest.includes(k);
    if (cmd === "--self-test") {
        const r = selfTest();
        for (const l of r.lines) console.error(l);
        console.log(`check-commits --self-test：${r.ok ? "通过" : "失败"}（坏样例 ${r.bad}、正例 ${r.good}）`);
        return r.ok ? 0 : 1;
    }
    const cwd = process.cwd();
    const personal = () => loadPersonalNeedles({ cwd });
    const body = () => (opt("--body-file") ? fs.readFileSync(opt("--body-file"), "utf8") : (process.env.PR_BODY ?? ""));
    let errors = [];
    let warns = [];
    if (cmd === "commits") {
        const tier = opt("--tier") || "same";
        if (!["same", "fork", "push"].includes(tier)) throw new Error(`未知档位 ${tier}`);
        if (!opt("--range") === !opt("--json")) throw new Error("commits 需要 --range 或 --json 其中之一");
        const { needles, notes } = personal();
        notes.forEach((n) => console.log(n));
        const list = readCommits({ range: opt("--range"), json: opt("--json") }, cwd);
        for (const c of list) {
            const r = checkCommit(c, { tier, personal: needles });
            errors.push(...r.errors.map((e) => `${c.sha.slice(0, 7)} ${e}`));
            warns.push(...r.warns.map((w) => `${c.sha.slice(0, 7)} ${w}`));
        }
        console.log(`检查了 ${list.length} 个提交（${tier}）`);
    } else if (cmd === "pr-text") {
        const { needles, notes } = personal();
        notes.forEach((n) => console.log(n));
        const title = opt("--title") ?? process.env.PR_TITLE ?? "";
        const args = { title, body: body(), fork: has("--fork"), author: opt("--author"), personal: needles };
        ({ errors, warns } = checkPrText(args));
    } else if (cmd === "branch") {
        errors = checkBranch({ head: opt("--head") || "", base: opt("--base"), fork: has("--fork") });
    } else if (cmd === "contract-guard") {
        const base = opt("--base");
        if (!base) throw new Error("contract-guard 需要 --base");
        // 冻结状态取 base 上的契约文档：同一个 PR 把 frozen 改回 draft 不能顺带给自己放行
        const statusOf = (doc) => {
            const text = git(["show", `${base}:${doc}`], { cwd, allowFail: true });
            return text === null ? null : (/^status:\s*(\w+)/m.exec(text)?.[1] ?? null);
        };
        errors = contractGuard({ changes: gitChanges(base, opt("--head") || "HEAD", cwd), statusOf, body: body() });
    } else {
        console.error("用法见文件头注释：commits | pr-text | branch | contract-guard | --self-test");
        return 1;
    }
    for (const w of warns) console.log(`::warning::${w}`);
    for (const e of errors) console.error(`  ${e}`);
    console.log(`check-commits ${cmd}：${errors.length ? `失败（${errors.length} 项）` : "通过"}`);
    return errors.length ? 1 : 0;
}

if (isMain(import.meta.url)) {
    try {
        process.exitCode = main(process.argv.slice(2));
    } catch (e) {
        console.error(`check-commits：${e.message}`);
        process.exitCode = 1;
    }
}
