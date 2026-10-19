// SPDX-License-Identifier: GPL-3.0-or-later
//
// workflow 加固检查（docs-truth 的一部分），扫 .github/workflows/*.yml 与本地 action：
//   - 远程 uses 一律钉 40 位 SHA，行尾注明版本；本地 uses 只能指向 ./.github/workflows 或 ./.github/actions
//   - 远程 action 与可复用 workflow 必须在 .github/actions-allowlist.json 里：org 的白名单比计划宽，Rinne 以这一份为准
//   - 全仓零 pull_request_target、零 workflow_run
//   - 每个 workflow 有顶层 permissions（不许 read-all / write-all）与 concurrency；每个 job 有 timeout-minutes
//     （调用可复用 workflow 的 job 不能写 timeout，由被调用方各自设）
//   - 调用可复用 workflow 的 job：必须显式写 job 级 permissions（只许读权限与评论两项写权限）、secrets 逐项传
//     （不许 inherit）、with 里不许出现 github.event.* / github.head_ref / inputs.*（pr_number 一项例外）；
//     引用同样钉 40 位 SHA 并注明版本
//   - required job（.github/required-checks.json 里的 context）不带 job 级 if（`!cancelled()` 除外），
//     所在 workflow 的触发器不带 paths 过滤：被跳过的 required job 会被当成通过
//   - run 脚本里不插值 ${{ }}：标题、正文、分支名这类不可信文本只经 env 传入
//   - 最后跑钉版 actionlint（-shellcheck= -pyflakes=，本机与 CI 都不依赖外部 linter）
// 解析器只认本仓手写 workflow 的写法（两格缩进的块式 YAML），不是完整的 YAML 解析器；认不出的写法宁可判红。

import fs from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { caseResult, inCI, localSetting } from "../lib/check-kit.mjs";

/** 引号外的 # 之后是注释。 */
function splitComment(line) {
    let quote = null;
    for (let i = 0; i < line.length; i++) {
        const c = line[i];
        if (quote) {
            if (c === quote) quote = null;
        } else if (c === '"' || c === "'") {
            quote = c;
        } else if (c === "#" && (i === 0 || /\s/.test(line[i - 1]))) {
            return [line.slice(0, i).trimEnd(), line.slice(i + 1).trim()];
        }
    }
    return [line.trimEnd(), ""];
}

/**
 * 逐行拆成 { no, indent, key, value, comment }；块标量（`run: |` 之后缩进更深的行）标 scalar 并记下所属的键。
 * 列表项 `- key: v` 的 indent 取键所在的列。
 */
export function workflowLines(text) {
    const out = [];
    let block = null; // { indent, key }
    text.split(/\r?\n/).forEach((raw, i) => {
        const indent = raw.match(/^ */)[0].length;
        if (block) {
            if (!raw.trim() || indent > block.indent) {
                out.push({ no: i + 1, indent, scalar: block.key, raw });
                return;
            }
            block = null;
        }
        if (!raw.trim() || raw.trim().startsWith("#")) return;
        const [code, comment] = splitComment(raw);
        const m = /^(\s*)(-\s+)?([A-Za-z0-9_-]+):(?:\s+(.*))?$/.exec(code);
        const keyIndent = m ? m[1].length + (m[2] ? m[2].length : 0) : indent;
        const line = {
            no: i + 1,
            indent: keyIndent,
            key: m ? m[3] : null,
            value: m ? (m[4] || "").trim() : code.trim(),
            comment,
            code,
            raw,
        };
        line.bad = unsupported(line, m);
        if (m && /^[|>][+-]?\d*$/.test(line.value)) block = { indent: keyIndent, key: line.key };
        out.push(line);
    });
    return out;
}

/**
 * 解析器认不了、也就核不了的写法：流式映射 `{…}`、装着映射的流式序列、写成流式的 steps / jobs、
 * 带引号的键、锚点 / 别名 / 标签，以及其他任何既不是「键: 值」也不是「- 标量」的行。
 * 这些写法里藏的 uses、if、permissions 本检查看不见，所以一律判红（空的 {} 与 [] 不藏东西，放行）。
 */
function unsupported(line, m) {
    const v = line.value;
    if (!m) {
        if (/^\s*(?:-\s+)?(["']).*?\1\s*:/.test(line.code)) return "不支持带引号的键";
        const item = /^\s*-\s+(.+)$/.exec(line.code);
        if (!item) return "认不出的写法";
        return /^[{[&*!]/.test(item[1].trim()) ? "列表项不支持流式集合、锚点、别名与标签" : null;
    }
    if (v === "{}" || v === "[]") return null;
    if (/^[{&*!]/.test(v)) return "不支持流式映射、锚点、别名与标签";
    if (v.startsWith("[") && (/[{:]/.test(v) || line.key === "steps" || line.key === "jobs")) {
        return "流式序列里只能放标量";
    }
    return null;
}

const unquote = (v) => v.replace(/^(["'])(.*)\1$/, "$2");

/** 文件里的全部远程 uses：[{ ref: "owner/repo[/path]@sha", repo: "owner/repo", sha, version, no }]。 */
export function remoteUses(text) {
    return workflowLines(text)
        .filter((l) => !l.scalar && l.key === "uses" && !unquote(l.value).startsWith("./"))
        .map((l) => {
            const ref = unquote(l.value);
            const at = ref.lastIndexOf("@");
            return {
                ref,
                repo: (at < 0 ? ref : ref.slice(0, at)).split("/").slice(0, 2).join("/"),
                sha: at < 0 ? "" : ref.slice(at + 1),
                version: l.comment,
                no: l.no,
            };
        });
}

/**
 * allowlist 的格式与 GitHub selected-actions 相同：github_owned_allowed 加 patterns_allowed。target 是 uses 去掉 @ref 的
 * 部分（owner/repo 或 owner/repo/路径）。认两种写法：owner/repo@*（整个仓），以及 GitHub 给可复用 workflow 的
 * owner/repo/<目录>/*@*（只放行该目录下一层的文件，例如 Synchain-oss/ci-workflows/.github/workflows/*@*）。
 * 其余一律逐字比较，不做别的通配。
 */
export function allowed(target, allowlist) {
    const [owner, name] = target.split("/");
    const repo = `${owner}/${name}`;
    if (allowlist.github_owned_allowed && /^(?:actions|github)$/.test(owner)) return true;
    return (allowlist.patterns_allowed || []).some((p) => {
        if (p === `${repo}@*`) return true;
        const dir = /^([^@*]+)\/\*@\*$/.exec(p)?.[1];
        if (!dir || dir.split("/").length < 3 || !target.startsWith(`${dir}/`)) return false;
        const file = target.slice(dir.length + 1);
        return file.length > 0 && !file.includes("/");
    });
}

/**
 * 结构：顶层键、某个顶层键下的整段、各个 job。job 是 jobs 下两格缩进的键，四格缩进的是它的设置；
 * 作为 status check 的名字有 name 取 name，否则取 job id。
 */
export function parseWorkflow(text) {
    const lines = workflowLines(text);
    const plain = lines.filter((l) => !l.scalar);
    const top = (k) => plain.find((l) => l.indent === 0 && l.key === k);
    const section = (k) => {
        const start = plain.indexOf(top(k));
        if (start < 0) return [];
        const end = plain.findIndex((l, i) => i > start && l.indent === 0);
        return plain.slice(start + 1, end < 0 ? plain.length : end);
    };
    const body = section("jobs");
    const heads = body.filter((l) => l.indent === 2 && l.key);
    const jobs = heads.map((head, k) => {
        const own = body.slice(body.indexOf(head) + 1, k + 1 < heads.length ? body.indexOf(heads[k + 1]) : body.length);
        const get = (key) => own.find((l) => l.indent === 4 && l.key === key);
        return { id: head.key, no: head.no, name: get("name") ? unquote(get("name").value) : head.key, own, get };
    });
    return { lines, plain, top, section, jobs };
}

/** job 级映射（`with:`、`secrets:`、`permissions:`）：{ head, entries, inline }。entries 是六格缩进的键行。 */
export function jobMap(job, key) {
    const at = job.own.findIndex((l) => l.indent === 4 && l.key === key);
    if (at < 0) return null;
    const head = job.own[at];
    const rest = job.own.slice(at + 1);
    const end = rest.findIndex((l) => l.indent <= 4);
    return {
        head,
        entries: rest.slice(0, end < 0 ? rest.length : end).filter((l) => l.indent === 6 && l.key),
        inline: unquote(head.value),
    };
}

/** 一行映射值的完整文本：块标量连正文一起取。 */
function valueText(lines, line) {
    if (!/^[|>][+-]?\d*$/.test(line.value)) return line.value;
    const at = lines.indexOf(line);
    const body = [];
    for (let i = at + 1; i < lines.length && lines[i].scalar === line.key; i++) body.push(lines[i].raw.trim());
    return body.join("\n");
}

/** `with:` 值里可以出现的一个表达式：PR 号是调用方与被调用方约定的数字，其余一律不许。 */
const ALLOWED_WITH_EXPR = /\$\{\{\s*inputs\.pr_number\s*\}\}/g;
/** 不可信的上下文：PR 标题、正文、分支名这类由 PR 作者决定的值。 */
const UNTRUSTED = /\bgithub\.event\b|\bgithub\.head_ref\b|\binputs\./;
/** job 级 permissions 的已知权限项。 */
const SCOPES = new Set([
    "actions",
    "attestations",
    "checks",
    "contents",
    "deployments",
    "discussions",
    "id-token",
    "issues",
    "packages",
    "pages",
    "pull-requests",
    "repository-projects",
    "security-events",
    "statuses",
]);
/** 调用可复用 workflow 的 job 只许这两个写权限：都是发评论要用的。 */
const WRITE_SCOPES = new Set(["issues", "pull-requests"]);

/** 一个 workflow 的加固问题。required：必须满足 required 规则的 job 名集合。 */
export function inspectWorkflow(file, text, { required = new Set(), allowlist }) {
    const errors = [];
    const err = (no, msg) => errors.push(`${file}:${no} ${msg}`);
    const { lines, plain, top, section, jobs } = parseWorkflow(text);
    for (const l of plain) if (l.bad) err(l.no, `${l.bad}：本检查只认块式 YAML，认不出的写法一律判红`);
    // 交叉核对：源码里出现的 uses 键数（含带引号的写法）必须等于解析出来的个数，不许有漏看的
    const raw = plain.reduce((n, l) => n + (l.code.match(/\buses\s*["']?\s*:/g) || []).length, 0);
    const parsed = plain.filter((l) => l.key === "uses").length;
    if (raw !== parsed) err(1, `源码里有 ${raw} 处 uses，解析出 ${parsed} 处：有写法没被认出`);
    for (const l of plain) {
        if (/\b(?:pull_request_target|workflow_run)\b/.test(`${l.key || ""} ${l.value}`)) {
            err(l.no, "不允许 pull_request_target / workflow_run 触发");
        }
        if (l.key === "run" && l.value.includes("${{")) err(l.no, "run 里不许插值 ${{ }}，经 env 传入");
    }
    for (const l of lines)
        if (l.scalar === "run" && l.raw.includes("${{")) err(l.no, "run 里不许插值 ${{ }}，经 env 传入");
    const perm = top("permissions");
    if (!perm) err(1, "缺顶层 permissions");
    else if (/^(?:read|write)-all$/.test(perm.value)) err(perm.no, "permissions 要逐项写最小权限");
    if (!top("concurrency")) err(1, "缺顶层 concurrency");
    const paths = section("on").find((l) => l.key === "paths" || l.key === "paths-ignore");
    if (!top("on")) err(1, "缺 on");
    if (!jobs.length) err(1, "没有找到任何 job");
    for (const job of jobs) {
        if (!job.get("timeout-minutes") && !job.get("uses")) err(job.no, `job ${job.id} 缺 timeout-minutes`);
        const jobPerm = job.get("permissions");
        if (jobPerm && /^(?:read|write)-all$/.test(unquote(jobPerm.value))) {
            err(jobPerm.no, `job ${job.id} 的 permissions 要逐项写最小权限`);
        }
        if (job.get("uses")) {
            // 调用可复用 workflow：权限、secret 与 with 的取值都经被调用方之手，逐项收紧
            if (!jobPerm) err(job.no, `job ${job.id} 调用可复用 workflow，必须显式写 job 级 permissions`);
            else {
                const map = jobMap(job, "permissions");
                if (!map.entries.length && map.inline) err(jobPerm.no, `job ${job.id} 的 permissions 要逐项写`);
                for (const p of map.entries) {
                    if (!SCOPES.has(p.key)) err(p.no, `job ${job.id} 的 permissions 里有未知的权限项 ${p.key}`);
                    const value = unquote(p.value);
                    if (value === "write" && !WRITE_SCOPES.has(p.key)) {
                        err(p.no, `job ${job.id} 调用可复用 workflow，${p.key} 不许给写权限`);
                    } else if (value !== "read" && value !== "write") {
                        err(p.no, `job ${job.id} 的 ${p.key} 只能是 read 或 write`);
                    }
                }
            }
            const secrets = jobMap(job, "secrets");
            if (secrets && secrets.inline)
                err(secrets.head.no, `job ${job.id} 的 secrets 只能逐项传，不能是 ${secrets.inline}`);
            else if (secrets)
                for (const s of secrets.entries) {
                    if (!/^\$\{\{\s*secrets\.[A-Za-z_][A-Za-z0-9_]*\s*\}\}$/.test(unquote(s.value))) {
                        err(s.no, `job ${job.id} 的 secret ${s.key} 只能取自 \${{ secrets.X }}`);
                    }
                }
            const withMap = jobMap(job, "with");
            if (withMap)
                for (const w of withMap.entries) {
                    const text = valueText(lines, w).replace(ALLOWED_WITH_EXPR, "");
                    if (UNTRUSTED.test(text)) {
                        err(w.no, `job ${job.id} 的 with.${w.key} 传了不可信值，只许静态值与 inputs.pr_number`);
                    }
                }
        }
        if (required.has(job.name)) {
            const cond = job.get("if");
            if (cond && !/^(?:\$\{\{\s*)?!cancelled\(\)(?:\s*\}\})?$/.test(unquote(cond.value))) {
                err(cond.no, `required job ${job.name} 不许带 job 级 if`);
            }
            if (paths) err(paths.no, `required job ${job.name} 所在的 workflow 不许按 paths 过滤`);
        }
    }
    for (const l of plain.filter((x) => x.key === "uses")) {
        const ref = unquote(l.value);
        if (ref.startsWith("./")) {
            if (!/^\.\/\.github\/(?:workflows|actions)\//.test(ref) || ref.includes("..")) {
                err(l.no, "本地 uses 只能指向 ./.github/workflows 或 ./.github/actions");
            }
            continue;
        }
        if (ref.startsWith("docker://")) {
            err(l.no, "不允许 docker:// action");
            continue;
        }
        const at = ref.lastIndexOf("@");
        if (at < 0 || !/^[0-9a-f]{40}$/.test(ref.slice(at + 1))) err(l.no, `没有钉 40 位 SHA：${ref}`);
        else if (!/^v?\d/.test(l.comment)) err(l.no, `钉 SHA 的 uses 要在行尾注明版本：${ref}`);
        const target = at < 0 ? ref : ref.slice(0, at);
        if (!allowed(target, allowlist)) err(l.no, `${target} 不在 .github/actions-allowlist.json 里`);
    }
    return errors;
}

/** 仓里要检查的 workflow 与本地 action 文件（仓内相对路径）。 */
export function workflowFiles(root) {
    const dir = path.join(root, ".github", "workflows");
    const out = fs.existsSync(dir)
        ? fs
              .readdirSync(dir)
              .filter((f) => /\.ya?ml$/.test(f))
              .map((f) => `.github/workflows/${f}`)
        : [];
    const walk = (rel) => {
        const abs = path.join(root, rel);
        if (!fs.existsSync(abs)) return;
        for (const e of fs.readdirSync(abs, { withFileTypes: true })) {
            if (e.isDirectory()) walk(`${rel}/${e.name}`);
            else if (/^action\.ya?ml$/.test(e.name)) out.push(`${rel}/${e.name}`);
        }
    };
    walk(".github/actions");
    return out.sort();
}

export const readJson = (root, rel) => JSON.parse(fs.readFileSync(path.join(root, rel), "utf8"));

/** required-checks.json 里出现的全部 context（含 pending）。 */
export const requiredNames = (checks) => new Set(Object.values(checks).flatMap((s) => s.contexts || []));

const PIN = "3d3c42e5aac5ba805825da76410c181273ba90b1";
const GOOD = [
    "name: Fixture",
    "on:",
    "  pull_request:",
    '    branches: [dev, "feature/**"]',
    "permissions:",
    "  contents: read",
    "concurrency:",
    "  group: ${{ github.workflow }}-${{ github.ref }}",
    "jobs:",
    "  gate:",
    "    name: gate",
    "    runs-on: ubuntu-latest",
    "    timeout-minutes: 5",
    "    steps:",
    `      - uses: actions/checkout@${PIN} # v7.0.1`,
    "      - name: Run",
    "        env:",
    "          TITLE: ${{ github.event.pull_request.title }}",
    "        run: |",
    '          echo "$TITLE"',
    "          echo uses: in a script is not a step",
].join("\n");

/** 调用可复用 workflow 的 job：权限逐项、secret 逐项、with 里只有静态值与 inputs.pr_number。 */
const SHARED = [
    "jobs:",
    "  shared:",
    "    permissions:",
    "      contents: read",
    "      issues: write",
    "      pull-requests: write",
    `    uses: Synchain-oss/ci-workflows/.github/workflows/pr-agent.yml@${PIN} # v1.3.1`,
    "    with:",
    "      provider: opencode-go",
    "      describe: false",
    "      pr_number: ${{ inputs.pr_number }}",
    "    secrets:",
    "      api_key: ${{ secrets.OPENAI_API_KEY }}",
].join("\n");
const WORKFLOW = `${GOOD.split("\njobs:")[0]}\n${SHARED}`;

export default {
    name: "workflows",
    group: "docs",
    inputs: [],
    run(ctx) {
        const files = workflowFiles(ctx.root);
        if (!files.length) return { ok: false, lines: ["没有找到任何 workflow"] };
        const allowlist = readJson(ctx.root, ".github/actions-allowlist.json");
        const required = requiredNames(readJson(ctx.root, ".github/required-checks.json"));
        const errors = files.flatMap((f) =>
            inspectWorkflow(f, fs.readFileSync(path.join(ctx.root, f), "utf8"), { required, allowlist }),
        );
        if (errors.length) return { ok: false, lines: errors };
        const bin =
            localSetting("RINNE_ACTIONLINT", "rinne.actionlint", { env: ctx.env, cwd: ctx.root }) || "actionlint";
        const r = spawnSync(bin, ["-no-color", "-shellcheck=", "-pyflakes=", ...files], {
            cwd: ctx.root,
            encoding: "utf8",
        });
        if (r.error) {
            if (inCI(ctx.env)) return { ok: false, lines: ["CI 上找不到 actionlint"] };
            return {
                skip: `加固检查通过（${files.length} 个文件）；本机找不到 actionlint，未跑（设置 RINNE_ACTIONLINT）`,
            };
        }
        if (r.status === 0) return { ok: true, lines: [`${files.length} 个文件，actionlint 通过`] };
        return { ok: false, lines: `${r.stdout}${r.stderr}`.trim().split("\n").slice(-30) };
    },
    selfTest() {
        const allowlist = {
            github_owned_allowed: true,
            patterns_allowed: ["anthropics/claude-code-action@*", "Synchain-oss/ci-workflows/.github/workflows/*@*"],
        };
        const required = new Set(["gate"]);
        const check = (text) => inspectWorkflow("f.yml", text, { required, allowlist });
        const swap = (a, b) => {
            if (!GOOD.includes(a)) throw new Error(`自测夹具里找不到要替换的片段：${a}`);
            return GOOD.replace(a, b);
        };
        const shared = (p) => check(swap(`actions/checkout@${PIN}`, `Synchain-oss/${p}@${PIN}`));
        const checkout = `      - uses: actions/checkout@${PIN} # v7.0.1`;
        const call = (a, b) => {
            if (!SHARED.includes(a)) throw new Error(`自测夹具里找不到要替换的片段：${a}`);
            return check(WORKFLOW.replace(a, b));
        };
        const dropLine = (line) => call(`${line}\n`, "");
        /** 换一份 allowlist 再检查同一份 workflow。 */
        const withAllow = (patterns, text = WORKFLOW) =>
            inspectWorkflow("f.yml", text, {
                required,
                allowlist: { github_owned_allowed: true, patterns_allowed: patterns },
            });
        const subPath = (p) => withAllow([p], GOOD.replace(`actions/checkout@${PIN}`, `owner/repo/sub@${PIN} # v1`));
        const emptyFile = (p) =>
            withAllow(
                [p],
                GOOD.replace(`actions/checkout@${PIN}`, `Synchain-oss/ci-workflows/.github/workflows/@${PIN} # v1`),
            );
        const cases = [
            ["加固齐全的 workflow", false, check(GOOD)],
            ["pull_request_target 触发", true, check(swap("  pull_request:", "  pull_request_target:"))],
            ["workflow_run 触发", true, check(swap("on:\n  pull_request:", "on:\n  workflow_run:"))],
            ["job 缺 timeout", true, check(swap("    timeout-minutes: 5\n", ""))],
            ["没钉 SHA 的 uses", true, check(swap(`@${PIN} # v7.0.1`, "@v7"))],
            ["钉了 SHA 却没注明版本", true, check(swap(" # v7.0.1", ""))],
            ["org 放行、计划不用的 pr-agent", true, check(swap(`actions/checkout@${PIN}`, `qodo-ai/pr-agent@${PIN}`))],
            [
                "org 放行、计划不用的 action-gh-release",
                true,
                check(swap(`actions/checkout@${PIN}`, `softprops/action-gh-release@${PIN}`)),
            ],
            [
                "计划内的第三方 action",
                false,
                check(swap(`actions/checkout@${PIN}`, `anthropics/claude-code-action@${PIN}`)),
            ],
            // GitHub 给可复用 workflow 的目录通配：只放行那个目录下一层的文件
            ["目录通配放行的共享 workflow", false, shared("ci-workflows/.github/workflows/pr-agent.yml")],
            ["目录通配不放行同仓的别的目录", true, shared("ci-workflows/.github/actions/x")],
            ["目录通配不放行更深一层", true, shared("ci-workflows/.github/workflows/sub/x.yml")],
            ["目录通配不放行同 owner 的别的仓", true, shared("other/.github/workflows/pr-agent.yml")],
            ["目录通配不放行整个仓", true, shared("ci-workflows")],
            ["缺顶层 permissions", true, check(swap("permissions:\n  contents: read\n", ""))],
            [
                "permissions 写成 write-all",
                true,
                check(swap("permissions:\n  contents: read", "permissions: write-all")),
            ],
            [
                "缺 concurrency",
                true,
                check(swap("concurrency:\n  group: ${{ github.workflow }}-${{ github.ref }}\n", "")),
            ],
            [
                "required job 带 job 级 if",
                true,
                check(swap("    timeout-minutes: 5", "    timeout-minutes: 5\n    if: github.actor != 'x'")),
            ],
            [
                "required job 只在取消时跳过",
                false,
                check(swap("    timeout-minutes: 5", "    timeout-minutes: 5\n    if: ${{ !cancelled() }}")),
            ],
            [
                "required workflow 按 paths 过滤",
                true,
                check(swap('    branches: [dev, "feature/**"]', '    branches: [dev]\n    paths: ["src/**"]')),
            ],
            [
                "run 里插值不可信上下文",
                true,
                check(swap('          echo "$TITLE"', "          echo ${{ github.event.pull_request.title }}")),
            ],
            ["本地 uses 越出 .github", true, check(swap(`actions/checkout@${PIN} # v7.0.1`, "./scripts/x"))],
            // 解析器认不了的写法：评审时用来绕过检查的四种，加上锚点
            ["流式映射写的步骤", true, check(swap(checkout, "      - { uses: softprops/action-gh-release@v2 }"))],
            ["带引号的 uses 键", true, check(swap(checkout, '      - "uses": qodo-ai/pr-agent@main'))],
            [
                "流式序列写的 steps",
                true,
                check(swap(`    steps:\n${checkout}`, "    steps: [{uses: softprops/action-gh-release@v2}]")),
            ],
            [
                "required job 的 if 键带引号",
                true,
                check(swap("    timeout-minutes: 5", `    timeout-minutes: 5\n    "if": x`)),
            ],
            ["锚点", true, check(swap("    runs-on: ubuntu-latest", "    runs-on: &os ubuntu-latest"))],
            [
                "job 级 permissions 写成 write-all",
                true,
                check(swap("    timeout-minutes: 5", "    timeout-minutes: 5\n    permissions: write-all")),
            ],
            [
                "空的 permissions 与标量流式序列",
                false,
                check(swap("permissions:\n  contents: read", "permissions: {}")),
            ],
            // 调用可复用 workflow 的 job
            ["调用可复用 workflow 的 job", false, check(WORKFLOW)],
            [
                "org 的白名单 owner 必须整段相等，前缀不算",
                true,
                check(swap(`actions/checkout@${PIN}`, `actions-x/foo@${PIN}`)),
            ],
            ["调用可复用 workflow 时 secrets: inherit", true, call("    secrets:\n", "    secrets: inherit\n")],
            [
                "调用可复用 workflow 时缺 job 级 permissions",
                true,
                dropLine("    permissions:\n      contents: read\n      issues: write\n      pull-requests: write"),
            ],
            [
                "调用可复用 workflow 时 with 传了 PR 标题",
                true,
                call("      describe: false", "      describe: ${{ github.event.pull_request.title }}"),
            ],
            [
                "调用可复用 workflow 时 with 传了 head_ref",
                true,
                call("      describe: false", "      describe: ${{ github.head_ref }}"),
            ],
            [
                "调用可复用 workflow 时 with 传了别的 inputs",
                true,
                call("      pr_number: ${{ inputs.pr_number }}", "      pr_number: ${{ inputs.other }}"),
            ],
            [
                "调用可复用 workflow 的 uses 没钉 SHA",
                true,
                call(
                    `Synchain-oss/ci-workflows/.github/workflows/pr-agent.yml@${PIN} # v1.3.1`,
                    "Synchain-oss/ci-workflows/.github/workflows/pr-agent.yml@main",
                ),
            ],
            ["调用可复用 workflow 的 uses 没注明 tag", true, call(" # v1.3.1", "")],
            [
                "调用可复用 workflow 时多给了 contents 写权限",
                true,
                call("      contents: read", "      contents: write"),
            ],
            [
                "调用可复用 workflow 时 secret 不是逐项取自 secrets",
                true,
                call("      api_key: ${{ secrets.OPENAI_API_KEY }}", "      api_key: literal"),
            ],
            // 目录通配只认 owner/repo/<目录>/* 这种三段以上的前缀，且文件名不能为空
            ["两段式的 owner/repo/* 不算目录通配", true, subPath("owner/repo/*@*")],
            ["目录通配不放行空文件名", true, emptyFile("Synchain-oss/ci-workflows/.github/workflows/*@*")],
        ].map(([label, bad, errors]) => ({ label, bad, passed: bad ? errors.length > 0 : errors.length === 0 }));
        return caseResult(cases);
    },
};
