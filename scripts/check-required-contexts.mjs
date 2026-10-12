// SPDX-License-Identifier: GPL-3.0-or-later
//
// required checks 的真源核对。.github/required-checks.json 按 ruleset 列出 required context：
// status 为 required 的已写进 .github/rulesets/<名>.json，pending 的等对应 job 落地、真名回读后再加。
//   node scripts/check-required-contexts.mjs
//        离线：每个 required context 恰好对应一个 workflow job；ruleset JSON 的 required_status_checks
//        与清单逐项一致（integration_id 是 GitHub Actions，strict 与清单一致）；feature/** 的 ruleset 必须
//        do_not_enforce_on_create，否则新主支线建不出来；pending 的 ruleset 还不带 required_status_checks。
//   node scripts/check-required-contexts.mjs --live [--sha <提交>] [--repo <owner/name>]
//        只读回读 GitHub（需要 gh 与 DLsnows 的 GH_TOKEN）：rulesets 与仓内 JSON 逐项一致；selected-actions
//        与 actions-allowlist.json 一致，org 继承来的、计划不用的放行项只报注记；sha_pinning_required 按
//        SHA_PINNING 判（暂缓期只报注记）；给了 --sha 时，该提交上 check run 的真名覆盖全部 required context。
//   --self-test

import fs from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { ROOT, assertNodeVersion, caseResult, isMain } from "./lib/check-kit.mjs";
import { parseWorkflow, readJson, workflowFiles } from "./checks.d/workflows.mjs";

/** GitHub Actions 这个 app 的 id：context 带上它，别的 app 报同名 status 不能顶替。 */
export const ACTIONS_APP = 15368;

/**
 * 仓库级「action 必须钉 SHA」开关的预期：required = 必须开启，没开判红；deferred = 暂缓，没开只报注记。
 * 暂缓的原因：评审 bot 的 action 若有没钉 SHA 的嵌套 uses，开关一开它就跑不起来，要等 bot 实跑验证过再开。
 * 仓内的 workflows 检查照样要求每个 uses 钉 SHA，不受这个开关影响。
 */
export const SHA_PINNING = "deferred";

export function loadRepo(root = ROOT) {
    const checks = readJson(root, ".github/required-checks.json");
    // 全部 ruleset 都登记在 .github/rulesets/，不只是带 required checks 的那几个
    const dir = path.join(root, ".github", "rulesets");
    const rulesets = Object.fromEntries(
        fs
            .readdirSync(dir)
            .filter((f) => f.endsWith(".json"))
            .map((f) => readJson(root, `.github/rulesets/${f}`))
            .map((r) => [r.name, r]),
    );
    const jobNames = workflowFiles(root).flatMap((f) =>
        parseWorkflow(fs.readFileSync(path.join(root, f), "utf8")).jobs.map((j) => j.name),
    );
    return { checks, rulesets, jobNames, allowlist: readJson(root, ".github/actions-allowlist.json") };
}

export function staticCheck({ checks, rulesets, jobNames }) {
    const errors = [];
    const notes = [];
    for (const [set, spec] of Object.entries(checks)) {
        const rule = rulesets[spec.ruleset]?.rules?.find((r) => r.type === "required_status_checks");
        if (spec.status === "pending") {
            if (rule) errors.push(`${set}：pending 的 ruleset 不该已经带 required_status_checks`);
            notes.push(`${set}：待加 ${spec.contexts.join("、")}`);
            continue;
        }
        if (spec.status !== "required") {
            errors.push(`${set}：status 只能是 required 或 pending`);
            continue;
        }
        for (const c of spec.contexts) {
            const n = jobNames.filter((name) => name === c).length;
            if (n !== 1) errors.push(`${set}：context ${c} 对应了 ${n} 个 workflow job（应当恰好一个）`);
        }
        if (!rule) {
            errors.push(`${set}：ruleset 缺 required_status_checks`);
            continue;
        }
        const p = rule.parameters || {};
        const got = p.required_status_checks || [];
        if (
            got
                .map((x) => x.context)
                .sort()
                .join() !== [...spec.contexts].sort().join()
        ) {
            errors.push(`${set}：ruleset 里的 context 与清单不一致`);
        }
        if (got.some((x) => x.integration_id !== ACTIONS_APP))
            errors.push(`${set}：每个 context 都要带 integration_id ${ACTIONS_APP}`);
        if (p.strict_required_status_checks_policy !== spec.strict) errors.push(`${set}：strict 与清单不一致`);
        const include = rulesets[spec.ruleset].conditions?.ref_name?.include || [];
        if (include.some((r) => r.startsWith("refs/heads/feature/")) && p.do_not_enforce_on_create !== true) {
            errors.push(`${set}：feature/** 的 ruleset 必须 do_not_enforce_on_create`);
        }
    }
    return { errors, notes };
}

/** 稳定序列化：键排序，rules 按 type 排序，只取 PUT 请求体里的字段。 */
const stable = (v) =>
    Array.isArray(v)
        ? `[${v.map(stable).join(",")}]`
        : v && typeof v === "object"
          ? `{${Object.keys(v)
                .sort()
                .map((k) => `${JSON.stringify(k)}:${stable(v[k])}`)
                .join(",")}}`
          : JSON.stringify(v);
export const normalize = (r) =>
    stable({
        name: r.name,
        target: r.target,
        enforcement: r.enforcement,
        conditions: r.conditions,
        bypass_actors: r.bypass_actors || [],
        rules: [...(r.rules || [])].sort((a, b) => (a.type < b.type ? -1 : 1)),
    });

/**
 * live：{ rulesets: { 名: GET 结果 }, listed: [{ name, source_type }]（作用于本仓的全部 ruleset，含 org 级），
 * selected, permissions, checkRuns?: [名] }。
 */
export function compareLive(repo, live, { shaPinning = SHA_PINNING } = {}) {
    const errors = [];
    const notes = [];
    for (const [name, mine] of Object.entries(repo.rulesets)) {
        if (!live.rulesets[name]) errors.push(`ruleset ${name} 在 GitHub 上不存在`);
        else if (normalize(live.rulesets[name]) !== normalize(mine)) errors.push(`ruleset ${name} 与仓内 JSON 不一致`);
    }
    // 仓内没登记的 ruleset（例如带 bypass 的）同样会改变合并门槛
    for (const r of live.listed || []) {
        if (!repo.rulesets[r.name]) errors.push(`GitHub 上多了仓内没有登记的 ruleset ${r.name}（${r.source_type}）`);
    }
    const a = repo.allowlist;
    const s = live.selected;
    if (s.github_owned_allowed !== a.github_owned_allowed)
        errors.push("selected-actions 的 github_owned_allowed 与清单不一致");
    if (s.verified_allowed) errors.push("selected-actions 不该放行 verified creator 的全部 action");
    for (const p of a.patterns_allowed) if (!s.patterns_allowed.includes(p)) errors.push(`计划要用的 ${p} 没被放行`);
    for (const p of s.patterns_allowed.filter((x) => !a.patterns_allowed.includes(x))) {
        if ((a.org_inherited_unused || []).includes(p)) notes.push(`org 继承、计划不用：${p}（仓内检查拒绝它）`);
        else errors.push(`selected-actions 多放行了清单里没有的 ${p}`);
    }
    if (live.permissions.sha_pinning_required !== true) {
        if (shaPinning === "required") errors.push("actions/permissions 的 sha_pinning_required 不是 true");
        else notes.push("sha_pinning_required 没开：暂缓，等评审 bot 实跑验证过再开");
    }
    if (live.checkRuns) {
        for (const spec of Object.values(repo.checks).filter((x) => x.status === "required")) {
            for (const c of spec.contexts)
                if (!live.checkRuns.includes(c)) errors.push(`提交上没有名为 ${c} 的 check run`);
        }
    }
    return { errors, notes };
}

function fetchLive(repo, nwo, sha) {
    const gh = (p) => {
        const r = spawnSync("gh", ["api", p], { encoding: "utf8", maxBuffer: 64 * 1024 * 1024 });
        if (r.status !== 0) throw new Error(`gh api ${p} 失败：${(r.stderr || "").trim().split("\n")[0]}`);
        return JSON.parse(r.stdout);
    };
    const listed = gh(`repos/${nwo}/rulesets?includes_parents=true&per_page=100`);
    const ids = Object.fromEntries(listed.map((r) => [r.name, r.id]));
    const rulesets = {};
    for (const name of Object.keys(repo.rulesets))
        if (ids[name]) rulesets[name] = gh(`repos/${nwo}/rulesets/${ids[name]}`);
    return {
        rulesets,
        listed: listed.map((r) => ({ name: r.name, source_type: r.source_type })),
        selected: gh(`repos/${nwo}/actions/permissions/selected-actions`),
        permissions: gh(`repos/${nwo}/actions/permissions`),
        checkRuns: sha ? gh(`repos/${nwo}/commits/${sha}/check-runs?per_page=100`).check_runs.map((c) => c.name) : null,
    };
}

export function selfTest() {
    const ctx = (n) => ({ context: n, integration_id: ACTIONS_APP });
    const rs = (rules, include = "refs/heads/feature/**") => ({
        name: "x",
        target: "branch",
        enforcement: "active",
        conditions: { ref_name: { include: [include], exclude: [] } },
        rules,
        bypass_actors: [],
    });
    const rsc = (p) => ({
        type: "required_status_checks",
        parameters: {
            strict_required_status_checks_policy: false,
            do_not_enforce_on_create: true,
            required_status_checks: [ctx("a")],
            ...p,
        },
    });
    const repo = (over = {}) => ({
        checks: { lines: { ruleset: "x", status: "required", strict: false, contexts: ["a"] } },
        rulesets: { x: rs([{ type: "deletion" }, rsc()]) },
        jobNames: ["a", "b"],
        allowlist: { github_owned_allowed: true, patterns_allowed: ["p/one@*"], org_inherited_unused: ["q/two@*"] },
        ...over,
    });
    const st = (r) => staticCheck(r).errors;
    const live = (r, over = {}) => ({
        rulesets: { x: r.rulesets.x },
        selected: { github_owned_allowed: true, verified_allowed: false, patterns_allowed: ["p/one@*", "q/two@*"] },
        permissions: { sha_pinning_required: true },
        checkRuns: ["a", "b"],
        ...over,
    });
    const good = repo();
    const cases = [
        ["清单、ruleset、job 三方一致", false, st(good)],
        ["context 对不上任何 job", true, st(repo({ jobNames: ["b"] }))],
        ["同名 job 有两个", true, st(repo({ jobNames: ["a", "a"] }))],
        [
            "ruleset 少一个 context",
            true,
            st(repo({ checks: { lines: { ...good.checks.lines, contexts: ["a", "b"] } } })),
        ],
        [
            "feature/** 没有 do_not_enforce_on_create",
            true,
            st(repo({ rulesets: { x: rs([rsc({ do_not_enforce_on_create: false })]) } })),
        ],
        [
            "context 不带 GitHub Actions 的 integration_id",
            true,
            st(repo({ rulesets: { x: rs([rsc({ required_status_checks: [{ context: "a" }] })]) } })),
        ],
        [
            "pending 的 ruleset 已带 required checks",
            true,
            st(repo({ checks: { lines: { ...good.checks.lines, status: "pending" } } })),
        ],
        ["live 与仓内一致，org 继承项只报注记", false, compareLive(good, live(good)).errors],
        [
            "live 的 ruleset 少了规则",
            true,
            compareLive(good, live(good, { rulesets: { x: rs([{ type: "deletion" }]) } })).errors,
        ],
        [
            "live 多放行了未登记的 action",
            true,
            compareLive(
                good,
                live(good, { selected: { ...live(good).selected, patterns_allowed: ["p/one@*", "z/z@*"] } }),
            ).errors,
        ],
        [
            "要求 SHA 钉版强制时，live 没开判红",
            true,
            compareLive(good, live(good, { permissions: { sha_pinning_required: false } }), {
                shaPinning: "required",
            }).errors,
        ],
        [
            "暂缓期 live 没开只报注记",
            false,
            compareLive(good, live(good, { permissions: { sha_pinning_required: false } }), {
                shaPinning: "deferred",
            }).errors,
        ],
        ["提交上缺 required check run", true, compareLive(good, live(good, { checkRuns: ["b"] })).errors],
        [
            "GitHub 上多了未登记的 ruleset",
            true,
            compareLive(
                good,
                live(good, {
                    listed: [
                        { name: "x", source_type: "Repository" },
                        { name: "bypass", source_type: "Repository" },
                    ],
                }),
            ).errors,
        ],
        [
            "列出的 ruleset 都已登记",
            false,
            compareLive(good, live(good, { listed: [{ name: "x", source_type: "Repository" }] })).errors,
        ],
    ].map(([label, bad, errors]) => ({ label, bad, passed: bad ? errors.length > 0 : errors.length === 0 }));
    const note = compareLive(good, live(good)).notes.some((n) => n.includes("q/two@*"));
    cases.push({ label: "org 继承项出现在注记里", bad: false, passed: note });
    const off = { permissions: { sha_pinning_required: false } };
    const deferred = compareLive(good, live(good, off), { shaPinning: "deferred" }).notes;
    cases.push({
        label: "暂缓期的注记写明原因",
        bad: false,
        passed: deferred.some((n) => n.includes("sha_pinning_required")),
    });
    return caseResult(cases);
}

function main(argv) {
    assertNodeVersion();
    const opt = (k) => (argv.includes(k) ? argv[argv.indexOf(k) + 1] : undefined);
    if (argv.includes("--self-test")) {
        const r = selfTest();
        for (const l of r.lines) console.error(l);
        console.log(
            `check-required-contexts --self-test：${r.ok ? "通过" : "失败"}（坏样例 ${r.bad}、正例 ${r.good}）`,
        );
        return r.ok ? 0 : 1;
    }
    const repo = loadRepo();
    const r = argv.includes("--live")
        ? compareLive(repo, fetchLive(repo, opt("--repo") || "synchain-oss/rinne", opt("--sha")))
        : staticCheck(repo);
    for (const n of r.notes) console.log(`  注记：${n}`);
    for (const e of r.errors) console.error(`  ${e}`);
    console.log(
        `check-required-contexts${argv.includes("--live") ? " --live" : ""}：${r.errors.length ? `失败（${r.errors.length} 项）` : "通过"}`,
    );
    return r.errors.length ? 1 : 0;
}

if (isMain(import.meta.url)) {
    try {
        process.exitCode = main(process.argv.slice(2));
    } catch (e) {
        console.error(`check-required-contexts：${e.message}`);
        process.exitCode = 1;
    }
}
