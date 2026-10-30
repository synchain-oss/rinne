// SPDX-License-Identifier: GPL-3.0-or-later
//
// 设计稿回收的机检：用无头浏览器逐场景打开设计稿（.dc.html），对着仓内真源检查，并输出缺失清单与 DELTA 草稿。
// 判据：
//   场景    清单里的场景 ID 集合、窗口、语言与场景表相同，elements 覆盖 mustShow（与 check-scenes 同一套）；
//   覆盖    每个场景里 mustShow 的元件都画到了：按锚点表找 data-gb（根锚点或其子锚点），整窗场景的元件根节点带 data-state；
//   文案    每个带 data-t 的节点（含 data-t-title、data-t-aria）的文字与 web/i18n 的词条逐字一致（zh、en；fr 只查非空），
//           占位符用 data-t-vals 代入；词条文件还没入库的前缀只记「待补」，不判红；带 data-errata 的节点只记不判红；
//   颜色    场景里所有元素的计算样式里出现的颜色（文字、背景、边框、描边、阴影、渐变）只来自 web/shared/tokens.css 的色值；
//           宿主给的轨道颜色（data-mock="track.color"）是数据，不在此列；
//   安全红字 红色语义（data-tone="red"）的文字与紧急切断（data-safety="panic"）不降级：不透明度链乘积 ≥ .99、颜色 alpha ≥ .99、
//           可见、不被截断、字号 ≥ 9 px；整窗场景凡 mustShow 里有 I-17 的必须画出紧急切断；
//   外链    不引用任何 http(s) 地址（带 data-preview-only 的预览字体块除外），样式里也不能有 url(http…) 与 @import。
// 输出：逐类问题清单（A 缺失、B 偏离、D 稿内缺陷的草稿分类供用户一次裁定）和一份按 DELTA 固定五节写的草稿。
// 设计稿的位置、浏览器缺失时的处理与 check-scenes 相同：设计稿没入库就跳过。
//   node scripts/check-design-manifest.mjs [--design <文件>] [--scene <id>]... [--delta <名字>.md|.json] [--self-test]
// 退出码：0 没有判红的问题；1 有；2 整个检查跳过（没有设计稿或没有浏览器）。

import fs from "node:fs";
import path from "node:path";
import { pathToFileURL } from "node:url";
import { ROOT, assertNodeVersion, caseResult, isMain } from "./lib/check-kit.mjs";
import { compareManifest, locateDesign, readManifest } from "./check-scenes.mjs";
import { resolveOutFile } from "../web-preview/tests/lib/safe-out.mjs";

// ---------------------------------------------------------------- 浏览器里跑的采集脚本

/**
 * 一个带 data-t* 的节点产出的文案记录。占位符的代入值：文字读 data-t-vals，title 读 data-t-title-vals，
 * aria 读 data-t-aria-vals，后两个缺省时退回 data-t-vals。
 * 这个函数的源码会原样嵌进下面浏览器里跑的采集脚本，自测在 Node 里用假节点跑的是同一份，所以它不能引用函数外的任何变量。
 */
export function textRecordsOf(el) {
    const valsOf = (name) => {
        const attr = el.hasAttribute(name) ? name : "data-t-vals";
        if (!el.hasAttribute(attr)) return null;
        try {
            return JSON.parse(el.getAttribute(attr));
        } catch {
            return "bad";
        }
    };
    const errata = !!el.closest("[data-errata]");
    // 目录里的多语言对照格外层带 data-lang，按该语言列核对；没有就按场景语言。
    // 只看祖先，不看节点自己：节点自带 data-lang 的（比如语言胶囊里各语言那一格）不改变核对语言
    const langHost = el.parentElement ? el.parentElement.closest("[data-lang]") : null;
    const lang = langHost ? langHost.getAttribute("data-lang") : null;
    const out = [];
    if (el.hasAttribute("data-t"))
        out.push({
            attr: "text",
            key: el.getAttribute("data-t"),
            vals: valsOf("data-t-vals"),
            errata,
            lang,
            text: el.textContent,
        });
    if (el.hasAttribute("data-t-title"))
        out.push({
            attr: "title",
            key: el.getAttribute("data-t-title"),
            vals: valsOf("data-t-title-vals"),
            errata,
            lang,
            text: el.getAttribute("title") || "",
        });
    if (el.hasAttribute("data-t-aria"))
        out.push({
            attr: "aria",
            key: el.getAttribute("data-t-aria"),
            vals: valsOf("data-t-aria-vals"),
            errata,
            lang,
            text: el.getAttribute("aria-label") || "",
        });
    return out;
}

/** 在页面里采集一个场景的快照。返回字符串形式的表达式，调用方 eval。 */
export const collectExpression = (id) => `(() => {
    const id = ${JSON.stringify(id)};
    if (window.rinneShowScene(id) !== true) return { shown: false };
    const root = document.querySelector('[data-scene-id="' + id + '"]');
    if (!root) return { shown: false };
    const num = (s) => Number.parseFloat(s) || 0;
    const colorRe = /rgba?\\([^)]*\\)/g;
    const colors = new Set();
    const addColors = (text) => { for (const m of String(text).matchAll(colorRe)) colors.add(m[0]); };
    const props = ["color", "backgroundColor", "borderTopColor", "borderRightColor", "borderBottomColor", "borderLeftColor",
        "outlineColor", "textDecorationColor", "caretColor", "fill", "stroke", "boxShadow", "backgroundImage", "textShadow"];
    const chainOpacity = (el) => { let o = 1; for (let n = el; n && n !== document.body; n = n.parentElement) o *= num(getComputedStyle(n).opacity); return o; };
    const alphaOf = (c) => { const m = /rgba?\\(([^)]*)\\)/.exec(c); if (!m) return 1; const p = m[1].split(/[ ,/]+/).filter(Boolean); return p.length > 3 ? num(p[3]) : 1; };
    const all = [root, ...root.querySelectorAll("*")];
    for (const el of all) { if (el.getAttribute("data-mock") === "track.color") continue; const cs = getComputedStyle(el); for (const p of props) addColors(cs[p]); }
    const gbs = [...root.querySelectorAll("[data-gb]")].map((e) => ({ gb: e.getAttribute("data-gb"), state: e.getAttribute("data-state") || "" }));
    const textRecordsOf = ${textRecordsOf.toString()};
    const texts = [];
    for (const el of root.querySelectorAll("[data-t],[data-t-title],[data-t-aria]")) texts.push(...textRecordsOf(el));
    const redNodes = [];
    for (const el of root.querySelectorAll('[data-tone="red"],[data-safety="panic"]')) {
        const targets = [el, ...el.querySelectorAll("[data-t]")].filter((e) => e === el || e.hasAttribute("data-t"));
        for (const t of targets) {
            const cs = getComputedStyle(t);
            if (!(t.textContent || "").trim()) continue;
            redNodes.push({
                gb: (el.closest("[data-gb]") || el).getAttribute("data-gb") || "", key: t.getAttribute("data-t") || "",
                safety: el.getAttribute("data-safety") === "panic", opacity: chainOpacity(t), alpha: alphaOf(cs.color),
                visible: cs.visibility === "visible" && cs.display !== "none", fontSize: num(cs.fontSize),
                clipped: t.scrollWidth > t.clientWidth + 1 || t.scrollHeight > t.clientHeight + 1,
            });
        }
    }
    const externals = [];
    for (const el of [root, ...root.querySelectorAll("*")]) {
        if (el.closest("[data-preview-only]")) continue;
        for (const a of ["src", "href", "action"]) { const v = el.getAttribute(a); if (v && /^(https?:)?[/][/]/i.test(v)) externals.push(v); }
        if (/url\\(\\s*["']?(https?:)?[/][/]/i.test(el.getAttribute("style") || "")) externals.push("style:" + el.getAttribute("style").slice(0, 60));
    }
    const rect = root.getBoundingClientRect();
    return { shown: rect.width > 0 && rect.height > 0, gbs, texts, redNodes, externals, colors: [...colors] };
})()`;

/** 文档级的外部引用（一次）：样式表里的 @import 与 url(http…)，以及运行期实际发出的资源请求。 */
export const documentExternalsExpression = `(() => {
    const styles = [];
    for (const sheet of document.styleSheets) {
        let rules = [];
        try { rules = [...sheet.cssRules]; } catch { continue; }
        for (const r of rules) { if (r.type === 3 || /url\\(\\s*["']?(https?:)?[/][/]/i.test(r.cssText)) styles.push(r.cssText.slice(0, 80)); }
    }
    const resources = performance.getEntriesByType("resource").map((e) => e.name).filter((u) => /^https?:/i.test(u));
    return { styles, resources };
})()`;

// ---------------------------------------------------------------- 纯分析

/** 颜色字面量（#rgb、#rrggbb、#rrggbbaa、rgb()、rgba()）归一成 "r,g,b"（不看 alpha）。取不到返回 null。 */
export function rgbKey(literal) {
    const s = String(literal).trim().toLowerCase();
    let m = /^#([0-9a-f]{3,8})$/.exec(s);
    if (m) {
        let h = m[1];
        if (h.length === 3 || h.length === 4) h = [...h].map((c) => c + c).join("");
        if (h.length !== 6 && h.length !== 8) return null;
        return [0, 2, 4].map((i) => parseInt(h.slice(i, i + 2), 16)).join(",");
    }
    m = /^rgba?\(([^)]*)\)$/.exec(s);
    if (m) {
        const p = m[1]
            .split(/[ ,/]+/)
            .filter(Boolean)
            .slice(0, 3)
            .map((x) => Math.round(Number.parseFloat(x)));
        return p.length === 3 && p.every(Number.isFinite) ? p.join(",") : null;
    }
    return null;
}

/** tokens.css 里出现的全部颜色，返回 Set("r,g,b")。 */
export function tokenColors(css) {
    const out = new Set();
    for (const m of css.replace(/\/\*[\s\S]*?\*\//g, "").matchAll(/#[0-9a-fA-F]{3,8}\b|rgba?\([^)]*\)/g)) {
        const k = rgbKey(m[0]);
        if (k) out.add(k);
    }
    return out;
}

/** 稿件源码里允许出现的网址：联系信息、项目主页和 SVG 命名空间。 */
const ALLOWED_SOURCE_URL =
    /^https?:\/\/(?:www\.w3\.org\/2000\/svg|www\.w3\.org\/1999\/xlink|synchain\.ca(?:\/|$)|github\.com\/synchain-oss\/rinne(?:\/|$))/;

/**
 * 文档级外链：稿件源码（.dc.html）里的网址，以及样式表里的外部引用，除放行名单和带 data-preview-only 的预览块外都判红。
 * 运行期实际发出的资源请求只记录：它们多半来自设计稿运行时（support.js）而不是稿件内容，由用户判断要不要管。
 */
export function documentFindings(source, doc) {
    const out = [];
    const body = source.replace(/<script type="application\/json" id="rinne-scenes">[\s\S]*?<\/script>/, "");
    for (const line of body.split("\n")) {
        if (line.includes("data-preview-only")) continue;
        for (const m of line.matchAll(/https?:\/\/[^\s"'`)<>\\]+/g)) {
            if (!ALLOWED_SOURCE_URL.test(m[0]))
                out.push({
                    kind: "外链",
                    severity: "red",
                    scene: null,
                    detail: `稿件源码里的外部地址：${m[0]}`,
                    element: null,
                });
        }
    }
    for (const r of doc.styles || [])
        out.push({ kind: "外链", severity: "red", scene: null, detail: `样式表里的外部引用：${r}`, element: null });
    for (const u of [...new Set(doc.resources || [])]) {
        out.push({
            kind: "外链",
            severity: "note",
            scene: null,
            detail: `运行期发出的外部请求：${u}（多半来自设计稿运行时，不是稿件内容）`,
            element: null,
        });
    }
    return out;
}

const norm = (s) =>
    String(s)
        .replace(/[ \t\r\n]+/g, " ")
        .trim();
const has = (o, k) => Object.prototype.hasOwnProperty.call(o, k);
const LANG_CODES = ["zh", "en", "fr"];

/** 词条文本代入占位符，与 web/shared/i18n.js 的 format 同口径。 */
const fill = (text, vals) =>
    String(text).replace(/\{(\w+)\}/g, (m, k) =>
        vals && typeof vals === "object" && has(vals, k) ? String(vals[k]) : m,
    );

/**
 * 分析一组快照，返回问题数组 [{ kind, severity: "red"|"note", scene, detail, element? }]。
 * @param {object} i
 * @param {Array} i.scenes 场景表
 * @param {object} i.anchors 元件编号表
 * @param {Array} i.manifestFindings 来自 compareManifest 的问题（字符串数组）
 * @param {Record<string, object>} i.snapshots 场景 ID 到快照
 * @param {{zh:object,en:object,fr:object}} i.dicts 合并后的词条
 * @param {Set<string>} i.tokens 令牌色集合
 * @param {(gb:string, code:string)=>boolean} i.anchorMatches
 */
export function analyze(i) {
    const out = [];
    const add = (kind, severity, scene, detail, element = null) => out.push({ kind, severity, scene, detail, element });
    for (const d of i.manifestFindings) add("场景", "red", null, d);
    for (const s of i.scenes) {
        const snap = i.snapshots[s.id];
        if (!snap) continue; // 清单里没有的场景已经在 manifestFindings 里报过
        if (!snap.shown) {
            add("覆盖", "red", s.id, "rinneShowScene 没能显示该场景");
            continue;
        }
        // 覆盖：mustShow 的元件都画到了；整窗场景的元件根节点带 data-state
        for (const code of s.mustShow) {
            const hits = snap.gbs.filter((g) => i.anchorMatches(g.gb, code));
            if (!hits.length) add("覆盖", "red", s.id, `元件 ${code}（${i.anchors[code]}）没有画到`, code);
            else if (s.role !== "catalog" && !hits.some((g) => g.state !== ""))
                add("覆盖", "red", s.id, `元件 ${code} 的节点没有 data-state`, code);
        }
        // 文案
        for (const t of snap.texts) {
            const lang = LANG_CODES.includes(t.lang) ? t.lang : s.lang;
            if (t.vals === "bad") {
                add("文案", "red", s.id, `${t.key} 的 data-t-vals 不是合法 JSON`, t.key);
                continue;
            }
            const dict = i.dicts[lang] || {};
            if (!has(dict, t.key)) {
                // 词条文件是分批入库的，缺键不判红，进「待补」清单；键在而文字不同才是偏离
                add("文案", "note", s.id, `词条 ${t.key} 待补（web/i18n 里还没有这个键）`, t.key);
                continue;
            }
            const got = norm(t.text);
            if (lang === "fr") {
                if (got === "") add("文案", "red", s.id, `${t.key}（fr）是空的`, t.key);
                continue;
            }
            const want = norm(fill(dict[t.key], t.vals));
            if (got !== want) {
                add(
                    "文案",
                    t.errata ? "note" : "red",
                    s.id,
                    `${t.key}（${lang}）与词条不一致：设计稿「${got}」，词条「${want}」${t.errata ? "（带 data-errata，等词条合入）" : ""}`,
                    t.key,
                );
            }
        }
        // 颜色
        const stray = [...new Set(snap.colors.map(rgbKey).filter((k) => k !== null && k !== "0,0,0"))].filter(
            (k) => !i.tokens.has(k),
        );
        // 纯黑只在 alpha 为 0 的透明里出现，不算；真正的黑色阴影也走 token
        if (stray.length)
            add(
                "颜色",
                "red",
                s.id,
                `不在 token 里的颜色：${stray
                    .slice(0, 8)
                    .map((k) => `rgb(${k})`)
                    .join("、")}${stray.length > 8 ? ` 等 ${stray.length} 种` : ""}`,
            );
        // 安全红字
        if (s.role !== "catalog" && s.mustShow.includes("I-17") && !snap.redNodes.some((r) => r.safety)) {
            add("安全红字", "red", s.id, '整窗场景必须画出紧急切断（data-safety="panic"）', "I-17");
        }
        for (const r of snap.redNodes) {
            const where = `${r.gb}${r.key ? ` ${r.key}` : ""}`;
            if (r.opacity < 0.99)
                add("安全红字", "red", s.id, `${where} 的不透明度链乘积 ${r.opacity.toFixed(2)}，红字不得降级`, r.gb);
            if (r.alpha < 0.99)
                add("安全红字", "red", s.id, `${where} 的文字颜色 alpha ${r.alpha.toFixed(2)}，红字不得降级`, r.gb);
            if (!r.visible) add("安全红字", "red", s.id, `${where} 不可见`, r.gb);
            if (r.clipped) add("安全红字", "red", s.id, `${where} 被截断`, r.gb);
            if (r.fontSize < 9) add("安全红字", "red", s.id, `${where} 字号 ${r.fontSize}px，小于 9px`, r.gb);
        }
        // 外链
        for (const url of snap.externals) add("外链", "red", s.id, `外部引用：${url}`);
    }
    // 同一场景里同一条记录只留一份
    const seen = new Set();
    return out.filter((f) => {
        const k = `${f.severity}|${f.kind}|${f.scene}|${f.detail}`;
        return seen.has(k) ? false : (seen.add(k), true);
    });
}

/** 按 A 缺失、B 偏离、D 稿内缺陷的草稿分类生成报告文本。 */
export function renderReport(findings) {
    const cls = {
        场景: "A 缺失",
        覆盖: "A 缺失",
        文案: "B 偏离",
        颜色: "B 偏离",
        安全红字: "D 稿内缺陷",
        外链: "D 稿内缺陷",
    };
    const lines = [];
    const red = findings.filter((f) => f.severity === "red");
    const notes = findings.filter((f) => f.severity === "note");
    lines.push(`设计稿机检：${red.length} 项需要处理，${notes.length} 项仅记录`);
    for (const kind of ["场景", "覆盖", "文案", "颜色", "安全红字", "外链"]) {
        const list = findings.filter((f) => f.kind === kind);
        if (!list.length) continue;
        lines.push("", `## ${kind}（草稿分类 ${cls[kind]}）`);
        for (const f of list)
            lines.push(`- ${f.severity === "note" ? "[记录] " : ""}${f.scene ? `${f.scene}：` : ""}${f.detail}`);
    }
    return lines.join("\n");
}

/** 按 DELTA 固定五节写的草稿：背景、改动（按元件 ID 逐条）、新增或修改的场景 ID、不变的硬约束、交付。 */
export function renderDelta(findings) {
    const red = findings.filter((f) => f.severity === "red");
    const byElement = new Map();
    for (const f of red) {
        const key = f.element || "（场景级）";
        byElement.set(key, [...(byElement.get(key) || []), `${f.scene ? `${f.scene}：` : ""}${f.detail}`]);
    }
    const scenes = [...new Set(red.map((f) => f.scene).filter(Boolean))];
    return [
        "# DELTA 草稿（机检生成，待用户裁定 A 缺失 / B 偏离 / C 新增 / D 稿内缺陷）",
        "",
        "## 背景",
        `机检在设计稿上发现 ${red.length} 项需要处理的问题。`,
        "",
        "## 改动（按元件 ID 逐条）",
        ...[...byElement].flatMap(([k, v]) => [`- ${k}`, ...v.map((x) => `  - ${x}`)]),
        "",
        "## 新增或修改的场景 ID",
        scenes.length ? scenes.map((s) => `- ${s}`).join("\n") : "- 无",
        "",
        "## 不变的硬约束",
        "- 设计稿说明里列出的硬约束全部不变；本草稿只改上面列出的项。",
        "",
        "## 交付",
        "- 在同一个 .dc.html 上增量修改，版本号加一，改动过的场景在下拉里标「[Δ]」。",
        "",
    ].join("\n");
}

// ---------------------------------------------------------------- 取真源 + 浏览器运行

async function loadSources(root) {
    const url = (p) => pathToFileURL(path.join(root, p)).href;
    const { SCENES } = await import(url("web-preview/mock/scenarios.js"));
    const { ELEMENT_ANCHORS, anchorMatches } = await import(url("web-preview/mock/anchors.js"));
    const { DESIGN } = await import(url("web/shared/design-box.js"));
    const { mergeDicts } = await import(url("web/shared/i18n.js"));
    const parts = [];
    for (const name of ["common", "input", "output", "guide"]) {
        const f = path.join(root, "web", "i18n", `${name}.js`);
        if (!fs.existsSync(f)) continue;
        const part = (await import(pathToFileURL(f).href)).default;
        parts.push(part);
    }
    const tokens = tokenColors(fs.readFileSync(path.join(root, "web/shared/tokens.css"), "utf8"));
    return { SCENES, ELEMENT_ANCHORS, anchorMatches, DESIGN, dicts: mergeDicts(...parts), tokens };
}

/** 跑完整检查。返回 { skip } 或 { findings }。 */
export async function runAgainstDesign(root, designFile, only = []) {
    const { findBrowser, launchBrowser } = await import(
        pathToFileURL(path.join(root, "web-preview/tests/lib/browser.mjs")).href
    );
    if (!findBrowser()) return { skip: "没有 Chromium 系浏览器" };
    const src = await loadSources(root);
    const m = readManifest(designFile);
    if (m.problem)
        return { findings: [{ kind: "场景", severity: "red", scene: null, detail: m.problem, element: null }] };
    const scenes = only.length ? src.SCENES.filter((s) => only.includes(s.id)) : src.SCENES;
    const browser = await launchBrowser({ width: 1600, height: 1100 });
    const snapshots = {};
    let docExt = { styles: [], resources: [] };
    try {
        const page = await browser.newPage();
        await page.goto(pathToFileURL(designFile).href);
        await page.waitFor("typeof window.rinneShowScene === 'function'", {
            timeout: 30000,
            label: "设计稿的 rinneShowScene",
        });
        await new Promise((r) => setTimeout(r, 1500));
        for (const s of scenes) {
            snapshots[s.id] = await page.eval(collectExpression(s.id));
            await new Promise((r) => setTimeout(r, 150));
        }
        docExt = await page.eval(documentExternalsExpression);
    } finally {
        await browser.close();
    }
    const manifestFindings = compareManifest(src.SCENES, m.manifest, src.DESIGN);
    const findings = analyze({
        scenes,
        anchors: src.ELEMENT_ANCHORS,
        manifestFindings,
        snapshots,
        dicts: src.dicts,
        tokens: src.tokens,
        anchorMatches: src.anchorMatches,
    });
    findings.push(...documentFindings(fs.readFileSync(designFile, "utf8"), docExt));
    return { findings };
}

// ---------------------------------------------------------------- 自测（纯分析，不开浏览器）

export function selfTest() {
    const anchors = { "I-01": "input.shell", "I-17": "input.panic" };
    const base = () => ({
        scenes: [{ id: "in-a", role: "input", lang: "zh", mustShow: ["I-01", "I-17"] }],
        anchors,
        manifestFindings: [],
        snapshots: {
            "in-a": {
                shown: true,
                gbs: [
                    { gb: "input.shell", state: "normal" },
                    { gb: "input.panic", state: "armed" },
                ],
                texts: [{ attr: "text", key: "common.cancel", vals: null, errata: false, text: " 取消 " }],
                redNodes: [
                    {
                        gb: "input.panic",
                        key: "",
                        safety: true,
                        opacity: 1,
                        alpha: 1,
                        visible: true,
                        fontSize: 12,
                        clipped: false,
                    },
                ],
                externals: [],
                colors: ["rgb(198, 110, 120)", "rgba(0, 0, 0, 0)"],
            },
        },
        dicts: {
            zh: { "common.cancel": "取消", "common.hello": "你好 {n}" },
            en: { "common.cancel": "Cancel" },
            fr: { "common.cancel": "Annuler" },
        },
        tokens: new Set(["198,110,120"]),
        anchorMatches: (gb, code) => gb === anchors[code] || gb.startsWith(`${anchors[code]}.`),
    });
    const mutate = (fn) => {
        const i = base();
        fn(i);
        return analyze(i);
    };
    const reds = (r) => r.filter((f) => f.severity === "red");
    const snap = (i) => i.snapshots["in-a"];
    const cases = [];
    const add = (label, bad, findings, kind) => {
        const r = reds(findings).filter((f) => !kind || f.kind === kind);
        cases.push({ label, bad, passed: bad ? r.length > 0 : r.length === 0 });
    };

    add("一切正常", false, analyze(base()));
    add(
        "fr 只查非空",
        false,
        mutate((i) => {
            i.scenes[0].lang = "fr";
            snap(i).texts[0].text = "n’importe quoi";
        }),
    );
    add(
        "词条还没有的键只记待补",
        false,
        mutate((i) => snap(i).texts.push({ attr: "text", key: "in.new", vals: null, errata: false, text: "x" })),
    );
    add(
        "带 data-errata 的不一致只记录",
        false,
        mutate((i) => {
            snap(i).texts[0].text = "别的";
            snap(i).texts[0].errata = true;
        }),
    );
    add(
        "清单差异判红",
        true,
        mutate((i) => i.manifestFindings.push("场景表有 in-x，设计稿清单没有")),
        "场景",
    );
    add(
        "场景显示不出来",
        true,
        mutate((i) => (snap(i).shown = false)),
        "覆盖",
    );
    add(
        "mustShow 的元件没画",
        true,
        mutate((i) => snap(i).gbs.pop()),
        "覆盖",
    );
    add(
        "整窗元件缺 data-state",
        true,
        mutate((i) => (snap(i).gbs[0].state = "")),
        "覆盖",
    );
    add(
        "文案与词条不一致",
        true,
        mutate((i) => (snap(i).texts[0].text = "确定")),
        "文案",
    );
    add(
        "en 文案不一致",
        true,
        mutate((i) => {
            i.scenes[0].lang = "en";
            snap(i).texts[0].text = "Cancel!";
        }),
        "文案",
    );
    add(
        "fr 文案为空",
        true,
        mutate((i) => {
            i.scenes[0].lang = "fr";
            snap(i).texts[0].text = "  ";
        }),
        "文案",
    );
    add(
        "占位符代入错误",
        true,
        mutate((i) =>
            snap(i).texts.push({ attr: "text", key: "common.hello", vals: { n: 3 }, errata: false, text: "你好 4" }),
        ),
        "文案",
    );
    add(
        "占位符代入正确",
        false,
        mutate((i) =>
            snap(i).texts.push({ attr: "text", key: "common.hello", vals: { n: 3 }, errata: false, text: "你好 3" }),
        ),
    );
    add(
        "data-t-vals 不是 JSON",
        true,
        mutate((i) => snap(i).texts.push({ attr: "text", key: "common.hello", vals: "bad", errata: false, text: "" })),
        "文案",
    );
    // title、aria 的占位符代入值各有自己的属性。用假节点跑 textRecordsOf（与浏览器里嵌入的是同一份源码）再交给 analyze
    // lang 是祖先的 data-lang，selfLang 是节点自己带的；closest 与浏览器一样先算自己再往上找，parentElement.closest 只找祖先
    const fakeEl = (attrs, { errata = false, text = "", lang = null, selfLang = null } = {}) => {
        const ancestor = lang ? { getAttribute: () => lang } : null;
        const self = selfLang ? { getAttribute: () => selfLang } : null;
        return {
            hasAttribute: (n) => Object.hasOwn(attrs, n),
            getAttribute: (n) => (Object.hasOwn(attrs, n) ? attrs[n] : null),
            closest: (sel) => {
                if (sel === "[data-errata]") return errata ? {} : null;
                if (sel === "[data-lang]") return self || ancestor;
                return null;
            },
            parentElement: { closest: (sel) => (sel === "[data-lang]" ? ancestor : null) },
            textContent: text,
        };
    };
    const withNode = (attrs, opts, dict) =>
        mutate((i) => {
            snap(i).texts = textRecordsOf(fakeEl(attrs, opts));
            Object.assign(i.dicts.zh, dict);
        });
    const tipDict = { "x.tip": "共 {n} 项", "x.aria": "{n} 项可选" };
    // 目录里的多语言对照格外层带 data-lang：按该语言的词条核对，不按场景语言
    add(
        "带 data-lang=en 的对照格按 en 词条核对",
        false,
        withNode({ "data-t": "common.cancel" }, { lang: "en", text: "Cancel" }, {}),
    );
    add(
        "带 data-lang=en 的对照格里放了 zh 文字",
        true,
        withNode({ "data-t": "common.cancel" }, { lang: "en", text: "取消" }, {}),
        "文案",
    );
    // fr 节点同样按节点自己的语言走「只查非空」：文字非空为绿、空串为红（场景语言是 zh，不拿 zh 词条去比 fr 文字）
    add(
        "带 data-lang=fr 的对照格文字非空：只查非空，为绿",
        false,
        withNode({ "data-t": "common.cancel" }, { lang: "fr", text: "Annuler" }, {}),
    );
    add(
        "带 data-lang=fr 的对照格是空的",
        true,
        withNode({ "data-t": "common.cancel" }, { lang: "fr", text: "  " }, {}),
        "文案",
    );
    add(
        "data-lang 取值不认识时回到场景语言",
        false,
        withNode({ "data-t": "common.cancel" }, { lang: "xx", text: "取消" }, {}),
    );
    // 节点自己带 data-lang 不改变核对语言：只有祖先的 data-lang 才算（自己带 fr 就退成只查非空，会放过不一致）
    add(
        "节点自己带 data-lang=fr：仍按场景语言逐字比，文字与 zh 词条不一致为红",
        true,
        withNode({ "data-t": "common.cancel" }, { selfLang: "fr", text: "Annuler" }, {}),
        "文案",
    );
    add(
        "节点自己带 data-lang=en：仍按场景语言比，文字与 zh 词条一致为绿",
        false,
        withNode({ "data-t": "common.cancel" }, { selfLang: "en", text: "取消" }, {}),
    );
    add(
        "节点自己带 data-lang=fr、祖先带 data-lang=en：按祖先的 en 逐字比",
        true,
        withNode({ "data-t": "common.cancel" }, { selfLang: "fr", lang: "en", text: "Annuler" }, {}),
        "文案",
    );
    add(
        "title 的代入值写在 data-t-title-vals 时按它代入",
        false,
        withNode({ "data-t-title": "x.tip", "data-t-title-vals": '{"n":3}', title: "共 3 项" }, {}, tipDict),
    );
    add(
        "title 的代入值写在 data-t-title-vals 但文字不对",
        true,
        withNode({ "data-t-title": "x.tip", "data-t-title-vals": '{"n":3}', title: "共 4 项" }, {}, tipDict),
        "文案",
    );
    add(
        "aria 的代入值写在 data-t-aria-vals 时按它代入",
        false,
        withNode({ "data-t-aria": "x.aria", "data-t-aria-vals": '{"n":5}', "aria-label": "5 项可选" }, {}, tipDict),
    );
    add(
        "aria 的代入值写在 data-t-aria-vals 但文字不对",
        true,
        withNode({ "data-t-aria": "x.aria", "data-t-aria-vals": '{"n":5}', "aria-label": "6 项可选" }, {}, tipDict),
        "文案",
    );
    add(
        "同一节点上 title 与 aria 各用各的值（取错属性会判红）",
        false,
        withNode(
            {
                "data-t-title": "x.tip",
                "data-t-title-vals": '{"n":3}',
                title: "共 3 项",
                "data-t-aria": "x.aria",
                "data-t-aria-vals": '{"n":5}',
                "aria-label": "5 项可选",
            },
            {},
            tipDict,
        ),
    );
    add(
        "文字仍读 data-t-vals，不读 data-t-title-vals",
        false,
        withNode(
            { "data-t": "common.hello", "data-t-vals": '{"n":3}', "data-t-title-vals": '{"n":9}' },
            { text: "你好 3" },
            {},
        ),
    );
    add(
        "title、aria 没有自己的代入值时退回 data-t-vals",
        false,
        withNode(
            {
                "data-t-vals": '{"n":3}',
                "data-t-title": "common.hello",
                title: "你好 3",
                "data-t-aria": "common.hello",
                "aria-label": "你好 3",
            },
            {},
            {},
        ),
    );
    add(
        "data-t-title-vals 不是 JSON",
        true,
        withNode({ "data-t-title": "x.tip", "data-t-title-vals": "{oops", title: "" }, {}, tipDict),
        "文案",
    );
    add(
        "出现不在 token 里的颜色",
        true,
        mutate((i) => snap(i).colors.push("rgb(1, 2, 3)")),
        "颜色",
    );
    add(
        "红字被降级（不透明度）",
        true,
        mutate((i) => (snap(i).redNodes[0].opacity = 0.6)),
        "安全红字",
    );
    add(
        "红字被降级（alpha）",
        true,
        mutate((i) => (snap(i).redNodes[0].alpha = 0.5)),
        "安全红字",
    );
    add(
        "红字被截断",
        true,
        mutate((i) => (snap(i).redNodes[0].clipped = true)),
        "安全红字",
    );
    add(
        "红字太小",
        true,
        mutate((i) => (snap(i).redNodes[0].fontSize = 8)),
        "安全红字",
    );
    add(
        "整窗场景没画紧急切断",
        true,
        mutate((i) => (snap(i).redNodes = [])),
        "安全红字",
    );
    add(
        "引用外链",
        true,
        mutate((i) => snap(i).externals.push("https://example.invalid/x.css")),
        "外链",
    );
    add(
        "稿件源码里的外部地址",
        true,
        documentFindings('<link href="https://fonts.example.invalid/x.css">', {}),
        "外链",
    );
    add(
        "预览专用块里的外部地址放行",
        false,
        documentFindings('<link data-preview-only="true" href="https://fonts.example.invalid/x.css">', {}),
    );
    add(
        "联系信息与 SVG 命名空间放行",
        false,
        documentFindings(
            "https://synchain.ca/a https://github.com/synchain-oss/rinne/issues http://www.w3.org/2000/svg",
            {},
        ),
    );
    add("样式表里的外部引用", true, documentFindings("", { styles: ["@import url(https://x.invalid/a.css)"] }), "外链");
    add("运行期请求只记录", false, documentFindings("", { resources: ["https://cdn.invalid/a.js"] }));
    const rep = renderReport(analyze(base()));
    cases.push({ label: "报告能生成", bad: false, passed: rep.includes("0 项需要处理") });
    cases.push({
        label: "DELTA 草稿带固定五节",
        bad: false,
        passed: ["## 背景", "## 改动", "## 新增或修改的场景 ID", "## 不变的硬约束", "## 交付"].every((h) =>
            renderDelta([]).includes(h),
        ),
    });
    cases.push({
        label: "颜色归一：#rgb 与 rgba() 同键",
        bad: false,
        passed: rgbKey("#f00") === "255,0,0" && rgbKey("rgba(255, 0, 0, .5)") === "255,0,0",
    });
    return caseResult(cases);
}

// ---------------------------------------------------------------- CLI

if (isMain(import.meta.url)) {
    assertNodeVersion();
    const argv = process.argv.slice(2);
    if (argv.includes("--self-test")) {
        const r = selfTest();
        for (const l of r.lines) console.error(l);
        console.log(`check-design-manifest 自测：${r.ok ? "通过" : "失败"}（坏样例 ${r.bad}、正例 ${r.good}）`);
        process.exitCode = r.ok ? 0 : 1;
    } else {
        const opt = (k) => (argv.includes(k) ? argv[argv.indexOf(k) + 1] : null);
        const only = argv.flatMap((a, i) => (a === "--scene" ? [argv[i + 1]] : []));
        const file = locateDesign(ROOT, opt("--design"));
        if (!file) {
            console.log("check-design-manifest：设计稿还没有入库（docs/design/ 不存在），跳过");
            process.exitCode = 2;
        } else {
            const r = await runAgainstDesign(ROOT, file, only);
            if (r.skip) {
                console.log(`check-design-manifest：跳过（${r.skip}）`);
                process.exitCode = 2;
            } else {
                console.log(renderReport(r.findings));
                if (opt("--delta")) {
                    // 只写进 build/design-delta/，只接受不带目录的文件名，后缀限 .md / .json（见 safe-out.mjs）
                    const out = resolveOutFile(ROOT, ["build", "design-delta"], opt("--delta"), [".md", ".json"]);
                    fs.writeFileSync(out, renderDelta(r.findings));
                    console.log(`DELTA 草稿已写入 ${path.relative(ROOT, out)}`);
                }
                process.exitCode = r.findings.some((f) => f.severity === "red") ? 1 : 0;
            }
        }
    }
}
