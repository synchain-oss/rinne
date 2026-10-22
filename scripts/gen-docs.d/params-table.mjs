// SPDX-License-Identifier: GPL-3.0-or-later
//
// 参数表：真源是 tests/golden/params-input.json（参数的范围、默认值、单位、是否可自动化）与
// web/i18n/input.js（界面上的显示名）。两张真源今天都还不在仓里，所以本模块走跳过路径。
// 字段名（range / min / max / unit / automatable / i18nKey）是按这张表需要的信息写的；真正的 golden 落地时，
// 以它的实际形状为准重新对一遍。
//
// 缺字段时的口径：
//   - golden 文件不存在 → 跳过：validate 返回空数组，render 不被调用；
//   - golden 存在但缺字段（缺参数集合 / 某参数缺 id / default / range / unit / automatable）→ 明确报错，指名字段与参数，
//     例如「tests/golden/params-input.json 的 feedback 缺 default」；绝不生成占位值，也不拿参数 ID 顶替显示名；
//     unit 可以是空串（无单位）但必须写出来，automatable 必须是布尔值；
//   - 显示名缺某个参数 → 同样报错：「web/i18n/input.js 缺参数 feedback 的显示名」；
//   - 显示名词条文件不存在：只有在某个落地面已经接入（文件里放了标记）时才报，因为那时 render 会被调用、会抛错，
//     validate 与 render 对「词条文件缺失」的反应要一致；没有落地面接入时不拦 golden 先落地。
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
import { findRegion, readRepoText } from "../lib/docs-kit.mjs";
import { caseResult } from "../lib/check-kit.mjs";

export const GOLDEN_FILE = "tests/golden/params-input.json";
export const I18N_FILE = "web/i18n/input.js";

/** 表头。id 决定语言：en / zh 各一份，其余 id（契约文档）用中文。 */
const HEADERS = {
    en: ["ID", "Display name", "Range", "Default", "Unit", "Automatable"],
    zh: ["ID", "显示名", "范围", "默认值", "单位", "可自动化"],
};
/** 「可自动化」列的是 / 否，随语言走。 */
const YES_NO = { en: ["Yes", "No"], zh: ["是", "否"] };
/** id → 语言。契约文档正文用中文（CLAUDE.md 的「文档、上游与第三方」一节）。 */
const ID_LANG = { en: "en", zh: "zh", params: "zh" };

/** 判据名 → 说明。 */
export const RULES = {
    "golden-shape": "golden 里有非空的参数集合，每项都有 id",
    "golden-unique": "参数 id 不重名",
    "golden-default": "每个参数都有 default",
    "golden-range": "每个参数都有非空的范围",
    "golden-unit": "每个参数都写出 unit（字符串，可为空串）",
    "golden-automatable": "每个参数的 automatable 是布尔值",
    "i18n-missing": "显示名词条文件在，但缺某个参数的显示名；或有落地面接入而词条文件不在",
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

/** 表格单元：竖线转义（否则列数会算错），换行换成 <br>（否则表格会被撕开）。 */
const cell = (v) =>
    String(v ?? "")
        .replace(/\|/g, "\\|")
        .replace(/\r?\n/g, "<br>");

/** 默认值的写法：字符串加引号（空串与缺值要分得开），其余按字面量。 */
export const defaultText = (v) => (typeof v === "string" ? JSON.stringify(v) : String(v));

/** 范围的写法：range 可以是字符串、数组或对象；否则用 min 与 max 拼。空串、空数组都算没写，返回 null。 */
export function rangeText(param) {
    const r = param.range;
    if (typeof r === "string" && r.trim()) return r;
    if (Array.isArray(r) && r.length) return r.join("…");
    // undefined 与 null 都算没写：null 不能被渲染成「null…null」
    const written = (v) => v !== undefined && v !== null;
    if (r && typeof r === "object" && !Array.isArray(r) && written(r.min) && written(r.max)) return `${r.min}…${r.max}`;
    if (written(param.min) && written(param.max)) return `${param.min}…${param.max}`;
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
        if (typeof p.unit !== "string") out.push(`[golden-unit] ${where} 缺 unit（无单位写空串）`);
        if (typeof p.automatable !== "boolean") out.push(`[golden-automatable] ${where} 的 automatable 不是布尔值`);
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

/** 渲染出的表格。params 是 golden 的参数数组（已通过 paramsProblems）；dict 是词条表；lang 是 "en" 或 "zh"。 */
export function renderParamTable(params, dict, lang) {
    const head = HEADERS[lang];
    if (!head) throw new Error(`未知的语言 ${lang}`);
    const rows = params.map((p) => {
        const key = nameKeyOf(p, dict);
        const name = key ? dict?.[lang]?.[key] : undefined;
        if (typeof name !== "string") throw new Error(`缺参数 ${p.id} 的 ${lang} 显示名`);
        return [
            cell(p.id),
            cell(name),
            cell(rangeText(p)),
            cell(defaultText(p.default)),
            cell(p.unit),
            cell(YES_NO[lang][p.automatable ? 0 : 1]),
        ];
    });
    const sep = head.map(() => "---");
    return [head, sep, ...rows].map((r) => `| ${r.join(" | ")} |`).join("\n");
}

/** 参数表；真源不成形时抛错。golden 是 params-input.json 的内容，dict 是词条表，lang 是 "en" 或 "zh"。 */
export function paramTableOf(golden, dict, lang) {
    if (!dict) throw new Error(`缺 ${I18N_FILE}`);
    const problems = paramsProblems(golden, dict);
    if (problems.length) throw new Error(problems.join("；"));
    return renderParamTable(paramsOf(golden), dict, lang);
}

/** 问题串里的判据名。 */
export const ruleOf = (problem) => /^\[([^\]]+)\]/.exec(problem)?.[1] ?? "";

/**
 * 自测用例：每个用例 { label, bad, passed, expect }。期望为空表示正例（一条问题都不许有），
 * 否则是坏样例（期望的判据都要被点名）。末尾的覆盖用例核对每条判据都被某个通过的坏样例点名。
 */
export function selfTestCases() {
    const param = (over = {}) => ({
        id: "feedback",
        range: "0…1",
        default: 0,
        unit: "%",
        automatable: true,
        ...over,
    });
    const dict = { zh: { feedback: "反馈" }, en: { feedback: "Feedback" }, fr: { feedback: "Retour" } };
    const cases = [];
    const add = (label, golden, d, expect) => {
        const rules = paramsProblems(golden, d).map(ruleOf).filter(Boolean);
        const bad = expect.length > 0;
        cases.push({ label, bad, passed: bad ? expect.every((r) => rules.includes(r)) : rules.length === 0, expect });
    };
    const threw = (g, d, lang = "en") => {
        try {
            paramTableOf(g, d, lang);
            return false;
        } catch {
            return true;
        }
    };
    add("齐的参数表没问题（数组形）", { params: [param()] }, dict, []);
    add(
        "齐的参数表没问题（映射形）",
        { feedback: { range: "0…1", default: 0, unit: "", automatable: false } },
        dict,
        [],
    );
    add("缺参数集合", {}, dict, ["golden-shape"]);
    add("参数集合是空的", { params: [] }, dict, ["golden-shape"]);
    add("某项缺 id", { params: [{ range: "0…1", default: 0, unit: "", automatable: true }] }, dict, ["golden-shape"]);
    add("参数集合里有不是对象的项", { params: [param(), null] }, dict, ["golden-shape"]);
    add("id 重名", { params: [param(), param()] }, dict, ["golden-unique"]);
    add("feedback 缺 default", { params: [param({ default: undefined })] }, dict, ["golden-default"]);
    add("feedback 缺 range", { params: [param({ range: undefined })] }, dict, ["golden-range"]);
    add("range 是空串", { params: [param({ range: "" })] }, dict, ["golden-range"]);
    add("min 与 max 写成 null 等于没写 range", { params: [param({ range: undefined, min: null, max: null })] }, dict, [
        "golden-range",
    ]);
    add(
        "range 对象里的 min 与 max 写成 null 等于没写",
        { params: [param({ range: { min: null, max: null } })] },
        dict,
        ["golden-range"],
    );
    add("range 对象里的 min 与 max 是数字就是写了 range", { params: [param({ range: { min: 0, max: 1 } })] }, dict, []);
    add("min 与 max 是数字就是写了 range", { params: [param({ range: undefined, min: 0, max: 1 })] }, dict, []);
    cases.push({
        label: "min 与 max 渲染成「0…1」，不出现 null",
        bad: false,
        passed:
            paramTableOf({ params: [param({ range: undefined, min: 0, max: 1 })] }, dict, "en").includes("0…1") &&
            !paramTableOf({ params: [param({ range: undefined, min: 0, max: 1 })] }, dict, "en").includes("null"),
        expect: [],
    });
    add("feedback 缺 unit", { params: [param({ unit: undefined })] }, dict, ["golden-unit"]);
    add("unit 不是字符串", { params: [param({ unit: 5 })] }, dict, ["golden-unit"]);
    add("feedback 缺 automatable", { params: [param({ automatable: undefined })] }, dict, ["golden-automatable"]);
    add("automatable 写成字符串", { params: [param({ automatable: "yes" })] }, dict, ["golden-automatable"]);
    add("词条表里没有 feedback 的显示名", { params: [param()] }, { zh: {}, en: {}, fr: {} }, ["i18n-missing"]);
    // 判据 render-refuses：真源不成形时 render 抛错，不出占位表格
    cases.push({
        label: "真源缺字段时 render 抛错",
        bad: true,
        passed: threw({ params: [param({ default: undefined })] }, dict),
        expect: ["render-refuses"],
    });
    cases.push({
        label: "没有词条表时 render 抛错",
        bad: true,
        passed: threw({ params: [param()] }, null),
        expect: [],
    });
    cases.push({
        label: "真源齐备时 render 不抛错",
        bad: false,
        passed: !threw({ params: [param()] }, dict),
        expect: [],
    });
    // 正例：显示名取自词条表；映射形与数组形取到同一个 default；是 / 否随语言走
    const table = paramTableOf({ params: [param()] }, dict, "en");
    const mapped = paramTableOf({ feedback: { range: "0…1", default: 0, unit: "%", automatable: true } }, dict, "en");
    const zh = paramTableOf({ params: [param({ automatable: false })] }, dict, "zh");
    cases.push({
        label: "表格用词条表里的显示名",
        bad: false,
        passed: table.includes("Feedback") && !table.includes("undefined"),
        expect: [],
    });
    cases.push({ label: "两种 golden 形状渲染出的表格逐字相同", bad: false, passed: table === mapped, expect: [] });
    cases.push({
        label: "是 / 否随语言走：英文表写 Yes，中文表写 否",
        bad: false,
        passed: table.includes("| Yes |") && !table.includes("是") && zh.includes("| 否 |") && !zh.includes("No"),
        expect: [],
    });
    cases.push({
        label: "缺 default 的报错点名参数",
        bad: false,
        passed: paramsProblems({ params: [param({ default: undefined })] }, dict).some(
            (m) => m.includes("feedback") && m.includes("default"),
        ),
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

/** 本模块的某个落地面是否已经接入（文件在、放了标记）。 */
function anySurfaceAdopted(root, surfaces) {
    return surfaces.some((s) => {
        const text = readRepoText(root, s.file);
        try {
            return text !== null && findRegion(text, `gen:params-table:${s.id}`) !== null;
        } catch {
            return true; // 标记坏了也算接入，让引擎去报
        }
    });
}

const SURFACES = [
    { file: "docs/contracts/PARAMETERS.md", id: "params", required: false },
    { file: "docs/USER_GUIDE.md", id: "en", required: false },
    { file: "docs/USER_GUIDE.zh-CN.md", id: "zh", required: false },
];

export default {
    name: "params-table",
    sources: [GOLDEN_FILE, I18N_FILE],
    surfaces: SURFACES,
    validate(ctx) {
        const golden = loadGolden(ctx.root);
        if (golden === null) return []; // 真源缺失：跳过，不报问题
        const dict = loadI18n(ctx.root);
        if (dict === null && anySurfaceAdopted(ctx.root, SURFACES))
            return [`[i18n-missing] 参数表的落地面已接入，但缺 ${I18N_FILE}`, ...paramsProblems(golden, null)];
        return paramsProblems(golden, dict); // dict 为 null 时只查 golden 自己的字段
    },
    render(ctx, id) {
        const golden = loadGolden(ctx.root);
        if (golden === null) throw new Error(`缺 ${GOLDEN_FILE}`);
        const dict = loadI18n(ctx.root);
        if (dict === null) throw new Error(`缺 ${I18N_FILE}`);
        const lang = ID_LANG[id];
        if (!lang) throw new Error(`未知的落地面 id ${id}`);
        return paramTableOf(golden, dict, lang);
    },
    selfTest,
};
