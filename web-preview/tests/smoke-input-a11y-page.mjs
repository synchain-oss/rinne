// SPDX-License-Identifier: GPL-3.0-or-later
//
// Input 页面可访问性冒烟（无头浏览器），真源页面经预览壳页装载。
//   ① 名称：页上每个可交互件都有非空的可访问名称（aria-labelledby > aria-label > 文字 > title）；
//      40 个 in-* 场景（含首启的 8 个）的中文页全量审计，英文与法文页审计最有代表性的几个场景，首启的画面三语都审计；
//      预设面板的八个子态（场景表只有两个）逐语言审计；
//      对话框、列表框、原生下拉、输入框的名称只能由作者给；
//   ② 角色与状态：滑块（反馈量、Mix、低切、高切、延迟、上限）有 valuemin / valuemax / valuenow / valuetext 且取值合理，
//      开关有 aria-checked，单位分段是 tablist，电平管是 meter（-60 到 0），紧急切断是有名称的按钮，
//      限制器关闭确认是 alertdialog 且两颗按钮有名称；
//   ③ 键盘：反馈滑块的方向键 1 步、PgUp / PgDn 10 步、Home 到 0、End 在限制器开着时停在 100%（闸），Enter 进入数值输入、Esc 退出且不改值；
//      紧急切断用键盘（空格与回车）能触发；Tab 走一圈每一站都有看得见的焦点环；
//   ④ 坏样例：去掉滑块的名称、关掉焦点环、把 valuemax 改小，上面各项必须各自判红（量尺本身可信）。
//   ⑤ 首启叠层（语言卡、规则页、导览各步，三语）：叠层是带名称的模态对话框（role=dialog、aria-modal、aria-labelledby 指到有字的节点）、
//      叠层打开时内容层 inert、键盘焦点在叠层之内；叠层里每颗按钮有名称，规则页未放行时两颗按钮真禁用并带 title、放行后可用，
//      导览第 1 步的「上一步」禁用；规则页正文区能用键盘到达；紧急切断在三种叠层下都不在 inert 里、有名称、能聚焦，
//      Tab 走一圈每一站只落在叠层或紧急切断上、且有看得见的焦点环；用键盘（空格）在三种叠层下触发紧急切断，桥收到一次 panic、叠层关掉。
//      已在 smoke-first-run-page 断言过的不重复：Esc 的去留、按钮放行的时机、红字、紧急切断的命中点与导览中真点切断。
//      坏样例：去掉叠层名称、去掉内容层 inert、把焦点移出叠层、把规则页按钮改成可用、让紧急切断落进 inert、关掉叠层里的焦点环，各项必须判红。
// 浏览器缺失时整套跳过（退出码 2），不是通过。本机同时只能有一个浏览器冒烟。
//
// 用法：node web-preview/tests/smoke-input-a11y-page.mjs [仓库根]

import { createRun, repoRoot } from "./lib/harness.mjs";
import { findBrowser, launchBrowser } from "./lib/browser.mjs";
import { startStaticServer } from "./lib/static-server.mjs";
import {
    FRAMES,
    LANGS,
    PRESET_STATES,
    SCENES,
    VIEW,
    frameTag,
    inFrame,
    isFirstRunScene,
    openFrame,
    openInput,
    openPresetState,
    overlayProbeFailures,
    recordBridge,
} from "./smoke-input-kit-page.mjs";
import { overlayA11yProbe, overlayProblems } from "../fixtures/first-run/measure.mjs";

const ROOT = repoRoot();
const run = createRun("Input 页面可访问性");
/** 导览共几步（场景表里 in-tour-N 的个数）：最后一步的按钮是「完成」而不是「下一步」。 */
const TOUR_STEPS = SCENES.filter((s) => /^in-tour-[0-9]+$/.test(s.id)).length;
if (!findBrowser()) run.skip("没有 Chromium 系浏览器");

/** 页内名称审计：返回 { interactive:N, missing:[描述] }。 */
function nameAudit(doc, win) {
    const shell = doc.querySelector('[data-gb="input.shell"]');
    const shown = (e) => {
        for (let n = e; n && n !== doc.documentElement; n = n.parentElement) {
            if (n.hidden) return false;
            const cs = win.getComputedStyle(n);
            if (cs.display === "none" || cs.visibility === "hidden") return false;
        }
        return true;
    };
    const nameOf = (e) => {
        const by = e.getAttribute("aria-labelledby");
        if (by) {
            const txt = by
                .split(/\s+/)
                .map((id) => (doc.getElementById(id) || {}).textContent || "")
                .join(" ")
                .trim();
            if (txt) return txt;
        }
        const al = (e.getAttribute("aria-label") || "").trim();
        if (al) return al;
        // 原生下拉的名称只认作者给的（上面两种，或关联的 label），不取 title：title 是悬停说明，
        // 且时值下拉只在三连音与附点档才有，别的档位会没有名字；也不取选项文字（那是当前值，不是名称）
        if (e.tagName === "SELECT")
            return Array.from(e.labels || [])
                .map((l) => (l.textContent || "").trim())
                .join(" ")
                .trim();
        // 滑块、电平管、对话框与分组容器的名称只能由作者给（aria-label 或 aria-labelledby），不取里面的文字
        const authorOnly = /^(slider|meter|dialog|alertdialog|tablist|radiogroup|listbox)$/.test(
            e.getAttribute("role") || "",
        );
        const tc = (e.textContent || "").trim();
        if (tc && !authorOnly) return tc;
        return (e.getAttribute("title") || "").trim();
    };
    const SEL =
        'button, select, input, a[href], [role="button"], [role="radio"], [role="tab"], [role="switch"], [role="link"], [role="checkbox"], [role="slider"], [role="meter"], [role="dialog"], [role="alertdialog"], [role="listbox"], [role="option"], [tabindex="0"]';
    const missing = [];
    let n = 0;
    for (const e of shell.querySelectorAll(SEL)) {
        if (!shown(e)) continue;
        n++;
        if (!nameOf(e))
            missing.push(`${e.getAttribute("data-gb") || e.getAttribute("data-part") || e.className || e.tagName}`);
    }
    return { interactive: n, missing };
}

/** 页内角色与状态审计：返回 { sliders:[...], switches:[...], meters:[...], tabs, panic }。 */
function roleAudit(doc, win) {
    const shell = doc.querySelector('[data-gb="input.shell"]');
    const num = (e, a) => (e.hasAttribute(a) ? Number(e.getAttribute(a)) : NaN);
    const shown = (e) => {
        for (let n = e; n && n !== doc.documentElement; n = n.parentElement) {
            if (n.hidden) return false;
            const cs = win.getComputedStyle(n);
            if (cs.display === "none" || cs.visibility === "hidden") return false;
        }
        return true;
    };
    const all = (sel) => [...shell.querySelectorAll(sel)].filter(shown);
    const sliders = all('[role="slider"]').map((e) => ({
        name: e.getAttribute("aria-label") || "",
        min: num(e, "aria-valuemin"),
        max: num(e, "aria-valuemax"),
        now: num(e, "aria-valuenow"),
        text: e.getAttribute("aria-valuetext") || "",
        tab: e.tabIndex,
    }));
    const switches = all('[role="switch"]').map((e) => ({
        name: e.getAttribute("aria-label") || e.textContent.trim(),
        checked: e.getAttribute("aria-checked"),
    }));
    const meters = all('[role="meter"]').map((e) => ({
        name: e.getAttribute("aria-label") || "",
        min: num(e, "aria-valuemin"),
        max: num(e, "aria-valuemax"),
        now: num(e, "aria-valuenow"),
        text: e.getAttribute("aria-valuetext") || "",
    }));
    const tablists = all('[role="tablist"]').map((e) => ({
        name: e.getAttribute("aria-label") || "",
        tabs: e.querySelectorAll('[role="tab"]').length,
    }));
    const panic = shell.querySelector('[data-gb="input.panic"]');
    return {
        sliders,
        switches,
        meters,
        tablists,
        panic: panic
            ? { tag: panic.tagName, name: panic.getAttribute("aria-label") || panic.textContent.trim() }
            : null,
    };
}

const KEY = {
    Tab: { key: "Tab", code: "Tab", vk: 9 },
    Escape: { key: "Escape", code: "Escape", vk: 27 },
    ArrowUp: { key: "ArrowUp", code: "ArrowUp", vk: 38 },
    ArrowDown: { key: "ArrowDown", code: "ArrowDown", vk: 40 },
    PageUp: { key: "PageUp", code: "PageUp", vk: 33 },
    PageDown: { key: "PageDown", code: "PageDown", vk: 34 },
    Home: { key: "Home", code: "Home", vk: 36 },
    End: { key: "End", code: "End", vk: 35 },
    Space: { key: " ", code: "Space", vk: 32, text: " " },
    Enter: { key: "Enter", code: "Enter", vk: 13, text: "\r" },
};
async function press(page, name) {
    const k = KEY[name];
    const base = { key: k.key, code: k.code, windowsVirtualKeyCode: k.vk, nativeVirtualKeyCode: k.vk };
    await page.send("Input.dispatchKeyEvent", {
        type: k.text ? "keyDown" : "rawKeyDown",
        ...base,
        ...(k.text ? { text: k.text } : {}),
    });
    await page.send("Input.dispatchKeyEvent", { type: "keyUp", ...base });
    await page.eval("new Promise((r) => setTimeout(r, 50))");
}

/** 把键盘焦点放进页面 iframe 里，并清掉元素焦点（Tab 从文档开头走）。 */
const focusFrame = (page) =>
    inFrame(page, (doc, win) => {
        win.focus();
        if (doc.activeElement && doc.activeElement !== doc.body) doc.activeElement.blur();
        // 顺序焦点导航的起点是上一次聚焦的节点，blur 不会重置它：首启叠层打开时焦点在叠层里，Tab 会从那里往后走、
        // 漏掉文档里排在叠层前面的件（紧急切断就在前面）。把起点放回文档开头，Tab 一圈才是完整的一圈
        doc.body.tabIndex = -1;
        doc.body.focus();
        doc.body.removeAttribute("tabindex");
    });

/** 焦点落在页面里指定选择器的元素上。 */
const focusOn = (page, selector) =>
    inFrame(
        page,
        (doc, win, sel) => {
            win.focus();
            const e = doc.querySelector(sel);
            if (!e) return false;
            e.focus();
            return doc.activeElement === e;
        },
        selector,
    );

/** 页内：把焦点放到紧急切断上，返回它的角色、名称、是否在 inert 里、能否聚焦。 */
function panicProbe(doc) {
    const p = doc.querySelector('[data-gb="input.panic"]');
    if (!p) return null;
    p.focus();
    return {
        tag: p.tagName,
        name: (p.getAttribute("aria-label") || p.textContent || "").trim(),
        inert: Boolean(p.closest("[inert]")),
        disabled: p.disabled,
        focused: doc.activeElement === p,
    };
}

/** 紧急切断在叠层之下的问题清单，空表示都对：是有名称的按钮、不在 inert 里、没禁用、聚焦得上。 */
const panicProblems = (p) =>
    !p
        ? ["页面里没有紧急切断"]
        : [
              p.tag !== "BUTTON" && `应是 button，实得 ${p.tag}`,
              !p.name && "没有可访问名称",
              p.inert && "落在 inert 里",
              p.disabled && "被禁用",
              !p.focused && "聚焦不上",
          ].filter(Boolean);

const FB = '[data-gb="input.fb.knob"] [role="slider"]';
const readNow = (page) =>
    inFrame(page, (doc, win, sel) => Number(doc.querySelector(sel).getAttribute("aria-valuenow")), FB);

/** 设置反馈值（经桥），等页面回声到位。键盘与滚轮的手势停手 300 ms 才结束，结束之前页面会丢弃回声帧，所以先等它收尾。 */
async function setFeedback(page, v) {
    await page.eval("new Promise((r) => setTimeout(r, 700))");
    await page.eval(`window.__RINNE_PREVIEW__.mock.setParam("feedback", ${v})`);
    await page.waitFor(
        `(() => { const e = document.querySelector("iframe").contentDocument.querySelector('${FB}'); return !!e && Number(e.getAttribute("aria-valuenow")) === ${v}; })()`,
        { label: `反馈量应回声到 ${v}` },
    );
}

/** Tab 走一圈，返回每一站 { id, ring }。ring 只认 outline：焦点环的实现就是 outline。 */
async function tabWalk(page) {
    await focusFrame(page);
    const tabbable = await inFrame(
        page,
        (doc) =>
            [
                ...doc
                    .querySelector('[data-gb="input.shell"]')
                    .querySelectorAll('button, select, input, a[href], [tabindex="0"]'),
            ].filter(
                (e) =>
                    !e.disabled &&
                    !e.hidden &&
                    !e.closest("[inert]") &&
                    e.tabIndex >= 0 &&
                    e.getClientRects().length > 0,
            ).length,
    );
    const stops = [];
    for (let i = 0; i < tabbable; i++) {
        await press(page, "Tab");
        const f = await inFrame(page, (doc, win) => {
            const a = doc.activeElement;
            if (!a || a === doc.body) return null;
            // 焦点环按设计画在旋钮盘、卡箍、滑块头上：元件自己或它的后代有 outline 都算看得见
            const outlined = (e) => {
                const cs = win.getComputedStyle(e);
                return cs.outlineStyle !== "none" && parseFloat(cs.outlineWidth) > 0;
            };
            const ring = outlined(a) || [...a.querySelectorAll("*")].some(outlined);
            return {
                id: `${a.getAttribute("data-gb") || a.getAttribute("data-part") || a.className || a.tagName}`,
                ring,
                // 首启叠层打开时，每一站只该落在叠层里或紧急切断上
                inOverlay: Boolean(
                    a.closest('[data-gb="input.langStart"], [data-gb="input.rules"], [data-gb="input.tour"]'),
                ),
                panic: a.matches('[data-gb="input.panic"]'),
            };
        });
        if (!f) break;
        stops.push(f);
    }
    return { tabbable, stops };
}

const server = await startStaticServer(ROOT);
const browser = await launchBrowser({ width: VIEW.w, height: VIEW.h });
try {
    const page = await browser.newPage();
    // 无头页默认没有焦点，键盘事件会落空；强制页面处于有焦点状态
    await page.send("Emulation.setFocusEmulationEnabled", { enabled: true });

    // ---- ① 名称审计：中文全量，英、法文看代表场景
    const REPRESENTATIVE = [
        "in-playing",
        "in-limiter-confirm",
        "in-about",
        "in-fr-worst",
        "in-conflict",
        "in-takeover-confirm",
    ];
    let audited = 0;
    for (const lang of LANGS) {
        for (const frame of FRAMES) {
            const { scene } = frame;
            if (lang !== "zh" && !REPRESENTATIVE.includes(scene.id) && !isFirstRunScene(scene)) continue;
            const tag = frameTag(frame, lang);
            await openFrame(page, server, frame, { lang });
            const audit = await inFrame(page, nameAudit);
            audited++;
            run.eq(
                await page.eval(`document.querySelector("iframe").contentDocument.documentElement.lang`),
                lang,
                `${tag}：<html lang> 应等于界面语言`,
            );
            run.check(audit.interactive >= 1, `${tag}：应有可交互件（防空转），实得 ${audit.interactive}`);
            run.eq(audit.missing, [], `${tag}：这些可交互件没有可访问名称`);
            if (scene.id === "in-playing")
                run.check(audit.interactive >= 30, `${tag}：整窗可交互件应不少于 30 个，实得 ${audit.interactive}`);
        }
    }
    run.log(`  名称审计共 ${audited} 页`);
    // 中文全量；英、法文审计代表场景，加上首启的 9 个画面（8 个场景加规则页放行后一个）三语都审计
    run.eq(
        audited,
        FRAMES.length + (LANGS.length - 1) * (REPRESENTATIVE.length + 9),
        "名称审计的页数：中文全量，英、法文为代表场景加首启画面",
    );

    // 预设面板的子态：逐语言逐子态摆出来审计；面板、列表框、条目、输入框、各按钮都要有名称，下拉也要
    let presetAudited = 0;
    for (const lang of LANGS) {
        for (const state of PRESET_STATES) {
            const tag = `预设面板 ${state}/${lang}`;
            await openInput(page, server, "in-playing", { lang });
            await openPresetState(page, state);
            const audit = await inFrame(page, nameAudit);
            presetAudited++;
            run.check(audit.interactive >= 30, `${tag}：整窗可交互件应不少于 30 个，实得 ${audit.interactive}`);
            run.eq(audit.missing, [], `${tag}：这些可交互件没有可访问名称`);
            const roles = await inFrame(page, (doc) => ({
                dialog: [...doc.querySelectorAll('.in-pm[role="dialog"]')].filter((e) => !e.hidden).length,
                listbox: [...doc.querySelectorAll('.in-pm [role="listbox"]')].filter((e) => !e.closest("[hidden]"))
                    .length,
                options: doc.querySelectorAll('.in-pm [role="option"]').length,
            }));
            run.eq(roles.dialog, 1, `${tag}：面板应是一个 role=dialog`);
            run.eq(roles.listbox, 1, `${tag}：列表应是一个 role=listbox`);
            run.check(roles.options >= 8, `${tag}：列表里应至少有 8 个 role=option，实得 ${roles.options}`);
        }
    }
    run.log(`  预设面板子态名称审计共 ${presetAudited} 页`);

    // ---- ② 角色与状态
    for (const lang of LANGS) {
        const tag = `in-playing/${lang}`;
        await openInput(page, server, "in-playing", { lang });
        const a = await inFrame(page, roleAudit);
        run.eq(a.sliders.length, 6, `${tag}：滑块应有 6 个（反馈量、Mix、低切、高切、延迟、上限）`);
        for (const s of a.sliders) {
            run.check(s.name.length > 0, `${tag}：滑块应有名称`);
            run.check(
                Number.isFinite(s.min) &&
                    Number.isFinite(s.max) &&
                    Number.isFinite(s.now) &&
                    s.min < s.max &&
                    s.now >= s.min &&
                    s.now <= s.max,
                `${tag}：滑块「${s.name}」的 valuemin / valuemax / valuenow 应是 min < max 且 now 在其间，实得 ${JSON.stringify(s)}`,
            );
            run.check(
                s.text.length > 0 && !/NaN|undefined|null/.test(s.text),
                `${tag}：滑块「${s.name}」的 aria-valuetext 应有读数，实得 ${JSON.stringify(s.text)}`,
            );
            run.eq(s.tab, 0, `${tag}：滑块「${s.name}」应能 Tab 到`);
        }
        run.eq(a.sliders.find((s) => s.max === 150)?.now, 72, `${tag}：反馈量滑块范围 0 到 150，当前 72`);
        run.eq(a.switches.length, 4, `${tag}：开关应有 4 个（环路、极性反转、限制器、失控保护）`);
        for (const s of a.switches)
            run.check(s.checked === "true" || s.checked === "false", `${tag}：开关「${s.name}」应有 aria-checked`);
        run.eq(a.meters.length, 3, `${tag}：电平管应有 3 根`);
        for (const m of a.meters) {
            run.eq([m.min, m.max], [-60, 0], `${tag}：电平管「${m.name}」范围应是 -60 到 0`);
            run.check(
                m.name.length > 0 && /dBFS/.test(m.text),
                `${tag}：电平管「${m.name}」应有名称与带 dBFS 的读数，实得 ${JSON.stringify(m)}`,
            );
        }
        run.check(
            a.tablists.filter((t) => t.tabs >= 2 && t.name).length >= 3,
            `${tag}：语言、反馈量单位、延迟单位三组分段应是有名称的 tablist，实得 ${JSON.stringify(a.tablists)}`,
        );
        run.check(
            a.panic && a.panic.tag === "BUTTON" && a.panic.name.length > 0,
            `${tag}：紧急切断应是有名称的按钮，实得 ${JSON.stringify(a.panic)}`,
        );
    }
    await openInput(page, server, "in-limiter-confirm", { lang: "zh" });
    {
        const d = await inFrame(page, (doc) => {
            const e = doc.querySelector('[data-part="input.limiter.confirm"]');
            if (!e) return null;
            return {
                role: e.getAttribute("role"),
                buttons: [...e.querySelectorAll("button")].map((b) => b.textContent.trim()),
            };
        });
        run.eq(d && d.role, "alertdialog", "限制器关闭确认应是 alertdialog");
        run.check(
            d && d.buttons.length === 2 && d.buttons.every((b) => b.length > 0),
            `确认框两颗按钮都应有文字，实得 ${JSON.stringify(d)}`,
        );
    }

    // ---- ③ 键盘
    await openInput(page, server, "in-playing", { lang: "zh" });
    run.check(await focusOn(page, FB), "应能把焦点放到反馈量滑块上");
    run.eq(await readNow(page), 72, "起点 72%");
    await press(page, "ArrowUp");
    await page.waitFor(
        `(() => Number(document.querySelector("iframe").contentDocument.querySelector('${FB}').getAttribute("aria-valuenow")) === 73)()`,
        { label: "方向键上应加 1" },
    );
    await press(page, "ArrowDown");
    await press(page, "ArrowDown");
    await page.waitFor(
        `(() => Number(document.querySelector("iframe").contentDocument.querySelector('${FB}').getAttribute("aria-valuenow")) === 71)()`,
        { label: "方向键下应减 1" },
    );
    await press(page, "PageUp");
    await page.waitFor(
        `(() => Number(document.querySelector("iframe").contentDocument.querySelector('${FB}').getAttribute("aria-valuenow")) === 81)()`,
        { label: "PgUp 应加 10" },
    );
    await press(page, "PageDown");
    await page.waitFor(
        `(() => Number(document.querySelector("iframe").contentDocument.querySelector('${FB}').getAttribute("aria-valuenow")) === 71)()`,
        { label: "PgDn 应减 10" },
    );
    await press(page, "End");
    await page.waitFor(
        `(() => Number(document.querySelector("iframe").contentDocument.querySelector('${FB}').getAttribute("aria-valuenow")) === 100)()`,
        { label: "End 在限制器开着时应停在 100%（闸）" },
    );
    await press(page, "ArrowUp");
    await page.eval("new Promise((r) => setTimeout(r, 150))");
    run.eq(await readNow(page), 100, "限制器开着时，方向键向上过不了 100%");
    await press(page, "Home");
    await page.waitFor(
        `(() => Number(document.querySelector("iframe").contentDocument.querySelector('${FB}').getAttribute("aria-valuenow")) === 0)()`,
        { label: "Home 应回到 0" },
    );
    // Enter 进入数值输入，Esc 退出且不改值
    await setFeedback(page, 64);
    await focusOn(page, FB);
    await press(page, "Enter");
    await page.waitFor(
        `!!document.querySelector("iframe").contentDocument.querySelector('[data-gb="input.numEdit"]')`,
        { label: "Enter 应打开数值输入" },
    );
    await press(page, "Escape");
    await page.waitFor(`!document.querySelector("iframe").contentDocument.querySelector('[data-gb="input.numEdit"]')`, {
        label: "Esc 应关闭数值输入",
    });
    run.eq(await readNow(page), 64, "Esc 退出数值输入不改值");

    // 紧急切断用键盘触发：空格与回车各一次（第二次要等冷却，只验第一次的调用与第二次的键位）
    for (const key of ["Space", "Enter"]) {
        await openInput(page, server, "in-playing", { lang: "zh" });
        const rec = await recordBridge(page, ["panic"]);
        run.check(await focusOn(page, '[data-gb="input.panic"]'), `${key}：应能把焦点放到紧急切断上`);
        await press(page, key);
        await page.waitFor(`window.__calls.some((c) => c[0] === "panic")`, { label: `${key} 应触发紧急切断` });
        run.eq((await rec.calls()).filter((c) => c[0] === "panic").length, 1, `${key}：紧急切断恰好调一次`);
    }

    // Tab 走一圈：每一站都有看得见的焦点环
    await openInput(page, server, "in-playing", { lang: "zh" });
    {
        const { tabbable, stops } = await tabWalk(page);
        run.check(stops.length >= 20, `Tab 应至少走过 20 站，实得 ${stops.length}（可 Tab 到 ${tabbable}）`);
        run.eq(
            stops.filter((s) => !s.ring).map((s) => s.id),
            [],
            "这些 Tab 停靠点没有看得见的焦点环",
        );
        run.check(
            stops.some((s) => /panic/.test(s.id)) && stops.some((s) => /fb|disc/.test(s.id)),
            `Tab 一圈应经过紧急切断与反馈量滑块，实得 ${JSON.stringify(stops.map((s) => s.id))}`,
        );
    }

    // ---- ④ 坏样例
    // 去掉滑块名称：名称审计要抓出来
    await openInput(page, server, "in-playing", { lang: "zh" });
    await inFrame(page, (doc) => {
        const e = doc.querySelector('[data-gb="input.fb.knob"] [role="slider"]');
        e.removeAttribute("aria-label");
        e.removeAttribute("aria-labelledby");
        e.removeAttribute("title");
    });
    {
        const audit = await inFrame(page, nameAudit);
        run.check(
            audit.missing.length >= 1,
            `坏样例：去掉滑块名称后名称审计应报缺，实得 ${JSON.stringify(audit.missing)}`,
        );
    }
    // 把 valuemax 改成比 valuemin 小：角色审计要能量出来
    await inFrame(page, (doc) => {
        doc.querySelector('[data-gb="input.fb.knob"] [role="slider"]').setAttribute("aria-valuemax", "-1");
    });
    {
        const a = await inFrame(page, roleAudit);
        run.check(
            a.sliders.some((s) => !(s.min < s.max)),
            "坏样例：valuemax 改小后角色审计应量出 min >= max",
        );
    }
    // 关掉焦点环：Tab 走一圈要报出没有环的停靠点
    await openInput(page, server, "in-playing", { lang: "zh" });
    await inFrame(page, (doc) => {
        const s = doc.createElement("style");
        s.textContent = ".rn-shell *, .rn-shell *:focus-visible { outline: none !important; }";
        doc.head.append(s);
    });
    {
        const { stops } = await tabWalk(page);
        run.check(
            stops.length >= 20 && stops.filter((s) => !s.ring).length >= 20,
            `坏样例：关掉焦点环后应报出没有环的停靠点，实得 ${stops.filter((s) => !s.ring).length}`,
        );
    }

    // ---- ⑤ 首启叠层：角色与名称、内容层 inert、焦点、紧急切断、Tab 一圈（三语，每个首启画面）
    {
        const OVERLAY_ANY =
            '(() => { const d = document.querySelector("iframe").contentDocument; return ["langStart", "rules", "tour"].every((k) => { const e = d.querySelector(\'[data-gb="input.\' + k + \'"]\'); return !e || !!e.closest("[hidden]"); }); })()';
        let checked = 0;
        for (const lang of LANGS) {
            for (const frame of FRAMES) {
                if (!isFirstRunScene(frame.scene)) continue;
                const tag = frameTag(frame, lang);
                await openFrame(page, server, frame, { lang });
                const o = await inFrame(page, overlayA11yProbe, "input");
                run.eq(overlayProblems(o, frame, TOUR_STEPS), [], `${tag}：叠层的无障碍问题`);
                run.eq(panicProblems(await inFrame(page, panicProbe)), [], `${tag}：叠层之上的紧急切断`);
                const { stops } = await tabWalk(page);
                run.check(
                    stops.some((s) => s.inOverlay),
                    `${tag}：Tab 一圈应进到叠层里，实得 ${JSON.stringify(stops.map((s) => s.id))}`,
                );
                run.check(
                    stops.some((s) => s.panic),
                    `${tag}：Tab 一圈应到紧急切断，实得 ${JSON.stringify(stops.map((s) => s.id))}`,
                );
                run.eq(
                    stops.filter((s) => !s.inOverlay && !s.panic).map((s) => s.id),
                    [],
                    `${tag}：Tab 一圈只该落在叠层或紧急切断上（内容层 inert）`,
                );
                run.eq(
                    stops.filter((s) => !s.ring).map((s) => s.id),
                    [],
                    `${tag}：这些 Tab 停靠点没有看得见的焦点环`,
                );
                checked++;
            }
        }
        run.eq(checked, 9 * LANGS.length, "首启画面：8 个场景加规则页放行后一个，每种语言各查一遍");

        // 用键盘（空格）在三种叠层下触发紧急切断：桥收到一次 panic，叠层关掉
        for (const id of ["in-lang", "in-rules", "in-tour-1"]) {
            await openInput(page, server, id, { lang: "zh" });
            const rec = await recordBridge(page, ["panic"]);
            run.check(await focusOn(page, '[data-gb="input.panic"]'), `${id}：应能把焦点放到紧急切断上`);
            await press(page, "Space");
            await page.waitFor(`window.__calls.some((c) => c[0] === "panic")`, {
                label: `${id}：空格应触发紧急切断`,
            });
            run.eq((await rec.calls()).filter((c) => c[0] === "panic").length, 1, `${id}：紧急切断恰好调一次`);
            await page.waitFor(OVERLAY_ANY, { label: `${id}：紧急切断后叠层应立即关` });
        }

        // 坏样例：量尺本身可信。每一条改坏之后，对应的判据必须报出来
        const bad = async (label, id, sub, mutate, check) => {
            const frame = { scene: SCENES.find((s) => s.id === id), sub };
            await openFrame(page, server, frame, { lang: "zh" });
            run.eq(
                overlayProblems(await inFrame(page, overlayA11yProbe, "input"), frame, TOUR_STEPS),
                [],
                `坏样例前提（${label}）：改坏之前应没有问题`,
            );
            await inFrame(page, mutate);
            await check(frame);
        };
        const overlayHas = (re, label) => async (frame) => {
            const got = overlayProblems(await inFrame(page, overlayA11yProbe, "input"), frame, TOUR_STEPS);
            run.check(
                got.some((m) => re.test(m)),
                `坏样例：${label}，应报出 ${re}，实得 ${JSON.stringify(got)}`,
            );
        };
        await bad(
            "导览去掉名称",
            "in-tour-2",
            "",
            (doc) => doc.querySelector('[data-gb="input.tour"]').removeAttribute("aria-labelledby"),
            overlayHas(/没有可访问名称/, "导览对话框没有名称"),
        );
        await bad(
            "语言卡去掉名称",
            "in-lang",
            "",
            (doc) => doc.querySelector('[data-gb="input.langStart"]').removeAttribute("aria-labelledby"),
            overlayHas(/没有可访问名称/, "语言卡没有名称"),
        );
        await bad(
            "去掉内容层 inert",
            "in-rules",
            "",
            (doc) => doc.querySelector('[data-gb="input.shell"] > [data-part="content"]').removeAttribute("inert"),
            overlayHas(/内容层应是 inert/, "内容层没有 inert"),
        );
        await bad(
            "焦点移出叠层",
            "in-tour-3",
            "",
            (doc) => doc.activeElement && doc.activeElement.blur(),
            overlayHas(/焦点应在叠层之内/, "焦点不在叠层里"),
        );
        await bad(
            "规则页按钮在未放行时可用",
            "in-rules",
            "",
            (doc) => doc.querySelector('[data-part="rules.start"]').removeAttribute("disabled"),
            overlayHas(/rules\.start 在未放行应禁用/, "未放行的按钮没禁用"),
        );
        await bad(
            "规则页按钮放行后仍禁用",
            "in-rules",
            "ready",
            (doc) => doc.querySelector('[data-part="rules.skip"]').setAttribute("disabled", ""),
            overlayHas(/rules\.skip 在放行后应可用/, "放行后的按钮仍禁用"),
        );
        await bad(
            "按钮没有名称",
            "in-lang",
            "",
            (doc) => {
                const b = doc.querySelector('[data-part="langStart.en"]');
                b.textContent = "";
            },
            overlayHas(/langStart\.en 没有名称/, "语言按钮没有名称"),
        );
        // 紧急切断落进 inert：panicProblems 要报
        await openInput(page, server, "in-rules", { lang: "zh" });
        await inFrame(page, (doc) => doc.querySelector('[data-gb="input.panic"]').setAttribute("inert", ""));
        {
            const got = panicProblems(await inFrame(page, panicProbe));
            run.check(
                got.some((m) => /inert/.test(m)) && got.some((m) => /聚焦不上/.test(m)),
                `坏样例：紧急切断落进 inert，应报「落在 inert 里」与「聚焦不上」，实得 ${JSON.stringify(got)}`,
            );
        }
        // 内容层不 inert：Tab 一圈会走进内容层，「只该落在叠层或紧急切断上」要报红
        await openInput(page, server, "in-tour-2", { lang: "zh" });
        await inFrame(page, (doc) =>
            doc.querySelector('[data-gb="input.shell"] > [data-part="content"]').removeAttribute("inert"),
        );
        {
            const { stops } = await tabWalk(page);
            run.check(
                stops.filter((s) => !s.inOverlay && !s.panic).length >= 5,
                `坏样例：去掉内容层 inert 后 Tab 一圈应走进内容层，实得 ${JSON.stringify(stops.map((s) => s.id))}`,
            );
        }
        // 关掉焦点环：叠层里的停靠点要报出没有环
        await openInput(page, server, "in-tour-2", { lang: "zh" });
        await inFrame(page, (doc) => {
            const s = doc.createElement("style");
            s.textContent = ".rn-shell *, .rn-shell *:focus-visible { outline: none !important; }";
            doc.head.append(s);
        });
        {
            const { stops } = await tabWalk(page);
            run.check(
                stops.filter((s) => s.inOverlay && !s.ring).length >= 2,
                `坏样例：关掉焦点环后叠层里应报出没有环的停靠点，实得 ${JSON.stringify(stops)}`,
            );
        }
        // 叠层没摆出来：overlayProbeFailures 要报（量尺不能在普通页面上空转）
        await openInput(page, server, "in-playing", { lang: "zh" });
        run.check(
            (await overlayProbeFailures(page, { scene: SCENES.find((s) => s.id === "in-tour-2"), sub: "" })).length > 0,
            "坏样例：普通页面上没有叠层，应判「叠层没摆出来」",
        );
    }

    run.check(page.errors.length === 0, `页面不应有错误：${page.errors.slice(0, 3).join("；")}`);
} finally {
    await browser.close();
    await server.close();
}
run.finish();
