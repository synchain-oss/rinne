// SPDX-License-Identifier: GPL-3.0-or-later
//
// 宿主分级表：真源是 docs/daw-tiers.json 的 hosts[]。等级与状态是这套数据的唯一真源，文档里只放生成块，
// 不另写一份等级表。真源文件由「宿主等级与发布平台」这一维度的卡维护，本模块只读。
//
// 渲染成一张 Markdown 表格：宿主 / 版本 / 平台 / 格式 / 等级 / 状态 / 备注。
// 等级与状态用同一份枚举（TIER_LABELS / STATUS_LABELS），中英两侧不另造文案；备注按 id 取 en / zh。
// 取值的合法性（等级、状态、字段齐全、证据齐备）由 docs 组的 tiers 检查管；本模块只管「能不能渲染出来」：
// 真源文件不在时 validate 返回空数组并跳过；文件在但 hosts 不成形时报明确的问题，render 抛错，不生成占位值。

import { readRepoText } from "../lib/docs-kit.mjs";
import { caseResult } from "../lib/check-kit.mjs";

export const SOURCE_FILE = "docs/daw-tiers.json";
/** 渲染时要读的宿主字段。 */
const HOST_FIELDS = ["host", "versions", "os", "formats", "tier", "status", "notes"];
/** checks 的子字段。 */
const CHECK_FIELDS = ["realtime", "offline", "freeze", "reopen", "buffers", "asioGuard", "loopPeriod"];

/** 等级与状态的显示文案：两份枚举共用一份文案，不另造说法。 */
export const TIER_LABELS = { 1: "Tier 1", 2: "Tier 2", 3: "Tier 3", "out-of-scope": "Out of scope" };
export const STATUS_LABELS = {
    "awaiting-test": "Awaiting test",
    partial: "Partial",
    verified: "Verified",
    "out-of-scope": "Out of scope",
};
/** 等级为 null（尚未在真机上验证）时等级列的写法。 */
export const TIER_UNTESTED = "—";

/** 判据名 → 说明。 */
export const RULES = {
    "hosts-shape": "hosts 是非空数组，每项有 host / versions / os / formats / tier / status / notes",
    "hosts-formats": "formats 是数组",
    "hosts-notes": "notes 有 en 与 zh 两份",
    "hosts-checks": "checks 有登记的全部子字段",
    "render-refuses": "真源不成形时 render 抛错，不生成占位表格",
};

/** 读 daw-tiers.json；文件不在返回 null，JSON 不合法抛错。 */
export function loadTiers(root) {
    const text = readRepoText(root, SOURCE_FILE);
    if (text === null) return null;
    try {
        return JSON.parse(text);
    } catch (e) {
        throw new Error(`${SOURCE_FILE} 不是合法的 JSON：${e.message}`);
    }
}

/** 表格单元里的竖线要转义，否则表格列数会算错。 */
const cell = (v) => String(v ?? "").replace(/\|/g, "\\|");

/**
 * 等级列的文案。tier 为 null 时看 status：out-of-scope 的宿主是「明确不支持」，
 * 与「还没在真机上验证」不是一回事，不能都写成破折号。
 */
export function tierLabel(tier, status) {
    if (tier === null || tier === undefined) {
        if (status === "out-of-scope") return TIER_LABELS["out-of-scope"];
        return TIER_UNTESTED;
    }
    return TIER_LABELS[tier] ?? String(tier);
}

/** 状态列的文案。 */
export const statusLabel = (status) => STATUS_LABELS[status] ?? String(status);

/** 表头：中英各一份，其余内容逐字相同。 */
const HEADERS = {
    en: ["Host", "Version", "Platform", "Formats", "Tier", "Status", "Notes"],
    zh: ["宿主", "版本", "平台", "格式", "等级", "状态", "备注"],
};

/** hosts 的形状问题；返回问题字符串数组，每条以 [判据名] 开头。 */
export function hostsProblems(tiers) {
    const out = [];
    if (!tiers || typeof tiers !== "object") return [`[hosts-shape] ${SOURCE_FILE} 的内容不是对象`];
    if (!Array.isArray(tiers.hosts)) return [`[hosts-shape] ${SOURCE_FILE} 缺 hosts 数组`];
    if (!tiers.hosts.length) return [`[hosts-shape] ${SOURCE_FILE} 的 hosts 数组是空的`];
    tiers.hosts.forEach((h, i) => {
        const where = `${SOURCE_FILE} 的 hosts 第 ${i + 1} 项`;
        if (!h || typeof h !== "object") return out.push(`[hosts-shape] ${where} 不是对象`);
        for (const k of HOST_FIELDS) if (!(k in h)) out.push(`[hosts-shape] ${where} 缺 ${k}`);
        if (!Array.isArray(h.formats)) out.push(`[hosts-formats] ${where} 的 formats 不是数组`);
        if (h.checks && typeof h.checks === "object")
            for (const k of CHECK_FIELDS) if (!(k in h.checks)) out.push(`[hosts-checks] ${where} 的 checks 缺 ${k}`);
        if (h.notes && typeof h.notes === "object")
            for (const lang of ["en", "zh"])
                if (typeof h.notes[lang] !== "string") out.push(`[hosts-notes] ${where} 的 notes 缺 ${lang}`);
    });
    return out;
}

/** 渲染出的表格。hosts 是 daw-tiers.json 的 hosts 数组；lang 是 "en" 或 "zh"。 */
export function renderTierTable(hosts, lang) {
    const head = HEADERS[lang] ?? HEADERS.en;
    const rows = hosts.map((h) => [
        cell(h.host),
        cell(h.versions),
        cell(h.os),
        cell((h.formats ?? []).join(" / ")),
        cell(tierLabel(h.tier, h.status)),
        cell(statusLabel(h.status)),
        cell(h.notes?.[lang] ?? h.notes?.en ?? ""),
    ]);
    const sep = head.map(() => "---");
    return [head, sep, ...rows].map((r) => `| ${r.join(" | ")} |`).join("\n");
}

/** 等级表；真源不成形时抛错。tiers 是 daw-tiers.json 的内容，lang 是 "en" 或 "zh"。 */
export function tierTableOf(tiers, lang) {
    const problems = hostsProblems(tiers);
    if (problems.length) throw new Error(problems.join("；"));
    return renderTierTable(tiers.hosts, lang);
}

/** 问题串里的判据名。 */
export const ruleOf = (problem) => /^\[([^\]]+)\]/.exec(problem)?.[1] ?? "";

/** 自测：每个判据都有坏样例点名，另有一个覆盖用例核对每条判据都被点名过。 */
export function selfTest() {
    const host = (over = {}) => ({
        host: "Cubase Pro",
        versions: "15",
        os: "windows",
        formats: ["VST3"],
        tier: null,
        targetTier: 1,
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
        notes: { en: "Not tested on a real host yet.", zh: "尚未在真机上测试。" },
        ...over,
    });
    const good = { hosts: [host()] };
    /** 去掉一个字段（真缺字段，不是值为 undefined）。 */
    const without = (h, k) => {
        const c = { ...h };
        delete c[k];
        return c;
    };
    const cases = [];
    const add = (label, tiers, expectRules) => {
        const rules = hostsProblems(tiers).map(ruleOf).filter(Boolean);
        cases.push({ label, bad: rules.length > 0, passed: expectRules.every((r) => rules.includes(r)) });
    };
    add("齐的 hosts 没问题", good, []);
    add("缺 hosts 数组", { platforms: [] }, ["hosts-shape"]);
    add("hosts 是空的", { hosts: [] }, ["hosts-shape"]);
    add("某项缺 status", { hosts: [without(host(), "status")] }, ["hosts-shape"]);
    add("formats 不是数组", { hosts: [{ ...host(), formats: "VST3" }] }, ["hosts-formats"]);
    add("notes 缺 zh", { hosts: [{ ...host(), notes: { en: "x" } }] }, ["hosts-notes"]);
    add("checks 缺一个子字段", { hosts: [{ ...host(), checks: { realtime: false } }] }, ["hosts-checks"]);
    // 判据 render-refuses：真源不成形时 render 抛错，不出占位表格
    const threw = (tiers) => {
        try {
            tierTableOf(tiers, "en");
            return false;
        } catch {
            return true;
        }
    };
    cases.push({
        label: "真源缺字段时 render 抛错",
        bad: true,
        passed: threw({ hosts: [without(host(), "status")] }),
    });
    cases.push({
        label: "真源齐备时 render 不抛错",
        bad: false,
        passed: !threw(good),
    });
    // 正例：表格的行数与 hosts 条数一致；等级列用同一份枚举
    const table = renderTierTable(good.hosts, "en");
    const rows = table.split("\n");
    cases.push({
        label: "表格行数与 hosts 条数一致",
        bad: false,
        passed:
            rows.length === good.hosts.length + 2 &&
            tierLabel(null, "awaiting-test") === TIER_UNTESTED &&
            tierLabel(null, "out-of-scope") === TIER_LABELS["out-of-scope"],
    });
    const seen = new Set();
    for (const t of [
        good,
        { platforms: [] },
        { hosts: [] },
        { hosts: [without(host(), "status")] },
        { hosts: [{ ...host(), formats: "VST3" }] },
        { hosts: [{ ...host(), notes: { en: "x" } }] },
        { hosts: [{ ...host(), checks: { realtime: false } }] },
    ])
        for (const p of hostsProblems(t)) seen.add(ruleOf(p));
    seen.add("render-refuses");
    cases.push({
        label: "每条判据都至少被一个坏样例点名",
        bad: false,
        passed: Object.keys(RULES).every((r) => seen.has(r)),
    });
    return caseResult(cases);
}

export default {
    name: "daw-tiers",
    sources: [SOURCE_FILE],
    surfaces: [
        { file: "README.md", id: "en", required: false },
        { file: "README.zh-CN.md", id: "zh", required: false },
        { file: "docs/DAW_COMPATIBILITY.md", id: "en", required: false },
    ],
    validate(ctx) {
        const tiers = loadTiers(ctx.root);
        if (tiers === null) return []; // 真源缺失：跳过，不报问题
        return hostsProblems(tiers);
    },
    render(ctx, id) {
        const tiers = loadTiers(ctx.root);
        if (tiers === null) throw new Error(`缺 ${SOURCE_FILE}`);
        return tierTableOf(tiers, id);
    },
    selfTest,
};
