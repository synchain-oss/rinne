// SPDX-License-Identifier: GPL-3.0-or-later
//
// Input 页面「真宿主，首批快照迟到」冒烟（无头浏览器）。页面接的是真桥（预览里的 mock 按真桥的名表装进去），
// 但 requestInitialState 只回执、不发首帧，直到测试放行：
//   ① 400 ms 兜底到点之后，页面揭幕但不编造：标签在，没有值的地方写「—」，不出现任何默认值（0%、25%、100.0……），
//      配对 pill 不声称任何状态；
//   ② 所有写入与手势都关着：五张卡、组选择、对端行里的每个控件都在 inert 之内；对它们派发指针拖动、双击、滚轮、方向键、
//      点击、Ctrl+单击，桥一次写入都收不到，读数仍是「—」，数值输入框与限制器确认卡都不弹；真鼠标点上去也一样；
//   ③ 紧急切断照常可点，真点一下桥恰好收到一次 panic；语言、关于不受影响（语言真能切，关于面板能开、高级设置那几行
//      关着）；缩放不可用：宿主的档位还不知道，选档记下的回退目标是编的，所以下拉置灰、里面只有「—」，强行塞进一个
//      真实档位并发出 change，桥也收不到 setUiScale、不出缩放确认条；快照到齐后下拉恢复，选档真调 setUiScale；
//   ④ requestInitialState 按间隔重发，次数有上界（首次加三次重发）；快照一到就不再重发；
//   ⑤ 首批真快照走正常的首帧路径：带锁存切断（界面出现「恢复回送」）、反馈单位 dB、已关闭过导出提示的快照来了，
//      界面是切断态、单位 dB、导出提示不再弹出；第一批参数回声直接落位，没有从默认值逼近的过程，也没有「自动」标记；
//      解锁后 inert 与置灰撤掉，同一套刺激这时能写到桥上（防空转）。
//   ⑥ 对照：没有关闭过导出提示的同一快照，导出提示会弹出（说明 ⑤ 里「不弹」是真的被基线压住，不是看不见）；
//   ⑦ 只来了 state、pair 还没到（场景 E）：仍算没就绪，继续锁着、刺激写不到桥；pair 补上才放行。
//   ⑧ 关于面板在 400 ms 兜底揭幕之前就打开（场景 F，用钩子把兜底定时器扣住，保证先后顺序而不靠计时）：
//      首批快照到后，高级设置那几行放开、版本号写上。
// 另：预设槽与消息位整块在 inert 之内（里面暂时没有控件也一样），页脚版本号写「—」。
// 浏览器缺失时整套跳过（退出码 2），不是通过。本机同时只能有一个浏览器冒烟。
//
// 用法：node web-preview/tests/smoke-input-slow-first-frame-page.mjs [仓库根]

import { pathToFileURL } from "node:url";
import path from "node:path";
import { createRun, repoRoot } from "./lib/harness.mjs";
import { findBrowser, launchBrowser } from "./lib/browser.mjs";
import { startStaticServer } from "./lib/static-server.mjs";
import { VIEW, centerOf, inFrame, recordBridge, settle } from "./smoke-input-kit-page.mjs";

const ROOT = repoRoot();
const run = createRun("Input 页面真宿主首批快照迟到");
if (!findBrowser()) run.skip("没有 Chromium 系浏览器");

const u = (rel) => pathToFileURL(path.join(ROOT, rel)).href;
const { GATED_WRITES } = await import(u("web/input/bridge-gate.js"));

/** 要记录的桥函数：全部写入加 panic、语言、缩放。 */
const RECORDED = [...GATED_WRITES, "panic", "setLang", "setUiScale", "requestInitialState"];
const PANIC = '[data-gb="input.panic"]';

/** 把 requestInitialState 扣住：壳页在建会话时给 mock 装上钩子（addScriptToEvaluateOnNewDocument 在壳页脚本之前跑）。 */
const HOLD_HOOK = `(() => {
    window.__hold = true;
    window.__initialCalls = 0;
    let session;
    Object.defineProperty(window, "__RINNE_PREVIEW__", {
        configurable: true,
        get() { return session; },
        set(s) {
            session = s;
            const m = s && s.mock;
            if (!m || typeof m.requestInitialState !== "function") return;
            const orig = m.requestInitialState;
            window.__origInitial = () => orig.apply(m, []);
            m.requestInitialState = function () {
                window.__initialCalls++;
                return window.__hold ? { ok: true } : orig.apply(m, arguments);
            };
        },
    });
})()`;

/**
 * 把页面的 400 ms 兜底揭幕（showPending）扣住直到放行：用它造出「快照没到、也还没揭幕」的窗口，
 * 不靠真实计时。只在 Input 页面所在的帧里装，只扣名为 showPending 的那一个定时器。
 */
const HOLD_FALLBACK_HOOK = `(() => {
    if (!location.pathname.endsWith("/web/input/index.html")) return;
    const orig = window.setTimeout;
    const held = [];
    window.__heldFallback = held;
    window.setTimeout = function (fn, ms, ...rest) {
        if (ms === 400 && typeof fn === "function" && fn.name === "showPending" && !window.__fallbackReleased) {
            held.push([fn, rest]);
            return 0;
        }
        return orig.call(window, fn, ms, ...rest);
    };
    window.__releaseFallback = () => {
        window.__fallbackReleased = true;
        for (const [fn, rest] of held.splice(0)) orig.call(window, fn, 0, ...rest);
    };
})()`;

/** 打开 Input 页面并扣住首批快照；返回时页面已建好外壳。holdFallback 为真时连兜底揭幕也扣住。 */
async function openHeld(browser, server, scenario, { holdFallback = false } = {}) {
    const page = await browser.newPage();
    await page.setViewport(VIEW.w, VIEW.h, 1);
    await page.send("Page.addScriptToEvaluateOnNewDocument", { source: HOLD_HOOK });
    if (holdFallback) await page.send("Page.addScriptToEvaluateOnNewDocument", { source: HOLD_FALLBACK_HOOK });
    const q = new URLSearchParams({ fresh: "1", auto: "0", lang: "zh", scenario });
    await page.goto(`${server.url}/web-preview/input.html?${q}`);
    await page.waitFor(
        `(() => { const s = document.querySelector(".pv-status"); return !!s && s.getAttribute("data-ok") !== "wait"; })()`,
        { timeout: 20000, label: "壳页注入" },
    );
    await page.waitFor(
        `(() => { const f = document.querySelector("iframe"); return !!(f && f.contentDocument && f.contentDocument.querySelector('[data-gb="input.shell"]')); })()`,
        { timeout: 20000, label: "页面外壳" },
    );
    return page;
}

const sleep = (page, ms) => page.eval(`new Promise((r) => setTimeout(r, ${ms}))`);

/** 壳页坐标里真点一下。 */
async function click(page, x, y) {
    const base = { x, y, button: "left", clickCount: 1, pointerType: "mouse" };
    await page.send("Input.dispatchMouseEvent", { type: "mouseMoved", x, y });
    await page.send("Input.dispatchMouseEvent", { type: "mousePressed", ...base });
    await page.send("Input.dispatchMouseEvent", { type: "mouseReleased", ...base });
    await sleep(page, 120);
}

// ---------------------------------------------------------------- 页内探针

/** 快照没到时的观测：揭幕标记、读数、标签、pill、inert 与可点性。 */
function pendingProbe(doc) {
    const q = (s) => doc.querySelector(s);
    const all = (s) => [...doc.querySelectorAll(s)];
    const text = (p) => {
        const e = q(`[data-part="${p}"]`);
        return e ? e.textContent.trim() : null;
    };
    const content = q('[data-part="content"]');
    const FREE = '[data-gb="input.lang"], [data-gb="input.footer"], .rn-brand, [data-gb="input.panic"]';
    const CTRL = 'button, [role="slider"], [role="switch"], [role="tab"], [role="radio"], select, input';
    const locked = all(CTRL).filter((e) => content.contains(e) && !e.closest(FREE));
    // 缩放下拉在页脚里但等待期是置灰的（单独断言），不算「照常可用」的那一批
    const free = all(CTRL).filter(
        (e) => (e.closest(FREE) || e.closest('[data-gb="input.panic"]')) && !e.classList.contains("rn-footer__scale"),
    );
    const eyebrows = all(".in-card .sc-eyebrow").map((e) => e.textContent.trim());
    return {
        pending: content.getAttribute("data-pending"),
        shell: !!q('[data-gb="input.shell"]'),
        readouts: {
            fb: text("fb.readout"),
            mix: text("mix.readout"),
            dly: text("dly.readout"),
            ceiling: text("ceiling.readout"),
            gr: text("gr.readout"),
            tone: all('[data-part="tone.readout"]').map((e) => e.textContent.trim()),
        },
        eyebrows,
        pill: (q('[data-gb="input.pair.pill"]') || { getAttribute: () => "missing" }).getAttribute("data-state"),
        lockedCount: locked.length,
        notInert: locked.filter((e) => !e.closest("[inert]")).map((e) => e.getAttribute("data-part") || e.className),
        freeCount: free.length,
        freeInert: free.filter((e) => e.closest("[inert]")).map((e) => e.getAttribute("data-part") || e.className),
        freeDisabled: free.filter((e) => e.disabled).map((e) => e.getAttribute("data-part") || e.className),
        panicLabel: (q('[data-gb="input.panic"]') || { textContent: "" }).textContent.trim(),
        version: (q('[data-gb="input.footer.version"]') || { textContent: "" }).textContent.trim(),
        scale: (() => {
            const sel = q(".rn-footer__scale");
            if (!sel) return null;
            return {
                disabled: sel.disabled,
                options: [...sel.options].map((o) => o.textContent.trim()),
                selected: sel.selectedOptions.length ? sel.selectedOptions[0].textContent.trim() : null,
            };
        })(),
        // 不在五张卡里、也要整块 inert 的几处：预设槽、消息位（没有控件时也得是 inert，以后往里放东西不会漏）
        slotsNotInert: ['[data-slot="preset-trigger"]', '[data-gb="input.band"]']
            .filter((s) => !q(s) || !q(s).closest("[inert]"))
            .map((s) => s),
        disabledToggles: all('[data-part$="toggle"][aria-disabled="true"]').length,
        disabledSegs: all('[data-part="fb.unit"][aria-disabled="true"], [data-part="dly.unit"][aria-disabled="true"]')
            .length,
        disabledPills: all(
            '[data-gb="input.pair"] .rn-group__pill[aria-disabled="true"], .rn-group__pill[aria-disabled="true"]',
        ).length,
        numEdit: !!q('[data-gb="input.numEdit"]'),
        limConfirm: !!q('[data-part="input.limiter.confirm"]:not([hidden])'),
        lang: doc.documentElement.lang,
        bodyText: content.textContent,
    };
}

/**
 * 对所有被关着的控件派发各种刺激：指针拖动、双击、滚轮、方向键、点击、对读数的 Ctrl+单击。
 * 合成事件不受 inert 影响，所以这一项测的是代码里的守卫，不是 inert。
 */
async function poke(doc, win) {
    const content = doc.querySelector('[data-part="content"]');
    const FREE = '[data-gb="input.lang"], [data-gb="input.footer"], .rn-brand, [data-gb="input.panic"]';
    const nodes = [
        ...doc.querySelectorAll('button, [role="slider"], [role="switch"], [role="tab"], [role="radio"]'),
    ].filter((e) => content.contains(e) && !e.closest(FREE));
    for (const e of nodes) {
        const r = e.getBoundingClientRect();
        const x = r.left + r.width / 2;
        const y = r.top + r.height / 2;
        const ptr = (type, dx, dy, extra) =>
            e.dispatchEvent(
                new win.PointerEvent(type, {
                    bubbles: true,
                    cancelable: true,
                    button: 0,
                    buttons: type === "pointerup" ? 0 : 1,
                    pointerId: 7,
                    clientX: x + dx,
                    clientY: y + dy,
                    ...extra,
                }),
            );
        ptr("pointerdown", 0, 0);
        ptr("pointermove", 6, -30);
        ptr("pointermove", 12, -60);
        ptr("pointerup", 12, -60);
        e.dispatchEvent(new win.MouseEvent("dblclick", { bubbles: true, cancelable: true, clientX: x, clientY: y }));
        e.dispatchEvent(
            new win.WheelEvent("wheel", { bubbles: true, cancelable: true, deltaY: -120, clientX: x, clientY: y }),
        );
        for (const key of ["ArrowUp", "End", "Enter"])
            e.dispatchEvent(new win.KeyboardEvent("keydown", { bubbles: true, cancelable: true, key }));
        e.click();
    }
    const readouts = doc.querySelectorAll(
        '[data-part="fb.readout"], [data-part="mix.readout"], [data-part="dly.readout"], [data-part="tone.readout"], [data-part="ceiling.readout"]',
    );
    for (const e of readouts) {
        e.dispatchEvent(
            new win.PointerEvent("pointerdown", {
                bubbles: true,
                cancelable: true,
                button: 0,
                ctrlKey: true,
                pointerId: 7,
            }),
        );
        e.dispatchEvent(new win.MouseEvent("click", { bubbles: true, cancelable: true, ctrlKey: true }));
    }
    // 超过「单击读数等双击判定」的窗口再看
    await new Promise((r) => win.setTimeout(r, 450));
    return nodes.length;
}

/** 放行之后的观测：状态、单位、切断、导出提示、各处读数。 */
function readyProbe(doc) {
    const q = (s) => doc.querySelector(s);
    const text = (p) => {
        const e = q(`[data-part="${p}"]`);
        return e ? e.textContent.trim() : null;
    };
    const content = q('[data-part="content"]');
    return {
        pending: content.getAttribute("data-pending"),
        inerts: [...content.querySelectorAll("[inert]")].length,
        pill: (q('[data-gb="input.pair.pill"]') || { getAttribute: () => null }).getAttribute("data-state"),
        unit: (q('[data-part="fb.unit"]') || { getAttribute: () => null }).getAttribute("data-state"),
        restore: !!q('[data-gb="input.band"] [data-part="band.action"][data-action="restore"]'),
        band: (q('[data-gb="input.band"]') || { textContent: "" }).textContent,
        fb: text("fb.readout"),
        mix: text("mix.readout"),
        dly: text("dly.readout"),
        knobState: (q('[data-gb="input.fb.knob"]') || { getAttribute: () => null }).getAttribute("data-state"),
        disabledAny: [...content.querySelectorAll('[aria-disabled="true"]')].length,
        lang: doc.documentElement.lang,
    };
}

/** 放行前装上的观测：记下反馈、Mix、延迟三处读数和三个旋钮的 data-state 此后出现过的全部值。 */
function installWatch(doc, win) {
    const log = { texts: { fb: [], mix: [], dly: [] }, states: [] };
    const sel = { fb: '[data-part="fb.readout"]', mix: '[data-part="mix.readout"]', dly: '[data-part="dly.readout"]' };
    const record = () => {
        for (const k of Object.keys(sel)) {
            const e = doc.querySelector(sel[k]);
            const t = e ? e.textContent.trim() : null;
            if (t !== null && log.texts[k][log.texts[k].length - 1] !== t) log.texts[k].push(t);
        }
        for (const s of ['[data-gb="input.fb.knob"]', '[data-gb="input.mix.knob"]', '[data-part="dly.slider"]']) {
            const e = doc.querySelector(s);
            const v = e ? e.getAttribute("data-state") : null;
            if (v !== null) log.states.push(v);
        }
    };
    const mo = new win.MutationObserver(record);
    mo.observe(doc.querySelector('[data-part="content"]'), {
        subtree: true,
        childList: true,
        characterData: true,
        attributes: true,
        attributeFilter: ["data-state"],
    });
    record();
    win.__watch = log;
    return true;
}

// ---------------------------------------------------------------- 场景

const server = await startStaticServer(ROOT);
const browser = await launchBrowser({ width: VIEW.w, height: VIEW.h });
try {
    // ====== 场景 A：带锁存切断、dB 单位、已关闭过导出提示、非默认参数
    {
        const page = await openHeld(browser, server, "in-panic");
        const rec = await recordBridge(page, RECORDED);
        // 400 ms 兜底到点，再留余量
        await sleep(page, 700);

        const p = await inFrame(page, pendingProbe);
        run.eq(p.pending, "1", "A：兜底到点后外壳内容层应带 data-pending");
        run.check(p.shell, "A：应有外壳");
        run.eq(p.readouts.fb, "—", "A：反馈读数应写「—」");
        run.eq(p.readouts.mix, "—", "A：Mix 读数应写「—」");
        run.eq(p.readouts.dly, "—", "A：延迟读数应写「—」");
        run.eq(p.readouts.ceiling, "—", "A：上限读数应写「—」");
        run.eq(p.readouts.gr, "—", "A：增益缩减读数应写「—」");
        run.eq(p.readouts.tone, ["—", "—"], "A：低切与高切读数应写「—」");
        run.check(
            p.eyebrows.length >= 5 && p.eyebrows.every((t) => t.length > 0),
            `A：五张卡的标题都应在，实得 ${JSON.stringify(p.eyebrows)}`,
        );
        run.check(
            !/(^|\s)(0%|25%|100\.0|20 Hz)(\s|$)/.test(p.bodyText) && !p.bodyText.includes("−6.0 dBFS"),
            `A：不应出现默认值（0%、25%、100.0、−6.0 dBFS……），实得 ${JSON.stringify(p.bodyText.slice(0, 160))}`,
        );
        run.eq(p.pill, null, "A：配对 pill 不应声称任何状态");
        run.check(p.lockedCount >= 12, `A：应有一批被关着的控件（防空转），实得 ${p.lockedCount}`);
        run.eq(p.notInert, [], "A：五张卡、组选择、对端行里的每个控件都应在 inert 之内");
        run.eq(p.slotsNotInert, [], "A：预设槽与消息位应整块在 inert 之内（即使里面暂时没有控件）");
        run.check(p.version.includes("—"), `A：页脚版本号应写「—」，实得 ${JSON.stringify(p.version)}`);
        run.eq(
            p.scale,
            { disabled: true, options: ["—"], selected: "—" },
            "A：快照没到时缩放下拉应置灰，里面只有一项「—」（宿主的档位还不知道，不编读数）",
        );
        run.eq(p.disabledToggles, 4, "A：环路、限制器、守卫、极性四个开关应都带 aria-disabled");
        run.eq(p.disabledSegs, 2, "A：反馈单位与延迟单位两个分段控件应都带 aria-disabled");
        run.eq(p.disabledPills, 8, "A：八枚组胶囊应都带 aria-disabled");
        run.check(p.freeCount >= 6, `A：语言、页脚、紧急切断里应有一批可用的控件（防空转），实得 ${p.freeCount}`);
        run.eq(p.freeInert, [], "A：语言、页脚、紧急切断不应在 inert 之内");
        run.eq(p.freeDisabled, [], "A：语言、页脚、紧急切断不应被置灰");
        run.check(p.panicLabel.length > 0, "A：紧急切断应有文字");

        // ② 刺激：合成事件
        await rec.clear();
        const poked = await inFrame(page, poke);
        run.check(poked >= 12, `A：刺激应落在一批控件上（防空转），实得 ${poked}`);
        const writes = (await rec.calls()).filter((c) => GATED_WRITES.includes(c[0]));
        run.eq(writes, [], "A：快照没到时，对所有控件的刺激不应让桥收到任何写入");
        const p2 = await inFrame(page, pendingProbe);
        run.eq(
            [p2.readouts.fb, p2.readouts.mix, p2.readouts.dly, p2.readouts.ceiling],
            ["—", "—", "—", "—"],
            "A：刺激之后读数仍应是「—」（手势没有改动显示值）",
        );
        run.eq([p2.numEdit, p2.limConfirm], [false, false], "A：刺激之后不应弹数值输入框或限制器确认卡");

        // 真鼠标点上去：反馈表盘、环路开关、组 B
        for (const sel of ['[data-part="disc"]', '[data-part="loop.toggle"]', '[data-group="B"]']) {
            const c = await centerOf(page, sel);
            await click(page, c.x, c.y);
        }
        const writes2 = (await rec.calls()).filter((c) => GATED_WRITES.includes(c[0]));
        run.eq(writes2, [], "A：真鼠标点反馈表盘、环路开关、组 B，桥不应收到写入");

        // ③ 紧急切断照常可点
        await rec.clear();
        const pc = await centerOf(page, PANIC);
        await click(page, pc.x, pc.y);
        run.eq(
            (await rec.calls()).filter((c) => c[0] === "panic").length,
            1,
            "A：快照没到时点紧急切断，桥应恰好收到一次 panic",
        );

        // ③ 语言：真点 EN，桥收到 setLang，页面换成英文
        await rec.clear();
        const en = await centerOf(page, '[data-gb="input.lang"] [data-lang="en"]');
        await click(page, en.x, en.y);
        await page.waitFor(`document.querySelector("iframe").contentDocument.documentElement.lang === "en"`, {
            label: "A：快照没到时切到 EN，页面语言应换成 en",
        });
        run.eq(
            (await rec.calls()).filter((c) => c[0] === "setLang"),
            [["setLang", "en"]],
            "A：点 EN，桥应收到 setLang('en')",
        );
        const pe = await inFrame(page, pendingProbe);
        run.check(
            pe.eyebrows.every((t) => t.length > 0) && pe.eyebrows.join("|") !== p.eyebrows.join("|"),
            `A：切到 EN 后五张卡的标题应换成英文，实得 ${JSON.stringify(pe.eyebrows)}`,
        );

        // ③ 关于：真点版本按钮，面板打开，高级设置那几行关着；再关掉
        const vb = await centerOf(page, '[data-gb="input.footer.version"]');
        await click(page, vb.x, vb.y);
        const about = await inFrame(page, (doc) => {
            const panel = doc.querySelector('[data-gb="input.about"]');
            const adv = [...doc.querySelectorAll("[data-adv], [data-tail]")];
            return { open: !!panel && !panel.hidden, adv: adv.length, enabled: adv.filter((b) => !b.disabled).length };
        });
        run.check(about.open, "A：快照没到时点版本按钮，关于面板应打开");
        run.check(about.adv >= 4, `A：关于面板应有高级设置的几行（防空转），实得 ${about.adv}`);
        run.eq(about.enabled, 0, "A：快照没到时，关于面板里写宿主的高级设置应全部置灰");
        const ab = await rec.calls();
        run.eq(
            ab.filter((c) => GATED_WRITES.includes(c[0])),
            [],
            "A：打开关于面板不应写桥",
        );
        const closeBtn = await centerOf(page, '[data-part="about.close"]');
        await click(page, closeBtn.x, closeBtn.y);

        // ③ 缩放：宿主的档位还不知道，选档要记下的回退目标是编的，所以等待期缩放不可用。
        // 下拉置灰、里面只有「—」（pendingProbe 已断言）；再把一个真实档位塞进去、强行打开并发出 change，
        // 下面几层也要挡住：桥不应收到 setUiScale、不应出现缩放确认条
        await rec.clear();
        await inFrame(page, (doc, win) => {
            const sel = doc.querySelector(".rn-footer__scale");
            const opt = doc.createElement("option");
            opt.value = "1.5";
            opt.textContent = "150%";
            sel.append(opt);
            sel.disabled = false;
            sel.value = "1.5";
            sel.dispatchEvent(new win.Event("change", { bubbles: true }));
        });
        await sleep(page, 300);
        run.eq(
            (await rec.calls()).filter((c) => c[0] === "setUiScale" || c[0] === "commitUiScale"),
            [],
            "A：快照没到时选缩放档，桥不应收到 setUiScale 或 commitUiScale",
        );
        run.eq(
            await inFrame(page, (doc) => {
                const m = doc.querySelector('[data-gb="input.scale.confirm"]');
                if (!m) return false;
                const scrim = m.closest('[data-part="scrim"]') || m;
                return !scrim.hidden;
            }),
            false,
            "A：快照没到时不应出现缩放确认条",
        );

        // ④ requestInitialState 的重发：首次加三次，不超过四次
        await sleep(page, 2200);
        const calls1 = await page.eval(`window.__initialCalls`);
        run.eq(calls1, 4, "A：快照迟迟不到，requestInitialState 应是首次加三次重发共四次");

        // ⑤ 放行：先把宿主模型摆成「切断 + dB + 已关闭过导出提示 + 非默认参数」，装上观测，再让宿主发首批快照
        await page.eval(`(() => {
            const m = window.__RINNE_PREVIEW__.model;
            m.cfg.fbUnit = "db";
            m.values.feedback = 60;
            m.values.mix = 40;
            m.values.delay_ms = 250;
            m.values.low_cut = 120;
            m.values.high_cut = 8000;
            m.exportCauses = { bits: 3, count: 3 };
            m.dismissed.add("exportMismatch");
        })()`);
        await inFrame(page, installWatch);
        await page.eval(`(() => { window.__hold = false; window.__origInitial(); })()`);
        await page.waitFor(
            `(() => { const f = document.querySelector("iframe"); const p = f.contentDocument.querySelector('[data-gb="input.pair.pill"]'); return !!p && p.hasAttribute("data-state"); })()`,
            { timeout: 8000, label: "A：放行后页面应画出首批快照" },
        );
        await settle(page);
        const r = await inFrame(page, readyProbe);
        run.eq(r.pending, null, "A：快照到齐后应撤掉 data-pending");
        run.eq(r.inerts, 0, "A：快照到齐后应撤掉 inert");
        run.eq(r.disabledAny, 0, "A：快照到齐后应撤掉所有 aria-disabled");
        run.eq(r.restore, true, "A：锁存切断的快照来了，消息位应出现「恢复回送」");
        run.eq(r.unit, "db", "A：快照里的反馈单位 dB 应被采纳（首帧路径）");
        run.check(!r.fb.endsWith("%") && r.fb !== "—", `A：反馈主读数应是 dB 值，实得 ${JSON.stringify(r.fb)}`);
        run.check(
            !r.band.includes("上次导出可能与播放不同"),
            `A：已关闭过的导出提示不应再弹，实得 ${JSON.stringify(r.band.slice(0, 80))}`,
        );

        // 观测：没有从默认值逼近，没有「自动」标记
        await sleep(page, 900);
        const w = await page.eval(`document.querySelector("iframe").contentWindow.__watch`);
        for (const k of ["fb", "mix", "dly"]) {
            const seen = w.texts[k].filter((t) => t !== "—");
            run.eq(
                seen.length,
                1,
                `A：${k} 读数在首批参数回声里应一步落位（不经过中间值），实得 ${JSON.stringify(w.texts[k])}`,
            );
        }
        run.check(
            !w.states.includes("auto"),
            `A：首批参数回声不应被当成宿主自动化（不应出现 auto 态），实得 ${JSON.stringify([...new Set(w.states)])}`,
        );

        // 防空转：放行之后同一套刺激能写到桥上
        await rec.clear();
        // 反馈表盘 Ctrl+单击会弹数值输入，先 Esc 清掉再刺激其余
        const poked2 = await inFrame(page, poke);
        run.check(poked2 >= 12, `A：放行后刺激应仍落在一批控件上，实得 ${poked2}`);
        const writes3 = (await rec.calls()).filter((c) => GATED_WRITES.includes(c[0]));
        run.check(writes3.length > 0, "A：放行后同一套刺激应能写到桥上（防空转）");

        // 放行之后缩放可用：下拉恢复成真实的档位表，选档真调 setUiScale
        await rec.clear();
        const picked = await inFrame(page, (doc, win) => {
            const sel = doc.querySelector(".rn-footer__scale");
            if (!sel || sel.disabled) return { ok: false, why: "没有下拉或被置灰" };
            const opt = [...sel.options].find((o) => !o.disabled && Number(o.value) > 0 && Number(o.value) !== 1);
            if (!opt) return { ok: false, why: "没有可选的非 100% 档" };
            sel.value = opt.value;
            sel.dispatchEvent(new win.Event("change", { bubbles: true }));
            return { ok: true, factor: Number(opt.value) };
        });
        run.check(picked.ok, `A：快照到齐后缩放下拉应可用：${picked.why || ""}`);
        if (picked.ok) {
            await sleep(page, 200);
            run.eq(
                (await rec.calls()).filter((c) => c[0] === "setUiScale"),
                [["setUiScale", picked.factor]],
                "A：快照到齐后选缩放档，桥应收到 setUiScale",
            );
            // 取消预览，免得确认条挡着后面的步骤
            await inFrame(page, (doc) => {
                const b = doc.querySelector('[data-gb="input.scale.confirm"] [data-part="confirm.cancel"]');
                if (b) b.click();
            });
        }
        run.check(page.errors.length === 0, `A：页面不应有错误：${page.errors.slice(0, 3).join("；")}`);
        await page.close();
    }

    // ====== 场景 B / C：导出提示的基线（B 已关闭过，C 对照）
    for (const [tag, dismissed] of [
        ["B", true],
        ["C", false],
    ]) {
        // 安静场景（没有别的警告抢消息位），导出原因自己摆
        const page = await openHeld(browser, server, "in-playing");
        await sleep(page, 700);
        await page.eval(`(() => {
            const m = window.__RINNE_PREVIEW__.model;
            m.exportCauses = { bits: 3, count: 3 };
            ${dismissed ? 'm.dismissed.add("exportMismatch");' : ""}
            window.__hold = false;
            window.__origInitial();
        })()`);
        await page.waitFor(
            `(() => { const f = document.querySelector("iframe"); const p = f.contentDocument.querySelector('[data-gb="input.pair.pill"]'); return !!p && p.hasAttribute("data-state"); })()`,
            { timeout: 8000, label: `${tag}：放行后页面应画出首批快照` },
        );
        await settle(page);
        const r = await inFrame(page, readyProbe);
        const shown = r.band.includes("上次导出可能与播放不同");
        if (dismissed) run.eq(shown, false, "B：已关闭过导出提示的快照，首帧之后不应再弹导出提示");
        else run.eq(shown, true, "C：没关闭过导出提示的同一快照，应弹导出提示（对照：B 里的「不弹」不是看不见）");
        await page.close();
    }

    // ====== 场景 E：只来了 state、pair 还没到：仍算没就绪，继续锁着；pair 到了才放行
    {
        const page = await openHeld(browser, server, "in-playing");
        const rec = await recordBridge(page, RECORDED);
        await sleep(page, 700);
        // 让宿主这一轮只发 pair 以外的帧：包住 core.push，丢掉 rinne.pair（周期回声也走它）
        await page.eval(`(() => {
            const core = window.__RINNE_PREVIEW__.core;
            const push = core.push;
            window.__pushOrig = push;
            core.push = (name) => (name === "rinne.pair" ? false : push.call(core, name));
            window.__hold = false;
            window.__origInitial();
        })()`);
        await sleep(page, 900);
        const p = await inFrame(page, pendingProbe);
        run.eq(p.pending, "1", "E：只有 state、没有 pair 时应仍带 data-pending（不算就绪）");
        run.eq(p.pill, null, "E：没有 pair 时配对 pill 不应声称任何状态");
        run.eq(p.notInert, [], "E：没有 pair 时各控件仍应在 inert 之内");
        await rec.clear();
        const poked = await inFrame(page, poke);
        run.check(poked >= 12, `E：刺激应落在一批控件上（防空转），实得 ${poked}`);
        run.eq(
            (await rec.calls()).filter((c) => GATED_WRITES.includes(c[0])),
            [],
            "E：没有 pair 时对所有控件的刺激不应让桥收到任何写入",
        );
        // pair 补上：放行
        await page.eval(`(() => {
            const core = window.__RINNE_PREVIEW__.core;
            core.push = window.__pushOrig;
            core.push("rinne.pair");
        })()`);
        await page.waitFor(
            `(() => { const f = document.querySelector("iframe"); const p = f.contentDocument.querySelector('[data-gb="input.pair.pill"]'); return !!p && p.hasAttribute("data-state"); })()`,
            { timeout: 8000, label: "E：pair 补上后页面应画出配对状态" },
        );
        await settle(page);
        const r = await inFrame(page, readyProbe);
        run.eq([r.pending, r.inerts, r.disabledAny], [null, 0, 0], "E：pair 补上后应撤掉 data-pending、inert 与置灰");
        run.check(page.errors.length === 0, `E：页面不应有错误：${page.errors.slice(0, 3).join("；")}`);
        await page.close();
    }

    // ====== 场景 F：关于面板在兜底揭幕之前打开；首批快照到后高级设置放开、版本写上
    {
        const page = await openHeld(browser, server, "in-playing", { holdFallback: true });
        const rec = await recordBridge(page, RECORDED);
        // 远超 400 ms：兜底被钩子扣着，页面既没有快照也没有揭幕
        await sleep(page, 700);
        const held = await inFrame(page, (doc, win) => ({
            queued: win.__heldFallback ? win.__heldFallback.length : -1,
            pending: doc.querySelector('[data-part="content"]').getAttribute("data-pending"),
        }));
        run.eq(held, { queued: 1, pending: null }, "F：兜底揭幕应被扣住（防空转：先后顺序是造出来的，不是等出来的）");
        const vb = await centerOf(page, '[data-gb="input.footer.version"]');
        await click(page, vb.x, vb.y);
        const aboutProbe = (doc) => {
            const panel = doc.querySelector('[data-gb="input.about"]');
            const adv = [...doc.querySelectorAll("[data-adv], [data-tail]")];
            return {
                open: !!panel && !panel.hidden,
                adv: adv.length,
                enabled: adv.filter((b) => !b.disabled).length,
                build: (doc.querySelector(".rn-about__build") || { textContent: "" }).textContent,
            };
        };
        const before = await inFrame(page, aboutProbe);
        run.check(before.open, "F：兜底之前点版本按钮，关于面板应打开");
        run.check(before.adv >= 4, `F：关于面板应有高级设置的几行（防空转），实得 ${before.adv}`);
        run.eq(before.enabled, 0, "F：快照没到时高级设置那几行应置灰");
        run.check(
            !before.build.includes("0.0.0-preview"),
            `F：快照没到时版本不应是真值，实得 ${JSON.stringify(before.build)}`,
        );
        // 放行首批快照，不放行兜底：面板开着，S.pending 从没置过位
        await page.eval(`(() => { window.__hold = false; window.__origInitial(); })()`);
        await page.waitFor(
            `(() => { const f = document.querySelector("iframe"); const p = f.contentDocument.querySelector('[data-gb="input.pair.pill"]'); return !!p && p.hasAttribute("data-state"); })()`,
            { timeout: 8000, label: "F：放行后页面应画出首批快照" },
        );
        await settle(page);
        const after = await inFrame(page, aboutProbe);
        run.check(after.open, "F：首批快照到后关于面板仍开着");
        run.eq(after.enabled, after.adv, "F：首批快照到后，高级设置那几行应全部放开（即使揭幕兜底从没触发过）");
        run.check(
            after.build.includes("0.0.0-preview"),
            `F：首批快照到后版本应写上宿主的值，实得 ${JSON.stringify(after.build)}`,
        );
        const left = await inFrame(page, (doc, win) => (win.__heldFallback ? win.__heldFallback.length : -1));
        run.eq(left, 1, "F：整个过程兜底揭幕一直被扣着（它没有参与这次放开）");
        const ab = await rec.calls();
        run.eq(
            ab.filter((c) => GATED_WRITES.includes(c[0])),
            [],
            "F：打开关于面板、放行快照都不应写桥",
        );
        run.check(page.errors.length === 0, `F：页面不应有错误：${page.errors.slice(0, 3).join("；")}`);
        await page.close();
    }

    // ====== 场景 D：重发能把快照拉回来；快照到了就不再重发
    {
        const page = await openHeld(browser, server, "in-playing");
        await sleep(page, 600);
        const c0 = await page.eval(`window.__initialCalls`);
        run.check(c0 >= 2, `D：到 600 ms 时应已重发过至少一次，实得共 ${c0} 次`);
        // 放行但不手动补发：下一次重发应当把首批快照拉回来
        await page.eval(`window.__hold = false`);
        await page.waitFor(
            `(() => { const f = document.querySelector("iframe"); const p = f.contentDocument.querySelector('[data-gb="input.pair.pill"]'); return !!p && p.hasAttribute("data-state"); })()`,
            { timeout: 5000, label: "D：宿主恢复后，下一次重发应拉回首批快照" },
        );
        const c1 = await page.eval(`window.__initialCalls`);
        await sleep(page, 1500);
        const c2 = await page.eval(`window.__initialCalls`);
        run.eq(c2, c1, "D：快照到了之后不应再重发 requestInitialState");
        await page.close();
    }
} finally {
    await browser.close();
    await server.close();
}
run.finish();
