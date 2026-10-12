// SPDX-License-Identifier: GPL-3.0-or-later
//
// 提交与 PR 的卫生门禁。四个子命令共用一套判据，commit-msg / pre-push 两个 hook 也调这里：
//   commits         --range <A..B> | --json <PR 提交列表> [--expect <n>]  [--tier same|fork|push]
//                   DCO、只允许 Signed-off-by 一种尾注、AI 署名、subject 格式、作者与提交者邮箱
//   pr-text         标题与正文取 --title / --body-file，缺省读环境变量 PR_TITLE / PR_BODY；
//                   [--fork --author "名字 <邮箱>" | --commits-json <PR 提交列表>]；隐私规则、AI 署名、正文末行签名
//   branch          --head <名> [--base <名>] [--fork]；按 base 分命名规则，名字里不得有私有编号与个人针
//   contract-guard  --base <ref> [--head <ref>] [--body-file <f>]；改到冻结契约面时，要求正文写
//                   contract-neutral: <理由>，或同 PR 新增 docs/contract-changes/<YYYYMMDD>-<slug>.md
//   dependabot      --json <PR 提交列表>；每个提交都是 dependabot 的（作者与提交者都对得上）时退出 0，否则 1
//   --needles local|ci|ci-optional（个人针来源，见 privacy-needles.mjs）  --self-test
// 提交分三档：same 同仓（维护者身份、签名逐字一致）；fork 外部贡献（DCO 本义：有与作者一致的签名，
// AI 署名只告警）；push 推送后兜底（提交者可以是 GitHub 网页合并，作者不在白名单时按 fork 判）。

import fs from "node:fs";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { assertNodeVersion, caseResult, git, isMain, j, tempRepo } from "./lib/check-kit.mjs";
import * as needles from "./lib/privacy-needles.mjs";

const SELF = fileURLToPath(import.meta.url);

const { FAKE_NEEDLE, mask, plainText, redact, scan } = needles;

export const MAINTAINER = "DLsnows <noreply@synchain.ca>";
export const SIGNOFF = `Signed-off-by: ${MAINTAINER}`;
const SAME_AUTHORS = ["noreply@synchain.ca", "140901583+dlsnows@users.noreply.github.com"];
const WEB_FLOW = "noreply@github.com";
const COMMITTERS = ["noreply@synchain.ca", WEB_FLOW];
const DEPENDABOT = "49699333+dependabot[bot]@users.noreply.github.com";
export const SUBJECT =
    /^(?:feat|fix|docs|test|refactor|perf|ci|build|chore|style|revert|harden|release|merge)\([a-z0-9][a-z0-9/-]*\): \S/;
const GIT_SUBJECT = /^(?:Merge (?:remote-tracking )?branch '[^']+'(?: of \S+)?(?: into \S+)?|Revert ".+")$/;
// 尾注按 git 的写法认：键前可有空白，冒号前也可有空白（git interpret-trailers 都当尾注）
const SIGNOFF_LINE = /^\s*signed-off-by\s*:/i;
const TRAILER = /^\s*[A-Za-z0-9][A-Za-z0-9-]*\s*:(?!\/\/)\s*\S/;
const AI_TRAILER = new RegExp(j("^\\s*(?:co-authored", "-by|cla", "ude-session)\\s*:"), "i");
const DIFF = ["--no-ext-diff", "--no-textconv"];

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

/** 最后一段里的尾注行。按 git 的口径：整段都是尾注，或至少四分之一是尾注且含 Signed-off-by，才算尾注块。 */
function trailers(text) {
    const paras = text.split(/\n\s*\n/);
    if (paras.length < 2) return [];
    const lines = paras.at(-1).split("\n");
    const found = lines.filter((l) => TRAILER.test(l));
    const block =
        found.length === lines.length || (found.length * 4 >= lines.length && found.some((l) => SIGNOFF_LINE.test(l)));
    return block ? found : [];
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
    if (trailers(text).some((l) => !SIGNOFF_LINE.test(l)))
        (fork ? warns : errors).push("尾注只允许 Signed-off-by 一种");
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

/** dependabot 的提交：作者邮箱可以伪造，所以提交者也要是 GitHub。 */
export const isDependabot = (c) =>
    c.author.email.toLowerCase() === DEPENDABOT && c.committer.email.toLowerCase() === WEB_FLOW;

/** 一个提交：c = { sha, parents, author: {name, email}, committer: {name, email}, message }。 */
export function checkCommit(c, { tier = "same", personal = [] } = {}) {
    const ae = c.author.email.toLowerCase();
    const ce = c.committer.email.toLowerCase();
    // dependabot 只免 subject 与签名，隐私规则照扫
    if (isDependabot(c) && tier !== "fork") return textFindings(c.message, { fork: true, personal });
    const errors = [];
    if (tier === "same" && !SAME_AUTHORS.includes(ae)) errors.push("作者邮箱不在同仓白名单");
    if (tier !== "fork" && !COMMITTERS.includes(ce)) errors.push("提交者邮箱不在白名单");
    const r = checkMessage(c.message, { tier, author: c.author, merge: c.parents > 1, personal });
    return { errors: [...errors, ...r.errors], warns: r.warns };
}

/** authors：fork PR 时可接受的签名人（"名字 <邮箱>"），正文末行须与其中之一一致。 */
export function checkPrText({ title, body, fork = false, authors = [], personal = [] }) {
    const t = textFindings(title, { fork, personal, label: "标题" });
    const b = textFindings(body, { fork, personal, label: "正文" });
    const errors = [...t.errors, ...b.errors];
    if (!SUBJECT.test(title)) errors.push("PR 标题必须是 type(scope): 描述");
    const last = body.trimEnd().split(/\r?\n/).at(-1).trim();
    if (fork) {
        const ok = authors.some((a) => last.toLowerCase() === `signed-off-by: ${a}`.toLowerCase());
        if (!ok) errors.push("正文最后一行必须是与提交作者一致的 Signed-off-by");
    } else if (last !== SIGNOFF) {
        errors.push(`正文最后一行必须是「${SIGNOFF}」`);
    }
    return { errors, warns: [...t.warns, ...b.warns] };
}

const LONG_LIVED = /^(?:dev|prod|staging|main|master|feature\/.*)$/;
export function checkBranch({ head, base, fork = false, personal = [] }) {
    if (fork) {
        // 外部贡献只进 dev：feature 主支线与 prod 都只收本仓的分支
        const errors = base === "dev" ? [] : ["fork 的 PR 只能开到 dev"];
        if (LONG_LIVED.test(head)) errors.push("fork 的分支不得使用长期分支名");
        return errors;
    }
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
    for (const f of scan(head, {}, { personal })) errors.push(`分支名含 [${f.rule.id}] ${redact(f)}`);
    return errors;
}

export const CONTRACTS = {
    ipc: "docs/contracts/IPC_CONTRACT.md",
    params: "docs/contracts/PARAMETERS.md",
    state: "docs/contracts/STATE_SCHEMA.md",
};
// [路径, 所属契约（* = 任一契约冻结即受守）, 只看哪些改动行]。按行判的路径只看 -U0 的改动行：
// CMake 里经变量间接给身份（set(X Rnni) 再 PLUGIN_CODE ${X}）、或值写在下一行的情形这里抓不到，
// 由身份 golden 的静态检查兜底。
export const GUARDED = [
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

/** 契约文档的 status 行：大小写、引号、强调、引用块都容忍；文档存在却读不出 draft / frozen 时为 invalid。 */
export function parseStatus(text) {
    const m = /^[\s>]*[*_]{0,2}status[*_]{0,2}\s*[:：]\s*[*_"'`]*\s*([A-Za-z]+)/im.exec(text);
    const v = m?.[1].toLowerCase();
    return v === "draft" || v === "frozen" ? v : "invalid";
}

/**
 * changes: [{ status: "A"|"M"|"D"|"R…", path, oldPath?, lines?: 改动行（只有按行判的路径需要） }]；
 * statusOf(契约文档) 返回 base 上该文档的状态，headStatusOf 返回本 PR 改完之后的状态：
 * frozen / draft / invalid，文档不存在时为 null。
 * - 本 PR 新增或改动的契约文档，status 行必须读得出来，否则判红：坏行一旦合进去，后面谁都绕不过。
 * - base 上读不出 status 的按冻结处理：contract-neutral 理由或变更说明照样能放行修它的 PR。
 */
export function contractGuard({ changes, statusOf, headStatusOf = () => null, body = "" }) {
    const errors = [];
    const touched = [];
    const contractDocs = Object.values(CONTRACTS);
    for (const ch of changes) {
        if (contractDocs.includes(ch.path) && !/^D/.test(ch.status) && headStatusOf(ch.path) === "invalid") {
            errors.push(`${ch.path} 改完后的 status 行无法解析（应为 status: draft 或 status: frozen）`);
        }
        for (const p of [ch.path, ch.oldPath].filter(Boolean)) {
            const g = GUARDED.find(([re]) => re.test(p));
            if (!g) continue;
            // 按行判的路径被改名或删除时整份算改动
            if (g[2] && !/^[RD]/.test(ch.status) && !(ch.lines || []).some((l) => g[2].test(l))) continue;
            const docs = g[1] === "*" ? contractDocs : [CONTRACTS[g[1]]];
            const states = docs.map(statusOf);
            if (states.some((s) => s === "frozen" || s === "invalid")) touched.push(`  ${p}`);
        }
    }
    const neutral = /^contract-neutral:[ \t]*(?!<)\S/m.test(body);
    const doc = changes.some((c) => c.status === "A" && /^docs\/contract-changes\/\d{8}-[a-z0-9-]+\.md$/.test(c.path));
    if (!touched.length || neutral || doc) return errors;
    const how = "PR 正文写 contract-neutral: <理由>，或同 PR 新增 docs/contract-changes/<YYYYMMDD>-<slug>.md";
    return [...errors, `改到已冻结（或 status 读不出来、按冻结处理）的契约面：${how}`, ...touched];
}

function gitChanges(base, head, cwd) {
    const range = `${base}...${head}`;
    const tok = git(["diff", ...DIFF, "--name-status", "-M", "-z", range], { cwd }).split("\0");
    const changes = [];
    for (let i = 0; i + 1 < tok.length;) {
        const status = tok[i];
        const two = /^[RC]/.test(status);
        const ch = two ? { status, oldPath: tok[i + 1], path: tok[i + 2] } : { status, path: tok[i + 1] };
        i += two ? 3 : 2;
        if (lineFiltered(ch.path) || lineFiltered(ch.oldPath)) {
            const paths = [ch.oldPath, ch.path].filter(Boolean);
            ch.lines = git(["diff", ...DIFF, "-U0", "--no-color", range, "--", ...paths], { cwd })
                .split("\n")
                .filter((l) => /^[+-]/.test(l) && !/^(?:\+\+\+|---) /.test(l));
        }
        changes.push(ch);
    }
    return changes;
}

/**
 * PR 提交列表：一个数组；gh api --paginate 首尾相接的多个数组；--slurp 再包一层的嵌套数组；
 * 或 --jq '.[]' 那样一行一个对象。都展平成提交对象的列表。
 */
export function parseCommitList(text) {
    const values = [];
    let depth = 0;
    let start = 0;
    let str = false;
    let esc = false;
    for (let i = 0; i < text.length; i++) {
        const c = text[i];
        if (str) {
            if (esc) esc = false;
            else if (c === "\\") esc = true;
            else if (c === '"') str = false;
        } else if (c === '"' && depth > 0) {
            str = true;
        } else if (c === "[" || c === "{") {
            if (depth++ === 0) start = i;
        } else if ((c === "]" || c === "}") && depth > 0) {
            if (--depth === 0) values.push(JSON.parse(text.slice(start, i + 1)));
        } else if (depth === 0 && !/\s/.test(c)) {
            throw new Error("提交列表里有无法解析的内容");
        }
    }
    if (depth || str) throw new Error("提交列表不完整");
    const flat = (v) => (Array.isArray(v) ? v.flatMap(flat) : [v]);
    const list = values.flatMap(flat);
    if (list.some((c) => !c?.sha || !c.commit?.author || !c.commit?.committer)) throw new Error("提交列表的条目缺字段");
    return list;
}

function readCommits({ range, json, expect }, cwd) {
    if (json) {
        const list = parseCommitList(fs.readFileSync(json, "utf8"));
        // 列表接口最多返回 250 个提交；只取到第一页时也会少，所以和 PR 的提交数对一次
        if (expect !== undefined && list.length !== Number(expect)) {
            throw new Error(`提交列表有 ${list.length} 个，PR 报告的是 ${expect} 个`);
        }
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
const WEB = { name: "GitHub", email: WEB_FLOW };
const BOT = { name: "dependabot[bot]", email: DEPENDABOT };
const msg = (...body) => ["ci(checks): 加入检查", "", "说明一行。", "", ...body].join("\n");
const commit = (message, extra = {}) => ({ parents: 1, author: ME, committer: ME, message, ...extra });
const apiCommit = (sha) => ({ sha, parents: [{}], commit: { author: ME, committer: ME, message: msg(SIGNOFF) } });

export function selfTest() {
    const cases = [];
    const t = (label, bad, errors) => cases.push({ label, bad, passed: bad ? errors.length > 0 : errors.length === 0 });
    const c = (label, bad, x, tier) => t(label, bad, checkCommit(x, { tier }).errors);
    const throws = (label, fn) => {
        let threw = false;
        try {
            fn();
        } catch {
            threw = true;
        }
        cases.push({ label, bad: true, passed: threw });
    };
    c("同仓：维护者身份与签名", false, commit(msg(SIGNOFF)));
    c("同仓：多行相同签名（squash 合并）", false, commit(msg(SIGNOFF, SIGNOFF)));
    c("同仓：混入别的签名", true, commit(msg(SIGNOFF, "Signed-off-by: Other <other@example.com>")));
    c("同仓：缺签名", true, commit(msg()));
    c("同仓：协作者尾注", true, commit(msg(j("Co-Authored", "-By: Someone <s@example.com>"), SIGNOFF)));
    c("同仓：冒号前有空格的协作者尾注", true, commit(msg(j("Co-authored", "-by : Someone <s@example.com>"), SIGNOFF)));
    c("同仓：行首有空格的协作者尾注", true, commit(msg(j(" Co-authored", "-by: Someone <s@example.com>"), SIGNOFF)));
    c("同仓：会话尾注", true, commit(msg(j("Cla", "ude-Session: abc"), SIGNOFF)));
    c("同仓：生成声明", true, commit(msg(j("Generated with Cla", "ude Code"), "", SIGNOFF)));
    c("同仓：另一种生成声明", true, commit(msg(j("Generated by Cla", "ude Code"), "", SIGNOFF)));
    c("同仓：AI 邮箱", true, commit(msg(j("见 noreply@anthr", "opic.com"), "", SIGNOFF)));
    c("同仓：其他尾注", true, commit(msg("Reviewed-by: Someone <s@example.com>", SIGNOFF)));
    c("同仓：尾注块里夹一行正文", true, commit(msg("说明", "Reviewed-by: Someone <s@example.com>", SIGNOFF)));
    c("同仓：变体签名", true, commit(msg("Signed-off-by : DLsnows <noreply@synchain.ca>")));
    c("同仓：subject 格式不对", true, commit(`update stuff\n\n${SIGNOFF}`));
    c("同仓：subject 缺 scope", true, commit(`ci: 加入检查\n\n${SIGNOFF}`));
    c("同仓：subject 含卡号", true, commit(`ci(checks): 完成 ${j("R", "N-01a")}\n\n${SIGNOFF}`));
    c("同仓：作者不在白名单", true, commit(msg(SIGNOFF), { author: OUTSIDER }));
    const merge = "Merge remote-tracking branch 'origin/feature/v1' into feat/x";
    c("同仓：git 默认合并 subject", false, commit(`${merge}\n\n${SIGNOFF}`));
    c("同仓：合并提交免签名", false, commit("Merge pull request #3 from synchain-oss/feat/x", { parents: 2 }));
    c("dependabot", false, commit("ci(deps): bump x", { author: BOT, committer: WEB }));
    c("伪造的 dependabot 作者", true, commit("ci(deps): bump x", { author: BOT }));
    c(
        "dependabot 消息照扫隐私",
        true,
        commit(`ci(deps): bump x\n\n${OUTSIDER.email}`, { author: BOT, committer: WEB }),
    );
    const own = `Signed-off-by: ${OUTSIDER.name} <${OUTSIDER.email}>`;
    c("fork：作者真实邮箱 + 自签名", false, commit(msg(own), { author: OUTSIDER, committer: OUTSIDER }), "fork");
    c("fork：签名与作者不一致", true, commit(msg(SIGNOFF), { author: OUTSIDER, committer: OUTSIDER }), "fork");
    const forkAi = checkCommit(commit(msg(j("Co-Authored", "-By: Bot <b@example.com>"), own), { author: OUTSIDER }), {
        tier: "fork",
    });
    t("fork：AI 尾注只告警", false, forkAi.warns.length ? forkAi.errors : ["没有告警"]);
    c("推送兜底：网页合并外部贡献", false, commit(msg(own), { author: OUTSIDER, committer: WEB }), "push");
    c("推送兜底：维护者作者却缺签名", true, commit(msg(), { committer: WEB }), "push");
    t("commit-msg：恰好一行签名", true, checkMessage(msg(SIGNOFF, SIGNOFF), { exactlyOne: true }).errors);

    const b = (label, bad, args) => t(label, bad, checkBranch({ personal: [FAKE_NEEDLE], ...args }));
    b("分支：feat/<slug>", false, { head: "feat/check-scripts", base: "feature/v1" });
    b("分支：不是 feat/", true, { head: "fix/check-scripts", base: "feature/v1" });
    b("分支：含卡号", true, { head: j("feat/r", "n-01a-checks") });
    b("分支：含条件卡号", true, { head: j("feat/k", "8-fallback") });
    b("分支：含个人针", true, { head: `feat/${FAKE_NEEDLE}` });
    b("分支：dev 到 prod", false, { head: "dev", base: "prod" });
    b("分支：主支线直接到 prod", true, { head: "feature/v1", base: "prod" });
    b("分支：主支线到 dev", false, { head: "feature/v1", base: "dev" });
    b("分支：fork 用长期分支名", true, { head: "dev", base: "dev", fork: true });
    b("分支：fork 普通分支", false, { head: "patch-1", base: "dev", fork: true });
    b("分支：fork 开到 prod", true, { head: "patch-1", base: "prod", fork: true });
    b("分支：fork 开到主支线", true, { head: "patch-1", base: "feature/v1", fork: true });

    const body = (...l) => ["## 做了什么", "加入检查脚本。【重要】项已清零。", "", ...l].join("\n");
    const pr = (label, bad, args) => t(label, bad, checkPrText({ title: "ci(checks): 加入检查", ...args }).errors);
    const outsider = `${OUTSIDER.name} <${OUTSIDER.email}>`;
    pr("PR：正文末行签名", false, { body: body(SIGNOFF) });
    pr("PR：正文缺签名", true, { body: body("谢谢") });
    pr("PR：标题含卡号", true, { title: `ci(checks): ${j("R", "N05")} 收尾`, body: body(SIGNOFF) });
    pr("PR：正文含生成声明", true, { body: body(j("Generated with [Cla", "ude Code]"), SIGNOFF) });
    pr("PR：正文含署名链接", true, { body: body(j("https://cla", "ude.com/cla", "ude-code"), SIGNOFF) });
    pr("PR：正文含私有章节号", true, { body: body(j("见 0", "6 §", "2"), SIGNOFF) });
    pr("PR：fork 作者签名", false, { fork: true, authors: [outsider], body: body(own) });
    pr("PR：fork 签名不是提交作者", true, { fork: true, authors: ["Bob <bob@example.com>"], body: body(own) });

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
    const badBase = { statusOf: (d) => (d === CONTRACTS.ipc ? "invalid" : "draft") };
    g("契约：base 上 status 读不出来，按冻结处理", true, [layout], badBase);
    g("契约：base 上 status 读不出来，contract-neutral 照样放行", false, [layout], {
        ...badBase,
        body: "contract-neutral: 只改注释",
    });
    const fixDoc = { status: "M", path: CONTRACTS.ipc };
    const fixed = { ...badBase, headStatusOf: () => "frozen" };
    g("契约：修 status 行的 PR 没有理由判红", true, [fixDoc], fixed);
    g(
        "契约：修 status 行的 PR 带变更说明放行",
        false,
        [fixDoc, { status: "A", path: "docs/contract-changes/20261101-fix.md" }],
        fixed,
    );
    const addBad = { statusOf: () => null, headStatusOf: (d) => (d === CONTRACTS.ipc ? "invalid" : null) };
    g("契约：新增的契约文档 status 读不出来判红", true, [{ status: "A", path: CONTRACTS.ipc }], addBad);
    g("契约：改坏 status 行即使有理由也判红", true, [fixDoc], {
        statusOf: () => "frozen",
        headStatusOf: () => "invalid",
        body: "contract-neutral: 冻结改回草案",
    });
    g("契约：把冻结文件改名移走", true, [{ status: "R090", oldPath: "tests/golden/ipc-layout.json", path: "x.json" }]);
    g("契约：其余 golden 在任一契约冻结时受守", true, [{ status: "M", path: "tests/golden/curve.json" }]);
    const frozenParams = { statusOf: (doc) => (doc === CONTRACTS.params ? "frozen" : null) };
    const cmake = (lines) => [{ status: "M", path: "src/input/CMakeLists.txt", lines }];
    g("契约：CMake 身份行", true, cmake(["-    PLUGIN_CODE Rnin", "+    PLUGIN_CODE Rnix"]), frozenParams);
    g("契约：CMake 非身份行", false, cmake(["+    target_sources(x)"]), frozenParams);
    const table = (lines, status = "M") => [{ status, path: "src/input/ParamTable.h", lines }];
    g("契约：参数表宏行", true, table(["+RINNE_PARAM(feedback, 1, 0, 1, 1, 0, pct)"]), frozenParams);
    g("契约：参数表注释行", false, table(["+// 说明"]), frozenParams);
    g(
        "契约：参数表整份改名",
        true,
        [{ status: "R100", oldPath: "src/input/ParamTable.h", path: "src/input/P.h" }],
        frozenParams,
    );
    g("契约：参数表被删除", true, table([], "D"), frozenParams);
    for (const s of [
        "Status: frozen",
        'status: "frozen"',
        "**status**: frozen",
        "> status: frozen",
        "status: Frozen",
    ]) {
        t(`契约 status 写法：${s}`, false, parseStatus(`# 契约\n\n${s}\n`) === "frozen" ? [] : ["没认出来"]);
    }
    t("契约 status：未知值", true, parseStatus("status: wip\n") === "invalid" ? ["invalid"] : []);
    t("契约 status：没有这一行", true, parseStatus("# 契约\n") === "invalid" ? ["invalid"] : []);

    const page = (...shas) => JSON.stringify(shas.map(apiCommit));
    t("提交列表：多页首尾相接", false, parseCommitList(`${page("a", "b")}${page("c")}`).length === 3 ? [] : ["x"]);
    t("提交列表：--slurp 嵌套", false, parseCommitList(`[${page("a")},${page("b")}]`).length === 2 ? [] : ["x"]);
    const ndjson = ["a", "b"].map((s) => JSON.stringify(apiCommit(s))).join("\n");
    t("提交列表：一行一个对象", false, parseCommitList(ndjson).length === 2 ? [] : ["x"]);
    throws("提交列表：截断的 JSON", () => parseCommitList(page("a").slice(0, -2)));
    throws("提交列表：条目缺字段", () => parseCommitList('[{"sha":"a"}]'));
    const botCommit = { author: BOT, committer: WEB };
    t("dependabot：作者与提交者都对得上", false, isDependabot(botCommit) ? [] : ["没认出"]);
    t("dependabot：提交者不是 GitHub 不算", true, isDependabot({ ...botCommit, committer: ME }) ? [] : ["x"]);

    // 端到端，经真实的 contract-guard 子命令复现卡死：坏 status 行先被挡住；
    // 若 base 上已经是坏行，修它的 PR 凭 contract-neutral 能合进去
    const repo = tempRepo();
    const nf = `${repo.dir}-needles.txt`;
    const bf = `${repo.dir}-body.md`;
    try {
        fs.writeFileSync(nf, `${FAKE_NEEDLE}\n`);
        const save = (m) => {
            repo.git(["add", "-A"]);
            repo.git(["commit", "-q", "-m", m]);
        };
        const guard = (text) => {
            fs.writeFileSync(bf, `${text}\n\n${SIGNOFF}\n`);
            const args = [SELF, "contract-guard", "--base", "main", "--body-file", bf];
            const env = { ...repo.env, RINNE_PRIVACY_LOCAL: nf };
            return spawnSync(process.execPath, args, { cwd: repo.dir, env, encoding: "utf8" }).status;
        };
        const e2e = (label, bad, code) =>
            cases.push({ label: `契约端到端：${label}`, bad, passed: code === (bad ? 1 : 0) });
        repo.write("README.md", "x\n");
        save("init");
        repo.git(["checkout", "-q", "-b", "add-doc"]);
        repo.write(CONTRACTS.ipc, "# IPC\n\n状态：frozen\n");
        save("docs: 加契约");
        e2e("新增的契约文档 status 读不出来判红", true, guard("说明"));
        // 模拟坏行已经在 base 上（例如来自这道检查之前）
        repo.git(["checkout", "-q", "main"]);
        repo.git(["merge", "-q", "--ff-only", "add-doc"]);
        repo.git(["checkout", "-q", "-b", "fix"]);
        repo.write(CONTRACTS.ipc, "# IPC\n\nstatus: frozen\n");
        save("docs: 修 status 行");
        e2e("base 上是坏行，修复 PR 没有理由判红", true, guard("说明"));
        e2e("base 上是坏行，修复 PR 凭 contract-neutral 放行", false, guard("contract-neutral: 只修 status 行"));
    } finally {
        repo.cleanup();
        fs.rmSync(nf, { force: true });
        fs.rmSync(bf, { force: true });
    }
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
    const personal = () => {
        const p = needles.loadPersonalNeedles({ cwd, mode: needles.needlesMode(rest) });
        p.notes.forEach((n) => console.log(n));
        return p.needles;
    };
    const body = () =>
        opt("--body-file")
            ? plainText(fs.readFileSync(opt("--body-file")), "PR 正文")
            : plainText(process.env.PR_BODY ?? "", "PR_BODY");
    let errors = [];
    let warns = [];
    if (cmd === "commits") {
        const tier = opt("--tier") || "same";
        if (!["same", "fork", "push"].includes(tier)) throw new Error(`未知档位 ${tier}`);
        if (!opt("--range") === !opt("--json")) throw new Error("commits 需要 --range 或 --json 其中之一");
        const needleList = personal();
        const list = readCommits({ range: opt("--range"), json: opt("--json"), expect: opt("--expect") }, cwd);
        for (const c of list) {
            const r = checkCommit(c, { tier, personal: needleList });
            errors.push(...r.errors.map((e) => `${c.sha.slice(0, 7)} ${e}`));
            warns.push(...r.warns.map((w) => `${c.sha.slice(0, 7)} ${w}`));
        }
        console.log(`检查了 ${list.length} 个提交（${tier}）`);
    } else if (cmd === "pr-text") {
        const title = plainText(opt("--title") ?? process.env.PR_TITLE ?? "", "PR 标题");
        const authors = [
            ...(opt("--author") ? [opt("--author")] : []),
            ...(opt("--commits-json")
                ? parseCommitList(fs.readFileSync(opt("--commits-json"), "utf8")).map(
                      (c) => `${c.commit.author.name} <${c.commit.author.email}>`,
                  )
                : []),
        ];
        ({ errors, warns } = checkPrText({ title, body: body(), fork: has("--fork"), authors, personal: personal() }));
    } else if (cmd === "dependabot") {
        // 只回答「是不是纯 dependabot 的 PR」，不输出别的：给 workflow 决定要不要跳过正文签名检查
        const list = parseCommitList(fs.readFileSync(opt("--json"), "utf8"));
        const all = list.length > 0 && list.every((c) => isDependabot(c.commit));
        console.log(`check-commits dependabot：${all ? "全部提交都是 dependabot 的" : "不是纯 dependabot 的 PR"}`);
        return all ? 0 : 1;
    } else if (cmd === "branch") {
        errors = checkBranch({
            head: opt("--head") || "",
            base: opt("--base"),
            fork: has("--fork"),
            personal: personal(),
        });
    } else if (cmd === "contract-guard") {
        const base = opt("--base");
        if (!base) throw new Error("contract-guard 需要 --base");
        const head = opt("--head") || "HEAD";
        // 冻结状态取 base 上的契约文档：同一个 PR 把 frozen 改回 draft 不能顺带给自己放行；
        // head 上的版本只用来确认 status 行读得出来
        const statusAt = (ref) => (doc) => {
            const text = git(["show", `${ref}:${doc}`], { cwd, allowFail: true });
            return text === null ? null : parseStatus(text);
        };
        const changes = gitChanges(base, head, cwd);
        // 输出里有路径：路径里的个人针与私有编号同样打码
        const list = personal();
        const args = { changes, statusOf: statusAt(base), headStatusOf: statusAt(head), body: body() };
        errors = contractGuard(args).map((e) => mask(e, list));
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
