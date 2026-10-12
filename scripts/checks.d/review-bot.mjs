// SPDX-License-Identifier: GPL-3.0-or-later
//
// 评审 bot 的结构检查（docs-truth 的一部分），守住三个评审 workflow 的安全前提：
//   - claude-review.yml：claude-pr-review 里恰好一个 claude-code-action 步骤；prompt 与 claude_args 过
//     review-bot.mjs 的结构核对（PR NUMBER 行、机读尾注格式、不可信数据声明是最后一节、只读的 --tools 与
//     --allowedTools、--disallowedTools 黑名单、无 Bash 放行），--model 是 claude-opus-5-5；不按 paths 过滤；
//     job 级 if 只放行非 draft、同仓、非 dependabot 的 PR；
//   - deepseek-review.yml：job 级 if 以 vars.ENABLE_DEEPSEEK == 'true' && 开头（没建变量时显示 skipped）；
//   - review-dispatch.yml：只认 PR 上以 /review 开头、OWNER / MEMBER / COLLABORATOR 发的评论；checkout 只签出目标分支。
// 解析沿用 workflows.mjs 的块式 YAML 读法。

import fs from "node:fs";
import path from "node:path";
import { caseResult } from "../lib/check-kit.mjs";
import { argValue, checkConfig, stepConfig, FIXTURE, selfTest as scriptSelfTest } from "../review-bot.mjs";
import { parseWorkflow } from "./workflows.mjs";

export const MODEL = "claude-opus-5-5";
const SAME_REPO = [
    "github.event.pull_request.draft == false",
    "github.event.pull_request.head.repo.full_name == github.repository",
    "github.event.pull_request.user.login != 'dependabot[bot]'",
];
const DISPATCH = [
    "github.event.issue.pull_request",
    "startsWith(github.event.comment.body, '/review')",
    `contains(fromJSON('["OWNER","MEMBER","COLLABORATOR"]'), github.event.comment.author_association)`,
];

/** job 级 if 的完整文本：块标量（>- 等）按 YAML 折叠成一行。 */
function jobIf(wf, id) {
    const job = wf.jobs.find((j) => j.id === id);
    if (!job) return null;
    const line = job.get("if");
    if (!line) return "";
    if (!/^[|>]/.test(line.value)) return line.value.replace(/^(["'])(.*)\1$/, "$2");
    const start = wf.lines.indexOf(line);
    const body = [];
    for (let i = start + 1; i < wf.lines.length && wf.lines[i].scalar === "if"; i++) body.push(wf.lines[i].raw.trim());
    return body.filter(Boolean).join(" ");
}

const hasPaths = (wf) => wf.section("on").some((l) => l.key === "paths" || l.key === "paths-ignore");

/** read(rel) 返回文本或 null。 */
export function inspectReviewWorkflows(read) {
    const errors = [];
    const cr = read(".github/workflows/claude-review.yml");
    if (cr === null) return ["缺 .github/workflows/claude-review.yml"];
    try {
        const raw = stepConfig(cr);
        for (const e of checkConfig(raw)) errors.push(`claude-review.yml：${e}`);
        if (argValue(raw.args, "model") !== MODEL) errors.push(`claude-review.yml：--model 应为 ${MODEL}`);
    } catch (e) {
        errors.push(`claude-review.yml：${e.message}`);
    }
    const wf = parseWorkflow(cr);
    if (hasPaths(wf)) errors.push("claude-review.yml 不许按 paths 过滤：纯文档 PR 也要审");
    const job = wf.jobs.find((j) => j.id === "claude-pr-review");
    if (!job || job.name !== "claude-pr-review")
        errors.push("claude-review.yml 的 job 与 check 名必须是 claude-pr-review");
    const cond = jobIf(wf, "claude-pr-review") || "";
    for (const c of SAME_REPO) if (!cond.includes(c)) errors.push(`claude-review.yml 的 job 级 if 缺「${c}」`);

    const ds = read(".github/workflows/deepseek-review.yml");
    if (ds === null) errors.push("缺 .github/workflows/deepseek-review.yml");
    else {
        const dwf = parseWorkflow(ds);
        const dc = jobIf(dwf, "deepseek-pr-review");
        if (dc === null) errors.push("deepseek-review.yml 缺 job deepseek-pr-review");
        else {
            if (!dc.startsWith("vars.ENABLE_DEEPSEEK == 'true' &&")) {
                errors.push("deepseek-review.yml 的 job 级 if 必须以 vars.ENABLE_DEEPSEEK == 'true' && 开头");
            }
            for (const c of SAME_REPO) if (!dc.includes(c)) errors.push(`deepseek-review.yml 的 job 级 if 缺「${c}」`);
        }
    }

    const rd = read(".github/workflows/review-dispatch.yml");
    if (rd === null) errors.push("缺 .github/workflows/review-dispatch.yml");
    else {
        const rwf = parseWorkflow(rd);
        const rc = jobIf(rwf, "dispatch");
        if (rc === null) errors.push("review-dispatch.yml 缺 job dispatch");
        else for (const c of DISPATCH) if (!rc.includes(c)) errors.push(`review-dispatch.yml 的 job 级 if 缺「${c}」`);
        if (/CONTRIBUTOR/.test(rc || "")) errors.push("review-dispatch.yml 不许放行 CONTRIBUTOR");
        // 每个 checkout 步骤都要显式签出目标分支，不许签出 PR 的代码
        const plain = rwf.plain;
        plain.forEach((l, i) => {
            if (l.key !== "uses" || !/^actions\/checkout@/.test(l.value)) return;
            const next = plain.findIndex((x, k) => k > i && x.indent === l.indent && /^\s*-\s/.test(x.code));
            const step = plain.slice(i, next < 0 ? plain.length : next);
            const ref = step.find((x) => x.key === "ref");
            if (!ref || ref.value !== "${{ steps.pr.outputs.base }}") {
                errors.push(
                    `review-dispatch.yml:${l.no} checkout 必须 ref: \${{ steps.pr.outputs.base }}（只签出目标分支）`,
                );
            }
        });
        if (/refs\/pull\/|gh pr checkout/.test(rd)) errors.push("review-dispatch.yml 不许签出 PR 的代码");
    }
    return errors;
}

const cond = (lines) => ["    if: >-", ...lines.map((l) => `      ${l}`)];
const CLAUDE = FIXTURE.replace(
    "    runs-on: ubuntu-latest",
    [
        "    name: claude-pr-review",
        ...cond(SAME_REPO.map((c, i) => (i < SAME_REPO.length - 1 ? `${c} &&` : c))),
        "    runs-on: ubuntu-latest",
    ].join("\n"),
);
const ON = "on:\n  pull_request:\n    types: [opened, synchronize]\n";
const DEEPSEEK = [
    "jobs:",
    "  deepseek-pr-review:",
    `    if: vars.ENABLE_DEEPSEEK == 'true' && ${SAME_REPO.join(" && ")}`,
    "    runs-on: ubuntu-latest",
].join("\n");
const REVIEW_DISPATCH = [
    "jobs:",
    "  dispatch:",
    ...cond(DISPATCH.map((c, i) => (i < DISPATCH.length - 1 ? `${c} &&` : c))),
    "    steps:",
    "      - uses: actions/checkout@3d3c42e5aac5ba805825da76410c181273ba90b1 # v7.0.1",
    "        with:",
    "          ref: ${{ steps.pr.outputs.base }}",
    "      - name: Next",
    "        run: echo",
].join("\n");

export default {
    name: "review-bot",
    group: "docs",
    inputs: [],
    run(ctx) {
        const read = (rel) => {
            const abs = path.join(ctx.root, rel);
            return fs.existsSync(abs) ? fs.readFileSync(abs, "utf8") : null;
        };
        const errors = inspectReviewWorkflows(read);
        if (errors.length) return { ok: false, lines: errors };
        return { ok: true, lines: ["claude-review、deepseek-review、review-dispatch 的结构前提都在"] };
    },
    selfTest() {
        const files = { claude: `${ON}${CLAUDE}`, deepseek: DEEPSEEK, dispatch: REVIEW_DISPATCH };
        const check = (over = {}) => {
            const f = { ...files, ...over };
            const map = {
                ".github/workflows/claude-review.yml": f.claude,
                ".github/workflows/deepseek-review.yml": f.deepseek,
                ".github/workflows/review-dispatch.yml": f.dispatch,
            };
            return inspectReviewWorkflows((rel) => map[rel] ?? null);
        };
        const swap = (key, a, b) => {
            if (!files[key].includes(a)) throw new Error(`自测夹具里找不到要替换的片段：${a}`);
            return check({ [key]: files[key].replace(a, b) });
        };
        const cases = [
            ["结构齐全的三个 workflow", false, check()],
            ["claude-review 换了模型", true, swap("claude", "--model claude-opus-5-5", "--model claude-haiku-5-5")],
            ["claude-review 按 paths 过滤", true, swap("claude", "    types:", '    paths: ["src/**"]\n    types:')],
            ["claude-review 放行 fork PR", true, swap("claude", `      ${SAME_REPO[1]} &&\n`, "")],
            ["claude-review 的 job 改了名", true, swap("claude", "    name: claude-pr-review", "    name: review")],
            ["claude-review 的 prompt 少了不可信数据声明", true, swap("claude", "## 不可信数据声明", "## 声明")],
            ["deepseek 的 if 不以开关开头", true, swap("deepseek", "vars.ENABLE_DEEPSEEK == 'true' && ", "")],
            ["deepseek 的开关挪到了后面", true, swap("deepseek", "vars.ENABLE_DEEPSEEK == 'true' && ", "true && ")],
            ["dispatch 不核对评论者身份", true, swap("dispatch", ` &&\n      ${DISPATCH[2]}`, "")],
            ["dispatch 放行 CONTRIBUTOR", true, swap("dispatch", '"COLLABORATOR"]', '"COLLABORATOR","CONTRIBUTOR"]')],
            ["dispatch 用 contains 认 /review", true, swap("dispatch", "startsWith(", "contains(")],
            ["dispatch 签出 PR head", true, swap("dispatch", "steps.pr.outputs.base", "steps.pr.outputs.head")],
            [
                "dispatch 的 checkout 没写 ref",
                true,
                swap("dispatch", "          ref: ${{ steps.pr.outputs.base }}\n", ""),
            ],
            ["缺 deepseek-review.yml", true, check({ deepseek: null })],
        ].map(([label, bad, errors]) => ({ label, bad, passed: bad ? errors.length > 0 : errors.length === 0 }));
        // review-bot.mjs 自己的判据（取总结、尾注归一、上一轮与处置汇总、config）一并在这里跑
        const own = caseResult(cases);
        const bot = scriptSelfTest();
        return {
            ok: own.ok && bot.ok,
            bad: own.bad + bot.bad,
            good: own.good + bot.good,
            lines: [...own.lines, ...bot.lines],
        };
    },
};
