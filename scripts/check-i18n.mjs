// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/web.json
//
// 界面词条门禁：web/i18n/*.js 里各页面的词条表合并后逐项检查。
//   node scripts/check-i18n.mjs [--strict-unused] [--dump-json] [--self-test]
//
// 判据：
//   1. 三语键集合全等，值都是字符串（扁平表，不允许嵌套）；
//   2. 每个键的占位符集合在三语里相同（{name} 一类）；
//   3. 标点：zh 的汉字旁不得出现半角 , : ; ? !；fr 的 ; : ? ! % 之前、« » 的内侧必须是 U+00A0 或 U+202F，
//      不得是普通空格或没有空格；en 与 fr 不得用直引号和直撇号；
//   4. 禁用字形：emoji 与 ✓ ⚠ ✕ ⇕ ① ⓘ ♩ ‹ ›（界面里画成 SVG）；
//   5. en 与 fr 的值不得含汉字，只有 CJK_ALLOWED 白名单里的键例外；
//   6. guide.ruleN.lead 三语都非空；
//   7. 页面 HTML 里的 data-t / data-t-aria 键必须在词条表里；
//   8. --strict-unused：词条表里的键必须被 web/ 下的 html 与 js 引用（i18n 目录本身不算）。
//      引用可以是完整的键字面量，也可以是以「.」结尾的前缀字面量（"common.langStart." + code）。
//   --dump-json 把合并后的词条以 { key: { zh, en, fr } } 打印到 stdout，供 check-text-fit.py 读取。

import fs from "node:fs";
import path from "node:path";
import { pathToFileURL } from "node:url";
import { ROOT, assertNodeVersion, caseResult, isMain, j, readText } from "./lib/check-kit.mjs";

export const LANGS = ["zh", "en", "fr"];

/**
 * 允许在 en 或 fr 里出现汉字的键：语言选择卡的标题与「中文」按钮、语言切换里的「中」、关于面板的读音说明。
 * 这些是有意要让所有语言的用户都认得出来的文字。删掉任何一项，对应的词条就会变红。
 */
export const CJK_ALLOWED = new Set(["about.origin", "common.lang.zh", "common.langStart.title", "common.langStart.zh"]);

const CJK = new RegExp("[\\u2e80-\\u9fff\\uf900-\\ufaff\\uff00-\\uffef]", "u");
const HAN = "\\u3400-\\u9fff";
// 不换行空格 U+00A0 与窄不换行空格 U+202F
const NBSPS = String.fromCodePoint(0x00a0, 0x202f);
// 禁用字形：emoji 区段、杂项符号与装饰符号区、带圈字符、单书名号、半圆音符、双向箭头（界面里一律画成 SVG）
const FORBIDDEN = new RegExp(
    "[\\u{1F000}-\\u{1FAFF}\\u2460-\\u24FF\\u2600-\\u27BF\\u2039\\u203A\\u2669-\\u266F\\u21D5]",
    "u",
);

const placeholders = (s) => [...String(s).matchAll(/\{(\w+)\}/g)].map((m) => m[1]).sort();

/** 一条值的排版问题，返回 [描述]。lang 是 zh、en 或 fr。 */
export function typographyProblems(lang, value) {
    const out = [];
    const v = String(value);
    for (const ch of v) {
        if (FORBIDDEN.test(ch)) {
            out.push(`禁用字形 U+${ch.codePointAt(0).toString(16).toUpperCase().padStart(4, "0")}`);
        }
    }
    if (lang === "zh") {
        if (new RegExp(`[${HAN}][,:;?!]|[,:;?!][${HAN}]`, "u").test(v)) out.push("汉字旁出现半角标点，应为全角");
    }
    if (lang === "en" || lang === "fr") {
        if (/['"]/.test(v)) out.push("直引号或直撇号，应为弯引号 “ ” 和弯撇号 ’");
    }
    if (lang === "fr") {
        for (let i = 1; i < v.length; i++) {
            const mark = v[i];
            if (!";:?!%".includes(mark)) continue;
            const prev = v[i - 1];
            const next = v[i + 1];
            // 冒号后面紧跟非空白字符时是时间、地址一类的用法，不是标点
            if (mark === ":" && next !== undefined && !/\s/.test(next)) continue;
            // 百分号只在跟着数字或占位符时才是数值的单位
            if (mark === "%" && !/[\d}\s]/.test(prev)) continue;
            if (prev === "(" || prev === "{") continue;
            if (!NBSPS.includes(prev)) out.push(`法语 ${mark} 之前应为 U+00A0 或 U+202F`);
        }
        for (const m of v.matchAll(/«(\s?)/gu)) {
            if (!NBSPS.includes(m[1])) out.push("法语 « 之后应为 U+00A0 或 U+202F");
        }
        for (const m of v.matchAll(/(\s?)»/gu)) {
            if (!NBSPS.includes(m[1])) out.push("法语 » 之前应为 U+00A0 或 U+202F");
        }
    }
    return [...new Set(out)];
}

/** 对合并后的词条表做全部静态判据，返回问题字符串数组（不含引用扫描）。 */
export function lintDict(T, { allowed = CJK_ALLOWED } = {}) {
    const problems = [];
    for (const lang of LANGS) {
        if (!T || typeof T[lang] !== "object" || T[lang] === null) {
            return [`缺少语言 ${lang}`];
        }
    }
    const keys = new Set(LANGS.flatMap((l) => Object.keys(T[l])));
    for (const key of [...keys].sort()) {
        const have = LANGS.filter((l) => Object.prototype.hasOwnProperty.call(T[l], key));
        if (have.length !== LANGS.length) {
            problems.push(`${key}：只在 ${have.join("、")} 里有，三语键集合必须全等`);
            continue;
        }
        for (const lang of LANGS) {
            if (typeof T[lang][key] !== "string") problems.push(`${key}（${lang}）：值必须是字符串`);
        }
        if (LANGS.some((l) => typeof T[l][key] !== "string")) continue;
        const ph = LANGS.map((l) => placeholders(T[l][key]).join(","));
        if (new Set(ph).size !== 1)
            problems.push(
                `${key}：三语的占位符不一致（zh ${ph[0] || "无"}；en ${ph[1] || "无"}；fr ${ph[2] || "无"}）`,
            );
        for (const lang of LANGS) {
            for (const p of typographyProblems(lang, T[lang][key])) problems.push(`${key}（${lang}）：${p}`);
        }
        for (const lang of ["en", "fr"]) {
            if (CJK.test(T[lang][key]) && !allowed.has(key)) problems.push(`${key}（${lang}）：值含汉字，不在白名单内`);
        }
        if (/^guide\.rule\d+\.lead$/.test(key)) {
            for (const lang of LANGS) {
                if (!T[lang][key].trim()) problems.push(`${key}（${lang}）：不得为空`);
            }
        }
    }
    return problems;
}

/** 词条文件目录里的各页词条表，合并后返回；键重名会抛错。 */
export async function loadDict(root = ROOT) {
    const dir = path.join(root, "web", "i18n");
    const files = fs.existsSync(dir)
        ? fs
              .readdirSync(dir)
              .filter((f) => f.endsWith(".js"))
              .sort()
        : [];
    const parts = [];
    for (const f of files) {
        const mod = await import(pathToFileURL(path.join(dir, f)).href);
        if (!mod.default || typeof mod.default !== "object")
            throw new Error(`web/i18n/${f} 没有默认导出 { zh, en, fr }`);
        parts.push(mod.default);
    }
    const { mergeDicts } = await import(pathToFileURL(path.join(root, "web", "shared", "i18n.js")).href);
    return { files, T: mergeDicts(...parts) };
}

function walk(dir, acc, skip) {
    for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
        const p = path.join(dir, e.name);
        const rel = path.relative(ROOT, p).replace(/\\/g, "/");
        if (skip.some((s) => rel === s || rel.startsWith(s + "/"))) continue;
        if (e.isDirectory()) walk(p, acc, skip);
        else if (/\.(?:js|mjs|html)$/.test(e.name)) acc.push(p);
    }
    return acc;
}

/** 扫 web/ 下页面代码对词条键的引用：返回 { exact: Set, prefixes: Set, dataT: Set }。 */
export function referencesIn(files) {
    const exact = new Set();
    const prefixes = new Set();
    const dataT = new Set();
    for (const f of files) {
        const text = readText(f);
        if (text === null) continue;
        for (const m of text.matchAll(/["'`]([A-Za-z][\w-]*(?:\.[\w-]+)*\.?)["'`]/g)) {
            (m[1].endsWith(".") ? prefixes : exact).add(m[1]);
        }
        if (f.endsWith(".html")) {
            for (const m of text.matchAll(/\bdata-t(?:-[a-z]+)?\s*=\s*"([^"]+)"/g)) dataT.add(m[1]);
        }
    }
    return { exact, prefixes, dataT };
}

/** 引用相关的问题：data-t 指向不存在的键；strict 时再加上没人引用的键。 */
export function referenceProblems(T, refs, { strictUnused }) {
    const problems = [];
    const keys = Object.keys(T.zh);
    for (const k of refs.dataT) {
        if (!Object.prototype.hasOwnProperty.call(T.zh, k)) problems.push(`HTML 的 data-t 引用了不存在的键 ${k}`);
    }
    if (strictUnused) {
        for (const k of keys) {
            const used = refs.exact.has(k) || refs.dataT.has(k) || [...refs.prefixes].some((p) => k.startsWith(p));
            if (!used) problems.push(`${k}：没有任何页面代码引用（死键）`);
        }
    }
    return problems;
}

export async function checkTree(root = ROOT, { strictUnused = true } = {}) {
    const { files, T } = await loadDict(root);
    const problems = lintDict(T);
    const web = path.join(root, "web");
    const pages = walk(web, [], ["web/i18n", "web/shared/i18n.js", "web/js/juce", "web/fonts"]);
    problems.push(...referenceProblems(T, referencesIn(pages), { strictUnused }));
    return { files, keys: Object.keys(T.zh).length, problems };
}

// SCVB 的一条旧词条原样（普通空格、没有不换行空格），自测里它必须变红
const LEGACY_LOCKED = j("WebKit antérieur à 26.4 : zoom fixé à 100", " %");

export async function selfTest() {
    const base = {
        zh: { "a.x": "缩放已应用，{s} 秒后回退", "a.y": "取消" },
        en: { "a.x": "Scale applied, reverting in {s} s", "a.y": "Cancel" },
        fr: { "a.x": "Zoom appliqué, retour dans {s}\u00a0s", "a.y": "Annuler" },
    };
    const clone = () => JSON.parse(JSON.stringify(base));
    const withKey = (lang, key, value) => {
        const t = clone();
        t[lang][key] = value;
        return t;
    };
    const cases = [];
    const add = (label, bad, T, opts) => {
        const p = lintDict(T, opts);
        cases.push({ label, bad, passed: bad ? p.length > 0 : p.length === 0 });
    };
    add("干净的词条表", false, clone());
    {
        const t = clone();
        delete t.fr["a.y"];
        add("fr 缺一个键", true, t);
    }
    add("占位符不一致", true, withKey("en", "a.x", "Scale applied"));
    add("zh 汉字旁半角冒号", true, withKey("zh", "a.y", "取消:"));
    add("fr 冒号前普通空格（旧词条原样）", true, withKey("fr", "a.y", LEGACY_LOCKED));
    add("fr 冒号前没有空格", true, withKey("fr", "a.y", "Note: ok mais:"));
    add("fr 冒号前 U+202F", false, withKey("fr", "a.y", "Note\u202f: ok"));
    add("fr 直撇号", true, withKey("fr", "a.y", "l'écran"));
    add("en 直引号", true, withKey("en", "a.y", 'say "hi"'));
    add("禁用字形", true, withKey("en", "a.y", "Done ✓"));
    add("en 含汉字（新增 CJK）", true, withKey("en", "a.y", "取消 Cancel"));
    add("en 含汉字但在白名单", false, withKey("en", "a.y", "取消 Cancel"), { allowed: new Set(["a.y"]) });
    add("删掉白名单项后同一条词条变红", true, withKey("en", "a.y", "取消 Cancel"), { allowed: new Set() });
    add(
        "guide 条目 lead 为空",
        true,
        (() => {
            const t = clone();
            for (const l of LANGS) t[l]["guide.rule1.lead"] = l === "fr" ? "" : "x";
            return t;
        })(),
    );
    {
        const T = clone();
        const refs = { exact: new Set(["a.x"]), prefixes: new Set(), dataT: new Set(["a.nope"]) };
        const p = referenceProblems(T, refs, { strictUnused: true });
        cases.push({ label: "死键与 data-t 指向缺失键", bad: true, passed: p.length === 2 });
        const ok = referenceProblems(
            T,
            { exact: new Set(["a.x"]), prefixes: new Set(["a."]), dataT: new Set() },
            { strictUnused: true },
        );
        cases.push({ label: "前缀字面量算引用", bad: false, passed: ok.length === 0 });
    }
    {
        // 真实仓：删掉白名单里的 common.langStart.zh 后，现有词条必须变红
        const { T } = await loadDict(ROOT);
        const live = lintDict(T);
        cases.push({ label: "仓内词条表干净", bad: false, passed: live.length === 0 });
        const cut = new Set(CJK_ALLOWED);
        cut.delete("common.langStart.zh");
        cases.push({ label: "仓内词条：删白名单项变红", bad: true, passed: lintDict(T, { allowed: cut }).length > 0 });
    }
    return caseResult(cases);
}

async function main(argv) {
    assertNodeVersion();
    if (argv.includes("--self-test")) {
        const r = await selfTest();
        for (const l of r.lines) console.error(l);
        console.log(`check-i18n --self-test：${r.ok ? "通过" : "失败"}（坏样例 ${r.bad}、正例 ${r.good}）`);
        return r.ok ? 0 : 1;
    }
    if (argv.includes("--dump-json")) {
        const { T } = await loadDict(ROOT);
        const out = {};
        for (const k of Object.keys(T.zh)) out[k] = { zh: T.zh[k], en: T.en[k], fr: T.fr[k] };
        process.stdout.write(JSON.stringify(out));
        return 0;
    }
    const r = await checkTree(ROOT, { strictUnused: argv.includes("--strict-unused") });
    if (!r.problems.length) {
        console.log(`check-i18n 通过：${r.files.length} 个词条文件，${r.keys} 个键`);
        return 0;
    }
    console.error(`check-i18n 失败：${r.problems.length} 项`);
    for (const p of r.problems) console.error(`  ${p}`);
    return 1;
}

if (isMain(import.meta.url)) process.exitCode = await main(process.argv.slice(2));
