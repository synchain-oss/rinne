// SPDX-License-Identifier: GPL-3.0-or-later
//
// Output 页面的可访问性冒烟（无头浏览器）：真页面（夹具 web-preview/fixtures/output）× 中 / 英 / 法。
//   ① 名称：页上每个可交互件都有非空的可访问名称（aria-labelledby > aria-label > 文字 > title），全部场景都查；
//   ② 角色与状态：配对卡 group + 名称、语言胶囊 tablist / tab、组选择 radiogroup / radio（roving tabindex，恰好一个 tabindex=0）、
//      消息位红为 alert 其余 status（全部场景）、关于面板 dialog + aria-modal + aria-labelledby、缩放确认 dialog、轻提示 status；
//   ③ 键盘：组选择方向键只移焦点、空格与回车才选中；关于面板打开时焦点在里面、外壳内容 inert，Esc 关闭且焦点还给版本按钮；
//      Tab 走一圈，每一站都有看得见的焦点环；接管确认条展开时其余组 aria-disabled 且不接受点选；
//   ④ 坏样例：夹具的 ?defect=noname 必须被名称审计抓出来，?defect=noring 必须被焦点环检查抓出来。
// 浏览器缺失时整套跳过（退出码 2）。
//
// 用法：node web-preview/tests/output-a11y-page.mjs [仓库根]

import { createRun, repoRoot } from "./lib/harness.mjs";
import { findBrowser, launchBrowser } from "./lib/browser.mjs";
import { startStaticServer } from "./lib/static-server.mjs";
import { awaitState } from "./lib/await-state.mjs";
import {
    LANGS,
    NAME_AUDIT,
    callsOf,
    click,
    enableFocus,
    focusOf,
    loadDesign,
    loadOutputScenes,
    openOutput,
    press,
    tabWalk,
} from "../fixtures/output/lib.mjs";

const ROOT = repoRoot();
const run = createRun("Output 页面可访问性");
if (!findBrowser()) run.skip("没有 Chromium 系浏览器");

const box = await loadDesign(ROOT);
const scenes = await loadOutputScenes(ROOT);
const server = await startStaticServer(ROOT);
const browser = await launchBrowser();

const gb = (part) => `[data-gb="output.${part}"]`;

try {
    // 每个语言用一张新页：Tab 走一圈会把键盘焦点留在奇怪的地方，后面的键盘事件会落空
    const errors = [];
    let page = null;
    const freshPage = async () => {
        if (page) {
            errors.push(...page.errors);
            await page.close();
        }
        page = await browser.newPage();
        await enableFocus(page);
    };

    for (const lang of LANGS) {
        await freshPage();

        // ---- ① 名称 + 消息位角色：全部场景 ----
        for (const sc of scenes) {
            const tag = `${lang}/${sc.id}`;
            await openOutput(page, server, box, lang, sc.id);
            const audit = await page.eval(NAME_AUDIT);
            run.check(audit.interactive >= 12, `${tag}：应有足够多的可交互件（防空转），实得 ${audit.interactive}`);
            run.eq(audit.missing, [], `${tag}：这些可交互件没有可访问名称`);
            const band = await page.eval(
                `(() => { const b = document.querySelector('${gb("band")}'); return b ? { role: b.getAttribute("role"), tone: b.getAttribute("data-tone"), state: b.getAttribute("data-state") } : null; })()`,
            );
            run.eq(
                band && band.role,
                band && band.tone === "red" ? "alert" : "status",
                `${tag}：消息位的角色应随颜色（红 alert，其余 status），实得 ${JSON.stringify(band)}`,
            );
            run.eq(await page.eval("document.documentElement.lang"), lang, `${tag}：<html lang>`);
        }

        // ---- ② 整窗的角色与状态 ----
        await openOutput(page, server, box, lang, "out-connected");
        const tag = lang;
        run.eq(await page.eval("document.documentElement.lang"), lang, `${tag}：<html lang> 应等于界面语言`);
        const q = (sel) =>
            page.eval(
                `(() => { const e = document.querySelector(${JSON.stringify(sel)}); if (!e) return null; return { role: e.getAttribute("role"), tag: e.tagName, label: e.getAttribute("aria-label") || "", title: e.getAttribute("title") || "", text: (e.textContent || "").trim() }; })()`,
            );
        const pair = await q(gb("pair"));
        run.check(
            pair && pair.role === "group" && pair.label.length > 0,
            `${tag}：配对卡应是带名称的 group，实得 ${JSON.stringify(pair)}`,
        );
        const pill = await q(gb("pair.pill"));
        run.check(
            pill && pill.text.length > 0,
            `${tag}：配对状态灯应有文字（状态不能只靠颜色），实得 ${JSON.stringify(pill)}`,
        );
        const help = await q(gb("footer.help"));
        run.check(
            help && help.tag === "BUTTON" && help.label.length > 0,
            `${tag}：「?」应是带名称的 button，实得 ${JSON.stringify(help)}`,
        );
        const scale = await q(`${gb("footer.scale")} select`);
        run.check(
            scale && scale.tag === "SELECT" && scale.label.length > 0,
            `${tag}：缩放下拉应是带名称的 select，实得 ${JSON.stringify(scale)}`,
        );
        const ver = await q(gb("footer.version"));
        run.check(
            ver && ver.tag === "BUTTON" && ver.text.length > 0,
            `${tag}：版本按钮应是带文字的 button，实得 ${JSON.stringify(ver)}`,
        );
        const lcap = await page.eval(
            `(() => { const r = document.querySelector('${gb("lang")}'); const tabs = [...r.querySelectorAll('[role="tab"]')]; return { role: r.getAttribute("role"), label: r.getAttribute("aria-label") || "", tabs: tabs.length, selected: tabs.filter((t) => t.getAttribute("aria-selected") === "true").length, names: tabs.map((t) => (t.textContent || "").trim()) }; })()`,
        );
        run.eq(lcap.role, "tablist", `${tag}：语言胶囊应是 tablist`);
        run.check(lcap.label.length > 0, `${tag}：语言胶囊应有名称`);
        run.eq([lcap.tabs, lcap.selected], [3, 1], `${tag}：语言胶囊应有 3 个 tab、恰好 1 个被选中`);
        run.check(
            lcap.names.every((n) => n.length > 0),
            `${tag}：每个语言 tab 都应有文字`,
        );
        const grp = await page.eval(
            `(() => { const r = document.querySelector('${gb("group")}'); const bar = r.querySelector('[role="radiogroup"]'); const radios = [...r.querySelectorAll('[role="radio"]')]; return { bar: Boolean(bar), n: radios.length, checked: radios.filter((x) => x.getAttribute("aria-checked") === "true").length, stops: radios.filter((x) => x.tabIndex === 0).length }; })()`,
        );
        run.check(grp.bar, `${tag}：组选择应有 radiogroup`);
        run.eq([grp.n, grp.checked, grp.stops], [8, 1, 1], `${tag}：8 个 radio、恰好 1 个选中、恰好 1 个 tab 停靠点`);

        // ---- ③ 键盘：组选择 ----
        await page.eval(`document.querySelector('${gb("group")} [role="radio"][tabindex="0"]').focus()`);
        const before = await page.eval("window.__fx.calls.length");
        await press(page, "ArrowRight");
        const afterArrow = await focusOf(page);
        run.eq(afterArrow && afterArrow.group, "B", `${tag}：方向键右应把焦点移到 B`);
        run.eq(await page.eval("window.__fx.calls.length"), before, `${tag}：方向键只移焦点，不触发选组`);
        await press(page, "Space");
        run.eq(await callsOf(page, "setGroup"), [["B"]], `${tag}：空格应选中焦点所在的组`);
        await page.eval(`document.querySelector('${gb("group")} [role="radio"][data-group="D"]').focus()`);
        await press(page, "Enter");
        run.eq(await callsOf(page, "setGroup"), [["B"], ["D"]], `${tag}：回车应选中焦点所在的组`);

        // ---- ③ 键盘：接管确认条期间其余组不可选 ----
        await openOutput(page, server, box, lang, "out-takeover-confirm");
        const dis = await page.eval(
            `[...document.querySelectorAll('${gb("group")} [role="radio"]')].map((r) => [r.getAttribute("data-group"), r.getAttribute("aria-checked"), r.getAttribute("aria-disabled")])`,
        );
        run.eq(
            dis.filter((x) => x[2] === "true").length,
            7,
            `${tag}：确认条展开期间除选中组外 7 枚 radio 应 aria-disabled，实得 ${JSON.stringify(dis)}`,
        );
        await click(page, `${gb("group")} [role="radio"][aria-disabled="true"]`);
        run.eq(await callsOf(page, "setGroup"), [], `${tag}：禁用的组点了不应调 setGroup`);

        // ---- ③ 键盘：关于面板的开关与焦点 ----
        await openOutput(page, server, box, lang, "out-connected");
        await page.eval(
            `(() => { const v = document.querySelector('${gb("footer.version")}'); v.focus(); v.click(); })()`,
        );
        await page.waitFor(
            `Boolean(document.querySelector('${gb("about")}')) && !document.querySelector('${gb("about")}').closest('[hidden]')`,
            { label: `${tag}：点版本按钮应打开关于面板` },
        );
        const dlg = await page.eval(
            `(() => { const d = document.querySelector('${gb("about")}'); const by = d.getAttribute("aria-labelledby"); const lab = by && document.getElementById(by); const shellContent = document.querySelector(".rn-shell__content"); return { role: d.getAttribute("role"), modal: d.getAttribute("aria-modal"), labelled: Boolean(lab && lab.textContent.trim()), focusIn: d.contains(document.activeElement), inert: shellContent.hasAttribute("inert") }; })()`,
        );
        run.eq(
            dlg,
            { role: "dialog", modal: "true", labelled: true, focusIn: true, inert: true },
            `${tag}：关于面板应是带名称的模态对话框，焦点在里面，外壳内容 inert`,
        );
        const ctl = await page.eval(
            `(() => { const d = document.querySelector('${gb("about")}'); const sw = [...d.querySelectorAll('[role="switch"]')]; return { switches: sw.length, switchOk: sw.every((s) => ["true", "false"].includes(s.getAttribute("aria-checked")) && (s.getAttribute("aria-label") || s.getAttribute("aria-labelledby") || s.textContent.trim())) }; })()`,
        );
        run.check(
            ctl.switches >= 1 && ctl.switchOk,
            `${tag}：关于面板的开关应是带状态与名称的 switch，实得 ${JSON.stringify(ctl)}`,
        );
        const dlgNames = await page.eval(NAME_AUDIT);
        run.eq(dlgNames.missing, [], `${tag}：关于面板里这些可交互件没有可访问名称`);
        // 对话框内 Tab 循环：焦点不会跑到外壳内容里（那里 inert）
        const dlgStops = await page.eval(
            `(() => { const d = document.querySelector('${gb("about")}'); return [...d.querySelectorAll('button, select, input, a[href], [tabindex]')].filter((e) => e.tabIndex >= 0 && !e.disabled && !e.hidden && !e.closest("[hidden]") && e.getClientRects().length > 0).length; })()`,
        );
        run.check(dlgStops >= 3, `${tag}：对话框里应有足够多的 Tab 停靠点（防空转），实得 ${dlgStops}`);
        for (let i = 0; i < dlgStops + 2; i++) {
            await press(page, "Tab");
            const inShell = await page.eval(
                `document.querySelector(".rn-shell__content").contains(document.activeElement)`,
            );
            if (inShell) {
                run.check(false, `${tag}：对话框打开时按 Tab 第 ${i + 1} 次焦点落进了外壳内容`);
                break;
            }
        }
        // 焦点走出对话框后（没有焦点陷阱，外壳内容 inert，焦点去了文档外）把焦点放回对话框再按 Esc
        await page.eval(`document.querySelector('${gb("about")}').querySelector("button").focus()`);
        await press(page, "Escape");
        await page.eval("new Promise((r) => setTimeout(r, 500))");
        run.eq(
            await page.eval(`document.querySelector('${gb("about")}').closest('[hidden]') !== null`),
            true,
            `${tag}：Esc 应关闭关于面板`,
        );
        const back = await page.eval(
            `(() => ({ focusBack: document.activeElement === document.querySelector('${gb("footer.version")}'), inert: document.querySelector(".rn-shell__content").hasAttribute("inert") }))()`,
        );
        run.eq(back, { focusBack: true, inert: false }, `${tag}：关闭后焦点应回到版本按钮，外壳内容不再 inert`);

        // ---- ③ 键盘：Tab 走一圈，每一站都有看得见的焦点环 ----
        await openOutput(page, server, box, lang, "out-connected");
        const stops = await tabWalk(page);
        run.check(stops.length >= 5, `${tag}：Tab 应至少走过 5 站（语言、组、缩放、版本、？），实得 ${stops.length}`);
        run.eq(
            stops.filter((s) => !s.ring).map((s) => s.id),
            [],
            `${tag}：这些 Tab 停靠点没有看得见的焦点环`,
        );
        // 红色消息态下也走一圈（动作按钮是新增的停靠点）
        await openOutput(page, server, box, lang, "out-conflict");
        const stops2 = await tabWalk(page);
        run.eq(
            stops.filter((s) => s.part === "band.action").length,
            0,
            `${tag}：已连接态没有消息位动作按钮这一站（对照）`,
        );
        run.check(
            stops2.some((s) => s.part === "band.action" && s.action === "repair"),
            `${tag}：冲突态应有消息位的「重新配对」动作按钮这一站，实得 ${JSON.stringify(stops2.map((s) => [s.part, s.action, s.id]))}`,
        );
        run.eq(
            stops2.filter((s) => !s.ring).map((s) => s.id),
            [],
            `${tag}：冲突态下这些 Tab 停靠点没有看得见的焦点环`,
        );

        // ---- 缩放确认与轻提示的角色 ----
        await openOutput(page, server, box, lang, "out-scale-confirm");
        const d = await page.eval(
            `(() => { const d = document.querySelector('${gb("scale.confirm")}'); const by = d.getAttribute("aria-labelledby"); const lab = by && document.getElementById(by); return { role: d.getAttribute("role"), modal: d.getAttribute("aria-modal"), labelled: Boolean(lab && lab.textContent.trim()) }; })()`,
        );
        run.eq(d, { role: "dialog", modal: "true", labelled: true }, `${tag}：缩放确认应是带名称的模态对话框`);
        await openOutput(page, server, box, lang, "out-connected");
        await click(page, `${gb("group")} [data-group="C"]`);
        await awaitState(
            () =>
                page.eval(
                    `(() => { const t = document.querySelector('${gb("toast")}'); return t && !t.hidden ? t.textContent.trim() : ""; })()`,
                ),
            (s) => s.length > 0,
            "选组成功的提示",
        );
        run.eq(
            await page.eval(`document.querySelector('${gb("toast")}').getAttribute("role")`),
            "status",
            `${tag}：轻提示应是 status 且有文字`,
        );
    }

    // ---- ④ 坏样例 ----
    await freshPage();
    await openOutput(page, server, box, "zh", "out-connected", "&defect=noname");
    const bad = await page.eval(NAME_AUDIT);
    run.check(bad.missing.length >= 1, `坏样例 noname：名称审计应抓出缺名称的件，实得 ${JSON.stringify(bad.missing)}`);

    await freshPage();
    await openOutput(page, server, box, "zh", "out-connected", "&defect=noring");
    const noRing = (await tabWalk(page)).filter((x) => !x.ring);
    run.check(noRing.length >= 4, `坏样例 noring：应报出没有焦点环的停靠点，实得 ${noRing.length} 个`);

    errors.push(...page.errors);
    run.check(errors.length === 0, `页面不应有错误：${errors.slice(0, 3).join("；")}`);
} finally {
    await browser.close();
    await server.close();
}
run.finish();
