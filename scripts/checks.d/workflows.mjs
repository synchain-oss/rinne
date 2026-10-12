// SPDX-License-Identifier: GPL-3.0-or-later
//
// workflow 加固检查（docs-truth 的一部分），扫 .github/workflows/*.yml 与本地 action：
//   - 远程 uses 一律钉 40 位 SHA，行尾注明版本；本地 uses 只能指向 ./.github/workflows 或 ./.github/actions
//   - 远程 action 必须在 .github/actions-allowlist.json 里：org 的白名单比计划宽，Rinne 以这一份为准
//   - 全仓零 pull_request_target、零 workflow_run
//   - 每个 workflow 有顶层 permissions（不许 read-all / write-all）与 concurrency；每个 job 有 timeout-minutes
//     （调用可复用 workflow 的 job 不能写 timeout，由被调用方各自设）
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
            raw,
        };
        if (m && /^[|>][+-]?\d*$/.test(line.value)) block = { indent: keyIndent, key: line.key };
        out.push(line);
    });
    return out;
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

/** allowlist 的格式与 GitHub selected-actions 相同：github_owned_allowed 加 patterns_allowed（owner/repo@*）。 */
export function allowed(repo, allowlist) {
    if (allowlist.github_owned_allowed && /^(?:actions|github)\//.test(repo)) return true;
    return (allowlist.patterns_allowed || []).some((p) => p === `${repo}@*`);
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
        return { id: head.key, no: head.no, name: get("name") ? unquote(get("name").value) : head.key, get };
    });
    return { lines, plain, top, section, jobs };
}

/** 一个 workflow 的加固问题。required：必须满足 required 规则的 job 名集合。 */
export function inspectWorkflow(file, text, { required = new Set(), allowlist }) {
    const errors = [];
    const err = (no, msg) => errors.push(`${file}:${no} ${msg}`);
    const { lines, plain, top, section, jobs } = parseWorkflow(text);
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
        const repo = (at < 0 ? ref : ref.slice(0, at)).split("/").slice(0, 2).join("/");
        if (!allowed(repo, allowlist)) err(l.no, `${repo} 不在 .github/actions-allowlist.json 里`);
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
        const allowlist = { github_owned_allowed: true, patterns_allowed: ["anthropics/claude-code-action@*"] };
        const required = new Set(["gate"]);
        const check = (text) => inspectWorkflow("f.yml", text, { required, allowlist });
        const swap = (a, b) => GOOD.replace(a, b);
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
        ].map(([label, bad, errors]) => ({ label, bad, passed: bad ? errors.length > 0 : errors.length === 0 }));
        return caseResult(cases);
    },
};
