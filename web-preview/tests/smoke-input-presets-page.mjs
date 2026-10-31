// SPDX-License-Identifier: GPL-3.0-or-later
//
// Input 页面预设面板与十六档时值下拉冒烟（无头浏览器），真源页面经预览壳页装载，对 mock 桥。
//   ① （名称清洗、契约原因码、mock 语义、档位顺序对拍等不需要浏览器的部分在 smoke-input-presets-model.mjs）；
//   ② 三语：按钮的标签、当前名与「 *」（跟 state.preset.dirty）、面板各子态的文案（列表、保存输入、名称为空、重名、
//      非法字符、保存失败、删除确认）、四条 toast，全部取自词条表；工厂预设的名字取 rinne.presets 里当前语言的那一份；
//   ③ 几何：面板右缘不过 524、行高 24、用户段超过 3 行时在段内滚动而面板不长高、长名省略并带完整 title；
//      面板打开时紧急切断仍可点（elementFromPoint 与真点），点它会先收起面板；
//   ④ 流程（对桥的实际调用）：载入、保存新预设、重名覆盖、覆盖当前、删除当前（含确认与取消）、清洗后的名字发给桥、
//      失败回执（载入失败、保存失败、宿主回 exists、覆盖失败、删除失败）、载入时「淡入中」出现；
//   ⑤ 键盘与外点：方向键下拉开面板、↑↓/Home/End/Enter、Esc 收起并还焦点、Tab 在面板内循环、点面板外收起且这一下点击
//      照常生效（不吞）；
//   ⑥ 时值下拉：十六档的顺序与 UI_BRIDGE、参数表、mock 契约三处逐档一致，三语标签，选中走 setParam，三连音与附点档带说明；
//   ⑦ 就绪前禁写：首批快照没到时按钮置灰且在 inert 内，真点与合成点击都开不了面板；分帧到达时（丢掉 rinne.presets，或丢掉
//      rinne.pair）强行放开按钮的合成点击与 ↓ 也开不了；缺的那一帧一到（防空转）面板能开；
//   ⑧ 预览场景 in-preset-menu / in-preset-save 摆出的子态。
// 页面级的适配、对比度、可访问性、紧急切断命中另在各自的 smoke-input-*-page 里把 in-preset-* 场景纳入范围。
// 浏览器缺失时整套跳过（退出码 2），不是通过。本机同时只能有一个浏览器冒烟。
//
// 用法：node web-preview/tests/smoke-input-presets-page.mjs [仓库根]

import fs from "node:fs";
import path from "node:path";
import { pathToFileURL } from "node:url";
import { createRun, repoRoot } from "./lib/harness.mjs";
import { findBrowser, launchBrowser } from "./lib/browser.mjs";
import { startStaticServer } from "./lib/static-server.mjs";
import { LANGS, VIEW, centerOf, inFrame, openInput, recordBridge, settle } from "./smoke-input-kit-page.mjs";

const ROOT = repoRoot();
const run = createRun("Input 页面预设面板与时值下拉");

const u = (rel) => pathToFileURL(path.join(ROOT, rel)).href;
const common = (await import(u("web/i18n/common.js"))).default;
const inputDict = (await import(u("web/i18n/input.js"))).default;
const { FACTORY_PRESETS } = await import(u("web-preview/mock/contract.js"));

/** 某语言的取词条函数，占位符 {k} 按 vals 填。 */
const T =
    (lang) =>
    (key, vals = {}) => {
        const table = { ...common[lang], ...inputDict[lang] };
        const raw = Object.prototype.hasOwnProperty.call(table, key) ? table[key] : `!!${key}`;
        return raw.replace(/\{(\w+)\}/g, (m, k) => (k in vals ? String(vals[k]) : m));
    };

if (!findBrowser()) run.skip("没有 Chromium 系浏览器");

// ---------------------------------------------------------------- 驱动与探针
const MENU = '[data-gb="input.preset.menu"]';
const TRIG = '[data-gb="input.preset.trigger"]';
const PANIC = '[data-gb="input.panic"]';

const KEY = {
    Tab: { key: "Tab", code: "Tab", vk: 9 },
    Escape: { key: "Escape", code: "Escape", vk: 27 },
    ArrowUp: { key: "ArrowUp", code: "ArrowUp", vk: 38 },
    ArrowDown: { key: "ArrowDown", code: "ArrowDown", vk: 40 },
    Home: { key: "Home", code: "Home", vk: 36 },
    End: { key: "End", code: "End", vk: 35 },
    Enter: { key: "Enter", code: "Enter", vk: 13, text: "\r" },
    Backspace: { key: "Backspace", code: "Backspace", vk: 8 },
};

async function press(page, name, modifiers = 0) {
    const k = KEY[name];
    const base = { key: k.key, code: k.code, windowsVirtualKeyCode: k.vk, nativeVirtualKeyCode: k.vk, modifiers };
    await page.send("Input.dispatchKeyEvent", {
        type: k.text ? "keyDown" : "rawKeyDown",
        ...base,
        ...(k.text ? { text: k.text } : {}),
    });
    await page.send("Input.dispatchKeyEvent", { type: "keyUp", ...base });
    await page.eval("new Promise((r) => setTimeout(r, 60))");
}

/** 壳页坐标里真点一下。 */
async function click(page, x, y) {
    const base = { x, y, button: "left", clickCount: 1, pointerType: "mouse" };
    await page.send("Input.dispatchMouseEvent", { type: "mouseMoved", x, y });
    await page.send("Input.dispatchMouseEvent", { type: "mousePressed", ...base });
    await page.send("Input.dispatchMouseEvent", { type: "mouseReleased", ...base });
    await page.eval("new Promise((r) => setTimeout(r, 120))");
}
const clickSel = async (page, sel) => {
    const c = await centerOf(page, sel);
    await click(page, c.x, c.y);
};
const sleep = (page, ms) => page.eval(`new Promise((r) => setTimeout(r, ${ms}))`);
const focusSel = (page, sel) =>
    inFrame(
        page,
        (doc, win, s) => {
            win.focus();
            doc.querySelector(s).focus();
        },
        sel,
    );

/** 往输入框里替换文字：先全选再用 Input.insertText 插入（真输入路径）；空串直接清空并派发 input（退格键在无头模式下不稳）。 */
async function typeInto(page, sel, text) {
    await inFrame(
        page,
        (doc, win, s, clear) => {
            win.focus();
            const e = doc.querySelector(s);
            e.focus();
            e.select();
            if (clear) {
                e.value = "";
                e.dispatchEvent(new win.Event("input", { bubbles: true }));
            }
        },
        sel,
        text === "",
    );
    if (text !== "") await page.send("Input.insertText", { text });
    await sleep(page, 60);
}

/** 面板与按钮的整体读数。 */
function probe(doc, win) {
    const q = (s) => doc.querySelector(s);
    const root = q('[data-gb="input.preset.menu"]');
    const trig = q('[data-gb="input.preset.trigger"]');
    const shell = q('[data-gb="input.shell"]').getBoundingClientRect();
    const shown = (e) => !!e && !e.closest("[hidden]");
    const txt = (e) => (e ? e.textContent : null);
    const rect = (e) => {
        const r = e.getBoundingClientRect();
        return {
            l: r.left - shell.left,
            t: r.top - shell.top,
            r: r.right - shell.left,
            b: r.bottom - shell.top,
            w: r.width,
            h: r.height,
        };
    };
    const part = (p) => root.querySelector(`[data-part="${p}"]`);
    const btn = (e) =>
        e
            ? {
                  text: e.textContent,
                  disabled: e.disabled,
                  shown: shown(e),
                  h: e.getBoundingClientRect().height,
                  title: e.getAttribute("title"),
              }
            : null;
    const dirty = trig.querySelector(".in-pt__dirty");
    const toast = q('[data-gb="input.toast"]');
    const list = part("preset.user.list");
    const open = shown(root);
    const out = {
        open,
        state: root.getAttribute("data-state"),
        trig: {
            label: txt(trig.querySelector(".in-pt__label")),
            name: txt(trig.querySelector(".in-pt__text")),
            nameTitle: trig.querySelector(".in-pt__text").getAttribute("title"),
            nameCut:
                trig.querySelector(".in-pt__text").scrollWidth > trig.querySelector(".in-pt__text").clientWidth + 1,
            dirty: shown(dirty) ? txt(dirty) : null,
            dirtyTitle: dirty.getAttribute("title"),
            disabled: trig.disabled,
            expanded: trig.getAttribute("aria-expanded"),
            haspopup: trig.getAttribute("aria-haspopup"),
            state: trig.getAttribute("data-state"),
            inert: !!trig.closest("[inert]"),
        },
        toast:
            toast && !toast.hidden
                ? {
                      text: txt(q(".rn-toast__text")),
                      state: toast.getAttribute("data-state"),
                      tone: toast.getAttribute("data-tone"),
                  }
                : null,
        fade: !!q('[data-fade="1"]'),
        active: doc.activeElement
            ? {
                  inside: root.contains(doc.activeElement),
                  id: doc.activeElement.getAttribute("data-id"),
                  part: doc.activeElement.getAttribute("data-part"),
                  gb: doc.activeElement.getAttribute("data-gb"),
                  tag: doc.activeElement.tagName,
              }
            : null,
    };
    if (!open) return out;
    out.rect = rect(root);
    out.role = root.getAttribute("role");
    out.ariaLabel = root.getAttribute("aria-label");
    out.rows = [...root.querySelectorAll(".in-pm__opt")].map((r) => {
        const name = r.querySelector(".in-pm__name");
        return {
            id: r.getAttribute("data-id"),
            kind: r.getAttribute("data-kind"),
            text: name.textContent,
            title: name.getAttribute("title"),
            role: r.getAttribute("role"),
            selected: r.getAttribute("aria-selected"),
            chk: !r.firstChild.hidden,
            tab: r.tabIndex,
            h: r.getBoundingClientRect().height,
            ellipsis: name.scrollWidth > name.clientWidth,
        };
    });
    out.labels = {
        factory: txt(root.querySelector(".in-pm__label")),
        user: txt(root.querySelectorAll(".in-pm__label")[1]),
        empty: shown(part("preset.user.empty")) ? txt(part("preset.user.empty")) : null,
        note: txt(root.querySelector(".in-pm__note")),
        listRole: root.querySelector('[role="listbox"]')
            ? root.querySelector('[role="listbox"]').getAttribute("aria-label")
            : null,
    };
    out.userList = shown(list)
        ? {
              clientH: list.clientHeight,
              scrollH: list.scrollHeight,
              overflowY: win.getComputedStyle(list).overflowY,
              scrollTop: list.scrollTop,
              top: rect(list).t,
          }
        : null;
    const actList = root.querySelector(".in-pm__acts");
    out.acts = shown(actList)
        ? {
              saveAs: btn(part("preset.saveAs")),
              overwrite: btn(part("preset.overwrite")),
              delete: btn(part("preset.delete")),
          }
        : null;
    const conf = part("preset.delete.confirm");
    out.del = shown(conf)
        ? {
              text: txt(conf.querySelector(".in-pm__delmsg")),
              cancel: btn(conf.querySelectorAll("button")[0]),
              go: btn(conf.querySelectorAll("button")[1]),
              tone: conf.getAttribute("data-tone"),
          }
        : null;
    const field = part("preset.name.input");
    out.save = shown(field)
        ? {
              value: field.value,
              placeholder: field.getAttribute("placeholder"),
              ariaLabel: field.getAttribute("aria-label"),
              focused: doc.activeElement === field,
              msg: shown(part("preset.msg"))
                  ? { text: txt(part("preset.msg")), tone: part("preset.msg").getAttribute("data-tone") }
                  : null,
              cancel: btn(root.querySelectorAll(".in-pm__save .in-pm__btn")[0]),
              save: btn(root.querySelectorAll(".in-pm__save .in-pm__btn")[1]),
          }
        : null;
    return out;
}
const P = (page) => inFrame(page, probe);

/** 等面板满足条件。 */
const waitMenu = (page, expr, label) =>
    page.waitFor(
        `(() => { const d = document.querySelector("iframe").contentDocument; const r = d.querySelector('${MENU}'); return !!r && ${expr}; })()`,
        { timeout: 6000, label },
    );
const waitClosed = (page, label) =>
    page.waitFor(
        `(() => { const d = document.querySelector("iframe").contentDocument; const r = d.querySelector('${MENU}'); return !!r && r.hidden; })()`,
        { timeout: 6000, label },
    );
const waitToast = (page, state, label) =>
    page.waitFor(
        `(() => { const d = document.querySelector("iframe").contentDocument; const t = d.querySelector('[data-gb="input.toast"]'); return !!t && !t.hidden && t.getAttribute("data-state") === ${JSON.stringify(state)}; })()`,
        { timeout: 6000, label },
    );

const mockCall = (page, name, ...args) =>
    page.eval(`JSON.stringify(window.__RINNE_PREVIEW__.mock.${name}(...${JSON.stringify(args)}))`);
const model = (page, expr) =>
    page
        .eval(`JSON.stringify((() => { const m = window.__RINNE_PREVIEW__.model; return ${expr}; })())`)
        .then((s) => JSON.parse(s));

/** 让桥的某个函数回固定结果（同时记调用），返回恢复函数。 */
async function forceResult(page, name, result) {
    await page.eval(`(() => {
        const m = window.__RINNE_PREVIEW__.mock;
        window.__origs = window.__origs || {};
        window.__origs.${name} = m.${name};
        m.${name} = (...a) => { window.__calls.push(["${name}", ...a]); return ${JSON.stringify(result)}; };
    })()`);
    return () =>
        page.eval(`(() => { const m = window.__RINNE_PREVIEW__.mock; m.${name} = window.__origs.${name}; })()`);
}
const bridgeCalls = async (page, names) =>
    (await page.eval(`window.__calls.slice()`)).filter((c) => names.includes(c[0]));
const RECORDED = ["loadPreset", "savePreset", "deletePreset", "setParam", "setLang", "panic"];

/** 往 mock 里塞用户预设（经桥函数，页面随 rinne.presets 的重发更新）。 */
async function seedUsers(page, names) {
    for (const n of names) await mockCall(page, "savePreset", n, true);
    await sleep(page, 150);
}

/** 打开某场景并装上调用记录。 */
async function fresh(page, server, scene, lang) {
    await openInput(page, server, scene, { lang });
    return recordBridge(page, RECORDED);
}
const openMenu = async (page) => {
    await clickSel(page, TRIG);
    await waitMenu(page, "!r.hidden", "点按钮后面板应打开");
};

const factoryName = (id, lang) => FACTORY_PRESETS.find((p) => p.id === id).name[lang];
const FACTORY_IDS = FACTORY_PRESETS.map((p) => p.id);

// ---------------------------------------------------------------- 浏览器部分
const server = await startStaticServer(ROOT);
const browser = await launchBrowser({ width: VIEW.w, height: VIEW.h });
try {
    const page = await browser.newPage();

    // ====== ②③ 三语：按钮、列表、各子态文案、几何、toast
    for (const lang of LANGS) {
        const t = T(lang);
        const tag = `[${lang}]`;
        await fresh(page, server, "in-playing", lang);

        // 按钮
        let p = await P(page);
        run.eq(p.trig.label, t("in.preset.label"), `${tag}按钮标签`);
        run.eq(p.trig.name, factoryName("slapback", lang), `${tag}按钮上的当前名应是 rinne.presets 里当前语言的那一份`);
        run.eq(p.trig.dirty, " *", `${tag}state.preset.dirty 为真时名字后应有「 *」`);
        run.eq(p.trig.nameCut, false, `${tag}当前名连同「 *」在按钮里放得下，不被省略号截掉（法语靠收紧标签字距）`);
        run.eq(p.trig.dirtyTitle, t("in.preset.dirtyTip"), `${tag}「 *」的悬停说明`);
        run.eq(
            [p.trig.disabled, p.trig.inert, p.trig.haspopup, p.trig.expanded],
            [false, false, "dialog", "false"],
            `${tag}就绪后按钮可用，aria-haspopup=dialog，收起时 aria-expanded=false`,
        );
        run.eq(p.open, false, `${tag}面板默认收起`);

        // 「 *」跟 dirty：模型里置为不脏，按钮上的标记应消失
        await page.eval(
            `(() => { window.__RINNE_PREVIEW__.model.presetDirty = false; window.__RINNE_PREVIEW__.core.scheduleEcho(); })()`,
        );
        await page.waitFor(
            `!document.querySelector("iframe").contentDocument.querySelector('.in-pt__dirty') || document.querySelector("iframe").contentDocument.querySelector('.in-pt__dirty').hidden`,
            { label: `${tag}dirty 为假时「 *」应消失` },
        );
        run.eq((await P(page)).trig.dirty, null, `${tag}dirty 为假时不应有「 *」`);
        await page.eval(
            `(() => { window.__RINNE_PREVIEW__.model.presetDirty = true; window.__RINNE_PREVIEW__.core.scheduleEcho(); })()`,
        );
        await page.waitFor(
            `document.querySelector("iframe").contentDocument.querySelector('.in-pt__dirty') && !document.querySelector("iframe").contentDocument.querySelector('.in-pt__dirty').hidden`,
            { label: `${tag}dirty 为真时「 *」应出现` },
        );

        // 列表态
        await openMenu(page);
        p = await P(page);
        run.eq(p.state, "list", `${tag}打开后是列表态`);
        run.eq(
            [p.role, p.ariaLabel, p.labels.listRole],
            ["dialog", t("in.preset.label"), t("in.preset.label")],
            `${tag}面板是有名称的 dialog，里面的 listbox 也有名称`,
        );
        run.eq(p.trig.expanded, "true", `${tag}打开后 aria-expanded=true`);
        run.eq(
            p.rows.filter((r) => r.kind === "factory").map((r) => r.id),
            FACTORY_IDS,
            `${tag}工厂预设八个，顺序同 rinne.presets`,
        );
        run.eq(
            p.rows.filter((r) => r.kind === "factory").map((r) => r.text),
            FACTORY_PRESETS.map((x) => x.name[lang]),
            `${tag}工厂预设的名字应取当前语言`,
        );
        run.eq(
            p.rows.filter((r) => r.chk).map((r) => r.id),
            ["slapback"],
            `${tag}只有当前预设带勾`,
        );
        run.eq(
            p.rows.filter((r) => r.selected === "true").map((r) => r.id),
            ["slapback"],
            `${tag}只有当前预设 aria-selected=true`,
        );
        run.eq(
            p.rows.filter((r) => r.tab === 0).map((r) => r.id),
            ["slapback"],
            `${tag}漫游焦点（tabindex=0）只在当前预设上`,
        );
        run.check(
            p.rows.every((r) => r.role === "option" && Math.abs(r.h - 24) < 0.6),
            `${tag}每个条目是 option 且命中区高 24`,
        );
        run.eq(
            [p.labels.factory, p.labels.user, p.labels.empty, p.labels.note],
            [t("in.preset.factory"), t("in.preset.user"), t("in.preset.empty"), t("in.preset.note")],
            `${tag}分段标签、空用户段提示与底注`,
        );
        run.eq(p.acts.saveAs.text, t("in.preset.saveAs"), `${tag}「保存为新预设」`);
        run.eq(p.acts.overwrite.text, t("in.preset.overwrite", { name: "—" }), `${tag}当前是工厂预设时覆盖项无名字`);
        run.eq(
            [p.acts.overwrite.disabled, p.acts.delete.disabled, p.acts.saveAs.disabled],
            [true, true, false],
            `${tag}当前是工厂预设：覆盖与删除置灰，保存为新预设可用`,
        );
        run.check(
            [p.acts.saveAs, p.acts.overwrite, p.acts.delete].every((b) => Math.abs(b.h - 24) < 0.6),
            `${tag}动作行命中区高 24`,
        );
        run.check(
            p.rect.r <= 524.5 && p.rect.l >= 253.5,
            `${tag}面板右缘应不过 524（设计盒坐标），实得 ${p.rect.l.toFixed(1)}–${p.rect.r.toFixed(1)}`,
        );
        run.check(p.rect.b <= 600, `${tag}面板不应越出设计盒底边，实得 ${p.rect.b.toFixed(1)}`);
        run.eq(p.userList, null, `${tag}没有用户预设时不画用户列表`);

        // 面板打开时紧急切断仍可点：elementFromPoint 五点 + 真点
        const hit = await inFrame(
            page,
            (doc, win, sel) => {
                const e = doc.querySelector(sel);
                const r = e.getBoundingClientRect();
                const pts = [
                    [0.5, 0.5],
                    [0.15, 0.3],
                    [0.85, 0.3],
                    [0.15, 0.7],
                    [0.85, 0.7],
                ];
                return pts.filter(([x, y]) => {
                    const t = doc.elementFromPoint(r.left + r.width * x, r.top + r.height * y);
                    return t && (t === e || e.contains(t));
                }).length;
            },
            PANIC,
        );
        run.eq(hit, 5, `${tag}面板打开时紧急切断五个采样点都应落在按钮自己身上`);

        // 保存输入态与子态文案
        await clickSel(page, '[data-part="preset.saveAs"]');
        await waitMenu(page, `r.getAttribute("data-state") !== "list"`, `${tag}点「保存为新预设」应进入保存输入态`);
        p = await P(page);
        run.eq(p.state, "saveInput", `${tag}保存输入态`);
        run.eq(p.save.value, t("in.preset.defaultName", { n: 1 }), `${tag}输入框的默认名`);
        run.eq(
            [p.save.placeholder, p.save.ariaLabel],
            [t("in.preset.namePlaceholder"), t("in.preset.namePlaceholder")],
            `${tag}输入框的占位与名称`,
        );
        run.eq(
            [p.save.focused, p.save.save.disabled, p.save.msg],
            [true, false, null],
            `${tag}输入框拿焦点，保存可用，无提示`,
        );
        run.eq([p.save.cancel.text, p.save.save.text], [t("common.cancel"), t("common.save")], `${tag}保存区按钮文案`);

        await typeInto(page, '[data-part="preset.name.input"]', "");
        p = await P(page);
        run.eq(p.state, "empty", `${tag}名字为空时是 empty 态`);
        run.eq([p.save.save.disabled, p.save.msg], [true, null], `${tag}名字为空：保存置灰，不提示`);

        await typeInto(page, '[data-part="preset.name.input"]', "Vox/回旋");
        p = await P(page);
        run.eq(p.state, "badChars", `${tag}含非法字符是 badChars 态`);
        run.eq(p.save.msg, { text: t("in.preset.badChars"), tone: "amber" }, `${tag}非法字符提示（琥珀）`);
        run.eq(p.save.save.disabled, true, `${tag}含非法字符时保存置灰`);

        // 重名：先造一个用户预设
        await seedUsers(page, ["Vox 回旋"]);
        await typeInto(page, '[data-part="preset.name.input"]', "vox 回旋");
        p = await P(page);
        run.eq(p.state, "dupe", `${tag}与已有用户预设重名（不分大小写）是 dupe 态`);
        run.eq(p.save.msg, { text: t("in.preset.dupe"), tone: "amber" }, `${tag}重名提示（琥珀）`);
        run.eq(p.save.save.disabled, false, `${tag}重名时保存仍可用（按覆盖发）`);

        // 保存失败
        const restoreSave = await forceResult(page, "savePreset", { ok: false, reason: "write-failed" });
        await typeInto(page, '[data-part="preset.name.input"]', "New one");
        await clickSel(page, ".in-pm__save .sc-btn--cta");
        await waitMenu(
            page,
            `r.getAttribute("data-state") === "saveFail"`,
            `${tag}宿主回 write-failed 应进入 saveFail 态`,
        );
        p = await P(page);
        run.eq(
            p.save.msg,
            { text: t("in.preset.saveFail", { reason: "write-failed" }), tone: "red" },
            `${tag}保存失败提示（红，带原因码）`,
        );
        run.eq(p.open, true, `${tag}保存失败时面板不收起`);
        await restoreSave();
        await typeInto(page, '[data-part="preset.name.input"]', "New one!");
        run.eq((await P(page)).state, "saveInput", `${tag}改动名字后失败提示应消失`);

        // 保存成功 -> toast
        await clickSel(page, ".in-pm__save .sc-btn--cta");
        await waitToast(page, "presetSaved", `${tag}保存成功应出 toast`);
        p = await P(page);
        run.eq(p.toast.text, t("toast.presetSaved", { name: "New one!" }), `${tag}保存 toast 文案`);
        run.eq(p.toast.tone, "green", `${tag}保存 toast 是绿色`);
        run.eq(p.open, false, `${tag}保存成功后面板收起`);
        run.eq(p.trig.name, "New one!", `${tag}保存后按钮上是新预设名`);
        run.eq(p.trig.dirty, null, `${tag}保存后不再带「 *」`);

        // 用户段、覆盖与删除的文案
        await openMenu(page);
        p = await P(page);
        const users = p.rows.filter((r) => r.kind === "user");
        run.eq(
            users.map((r) => r.text),
            ["New one!", "Vox 回旋"],
            `${tag}用户预设按名字不分大小写排序`,
        );
        run.eq(
            p.rows.filter((r) => r.chk).map((r) => r.text),
            ["New one!"],
            `${tag}当前预设（刚保存的）带勾`,
        );
        run.eq(p.acts.overwrite.text, t("in.preset.overwrite", { name: "New one!" }), `${tag}覆盖项写当前预设名`);
        run.eq(p.acts.delete.text, t("in.preset.delete", { name: "New one!" }), `${tag}删除项写当前预设名`);
        run.eq(
            [p.acts.overwrite.disabled, p.acts.delete.disabled],
            [false, false],
            `${tag}当前是用户预设：覆盖与删除可用`,
        );

        await clickSel(page, '[data-part="preset.delete"]');
        await waitMenu(page, `r.getAttribute("data-state") === "deleteConfirm"`, `${tag}点「删除」应进入删除确认`);
        p = await P(page);
        run.eq(p.del.text, t("in.preset.deleteConfirm"), `${tag}删除确认文案`);
        run.eq(
            [p.del.cancel.text, p.del.go.text, p.del.tone],
            [t("common.cancel"), t("common.delete"), "amber"],
            `${tag}删除确认的按钮与琥珀色`,
        );
        run.eq(p.active.part, null, `${tag}删除确认里焦点不在条目上`);
        run.eq(p.active.inside, true, `${tag}删除确认的默认焦点在面板里（取消键）`);
        await clickSel(page, '[data-part="preset.delete.confirm"] button:last-of-type');
        await waitToast(page, "presetDeleted", `${tag}删除成功应出 toast`);
        p = await P(page);
        run.eq(p.toast.text, t("toast.presetDeleted", { name: "New one!" }), `${tag}删除 toast 文案`);
        run.eq(p.state, "list", `${tag}删除后回列表态（面板不收起）`);
        run.eq(p.trig.name, "—", `${tag}删掉当前预设后按钮写「—」`);

        // 载入 -> toast + 淡入；失败 -> 琥珀 toast
        await page.eval(
            `(() => { const d = document.querySelector("iframe").contentDocument; window.__fadeSeen = false; const mo = new MutationObserver(() => { if (d.querySelector('[data-fade="1"]')) window.__fadeSeen = true; }); mo.observe(d.body, { subtree: true, attributes: true, childList: true, attributeFilter: ["data-fade"] }); })()`,
        );
        await clickSel(page, '.in-pm__opt[data-id="pingPong"]');
        await waitToast(page, "presetLoaded", `${tag}载入成功应出 toast`);
        p = await P(page);
        run.eq(p.toast.text, t("toast.presetLoaded", { name: factoryName("pingPong", lang) }), `${tag}载入 toast 文案`);
        run.eq(p.toast.tone, "green", `${tag}载入 toast 是绿色`);
        run.eq(
            [p.open, p.trig.name, p.trig.dirty],
            [false, factoryName("pingPong", lang), null],
            `${tag}载入后面板收起，按钮写新预设名，不带「 *」`,
        );
        await page
            .waitFor(`window.__fadeSeen === true`, { timeout: 4000, label: `${tag}载入后回送应出现「淡入中」` })
            .catch(() => {});
        run.eq(await page.eval(`window.__fadeSeen === true`), true, `${tag}载入预设后应出现「淡入中」标记`);

        await openMenu(page);
        const restoreLoad = await forceResult(page, "loadPreset", { ok: false, reason: "bad-file" });
        await clickSel(page, '.in-pm__opt[data-id="init"]');
        await waitToast(page, "presetBroken", `${tag}载入失败应出 toast`);
        p = await P(page);
        run.eq([p.toast.text, p.toast.tone], [t("toast.presetBroken"), "amber"], `${tag}载入失败 toast（琥珀）`);
        run.eq(
            [p.open, p.trig.name],
            [true, factoryName("pingPong", lang)],
            `${tag}载入失败：面板不收起，当前预设不变`,
        );
        await restoreLoad();

        // 底注与整体高度（有 1 个用户预设之后）
        p = await P(page);
        run.check(p.rect.h > 300 && p.rect.h < 440, `${tag}面板整体高度应在 300–440 之间，实得 ${p.rect.h.toFixed(1)}`);
        run.check(p.rect.b <= 600 && p.rect.r <= 524.5, `${tag}面板不越界`);
        await press(page, "Escape");
        run.check(!(await P(page)).open, `${tag}Esc 收起面板`);
        run.check(page.errors.length === 0, `${tag}页面不应有错误：${page.errors.slice(0, 3).join("；")}`);
    }

    // ====== ③ 用户段滚动、长名省略、面板高度
    {
        const lang = "zh";
        const t = T(lang);
        const rec = await fresh(page, server, "in-playing", lang);
        void rec;
        const long = "我的超长预设名称用于测试截断效果甲乙";
        await seedUsers(page, ["Vox 回旋", "Long tail 2"]);
        await openMenu(page);
        const h2 = (await P(page)).rect.h;
        await seedUsers(page, [long]);
        const p3 = await P(page);
        run.eq(p3.userList.clientH, 72, "3 个用户预设：用户段高 72（刚好 3 行）");
        run.check(
            p3.userList.scrollH <= 72 + 0.5,
            `3 个用户预设：不需要滚动，实得 scrollHeight ${p3.userList.scrollH}`,
        );
        const longRow = p3.rows.find((r) => r.text === long);
        run.check(longRow && longRow.ellipsis, "超长的用户预设名应被省略号截断");
        run.eq(longRow && longRow.title, long, "超长的用户预设名应带完整 title");
        await seedUsers(page, ["Aa 1", "Bb 2", "Cc 3"]);
        const p6 = await P(page);
        run.eq(p6.userList.clientH, 72, "6 个用户预设：用户段仍高 72");
        run.check(
            p6.userList.scrollH > 72 + 20,
            `6 个用户预设：内容应超出而在段内滚动，实得 scrollHeight ${p6.userList.scrollH}`,
        );
        run.eq(p6.userList.overflowY, "auto", "用户段 overflow-y 应为 auto");
        run.eq(p6.rect.h, p3.rect.h, "用户预设从 3 个增到 6 个，面板整体高度不变（超出的部分在段内滚动）");
        run.check(p3.rect.h > h2, "2 个用户预设比 3 个矮（防空转：高度确实随行数变）");
        run.check(
            p6.rows.filter((r) => r.kind === "user").every((r) => Math.abs(r.h - 24) < 0.6),
            "用户段的行高仍是 24",
        );
        // 真滚轮
        const lc = await centerOf(page, '[data-part="preset.user.list"]');
        await page.send("Input.dispatchMouseEvent", { type: "mouseWheel", x: lc.x, y: lc.y, deltaX: 0, deltaY: 120 });
        await sleep(page, 200);
        const ps = await P(page);
        run.check(ps.userList.scrollTop > 0, `滚轮应让用户段滚动，实得 scrollTop ${ps.userList.scrollTop}`);
        run.eq([ps.rect.t, ps.rect.h], [p6.rect.t, p6.rect.h], "用户段滚动时面板自己不动");
        // 键盘到最后一个：焦点条目要滚进可见区
        await focusSel(page, ".in-pm__opt[tabindex='0']");
        await press(page, "End");
        const pe = await P(page);
        const last = pe.rows[pe.rows.length - 1];
        run.eq(pe.active.id, last.id, "End 应把焦点移到最后一个条目");
        const vis = await inFrame(
            page,
            (doc, win, id) => {
                const row = doc.querySelector(`.in-pm__opt[data-id="${id}"]`);
                const list = doc.querySelector('[data-part="preset.user.list"]');
                const a = row.getBoundingClientRect();
                const b = list.getBoundingClientRect();
                return a.top >= b.top - 0.5 && a.bottom <= b.bottom + 0.5;
            },
            last.id,
        );
        run.check(vis, "焦点移到用户段最后一个条目时，它应滚进用户段的可见区");
        void t;
    }

    // ====== ④ 流程（中文）：对桥的实际调用
    {
        const lang = "zh";
        const rec = await fresh(page, server, "in-playing", lang);
        // 载入工厂预设：桥收到 loadPreset(id)
        await openMenu(page);
        await rec.clear();
        await clickSel(page, '.in-pm__opt[data-id="roundDub"]');
        await waitToast(page, "presetLoaded", "载入");
        run.eq(
            await bridgeCalls(page, ["loadPreset"]),
            [["loadPreset", "roundDub"]],
            "点工厂预设，桥应恰好收到 loadPreset(id)",
        );
        run.eq(await model(page, "m.presetId"), "roundDub", "mock 的当前预设应换成所点的");

        // 保存：清洗后的名字发给桥；新名 overwrite=false
        await openMenu(page);
        await rec.clear();
        await clickSel(page, '[data-part="preset.saveAs"]');
        await waitMenu(page, `r.getAttribute("data-state") !== "list"`, "进入保存态");
        // 带控制字符与首尾空白的名字：控制字符在输入框里无法键入，直接设值并派发 input
        await inFrame(page, (doc, win) => {
            const e = doc.querySelector('[data-part="preset.name.input"]');
            e.value = `  Echo${String.fromCharCode(1)} A${String.fromCharCode(0xa0)}  `;
            e.dispatchEvent(new win.Event("input", { bubbles: true }));
        });
        await sleep(page, 60);
        run.eq((await P(page)).state, "saveInput", "带控制字符与首尾空白的名字清洗后合法，应是 saveInput 态");
        await clickSel(page, ".in-pm__save .sc-btn--cta");
        await waitToast(page, "presetSaved", "保存");
        run.eq(
            await bridgeCalls(page, ["savePreset"]),
            [["savePreset", "Echo A", false]],
            "桥应收到清洗后的名字，overwrite=false",
        );
        const idA = await model(page, "m.presetId");
        run.check(/^u-[0-9a-f]{12}$/.test(idA), `新用户预设的 ID 应是 u-<12 位十六进制>，实得 ${idA}`);

        // 重名覆盖：改个大小写再存，overwrite=true，ID 不变，列表里仍只有一个
        await openMenu(page);
        await rec.clear();
        await clickSel(page, '[data-part="preset.saveAs"]');
        await waitMenu(page, `r.getAttribute("data-state") !== "list"`, "进入保存态");
        await typeInto(page, '[data-part="preset.name.input"]', "echo a");
        run.eq((await P(page)).state, "dupe", "大小写不同的同名应判重名");
        await clickSel(page, ".in-pm__save .sc-btn--cta");
        await waitToast(page, "presetSaved", "覆盖保存");
        run.eq(
            await bridgeCalls(page, ["savePreset"]),
            [["savePreset", "echo a", true]],
            "重名保存时桥应收到 overwrite=true",
        );
        run.eq(
            await model(page, "m.userPresets.map((p) => [p.id, p.name])"),
            [[idA, "Echo A"]],
            "覆盖保存沿用原 ID，用户预设仍只有一个",
        );

        // 覆盖当前：不经输入框
        await openMenu(page);
        await rec.clear();
        await clickSel(page, '[data-part="preset.overwrite"]');
        await waitToast(page, "presetSaved", "覆盖当前");
        run.eq(
            await bridgeCalls(page, ["savePreset"]),
            [["savePreset", "Echo A", true]],
            "点「覆盖」，桥应收到当前预设名与 overwrite=true",
        );
        run.eq((await P(page)).open, false, "覆盖成功后面板收起");

        // 覆盖失败：切到保存行并显示失败
        await openMenu(page);
        const restoreOw = await forceResult(page, "savePreset", { ok: false, reason: "write-failed" });
        await clickSel(page, '[data-part="preset.overwrite"]');
        await waitMenu(page, `r.getAttribute("data-state") === "saveFail"`, "覆盖失败应进入 saveFail 态");
        let p = await P(page);
        run.eq([p.save.value, p.save.msg.tone], ["Echo A", "red"], "覆盖失败：保存行带着当前预设名，提示为红色");
        await restoreOw();

        // 宿主回 exists（页面手里的列表旧了）：按重名处理，下一次保存按覆盖发
        await clickSel(page, ".in-pm__save .in-pm__btn:first-of-type");
        await waitMenu(page, `r.getAttribute("data-state") === "list"`, "取消回列表");
        await clickSel(page, '[data-part="preset.saveAs"]');
        await waitMenu(page, `r.getAttribute("data-state") !== "list"`, "进入保存态");
        await typeInto(page, '[data-part="preset.name.input"]', "Stale name");
        await rec.clear();
        const restoreEx = await forceResult(page, "savePreset", { ok: false, reason: "exists" });
        await clickSel(page, ".in-pm__save .sc-btn--cta");
        await waitMenu(page, `r.getAttribute("data-state") === "dupe"`, "宿主回 exists 应进入 dupe 态");
        await restoreEx();
        await clickSel(page, ".in-pm__save .sc-btn--cta");
        await waitToast(page, "presetSaved", "按覆盖重发");
        run.eq(
            (await bridgeCalls(page, ["savePreset"])).map((c) => c.slice(1)),
            [
                ["Stale name", false],
                ["Stale name", true],
            ],
            "宿主回 exists 后，下一次保存应按 overwrite=true 重发",
        );

        // 删除：取消不调桥；确认调 deletePreset(id)
        await openMenu(page);
        await rec.clear();
        await clickSel(page, '[data-part="preset.delete"]');
        await waitMenu(page, `r.getAttribute("data-state") === "deleteConfirm"`, "删除确认");
        await clickSel(page, '[data-part="preset.delete.confirm"] button:first-of-type');
        await waitMenu(page, `r.getAttribute("data-state") === "list"`, "取消回列表");
        run.eq(await bridgeCalls(page, ["deletePreset"]), [], "删除确认里点取消，桥不应收到 deletePreset");
        const curId = await model(page, "m.presetId");
        await clickSel(page, '[data-part="preset.delete"]');
        await waitMenu(page, `r.getAttribute("data-state") === "deleteConfirm"`, "删除确认");
        // 删除失败：留在确认态，不出 toast
        const restoreDel = await forceResult(page, "deletePreset", { ok: false, reason: "write-failed" });
        await clickSel(page, '[data-part="preset.delete.confirm"] button:last-of-type');
        await sleep(page, 300);
        p = await P(page);
        run.eq(p.state, "deleteConfirm", "删除失败应留在确认态");
        run.check(!p.toast || p.toast.state !== "presetDeleted", "删除失败不应出「已删除」toast");
        await restoreDel();
        await rec.clear();
        await clickSel(page, '[data-part="preset.delete.confirm"] button:last-of-type');
        await waitToast(page, "presetDeleted", "删除成功");
        run.eq(
            await bridgeCalls(page, ["deletePreset"]),
            [["deletePreset", curId]],
            "确认删除，桥应收到 deletePreset(当前预设 ID)",
        );
        run.eq(await model(page, "m.presetId"), null, "mock 里删掉当前预设后没有当前预设");

        // 输入框里实时截到 24 个码点，按码点不按 UTF-16 单元：30 个普通字符与 30 个代理对字符都停在 24 个码点
        await clickSel(page, '[data-part="preset.saveAs"]');
        await waitMenu(page, `r.getAttribute("data-state") !== "list"`, "进入保存态");
        const cpCount = () =>
            inFrame(page, (doc) => Array.from(doc.querySelector('[data-part="preset.name.input"]').value).length);
        await typeInto(page, '[data-part="preset.name.input"]', "abcdefghij".repeat(3));
        run.eq(await cpCount(), 24, "输入 30 个字符，输入框里实时截到 24 个码点");
        await typeInto(page, '[data-part="preset.name.input"]', String.fromCodePoint(0x1f3b5).repeat(30));
        run.eq(await cpCount(), 24, "输入 30 个代理对字符，输入框里按码点截到 24 个（不是 24 个 UTF-16 单元）");
        await clickSel(page, ".in-pm__save .in-pm__btn:first-of-type");
        await waitMenu(page, `r.getAttribute("data-state") === "list"`, "取消回列表");

        // 工厂预设不能删：当前是工厂预设时删除置灰
        await clickSel(page, '.in-pm__opt[data-id="init"]');
        await waitToast(page, "presetLoaded", "载入 init");
        await openMenu(page);
        p = await P(page);
        run.eq([p.acts.overwrite.disabled, p.acts.delete.disabled], [true, true], "当前是工厂预设：覆盖与删除置灰");
        run.check(page.errors.length === 0, `流程：页面不应有错误：${page.errors.slice(0, 3).join("；")}`);
    }

    // ====== ⑤ 键盘与外点
    {
        const rec = await fresh(page, server, "in-playing", "zh");
        // 方向键下拉开面板，焦点落到当前预设
        await focusSel(page, TRIG);
        await press(page, "ArrowDown");
        await waitMenu(page, "!r.hidden", "按钮上按 ↓ 应打开面板");
        let p = await P(page);
        run.eq(p.active.id, "slapback", "打开后焦点应落在当前预设上");
        await press(page, "ArrowDown");
        run.eq((await P(page)).active.id, "roundDub", "↓ 移到下一个条目");
        await press(page, "ArrowUp");
        await press(page, "ArrowUp");
        run.eq((await P(page)).active.id, "init", "↑ 移到上一个条目");
        await press(page, "ArrowUp");
        run.eq((await P(page)).active.id, "init", "到顶不环绕");
        await press(page, "End");
        run.eq((await P(page)).active.id, FACTORY_IDS[FACTORY_IDS.length - 1], "End 到最后一个条目");
        await press(page, "Home");
        run.eq((await P(page)).active.id, "init", "Home 到第一个条目");
        run.eq((await P(page)).rows.filter((r) => r.tab === 0).length, 1, "漫游焦点任何时刻只有一个条目带 tabindex=0");

        // Enter 载入
        await press(page, "ArrowDown");
        await press(page, "ArrowDown");
        await rec.clear();
        await press(page, "Enter");
        await waitToast(page, "presetLoaded", "Enter 载入");
        run.eq(await bridgeCalls(page, ["loadPreset"]), [["loadPreset", "roundDub"]], "Enter 应载入焦点所在的预设");
        p = await P(page);
        run.eq([p.open, p.active.gb], [false, "input.preset.trigger"], "键盘载入后面板收起，焦点回到按钮");

        // Esc 收起并还焦点
        await press(page, "ArrowDown");
        await waitMenu(page, "!r.hidden", "再次打开");
        await press(page, "Escape");
        await waitClosed(page, "Esc 应收起面板");
        run.eq((await P(page)).active.gb, "input.preset.trigger", "Esc 收起后焦点回到按钮");

        // 每次打开都从当前预设起步：上一次停在末尾，再打开仍落在当前预设
        await press(page, "ArrowDown");
        await waitMenu(page, "!r.hidden", "再次打开");
        await press(page, "End");
        run.eq(
            (await P(page)).active.id,
            FACTORY_IDS[FACTORY_IDS.length - 1],
            "End 到最后一个条目（为下一步造出「上次停在末尾」）",
        );
        await press(page, "Escape");
        await waitClosed(page, "Esc 应收起面板");
        await press(page, "ArrowDown");
        await waitMenu(page, "!r.hidden", "第三次打开");
        run.eq((await P(page)).active.id, "roundDub", "再次打开时焦点回到当前预设，不停在上次停的末尾");
        await press(page, "Escape");
        await waitClosed(page, "Esc 应收起面板");

        // Tab 循环：列表态走一圈，焦点始终在面板内；Shift+Tab 反向
        await press(page, "ArrowDown");
        await waitMenu(page, "!r.hidden", "再次打开");
        const seen = [];
        for (let i = 0; i < 8; i++) {
            await press(page, "Tab");
            const a = (await P(page)).active;
            seen.push(a.inside);
        }
        run.check(seen.every(Boolean), `Tab 走 8 次，焦点应始终留在面板内，实得 ${JSON.stringify(seen)}`);
        for (let i = 0; i < 3; i++) await press(page, "Tab", 8);
        run.check((await P(page)).active.inside, "Shift+Tab 也应留在面板内");
        // 从最后一个再 Tab 回到第一个
        const stops = await inFrame(
            page,
            (doc) =>
                [
                    ...doc
                        .querySelector('[data-gb="input.preset.menu"]')
                        .querySelectorAll('[tabindex="0"], button, input'),
                ].filter((n) => !n.disabled && !n.closest("[hidden]")).length,
        );
        run.check(stops >= 2, `列表态应有至少 2 个 Tab 站（漫游条目、保存为新预设），实得 ${stops}`);
        await press(page, "Escape");

        // 外点：点别的控件，面板收起，这一下点击照常生效（切语言）
        await openMenu(page);
        await rec.clear();
        await clickSel(page, '[data-gb="input.lang"] [data-lang="en"]');
        await waitClosed(page, "点面板外应收起");
        run.eq(
            await bridgeCalls(page, ["setLang"]),
            [["setLang", "en"]],
            "点面板外的控件，这一下点击不应被吞（桥收到 setLang）",
        );
        // 点按钮本身：开 -> 关
        await openMenu(page);
        await clickSel(page, TRIG);
        await waitClosed(page, "再点按钮应收起");
        // 点面板内的非控件区不收起
        await openMenu(page);
        const lab = await centerOf(page, ".in-pm__note");
        await click(page, lab.x, lab.y);
        run.eq((await P(page)).open, true, "点面板内的文字区域不应收起");
        // 紧急切断：真点，面板收起，桥恰好一次 panic（面板此时仍开着）
        await rec.clear();
        await clickSel(page, PANIC);
        await waitClosed(page, "点紧急切断应收起面板");
        run.eq((await bridgeCalls(page, ["panic"])).length, 1, "面板打开时真点紧急切断，桥应恰好收到一次 panic");
        // 不经指针（键盘激活、脚本触发）按下紧急切断：没有 pointerdown 可借，面板也要收起
        await openMenu(page);
        await rec.clear();
        await inFrame(page, (doc2, win, s) => doc2.querySelector(s).click(), PANIC);
        await waitClosed(page, "不经指针触发紧急切断也应收起面板");
        run.eq((await bridgeCalls(page, ["panic"])).length, 1, "不经指针触发紧急切断，桥应恰好收到一次 panic");
    }

    // ====== ⑥ 十六档时值下拉
    {
        const doc = fs.readFileSync(path.join(ROOT, "docs/UI_BRIDGE.md"), "utf8");
        const row = doc.split("\n").find((l) => l.startsWith("| `delay_note` |"));
        const bridgeOrder = [...row.split("|")[2].matchAll(/`(n\d+[td]?)`/g)].map((m) => m[1]);
        run.eq(bridgeOrder.length, 16, "UI_BRIDGE 的取值表应列 16 档");

        for (const lang of LANGS) {
            const t = T(lang);
            const rec = await fresh(page, server, "in-delay-beats", lang);
            const sel = '[data-part="dly.note"]';
            const info = await inFrame(
                page,
                (doc2, win, s) => {
                    const e = doc2.querySelector(s);
                    const r = e.getBoundingClientRect();
                    return {
                        ids: [...e.options].map((o) => o.value),
                        texts: [...e.options].map((o) => o.textContent),
                        value: e.value,
                        title: e.getAttribute("title"),
                        label: e.getAttribute("aria-label"),
                        w: r.width,
                        h: r.height,
                        shown: !e.closest("[hidden]"),
                        disabled: e.disabled,
                    };
                },
                sel,
            );
            run.eq(info.ids, bridgeOrder, `[${lang}]下拉的选项顺序应与取值表逐档一致`);
            run.eq(
                info.texts,
                bridgeOrder.map((id) => t(`in.dly.note.${id}`)),
                `[${lang}]下拉的标签应取词条`,
            );
            run.eq(
                [info.value, info.shown, info.disabled],
                ["n8d", true, false],
                `[${lang}]场景是 1/8 附点：下拉可见可用且选中 n8d`,
            );
            run.eq(info.title, t("in.dly.noteTipD"), `[${lang}]附点档带附点说明`);
            run.eq(info.label, t("in.dly.eyebrow"), `[${lang}]下拉有可访问名称`);
            run.check(
                Math.abs(info.w - 224) < 0.6 && Math.abs(info.h - 26) < 0.6,
                `[${lang}]下拉尺寸 224×26，实得 ${info.w}×${info.h}`,
            );

            // 选中走 setParam：脚本改值并派发 change
            await rec.clear();
            await inFrame(
                page,
                (doc2, win, s) => {
                    const e = doc2.querySelector(s);
                    e.value = "n8t";
                    e.dispatchEvent(new win.Event("change", { bubbles: true }));
                },
                sel,
            );
            await page.waitFor(`window.__RINNE_PREVIEW__.model.values.delay_note === "n8t"`, {
                label: `[${lang}]选 n8t 后模型应更新`,
            });
            const setp = (await bridgeCalls(page, ["setParam"])).filter((c) => c[1] === "delay_note");
            run.eq(
                setp.map((c) => c.slice(1, 3)),
                [["delay_note", "n8t"]],
                `[${lang}]选中应走 setParam('delay_note', id)`,
            );
            const t2 = await inFrame(page, (doc2, win, s) => doc2.querySelector(s).getAttribute("title"), sel);
            run.eq(t2, t("in.dly.noteTipT"), `[${lang}]三连音档带三连音说明`);
            await inFrame(
                page,
                (doc2, win, s) => {
                    const e = doc2.querySelector(s);
                    e.value = "n8";
                    e.dispatchEvent(new win.Event("change", { bubbles: true }));
                },
                sel,
            );
            await page.waitFor(`window.__RINNE_PREVIEW__.model.values.delay_note === "n8"`, {
                label: `[${lang}]选 n8`,
            });
            run.eq(
                await inFrame(page, (doc2, win, s) => doc2.querySelector(s).getAttribute("title"), sel),
                null,
                `[${lang}]普通档没有悬停说明`,
            );
            // 读数同步：延迟卡的大读数应是所选时值的标签
            run.eq(
                await inFrame(page, (doc2) => doc2.querySelector('[data-part="dly.readout"]').textContent.trim()),
                t("in.dly.note.n8"),
                `[${lang}]选中后大读数同步`,
            );
        }

        // 真键盘：下拉获得焦点后 ↓ 选下一档（原生行为），走 setParam；顺序即取值表顺序
        {
            const rec = await fresh(page, server, "in-delay-beats", "zh");
            await focusSel(page, '[data-part="dly.note"]');
            await rec.clear();
            await press(page, "ArrowDown");
            await page.waitFor(
                `window.__RINNE_PREVIEW__.model.values.delay_note === "${bridgeOrder[bridgeOrder.indexOf("n8d") + 1]}"`,
                { label: "↓ 应选取值表里的下一档" },
            );
            run.eq(
                (await bridgeCalls(page, ["setParam"])).filter((c) => c[1] === "delay_note").map((c) => c[2]),
                [bridgeOrder[bridgeOrder.indexOf("n8d") + 1]],
                "键盘 ↓ 选到的是取值表的下一档，并走 setParam",
            );
        }
        // 毫秒档下拉不显示（槽位收起）
        await fresh(page, server, "in-playing", "zh");
        run.eq(
            await inFrame(page, (doc2) => !!doc2.querySelector('[data-part="dly.note"]').closest("[hidden]")),
            true,
            "毫秒档时下拉应收起",
        );
    }

    // ====== ⑧ 预览场景
    {
        await openInput(page, server, "in-preset-menu", { lang: "zh" });
        await waitMenu(page, "!r.hidden", "in-preset-menu 应摆出展开的面板");
        let p = await P(page);
        run.eq(p.state, "list", "in-preset-menu：列表态");
        run.eq(
            p.rows.filter((r) => r.chk).map((r) => r.id),
            ["slapback"],
            "in-preset-menu：当前预设是 slapback",
        );
        run.check(p.rect.r <= 524.5, `in-preset-menu：面板右缘不过 524，实得 ${p.rect.r.toFixed(1)}`);
        await openInput(page, server, "in-preset-save", { lang: "zh" });
        await waitMenu(page, "!r.hidden", "in-preset-save 应摆出展开的面板");
        p = await P(page);
        run.eq(p.state, "dupe", "in-preset-save：输入态加重名提示");
        run.eq(p.save.value, "Vox 回旋", "in-preset-save：输入框里是场景给的名字");
        run.eq(p.save.msg.text, T("zh")("in.preset.dupe"), "in-preset-save：重名提示文案");
    }

    // ====== ⑦ 就绪前禁写（真宿主，首批快照迟到）
    {
        const HOLD_HOOK = `(() => {
            window.__hold = true;
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
                    m.requestInitialState = function () { return window.__hold ? { ok: true } : orig.apply(m, arguments); };
                },
            });
        })()`;
        const held = await browser.newPage();
        await held.setViewport(VIEW.w, VIEW.h, 1);
        await held.send("Page.addScriptToEvaluateOnNewDocument", { source: HOLD_HOOK });
        const q = new URLSearchParams({ fresh: "1", auto: "0", lang: "zh", scenario: "in-playing" });
        await held.goto(`${server.url}/web-preview/input.html?${q}`);
        await held.waitFor(
            `(() => { const s = document.querySelector(".pv-status"); return !!s && s.getAttribute("data-ok") !== "wait"; })()`,
            { timeout: 20000, label: "壳页注入" },
        );
        await held.waitFor(
            `(() => { const f = document.querySelector("iframe"); return !!(f && f.contentDocument && f.contentDocument.querySelector('[data-gb="input.shell"]')); })()`,
            { timeout: 20000, label: "页面外壳" },
        );
        const rec = await recordBridge(held, RECORDED);
        await sleep(held, 700);

        let p = await P(held);
        run.eq(p.trig.disabled, true, "快照没到：预设按钮应置灰");
        run.eq(p.trig.inert, true, "快照没到：预设按钮应在 inert 之内");
        run.eq(p.trig.name, "—", "快照没到：预设名写「—」");
        run.eq(p.trig.dirty, null, "快照没到：不带「 *」");
        await clickSel(held, TRIG);
        run.eq((await P(held)).open, false, "快照没到：真点预设按钮开不了面板");
        await inFrame(held, (doc2, win) => {
            const b = doc2.querySelector('[data-gb="input.preset.trigger"]');
            b.disabled = false;
            b.click();
            b.dispatchEvent(new win.KeyboardEvent("keydown", { bubbles: true, cancelable: true, key: "ArrowDown" }));
        });
        await sleep(held, 150);
        run.eq((await P(held)).open, false, "快照没到：强行放开按钮并合成点击与 ↓ 也开不了面板");

        // 首个 requestInitialState 之前 mock 丢弃一切推送，这一页没法模拟「部分帧先到」；那几种情形在后面两页里真的分帧测
        run.eq(
            await bridgeCalls(held, ["loadPreset", "savePreset", "deletePreset"]),
            [],
            "快照没到：桥不应收到任何预设写入",
        );

        // 放行（防空转）
        await held.eval(`(() => { window.__hold = false; window.__origInitial(); })()`);
        await held.waitFor(
            `(() => { const f = document.querySelector("iframe"); const p = f.contentDocument.querySelector('[data-gb="input.pair.pill"]'); return !!p && p.hasAttribute("data-state"); })()`,
            { timeout: 8000, label: "放行后页面应画出首批快照" },
        );
        await settle(held);
        p = await P(held);
        run.eq([p.trig.disabled, p.trig.inert], [false, false], "放行后按钮可用且不再 inert");
        run.eq(p.trig.name, factoryName("slapback", "zh"), "放行后按钮写当前预设名");
        await openMenu(held);
        run.eq((await P(held)).open, true, "放行后（防空转）面板能开");
        run.check(held.errors.length === 0, `就绪前：页面不应有错误：${held.errors.slice(0, 3).join("；")}`);
        await held.close();

        // 另起两页：宿主可以分帧发，丢掉其中一帧（hook 住 core.push），别的帧到了也不该让面板开出来；那一帧一到才能开
        const dropHook = (name) => `(() => {
            let session;
            Object.defineProperty(window, "__RINNE_PREVIEW__", {
                configurable: true,
                get() { return session; },
                set(s) {
                    session = s;
                    const c = s && s.core;
                    if (!c || typeof c.push !== "function") return;
                    const push = c.push;
                    window.__origPush = (n) => push.call(c, n);
                    window.__drop = ${JSON.stringify(name)};
                    c.push = function (n) { return n === window.__drop ? undefined : push.apply(c, arguments); };
                },
            });
        })()`;
        const openDropPage = async (name) => {
            const pg = await browser.newPage();
            await pg.setViewport(VIEW.w, VIEW.h, 1);
            await pg.send("Page.addScriptToEvaluateOnNewDocument", { source: dropHook(name) });
            await pg.goto(
                `${server.url}/web-preview/input.html?${new URLSearchParams({ fresh: "1", lang: "zh", scenario: "in-playing" })}`,
            );
            await pg.waitFor(
                `(() => { const s = document.querySelector(".pv-status"); return !!s && s.getAttribute("data-ok") !== "wait"; })()`,
                { timeout: 20000, label: "壳页注入" },
            );
            await pg.waitFor(
                `(() => { const f = document.querySelector("iframe"); return !!(f && f.contentDocument && f.contentDocument.querySelector('[data-gb="input.shell"]')); })()`,
                { timeout: 20000, label: "页面外壳" },
            );
            await sleep(pg, 800);
            return pg;
        };
        const forceOpen = async (pg) => {
            await inFrame(pg, (doc2, win) => {
                const b = doc2.querySelector('[data-gb="input.preset.trigger"]');
                b.disabled = false;
                b.click();
                b.dispatchEvent(
                    new win.KeyboardEvent("keydown", { bubbles: true, cancelable: true, key: "ArrowDown" }),
                );
            });
            await sleep(pg, 150);
        };
        const PRESET_WRITES = ["loadPreset", "savePreset", "deletePreset"];

        // 页 A：rinne.presets 没到（state 与 pair 都到了）
        {
            const pg = await openDropPage("rinne.presets");
            const recA = await recordBridge(pg, RECORDED);
            let pn = await P(pg);
            run.eq(pn.trig.disabled, true, "快照到了而 rinne.presets 没到：预设按钮应置灰");
            run.eq(pn.trig.name, factoryName("slapback", "zh"), "快照到了：按钮照写当前预设名");
            await clickSel(pg, TRIG);
            run.eq((await P(pg)).open, false, "rinne.presets 没到：真点按钮开不了面板");
            await forceOpen(pg);
            run.eq((await P(pg)).open, false, "rinne.presets 没到：强行放开按钮并合成点击与 ↓ 也开不了面板");
            run.eq(await bridgeCalls(pg, PRESET_WRITES), [], "rinne.presets 没到：桥不应收到任何预设写入");
            await pg.eval(`window.__drop = null; window.__origPush("rinne.presets")`);
            await sleep(pg, 200);
            pn = await P(pg);
            run.eq(pn.trig.disabled, false, "列表到了：预设按钮可用");
            await openMenu(pg);
            run.eq((await P(pg)).open, true, "列表到了（防空转）：面板能开");
            run.check(pg.errors.length === 0, `没有列表的一页：页面不应有错误：${pg.errors.slice(0, 3).join("；")}`);
            await pg.close();
            void recA;
        }

        // 页 B：rinne.pair 没到（state 与 presets 到了）：页面有了界面状态与列表，只剩「两帧都到才算就绪」挡着
        {
            const pg = await openDropPage("rinne.pair");
            const recB = await recordBridge(pg, RECORDED);
            let pn = await P(pg);
            run.eq(pn.trig.disabled, true, "state、presets 到了而 pair 没到：预设按钮应置灰");
            run.eq(pn.trig.inert, true, "pair 没到：预设按钮应在 inert 之内");
            await clickSel(pg, TRIG);
            run.eq((await P(pg)).open, false, "pair 没到：真点按钮开不了面板");
            await forceOpen(pg);
            run.eq((await P(pg)).open, false, "pair 没到：强行放开按钮并合成点击与 ↓ 也开不了面板（就绪才能开）");
            run.eq(await bridgeCalls(pg, PRESET_WRITES), [], "pair 没到：桥不应收到任何预设写入");
            await pg.eval(`window.__drop = null; window.__origPush("rinne.pair")`);
            await pg.waitFor(
                `(() => { const f = document.querySelector("iframe"); const b = f && f.contentDocument && f.contentDocument.querySelector('[data-gb="input.preset.trigger"]'); return !!b && !b.disabled; })()`,
                { timeout: 8000, label: "pair 到了，预设按钮应放开" },
            );
            await settle(pg);
            await openMenu(pg);
            run.eq((await P(pg)).open, true, "pair 到了（防空转）：面板能开");
            run.check(pg.errors.length === 0, `没有 pair 的一页：页面不应有错误：${pg.errors.slice(0, 3).join("；")}`);
            await pg.close();
            void recB;
        }
    }

    run.check(page.errors.length === 0, `页面不应有错误：${page.errors.slice(0, 3).join("；")}`);
} finally {
    await browser.close();
    await server.close();
}
run.finish();
