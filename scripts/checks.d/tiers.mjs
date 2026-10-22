// SPDX-License-Identifier: GPL-3.0-or-later
//
// 宿主分级一致性（docs-truth 的一部分）。数据是 docs/daw-tiers.json：platforms 是平台与格式的清单，hosts 是一行一个宿主，
// 记它的目标等级（targetTier）、当前等级（tier）、状态（status）、证据与已过的验收项（checks）。
//
// 判据：
//   tiers-shape         顶层有 platforms 与 hosts；每条 host 有 host / versions / os / formats / tier / targetTier /
//                       status / evidence / checks / notes；notes 有 en 与 zh
//   tier-values         tier 与 targetTier ∈ {null, 1, 2, 3, "out-of-scope"}
//   status-values       status ∈ {awaiting-test, partial, verified, out-of-scope}
//   awaiting-null       status == "awaiting-test" ⇒ tier 为 null 且 evidence 为空
//   tier-evidence       tier 为 1 或 2 ⇒ evidence 非空，且每一条都成立（validation 文件存在 / issue 是 https URL / ci job 存在）
//   tier1-checks        tier === 1 ⇒ checks 全部为 true
//   verified-consistent status 与等级自洽：verified 要求 tier !== null 且 tier <= targetTier；partial 要求 tier 是 1…3
//                       的具体等级（partial = 部分验证，Tier 2 就是部分验证）；awaiting-test 与 out-of-scope 要求 tier 为 null
//   out-of-scope        status == "out-of-scope" ⇒ tier 为 null、evidence 为空、targetTier 为 "out-of-scope"
//   platforms-shape     platforms 每项有 id / os / arch / formats / minVersion；formats 取值在 {VST3, AU} 内；id 唯一
//
// 判据名见 RULES，报错行以 [名字] 开头。自测的每个坏样例写明它必须触发哪几条判据，并核对每条判据都至少被一个坏样例
// 点名：删掉任何一条判据，点名它的坏样例就变红。

import fs from "node:fs";
import path from "node:path";
import { ROOT, caseResult, readText } from "../lib/check-kit.mjs";
import { ciJobs, evidenceState } from "./claims.mjs";

export const TIERS_FILE = "docs/daw-tiers.json";

/** 判据名 → 说明。 */
export const RULES = {
    "tiers-shape": "daw-tiers.json 的形状：每条 host 的字段齐全，notes 有 en 与 zh",
    "tier-values": "tier 与 targetTier 的取值在登记的集合内",
    "status-values": "status 的取值在登记的集合内",
    "awaiting-null": "awaiting-test 的行 tier 为 null 且 evidence 为空",
    "tier-evidence": "Tier 1 与 Tier 2 必须有证据，且每条证据都成立",
    "tier1-checks": "Tier 1 的 checks 全部为 true",
    "verified-consistent":
        "status 与等级自洽：verified 要求 tier !== null 且 tier <= targetTier；partial 要求 tier 是 1…3 的具体等级；awaiting-test 与 out-of-scope 要求 tier 为 null",
    "out-of-scope": "out-of-scope 的行 tier 为 null、evidence 为空、targetTier 为 out-of-scope",
    "platforms-shape": "platforms 每项的字段齐全、formats 取值在集合内、id 唯一",
};

/** 等级的合法取值。 */
export const TIER_VALUES = [null, 1, 2, 3, "out-of-scope"];
/** 状态的合法取值。 */
export const STATUS_VALUES = ["awaiting-test", "partial", "verified", "out-of-scope"];
/** 合法格式。 */
export const FORMATS = ["VST3", "AU"];
/** 每条 host 必须有的字段。 */
export const HOST_FIELDS = [
    "host",
    "versions",
    "os",
    "formats",
    "tier",
    "targetTier",
    "status",
    "evidence",
    "checks",
    "notes",
];
/** 平台必须有的字段。 */
export const PLATFORM_FIELDS = ["id", "os", "arch", "formats", "minVersion"];

const isTier = (v) => TIER_VALUES.some((t) => t === v);
const isStatus = (v) => STATUS_VALUES.includes(v);
const isNull = (v) => v === null || v === undefined;

/**
 * 判据的实现。data 是 daw-tiers.json 解析后的对象；root 是仓根（evidence 查表用）。
 * 返回问题数组（每行以 [判据名] 开头）。
 */
export function tierProblems({ data, root, jobs = ciJobs(root) }) {
    const out = [];
    const add = (rule, message) => {
        if (!Object.hasOwn(RULES, rule)) throw new Error(`未登记的判据 ${rule}`);
        out.push(`[${rule}] ${message}`);
    };
    if (!data || typeof data !== "object" || !Array.isArray(data.platforms) || !Array.isArray(data.hosts)) {
        add("tiers-shape", `${TIERS_FILE} 顶层缺 platforms 或 hosts`);
        return out;
    }

    // 平台
    const ids = new Set();
    for (const p of data.platforms) {
        const id = p?.id ?? "(没有 id)";
        for (const f of PLATFORM_FIELDS)
            if (p?.[f] === undefined) add("platforms-shape", `${TIERS_FILE} 的平台「${id}」缺字段 ${f}`);
        if (typeof p?.id === "string") {
            if (ids.has(p.id)) add("platforms-shape", `${TIERS_FILE} 的平台 id「${p.id}」重复`);
            ids.add(p.id);
        }
        for (const f of p?.formats ?? [])
            if (!FORMATS.includes(f))
                add("platforms-shape", `${TIERS_FILE} 的平台「${id}」的格式「${f}」不在 {VST3, AU} 内`);
    }

    // 宿主
    for (const h of data.hosts) {
        const name = h?.host ?? "(没有 host)";
        for (const f of HOST_FIELDS)
            if (h?.[f] === undefined) add("tiers-shape", `${TIERS_FILE} 的宿主「${name}」缺字段 ${f}`);
        if (h?.notes && (typeof h.notes !== "object" || !h.notes.en || !h.notes.zh))
            add("tiers-shape", `${TIERS_FILE} 的宿主「${name}」的 notes 缺 en 或 zh`);
        if (h?.checks && typeof h.checks !== "object")
            add("tiers-shape", `${TIERS_FILE} 的宿主「${name}」的 checks 不是对象`);

        if (h?.tier !== undefined && !isTier(h.tier))
            add("tier-values", `${TIERS_FILE} 的宿主「${name}」的 tier「${h.tier}」不在登记的取值内`);
        if (h?.targetTier !== undefined && !isTier(h.targetTier))
            add("tier-values", `${TIERS_FILE} 的宿主「${name}」的 targetTier「${h.targetTier}」不在登记的取值内`);
        if (h?.status !== undefined && !isStatus(h.status))
            add("status-values", `${TIERS_FILE} 的宿主「${name}」的 status「${h.status}」不在登记的取值内`);

        const evidence = Array.isArray(h?.evidence) ? h.evidence : [];
        const tier = h?.tier;
        const status = h?.status;
        const target = h?.targetTier;

        if (status === "awaiting-test") {
            if (!isNull(tier)) add("awaiting-null", `${TIERS_FILE} 的宿主「${name}」是 awaiting-test，tier 应为 null`);
            if (evidence.length)
                add("awaiting-null", `${TIERS_FILE} 的宿主「${name}」是 awaiting-test，evidence 应为空`);
        }
        if (tier === 1 || tier === 2) {
            if (!evidence.length)
                add("tier-evidence", `${TIERS_FILE} 的宿主「${name}」是 Tier ${tier}，evidence 不能为空`);
            for (const item of evidence) {
                const state = evidenceState(root, item, jobs);
                if (state === "malformed")
                    add("tier-evidence", `${TIERS_FILE} 的宿主「${name}」的证据「${item}」形态不认识`);
                else if (state !== "ok") add("tier-evidence", `${TIERS_FILE} 的宿主「${name}」的证据「${item}」不成立`);
            }
        }
        if (tier === 1 && h?.checks && typeof h.checks === "object") {
            const falseOnes = Object.entries(h.checks)
                .filter(([, v]) => v !== true)
                .map(([k]) => k);
            if (falseOnes.length)
                add(
                    "tier1-checks",
                    `${TIERS_FILE} 的宿主「${name}」是 Tier 1，checks 的 ${falseOnes.join("、")} 还要 true`,
                );
        }
        // status 与等级自洽
        if (status === "verified") {
            if (isNull(tier) || typeof tier !== "number" || typeof target !== "number" || tier > target)
                add(
                    "verified-consistent",
                    `${TIERS_FILE} 的宿主「${name}」是 verified，但 tier（${tier}）与 targetTier（${target}）不自洽`,
                );
        } else if (status === "partial") {
            // partial = 只验证了列出的项目。等级按证据给，不固定成 3：Tier 2 就是「部分验证」。
            if (isNull(tier) || typeof tier !== "number")
                add("verified-consistent", `${TIERS_FILE} 的宿主「${name}」是 partial，tier 应为具体等级（1…3）`);
            else if (tier < 1 || tier > 3)
                add("verified-consistent", `${TIERS_FILE} 的宿主「${name}」是 partial，tier（${tier}）不在 1…3 内`);
        } else if (status === "awaiting-test" || status === "out-of-scope") {
            if (!isNull(tier))
                add("verified-consistent", `${TIERS_FILE} 的宿主「${name}」是 ${status}，tier 应为 null`);
        }
        if (status === "out-of-scope") {
            if (evidence.length) add("out-of-scope", `${TIERS_FILE} 的宿主「${name}」是 out-of-scope，evidence 应为空`);
            if (target !== "out-of-scope")
                add("out-of-scope", `${TIERS_FILE} 的宿主「${name}」是 out-of-scope，targetTier 应为 out-of-scope`);
        }
    }
    return out;
}

/** 一条 host 的默认行：全部未测。 */
export const hostRow = (host, versions, os, formats, targetTier, notesEn, notesZh) => ({
    host,
    versions,
    os,
    formats,
    tier: null,
    targetTier,
    status: targetTier === "out-of-scope" ? "out-of-scope" : "awaiting-test",
    evidence: [],
    checks: {
        realtime: false,
        offline: false,
        freeze: false,
        reopen: false,
        buffers: false,
        asioGuard: false,
        loopPeriod: false,
    },
    notes: { en: notesEn, zh: notesZh },
});

export async function selfTest(root = ROOT) {
    const jobs = new Set(["core-sanitizers"]);
    const good = {
        version: 1,
        platforms: [
            { id: "win-x64", os: "Windows", arch: "x64", formats: ["VST3"], minVersion: "Windows 10" },
            { id: "mac-arm64", os: "macOS", arch: "arm64", formats: ["VST3", "AU"], minVersion: "macOS 11" },
        ],
        hosts: [
            {
                host: "Cubase Pro",
                versions: "15",
                os: "windows",
                formats: ["VST3"],
                tier: 1,
                targetTier: 1,
                status: "verified",
                evidence: ["ci:core-sanitizers"],
                checks: {
                    realtime: true,
                    offline: true,
                    freeze: true,
                    reopen: true,
                    buffers: true,
                    asioGuard: true,
                    loopPeriod: true,
                },
                notes: { en: "Verified.", zh: "已验证。" },
            },
            {
                host: "LUNA",
                versions: "3.x",
                os: "macos",
                formats: ["VST3", "AU"],
                tier: null,
                targetTier: 2,
                status: "awaiting-test",
                evidence: [],
                checks: {
                    realtime: false,
                    offline: false,
                    freeze: false,
                    reopen: false,
                    buffers: false,
                    asioGuard: false,
                    loopPeriod: false,
                },
                notes: { en: "Not tested yet.", zh: "尚未测试。" },
            },
        ],
    };
    const run = (over) => tierProblems({ data: over ?? good, root, jobs });
    const cases = [
        ["一条 Tier 1 与一条未测的行都合规", [], null],
        [
            "partial 的行是 Tier 2（部分验证）时合规",
            [],
            {
                ...good,
                hosts: [
                    good.hosts[0],
                    {
                        ...good.hosts[1],
                        tier: 2,
                        targetTier: 2,
                        status: "partial",
                        evidence: ["ci:core-sanitizers"],
                    },
                ],
            },
        ],
        [
            "partial 的行是 Tier 3（只有 CI 证据）时合规",
            [],
            {
                ...good,
                hosts: [
                    good.hosts[0],
                    { ...good.hosts[1], tier: 3, targetTier: 3, status: "partial", evidence: ["ci:core-sanitizers"] },
                ],
            },
        ],
        [
            "partial 却没给 tier",
            ["verified-consistent"],
            {
                ...good,
                hosts: [
                    good.hosts[0],
                    { ...good.hosts[1], tier: null, status: "partial", evidence: ["ci:core-sanitizers"] },
                ],
            },
        ],
        [
            "partial 的 tier 不在 1…3 内",
            ["verified-consistent"],
            {
                ...good,
                hosts: [
                    good.hosts[0],
                    { ...good.hosts[1], tier: 4, status: "partial", evidence: ["ci:core-sanitizers"] },
                ],
            },
        ],
        [
            "Tier 1 的 evidence 为空",
            ["tier-evidence"],
            { ...good, hosts: [{ ...good.hosts[0], evidence: [] }, good.hosts[1]] },
        ],
        [
            "Tier 2 的证据指向不存在的 validation 文件",
            ["tier-evidence"],
            {
                ...good,
                hosts: [
                    good.hosts[0],
                    {
                        ...good.hosts[1],
                        tier: 2,
                        status: "partial",
                        evidence: ["validation:docs/validation/nowhere.md"],
                    },
                ],
            },
        ],
        [
            "awaiting-test 却给了 tier",
            ["awaiting-null", "verified-consistent"],
            { ...good, hosts: [good.hosts[0], { ...good.hosts[1], tier: 1 }] },
        ],
        [
            "awaiting-test 却有 evidence",
            ["awaiting-null"],
            { ...good, hosts: [good.hosts[0], { ...good.hosts[1], evidence: ["ci:core-sanitizers"] }] },
        ],
        [
            "Tier 1 但 checks 里有 false",
            ["tier1-checks"],
            {
                ...good,
                hosts: [{ ...good.hosts[0], checks: { ...good.hosts[0].checks, realtime: false } }, good.hosts[1]],
            },
        ],
        [
            "verified 但 tier 比 targetTier 高",
            ["verified-consistent"],
            { ...good, hosts: [{ ...good.hosts[0], tier: 3 }, good.hosts[1]] },
        ],
        [
            "tier 比 targetTier 好但 status 不是 verified",
            ["verified-consistent"],
            {
                ...good,
                hosts: [{ ...good.hosts[0], tier: 1, targetTier: 2, status: "awaiting-test" }, good.hosts[1]],
            },
        ],
        [
            "缺 notes.zh",
            ["tiers-shape"],
            {
                ...good,
                hosts: [good.hosts[0], { ...good.hosts[1], notes: { en: "x" } }],
            },
        ],
        [
            "out-of-scope 却带 evidence",
            ["out-of-scope"],
            {
                ...good,
                hosts: [
                    good.hosts[0],
                    {
                        ...good.hosts[1],
                        status: "out-of-scope",
                        targetTier: "out-of-scope",
                        evidence: ["ci:core-sanitizers"],
                    },
                ],
            },
        ],
        ["出现未登记的 tier 取值", ["tier-values"], { ...good, hosts: [good.hosts[0], { ...good.hosts[1], tier: 4 }] }],
        [
            "出现未登记的 status 取值",
            ["status-values"],
            { ...good, hosts: [good.hosts[0], { ...good.hosts[1], status: "maybe" }] },
        ],
        ["host 缺字段", ["tiers-shape"], { ...good, hosts: [{ host: "X" }] }],
        ["平台缺字段", ["platforms-shape"], { ...good, platforms: [{ id: "win-x64" }], hosts: good.hosts }],
        [
            "平台的格式不在集合内",
            ["platforms-shape"],
            {
                ...good,
                platforms: [{ ...good.platforms[0], formats: ["LV2"] }],
                hosts: good.hosts,
            },
        ],
        [
            "平台 id 重复",
            ["platforms-shape"],
            { ...good, platforms: [good.platforms[0], good.platforms[0]], hosts: good.hosts },
        ],
    ].map(([label, expect, data]) => {
        const errors = run(data ?? null);
        const missed = expect.filter((r) => !errors.some((e) => e.startsWith(`[${r}] `)));
        return {
            label: missed.length ? `${label}（没有触发 ${missed.join("、")}）` : label,
            bad: !!expect.length,
            passed: expect.length ? missed.length === 0 : errors.length === 0,
            expect,
        };
    });
    const unnamed = Object.keys(RULES).filter((r) => !cases.some((c) => c.expect.includes(r)));
    cases.push({
        label: `每条判据都有坏样例点名${unnamed.length ? `（没有：${unnamed.join("、")}）` : ""}`,
        bad: false,
        passed: unnamed.length === 0,
    });
    return caseResult(cases);
}

export default {
    name: "tiers",
    group: "docs",
    inputs: [],
    run(ctx) {
        const text = readText(path.join(ctx.root, TIERS_FILE));
        if (text === null) return { ok: false, lines: [`[tiers-shape] 缺 ${TIERS_FILE}`] };
        let data;
        try {
            data = JSON.parse(text);
        } catch (e) {
            return { ok: false, lines: [`[tiers-shape] ${TIERS_FILE} 不是合法 JSON：${e.message}`] };
        }
        const problems = tierProblems({ data, root: ctx.root });
        return { ok: problems.length === 0, lines: problems };
    },
    selfTest: (ctx) => selfTest(ctx.root),
};
