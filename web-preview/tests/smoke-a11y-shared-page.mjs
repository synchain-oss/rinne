// SPDX-License-Identifier: GPL-3.0-or-later
//
// 共享件的可访问性冒烟（无头浏览器）：Input / Output × 中 / 英 / 法。
//   ① 名称：页上每个可交互件都有非空的可访问名称（aria-labelledby > aria-label > 文字 > title）；
//   ② 角色与状态：语言胶囊 tablist / tab、组选择 radiogroup / radio（roving tabindex，恰好一个 tabindex=0）、
//      关于面板里的 switch 与 tail 的 radiogroup、对话框 dialog + aria-modal + aria-labelledby、消息位红为 alert 其余 status、轻提示 status；
//   ③ 键盘：方向键只移焦点不选中，空格与回车才选中；Esc 关闭关于面板且焦点还给版本按钮，打开期间外壳内容 inert；
//      Tab 走一圈，每一站都有看得见的焦点环；
//   ④ 坏样例：夹具的 ?defect=noname 必须被名称审计抓出来。
// 浏览器缺失时整套跳过（退出码 2）。
//
// 用法：node web-preview/tests/smoke-a11y-shared-page.mjs [仓库根]

import { createRun, repoRoot } from "./lib/harness.mjs";
import { findBrowser, launchBrowser } from "./lib/browser.mjs";
import { startStaticServer } from "./lib/static-server.mjs";
import { ROLES, LANGS, SCENES, loadDesign, openScene } from "./shared-measure.mjs";

const ROOT = repoRoot();
const run = createRun("共享件可访问性");
if (!findBrowser()) run.skip("没有 Chromium 系浏览器");

const design = await loadDesign(ROOT);
const server = await startStaticServer(ROOT);
const browser = await launchBrowser();

/** 页内名称审计：返回 { interactive:N, missing:[描述] }。简化版可访问名称：labelledby > aria-label > 文字 > title。 */
const NAME_AUDIT = `(() => {
    const stage = document.getElementById("stage");
    const shown = (e) => {
        for (let n = e; n && n !== document.documentElement; n = n.parentElement) {
            if (n.hidden) return false;
            const cs = getComputedStyle(n);
            if (cs.display === "none" || cs.visibility === "hidden") return false;
        }
        return true;
    };
    const nameOf = (e) => {
        const by = e.getAttribute("aria-labelledby");
        if (by) {
            const txt = by.split(/\\s+/).map((id) => (document.getElementById(id) || {}).textContent || "").join(" ").trim();
            if (txt) return txt;
        }
        const al = (e.getAttribute("aria-label") || "").trim();
        if (al) return al;
        const tc = (e.textContent || "").trim();
        if (tc && e.tagName !== "SELECT") return tc;
        return (e.getAttribute("title") || "").trim();
    };
    const SEL = 'button, select, input, a[href], [role="button"], [role="radio"], [role="tab"], [role="switch"], [role="link"], [role="checkbox"], [tabindex="0"]';
    const missing = [];
    let n = 0;
    for (const e of stage.querySelectorAll(SEL)) {
        if (!shown(e)) continue;
        n++;
        if (!nameOf(e)) missing.push((e.getAttribute("data-gb") || e.getAttribute("data-part") || e.className || e.tagName) + "");
    }
    return { interactive: n, missing };
})()`;

const KEY = {
    Tab: { key: "Tab", code: "Tab", vk: 9 },
    Escape: { key: "Escape", code: "Escape", vk: 27 },
    ArrowRight: { key: "ArrowRight", code: "ArrowRight", vk: 39 },
    ArrowLeft: { key: "ArrowLeft", code: "ArrowLeft", vk: 37 },
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
    await page.eval("new Promise((r) => setTimeout(r, 40))");
}
/** Tab 走一圈（只按可 Tab 到的件数），返回每一站的 { id, ring }。ring 只认 outline：焦点环的实现就是 outline。 */
async function tabWalk(page) {
    await page.eval("document.body.focus(); document.activeElement && document.activeElement.blur()");
    // 只按可 Tab 到的件数按键：多按一下焦点就走出文档，后面的键盘事件会落空
    const tabbable = await page.eval(
        `[...document.getElementById("stage").querySelectorAll('button, select, input, a[href], [tabindex="0"]')].filter((e) => !e.disabled && !e.hidden && !e.closest("[inert]") && e.getClientRects().length > 0).length`,
    );
    const stops = [];
    for (let i = 0; i < tabbable; i++) {
        await press(page, "Tab");
        const f = await page.eval(
            `(() => { const a = document.activeElement; if (!a || a === document.body) return null; const cs = getComputedStyle(a); const ring = cs.outlineStyle !== "none" && parseFloat(cs.outlineWidth) > 0; return { id: (a.getAttribute("data-gb") || a.getAttribute("data-group") || a.className || a.tagName) + "", ring }; })()`,
        );
        if (!f) break;
        stops.push(f);
    }
    return stops;
}

const cur = (page) =>
    page.eval(
        `(() => { const a = document.activeElement; return a ? { gb: a.getAttribute("data-gb") || "", part: a.getAttribute("data-part") || "", cls: String(a.className || ""), tag: a.tagName, group: a.getAttribute("data-group") || "" } : null; })()`,
    );

try {
    // 每个（角色，语言）用一张新页：Tab 走一圈会把这张页的键盘焦点留在奇怪的地方，后面的键盘事件会落空
    const errors = [];
    let page = null;
    const freshPage = async () => {
        if (page) {
            errors.push(...page.errors);
            await page.close();
        }
        page = await browser.newPage();
        // 无头页默认没有焦点，键盘事件会落空；强制页面处于有焦点状态
        await page.send("Emulation.setFocusEmulationEnabled", { enabled: true });
    };
    const WIN = SCENES.find((s) => s.id === "window");

    for (const role of ROLES) {
        for (const lang of LANGS) {
            const tag = `${role}/${lang}`;
            await freshPage();

            // ---- ① ② 整窗：名称与角色 ----
            await openScene(page, server, design, role, lang, WIN, "&st=cutPanic");
            run.eq(await page.eval("document.documentElement.lang"), lang, `${tag}：<html lang> 应等于界面语言`);
            const audit = await page.eval(NAME_AUDIT);
            run.check(audit.interactive >= 14, `${tag}：整窗应有足够多的可交互件（防空转），实得 ${audit.interactive}`);
            run.eq(audit.missing, [], `${tag}：这些可交互件没有可访问名称`);

            const q = (sel) =>
                page.eval(
                    `(() => { const e = document.querySelector(${JSON.stringify(sel)}); if (!e) return null; return { role: e.getAttribute("role"), tag: e.tagName, label: e.getAttribute("aria-label") || "", title: e.getAttribute("title") || "", text: (e.textContent || "").trim() }; })()`,
                );
            const help = await q(`[data-gb="${role}.footer.help"]`);
            run.check(
                help && help.tag === "BUTTON" && help.label.length > 0,
                `${tag}：「?」应是带名称的 button，实得 ${JSON.stringify(help)}`,
            );
            const scale = await q(`[data-gb="${role}.footer.scale"] select`);
            run.check(
                scale && scale.tag === "SELECT" && scale.label.length > 0,
                `${tag}：缩放下拉应是带名称的 select，实得 ${JSON.stringify(scale)}`,
            );
            const ver = await q(`[data-gb="${role}.footer.version"]`);
            run.check(
                ver && ver.tag === "BUTTON" && ver.text.length > 0,
                `${tag}：版本按钮应是带文字的 button，实得 ${JSON.stringify(ver)}`,
            );

            const lcap = await page.eval(
                `(() => { const r = document.querySelector('[data-gb="${role}.lang"]'); const tabs = [...r.querySelectorAll('[role="tab"]')]; return { role: r.getAttribute("role"), label: r.getAttribute("aria-label") || "", tabs: tabs.length, selected: tabs.filter((t) => t.getAttribute("aria-selected") === "true").length, names: tabs.map((t) => (t.textContent || "").trim()) }; })()`,
            );
            run.eq(lcap.role, "tablist", `${tag}：语言胶囊应是 tablist`);
            run.check(lcap.label.length > 0, `${tag}：语言胶囊应有名称`);
            run.eq([lcap.tabs, lcap.selected], [3, 1], `${tag}：语言胶囊应有 3 个 tab、恰好 1 个被选中`);
            run.check(
                lcap.names.every((n) => n.length > 0),
                `${tag}：每个语言 tab 都应有文字`,
            );

            const grp = await page.eval(
                `(() => { const r = document.querySelector('[data-gb="${role}.group"]'); const bar = r.querySelector('[role="radiogroup"]'); const radios = [...r.querySelectorAll('[role="radio"]')]; return { bar: Boolean(bar), n: radios.length, checked: radios.filter((x) => x.getAttribute("aria-checked") === "true").length, stops: radios.filter((x) => x.tabIndex === 0).length }; })()`,
            );
            run.check(grp.bar, `${tag}：组选择应有 radiogroup`);
            run.eq(
                [grp.n, grp.checked, grp.stops],
                [8, 1, 1],
                `${tag}：8 个 radio、恰好 1 个选中、恰好 1 个 tab 停靠点`,
            );

            // 消息位：切断是 alert
            const band = await page.eval(
                `(() => { const b = document.querySelector('[data-gb="${role}.band"]'); return b ? { role: b.getAttribute("role"), tone: b.getAttribute("data-tone") } : null; })()`,
            );
            run.eq(
                band && band.role,
                band && band.tone === "red" ? "alert" : "status",
                `${tag}：消息位的角色应随颜色（红 alert，其余 status），实得 ${JSON.stringify(band)}`,
            );

            // ---- ③ 键盘：组选择 ----
            await page.eval(`document.querySelector('[data-gb="${role}.group"] [role="radio"][tabindex="0"]').focus()`);
            const before = await page.eval("window.__fx.calls.length");
            await press(page, "ArrowRight");
            const afterArrow = await cur(page);
            run.eq(afterArrow && afterArrow.group, "B", `${tag}：方向键右应把焦点移到 B`);
            run.eq(await page.eval("window.__fx.calls.length"), before, `${tag}：方向键只移焦点，不触发选组`);
            await press(page, "Space");
            const picked = await page.eval(
                `window.__fx.calls.filter((c) => c.name === "setGroup").map((c) => c.args[0])`,
            );
            run.eq(picked, ["B"], `${tag}：空格应选中焦点所在的组`);
            await page.eval(
                `document.querySelector('[data-gb="${role}.group"] [role="radio"][data-group="D"]').focus()`,
            );
            await press(page, "Enter");
            run.eq(
                await page.eval(`window.__fx.calls.filter((c) => c.name === "setGroup").map((c) => c.args[0])`),
                ["B", "D"],
                `${tag}：回车应选中焦点所在的组`,
            );

            // ---- ③ 键盘：关于面板的开关与焦点 ----
            await openScene(page, server, design, role, lang, WIN);
            await page.eval(
                `(() => { const v = document.querySelector('[data-gb="${role}.footer.version"]'); v.focus(); v.click(); })()`,
            );
            await page.waitFor(
                `Boolean(document.querySelector('[data-gb="${role}.about"]')) && !document.querySelector('[data-gb="${role}.about"]').closest('[hidden]')`,
                { label: `${tag}：点版本按钮应打开关于面板` },
            );
            const dlg = await page.eval(
                `(() => { const d = document.querySelector('[data-gb="${role}.about"]'); const by = d.getAttribute("aria-labelledby"); const lab = by && document.getElementById(by); const shellContent = document.querySelector(".rn-shell__content"); return { role: d.getAttribute("role"), modal: d.getAttribute("aria-modal"), labelled: Boolean(lab && lab.textContent.trim()), focusIn: d.contains(document.activeElement), inert: shellContent.hasAttribute("inert") }; })()`,
            );
            run.eq(
                dlg,
                { role: "dialog", modal: "true", labelled: true, focusIn: true, inert: true },
                `${tag}：关于面板应是带名称的模态对话框，焦点在里面，外壳内容 inert`,
            );
            const ctl = await page.eval(
                `(() => { const d = document.querySelector('[data-gb="${role}.about"]'); const sw = [...d.querySelectorAll('[role="switch"]')]; const rg = [...d.querySelectorAll('[role="radiogroup"]')]; const rd = [...d.querySelectorAll('[role="radio"]')]; return { switches: sw.length, switchOk: sw.every((s) => ["true", "false"].includes(s.getAttribute("aria-checked")) && (s.getAttribute("aria-label") || s.getAttribute("aria-labelledby") || s.textContent.trim())), groups: rg.length, radios: rd.length, radioOk: rd.every((r) => ["true", "false"].includes(r.getAttribute("aria-checked"))) }; })()`,
            );
            run.check(
                ctl.switches >= 1 && ctl.switchOk,
                `${tag}：关于面板的开关应是带状态与名称的 switch，实得 ${JSON.stringify(ctl)}`,
            );
            if (role === "input")
                run.check(
                    ctl.groups >= 1 && ctl.radios >= 2 && ctl.radioOk,
                    `${tag}：Input 关于面板的 tail 模式应是 radiogroup，实得 ${JSON.stringify(ctl)}`,
                );
            const dlgNames = await page.eval(NAME_AUDIT);
            run.eq(dlgNames.missing, [], `${tag}：关于面板里这些可交互件没有可访问名称`);
            await press(page, "Escape");
            await page.eval("new Promise((r) => setTimeout(r, 500))");
            run.eq(
                await page.eval(`document.querySelector('[data-gb="${role}.about"]').closest('[hidden]') !== null`),
                true,
                `${tag}：Esc 应关闭关于面板`,
            );
            const back = await page.eval(
                `(() => ({ focusBack: document.activeElement === document.querySelector('[data-gb="${role}.footer.version"]'), inert: document.querySelector(".rn-shell__content").hasAttribute("inert") }))()`,
            );
            run.eq(back, { focusBack: true, inert: false }, `${tag}：关闭后焦点应回到版本按钮，外壳内容不再 inert`);

            // ---- ③ 键盘：Tab 走一圈，每一站都有看得见的焦点环 ----
            await openScene(page, server, design, role, lang, WIN);
            const stops = await tabWalk(page);
            run.check(stops.length >= 4, `${tag}：Tab 应至少走过 4 站，实得 ${stops.length}`);
            const bare = stops.filter((s) => !s.ring).map((s) => s.id);
            run.eq(bare, [], `${tag}：这些 Tab 停靠点没有看得见的焦点环`);

            // ---- 对话框与轻提示的角色 ----
            for (const sc of ["scale-confirm", "toast-group"]) {
                await openScene(
                    page,
                    server,
                    design,
                    role,
                    lang,
                    SCENES.find((s) => s.id === sc),
                );
                if (sc === "scale-confirm") {
                    const d = await page.eval(
                        `(() => { const d = document.querySelector('[data-gb="${role}.scale.confirm"]'); const by = d.getAttribute("aria-labelledby"); const lab = by && document.getElementById(by); return { role: d.getAttribute("role"), modal: d.getAttribute("aria-modal"), labelled: Boolean(lab && lab.textContent.trim()) }; })()`,
                    );
                    run.eq(
                        d,
                        { role: "dialog", modal: "true", labelled: true },
                        `${tag}：缩放确认应是带名称的模态对话框`,
                    );
                } else {
                    const tst = await page.eval(
                        `(() => { const t = document.querySelector('[data-gb="${role}.toast"]'); return { role: t.getAttribute("role"), shown: !t.hidden && t.textContent.trim().length > 0 }; })()`,
                    );
                    run.eq(tst, { role: "status", shown: true }, `${tag}：轻提示应是 status 且有文字`);
                }
            }
        }
    }

    // ---- ④ 坏样例 ----
    await freshPage();
    await openScene(page, server, design, "input", "zh", WIN, "&defect=noname");
    const bad = await page.eval(NAME_AUDIT);
    run.check(bad.missing.length >= 2, `坏样例 noname：名称审计应抓出缺名称的件，实得 ${JSON.stringify(bad.missing)}`);

    // 焦点环被去掉：Tab 走一圈必须报出没有焦点环的停靠点
    await freshPage();
    await openScene(page, server, design, "input", "zh", WIN, "&defect=noring");
    const noRing = (await tabWalk(page)).filter((x) => !x.ring);
    run.check(noRing.length >= 4, `坏样例 noring：应报出没有焦点环的停靠点，实得 ${noRing.length} 个`);

    errors.push(...page.errors);
    run.check(errors.length === 0, `页面不应有错误：${errors.slice(0, 3).join("；")}`);
} finally {
    await browser.close();
    await server.close();
}
run.finish();
