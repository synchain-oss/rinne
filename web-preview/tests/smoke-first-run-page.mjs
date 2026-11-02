// SPDX-License-Identifier: GPL-3.0-or-later
//
// 首启链的页面级冒烟（无头浏览器）：真源页面经预览壳页装载，指针与按键用 CDP 发真事件。
//   ① Input 全新（?fresh=1）：两路首批快照到齐后出语言卡；语言卡不响应 Esc；选语言后进规则页，按钮先禁用，
//      没激活的环路上按 Esc 不关；滚到底放行，[开始导览] 进导览，→ / ← 翻页，Esc 结束；三个写位依次写对；
//      结束后内容层不再 inert；再打开（不清全局位）整条链不出；
//   ② 规则页的四条红字：标题、四条的引导词与正文、脚注都是红色墨水、不透明、字重不低于 500，没有省略号与行数截断，
//      横向不溢出，可见的每一行没有被别的东西盖住（放行前的提示也不许盖），文字与词条逐字相同；标题完整落在面板里；
//   ③ 放行：不滚动时 8 秒后按钮放行，之前一直禁用；
//   ④ 紧急切断：语言卡、规则页、导览开着时，按钮内五个点的 elementFromPoint 都落在按钮自己身上；导览中真点一下，
//      叠层立即关、桥收到一次 panic、不写「引导已看」；
//   ⑤ 危险态立即关：语言卡开着时注入切断（L2）、规则页开着时失控灯亮，叠层立即关、不写位；危险解除后不再弹；
//   ⑥ 恢复的实例：不弹链，页脚换成 hint.firstUseRestored；「?」打开的规则页要确认（只有 [我已了解]），确认后页脚恢复；
//      每一帧快照处理完，页脚都与这一帧一致（打开时当帧就是恢复提示，确认后当帧换回）；
//   ⑦ Esc：要确认的规则页在环路激活（正在回送）时可以 Esc，不算确认；重看的规则页随时可以 Esc；
//      危险态下用户自己点「?」打开的规则页是普通模态，不自动关，紧急切断照常可点；
//   ⑧ Output：全新走 语言卡 → 规则页 → 4 步导览，链开着时外壳是遮罩态、结束后回常态；Input 的导览看过时只弹规则页
//      （[我已了解]），确认后不接导览；规则页开着时配对冲突（L2）叠层立即关；恢复的实例同样换页脚提示，同样当帧跟上；
//   ⑨ 导览开着时电平帧（增益衰减变了）不重量导览，快照变了才重量（对照）；
//   ⑩ setLang 回包不 ok：规则页照常打开，但退回宿主的语言，全局位没写；
//   坏样例：在紧急切断上盖一层透明元素，命中判据必须判红；把红字改成灰色、加省略号、正文底部盖一条不接受命中的渐变，
//   红字判据必须判红。
// 浏览器缺失时整套跳过（退出码 2），不是通过。本机同时只能有一个浏览器冒烟。
// 本文件同时导出首启的两个页面冒烟（smoke-tour-page、smoke-first-run-e2e-page）共用的打开、点击与量尺。
//
// 用法：node web-preview/tests/smoke-first-run-page.mjs [仓库根]

import path from "node:path";
import { pathToFileURL } from "node:url";
import { createRun, repoRoot } from "./lib/harness.mjs";
import { findBrowser, launchBrowser } from "./lib/browser.mjs";
import { startStaticServer } from "./lib/static-server.mjs";
import { inFrame, settle } from "./smoke-input-kit-page.mjs";

export { inFrame };

/** 预览壳页的视口：够放下 1 档的两个窗口。 */
export const VIEW = Object.freeze({ w: 1200, h: 900 });
export const PANIC = '[data-gb="input.panic"]';

/** 打开某个角色的页面并等它画完。fresh 先清全局位；scenario 缺省时按默认态（首启链在默认态下自动弹）。 */
export async function openRole(page, server, role, { fresh = true, scenario, lang } = {}) {
    const q = new URLSearchParams();
    if (fresh) q.set("fresh", "1");
    if (scenario) q.set("scenario", scenario);
    if (lang) q.set("lang", lang);
    await page.setViewport(VIEW.w, VIEW.h, 1);
    await page.goto(`${server.url}/web-preview/${role}.html?${q}`);
    const label = `${role}/${scenario || "默认态"}`;
    await page.waitFor(
        `(() => { const s = document.querySelector(".pv-status"); return !!s && s.getAttribute("data-ok") !== "wait"; })()`,
        { timeout: 20000, label: `壳页注入（${label}）` },
    );
    await page.waitFor(
        `(() => { const f = document.querySelector("iframe"); return !!(f && f.contentDocument && f.contentDocument.querySelector('[data-gb="${role}.shell"]')); })()`,
        { timeout: 20000, label: `页面 boot（${label}）` },
    );
    await settle(page);
}

/** 叠层现状：{ lang, rules（data-state 或 null）, buttons（规则页按钮名）, tour（步号或 null）, inert }。 */
export function overlayState(doc, win, role) {
    const shown = (e) => !!e && !e.closest("[hidden]");
    const lang = doc.querySelector(`[data-gb="${role}.langStart"]`);
    const rules = doc.querySelector(`[data-gb="${role}.rules"]`);
    const tour = doc.querySelector(`[data-gb="${role}.tour"]`);
    const content = doc.querySelector(`[data-gb="${role}.shell"] > [data-part="content"]`);
    return {
        lang: shown(lang),
        rules: shown(rules) ? rules.getAttribute("data-state") : null,
        buttons: shown(rules)
            ? [...rules.querySelectorAll('[data-part^="rules."]')]
                  .filter((b) => b.tagName === "BUTTON")
                  .map((b) => b.getAttribute("data-part").slice(6) + (b.disabled ? ":off" : ""))
            : [],
        tour: shown(tour) ? Number((tour.getAttribute("data-state") || "").replace("step-", "")) || null : null,
        inert: Boolean(content && content.hasAttribute("inert")),
    };
}

export const stateOf = (page, role) => inFrame(page, overlayState, role);

/** 等叠层到某个状态：pred 是 (s) => boolean 的源码字符串，s 是 overlayState 的结果。 */
export function waitOverlay(page, role, pred, label, timeout = 6000) {
    return page.waitFor(
        `(() => { const f = document.querySelector("iframe"); const s = (${overlayState.toString()})(f.contentDocument, f.contentWindow, ${JSON.stringify(role)}); return (${pred})(s); })()`,
        { timeout, label },
    );
}

/** 页面里某元素的中心（壳页视口坐标）；不存在返回 null。 */
export function centerIn(page, selector) {
    return page.eval(
        `(() => { const f = document.querySelector("iframe"); const e = f.contentDocument.querySelector(${JSON.stringify(selector)}); if (!e) return null; const b = e.getBoundingClientRect(); const o = f.getBoundingClientRect(); return { x: o.x + b.x + b.width / 2, y: o.y + b.y + b.height / 2 }; })()`,
    );
}

const mouse = (page, type, x, y, extra = {}) =>
    page.send("Input.dispatchMouseEvent", { type, x, y, button: "left", pointerType: "mouse", ...extra });

/** 真点一下（壳页坐标）。 */
export async function clickAt(page, x, y) {
    await mouse(page, "mouseMoved", x, y);
    await mouse(page, "mousePressed", x, y, { clickCount: 1 });
    await mouse(page, "mouseReleased", x, y, { clickCount: 1 });
    await page.eval("new Promise((r) => setTimeout(r, 120))");
}

/** 点页面里的某个元素；找不到抛错。 */
export async function clickPart(page, selector) {
    const c = await centerIn(page, selector);
    if (!c) throw new Error(`页面里没有 ${selector}`);
    await clickAt(page, c.x, c.y);
}

/** 拖动：按下、分小步移动、抬起。steps 是 [dx, dy] 的增量序列（壳页坐标）；每步后可选回调 onStep，返回真则提前抬起。 */
export async function drag(page, x, y, steps, onStep) {
    await mouse(page, "mouseMoved", x, y);
    await mouse(page, "mousePressed", x, y, { clickCount: 1 });
    let cx = x;
    let cy = y;
    for (const [dx, dy] of steps) {
        cx += dx;
        cy += dy;
        await mouse(page, "mouseMoved", cx, cy);
        if (onStep && (await onStep())) break;
    }
    await mouse(page, "mouseReleased", cx, cy, { clickCount: 1 });
    await page.eval("new Promise((r) => setTimeout(r, 150))");
}

const KEYS = {
    Escape: { code: "Escape", vk: 27 },
    ArrowRight: { code: "ArrowRight", vk: 39 },
    ArrowLeft: { code: "ArrowLeft", vk: 37 },
};

/** 按一个键（CDP 真按键，落在当前焦点所在的文档）。 */
export async function press(page, key) {
    const k = KEYS[key];
    const base = { key, code: k.code, windowsVirtualKeyCode: k.vk, nativeVirtualKeyCode: k.vk };
    await page.send("Input.dispatchKeyEvent", { type: "rawKeyDown", ...base });
    await page.send("Input.dispatchKeyEvent", { type: "keyUp", ...base });
    await page.eval("new Promise((r) => setTimeout(r, 120))");
}

/** 规则页的正文区滚到底（用户读到底）。 */
export const scrollRulesToEnd = (page, role) =>
    inFrame(
        page,
        (doc, win, r) => {
            const b = doc.querySelector(`[data-gb="${r}.rules"] [data-part="rules.body"]`);
            b.scrollTop = b.scrollHeight;
            b.dispatchEvent(new win.Event("scroll"));
        },
        role,
    );

/** 壳页的全局位（预览后端存在 localStorage 里）。 */
export const uiDefaults = (page) => page.eval(`JSON.parse(localStorage.getItem("rinne.uiDefaults") || "{}")`);

/** 在壳页里给桥函数装上调用记录（与 smoke-input-kit-page 的 recordBridge 同法，记录在 window.__calls）。 */
export async function recordBridge(page, names) {
    await page.eval(`(() => {
        const mock = window.__RINNE_PREVIEW__.mock;
        window.__calls = [];
        for (const n of ${JSON.stringify(names)}) {
            if (mock.__wrapped && mock.__wrapped.includes(n)) continue;
            const orig = mock[n];
            mock[n] = function (...a) { window.__calls.push([n, ...a]); return orig.apply(mock, a); };
            (mock.__wrapped = mock.__wrapped || []).push(n);
        }
    })()`);
    return { calls: () => page.eval(`window.__calls.slice()`) };
}

/** 首启的三个写位。 */
export const WRITES = Object.freeze(["setLang", "setRulesAck", "setGuideSeen"]);

/** 按钮内五个点的命中：{ points, hits, who }。 */
export function hitProbe(doc, win, sel) {
    const e = doc.querySelector(sel);
    const r = e.getBoundingClientRect();
    const pts = [
        [0.5, 0.5],
        [0.15, 0.3],
        [0.85, 0.3],
        [0.15, 0.7],
        [0.85, 0.7],
    ];
    let hits = 0;
    const who = [];
    for (const [fx, fy] of pts) {
        const t = doc.elementFromPoint(r.left + r.width * fx, r.top + r.height * fy);
        if (t && (t === e || e.contains(t))) hits++;
        else who.push(t ? t.getAttribute("data-gb") || t.getAttribute("data-part") || t.className || t.tagName : "无");
    }
    return { points: pts.length, hits, who };
}

/**
 * 规则页红字的量尺：expected 是 { title, items:[{lead, body}], footnote }。返回问题列表（空表示合格）。
 * 红字：颜色等于 --sem-red-ink-dark 的计算值、到面板为止的各层不透明度都是 1、字重不低于 500、
 * 没有省略号与行数截断、横向不溢出、可见的每一行没有被别的东西盖住；文字与词条逐字相同；标题完整落在面板里。
 */
export function redProbe(doc, win, role, expected) {
    const panel = doc.querySelector(`[data-gb="${role}.rules"]`);
    const probe = doc.createElement("span");
    probe.style.color = "var(--sem-red-ink-dark)";
    panel.append(probe);
    const red = win.getComputedStyle(probe).color;
    probe.remove();
    const out = [];
    const reds = [...panel.querySelectorAll('[data-tone="red"]')];
    if (reds.length < 10) out.push(`红字元素只有 ${reds.length} 个（应至少 10 个：标题、四条的引导词与正文、脚注）`);
    for (const e of reds) {
        const cs = win.getComputedStyle(e);
        const name = e.className;
        if (cs.color !== red) out.push(`${name} 颜色 ${cs.color}，不是红色墨水 ${red}`);
        if (Number(cs.fontWeight) < 500) out.push(`${name} 字重 ${cs.fontWeight} 低于 500`);
        if (cs.textOverflow === "ellipsis") out.push(`${name} 带省略号`);
        if (cs.webkitLineClamp && cs.webkitLineClamp !== "none") out.push(`${name} 有行数截断 ${cs.webkitLineClamp}`);
        if (e.scrollWidth > e.clientWidth + 1) out.push(`${name} 横向溢出 ${e.scrollWidth} > ${e.clientWidth}`);
        for (let n = e; n && n !== panel.parentElement; n = n.parentElement) {
            const o = Number(win.getComputedStyle(n).opacity);
            if (o < 1) out.push(`${name} 的祖先 ${n.className} 不透明度 ${o}`);
        }
    }
    // 遮挡：每条红字在可见区里的每一行，行中点的 elementFromPoint 都要落在这段文字自己（或其子节点）上。
    // 量的时候让面板里的一切（含 ::before / ::after）都接受命中，pointer-events: none 的覆盖层也藏不住
    const sheet = doc.createElement("style");
    const scope = `[data-gb="${role}.rules"]`;
    sheet.textContent = `${scope}, ${scope} *, ${scope} *::before, ${scope} *::after { pointer-events: auto !important; }`;
    doc.head.append(sheet);
    try {
        for (const e of reds) {
            const clip = (e.closest(".rn-rules__body") || panel).getBoundingClientRect();
            const range = doc.createRange();
            range.selectNodeContents(e);
            for (const r of range.getClientRects()) {
                if (r.width < 2 || r.height < 2) continue;
                const x = r.left + r.width / 2;
                const y = r.top + r.height / 2;
                // 滚出可见区的行不量
                if (x < clip.left + 1 || x > clip.right - 1 || y < clip.top + 1 || y > clip.bottom - 1) continue;
                const hit = doc.elementFromPoint(x, y);
                if (!hit || !(hit === e || e.contains(hit))) {
                    const who = hit ? hit.getAttribute("data-part") || hit.className || hit.tagName : "无";
                    out.push(`${e.className} 在 y=${Math.round(y - clip.top)} 的一行被 ${who} 盖住`);
                }
            }
        }
    } finally {
        sheet.remove();
    }
    const title = panel.querySelector(".rn-rules__title");
    const pr = panel.getBoundingClientRect();
    const tr = title.getBoundingClientRect();
    if (tr.top < pr.top - 1 || tr.bottom > pr.bottom + 1 || tr.left < pr.left - 1 || tr.right > pr.right + 1)
        out.push("标题没有完整落在面板里");
    const text = (e) => (e ? e.textContent : null);
    if (text(title) !== expected.title) out.push(`标题文字不符：${text(title)}`);
    const leads = [...panel.querySelectorAll(".rn-rules__lead")];
    const bodies = [...panel.querySelectorAll(".rn-rules__text")];
    expected.items.forEach((it, i) => {
        if (text(leads[i]) !== it.lead) out.push(`第 ${i + 1} 条引导词不符：${text(leads[i])}`);
        if (text(bodies[i]) !== it.body) out.push(`第 ${i + 1} 条正文不符：${text(bodies[i])}`);
    });
    if (text(panel.querySelector(".rn-rules__footnote")) !== expected.footnote) out.push("脚注文字不符");
    return out;
}

/** 词条：三语的 guide 加 common。 */
export async function loadDicts(root) {
    const load = (rel) => import(pathToFileURL(path.join(root, rel)).href);
    const common = (await load("web/i18n/common.js")).default;
    const guide = (await load("web/i18n/guide.js")).default;
    return Object.fromEntries(["zh", "en", "fr"].map((l) => [l, { ...common[l], ...guide[l] }]));
}

/** 规则页红字的期望文字。 */
export const rulesExpected = (dict) => ({
    title: dict["rules.title"],
    items: [1, 2, 3, 4].map((n) => ({ lead: dict[`rules.r${n}.lead`], body: dict[`rules.r${n}.body`] })),
    footnote: dict["rules.footnote"],
});

/**
 * 让下一个打开的壳页把后端标成「恢复的实例」（新机器上打开旧工程）。返回撤销函数。
 * hintRole 给了时另记一份「每帧之后的页脚」：每一帧 rinne.state 发出、页面处理完（同一任务里的微任务都跑完）之后，
 * 记下该帧的 restored、rulesAck 与页脚提示的 data-state，存在壳页的 window.__hintLog（过期帧与被后一帧赶上的不记）。
 */
export async function markRestored(page, { hintRole = null } = {}) {
    const { identifier } = await page.send("Page.addScriptToEvaluateOnNewDocument", {
        source: `(() => {
            if (window.top !== window) return;
            const role = ${JSON.stringify(hintRole)};
            window.__hintLog = [];
            let pv;
            let maxSeq = -Infinity;
            let latest = null;
            const watch = (f) => {
                if (!f || f.seq <= maxSeq) return;
                maxSeq = f.seq;
                latest = f;
                setTimeout(() => {
                    if (latest !== f) return;
                    const fr = document.querySelector("iframe");
                    const h = fr && fr.contentDocument && fr.contentDocument.querySelector('[data-gb="' + role + '.footer.hint"]');
                    window.__hintLog.push({ seq: f.seq, restored: f.restored === true, ack: f.rulesAck, hint: h ? h.getAttribute("data-state") : null });
                }, 0);
            };
            Object.defineProperty(window, "__RINNE_PREVIEW__", { configurable: true, get: () => pv, set: (v) => {
                if (v && v.model) v.model.restored = true;
                if (role && v && v.core) v.core.addEventListener("rinne.state", watch);
                pv = v;
            } });
        })();`,
    });
    return () => page.send("Page.removeScriptToEvaluateOnNewDocument", { identifier });
}

/**
 * 读 markRestored 记下的每帧页脚，返回问题列表：页面画过页脚之后，每一帧的页脚都要与这一帧一致——
 * 恢复的实例且没确认当前版本（RULES_VERSION 为 1）时是 restored，否则不是。另要求两种帧都出现过（量尺不空转）。
 */
export async function hintLogProblems(page) {
    const log = await page.eval("window.__hintLog.slice()");
    // 页面画出页脚之前的帧不算（提示位还是初始的 none）
    const first = log.findIndex((e) => e.hint && e.hint !== "none");
    const drawn = first < 0 ? [] : log.slice(first);
    const want = (e) => e.restored && e.ack !== 1;
    const out = drawn
        .filter((e) => want(e) !== (e.hint === "restored"))
        .map((e) => `第 ${e.seq} 帧（restored=${e.restored}，rulesAck=${e.ack}）之后页脚是 ${e.hint}`);
    if (!drawn.some((e) => want(e))) out.push("没有记到「要换恢复提示」的帧");
    if (!drawn.some((e) => e.restored && !want(e))) out.push("没有记到「确认之后」的帧");
    return out;
}

/** 外壳的 data-state（normal、modal、danger、bypassed）。 */
export const shellStateOf = (page, role) =>
    inFrame(page, (doc, win, r) => doc.querySelector(`[data-gb="${r}.shell"]`).getAttribute("data-state"), role);

/** 改预览后端的模型并让它发一帧快照。fn 是 (model) => void 的源码字符串。 */
export const poke = (page, fn) =>
    page.eval(`(() => { const s = window.__RINNE_PREVIEW__; (${fn})(s.model); s.core.scheduleEcho(); })()`);

const sleep = (page, ms) => page.eval(`new Promise((r) => setTimeout(r, ${ms}))`);

// ================================================================ 本文件作为冒烟运行
const isMain = process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href;
if (isMain) {
    const ROOT = repoRoot();
    const run = createRun("首启链页面");
    if (!findBrowser()) run.skip("没有 Chromium 系浏览器");
    const DICT = await loadDicts(ROOT);
    const server = await startStaticServer(ROOT);
    const browser = await launchBrowser({ width: VIEW.w, height: VIEW.h });
    try {
        const page = await browser.newPage();

        // ============================================================ ① ② ④ Input 全新的整条链
        await openRole(page, server, "input");
        let rec = await recordBridge(page, [...WRITES, "panic"]);
        await waitOverlay(page, "input", "(s) => s.lang", "全新：应出语言卡");
        let s = await stateOf(page, "input");
        run.check(
            s.lang && s.rules === null && s.tour === null && s.inert,
            `语言卡单独开着、内容层 inert：${JSON.stringify(s)}`,
        );
        {
            const h = await inFrame(page, hitProbe, PANIC);
            run.check(h.hits === h.points, `语言卡开着：紧急切断被盖住 ${h.points - h.hits} 个点（${h.who}）`);
        }
        await press(page, "Escape");
        run.check((await stateOf(page, "input")).lang, "语言卡不响应 Esc");
        await clickPart(page, '[data-lang-pick="en"]');
        await waitOverlay(page, "input", '(s) => s.rules === "gated"', "选语言后应进规则页（未放行）");
        s = await stateOf(page, "input");
        run.eq(s.buttons, ["skip:off", "start:off"], "首启规则页：[跳过导览] [开始导览]，先禁用");
        {
            const h = await inFrame(page, hitProbe, PANIC);
            run.check(h.hits === h.points, `规则页开着：紧急切断被盖住 ${h.points - h.hits} 个点（${h.who}）`);
            const red = await inFrame(page, redProbe, "input", rulesExpected(DICT.en));
            run.check(red.length === 0, `Input 规则页红字（en）：${red.join("；")}`);
        }
        await clickPart(page, '[data-gb="input.rules"] [data-part="rules.start"]');
        run.eq((await stateOf(page, "input")).rules, "gated", "禁用时点 [开始导览] 不动");
        await press(page, "Escape");
        run.eq((await stateOf(page, "input")).rules, "gated", "环路没激活：Esc 不关要确认的规则页");
        await scrollRulesToEnd(page, "input");
        await waitOverlay(page, "input", '(s) => s.rules === "ready"', "滚到底应放行");
        run.eq((await stateOf(page, "input")).buttons, ["skip", "start"], "放行后按钮可用");
        await clickPart(page, '[data-gb="input.rules"] [data-part="rules.start"]');
        await waitOverlay(page, "input", "(s) => s.tour === 1 && s.rules === null", "[开始导览] 后应进导览第 1 步");
        {
            const h = await inFrame(page, hitProbe, PANIC);
            run.check(h.hits === h.points, `导览开着：紧急切断被盖住 ${h.points - h.hits} 个点（${h.who}）`);
        }
        await press(page, "ArrowRight");
        run.eq((await stateOf(page, "input")).tour, 2, "→ 翻到第 2 步");
        await press(page, "ArrowLeft");
        run.eq((await stateOf(page, "input")).tour, 1, "← 翻回第 1 步");
        await press(page, "Escape");
        await waitOverlay(page, "input", "(s) => s.tour === null", "导览中 Esc 应结束");
        s = await stateOf(page, "input");
        run.check(
            !s.lang && s.rules === null && s.tour === null && !s.inert,
            `结束后没有叠层、内容层不 inert：${JSON.stringify(s)}`,
        );
        run.eq(
            (await rec.calls()).filter((c) => WRITES.includes(c[0])),
            [
                ["setLang", "en"],
                ["setRulesAck", 1],
                ["setGuideSeen", true],
            ],
            "写位依次是 setLang(en)、setRulesAck(1)、setGuideSeen(true)",
        );
        let d = await uiDefaults(page);
        run.check(
            d.langChosen === true && d.lang === "en" && d.rulesAckVersion === 1 && d.guideSeenInput === true,
            `全局位应写对：${JSON.stringify(d)}`,
        );
        // 再打开（不清全局位）：整条链不出
        await openRole(page, server, "input", { fresh: false });
        await sleep(page, 900);
        s = await stateOf(page, "input");
        run.check(!s.lang && s.rules === null && s.tour === null, `做完再打开：链不出（${JSON.stringify(s)}）`);
        // Output 也不出：规则确认过、Input 的导览看过
        await openRole(page, server, "output", { fresh: false });
        await sleep(page, 900);
        s = await stateOf(page, "output");
        run.check(!s.lang && s.rules === null && s.tour === null, `Input 都做过时 Output 不弹：${JSON.stringify(s)}`);

        // 坏样例：紧急切断上盖一层透明元素，命中判据必须判红
        await openRole(page, server, "input");
        await waitOverlay(page, "input", "(s) => s.lang", "坏样例前：应出语言卡");
        await inFrame(page, (doc) => {
            const b = doc.querySelector('[data-gb="input.shell"]');
            const cover = doc.createElement("div");
            cover.style.cssText = "position:absolute;inset:0;z-index:9999;background:transparent";
            b.append(cover);
        });
        {
            const h = await inFrame(page, hitProbe, PANIC);
            run.check(h.hits < h.points, "坏样例：紧急切断被透明层盖住，命中判据应判红");
        }

        // ============================================================ ③ 放行：不滚动时 8 秒
        await openRole(page, server, "input");
        await waitOverlay(page, "input", "(s) => s.lang", "放行：应出语言卡");
        await clickPart(page, '[data-lang-pick="zh"]');
        await waitOverlay(page, "input", '(s) => s.rules === "gated"', "放行：应进规则页");
        const t0 = Date.now();
        {
            const red = await inFrame(page, redProbe, "input", rulesExpected(DICT.zh));
            run.check(red.length === 0, `Input 规则页红字（zh）：${red.join("；")}`);
        }
        await sleep(page, 6500);
        run.eq((await stateOf(page, "input")).rules, "gated", "打开 6.5 秒：仍未放行");
        await waitOverlay(page, "input", '(s) => s.rules === "ready"', "不滚动 8 秒后应放行", 4000);
        const waited = Date.now() - t0;
        run.check(waited >= 7500, `放行等了 ${waited} ms，应不少于 8 秒（含量测误差）`);
        // 坏样例：红字改成灰色、加省略号、正文底部盖一条不接受命中的渐变，红字判据必须判红
        await inFrame(page, (doc) => {
            const t = doc.querySelector('[data-gb="input.rules"] .rn-rules__text');
            t.style.color = "rgb(128, 128, 128)";
            const l = doc.querySelector('[data-gb="input.rules"] .rn-rules__lead');
            l.style.textOverflow = "ellipsis";
            const body = doc.querySelector('[data-gb="input.rules"] [data-part="rules.body"]');
            const cover = doc.createElement("div");
            cover.style.cssText = `position:absolute;left:0;right:0;top:${body.offsetTop + body.clientHeight - 34}px;height:34px;pointer-events:none;background:linear-gradient(rgba(28,24,38,0),rgba(28,24,38,0.96))`;
            body.parentElement.append(cover);
        });
        {
            const red = await inFrame(page, redProbe, "input", rulesExpected(DICT.zh));
            run.check(red.length >= 3, `坏样例：红字改灰、加省略号、盖渐变应判红（实得 ${red.length} 项）`);
            run.check(
                red.some((m) => m.includes("盖住")),
                `坏样例：正文底部盖一条渐变，遮挡判据应判红（${red.join("；")}）`,
            );
        }

        // ============================================================ ④ 导览中点紧急切断
        await openRole(page, server, "input");
        rec = await recordBridge(page, [...WRITES, "panic"]);
        await waitOverlay(page, "input", "(s) => s.lang", "切断：应出语言卡");
        await clickPart(page, '[data-lang-pick="fr"]');
        await waitOverlay(page, "input", '(s) => s.rules === "gated"', "切断：应进规则页");
        await scrollRulesToEnd(page, "input");
        await waitOverlay(page, "input", '(s) => s.rules === "ready"', "切断：应放行");
        await clickPart(page, '[data-gb="input.rules"] [data-part="rules.start"]');
        await waitOverlay(page, "input", "(s) => s.tour === 1", "切断：应进导览");
        await clickPart(page, PANIC);
        await waitOverlay(page, "input", "(s) => s.tour === null && !s.inert", "导览中点紧急切断：叠层应立即关");
        {
            const calls = await rec.calls();
            run.eq(calls.filter((c) => c[0] === "panic").length, 1, "导览中点紧急切断：桥收到一次 panic");
            run.eq(calls.filter((c) => c[0] === "setGuideSeen").length, 0, "导览被紧急切断关掉：不写「引导已看」");
        }
        d = await uiDefaults(page);
        run.check(d.guideSeenInput === false && d.rulesAckVersion === 1, `切断后全局位：${JSON.stringify(d)}`);

        // ============================================================ ⑤ 危险态立即关
        await openRole(page, server, "input");
        rec = await recordBridge(page, WRITES);
        await waitOverlay(page, "input", "(s) => s.lang", "L2：应出语言卡");
        await poke(page, '(m) => { m.cut = "nan"; m.cutAt = Date.now(); }');
        await waitOverlay(page, "input", "(s) => !s.lang && !s.inert", "语言卡开着时注入切断（L2）：应立即关");
        await poke(page, "(m) => { m.cut = null; }");
        await sleep(page, 900);
        s = await stateOf(page, "input");
        run.check(!s.lang && s.rules === null && s.tour === null, `危险解除后不再弹：${JSON.stringify(s)}`);
        run.eq((await rec.calls()).length, 0, "L2 关掉：不写位");

        await openRole(page, server, "input");
        rec = await recordBridge(page, WRITES);
        await waitOverlay(page, "input", "(s) => s.lang", "失控灯：应出语言卡");
        await clickPart(page, '[data-lang-pick="zh"]');
        await waitOverlay(page, "input", '(s) => s.rules === "gated"', "失控灯：应进规则页");
        await poke(page, '(m) => { m.meters.lamps.guard = "reduced"; m.meters.lamps.guardDb = -6; }');
        await waitOverlay(page, "input", "(s) => s.rules === null && !s.inert", "规则页开着时失控灯亮：应立即关");
        run.eq(
            (await rec.calls()).filter((c) => c[0] !== "setLang"),
            [],
            "失控灯关掉：除了先前选的语言，不写位",
        );

        // ============================================================ ⑥ 恢复的实例
        {
            const undo = await markRestored(page, { hintRole: "input" });
            await openRole(page, server, "input");
            await undo();
            rec = await recordBridge(page, WRITES);
            run.check(
                await page.eval("window.__RINNE_PREVIEW__.model.restored === true"),
                "恢复的实例：后端已标记（防空转）",
            );
            await sleep(page, 900);
            s = await stateOf(page, "input");
            run.check(!s.lang && s.rules === null && s.tour === null, `恢复的实例：不弹链（${JSON.stringify(s)}）`);
            const hint = () =>
                inFrame(page, (doc) => {
                    const h = doc.querySelector('[data-gb="input.footer.hint"]');
                    return { state: h.getAttribute("data-state"), text: h.textContent };
                });
            run.eq(
                await hint(),
                { state: "restored", text: DICT.zh["hint.firstUseRestored"] },
                "恢复的实例：页脚换提示",
            );
            await clickPart(page, '[data-gb="input.footer.help"]');
            await waitOverlay(page, "input", '(s) => s.rules === "gated"', "「?」应打开要确认的规则页");
            run.eq((await stateOf(page, "input")).buttons, ["ack:off"], "恢复的实例从「?」进：只有 [我已了解]");
            await scrollRulesToEnd(page, "input");
            await waitOverlay(page, "input", '(s) => s.rules === "ready"', "恢复的实例：滚到底放行");
            await clickPart(page, '[data-gb="input.rules"] [data-part="rules.ack"]');
            await waitOverlay(page, "input", "(s) => s.rules === null", "[我已了解] 后关掉，不接导览");
            run.eq(await rec.calls(), [["setRulesAck", 1]], "[我已了解]：只写 setRulesAck(1)");
            await page.waitFor(
                `document.querySelector("iframe").contentDocument.querySelector('[data-gb="input.footer.hint"]').getAttribute("data-state") !== "restored"`,
                { label: "确认后页脚不再是恢复提示" },
            );
            await sleep(page, 300);
            const late = await hintLogProblems(page);
            run.check(late.length === 0, `恢复的实例：页脚当帧跟上快照（打开时与确认后）：${late.join("；")}`);
        }

        // ============================================================ ⑦ Esc：环路激活 / 重看
        await openRole(page, server, "input", { scenario: "in-playing" });
        await page.eval(
            `localStorage.setItem("rinne.uiDefaults", JSON.stringify({ langChosen: true, rulesAckVersion: 0, guideSeenInput: true }))`,
        );
        await openRole(page, server, "input", { fresh: false, scenario: "in-playing" });
        rec = await recordBridge(page, WRITES);
        await clickPart(page, '[data-gb="input.footer.help"]');
        await waitOverlay(page, "input", '(s) => s.rules === "gated"', "in-playing：「?」应打开要确认的规则页");
        await press(page, "Escape");
        await waitOverlay(page, "input", "(s) => s.rules === null", "环路激活：Esc 应关掉要确认的规则页");
        run.eq(await rec.calls(), [], "Esc 不算确认：不写位");
        await page.eval(
            `localStorage.setItem("rinne.uiDefaults", JSON.stringify({ langChosen: true, rulesAckVersion: 1, guideSeenInput: true }))`,
        );
        await openRole(page, server, "input", { fresh: false, scenario: "in-searching" });
        await clickPart(page, '[data-gb="input.footer.help"]');
        await waitOverlay(page, "input", '(s) => s.rules === "review"', "确认过当前版本：「?」打开重看");
        run.eq((await stateOf(page, "input")).buttons, ["close", "continue"], "重看：[关闭] [继续导览]，不设闸");
        await press(page, "Escape");
        await waitOverlay(page, "input", "(s) => s.rules === null", "重看：环路没激活也能 Esc");
        // 危险态下用户自己点「?」：规则页是普通模态，照常打开、不自动关，紧急切断照常可点
        await openRole(page, server, "input", { fresh: false, scenario: "in-conflict" });
        await clickPart(page, '[data-gb="input.footer.help"]');
        await waitOverlay(page, "input", '(s) => s.rules === "review"', "L2 下点「?」应打开重看的规则页");
        await sleep(page, 900);
        run.eq((await stateOf(page, "input")).rules, "review", "L2 下用户打开的规则页不自动关");
        {
            const h = await inFrame(page, hitProbe, PANIC);
            run.check(h.hits === h.points, `L2 下的规则页：紧急切断被盖住 ${h.points - h.hits} 个点（${h.who}）`);
        }
        // 重看中点紧急切断：叠层照样立即关（危险态不自动关的只是「不因快照关」，紧急切断关掉一切叠层）
        rec = await recordBridge(page, [...WRITES, "panic"]);
        await clickPart(page, PANIC);
        await waitOverlay(page, "input", "(s) => s.rules === null && !s.inert", "重看中点紧急切断：规则页应立即关");
        run.eq(
            (await rec.calls()).map((c) => c[0]),
            ["panic"],
            "重看中点紧急切断：桥只收到 panic，不写位",
        );

        // ============================================================ ⑨ 电平帧不重量导览
        await openRole(page, server, "input");
        await waitOverlay(page, "input", "(s) => s.lang", "电平帧：应出语言卡");
        await clickPart(page, '[data-lang-pick="zh"]');
        await waitOverlay(page, "input", '(s) => s.rules === "gated"', "电平帧：应进规则页");
        await scrollRulesToEnd(page, "input");
        await waitOverlay(page, "input", '(s) => s.rules === "ready"', "电平帧：应放行");
        await clickPart(page, '[data-gb="input.rules"] [data-part="rules.start"]');
        await waitOverlay(page, "input", "(s) => s.tour === 1", "电平帧：应进导览");
        await press(page, "ArrowRight");
        await waitOverlay(page, "input", "(s) => s.tour === 2", "电平帧：翻到有亮区的第 2 步");
        // 每次重量都会重写遮罩的 data-spot（值不变也算一次改动），数它
        await inFrame(page, (doc, win) => {
            const mask = doc.querySelector('[data-gb="input.tour"] .rn-tour__mask');
            win.__layouts = 0;
            win.__layoutObs = new win.MutationObserver((recs) => {
                win.__layouts += recs.length;
            });
            win.__layoutObs.observe(mask, { attributes: true, attributeFilter: ["data-spot"] });
        });
        for (const gr of [-3, 0, -6, 0]) {
            await poke(page, `(m) => { m.meters.gr = ${gr}; }`);
            await sleep(page, 150);
        }
        run.eq(await inFrame(page, (doc, win) => win.__layouts), 0, "导览开着时电平帧（增益衰减变了）不重量导览");
        // 对照：快照变了要重量（量尺不空转）
        await poke(page, "(m) => { m.sampleRate = m.sampleRate === 44100 ? 48000 : 44100; }");
        await page.waitFor(`document.querySelector("iframe").contentWindow.__layouts > 0`, {
            label: "对照：快照变了，导览应重量一次",
        });
        run.eq((await stateOf(page, "input")).tour, 2, "电平帧与快照都没让导览走步或关掉");

        // ============================================================ ⑩ 选的语言没写成：叠层退回宿主的语言
        await openRole(page, server, "input");
        await page.eval(
            `(() => { const m = window.__RINNE_PREVIEW__.mock; m.setLang = async () => ({ ok: false, reason: "not-ready" }); })()`,
        );
        await waitOverlay(page, "input", "(s) => s.lang", "setLang 没写成：应出语言卡");
        {
            const hostLang = await inFrame(page, (doc) => doc.documentElement.lang);
            run.check(hostLang !== "fr", `宿主的语言不是 fr（防空转）：${hostLang}`);
            await clickPart(page, '[data-lang-pick="fr"]');
            await waitOverlay(page, "input", '(s) => s.rules === "gated"', "setLang 没写成：照常进规则页");
            const want = DICT[hostLang]["rules.title"];
            await page.waitFor(
                `document.querySelector("iframe").contentDocument.querySelector('[data-gb="input.rules"] .rn-rules__title').textContent === ${JSON.stringify(want)}`,
                { label: `setLang 没写成：规则页应退回宿主的语言（${hostLang}）` },
            );
            const red = await inFrame(page, redProbe, "input", rulesExpected(DICT[hostLang]));
            run.check(red.length === 0, `setLang 没写成：规则页整页是宿主的语言（${red.join("；")}）`);
            const d = await uiDefaults(page);
            run.check(d.langChosen !== true, `setLang 没写成：全局位没写（${JSON.stringify(d)}）`);
        }

        // ============================================================ ⑧ Output
        await openRole(page, server, "output");
        rec = await recordBridge(page, WRITES);
        await waitOverlay(page, "output", "(s) => s.lang", "Output 全新：应出语言卡");
        run.eq(await shellStateOf(page, "output"), "modal", "Output 语言卡开着：外壳是遮罩态");
        await clickPart(page, '[data-lang-pick="fr"]');
        await waitOverlay(page, "output", '(s) => s.rules === "gated"', "Output：应进规则页");
        run.eq(await shellStateOf(page, "output"), "modal", "Output 规则页开着：外壳是遮罩态");
        {
            const red = await inFrame(page, redProbe, "output", rulesExpected(DICT.fr));
            run.check(red.length === 0, `Output 规则页红字（fr）：${red.join("；")}`);
        }
        await scrollRulesToEnd(page, "output");
        await waitOverlay(page, "output", '(s) => s.rules === "ready"', "Output：滚到底放行");
        await clickPart(page, '[data-gb="output.rules"] [data-part="rules.start"]');
        for (let i = 1; i <= 4; i++) {
            await waitOverlay(page, "output", `(s) => s.tour === ${i}`, `Output 导览第 ${i} 步`);
            if (i === 1) run.eq(await shellStateOf(page, "output"), "modal", "Output 导览开着：外壳是遮罩态");
            await clickPart(page, '[data-gb="output.tour"] [data-part="tour.callout"]');
        }
        await waitOverlay(page, "output", "(s) => s.tour === null && !s.inert", "Output 导览 4 步后结束");
        run.eq(await shellStateOf(page, "output"), "normal", "Output 首启链结束：外壳回到常态");
        run.eq(
            await rec.calls(),
            [
                ["setLang", "fr"],
                ["setRulesAck", 1],
                ["setGuideSeen", true],
            ],
            "Output 写位依次是 setLang(fr)、setRulesAck(1)、setGuideSeen(true)",
        );
        d = await uiDefaults(page);
        run.check(
            d.guideSeenOutput === true && d.guideSeenInput === false,
            `Output 写的是自己的「引导已看」：${JSON.stringify(d)}`,
        );

        // Input 的导览看过：只弹规则页
        await page.eval(
            `localStorage.setItem("rinne.uiDefaults", JSON.stringify({ langChosen: true, rulesAckVersion: 0, guideSeenInput: true }))`,
        );
        await openRole(page, server, "output", { fresh: false });
        rec = await recordBridge(page, WRITES);
        await waitOverlay(page, "output", '(s) => s.rules === "gated"', "Input 导览看过：Output 只弹规则页");
        run.eq((await stateOf(page, "output")).buttons, ["ack:off"], "后面不接导览：只有 [我已了解]");
        await scrollRulesToEnd(page, "output");
        await waitOverlay(page, "output", '(s) => s.rules === "ready"', "Output：放行");
        await clickPart(page, '[data-gb="output.rules"] [data-part="rules.ack"]');
        await waitOverlay(page, "output", "(s) => s.rules === null && s.tour === null", "确认后不接导览");
        run.eq(await rec.calls(), [["setRulesAck", 1]], "Output [我已了解]：只写 setRulesAck(1)");

        // 规则页开着时配对冲突（L2）
        await openRole(page, server, "output");
        rec = await recordBridge(page, WRITES);
        await waitOverlay(page, "output", "(s) => s.lang", "Output L2：应出语言卡");
        await clickPart(page, '[data-lang-pick="zh"]');
        await waitOverlay(page, "output", '(s) => s.rules === "gated"', "Output L2：应进规则页");
        await poke(page, '(m) => { m.pair.state = "conflict"; }');
        await waitOverlay(
            page,
            "output",
            "(s) => s.rules === null && !s.inert",
            "Output 规则页开着时配对冲突：应立即关",
        );
        run.eq(
            (await rec.calls()).filter((c) => c[0] !== "setLang"),
            [],
            "Output L2 关掉：不写位",
        );

        // 恢复的实例
        {
            const undo = await markRestored(page, { hintRole: "output" });
            await openRole(page, server, "output");
            await undo();
            await sleep(page, 900);
            s = await stateOf(page, "output");
            run.check(
                !s.lang && s.rules === null && s.tour === null,
                `Output 恢复的实例：不弹链（${JSON.stringify(s)}）`,
            );
            const h = await inFrame(page, (doc) => {
                const e = doc.querySelector('[data-gb="output.footer.hint"]');
                return { state: e.getAttribute("data-state"), text: e.textContent };
            });
            run.eq(h, { state: "restored", text: DICT.zh["hint.firstUseRestored"] }, "Output 恢复的实例：页脚换提示");
            await clickPart(page, '[data-gb="output.footer.help"]');
            await waitOverlay(
                page,
                "output",
                '(s) => s.rules === "gated"',
                "Output 恢复的实例：「?」应打开要确认的规则页",
            );
            await scrollRulesToEnd(page, "output");
            await waitOverlay(page, "output", '(s) => s.rules === "ready"', "Output 恢复的实例：滚到底放行");
            await clickPart(page, '[data-gb="output.rules"] [data-part="rules.ack"]');
            await page.waitFor(
                `document.querySelector("iframe").contentDocument.querySelector('[data-gb="output.footer.hint"]').getAttribute("data-state") !== "restored"`,
                { label: "Output 确认后页脚不再是恢复提示" },
            );
            await sleep(page, 300);
            const late = await hintLogProblems(page);
            run.check(late.length === 0, `Output 恢复的实例：页脚当帧跟上快照（打开时与确认后）：${late.join("；")}`);
        }

        run.check(page.errors.length === 0, `页面不应有错误：${page.errors.slice(0, 3).join("；")}`);
    } finally {
        await browser.close();
        await server.close();
    }
    run.finish();
}
