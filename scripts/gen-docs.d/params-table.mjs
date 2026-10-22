// SPDX-License-Identifier: GPL-3.0-or-later
//
// 参数表：真源是 tests/golden/params-input.json（参数的范围、默认值、单位、是否可自动化）与
// web/i18n/input.js（界面上的显示名）。两张真源今天都还不在仓里（分别由参数与界面两条线拥有），所以本模块走跳过路径。
//
// 缺字段时的口径（卡验收的硬要求）：
//   - golden 文件不存在 → 跳过：validate 返回空数组，render 不被调用；
//   - golden 存在但缺字段（缺参数集合 / 某参数缺 id / 缺 default / 缺 range） → 明确报错，指名字段与参数，
//     例如「tests/golden/params-input.json 的 feedback 缺 default」；绝不生成占位值，也不拿参数 ID 顶替显示名；
//   - 显示名缺某个参数 → 同样报错：「web/i18n/input.js 缺参数 feedback 的显示名」。
//
// golden 的参数集合有两种形状，都认：
//   数组形  {"params": [{ "id": "feedback", "default": 0, ... }, ...]}（键名 parameters 同义）
//   映射形  {"feedback": { "default": 0, ... }, ...}——键就是参数 ID。
// 映射形与 claims 检查的谓词路径一致（golden:…#feedback.default == 0），两种形状都要能取到同一个 default。
//
// 显示名的取法：golden 的每条参数用 i18nKey 指到 web/i18n/input.js 里的一个词条；没有 i18nKey 时退到「词条表里
// 有一项的键与参数 ID 同名」。两条都取不到就报错——显示名是界面那条线的真源，本模块不猜哪条参数该用哪个词条。

import fs from "node:fs";
import path from "node:path";
import { createRequire } from "node:module";
import { readRepoText } from "../lib/docs-kit.mjs";
import { caseResult } from "../lib/check-kit.mjs";

export const GOLDEN_FILE = "tests/golden/params-input.json";
export const I18N_FILE = "web/i18n/input.js";

/** 表头。id 决定语言：en / zh 各一份，其余 id（契约文档）用中文。 */
const HEADERS = {
    en: ["ID", "Display name", "Range", "Default", "Unit", "Automatable"],
    zh: ["ID", "显示名", "范围", "默认值", "单位", "可自动化"],
};
/** id → 语言。契约文档正文用中文（CLAUDE.md 的「文档、上游与第三方」一节）。 */
const ID_LANG = { en: "en", zh: "zh", params: "zh" };

/** 判据名 → 说明。 */
export const RULES = {
    "golden-shape": "golden 里有非空的参数集合，每项都有 id",
    "golden-unique": "参数 id 不重名",
    "golden-default": "每个参数都有 default",
    "golden-range": "每个参数都有范围",
    "i18n-missing": "显示名词条文件在，但缺某个参数的显示名",
    "render-refuses": "真源不成形时 render 抛错，不生成占位表格",
};

/** 读 golden；文件不在返回 null，JSON 不合法抛错。 */
export function loadGolden(root) {
    const text = readRepoText(root, GOLDEN_FILE);
    if (text === null) return null;
    try {
        return JSON.parse(text);
    } catch (e) {
        throw new Error(`${GOLDEN_FILE} 不是合法的 JSON：${e.message}`);
    }
}

/**
 * 读 web/i18n/input.js 的词条表（{ zh, en, fr }）；文件不在返回 null。
 * 词条文件是 ES 模块，用 require 读进来（Node 22 的 require(esm)）；钉版 Node 见 .node-version。
 */
export function loadI18n(root) {
    const abs = path.resolve(root, I18N_FILE);
    if (!fs.existsSync(abs)) return null;
    let mod;
    try {
        mod = createRequire(import.meta.url)(abs);
    } catch (e) {
        throw new Error(`${I18N_FILE} 读不出来：${e.message}`);
    }
    const dict = mod?.default ?? mod;
    if (!dict || typeof dict !== "object") throw new Error(`${I18N_FILE} 没有默认导出的词条表`);
    return dict;
}

/**
 * golden 里的参数集合，统一成数组。
 * 数组形：主键名 params，别名 parameters；映射形：顶层键就是参数 ID。
 * 两种形状都认；都不成形时返回 null。
 * 映射形里值为非对象的顶层键（如 version、status）当元数据，不算参数。
 */
export function paramsOf(golden) {
    if (!golden || typeof golden !== "object") return null;
    if (Array.isArray(golden.params)) return golden.params;
    if (Array.isArray(golden.parameters)) return golden.parameters;
    const entries = Object.entries(golden).filter(([, v]) => v && typeof v === "object" && !Array.isArray(v));
    return entries.map(([id, v]) => ({ id, ...v }));
}

/** 表格单元里的竖线要转义，否则表格列数会算错。 */
const cell = (v) => String(v ?? "").replace(/\|/g, "\\|");

/** 默认值的写法：字符串加引号（空串与缺值要分得开），其余按字面量。 */
export const defaultText = (v) => (typeof v === "string" ? JSON.stringify(v) : String(v));

/** 范围的写法：range 可以是字符串、数组或对象；否则用 min 与 max 拼。 */
export function rangeText(param) {
    if (param.range !== undefined && param.range !== null) {
        if (typeof param.range === "string") return param.range;
        if (Array.isArray(param.range)) return param.range.join("…");
        if (typeof param.range === "object") {
            const { min, max } = param.range;
            if (min !== undefined && max !== undefined) return `${min}…${max}`;
        }
    }
    if (param.min !== undefined && param.max !== undefined) return `${param.min}…${param.max}`;
    return null;
}

/** 显示名的词条键：golden 的 i18nKey 优先；否则取词条表里与参数 ID 同名的键。 */
export function nameKeyOf(param, dict) {
    if (typeof param.i18nKey === "string" && param.i18nKey) return param.i18nKey;
    if (!dict) return null;
    for (const lang of Object.keys(dict)) {
        const table = dict[lang];
        if (table && typeof table === "object" && param.id in table) return param.id;
    }
    return null;
}

/** 参数集合的形状问题；返回问题字符串数组，每条以 [判据名] 开头。dict 为 null 时不查显示名。 */
export function paramsProblems(golden, dict) {
    const out = [];
    const params = paramsOf(golden);
    if (params === null) return [`[golden-shape] ${GOLDEN_FILE} 缺参数集合`];
    if (!params.length) return [`[golden-shape] ${GOLDEN_FILE} 的参数集合是空的`];
    const ids = new Set();
    for (const p of params) {
        if (!p || typeof p !== "object") {
            out.push(`[golden-shape] ${GOLDEN_FILE} 的参数集合里有不是对象的项`);
            continue;
        }
        const id = typeof p.id === "string" && p.id ? p.id : null;
        const where = id ? `${GOLDEN_FILE} 的 ${id}` : `${GOLDEN_FILE} 的参数集合里有一项`;
        if (!id) out.push(`[golden-shape] ${where} 缺 id`);
        else if (ids.has(id)) out.push(`[golden-unique] ${where} 的 id 与别的项重名`);
        else ids.add(id);
        if (p.default === undefined || p.default === null) out.push(`[golden-default] ${where} 缺 default`);
        if (rangeText(p) === null) out.push(`[golden-range] ${where} 缺 range`);
        if (dict && id) {
            const key = nameKeyOf(p, dict);
            if (!key) out.push(`[i18n-missing] ${I18N_FILE} 缺参数 ${id} 的显示名`);
            else {
                const missing = Object.keys(dict).filter((l) => typeof dict[l]?.[key] !== "string");
                if (missing.length)
                    out.push(
                        `[i18n-missing] ${I18N_FILE} 缺参数 ${id} 的显示名（键 ${key}，缺 ${missing.join("、")}）`,
                    );
            }
        }
    }
    return out;
}

/** 渲染出的表格。params 是 golden 的参数数组；dict 是词条表；lang 是 "en" 或 "zh"。 */
export function renderParamTable(params, dict, lang) {
    const head = HEADERS[lang] ?? HEADERS.en;
    const rows = params.map((p) => {
        const key = nameKeyOf(p, dict);
        const name = key ? (dict?.[lang]?.[key] ?? "") : "";
        return [
            cell(p.id),
            cell(name),
            cell(rangeText(p) ?? ""),
            cell(defaultText(p.default)),
            cell(p.unit ?? ""),
            cell(p.automatable === undefined ? "" : p.automatable ? "是" : "否"),
        ];
    });
    const sep = head.map(() => "---");
    return [head, sep, ...rows].map((r) => `| ${r.join(" | ")} |`).join("\n");
}

/** 参数表；真源不成形时抛错。golden 是 params-input.json 的内容，dict 是词条表，lang 是 "en" 或 "zh"。 */
export function paramTableOf(golden, dict, lang) {
    const problems = paramsProblems(golden, dict);
    if (problems.length) throw new Error(problems.join("；"));
    return renderParamTable(paramsOf(golden), dict, lang);
}

/** 问题串里的判据名。 */
export const ruleOf = (problem) => /^\[([^\]]+)\]/.exec(problem)?.[1] ?? "";

/** 自测：每个判据都有坏样例点名，另有一个覆盖用例核对每条判据都被点名过。 */
export function selfTest() {
    const param = (over = {}) => ({ id: "feedback", range: "0…1", default: 0, ...over });
    const dict = { zh: { feedback: "反馈" }, en: { feedback: "Feedback" }, fr: { feedback: "Retour" } };
    const cases = [];
    const add = (label, golden, d, expectRules) => {
        const rules = paramsProblems(golden, d).map(ruleOf).filter(Boolean);
        cases.push({ label, bad: rules.length > 0, passed: expectRules.every((r) => rules.includes(r)) });
    };
    add("齐的参数表没问题（数组形）", { params: [param()] }, dict, []);
    add("齐的参数表没问题（映射形）", { feedback: { range: "0…1", default: 0 } }, dict, []);
    add("缺参数集合", {}, dict, ["golden-shape"]);
    add("参数集合是空的", { params: [] }, dict, ["golden-shape"]);
    add("某项缺 id", { params: [{ range: "0…1", default: 0 }] }, dict, ["golden-shape"]);
    add("id 重名", { params: [param(), param()] }, dict, ["golden-unique"]);
    add("feedback 缺 default", { params: [param({ default: undefined })] }, dict, ["golden-default"]);
    add("feedback 缺 range", { params: [param({ range: undefined })] }, dict, ["golden-range"]);
    add("词条表里没有 feedback 的显示名", { params: [param()] }, { zh: {}, en: {}, fr: {} }, ["i18n-missing"]);
    // 判据 render-refuses：真源不成形时 render 抛错，不出占位表格
    const threw = (g, d) => {
        try {
            paramTableOf(g, d, "en");
            return false;
        } catch {
            return true;
        }
    };
    cases.push({
        label: "真源缺字段时 render 抛错",
        bad: true,
        passed: threw({ params: [param({ default: undefined })] }, dict),
    });
    cases.push({
        label: "真源齐备时 render 不抛错",
        bad: false,
        passed: !threw({ params: [param()] }, dict),
    });
    // 正例：显示名取自词条表；映射形与数组形取到同一个 default
    const table = renderParamTable(paramsOf({ params: [param()] }), dict, "en");
    const mapped = renderParamTable(paramsOf({ feedback: { range: "0…1", default: 0 } }), dict, "en");
    cases.push({
        label: "表格用词条表里的显示名",
        bad: false,
        passed: table.includes("Feedback") && !table.includes("undefined"),
    });
    cases.push({
        label: "两种 golden 形状渲染出的表格逐字相同",
        bad: false,
        passed: table === mapped,
    });
    const missing = paramsProblems({ params: [param({ default: undefined })] }, dict);
    cases.push({
        label: "缺 default 的报错点名参数",
        bad: false,
        passed: missing.some((m) => m.includes("feedback") && m.includes("default")),
    });
    const seen = new Set();
    for (const [g, d] of [
        [{ params: [param()] }, dict],
        [{ feedback: { range: "0…1", default: 0 } }, dict],
        [{}, dict],
        [{ params: [] }, dict],
        [{ params: [{ range: "0…1", default: 0 }] }, dict],
        [{ params: [param(), param()] }, dict],
        [{ params: [param({ default: undefined })] }, dict],
        [{ params: [param({ range: undefined })] }, dict],
        [{ params: [param()] }, { zh: {}, en: {}, fr: {} }],
    ])
        for (const p of paramsProblems(g, d)) seen.add(ruleOf(p));
    seen.add("render-refuses");
    cases.push({
        label: "每条判据都至少被一个坏样例点名",
        bad: false,
        passed: Object.keys(RULES).every((r) => seen.has(r)),
    });
    return caseResult(cases);
}

export default {
    name: "params-table",
    sources: [GOLDEN_FILE, I18N_FILE],
    surfaces: [
        { file: "docs/contracts/PARAMETERS.md", id: "params", required: false },
        { file: "docs/USER_GUIDE.md", id: "en", required: false },
        { file: "docs/USER_GUIDE.zh-CN.md", id: "zh", required: false },
    ],
    validate(ctx) {
        const golden = loadGolden(ctx.root);
        if (golden === null) return []; // 真源缺失：跳过，不报问题
        const dict = loadI18n(ctx.root);
        return paramsProblems(golden, dict); // dict 为 null 时只查 golden 自己的字段
    },
    render(ctx, id) {
        const golden = loadGolden(ctx.root);
        if (golden === null) throw new Error(`缺 ${GOLDEN_FILE}`);
        const dict = loadI18n(ctx.root);
        if (dict === null) throw new Error(`缺 ${I18N_FILE}`);
        return paramTableOf(golden, dict, ID_LANG[id] ?? "en");
    },
    selfTest,
};
