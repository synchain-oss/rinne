// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/web.json
//
// 界面词条的 API：语言表、合并、占位符填充和 DOM 刷新。本文件不含任何词条。
//
// 词条按页面拆在 web/i18n/ 下：common.js（两个插件共用）、input.js、output.js、guide.js。
// 每个文件默认导出 { zh, en, fr }，三个语言各是一张扁平的 key 到字符串的表；
// 页面入口把自己要用的几份交给 installDicts() 合并后使用。
// 合并时键重名直接抛错：一个键只能有一个归属文件。
//
// 模块顶层没有副作用，也不触碰 window 或 document，检查脚本可以在 Node 里直接 import。

/** 支持的语言。用白名单而不是 T[lang] 的真值判断：后者会把 "constructor" 之类的原型链属性当成合法语言。 */
export const LANGS = ["zh", "en", "fr"];

/** 归一化语言码：不在白名单里一律回落到中文。 */
function normLang(lang) {
    return LANGS.includes(lang) ? lang : "zh";
}

/**
 * 合并若干份词条表，返回新的 { zh, en, fr }。
 * 每份都必须带齐三个语言；同一个键出现在两份里就抛错，错误信息带上键和语言。
 */
export function mergeDicts(...parts) {
    const merged = { zh: {}, en: {}, fr: {} };
    for (const part of parts) {
        for (const lang of LANGS) {
            const table = part && part[lang];
            if (!table || typeof table !== "object") {
                throw new TypeError(`词条表缺少语言 ${lang}`);
            }
            for (const key of Object.keys(table)) {
                if (Object.prototype.hasOwnProperty.call(merged[lang], key)) {
                    throw new Error(`词条键重名：${key}（${lang}）`);
                }
                merged[lang][key] = table[key];
            }
        }
    }
    return merged;
}

/** 当前页面生效的词条。页面入口调用 installDicts() 之前是空表。 */
let T = { zh: {}, en: {}, fr: {} };

/** 合并并安装本页要用的词条表，返回合并结果。 */
export function installDicts(...parts) {
    T = mergeDicts(...parts);
    return T;
}

/** 取某语言的词条表，未知语言回落到中文。 */
export function dict(lang) {
    return T[normLang(lang)];
}

/**
 * 占位符填充：{n}、{X}、{name} 一律按键名取，取不到就原样留着。
 *
 * 原样留着而不是填空串：词条里留下一个 {x}，是「调用方少传了一个值」的可见证据；
 * 填空串会让它变成一句读起来通顺、内容却缺了一块的话，没人会注意到。
 */
export function format(text, vals) {
    const v = vals || {};
    return String(text).replace(/\{(\w+)\}/g, (m, k) =>
        Object.prototype.hasOwnProperty.call(v, k) ? String(v[k]) : m,
    );
}

/**
 * 把 root 里所有 data-t="key" 元素的文本换成当前语言，
 * 并把 data-t-aria="key" 元素的 aria-label 一并同步；同时同步 documentElement.lang。
 * 只写 textContent 和 aria-label，不写任何 HTML。返回该语言的词条表，供动态文案渲染。
 * 占位符替换由调用方自己做，本模块只发词条表，不做模板求值。
 */
export function applyI18n(root, lang) {
    const t = dict(lang);
    // 与 normLang 同口径的 own-property 判定：data-t="toString" 一类原型链键不得命中
    const has = (k) => Object.prototype.hasOwnProperty.call(t, k);
    root.querySelectorAll("[data-t]").forEach((el) => {
        const key = el.getAttribute("data-t");
        if (has(key)) el.textContent = t[key];
    });
    root.querySelectorAll("[data-t-aria]").forEach((el) => {
        const key = el.getAttribute("data-t-aria");
        if (has(key)) el.setAttribute("aria-label", t[key]);
    });
    if (typeof document !== "undefined" && document.documentElement) {
        document.documentElement.lang = normLang(lang);
    }
    return t;
}
