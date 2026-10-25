// SPDX-License-Identifier: GPL-3.0-or-later
//
// 宿主分级表：真源是 docs/daw-tiers.json 的 hosts[]。等级与状态是这套数据的唯一真源，文档里只放生成块，
// 不另写一份等级表。等级与状态的取值（TIER_VALUES / STATUS_VALUES）和显示文案（中英各一份）都登记在本文件，
// docs 组的 tiers 检查从这里引入同一份取值，不另抄。
//
// 渲染成一张 Markdown 表格：宿主 / 版本 / 平台 / 格式 / 等级 / 状态 / 备注。
// 平台列写显示名（取自 platforms 里的 os 字段，如 Windows、macOS；any 与 linux 用本文件的小表），不输出 json 里的小写标识。
// 取值的合法性（等级、状态、字段齐全）本模块只查「能不能渲染出来」：取值或显示名认不出时报问题、render 抛错，
// 不生成占位值；证据是否齐备由 tiers 检查管。
// 真源文件不在时 validate 返回空数组并跳过。

import { readRepoText } from "../lib/docs-kit.mjs";
import { caseResult } from "../lib/check-kit.mjs";

export const SOURCE_FILE = "docs/daw-tiers.json";
/** 渲染时要读的宿主字段。 */
const HOST_FIELDS = ["host", "versions", "os", "formats", "tier", "status", "notes"];
/** checks 的子字段（验收矩阵的七项）。 */
export const CHECK_FIELDS = ["realtime", "offline", "freeze", "reopen", "buffers", "asioGuard", "loopPeriod"];

/** 等级的合法取值：null 是还没在真机上验证；数字越小等级越高。 */
export const TIER_VALUES = [null, 1, 2, 3, "out-of-scope"];
/** 状态的合法取值。 */
export const STATUS_VALUES = ["awaiting-test", "partial", "verified", "out-of-scope"];

/** 等级与状态的显示文案：每种语言一份，键就是上面的取值。 */
export const TIER_LABELS = {
    en: { 1: "Tier 1", 2: "Tier 2", 3: "Tier 3", "out-of-scope": "Out of scope" },
    zh: { 1: "Tier 1", 2: "Tier 2", 3: "Tier 3", "out-of-scope": "不支持" },
};
export const STATUS_LABELS = {
    en: { "awaiting-test": "Awaiting test", partial: "Partial", verified: "Verified", "out-of-scope": "Out of scope" },
    zh: { "awaiting-test": "待实测", partial: "部分验证", verified: "已验证", "out-of-scope": "不支持" },
};
/** 等级为 null（尚未在真机上验证）时等级列的写法。 */
export const TIER_UNTESTED = "—";
/** platforms 里没有的系统标识的显示名。 */
const OS_FALLBACK = { any: { en: "Any", zh: "任意" }, linux: { en: "Linux", zh: "Linux" } };

/** 判据名 → 说明。 */
export const RULES = {
    "hosts-shape": "hosts 是非空数组，每项有 host / versions / os / formats / tier / status / notes",
    "hosts-formats": "formats 是数组",
    "hosts-notes": "notes 有 en 与 zh 两份",
    "hosts-checks": "checks 是对象，有登记的全部子字段",
    "hosts-values": "tier、status、os 的取值认得出，渲染得出显示名",
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

/** 表格单元：竖线转义（否则列数会算错），换行换成 <br>（否则表格会被撕开）。 */
const cell = (v) =>
    String(v ?? "")
        .replace(/\|/g, "\\|")
        .replace(/\r?\n/g, "<br>");

/** 表头：中英各一份，其余内容逐字相同。 */
const HEADERS = {
    en: ["Host", "Version", "Platform", "Formats", "Tier", "Status", "Notes"],
    zh: ["宿主", "版本", "平台", "格式", "等级", "状态", "备注"],
};

/** 系统的显示名：先看 platforms 里的 os（按小写对），再看小表；认不出返回 null。 */
export function osLabel(os, platforms, lang) {
    const key = String(os).toLowerCase();
    const hit = (platforms ?? []).find((p) => String(p?.os ?? "").toLowerCase() === key);
    if (hit) return hit.os;
    return OS_FALLBACK[key]?.[lang] ?? null;
}

/**
 * 等级列的文案。tier 为 null 时看 status：out-of-scope 的宿主是「明确不支持」，
 * 与「还没在真机上验证」不是一回事，不能都写成破折号。认不出的取值抛错，不回退成原值。
 */
export function tierLabel(tier, status, lang = "en") {
    if (tier === null || tier === undefined) {
        if (status === "out-of-scope") return TIER_LABELS[lang]["out-of-scope"];
        if (status === "awaiting-test") return TIER_UNTESTED;
        throw new Error(`状态 ${status} 的行没有等级，渲染不了等级列`);
    }
    const label = TIER_LABELS[lang]?.[tier];
    if (label === undefined) throw new Error(`认不出的等级 ${tier}`);
    return label;
}

/** 状态列的文案；认不出的取值抛错。 */
export function statusLabel(status, lang = "en") {
    const label = STATUS_LABELS[lang]?.[status];
    if (label === undefined) throw new Error(`认不出的状态 ${status}`);
    return label;
}

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
        if ("checks" in h) {
            if (!h.checks || typeof h.checks !== "object" || Array.isArray(h.checks))
                out.push(`[hosts-checks] ${where} 的 checks 不是对象`);
            else
                for (const k of CHECK_FIELDS)
                    if (!(k in h.checks)) out.push(`[hosts-checks] ${where} 的 checks 缺 ${k}`);
        }
        if ("notes" in h) {
            if (!h.notes || typeof h.notes !== "object") out.push(`[hosts-notes] ${where} 的 notes 不是对象`);
            else
                for (const lang of ["en", "zh"])
                    if (typeof h.notes[lang] !== "string") out.push(`[hosts-notes] ${where} 的 notes 缺 ${lang}`);
        }
        if ("tier" in h && !TIER_VALUES.some((t) => t === h.tier))
            out.push(`[hosts-values] ${where} 的 tier「${h.tier}」不在登记的取值内`);
        if ("status" in h && !STATUS_VALUES.includes(h.status))
            out.push(`[hosts-values] ${where} 的 status「${h.status}」不在登记的取值内`);
        if ("tier" in h && "status" in h && h.tier === null && !["awaiting-test", "out-of-scope"].includes(h.status))
            out.push(`[hosts-values] ${where} 的 status 是 ${h.status} 却没有等级，渲染不了等级列`);
        if (typeof h.os === "string") {
            if (osLabel(h.os, tiers.platforms, "en") === null)
                out.push(`[hosts-values] ${where} 的 os「${h.os}」既不在 platforms 里也不是 any / linux，没有显示名`);
        } else if ("os" in h) out.push(`[hosts-values] ${where} 的 os 不是字符串`);
    });
    return out;
}

/** 渲染出的表格。tiers 是 daw-tiers.json 的内容（已通过 hostsProblems）；lang 是 "en" 或 "zh"。 */
export function renderTierTable(tiers, lang) {
    const head = HEADERS[lang];
    if (!head) throw new Error(`未知的语言 ${lang}`);
    const rows = tiers.hosts.map((h) => [
        cell(h.host),
        cell(h.versions),
        cell(osLabel(h.os, tiers.platforms, lang)),
        cell(h.formats.join(" / ")),
        cell(tierLabel(h.tier, h.status, lang)),
        cell(statusLabel(h.status, lang)),
        cell(h.notes[lang]),
    ]);
    const sep = head.map(() => "---");
    return [head, sep, ...rows].map((r) => `| ${r.join(" | ")} |`).join("\n");
}

/** 等级表；真源不成形时抛错。tiers 是 daw-tiers.json 的内容，lang 是 "en" 或 "zh"。 */
export function tierTableOf(tiers, lang) {
    const problems = hostsProblems(tiers);
    if (problems.length) throw new Error(problems.join("；"));
    return renderTierTable(tiers, lang);
}

/** 问题串里的判据名。 */
export const ruleOf = (problem) => /^\[([^\]]+)\]/.exec(problem)?.[1] ?? "";

/**
 * 自测用例：每个用例 { label, bad, passed, expect }。期望为空表示正例（一条问题都不许有），
 * 否则是坏样例（期望的判据都要被点名）。末尾的覆盖用例核对每条判据都被某个通过的坏样例点名。
 */
export function selfTestCases() {
    const checks = Object.fromEntries(CHECK_FIELDS.map((k) => [k, false]));
    const host = (over = {}) => ({
        host: "Cubase Pro",
        versions: "15",
        os: "windows",
        formats: ["VST3"],
        tier: null,
        targetTier: 1,
        status: "awaiting-test",
        evidence: [],
        checks,
        notes: { en: "Not tested on a real host yet.", zh: "尚未在真机上测试。" },
        ...over,
    });
    const platforms = [
        { id: "win-x64", os: "Windows", arch: "x64", formats: ["VST3"] },
        { id: "mac-arm64", os: "macOS", arch: "arm64", formats: ["VST3", "AU"] },
    ];
    const doc = (...hosts) => ({ platforms, hosts });
    const good = doc(host());
    /** 去掉一个字段（真缺字段，不是值为 undefined）。 */
    const without = (h, k) => {
        const c = { ...h };
        delete c[k];
        return c;
    };
    const cases = [];
    const add = (label, tiers, expect) => {
        const rules = hostsProblems(tiers).map(ruleOf).filter(Boolean);
        const bad = expect.length > 0;
        cases.push({ label, bad, passed: bad ? expect.every((r) => rules.includes(r)) : rules.length === 0, expect });
    };
    const threw = (tiers) => {
        try {
            tierTableOf(tiers, "en");
            return false;
        } catch {
            return true;
        }
    };
    add("齐的 hosts 没问题", good, []);
    add("缺 hosts 数组", { platforms: [] }, ["hosts-shape"]);
    add("hosts 是空的", { hosts: [] }, ["hosts-shape"]);
    add("某项缺 status", doc(without(host(), "status")), ["hosts-shape"]);
    add("某项不是对象", doc(null), ["hosts-shape"]);
    add("formats 不是数组", doc(host({ formats: "VST3" })), ["hosts-formats"]);
    add("notes 缺 zh", doc(host({ notes: { en: "x" } })), ["hosts-notes"]);
    add("notes 是 null", doc(host({ notes: null })), ["hosts-notes"]);
    add("checks 缺一个子字段", doc(host({ checks: { realtime: false } })), ["hosts-checks"]);
    add("checks 是 null", doc(host({ checks: null })), ["hosts-checks"]);
    add("tier 取值认不出", doc(host({ tier: 4 })), ["hosts-values"]);
    add("status 取值认不出（等级是具体数字，只剩取值这一处不对）", doc(host({ status: "maybe", tier: 2 })), [
        "hosts-values",
    ]);
    add("verified 却没有等级", doc(host({ status: "verified", tier: null })), ["hosts-values"]);
    add("os 没有显示名", doc(host({ os: "solaris" })), ["hosts-values"]);
    add("os 不是字符串", doc(host({ os: 5 })), ["hosts-values"]);
    // 判据 render-refuses：真源不成形时 render 抛错，不出占位表格
    cases.push({
        label: "真源缺字段时 render 抛错",
        bad: true,
        passed: threw(doc(without(host(), "status"))),
        expect: ["render-refuses"],
    });
    cases.push({
        label: "notes 缺 zh 时连英文表也拒绝渲染（不靠渲染中途碰巧出错）",
        bad: true,
        passed: threw(doc(host({ notes: { en: "x" } }))),
        expect: ["render-refuses"],
    });
    cases.push({ label: "真源齐备时 render 不抛错", bad: false, passed: !threw(good), expect: [] });
    // 正例：行数、平台显示名、中英文案、转义
    const rich = doc(
        host({ notes: { en: "a | b\nsecond line", zh: "甲 | 乙\n第二行" } }),
        host({ host: "GarageBand", os: "macos", formats: ["AU"], status: "out-of-scope", targetTier: "out-of-scope" }),
        host({ host: "CLAP", os: "any", formats: ["CLAP"], status: "out-of-scope", targetTier: "out-of-scope" }),
    );
    const en = tierTableOf(rich, "en");
    const zh = tierTableOf(rich, "zh");
    cases.push({
        label: "表格行数与 hosts 条数一致，换行不撕开表格",
        bad: false,
        passed: en.split("\n").length === rich.hosts.length + 2 && zh.split("\n").length === rich.hosts.length + 2,
        expect: [],
    });
    cases.push({
        label: "平台列写显示名（Windows、macOS、Any），不写小写标识",
        bad: false,
        passed:
            en.includes("| Windows |") &&
            en.includes("| macOS |") &&
            en.includes("| Any |") &&
            !en.includes("| windows |"),
        expect: [],
    });
    cases.push({
        label: "中文表的表头、状态与等级是中文，英文表是英文",
        bad: false,
        passed:
            zh.includes("| 宿主 |") &&
            zh.includes("待实测") &&
            zh.includes("不支持") &&
            !zh.includes("Awaiting") &&
            en.includes("| Host |") &&
            en.includes("Awaiting test") &&
            en.includes("Out of scope") &&
            !en.includes("待实测"),
        expect: [],
    });
    cases.push({
        label: "竖线转义、换行换成 <br>",
        bad: false,
        passed: en.includes("a \\| b<br>second line") && zh.includes("甲 \\| 乙<br>第二行"),
        expect: [],
    });
    cases.push({
        label: "登记的每个等级与状态在中英两种语言里都有文案",
        bad: false,
        passed:
            TIER_VALUES.filter((t) => t !== null).every((t) =>
                ["en", "zh"].every((l) => TIER_LABELS[l][t] !== undefined),
            ) && STATUS_VALUES.every((s) => ["en", "zh"].every((l) => STATUS_LABELS[l][s] !== undefined)),
        expect: [],
    });
    const unnamed = Object.keys(RULES).filter((r) => !cases.some((c) => c.passed && c.expect.includes(r)));
    cases.push({
        label: `每条判据都有坏样例点名${unnamed.length ? `（没有：${unnamed.join("、")}）` : ""}`,
        bad: false,
        passed: unnamed.length === 0,
        expect: [],
    });
    return cases;
}

export const selfTest = () => caseResult(selfTestCases());

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
