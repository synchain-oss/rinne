// SPDX-License-Identifier: GPL-3.0-or-later
//
// 变异检查的覆盖清单。run-checks 的 --mutation-check 只跑「导出了 mutationCheck 的检查」：某个检查的 mutationCheck 被删掉、
// 或改了名，它就不在名单里，悄悄不跑，整遍照样全绿。本检查拿运行器自己的名单（node scripts/run-checks.mjs
// --mutation-check --list，即 --mutation-check 真会跑的那批）对一份「必须有 mutationCheck」的清单 REQUIRED，缺一项就红。
// 本机 gates 的 mutation 与 compliance.yml 的 Mutation check 一步都先跑它，再跑 --mutation-check 本体。
//
// 判据名 → 说明：
//   status    运行器的 --list 退出码为 0（读不出名单不能当成「没有缺」）
//   empty     运行器的名单不是空的
//   none      REQUIRED 不为空（空清单会让 covered 恒过）
//   dup       REQUIRED 没有重复项
//   odd       REQUIRED 里只写检查名
//   covered   REQUIRED 里每一项都在运行器的名单里：没有 mutationCheck 的检查不在名单里
//   loaded    名单里的这一项不是加载失败的占位（占位的组名是 ?，它在 --mutation-check 里会判红，这里先点名）
// 自测的每个坏样例对应一条判据，mutationCheck 逐条关掉判据，自测必须因此变红。
//
// 追加规则：新检查加了 mutationCheck，就在 REQUIRED 里追加它的名字，之后它被删也会变红；不追加也不影响本检查和任何运行器，
// 只是还没有这层保护（run 会在输出里点名这些检查）。从 REQUIRED 里删一项等于撤掉对那个检查的保护，评审要看这一行的 diff。

import path from "node:path";
import { spawnSync } from "node:child_process";
import { caseResult } from "../lib/check-kit.mjs";

/** 必须有 mutationCheck 的检查（含本检查自己：它的 mutationCheck 被删同样会被这份清单认出来）。 */
export const REQUIRED = [
    "contract-static-identity",
    "contract-static-ipc",
    "contract-static-params",
    "docs-budget",
    "mutation-coverage",
    "privacy",
    "probe-analysis",
    "recursive-delete",
];

/** `--list` 的一行是 `名字<TAB>组<TAB>inputs`；加载失败的占位没有组，运行器写成 ?。 */
export function parseList(text) {
    return String(text)
        .split(/\r?\n/)
        .filter((l) => l.trim())
        .map((l) => {
            const [name, group] = l.split("\t");
            return { name, broken: group === "?" };
        });
}

/**
 * 核对覆盖：返回 { problems, unlisted }。status 与 listText 是 `--mutation-check --list` 的退出码与标准输出。
 * off 只给变异检查用，关掉其中一条判据（见 mutationCheck）。
 */
export function coverage({ required, status, listText }, off = {}) {
    const problems = [];
    const listed = parseList(listText);
    if (status !== 0) {
        if (!off.status) problems.push(`运行器的 --mutation-check --list 退出码 ${status}`);
    } else if (!listed.length && !off.empty) {
        problems.push("运行器的 --mutation-check --list 名单是空的");
    }
    if (!required.length && !off.none) problems.push("REQUIRED 是空的");
    const dup = required.filter((n, i) => required.indexOf(n) !== i);
    if (dup.length && !off.dup) problems.push(`REQUIRED 有重复项：${[...new Set(dup)].join("、")}`);
    const odd = required.filter((n) => typeof n !== "string" || !/^[a-z0-9]+(?:-[a-z0-9]+)*$/.test(n));
    if (odd.length && !off.odd) problems.push(`REQUIRED 里有不是检查名的项：${odd.map(String).join("、")}`);
    const byName = new Map(listed.map((e) => [e.name, e]));
    for (const name of required) {
        const e = byName.get(name);
        if (!e) {
            if (!off.covered) problems.push(`${name} 没有 mutationCheck：--mutation-check 不会跑它，却在 REQUIRED 里`);
        } else if (e.broken && !off.loaded) {
            problems.push(`${name} 加载失败，它的 mutationCheck 没法跑`);
        }
    }
    const unlisted = listed.filter((e) => !e.broken && !required.includes(e.name)).map((e) => e.name);
    return { problems, unlisted };
}

const FULL = ["a", "b", "c"].map((n) => `${n}\tdocs\t`).join("\n");

/** 自测用例；off 是被试的判据集（正常为空，mutationCheck 里关掉一条）。 */
function cases(off = {}) {
    const problems = (input) =>
        coverage({ required: ["a", "b", "c"], status: 0, listText: FULL, ...input }, off).problems;
    const mention = (input, what) => problems(input).some((p) => p.includes(what));
    return [
        ["清单齐全、运行器的名单都在", false, problems({}).length === 0],
        [
            "名单里多出没登记的检查：不判红，只在 unlisted 里点名",
            false,
            (() => {
                const r = coverage({ required: ["a", "b", "c"], status: 0, listText: `${FULL}\nd\tdocs\t` }, off);
                return r.problems.length === 0 && r.unlisted.join() === "d";
            })(),
        ],
        ["runner：运行器的 --list 退出码不是 0", true, mention({ status: 1 }, "退出码 1")],
        ["runner：运行器的 --list 名单是空的", true, mention({ listText: "" }, "名单是空的")],
        ["shape：REQUIRED 是空的", true, mention({ required: [] }, "REQUIRED 是空的")],
        ["shape：REQUIRED 有重复项", true, mention({ required: ["a", "b", "a"] }, "重复项：a")],
        ["shape：REQUIRED 里有不是检查名的项", true, mention({ required: ["a", "B c"] }, "不是检查名")],
        [
            "covered：必须有 mutationCheck 的检查不在名单里（mutationCheck 被删）",
            true,
            mention({ listText: "a\tdocs\t\nb\tdocs\t" }, "c 没有 mutationCheck"),
        ],
        [
            "covered：名单里是别的名字（mutationCheck 所在的检查改了名）",
            true,
            mention({ listText: "a\tdocs\t\nb\tdocs\t\nc2\tdocs\t" }, "c 没有 mutationCheck"),
        ],
        ["loaded：名单里是加载失败的占位", true, mention({ listText: "a\tdocs\t\nb\tdocs\t\nc\t?\t" }, "c 加载失败")],
    ].map(([label, bad, passed]) => ({ label, bad, passed }));
}

export default {
    name: "mutation-coverage",
    group: "compliance",
    inputs: [],
    run(ctx) {
        const r = spawnSync(
            process.execPath,
            [path.join(ctx.root, "scripts", "run-checks.mjs"), "--mutation-check", "--list"],
            {
                cwd: ctx.root,
                encoding: "utf8",
                env: ctx.env,
            },
        );
        const status = r.error ? `（起不来：${r.error.message}）` : (r.status ?? -1);
        const { problems, unlisted } = coverage({ required: REQUIRED, status, listText: r.stdout ?? "" });
        const lines = [...problems];
        if (r.status !== 0 && r.stderr) lines.push(...r.stderr.trim().split(/\r?\n/).slice(-5));
        if (unlisted.length) {
            lines.push(`有 mutationCheck 却没登记进 REQUIRED（被删时不会被认出来）：${unlisted.join("、")}`);
        }
        return { ok: problems.length === 0, lines };
    },
    selfTest: () => {
        const real = coverage({
            required: REQUIRED,
            status: 0,
            listText: REQUIRED.map((n) => `${n}\tdocs\t`).join("\n"),
        });
        return caseResult([
            ...cases(),
            { label: "REQUIRED 本身通过 shape 判据", bad: false, passed: real.problems.length === 0 },
            { label: "REQUIRED 含本检查自己", bad: false, passed: REQUIRED.includes("mutation-coverage") },
        ]);
    },
    /** 逐条关掉判据，自测用例必须因此至少红一条。 */
    mutationCheck: () => {
        const lines = [];
        for (const off of ["status", "empty", "none", "dup", "odd", "covered", "loaded"]) {
            if (cases({ [off]: true }).every((c) => c.passed)) lines.push(`关掉 ${off} 判据后自测仍是绿的`);
        }
        return { ok: lines.length === 0, lines };
    },
};
