// SPDX-License-Identifier: GPL-3.0-or-later
//
// 平台徽章：真源是 docs/daw-tiers.json 的顶层 platforms。渲染成一行「Windows x64 VST3 · macOS arm64 VST3/AU」：
// 一个平台写成「系统 架构 格式」，同一平台的多个格式用斜杠连起来，平台与平台之间用间隔号隔开，
// 这样间隔号只表示「换一个平台」，不会和格式的分隔混在一起。
// 平台名与格式名是专有名词，中英同形，所以 en 与 zh 两个 id 渲染出来逐字相同。
//
// 取值的合法性由 docs 组的 tiers 检查管（platforms-shape）；本模块只管「能不能渲染出来」：
// 真源文件不在时 validate 返回空数组并跳过；文件在但 platforms 不成形时报明确的问题，render 抛错，不生成占位值。

import { readRepoText } from "../lib/docs-kit.mjs";
import { caseResult } from "../lib/check-kit.mjs";

export const SOURCE_FILE = "docs/daw-tiers.json";
/** 平台与平台之间的分隔：间隔号 U+00B7。 */
export const PLATFORM_SEPARATOR = " · ";
/** 同一平台的多个格式之间的分隔。 */
export const FORMAT_SEPARATOR = "/";
/** 渲染时要读的平台字段（formats 是数组，另判）。 */
const PLATFORM_FIELDS = ["id", "os", "arch"];

/** 判据名 → 说明。 */
export const RULES = {
    "platforms-shape": "platforms 是非空数组，每项有 id / os / arch / formats",
    "platforms-unique": "platforms 的 id 不重名",
    "render-refuses": "真源不成形时 render 抛错，不生成占位徽章",
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

/** platforms 的形状问题；返回问题字符串数组，每条以 [判据名] 开头。 */
export function platformsProblems(tiers) {
    const out = [];
    if (!tiers || typeof tiers !== "object") return [`[platforms-shape] ${SOURCE_FILE} 的内容不是对象`];
    if (!Array.isArray(tiers.platforms)) return [`[platforms-shape] ${SOURCE_FILE} 缺 platforms 数组`];
    if (!tiers.platforms.length) return [`[platforms-shape] ${SOURCE_FILE} 的 platforms 数组是空的`];
    const ids = new Set();
    tiers.platforms.forEach((p, i) => {
        const where = `${SOURCE_FILE} 的 platforms 第 ${i + 1} 项`;
        if (!p || typeof p !== "object") return out.push(`[platforms-shape] ${where} 不是对象`);
        for (const k of PLATFORM_FIELDS)
            if (typeof p[k] !== "string" || !p[k]) out.push(`[platforms-shape] ${where} 缺 ${k}`);
        if (!Array.isArray(p.formats) || !p.formats.length) out.push(`[platforms-shape] ${where} 缺 formats 数组`);
        else if (p.formats.some((f) => typeof f !== "string" || !f))
            out.push(`[platforms-shape] ${where} 的 formats 里有不是非空字符串的项`);
        if (typeof p.id === "string") {
            if (ids.has(p.id)) out.push(`[platforms-unique] ${where} 的 id ${p.id} 与别的项重名`);
            ids.add(p.id);
        }
    });
    return out;
}

/** 一行徽章。platforms 是 daw-tiers.json 的 platforms 数组。 */
export function renderBadge(platforms) {
    return platforms.map((p) => `${p.os} ${p.arch} ${p.formats.join(FORMAT_SEPARATOR)}`).join(PLATFORM_SEPARATOR);
}

/** 徽章；真源不成形时抛错。tiers 是 daw-tiers.json 的内容。 */
export function badgeOf(tiers) {
    const problems = platformsProblems(tiers);
    if (problems.length) throw new Error(problems.join("；"));
    return renderBadge(tiers.platforms);
}

/** 问题串里的判据名。 */
export const ruleOf = (problem) => /^\[([^\]]+)\]/.exec(problem)?.[1] ?? "";

/**
 * 自测用例：每个用例 { label, bad, passed, expect }。期望为空表示正例（一条问题都不许有），
 * 否则是坏样例（期望的判据都要被点名）。末尾的覆盖用例核对每条判据都被某个通过的坏样例点名。
 */
export function selfTestCases() {
    const good = {
        platforms: [
            { id: "win-x64", os: "Windows", arch: "x64", formats: ["VST3"] },
            { id: "mac-arm64", os: "macOS", arch: "arm64", formats: ["VST3", "AU"] },
        ],
    };
    const cases = [];
    const add = (label, tiers, expect) => {
        const rules = platformsProblems(tiers).map(ruleOf).filter(Boolean);
        const bad = expect.length > 0;
        cases.push({ label, bad, passed: bad ? expect.every((r) => rules.includes(r)) : rules.length === 0, expect });
    };
    const threw = (tiers) => {
        try {
            badgeOf(tiers);
            return false;
        } catch {
            return true;
        }
    };
    add("齐的 platforms 没问题", good, []);
    add("缺 platforms 数组", { hosts: [] }, ["platforms-shape"]);
    add("platforms 是空的", { platforms: [] }, ["platforms-shape"]);
    add("某项缺 arch", { platforms: [{ id: "a", os: "Windows", formats: ["VST3"] }] }, ["platforms-shape"]);
    add("某项缺 formats", { platforms: [{ id: "a", os: "Windows", arch: "x64" }] }, ["platforms-shape"]);
    add("某项不是对象", { platforms: [null] }, ["platforms-shape"]);
    add("formats 里有空串", { platforms: [{ id: "a", os: "Windows", arch: "x64", formats: [""] }] }, [
        "platforms-shape",
    ]);
    add("id 重名", { platforms: [good.platforms[0], good.platforms[0]] }, ["platforms-unique"]);
    // 判据 render-refuses：真源不成形时 render 抛错，不出占位徽章
    const broken = { platforms: [{ id: "a", os: "Windows", formats: ["VST3"] }] };
    cases.push({ label: "真源缺字段时 render 抛错", bad: true, passed: threw(broken), expect: ["render-refuses"] });
    cases.push({ label: "真源齐备时 render 不抛错", bad: false, passed: !threw(good), expect: [] });
    // 正例：格式之间用斜杠，平台之间用间隔号
    cases.push({
        label: "徽章渲染成「Windows x64 VST3 · macOS arm64 VST3/AU」",
        bad: false,
        passed: badgeOf(good) === "Windows x64 VST3 · macOS arm64 VST3/AU",
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
    name: "platform-badge",
    sources: [SOURCE_FILE],
    surfaces: [
        { file: "README.md", id: "en", required: false },
        { file: "README.zh-CN.md", id: "zh", required: false },
    ],
    validate(ctx) {
        const tiers = loadTiers(ctx.root);
        if (tiers === null) return []; // 真源缺失：跳过，不报问题
        return platformsProblems(tiers);
    },
    render(ctx) {
        const tiers = loadTiers(ctx.root);
        if (tiers === null) throw new Error(`缺 ${SOURCE_FILE}`);
        return badgeOf(tiers);
    },
    selfTest,
};
