// SPDX-License-Identifier: GPL-3.0-or-later
//
// 页脚缩放下拉（触发钮 + 自绘浮层）的页面冒烟（无头浏览器）：Input、Output 两个插件。
//   ① 行为（共享件夹具，两个角色）：点开 / 再点收起 / 点外面收起；键盘（触发钮上 ↑ ↓ 只展开不换档，浮层里 ↑ ↓ Home End Enter Esc Tab，
//      Esc 把焦点还给触发钮且不冒泡到页面）；超屏档灰显且点了不生效；点当前档只收起、不起防呆；选新档后确认框出现、取消回到选档前的档位；
//      禁用时打不开、开着时被禁用会收起；锁定（旧 WebKit）时只剩 1 档、打不开；展开时 data-state 是 expanded；对比度与聚焦环。
//   ② 版面（真页面：Output 夹具与 Input 预览壳页）× 中 / 英 / 法 × 设计盒的全部缩放档位：展开的浮层不越出外壳与视口、
//      每一项都点得到（elementFromPoint 命中它自己，没有被别的件压住）、文字不被截；放不下时才内部滚动。
//   ③ 真页面上的点击：Input 真鼠标点开、点一档，桥收到 setUiScale。
//   ④ 放不下时：定位几何的纯函数（翻向、压高、倍率折算、下限）；真页面上档位多到放不下时压高并内部滚动，
//      键盘走到两端焦点项总在可视范围内；贴着外壳上沿、左缘的下拉翻向下、换到左缘对齐；销毁后不再响应点外面。
// 末尾有坏样例：把浮层压到外壳之外，版面量尺必须看得出。
// 浏览器缺失时整套跳过（退出码 2）。单个冒烟有 300 秒上限。
//
// 用法：node web-preview/tests/smoke-scale-select-page.mjs [仓库根]

import { join } from "node:path";
import { pathToFileURL } from "node:url";
import { createRun, repoRoot } from "./lib/harness.mjs";
import { findBrowser, launchBrowser } from "./lib/browser.mjs";
import { startStaticServer } from "./lib/static-server.mjs";
import { LANGS, ROLES, SCENES, loadDesign, openScene } from "./shared-measure.mjs";
import { centerOf, inFrame, openInput, recordBridge, settle } from "./smoke-input-kit-page.mjs";

const ROOT = repoRoot();
const run = createRun("缩放下拉");
if (!findBrowser()) run.skip("没有 Chromium 系浏览器");

const design = await loadDesign(ROOT);
const server = await startStaticServer(ROOT);
const browser = await launchBrowser();

const WIN = SCENES.find((s) => s.id === "window");
const wrapSel = (role) => `[data-gb="${role}.footer.scale"]`;
const KEY = {
    Tab: { key: "Tab", code: "Tab", vk: 9 },
    Escape: { key: "Escape", code: "Escape", vk: 27 },
    ArrowDown: { key: "ArrowDown", code: "ArrowDown", vk: 40 },
    ArrowUp: { key: "ArrowUp", code: "ArrowUp", vk: 38 },
    Home: { key: "Home", code: "Home", vk: 36 },
    End: { key: "End", code: "End", vk: 35 },
    Space: { key: " ", code: "Space", vk: 32, text: " " },
    Enter: { key: "Enter", code: "Enter", vk: 13, text: "\r" },
};
/** mods：CDP 的修饰键位掩码（Alt 1、Ctrl 2、Meta 4、Shift 8）。 */
async function press(page, name, mods = 0) {
    const k = KEY[name];
    const base = { key: k.key, code: k.code, windowsVirtualKeyCode: k.vk, nativeVirtualKeyCode: k.vk, modifiers: mods };
    await page.send("Input.dispatchKeyEvent", {
        type: k.text ? "keyDown" : "rawKeyDown",
        ...base,
        ...(k.text ? { text: k.text } : {}),
    });
    await page.send("Input.dispatchKeyEvent", { type: "keyUp", ...base });
    await page.eval("new Promise((r) => setTimeout(r, 60))");
}
async function mouse(page, x, y) {
    const base = { x, y, button: "left", clickCount: 1, pointerType: "mouse" };
    await page.send("Input.dispatchMouseEvent", { type: "mouseMoved", x, y });
    await page.send("Input.dispatchMouseEvent", { type: "mousePressed", ...base });
    await page.send("Input.dispatchMouseEvent", { type: "mouseReleased", ...base });
    await page.eval("new Promise((r) => setTimeout(r, 120))");
}

/** 经预览壳页装载真页面（Input 用套件里的 openInput，Output 照同一套写）并等页面画出首批快照。 */
async function openShell(page, server, role, scenario, lang, scale) {
    if (role === "input") return openInput(page, server, scenario, { lang, scale });
    const q = new URLSearchParams({ fresh: "1", scenario, lang, scale: String(scale) });
    await page.setViewport(1700, 1300, 1);
    await page.goto(`${server.url}/web-preview/output.html?${q}`);
    await page.waitFor(
        `(() => { const s = document.querySelector(".pv-status"); return !!s && s.getAttribute("data-ok") !== "wait"; })()`,
        { timeout: 20000, label: "壳页注入" },
    );
    await page.waitFor(
        `(() => { const f = document.querySelector("iframe"); return !!(f && f.contentDocument && f.contentDocument.querySelector('[data-gb="output.pair.pill"][data-state]')); })()`,
        { timeout: 20000, label: "Output 页面 boot" },
    );
    await settle(page);
}

/** 夹具页上读下拉的一帧：开合、焦点所在、各项状态。 */
const READ = (role) => `(() => {
    const w = document.querySelector(${JSON.stringify(wrapSel(role))});
    const trig = w.querySelector("button.rn-footer__scale");
    const opts = [...w.querySelectorAll('[role="option"]')];
    const a = document.activeElement;
    return {
        open: w.getAttribute("data-open"),
        state: w.getAttribute("data-state"),
        expanded: trig.getAttribute("aria-expanded"),
        label: trig.textContent.trim(),
        chev: getComputedStyle(w.querySelector(".rn-footer__scaleChev")).transform,
        trigBorder: getComputedStyle(trig).borderTopColor,
        trigOpacity: getComputedStyle(trig).opacity,
        disabled: trig.disabled,
        shown: getComputedStyle(w.querySelector(".rn-scale__panel")).display !== "none",
        focus: a === trig ? "trigger" : a && a.getAttribute("role") === "option" ? "option:" + a.getAttribute("data-scale") : a ? a.tagName : null,
        opts: opts.map((o) => ({ f: o.getAttribute("data-scale"), cur: o.getAttribute("data-current"), over: o.getAttribute("data-over"), dis: o.getAttribute("aria-disabled"), sel: o.getAttribute("aria-selected"), num: o.querySelector(".rn-scale__num").textContent, note: o.querySelector(".rn-scale__note").textContent, noteHidden: o.querySelector(".rn-scale__note").hidden, chk: getComputedStyle(o.querySelector(".rn-scale__chk")).visibility })),
    };
})()`;

/** 夹具里取桥调用的记录。 */
const calls = (page, name) =>
    page.eval(`window.__fx.calls.filter((c) => c.name === ${JSON.stringify(name)}).map((c) => c.args)`);

/** 展开浮层的版面量尺（在某个文档里跑，返回可序列化的结果）。wrapSelector 是缩放根节点。 */
function layoutProbe(doc, win, wrapSelector) {
    const w = doc.querySelector(wrapSelector);
    const shell = doc.querySelector(".sc-shell").getBoundingClientRect();
    const panel = w.querySelector(".rn-scale__panel");
    const pr = panel.getBoundingClientRect();
    const trig = w.querySelector("button.rn-footer__scale").getBoundingClientRect();
    const items = [...w.querySelectorAll('[role="option"]')];
    const visible = (r) => r.top >= pr.top - 0.5 && r.bottom <= pr.bottom + 0.5;
    const rows = items.map((o) => {
        const r = o.getBoundingClientRect();
        const cx = r.left + r.width / 2;
        const cy = r.top + r.height / 2;
        const hit = visible(r) ? doc.elementFromPoint(cx, cy) : null;
        return {
            f: o.getAttribute("data-scale"),
            over: o.getAttribute("data-over") === "1",
            inView: visible(r),
            hit: hit ? hit.closest('[role="option"]') === o : null,
            clipped: o.scrollWidth > o.clientWidth + 1,
            wrapped: o.scrollHeight > o.clientHeight + 1,
            noteClipped: [...o.children].some((c) => c.scrollWidth > c.clientWidth + 1),
            w: r.width,
        };
    });
    return {
        open: w.getAttribute("data-open"),
        placement: w.getAttribute("data-placement"),
        align: w.getAttribute("data-align"),
        panel: { l: pr.left, t: pr.top, r: pr.right, b: pr.bottom, w: pr.width, h: pr.height },
        shell: { l: shell.left, t: shell.top, r: shell.right, b: shell.bottom },
        view: { w: win.innerWidth, h: win.innerHeight },
        trig: { l: trig.left, t: trig.top, r: trig.right, b: trig.bottom },
        scrollable: panel.scrollHeight > panel.clientHeight + 1,
        rows,
    };
}

/** 版面判据：返回问题清单。 */
function layoutProblems(g, { expectNoScroll }) {
    const bad = [];
    const T = 0.75;
    if (g.open !== "1") bad.push("浮层没有展开");
    const p = g.panel;
    if (p.l < g.shell.l - T || p.r > g.shell.r + T || p.t < g.shell.t - T || p.b > g.shell.b + T)
        bad.push(`浮层越出外壳 ${JSON.stringify({ panel: p, shell: g.shell })}`);
    if (p.l < -T || p.t < -T || p.r > g.view.w + T || p.b > g.view.h + T) bad.push("浮层越出视口");
    if (p.h <= 0 || p.w <= 0) bad.push("浮层没有尺寸");
    // 与触发钮不重叠（向上展开时浮层在触发钮之上）
    if (g.placement === "up" ? p.b > g.trig.t + T : p.t < g.trig.b - T) bad.push("浮层压在触发钮上");
    if (expectNoScroll && g.scrollable) bad.push("设计盒里放得下，不该需要内部滚动");
    // 锚定：浮层贴齐触发钮的一侧、与触发钮隔 9 个 CSS 像素（触发钮 CSS 高 22，用它折出倍率）
    const k = (g.trig.b - g.trig.t) / 22;
    const tol = Math.max(1.5, k * 1.2);
    const edge = g.align === "start" ? Math.abs(p.l - g.trig.l) : Math.abs(p.r - g.trig.r);
    if (edge > tol) bad.push(`浮层没有贴齐触发钮的${g.align === "start" ? "左" : "右"}缘（差 ${edge.toFixed(1)}）`);
    const gap = g.placement === "up" ? g.trig.t - p.b : p.t - g.trig.b;
    if (Math.abs(gap - 9 * k) > tol)
        bad.push(`浮层与触发钮的间距应为 9 个 CSS 像素（${(9 * k).toFixed(1)}），实得 ${gap.toFixed(1)}`);
    for (const r of g.rows) {
        if (r.inView && r.hit !== true) bad.push(`档位 ${r.f} 的中心被别的件压住（命中 ${r.hit}）`);
        if (!r.inView && !g.scrollable) bad.push(`档位 ${r.f} 不在浮层可视范围内，浮层又不能滚动`);
        if (r.clipped || r.noteClipped) bad.push(`档位 ${r.f} 的文字被截断`);
        if (r.wrapped) bad.push(`档位 ${r.f} 的文字折行了`);
    }
    return bad;
}

/** 深色底上算相对亮度对比度的小工具（页内用 computed 的 rgb 字符串）。 */
const CONTRAST = (role) => `(() => {
    const w = document.querySelector(${JSON.stringify(wrapSel(role))});
    const parse = (s) => (s.match(/[\\d.]+/g) || []).slice(0, 4).map(Number);
    const lin = (v) => { v /= 255; return v <= 0.03928 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4); };
    const lum = ([r, g, b]) => 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b);
    const ratio = (a, b) => { const x = lum(a), y = lum(b); return (Math.max(x, y) + 0.05) / (Math.min(x, y) + 0.05); };
    const panel = w.querySelector(".rn-scale__panel");
    const bg = parse(getComputedStyle(panel).backgroundColor);
    const out = [];
    for (const o of w.querySelectorAll('[role="option"]')) {
        for (const part of [o.querySelector(".rn-scale__num"), o.querySelector(".rn-scale__note")]) {
            if (!part || !part.textContent.trim()) continue;
            out.push({ f: o.getAttribute("data-scale"), over: o.getAttribute("data-over"), what: part.className, ratio: ratio(parse(getComputedStyle(part).color), bg), alpha: bg[3] === undefined ? 1 : bg[3] });
        }
    }
    return out;
})()`;

try {
    // ================================================================ ① 行为（共享件夹具）
    for (const role of ROLES) {
        const tag = role;
        const page = await browser.newPage();
        await page.send("Emulation.setFocusEmulationEnabled", { enabled: true });
        const box = design[role];
        const W = wrapSel(role);
        const trigSel = `${W} button.rn-footer__scale`;
        const open = async () => {
            await page.eval(`document.querySelector(${JSON.stringify(trigSel)}).click()`);
        };
        const optSel = (f) => `${W} [role="option"][data-scale="${f}"]`;
        const confirmShown = () =>
            page.eval(
                `(() => { const d = document.querySelector('[data-gb="${role}.scale.confirm"]'); return Boolean(d) && !d.closest("[hidden]"); })()`,
            );
        // 屏幕 1000×700：设计盒 × 档位 + 宿主框放不下的档位灰显（判据与 scale-ui.js 同一条，这里按设计盒独立算一遍）
        const AVAIL = { w: 1000, h: 700 };
        const over = (f) => box.w * f + design.hostFrame.w > AVAIL.w || box.h * f + design.hostFrame.h > AVAIL.h;
        const presets = box.presets;
        const overF = presets.filter(over);
        const okF = presets.filter((f) => !over(f));
        run.check(overF.length >= 1 && okF.length >= 3, `${tag}：测试屏幕上应既有放不下的档位也有放得下的（防空转）`);

        await openScene(page, server, design, role, "zh", WIN, "&screen=1000x700");

        // ---- 结构与初态
        let r = await page.eval(READ(role));
        run.eq(
            [r.open, r.state, r.expanded, r.shown, r.focus === "trigger"],
            ["0", "collapsed", "false", false, false],
            `${tag}：初态收起（data-open 0、data-state collapsed、aria-expanded false、浮层不显示）`,
        );
        run.eq(r.opts.length, presets.length, `${tag}：档位项数等于设计盒的档位数`);
        run.eq(
            await page.eval(
                `(() => { const t = document.querySelector(${JSON.stringify(trigSel)}); const p = document.querySelector(${JSON.stringify(W)} + " .rn-scale__panel"); return [t.tagName, t.getAttribute("aria-haspopup"), p.getAttribute("role"), (p.getAttribute("aria-label") || "").length > 0, (t.getAttribute("aria-label") || "").length > 0]; })()`,
            ),
            ["BUTTON", "listbox", "listbox", true, true],
            `${tag}：触发钮是 button、aria-haspopup=listbox，浮层 role=listbox 且都带名称`,
        );
        run.eq(
            r.opts.filter((o) => o.cur === "1").map((o) => o.f),
            ["1"],
            `${tag}：当前档是 100%`,
        );
        run.eq(
            r.opts.filter((o) => o.over === "1").map((o) => Number(o.f)),
            overF,
            `${tag}：灰显的档位正是放不下 1000×700 屏幕的那几档`,
        );
        run.eq(
            r.opts.filter((o) => o.chk === "visible").map((o) => o.f),
            ["1"],
            `${tag}：只有当前档的勾号可见`,
        );
        run.eq(r.chev, "none", `${tag}：收起时触发钮的箭头不旋转`);
        const closedBorder = r.trigBorder;
        run.eq(
            r.opts.filter((o) => o.sel === "true").map((o) => o.f),
            ["1"],
            `${tag}：只有当前档 aria-selected 为 true`,
        );
        run.eq(
            r.opts.filter((o) => o.dis === "true").map((o) => Number(o.f)),
            overF,
            `${tag}：只有超屏档带 aria-disabled=true`,
        );
        run.check(
            r.opts.filter((o) => o.over === "1").every((o) => o.note !== "") &&
                r.opts.every((o) => (o.note === "") === o.noteHidden),
            `${tag}：超屏档都有注记并显示，别的档没有注记、那一格隐藏`,
        );
        // 切语言：注记跟着换（词条函数经 setT 换掉）
        {
            const notes = () =>
                page.eval(
                    `[...document.querySelectorAll(${JSON.stringify(W + " .rn-scale__note")})].filter((n) => !n.hidden).map((n) => n.textContent)`,
                );
            const zhNotes = await notes();
            const enText = await page.eval(`window.__fx.api.makeT("en")("common.scale.overflow")`);
            await page.eval(`window.__fx.api.footerUpdate({ t: window.__fx.api.makeT("en") })`);
            run.eq(
                await notes(),
                overF.map(() => enText),
                `${tag}：切到英文后注记换成英文词条`,
            );
            run.check(zhNotes.length === overF.length && zhNotes[0] !== enText, `${tag}：（防空转）中英文注记不同`);
            await page.eval(`window.__fx.api.footerUpdate({})`);
            run.eq(await notes(), zhNotes, `${tag}：切回中文后注记换回来`);
        }

        // ---- 点开、再点收起（真鼠标）
        let c = await page.eval(
            `(() => { const b = document.querySelector(${JSON.stringify(trigSel)}).getBoundingClientRect(); return { x: b.x + b.width / 2, y: b.y + b.height / 2 }; })()`,
        );
        await mouse(page, c.x, c.y);
        r = await page.eval(READ(role));
        run.eq(
            [r.open, r.state, r.expanded, r.shown],
            ["1", "expanded", "true", true],
            `${tag}：点触发钮展开（data-state 变 expanded）`,
        );
        run.check(
            r.chev !== "none" && r.trigBorder !== closedBorder,
            `${tag}：展开时箭头旋转、触发钮描边换成强调色（${r.chev}；${closedBorder} → ${r.trigBorder}）`,
        );
        run.eq(r.focus, "option:1", `${tag}：展开时焦点落在当前档`);
        await mouse(page, c.x, c.y);
        r = await page.eval(READ(role));
        run.eq(
            [r.open, r.state, r.shown, r.focus],
            ["0", "collapsed", false, "trigger"],
            `${tag}：再点触发钮收起，焦点回触发钮`,
        );

        // ---- 点外面收起（点在标题栏空白处）；点浮层里的空隙不收起
        await mouse(page, c.x, c.y);
        run.eq((await page.eval(READ(role))).open, "1", `${tag}：（准备）再次展开`);
        await mouse(page, 8, 8);
        r = await page.eval(READ(role));
        run.eq([r.open, r.state], ["0", "collapsed"], `${tag}：点外面收起`);
        run.eq((await calls(page, "setUiScale")).length, 0, `${tag}：以上开合都没有触发 setUiScale`);

        // ---- 键盘：触发钮上 ↑ ↓ 只展开不换档
        for (const k of ["ArrowDown", "ArrowUp"]) {
            await page.eval(`document.querySelector(${JSON.stringify(trigSel)}).focus()`);
            await press(page, k);
            r = await page.eval(READ(role));
            run.eq(
                [r.open, r.focus, r.label],
                ["1", "option:1", "100%"],
                `${tag}：触发钮上按 ${k} 只展开（焦点落当前档、读数不变）`,
            );
            run.eq((await calls(page, "setUiScale")).length, 0, `${tag}：触发钮上按 ${k} 不换档`);
            await press(page, "Escape");
            run.eq((await page.eval(READ(role))).open, "0", `${tag}：（准备）收起`);
        }
        // 带 Ctrl 的方向键是宿主与系统的地盘：触发钮上不展开，浮层里不移焦点
        await page.eval(`document.querySelector(${JSON.stringify(trigSel)}).focus()`);
        await press(page, "ArrowDown", 2);
        run.eq((await page.eval(READ(role))).open, "0", `${tag}：触发钮上按 Ctrl+↓ 放行，不展开`);
        await open();
        await press(page, "ArrowDown", 2);
        run.eq((await page.eval(READ(role))).focus, "option:1", `${tag}：浮层里按 Ctrl+↓ 放行，焦点不动`);
        await press(page, "Escape");
        run.eq((await page.eval(READ(role))).open, "0", `${tag}：（准备）收起`);

        // ---- 键盘：浮层里 ↑ ↓ Home End
        await page.eval(`document.querySelector(${JSON.stringify(trigSel)}).focus()`);
        await press(page, "ArrowDown");
        const idx = presets.indexOf(1);
        await press(page, "ArrowDown");
        r = await page.eval(READ(role));
        run.eq(r.focus, "option:" + presets[idx + 1], `${tag}：浮层里 ↓ 焦点下移一项`);
        await press(page, "ArrowUp");
        await press(page, "ArrowUp");
        r = await page.eval(READ(role));
        run.eq(r.focus, "option:" + presets[idx - 1], `${tag}：浮层里 ↑ 焦点上移一项`);
        await press(page, "Home");
        run.eq((await page.eval(READ(role))).focus, "option:" + presets[0], `${tag}：Home 到第一项`);
        await press(page, "ArrowUp");
        run.eq((await page.eval(READ(role))).focus, "option:" + presets[0], `${tag}：第一项再按 ↑ 不越界`);
        await press(page, "End");
        const lastF = presets[presets.length - 1];
        r = await page.eval(READ(role));
        run.eq(r.focus, "option:" + lastF, `${tag}：End 到最后一项（超屏档也能停留）`);
        run.eq(r.opts[r.opts.length - 1].over, "1", `${tag}：最后一项是灰显的超屏档（防空转）`);
        await press(page, "ArrowDown");
        run.eq((await page.eval(READ(role))).focus, "option:" + lastF, `${tag}：最后一项再按 ↓ 不越界`);
        // 聚焦环：键盘走到的项画 2px 的描边
        const ring = await page.eval(
            `(() => { const a = document.activeElement; const cs = getComputedStyle(a); return [cs.outlineStyle !== "none", parseFloat(cs.outlineWidth)]; })()`,
        );
        run.eq(ring, [true, 2], `${tag}：键盘聚焦的档位项有 2px 聚焦环`);

        // ---- Enter 选灰显的超屏档：收起、焦点回触发钮、不生效
        await press(page, "Enter");
        r = await page.eval(READ(role));
        run.eq([r.open, r.focus], ["0", "trigger"], `${tag}：Enter 选超屏档后收起、焦点回触发钮`);
        run.eq((await calls(page, "setUiScale")).length, 0, `${tag}：选超屏档不生效（桥没收到 setUiScale）`);
        run.eq(await confirmShown(), false, `${tag}：选超屏档不起确认框`);
        run.eq(r.label, "100%", `${tag}：选超屏档后读数不变`);

        // ---- 真鼠标点超屏档、点当前档
        await mouse(page, c.x, c.y);
        const rect = (f) =>
            page.eval(
                `(() => { const b = document.querySelector(${JSON.stringify(optSel(f))}).getBoundingClientRect(); return { x: b.x + b.width / 2, y: b.y + b.height / 2 }; })()`,
            );
        const o1 = await rect(overF[0]);
        await mouse(page, o1.x, o1.y);
        r = await page.eval(READ(role));
        run.eq(r.open, "0", `${tag}：鼠标点超屏档后收起`);
        run.eq(
            [(await calls(page, "setUiScale")).length, await confirmShown()],
            [0, false],
            `${tag}：鼠标点超屏档不生效、不起确认框`,
        );
        await mouse(page, c.x, c.y);
        const o2 = await rect(1);
        await mouse(page, o2.x, o2.y);
        r = await page.eval(READ(role));
        run.eq(r.open, "0", `${tag}：点当前档后收起`);
        run.eq(
            [(await calls(page, "setUiScale")).length, await confirmShown()],
            [0, false],
            `${tag}：点当前档只收起，不发预览、不起防呆`,
        );

        // ---- Esc：收起、焦点回触发钮、不冒泡到页面
        await page.eval(
            `window.__esc = 0; document.addEventListener("keydown", (e) => { if (e.key === "Escape") window.__esc++; });`,
        );
        await open();
        await press(page, "Escape");
        r = await page.eval(READ(role));
        run.eq(
            [r.open, r.focus, await page.eval("window.__esc")],
            ["0", "trigger", 0],
            `${tag}：浮层里 Esc 收起、焦点回触发钮、Esc 不冒泡到页面`,
        );
        await press(page, "Escape");
        run.eq(await page.eval("window.__esc"), 1, `${tag}：（防空转）浮层收起后 Esc 照常冒泡`);

        // ---- Tab 离开即收起
        await open();
        await press(page, "Tab");
        await page.eval("new Promise((r) => setTimeout(r, 60))");
        r = await page.eval(READ(role));
        run.eq(r.open, "0", `${tag}：Tab 离开浮层即收起`);
        run.eq((await calls(page, "setUiScale")).length, 0, `${tag}：Tab 离开不换档`);

        // ---- 处理过的键默认动作被拦（不让页面或浮层自己滚动、不触发按钮原生激活）；Tab 不拦
        const prevented = (key) =>
            page.eval(
                `(() => { const ev = new KeyboardEvent("keydown", { key: ${JSON.stringify(key)}, bubbles: true, cancelable: true }); document.activeElement.dispatchEvent(ev); return ev.defaultPrevented; })()`,
            );
        const got = {};
        await page.eval(`document.querySelector(${JSON.stringify(trigSel)}).focus()`);
        got.triggerArrowDown = await prevented("ArrowDown");
        for (const k of ["ArrowDown", "ArrowUp", "Home", "End"]) got[k] = await prevented(k);
        got.Space = await prevented(" ");
        await open();
        got.Enter = await prevented("Enter");
        await open();
        got.Escape = await prevented("Escape");
        await open();
        got.Tab = await prevented("Tab");
        await press(page, "Escape");
        run.eq(
            got,
            {
                triggerArrowDown: true,
                ArrowDown: true,
                ArrowUp: true,
                Home: true,
                End: true,
                Space: true,
                Enter: true,
                Escape: true,
                Tab: false,
            },
            `${tag}：处理过的键默认动作被拦，Tab 不拦`,
        );

        // ---- 选新档：键盘 Enter，确认框出现，取消回到选档前
        await page.eval(`document.querySelector(${JSON.stringify(trigSel)}).focus()`);
        await press(page, "ArrowDown");
        const pick = okF.find((f) => f !== 1);
        // 从当前档（100%）走到 pick：按顺序逐项移动
        const steps = presets.indexOf(pick) - presets.indexOf(1);
        for (let i = 0; i < Math.abs(steps); i++) await press(page, steps > 0 ? "ArrowDown" : "ArrowUp");
        run.eq(
            (await page.eval(READ(role))).focus,
            "option:" + pick,
            `${tag}：（准备）焦点走到 ${Math.round(pick * 100)}%`,
        );
        // Input 用 Enter、Output 用 Space 选档（两种键各验一遍；Space 的原生激活落在 keyup，浮层里要自己接，不能把刚收起的浮层又点开）
        await press(page, role === "input" ? "Enter" : "Space");
        await page.waitFor(`window.__fx.calls.some((x) => x.name === "setUiScale")`, {
            label: `${tag}：选档应调 setUiScale`,
        });
        run.eq(await calls(page, "setUiScale"), [[pick]], `${tag}：选新档调一次 setUiScale(${pick})`);
        r = await page.eval(READ(role));
        run.eq([r.open, r.focus.startsWith("option")], ["0", false], `${tag}：选档后浮层收起`);
        run.eq(await confirmShown(), true, `${tag}：选新档后出确认框（10 秒防呆照旧）`);
        const cd = await page.eval(`document.querySelector('[data-gb="${role}.scale.confirm"]').textContent`);
        // 读到的数可能因机器忙已经走掉几秒，只要落在 1…10 就说明起点是 10 秒（起点被改成 30 会判红）
        const secs = Number((cd.match(/(\d+)\s*秒/) || [])[1]);
        run.check(secs >= 1 && secs <= 10, `${tag}：确认框里在倒数，起点不超过 10 秒，实得 ${JSON.stringify(cd)}`);
        await page.eval(
            `document.querySelector('[data-gb="${role}.scale.confirm"] [data-part="confirm.cancel"]').click()`,
        );
        await page.waitFor(`window.__fx.calls.filter((x) => x.name === "setUiScale").length === 2`, {
            label: `${tag}：取消应把档位调回去`,
        });
        run.eq(
            (await calls(page, "setUiScale")).map((a) => a[0]),
            [pick, 1],
            `${tag}：取消回到选档前的 100%`,
        );
        run.eq(await confirmShown(), false, `${tag}：取消后确认框收起`);

        // ---- 对比度（浮层是不透明实底，直接按 computed 颜色算）
        await open();
        // 在下拉里按下鼠标不挪焦点：mousedown 的默认动作被拦（合成事件读 defaultPrevented）
        run.eq(
            await page.eval(
                `[${JSON.stringify(optSel(1))}, ${JSON.stringify(trigSel)}].map((s) => { const ev = new MouseEvent("mousedown", { bubbles: true, cancelable: true }); document.querySelector(s).dispatchEvent(ev); return ev.defaultPrevented; })`,
            ),
            [true, true],
            `${tag}：在档位项与触发钮上按下鼠标，默认动作被拦（焦点不挪）`,
        );
        const cr = await page.eval(CONTRAST(role));
        run.check(cr.length >= presets.length, `${tag}：对比度量到了每一项（防空转），实得 ${cr.length}`);
        const worst = cr.reduce((a, b) => (b.ratio < a.ratio ? b : a));
        run.check(worst.ratio >= 4.5, `${tag}：浮层里文字对比度应 ≥ 4.5，最差 ${JSON.stringify(worst)}`);
        run.check(
            cr.every((x) => x.alpha === 1),
            `${tag}：浮层底色应不透明（算对比度的前提）`,
        );
        {
            const hover = async (f) => {
                const p = await rect(f);
                await page.send("Input.dispatchMouseEvent", { type: "mouseMoved", x: p.x, y: p.y });
                await page.eval("new Promise((r) => setTimeout(r, 80))");
                return page.eval(
                    `(() => { const cs = getComputedStyle(document.querySelector(${JSON.stringify(optSel(f))})); return [cs.backgroundColor, cs.cursor]; })()`,
                );
            };
            const okPick = okF.find((f) => f !== 1);
            const hOk = await hover(okPick);
            run.check(
                hOk[0] !== "rgba(0, 0, 0, 0)" && hOk[1] === "pointer",
                `${tag}：悬停在可选档上有底色、光标是手形，实得 ${JSON.stringify(hOk)}`,
            );
            const hOver = await hover(overF[0]);
            run.check(
                hOver[0] === "rgba(0, 0, 0, 0)" && hOver[1] === "not-allowed",
                `${tag}：悬停在超屏档上没有底色、光标是禁止，实得 ${JSON.stringify(hOver)}`,
            );
        }
        await press(page, "Escape");

        // ---- 展开时被禁用会收起；禁用时打不开
        await open();
        run.eq((await page.eval(READ(role))).open, "1", `${tag}：（准备）展开`);
        await page.eval(`window.__fx.api.footerUpdate({ locked: true })`);
        r = await page.eval(READ(role));
        run.eq(
            [r.open, r.state, r.disabled],
            ["0", "macLocked", true],
            `${tag}：展开时被锁定会收起，data-state 为 macLocked`,
        );
        run.check(Number(r.trigOpacity) < 1, `${tag}：锁定时触发钮置灰（opacity ${r.trigOpacity}）`);
        await open();
        await page.eval(`document.querySelector(${JSON.stringify(trigSel)}).focus()`);
        await press(page, "ArrowDown");
        await mouse(page, c.x, c.y);
        run.eq((await page.eval(READ(role))).open, "0", `${tag}：锁定时点击、方向键都打不开`);
        await page.eval(`window.__fx.api.footerUpdate({ locked: false })`);
        // 页面直接写 disabled（首帧前）：开着时被写上也要收起
        await open();
        run.eq((await page.eval(READ(role))).open, "1", `${tag}：（准备）解锁后能再次展开`);
        await page.eval(`document.querySelector(${JSON.stringify(trigSel)}).disabled = true`);
        await page.eval("new Promise((r) => setTimeout(r, 60))");
        run.eq((await page.eval(READ(role))).open, "0", `${tag}：页面直接把触发钮置灰时，开着的浮层收起`);
        await page.eval(`document.querySelector(${JSON.stringify(trigSel)}).disabled = false`);
        run.check(page.errors.length === 0, `${tag}：页面不应有错误：${page.errors.slice(0, 3).join("；")}`);
        await page.close();
    }

    // ---- 锁定（旧 WebKit：rect 不含 zoom）：只剩 1 档、打不开
    const LEGACY_HOOK = `(() => {
        const real = Element.prototype.getBoundingClientRect;
        Element.prototype.getBoundingClientRect = function () {
            return this.style.zoom === "2" ? { width: this.offsetWidth } : real.call(this);
        };
    })()`;
    for (const role of ROLES) {
        const tag = `${role}（旧 WebKit 锁定）`;
        const page = await browser.newPage();
        await page.send("Emulation.setFocusEmulationEnabled", { enabled: true });
        await page.send("Page.addScriptToEvaluateOnNewDocument", { source: LEGACY_HOOK });
        await openScene(page, server, design, role, "zh", WIN);
        await page.eval(`window.__fx.api.footerUpdate({ locked: true })`); // 页面每秒会重发状态：这一次带上锁定后的选项表
        const r = await page.eval(READ(role));
        run.eq(
            r.opts.map((o) => o.f),
            ["1"],
            `${tag}：只剩 100% 一档`,
        );
        run.eq(
            [r.state, r.disabled, r.label],
            ["macLocked", true, "100%"],
            `${tag}：data-state 为 macLocked，触发钮置灰，读数 100%`,
        );
        const c = await page.eval(
            `(() => { const b = document.querySelector(${JSON.stringify(wrapSel(role) + " button.rn-footer__scale")}).getBoundingClientRect(); return { x: b.x + b.width / 2, y: b.y + b.height / 2 }; })()`,
        );
        await mouse(page, c.x, c.y);
        run.eq((await page.eval(READ(role))).open, "0", `${tag}：点不开`);
        run.eq((await calls(page, "setUiScale")).length, 0, `${tag}：没有 setUiScale`);
        await page.close();
    }

    // ================================================================ ② 版面：真页面 × 三语 × 全部档位
    // 真页面经预览壳页装载（?scale= 让壳页把 iframe 撑成设计盒 × 档位，页面自己的 shell-fit 按视口缩放）。
    // 屏幕可用区压到 1000×700：大档位灰显，浮层里出现最宽的那种行（勾号加数字加「超出当前屏幕」注记）；Input 档位 2 时当前档本身也是超屏档
    const layoutSeen = { output: 0, input: 0 };
    const SCREEN_HOOK = `(() => {
        Object.defineProperty(Screen.prototype, "availWidth", { configurable: true, get: () => 1000 });
        Object.defineProperty(Screen.prototype, "availHeight", { configurable: true, get: () => 700 });
    })()`;
    const SCENARIO = { input: "in-playing", output: "out-connected" };
    for (const role of ROLES) {
        const page = await browser.newPage();
        await page.send("Page.addScriptToEvaluateOnNewDocument", { source: SCREEN_HOOK });
        for (const lang of LANGS) {
            for (const z of design[role].presets) {
                await openShell(page, server, role, SCENARIO[role], lang, z);
                const where = `${role}/${lang}/${z} 倍`;
                const zoom = await inFrame(page, (doc) => Number(doc.querySelector(".rn-shell").style.zoom || "1"));
                run.check(Math.abs(zoom - z) < 0.01, `${where}：外壳应缩放到这一档，实得 ${zoom}`);
                await inFrame(
                    page,
                    (doc, win, sel) => doc.querySelector(sel + " button.rn-footer__scale").click(),
                    wrapSel(role),
                );
                const g = await inFrame(page, layoutProbe, wrapSel(role));
                run.eq(layoutProblems(g, { expectNoScroll: true }), [], `${where}：展开的浮层版面`);
                layoutSeen[role]++;
                if (lang === "fr" && z === design[role].presets[design[role].presets.length - 1]) {
                    const notes = await inFrame(
                        page,
                        (doc, win, sel) =>
                            [...doc.querySelectorAll(sel + " .rn-scale__note")]
                                .filter((n) => !n.hidden)
                                .map((n) => n.textContent),
                        wrapSel(role),
                    );
                    run.check(
                        notes.length >= 1,
                        `${where}：真页面上应有超屏注记（防空转），实得 ${JSON.stringify(notes)}`,
                    );
                }
            }
        }
        if (role === "output") {
            // 坏样例：把浮层推到外壳之外，版面判据必须判红
            await openShell(page, server, role, SCENARIO[role], "zh", 1);
            await inFrame(
                page,
                (doc, win, sel) => doc.querySelector(sel + " button.rn-footer__scale").click(),
                wrapSel(role),
            );
            await inFrame(
                page,
                (doc, win, sel) => {
                    doc.querySelector(sel + " .rn-scale__panel").style.right = "-260px";
                },
                wrapSel(role),
            );
            const gBad = await inFrame(page, layoutProbe, wrapSel(role));
            run.check(
                layoutProblems(gBad, { expectNoScroll: true }).some((x) => x.includes("越出")),
                "坏样例：浮层推出外壳，版面判据应判红",
            );
        }
        run.check(page.errors.length === 0, `${role} 版面：页面不应有错误：${page.errors.slice(0, 3).join("；")}`);
        await page.close();
    }
    run.eq(
        layoutSeen,
        { output: LANGS.length * design.output.presets.length, input: LANGS.length * design.input.presets.length },
        "版面量尺：每个插件、每种语言、每个档位都量到了",
    );

    // ================================================================ ③ Input 真页面上的点击
    {
        const page = await browser.newPage();
        await openInput(page, server, "in-playing", { lang: "zh" });
        const rec = await recordBridge(page, ["setUiScale", "commitUiScale"]);
        await rec.clear();
        const t = await centerOf(page, ".rn-footer__scale");
        await mouse(page, t.x, t.y);
        run.eq(
            await inFrame(page, (doc) =>
                doc.querySelector('[data-gb="input.footer.scale"]').getAttribute("data-state"),
            ),
            "expanded",
            "Input 真页面：点触发钮展开，data-state 为 expanded",
        );
        run.eq((await rec.calls()).length, 0, "Input 真页面：展开不调桥");
        const cur = await inFrame(page, (doc) =>
            doc.querySelector('[data-gb="input.footer.scale"] [data-current="1"]').getAttribute("data-scale"),
        );
        const want = design.input.presets.find((f) => String(f) !== cur && f <= 1);
        const o = await centerOf(page, `[data-gb="input.footer.scale"] [role="option"][data-scale="${want}"]`);
        await mouse(page, o.x, o.y);
        run.eq(
            (await rec.calls()).filter((x) => x[0] === "setUiScale"),
            [["setUiScale", want]],
            "Input 真页面：点一档，桥收到 setUiScale",
        );
        run.eq(
            await inFrame(page, (doc) => doc.querySelector('[data-gb="input.footer.scale"]').getAttribute("data-open")),
            "0",
            "Input 真页面：选档后浮层收起",
        );
        await inFrame(page, (doc) => {
            const b = doc.querySelector('[data-gb="input.scale.confirm"] [data-part="confirm.cancel"]');
            if (b) b.click();
        });
        run.check(page.errors.length === 0, `Input 真页面：页面不应有错误：${page.errors.slice(0, 3).join("；")}`);
        await page.close();
    }

    // ================================================================ ④ 放不下时
    // ---- 定位几何（纯函数，不进浏览器）：往哪边开、压不压高度
    const { choosePlacement, measuredZoom } = await import(
        pathToFileURL(join(ROOT, "web/shared/scale-select.js")).href
    );
    {
        const T = (top, bottom) => ({ top, bottom });
        const C = (top, bottom) => ({ top, bottom });
        const cases = [
            [
                "放得下就用默认方向（向上）",
                ["up", T(300, 322), C(0, 364), 200, 1],
                { placement: "up", maxHeight: null },
            ],
            [
                "默认向上放不下、向下放得下：翻到下面",
                ["up", T(60, 82), C(0, 364), 200, 1],
                { placement: "down", maxHeight: null },
            ],
            [
                "默认向下放得下：保持向下",
                ["down", T(60, 82), C(0, 364), 200, 1],
                { placement: "down", maxHeight: null },
            ],
            [
                "默认向下放不下、向上放得下：翻到上面",
                ["down", T(300, 322), C(0, 364), 200, 1],
                { placement: "up", maxHeight: null },
            ],
            [
                "两边都放不下：取更宽的上边，高度压到那边的空间",
                ["up", T(100, 122), C(0, 200), 200, 1],
                { placement: "up", maxHeight: 87 },
            ],
            [
                "两边都放不下：取更宽的下边，高度压到那边的空间",
                ["up", T(40, 62), C(0, 200), 200, 1],
                { placement: "down", maxHeight: 125 },
            ],
            ["压高不低于下限 72", ["up", T(30, 52), C(0, 90), 200, 1], { placement: "down", maxHeight: 72 }],
            [
                "两边一样宽且都放不下：保持默认方向（向上）",
                ["up", T(100, 122), C(0, 222), 200, 1],
                { placement: "up", maxHeight: 87 },
            ],
            [
                "两边一样宽且都放不下：保持默认方向（向下）",
                ["down", T(100, 122), C(0, 222), 200, 1],
                { placement: "down", maxHeight: 87 },
            ],
            [
                "倍率 2：空间折成 CSS 像素再比",
                ["up", T(200, 222), C(0, 364), 100, 2],
                { placement: "up", maxHeight: 87 },
            ],
            ["同一几何倍率 1 就放得下", ["up", T(200, 222), C(0, 364), 100, 1], { placement: "up", maxHeight: null }],
            [
                "非法倍率（NaN）按 1 处理",
                ["up", T(200, 222), C(0, 364), 100, NaN],
                { placement: "up", maxHeight: null },
            ],
            [
                "非法倍率（负数）按 1 处理",
                ["up", T(200, 222), C(0, 364), 100, -1],
                { placement: "up", maxHeight: null },
            ],
            [
                "非法的默认方向按向上（两边都放得下才分得出）",
                ["sideways", T(150, 172), C(0, 364), 100, 1],
                { placement: "up", maxHeight: null },
            ],
            [
                "自然高度不是数时按 0 处理（放得下）",
                ["up", T(300, 322), C(0, 364), NaN, 1],
                { placement: "up", maxHeight: null },
            ],
            [
                "刚好放得下（需要 = 空间）：不压",
                ["up", T(300, 322), C(0, 364), 287, 1],
                { placement: "up", maxHeight: null },
            ],
            ["差一点放不下：压高", ["up", T(300, 322), C(0, 364), 288, 1], { placement: "up", maxHeight: 287 }],
            [
                "另一边刚好放得下（需要 = 空间）：翻过去且不压",
                ["up", T(60, 82), C(0, 364), 269, 1],
                { placement: "down", maxHeight: null },
            ],
            ["空间带小数：压高向下取整", ["up", T(100.5, 122), C(0, 200), 200, 1], { placement: "up", maxHeight: 87 }],
            [
                "裁切框上沿不在 0：上方空间从裁切框上沿起算",
                ["up", T(300, 322), C(100, 364), 200, 1],
                { placement: "up", maxHeight: 187 },
            ],
            [
                "裁切框下沿在触发钮之下 300：向下空间从裁切框下沿起算",
                ["down", T(60, 82), C(0, 300), 200, 1],
                { placement: "down", maxHeight: null },
            ],
        ];
        for (const [name, args, want] of cases) run.eq(choosePlacement(...args), want, `定位几何：${name}`);

        const fake = (h, rh) => ({ offsetHeight: h, getBoundingClientRect: () => ({ height: rh }) });
        run.eq(measuredZoom(fake(22, 44)), 2, "量倍率：rect 高是 offsetHeight 的 2 倍就是 2");
        run.eq(measuredZoom(fake(22, 11)), 0.5, "量倍率：rect 高是 offsetHeight 的一半就是 0.5");
        run.eq(measuredZoom(fake(22, 22)), 1, "量倍率：相等就是 1（旧 WebKit 的锁定档也走这一支）");
        run.eq(measuredZoom(null), 1, "量倍率：没有节点按 1");
        run.eq(measuredZoom({ offsetHeight: 22 }), 1, "量倍率：节点没有 getBoundingClientRect 按 1");
        run.eq(measuredZoom(fake(0, 44)), 1, "量倍率：offsetHeight 为 0（未布局）按 1");
        run.eq(measuredZoom(fake(22, 0)), 1, "量倍率：rect 高为 0 按 1");
        run.eq(measuredZoom(fake(22, Infinity)), 1, "量倍率：量出无穷大按 1");
    }

    // ---- 真页面（共享件夹具）：档位多到放不下；翻向与换边；销毁
    for (const role of ROLES) {
        const tag = `${role}（放不下）`;
        let r;
        const page = await browser.newPage();
        await page.send("Emulation.setFocusEmulationEnabled", { enabled: true });
        // 屏幕 1000×700：标准档位表里有灰显的超屏档，行里就带注记（挤窄时折行的判据要有注记才有意义）
        await openScene(page, server, design, role, "zh", WIN, "&screen=1000x700");
        const W = wrapSel(role);
        const trigSel = `${W} button.rn-footer__scale`;
        const panelSel = `${W} .rn-scale__panel`;
        const probeAt = (sel) => page.eval(`(${layoutProbe.toString()})(document, window, ${JSON.stringify(sel)})`);
        const clickTrigger = () => page.eval(`document.querySelector(${JSON.stringify(trigSel)}).click()`);
        const many = Array.from({ length: 30 }, (_, i) => {
            const f = (i + 1) / 10;
            const over = i >= 28;
            return {
                factor: f,
                label: Math.round(f * 100) + "%",
                disabled: over,
                titleKey: over ? "common.scale.overflow" : null,
            };
        });

        // 三十档：默认向上放不下，向下也没有空间：压高并内部滚动
        await page.eval(`window.__fx.api.footerUpdate({ scaleOptions: ${JSON.stringify(many)}, scale: 1 })`);
        await clickTrigger();
        let g = await probeAt(W);
        const mh = await page.eval(`document.querySelector(${JSON.stringify(panelSel)}).style.maxHeight`);
        run.check(/^\d+px$/.test(mh), `${tag}：三十档放不下时应压 max-height，实得 ${JSON.stringify(mh)}`);
        run.check(g.scrollable, `${tag}：压高后浮层内部可滚动`);
        run.eq(g.placement, "up", `${tag}：向下没有空间时仍向上`);
        run.check(
            g.panel.t - g.shell.t >= 0 && g.panel.t - g.shell.t <= 7,
            `${tag}：压高后浮层占满上方可用空间（顶缘离外壳上沿 ≤ 7px），实得 ${g.panel.t - g.shell.t}`,
        );
        run.eq(
            layoutProblems(g, { expectNoScroll: false }),
            [],
            `${tag}：压高后浮层仍在外壳与视口内，看得见的档位都点得到`,
        );
        run.eq((await page.eval(READ(role))).focus, "option:1", `${tag}：展开时焦点落在当前档`);
        // 键盘走到两端：焦点项总会被滚进可视范围
        await press(page, "End");
        g = await probeAt(W);
        const lastRow = g.rows[g.rows.length - 1];
        run.check(lastRow.inView && lastRow.hit === true, `${tag}：End 之后最后一项滚进可视范围且点得到`);
        run.check(
            (await page.eval(`document.querySelector(${JSON.stringify(panelSel)}).scrollTop`)) > 0,
            `${tag}：End 之后浮层确实滚动了`,
        );
        run.check(
            await page.eval(
                `(() => { const p = document.querySelector(${JSON.stringify(panelSel)}); return Math.abs(p.scrollTop + p.clientHeight - p.scrollHeight) <= 1; })()`,
            ),
            `${tag}：End 之后滚到最底（连同下内边距）`,
        );
        await press(page, "Home");
        g = await probeAt(W);
        run.check(g.rows[0].inView && g.rows[0].hit === true, `${tag}：Home 之后第一项滚回可视范围且点得到`);
        run.eq(
            await page.eval(`document.querySelector(${JSON.stringify(panelSel)}).scrollTop`),
            0,
            `${tag}：Home 之后滚回顶部`,
        );
        // 逐项向下走到底：每一步焦点项都在可视范围内
        let seenHidden = 0;
        for (let i = 0; i < many.length; i++) {
            await press(page, "ArrowDown");
            const gi = await probeAt(W);
            const cur = await page.eval(`(document.activeElement.getAttribute("data-scale") || "")`);
            const row = gi.rows.find((x) => x.f === cur);
            if (!row || !row.inView) seenHidden++;
        }
        run.eq(seenHidden, 0, `${tag}：逐项 ↓ 时焦点项始终在可视范围内`);
        let seenHiddenUp = 0;
        for (let i = 0; i < many.length; i++) {
            await press(page, "ArrowUp");
            const gi = await probeAt(W);
            const cur = await page.eval(`(document.activeElement.getAttribute("data-scale") || "")`);
            const row = gi.rows.find((x) => x.f === cur);
            if (!row || !row.inView) seenHiddenUp++;
        }
        run.eq(seenHiddenUp, 0, `${tag}：逐项 ↑ 时焦点项始终在可视范围内`);
        // 超屏档（最后两档）同样会被 titleKey 的词条标注
        run.eq(
            (await page.eval(READ(role))).opts.filter((o) => o.note !== "").map((o) => o.f),
            ["2.9", "3"],
            `${tag}：带 titleKey 的灰显档才有注记`,
        );
        await press(page, "Escape");
        // 外壳整体缩放到 0.5：空间要按 CSS 像素折算，压高后的浮层同样占满上方可用空间、不越出外壳
        await page.eval(`document.querySelector(".rn-shell").style.zoom = "0.5"`);
        await clickTrigger();
        g = await probeAt(W);
        run.check(
            g.scrollable && g.panel.t - g.shell.t >= 0 && g.panel.t - g.shell.t <= 3.5,
            `${tag}：外壳倍率 0.5：压高后浮层占满上方可用空间（≤ 3.5px），实得 ${g.panel.t - g.shell.t}`,
        );
        run.eq(layoutProblems(g, { expectNoScroll: false }), [], `${tag}：外壳倍率 0.5：压高后浮层在外壳与视口内`);
        await press(page, "Escape");
        await page.eval(`document.querySelector(".rn-shell").style.zoom = ""`);

        // 档位表变了而浮层开着：重建后焦点还在原来那一档，浮层重新定位
        await page.eval(`window.__fx.api.footerUpdate({ scale: 1 })`); // 回到标准档位表
        await clickTrigger();
        g = await probeAt(W);
        run.eq(
            [await page.eval(`document.querySelector(${JSON.stringify(panelSel)}).style.maxHeight`), g.scrollable],
            ["", false],
            `${tag}：回到标准档位表再展开：max-height 清掉、不再滚动`,
        );
        // 焦点放在非当前档的 50%（标准档位表与三十档表里都有这一档）
        await page.eval(`document.querySelector(${JSON.stringify(W + ' [role="option"][data-scale="0.5"]')}).focus()`);
        const focusedBefore = (await page.eval(READ(role))).focus;
        run.eq(focusedBefore, "option:0.5", `${tag}：（准备）焦点在 50% 一档`);
        await page.eval(
            `window.__keep = document.querySelector(${JSON.stringify(W + ' [role="option"][data-scale="0.5"]')})`,
        );
        await page.eval(`window.__fx.api.footerUpdate({ scale: 1 })`);
        await page.eval(`window.__fx.api.footerUpdate({ scale: 1 })`);
        run.check(await page.eval(`window.__keep.isConnected`), `${tag}：档位表没变就不重建（元素还在文档里）`);
        run.eq((await page.eval(READ(role))).focus, focusedBefore, `${tag}：档位表没变，开着时焦点不动`);
        await page.eval(`window.__fx.api.footerUpdate({ scaleOptions: ${JSON.stringify(many)}, scale: 1 })`);
        r = await page.eval(READ(role));
        run.eq([r.open, r.opts.length], ["1", many.length], `${tag}：开着时换档位表：浮层仍开、档位项重建`);
        run.eq(r.focus, focusedBefore, `${tag}：开着时换档位表：焦点回到原来那一档`);
        run.check(
            /^\d+px$/.test(await page.eval(`document.querySelector(${JSON.stringify(panelSel)}).style.maxHeight`)),
            `${tag}：开着时换档位表：重新定位并压高`,
        );
        // 档位表被清空：浮层收起，读数没有可显示的档位时写破折号，再点也打不开
        // 焦点放在触发钮上（它在下拉内，focusout 不会替它收起）：清空是否收起只能靠 setOptions 自己
        await page.eval(`document.querySelector(${JSON.stringify(trigSel)}).focus()`);
        run.eq((await page.eval(READ(role))).open, "1", `${tag}：（准备）焦点在触发钮上、浮层仍开着`);
        await page.eval(`window.__fx.api.footerUpdate({ scaleOptions: [], scale: 0 })`);
        r = await page.eval(READ(role));
        run.eq([r.open, r.label, r.opts.length], ["0", "—", 0], `${tag}：档位表清空：浮层收起，读数为破折号`);
        await clickTrigger();
        run.eq((await page.eval(READ(role))).open, "0", `${tag}：没有档位时点触发钮打不开`);
        await page.eval(`window.__fx.api.footerUpdate({ scale: 1 })`);
        await page.eval(`window.__fx.api.footerUpdate({ scale: 0.9 })`);
        r = await page.eval(READ(role));
        run.eq(
            [r.label, r.opts.filter((o) => o.cur === "1").length],
            ["90%", 0],
            `${tag}：当前档不在档位表里：读数按百分比写、没有档位打勾`,
        );
        await page.eval(`window.__fx.api.footerUpdate({ scale: 1 })`);

        // 浮层被外力挤窄：行内文字不折行（折行会把 24 高的行撑乱，宁可被截，由版面判据抓住）
        await clickTrigger();
        await page.eval(
            `(() => { const p = document.querySelector(${JSON.stringify(panelSel)}); p.style.minWidth = "0"; p.style.width = "40px"; })()`,
        );
        const gs = await probeAt(W);
        run.check(
            gs.rows.some((x) => x.clipped || x.noteClipped),
            `${tag}：（防空转）挤窄到 40px 后确有文字被截`,
        );
        run.eq(
            gs.rows.filter((x) => x.wrapped).map((x) => x.f),
            [],
            `${tag}：挤窄到 40px 后没有哪一行折行`,
        );
        await page.eval(
            `(() => { const p = document.querySelector(${JSON.stringify(panelSel)}); p.style.minWidth = ""; p.style.width = ""; })()`,
        );
        await press(page, "Escape");

        // 翻向与换边：在外壳里临时放几个同款下拉（同一份 CSS、同一个模块），贴着上沿、左缘、右缘
        const flip = async (name, css, o, want, extra = {}) => {
            const sel = `[data-probe="${name}"]`;
            await page.eval(`(async () => {
                const m = await import(location.origin + "/web/shared/scale-select.js");
                const shell = document.querySelector(".sc-shell");
                const box = ${JSON.stringify(extra.box || "")};
                let host = shell;
                if (box) {
                    host = document.createElement("div");
                    host.setAttribute("data-probe-box", ${JSON.stringify(name)});
                    host.style.cssText = box;
                    shell.append(host);
                }
                const wrap = document.createElement("div");
                wrap.className = "rn-footer__scaleWrap rn-scale";
                wrap.setAttribute("data-probe", ${JSON.stringify(name)});
                wrap.style.cssText = ${JSON.stringify(css)};
                const trigger = document.createElement("button");
                trigger.type = "button";
                trigger.className = "rn-footer__scale";
                const lab = document.createElement("span");
                trigger.append(lab);
                const panel = document.createElement("div");
                panel.className = "rn-scale__panel";
                wrap.append(trigger, panel);
                host.append(wrap);
                const sel = m.createScaleSelect({ wrap, trigger, panel, label: lab, onPick() {}, ...${JSON.stringify(o)} });
                sel.setOptions([0.5, 0.75, 1, 1.25, 1.5, 2].slice(0, ${extra.count || 6}).map((f) => ({ factor: f, label: Math.round(f * 100) + "%", disabled: false, titleKey: null })));
                sel.setValue(1);
                sel.setOpen(true);
                (window.__probes = window.__probes || {})[${JSON.stringify(name)}] = sel;
            })()`);
            const gp = await probeAt(sel);
            run.eq([gp.placement, gp.align], want, `${tag}：${name} 的展开方向与对齐`);
            run.eq(
                layoutProblems(gp, { expectNoScroll: true }),
                [],
                `${tag}：${name} 展开后在外壳内、不压触发钮、每项点得到`,
            );
            if (extra.box) {
                const inBox = await page.eval(
                    `(() => { const c = document.querySelector('[data-probe-box=' + JSON.stringify(${JSON.stringify(name)}) + ']').getBoundingClientRect(); const p = document.querySelector(${JSON.stringify(sel)} + ' .rn-scale__panel').getBoundingClientRect(); return p.left >= c.left - 0.75 && p.right <= c.right + 0.75 && p.top >= c.top - 0.75 && p.bottom <= c.bottom + 0.75; })()`,
                );
                run.eq(inBox, true, `${tag}：${name} 展开后在容器的裁切框内`);
            }
            await page.eval(
                `(() => { const s = window.__probes[${JSON.stringify(name)}]; s.destroy(); document.querySelector(${JSON.stringify(sel)}).remove(); const b = document.querySelector('[data-probe-box=' + JSON.stringify(${JSON.stringify(name)}) + ']'); if (b) b.remove(); })()`,
            );
        };
        await flip("贴上沿（默认向上放不下）", "position:absolute;top:6px;right:24px;", {}, ["down", "end"]);
        await flip("贴左缘（右缘对齐会越出左边）", "position:absolute;top:6px;left:4px;", {}, ["down", "start"]);
        await flip("默认向下、贴下沿", "position:absolute;bottom:6px;right:24px;", { placement: "down" }, [
            "up",
            "end",
        ]);
        await flip("左缘对齐、贴右缘（会越出右边）", "position:absolute;bottom:6px;right:4px;", { align: "start" }, [
            "up",
            "end",
        ]);
        await flip("默认方向放得下就不翻", "position:absolute;bottom:60px;right:24px;", {}, ["up", "end"]);
        // 裁切框取最近的裁切祖先而不是整个视口：外壳里有个 overflow:hidden 的小容器，触发钮贴着容器上沿、左缘
        await flip("容器里贴上沿、左缘（空间按容器算）", "position:absolute;top:4px;left:4px;", {}, ["down", "start"], {
            box: "position:absolute;overflow:hidden;left:100px;top:130px;width:300px;height:200px;",
            count: 3,
        });

        // 点外面收起的文档级监听：用合成的 pointerdown 单独验（真鼠标点外面会同时让焦点离开，由 focusout 那一路先收起，分不出是哪条在起作用）
        const synth = (sel) =>
            page.eval(
                `document.querySelector(${JSON.stringify(sel)}).dispatchEvent(new PointerEvent("pointerdown", { bubbles: true }))`,
            );
        await clickTrigger();
        run.eq((await page.eval(READ(role))).open, "1", `${tag}：（准备）展开`);
        await synth(panelSel);
        run.eq((await page.eval(READ(role))).open, "1", `${tag}：在浮层里按下指针不收起`);
        await synth(trigSel);
        run.eq((await page.eval(READ(role))).open, "1", `${tag}：在触发钮上按下指针不由文档监听收起（收起归点击）`);
        await synth("body");
        run.eq((await page.eval(READ(role))).open, "0", `${tag}：在外面按下指针收起`);
        await synth("body");
        run.eq((await page.eval(READ(role))).open, "0", `${tag}：已收起时外面再按下指针无事`);

        // 销毁：摘掉文档级监听，在外面按下指针不再收起
        await clickTrigger();
        run.eq((await page.eval(READ(role))).open, "1", `${tag}：（准备）再展开`);
        await page.eval(`window.__fx.api.footer().destroy()`);
        await synth("body");
        run.eq((await page.eval(READ(role))).open, "1", `${tag}：销毁后在外面按下指针不再收起（文档监听已摘）`);
        await page.eval(`document.querySelector(${JSON.stringify(trigSel)}).disabled = true`);
        await page.eval("new Promise((r) => setTimeout(r, 60))");
        run.eq((await page.eval(READ(role))).open, "1", `${tag}：销毁后触发钮置灰不再收起浮层（属性监听已摘）`);
        run.check(page.errors.length === 0, `${tag}：页面不应有错误：${page.errors.slice(0, 3).join("；")}`);
        await page.close();
    }
} finally {
    await browser.close();
    await server.close();
}

run.finish();
