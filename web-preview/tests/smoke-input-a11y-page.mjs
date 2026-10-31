// SPDX-License-Identifier: GPL-3.0-or-later
//
// Input 页面可访问性冒烟（无头浏览器），真源页面经预览壳页装载。
//   ① 名称：页上每个可交互件都有非空的可访问名称（aria-labelledby > aria-label > 文字 > title）；
//      30 个 in-* 场景的中文页全量审计，英文与法文页审计最有代表性的几个场景；
//   ② 角色与状态：滑块（反馈量、Mix、低切、高切、延迟、上限）有 valuemin / valuemax / valuenow / valuetext 且取值合理，
//      开关有 aria-checked，单位分段是 tablist，电平管是 meter（-60 到 0），紧急切断是有名称的按钮，
//      限制器关闭确认是 alertdialog 且两颗按钮有名称；
//   ③ 键盘：反馈滑块的方向键 1 步、PgUp / PgDn 10 步、Home 到 0、End 在限制器开着时停在 100%（闸），Enter 进入数值输入、Esc 退出且不改值；
//      紧急切断用键盘（空格与回车）能触发；Tab 走一圈每一站都有看得见的焦点环；
//   ④ 坏样例：去掉滑块的名称、关掉焦点环、把 valuemax 改小，上面各项必须各自判红（量尺本身可信）。
// 浏览器缺失时整套跳过（退出码 2），不是通过。本机同时只能有一个浏览器冒烟。
//
// 用法：node web-preview/tests/smoke-input-a11y-page.mjs [仓库根]

import { createRun, repoRoot } from "./lib/harness.mjs";
import { findBrowser, launchBrowser } from "./lib/browser.mjs";
import { startStaticServer } from "./lib/static-server.mjs";
import { LANGS, SCENES, VIEW, inFrame, openInput, recordBridge } from "./smoke-input-kit-page.mjs";

const ROOT = repoRoot();
const run = createRun("Input 页面可访问性");
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
        // 滑块、电平管、对话框与分组容器的名称只能由作者给（aria-label 或 aria-labelledby），不取里面的文字
        const authorOnly = /^(slider|meter|dialog|alertdialog|tablist|radiogroup)$/.test(e.getAttribute("role") || "");
        const tc = (e.textContent || "").trim();
        if (tc && e.tagName !== "SELECT" && !authorOnly) return tc;
        return (e.getAttribute("title") || "").trim();
    };
    const SEL =
        'button, select, input, a[href], [role="button"], [role="radio"], [role="tab"], [role="switch"], [role="link"], [role="checkbox"], [role="slider"], [role="meter"], [role="dialog"], [role="alertdialog"], [tabindex="0"]';
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
        for (const scene of SCENES) {
            if (lang !== "zh" && !REPRESENTATIVE.includes(scene.id)) continue;
            const tag = `${scene.id}/${lang}`;
            await openInput(page, server, scene.id, { lang });
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

    run.check(page.errors.length === 0, `页面不应有错误：${page.errors.slice(0, 3).join("；")}`);
} finally {
    await browser.close();
    await server.close();
}
run.finish();
