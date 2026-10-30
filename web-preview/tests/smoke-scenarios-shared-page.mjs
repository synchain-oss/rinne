// SPDX-License-Identifier: GPL-3.0-or-later
//
// 共享件的行为冒烟（无头浏览器）：把夹具页当宿主，逐个共享件点一遍，看桥调用与界面状态对不对。
//   配对 pill：15 态的色语义、绿点脉冲规则（只有连通且音频在流动的两态脉冲）、bypassed 只在 Output；
//   组选择：点击 → setGroup、选中态跟随；语言胶囊：点击 → 切到所选语言；
//   消息位：红色优先、「+n」轮换与回位、动作按钮、冷却态不触发、✕ 关闭、确认条的两枚按钮、空闲态；
//   页脚：缩放下拉 → 确认框 → 倒计时在走 → 保存 / 取消 / Esc 各自的桥调用，超屏档位置灰，锁定态；
//   关于面板：诊断预览先取（不写剪贴板）、复制 → commit 取一次并出轻提示、四个链接只发白名单内的地址、开关与 tail 行；
//   轻提示：4 秒后自己收；外壳危险描边；发光圆心在 0.5 / 1.25 / 2 三档缩放下仍落在指针下。
// 浏览器缺失时整套跳过（退出码 2）。
//
// 用法：node web-preview/tests/smoke-scenarios-shared-page.mjs [仓库根]

import { pathToFileURL } from "node:url";
import path from "node:path";
import { createRun, repoRoot } from "./lib/harness.mjs";
import { findBrowser, launchBrowser } from "./lib/browser.mjs";
import { startStaticServer } from "./lib/static-server.mjs";
import { ROLES, SCENES, PAIR_TONES, loadDesign, openScene, sceneUrl } from "./shared-measure.mjs";

const ROOT = repoRoot();
const run = createRun("共享件行为");
if (!findBrowser()) run.skip("没有 Chromium 系浏览器");

const design = await loadDesign(ROOT);
const links = await import(pathToFileURL(path.join(ROOT, "web/shared/links.js")).href);
const server = await startStaticServer(ROOT);
const browser = await launchBrowser();

const scene = (id) => SCENES.find((s) => s.id === id);
const sleep = (page, ms) => page.eval(`new Promise((r) => setTimeout(r, ${ms}))`);
const calls = (page, name) =>
    page.eval(`window.__fx.calls.filter((c) => c.name === ${JSON.stringify(name)}).map((c) => c.args)`);
const clickPart = (page, sel) =>
    page.eval(
        `(() => { const e = document.querySelector(${JSON.stringify(sel)}); if (!e) return false; e.click(); return true; })()`,
    );
const count = (page, sel) => page.eval(`document.querySelectorAll(${JSON.stringify(sel)}).length`);
const attr = (page, sel, name) =>
    page.eval(
        `(() => { const e = document.querySelector(${JSON.stringify(sel)}); return e ? e.getAttribute(${JSON.stringify(name)}) : null; })()`,
    );
const rgbOf = (c) =>
    (/rgba?\(([^)]+)\)/.exec(c) || [, ""])[1]
        .split(",")
        .slice(0, 3)
        .map((v) => Math.round(Number(v)))
        .join(",");

/** 语义点色（来自 tokens 的 --sem-*），与浏览器算出的背景色比。 */
const DOT_RGB = { green: "120,176,142", amber: "212,176,118", red: "198,110,120", gray: "150,152,162" };

try {
    const page = await browser.newPage();
    const requests = await page.recordRequests();

    for (const role of ROLES) {
        const k = role === "input" ? "in" : "out";
        const gb = (part) => `[data-gb="${role}.${part}"]`;

        // ============================================================ 配对 pill
        await openScene(page, server, design, role, "zh", scene("pair"));
        const pills = await page.eval(
            `[...document.querySelectorAll('[data-gb$=".pair.pill"]')].map((p) => { const dot = p.querySelector(".sc-pill__dot"); const cell = p.closest(".fx-cell"); const cap = cell ? cell.querySelector(".fx-cap").textContent : ""; return { state: p.getAttribute("data-state"), tone: p.getAttribute("data-tone"), still: cap.includes("(still)"), dot: getComputedStyle(dot).backgroundColor, anim: getComputedStyle(dot).animationName, pulse: p.getAttribute("data-pulse") }; })`,
        );
        run.eq(
            pills.length,
            role === "input" ? 16 : 17,
            `${role}：配对目录里的 pill 个数（Input 14 态、Output 15 态，连通两态各多一格不流动）`,
        );
        run.eq(
            pills.some((p) => p.state === "bypassed"),
            role === "output",
            `${role}：bypassed 只在 Output 出现`,
        );
        for (const p of pills) {
            const tag = `${role} pill ${p.state}${p.still ? "（不流动）" : ""}`;
            run.eq(p.tone, PAIR_TONES[p.state], `${tag}：色语义`);
            run.eq(rgbOf(p.dot), DOT_RGB[PAIR_TONES[p.state]], `${tag}：绿点的实际颜色`);
            const pulses = (p.state === "connected" || p.state === "connectedUnverified") && !p.still;
            run.eq(p.pulse, pulses ? "1" : "0", `${tag}：data-pulse`);
            run.eq(p.anim !== "none", pulses, `${tag}：绿点是否真的在动画（connectedStill 与不流动时必须静止）`);
        }

        // ============================================================ 整窗
        await openScene(page, server, design, role, "zh", scene("window"));
        run.eq(await attr(page, gb("shell"), "data-state"), "normal", `${role}：常态外壳 data-state`);
        run.eq(
            await page.eval(`getComputedStyle(document.querySelector('${gb("shell")}'), "::after").opacity`),
            "0",
            `${role}：常态下危险描边不显示`,
        );

        // 组选择
        await clickPart(page, `${gb("group")} [data-group="C"]`);
        run.eq(await calls(page, "setGroup"), [["C"]], `${role}：点 C 应调 setGroup("C")`);
        run.eq(
            await page.eval(
                `[...document.querySelectorAll('${gb("group")} [role="radio"]')].filter((r) => r.getAttribute("aria-checked") === "true").map((r) => r.getAttribute("data-group"))`,
            ),
            ["C"],
            `${role}：选中态跟随`,
        );

        // 语言胶囊：点 EN 切到英文
        await clickPart(page, `${gb("lang")} [data-lang="en"]`);
        await page.waitFor(`window.__fx && window.__fx.ready === true && window.__fx.lang === "en"`, {
            label: `${role}：点 EN 后应以英文重开`,
        });
        run.eq(await page.eval("document.documentElement.lang"), "en", `${role}：语言切到英文`);

        // ---- 消息位 ----
        await openScene(page, server, design, role, "zh", scene("window"));
        const ids = role === "input" ? ["stale", "conflict", "cutPanic"] : ["stale", "conflict", "unassigned"];
        const secondRed = role === "input" ? "cutPanic" : "unassigned";
        await page.eval(`window.__fx.api.setMessages(${JSON.stringify(ids)})`);
        const band = gb("band");
        run.eq(await attr(page, band, "data-msg"), `band.${k}.conflict`, `${role}：红色优先，同色按页面给的顺序`);
        run.eq(await attr(page, band, "data-tone"), "red", `${role}：消息位 data-tone`);
        run.eq(await attr(page, band, "role"), "alert", `${role}：红色消息位是 alert`);
        const chip = await page.eval(
            `(() => { const c = document.querySelector('${band} [data-part="band.more"]'); return c ? c.textContent.trim() : null; })()`,
        );
        run.eq(chip, "+2", `${role}：三条消息应显示「+2」`);
        await clickPart(page, `${band} [data-part="band.more"]`);
        run.eq(await attr(page, band, "data-msg"), `band.${k}.${secondRed}`, `${role}：点「+2」轮换到下一条`);
        await clickPart(page, `${band} [data-part="band.more"]`);
        run.eq(await attr(page, band, "data-msg"), `band.${k}.stale`, `${role}：再点一次轮换到琥珀`);
        await clickPart(page, `${band} [data-part="band.more"]`);
        run.eq(await attr(page, band, "data-msg"), `band.${k}.conflict`, `${role}：轮满一圈回到第一条`);

        // 动作按钮
        await page.eval(
            `window.__fx.api.setMessages(${JSON.stringify(role === "input" ? ["cutPanic"] : ["conflict"])})`,
        );
        const actionName = role === "input" ? "restore" : "repair";
        await clickPart(page, `${band} [data-part="band.action"]`);
        run.eq(
            (await calls(page, "action")).map((a) => a[0]),
            [actionName],
            `${role}：点动作按钮应回调 ${actionName}`,
        );
        if (role === "input") {
            run.eq(await attr(page, band, "data-variant"), "dark", `${role}：切断类是深底危险条`);
            // 冷却态：按钮禁用、带进度弧、点了不回调
            await page.eval(
                `window.__fx.api.setMessages(["cutPanic"], { action: { name: "restore", cooldown: true } })`,
            );
            run.eq(await attr(page, band, "data-state"), "cutPanic+cooldown", `${role}：冷却态 data-state`);
            run.eq(
                await attr(page, `${band} [data-part="band.action"]`, "aria-disabled"),
                "true",
                `${role}：冷却中的按钮 aria-disabled`,
            );
            run.eq(await count(page, `${band} [data-part="cooldown-arc"]`), 1, `${role}：冷却中的按钮带进度弧`);
            await clickPart(page, `${band} [data-part="band.action"]`);
            run.eq((await calls(page, "action")).length, 1, `${role}：冷却中点按钮不应再回调`);
            // ✕ 关闭
            await page.eval(`window.__fx.api.setMessages(["exportMismatch"], { dismiss: true })`);
            await clickPart(page, `${band} [data-part="band.dismiss"]`);
            run.eq(await calls(page, "dismiss"), [["exportMismatch"]], `${role}：点 ✕ 应回调 onDismiss(id)`);
            run.eq(await attr(page, band, "data-state"), "idle", `${role}：关掉最后一条后回到空闲`);
            run.eq(
                await page.eval(`document.querySelector('${band}').textContent.trim()`),
                "",
                `${role}：Input 空闲位没有任何文字（不是「null」）`,
            );
        }

        // 确认条
        await page.eval(
            `window.__fx.api.setConfirm({ key: "band.${k}.takeover", vals: { g: "H" }, onConfirm: () => window.__fx.calls.push({ name: "confirm", args: [] }), onCancel: () => window.__fx.calls.push({ name: "cancelConfirm", args: [] }) })`,
        );
        run.eq(await attr(page, band, "data-state"), "confirm", `${role}：确认条 data-state`);
        run.eq(await count(page, `${band} button`), 2, `${role}：确认条有取消与确认两枚按钮`);
        await clickPart(page, `${band} [data-part="band.action"]`);
        await clickPart(page, `${band} [data-part="confirm.cancel"]`);
        run.eq(
            [(await calls(page, "confirm")).length, (await calls(page, "cancelConfirm")).length],
            [1, 1],
            `${role}：确认与取消各回调一次`,
        );
        await page.eval(`window.__fx.api.setConfirm(null); window.__fx.api.setMessages([])`);
        run.eq(await attr(page, band, "data-state"), "idle", `${role}：清空后回到空闲`);
        if (role === "output") {
            run.check(
                (await page.eval(`document.querySelector('${band}').textContent.trim()`)).length > 0,
                `${role}：Output 空闲位有提示文字`,
            );
            // 空闲提示是槽里的一行淡字：左缩进 10、竖向居中、不画卡片（设计稿 O-09）
            const idle = await page.eval(
                `(() => { const s = document.querySelector('${band}'); const t = s.querySelector('[data-gb="output.hint"]'); const a = s.getBoundingClientRect(), b = t.getBoundingClientRect(); const cs = getComputedStyle(s.firstElementChild); return { inset: Math.round(b.left - a.left), off: Math.round(Math.abs((b.top + b.bottom) / 2 - (a.top + a.bottom) / 2)), box: cs.boxShadow, bg: cs.backgroundColor, bd: cs.borderTopWidth }; })()`,
            );
            run.check(
                idle.inset >= 9 && idle.inset <= 12 && idle.off <= 2,
                `${role}：空闲提示应缩进 10 并竖向居中，实得 ${JSON.stringify(idle)}`,
            );
            run.check(
                idle.box === "none" && idle.bg === "rgba(0, 0, 0, 0)" && idle.bd === "0px",
                `${role}：空闲位不画卡片（无阴影、无底色、无边），实得 ${JSON.stringify(idle)}`,
            );
        }

        // ---- 页脚：缩放 ----
        await openScene(page, server, design, role, "zh", scene("window"));
        const sel = `${gb("footer.scale")} select`;
        const nOpt = await count(page, `${sel} option`);
        run.eq(nOpt, role === "input" ? 7 : 10, `${role}：缩放下拉的档位数`);
        const pickScale = (v) =>
            page.eval(
                `(() => { const s = document.querySelector(${JSON.stringify(sel)}); s.value = ${JSON.stringify(String(v))}; s.dispatchEvent(new Event("change", { bubbles: true })); })()`,
            );
        const dlg = gb("scale.confirm");
        const shown = () =>
            page.eval(
                `(() => { const d = document.querySelector(${JSON.stringify(dlg)}); return Boolean(d) && !d.closest("[hidden]"); })()`,
            );
        run.eq(await shown(), false, `${role}：未选档时没有确认框`);
        await pickScale(1.25);
        await page.waitFor(`window.__fx.calls.some((c) => c.name === "setUiScale")`, {
            label: `${role}：选档应调 setUiScale`,
        });
        run.eq(await calls(page, "setUiScale"), [[1.25]], `${role}：选 1.25 应调 setUiScale(1.25)`);
        run.eq(await shown(), true, `${role}：选档后弹出确认框`);
        // 位置按设计稿：Input 左两栏（x 122、y 210），Output 水平居中、顶边 (364 - 130) / 2
        const at = await page.eval(
            `(() => { const d = document.querySelector(${JSON.stringify(dlg)}).getBoundingClientRect(); const sh = document.querySelector('[data-gb$=".shell"]'); const s = sh.getBoundingClientRect(); return { x: Math.round(d.left - s.left - sh.clientLeft), y: Math.round(d.top - s.top - sh.clientTop), w: Math.round(d.width) }; })()`,
        );
        run.eq(
            at,
            role === "input" ? { x: 122, y: 210, w: 300 } : { x: 80, y: 117, w: 300 },
            `${role}：缩放确认框的位置与宽度`,
        );
        const t0 = await page.eval(`document.querySelector(${JSON.stringify(dlg)}).textContent`);
        await sleep(page, 2300);
        const t1 = await page.eval(`document.querySelector(${JSON.stringify(dlg)}).textContent`);
        run.check(t0 !== t1, `${role}：确认框的倒计时应在走（${JSON.stringify(t0)} → ${JSON.stringify(t1)}）`);
        await clickPart(page, `${dlg} [data-part="confirm.keep"]`);
        await page.waitFor(`window.__fx.calls.some((c) => c.name === "commitUiScale")`, {
            label: `${role}：保存应调 commitUiScale`,
        });
        run.eq(await shown(), false, `${role}：保存后确认框收起`);
        // 取消：回到选档前的档位
        const other = role === "input" ? 0.8 : 0.75;
        await pickScale(other);
        await page.waitFor(`window.__fx.calls.filter((c) => c.name === "setUiScale").length === 2`, {
            label: `${role}：再选档`,
        });
        await clickPart(page, `${dlg} [data-part="confirm.cancel"]`);
        // 点取消要立刻回退：只等 0.8 秒，远小于 10 秒倒计时，不能让超时自动回退顶替点击
        await sleep(page, 800);
        run.eq(
            (await calls(page, "setUiScale")).map((a) => a[0]),
            [1.25, other, 1.25],
            `${role}：取消回到选档前的档位（保存后的 1.25）`,
        );
        run.eq(await shown(), false, `${role}：取消后确认框收起`);
        // Esc 等于取消，不等于保存：同样立刻回退，倒计时不能代劳
        await pickScale(other);
        await page.waitFor(`window.__fx.calls.filter((c) => c.name === "setUiScale").length === 4`, {
            label: `${role}：Esc 之前再选一次档`,
        });
        for (const type of ["rawKeyDown", "keyUp"]) {
            await page.send("Input.dispatchKeyEvent", {
                type,
                key: "Escape",
                code: "Escape",
                windowsVirtualKeyCode: 27,
                nativeVirtualKeyCode: 27,
            });
        }
        await sleep(page, 800);
        run.eq(
            (await calls(page, "setUiScale")).map((a) => a[0]),
            [1.25, other, 1.25, other, 1.25],
            `${role}：Esc 回到选档前的档位`,
        );
        run.eq((await calls(page, "commitUiScale")).length, 1, `${role}：Esc 与取消都不应触发 commitUiScale`);
        run.eq(await shown(), false, `${role}：Esc 后确认框收起`);

        // 页脚：「?」与锁定
        await clickPart(page, gb("footer.help"));
        run.eq((await calls(page, "help")).length, 1, `${role}：点「?」应回调`);
        await openScene(page, server, design, role, "zh", scene("window"), "&locked=1");
        run.eq(await page.eval(`document.querySelector('${sel}').disabled`), true, `${role}：锁定态下拉禁用`);
        run.eq(await attr(page, gb("footer.hint"), "data-state"), "locked", `${role}：锁定态页脚提示`);
        // 超屏档位置灰
        await openScene(page, server, design, role, "zh", scene("window"), "&screen=1000x700");
        const greyed = await page.eval(
            `[...document.querySelectorAll('${sel} option')].filter((o) => o.disabled).map((o) => ({ v: o.value, title: o.title }))`,
        );
        run.check(
            greyed.length >= 1 && greyed.every((g) => g.title.length > 0),
            `${role}：1000×700 屏上应有置灰档位，且都带提示，实得 ${JSON.stringify(greyed)}`,
        );

        // ============================================================ 关于面板
        await openScene(page, server, design, role, "zh", scene("about"));
        const about = gb("about");
        run.eq(
            (await calls(page, "copyDiagnostics")).map((a) => a[0]),
            [false],
            `${role}：打开时先取诊断预览（不写剪贴板）`,
        );
        run.check(
            (await page.eval(`document.querySelector('${about} [data-part="about.diagPreview"]').textContent`)).length >
                20,
            `${role}：诊断预览有内容`,
        );
        await clickPart(page, `${about} [data-part="about.copy"]`);
        await page.waitFor(`window.__fx.calls.some((c) => c.name === "copyDiagnostics" && c.args[0] === true)`, {
            label: `${role}：点复制应以 commit=true 再取一次`,
        });
        run.eq(
            await attr(page, `${about} [data-part="about.copy"]`, "data-state"),
            "copied",
            `${role}：复制成功后按钮变「已复制」`,
        );
        run.eq(
            await page.eval(
                `(() => { const t = document.querySelector('${gb("toast")}'); return { shown: !t.hidden, tone: t.getAttribute("data-tone") }; })()`,
            ),
            { shown: true, tone: "green" },
            `${role}：复制成功出绿色轻提示`,
        );
        // 链接：只发白名单内的地址
        for (const name of ["source", "site", "contact", "notices"])
            await clickPart(page, `${about} [data-link="${name}"]`);
        const want = Object.values(links.aboutLinks("0.9.0"));
        run.eq(
            (await calls(page, "openExternal")).map((a) => a[0]),
            want,
            `${role}：四个链接发给桥的地址`,
        );
        run.check(
            want.every((u) => links.isAllowedExternal(u)),
            `${role}：四个地址都在白名单内`,
        );
        await clickPart(page, `${about} [data-part="about.viewLicense"]`);
        run.eq((await calls(page, "showLicense")).length, 1, `${role}：「查看许可」应调 showLicense`);
        // 开关
        await clickPart(page, `${about} [data-adv="diagLog"]`);
        await page.waitFor(
            `document.querySelector('${about} [data-adv="diagLog"]').getAttribute("aria-checked") === "true"`,
            { label: `${role}：诊断日志开关应打开` },
        );
        run.eq(await calls(page, "setDiagLog"), [[true]], `${role}：诊断日志开关 → setDiagLog(true)`);
        if (role === "input") {
            await clickPart(page, `${about} [data-adv="resetOnPlay"]`);
            await page.waitFor(
                `document.querySelector('${about} [data-adv="resetOnPlay"]').getAttribute("aria-checked") === "false"`,
                { label: "起播复位开关应关闭" },
            );
            run.eq(await calls(page, "setResetOnPlay"), [[false]], `${role}：起播复位开关 → setResetOnPlay(false)`);
            const modes = await page.eval(
                `[...document.querySelectorAll('${about} [data-tail]')].map((n) => n.getAttribute("data-tail"))`,
            );
            run.eq(modes.length, 3, `${role}：tail 模式三项`);
            await clickPart(page, `${about} [data-tail="${modes[1]}"]`);
            await page.waitFor(
                `document.querySelector('${about} [data-tail="${modes[1]}"]').getAttribute("aria-checked") === "true"`,
                { label: "tail 选中应跟随" },
            );
            run.eq(await calls(page, "setTailMode"), [[modes[1]]], `${role}：点第二个 tail 模式 → setTailMode`);
        } else {
            run.eq(
                [await count(page, `${about} [data-adv="resetOnPlay"]`), await count(page, `${about} [data-tail]`)],
                [0, 0],
                `${role}：Output 的关于面板没有起播复位与 tail 行`,
            );
        }
        // 复制失败：红色错误行 + 红色轻提示
        await openScene(page, server, design, role, "zh", scene("about-copyFail"));
        const fail = await page.eval(
            `(() => { const f = document.querySelector('${about} .rn-about__fail'); const t = document.querySelector('${gb("toast")}'); return { shown: !f.hidden, text: f.textContent, role: f.getAttribute("role"), tone: t.getAttribute("data-tone") }; })()`,
        );
        run.check(
            fail.shown && fail.text.includes("clipboard denied") && fail.role === "alert" && fail.tone === "red",
            `${role}：复制失败应显示带原因的 alert 与红色轻提示，实得 ${JSON.stringify(fail)}`,
        );

        // ============================================================ 外壳危险描边
        await openScene(page, server, design, role, "zh", scene("window-danger"));
        run.eq(await attr(page, gb("shell"), "data-state"), "danger", `${role}：危险态外壳 data-state`);
        run.eq(
            await page.eval(`getComputedStyle(document.querySelector('${gb("shell")}'), "::after").opacity`),
            "1",
            `${role}：危险态画出描边`,
        );
    }

    // ============================================================ 轻提示 4 秒后自己收（只在 Input 量一次，要等真实时间）
    await openScene(page, server, design, "input", "zh", scene("toast-group"));
    const toastSel = '[data-gb="input.toast"]';
    run.eq(await page.eval(`!document.querySelector('${toastSel}').hidden`), true, "轻提示：刚弹出时可见");
    // 时间从夹具弹出轻提示的那一刻算起：openScene 要等字体与入场动画静止，返回的快慢不能算进 4 秒里
    const shownAge = async () => page.eval("performance.now() - window.__fxToastAt");
    const age0 = await shownAge();
    run.check(age0 < 3000, `轻提示：openScene 返回时已过去 ${Math.round(age0)} ms，应远小于 3.2 秒`);
    await sleep(page, Math.max(0, 3200 - age0));
    run.eq(await page.eval(`!document.querySelector('${toastSel}').hidden`), true, "轻提示：3.2 秒时还在");
    await sleep(page, Math.max(0, 4300 - (await shownAge())));
    run.eq(await page.eval(`!document.querySelector('${toastSel}').hidden`), false, "轻提示：4.3 秒时已收");

    // ============================================================ 「+n」无人操作 8 秒后回到第一条（要等真实时间）
    await openScene(page, server, design, "input", "zh", scene("window"));
    await page.eval(`window.__fx.api.setMessages(["stale", "conflict"])`);
    await clickPart(page, '[data-gb="input.band"] [data-part="band.more"]');
    run.eq(await attr(page, '[data-gb="input.band"]', "data-msg"), "band.in.stale", "轮换：点「+1」后显示第二条");
    await sleep(page, 8400);
    run.eq(await attr(page, '[data-gb="input.band"]', "data-msg"), "band.in.conflict", "轮换：8 秒无操作后回到第一条");

    // ============================================================ 发光圆心：三档缩放下都落在指针下
    for (const role of ROLES) {
        for (const f of [0.5, 1.25, 2]) {
            await openScene(page, server, design, role, "zh", scene("window"));
            const box = design[role];
            await page.setViewport(Math.round(box.w * f), Math.round(box.h * f));
            await page.waitFor(
                `Math.abs(Number(document.querySelector('[data-gb="${role}.shell"]').style.zoom) - ${f}) < 0.01`,
                { label: `${role}：视口按 ${f} 缩放后外壳倍率应跟上` },
            );
            const target = `[data-gb="${role}.footer.version"]`;
            const r = await page.eval(
                `(() => { const e = document.querySelector('${target}'); const b = e.getBoundingClientRect(); return { x: b.left, y: b.top, w: b.width, h: b.height, ow: e.offsetWidth, oh: e.offsetHeight }; })()`,
            );
            const move = (x, y) =>
                page.send("Input.dispatchMouseEvent", {
                    type: "mouseMoved",
                    x,
                    y,
                    button: "none",
                    pointerType: "mouse",
                });
            // 中心点
            await move(r.x + r.w / 2, r.y + r.h / 2);
            let g = await page.eval(
                `(() => { const e = document.querySelector('${target}'); return { mx: parseFloat(e.style.getPropertyValue("--mx")), my: parseFloat(e.style.getPropertyValue("--my")), o: e.style.getPropertyValue("--glow-o") }; })()`,
            );
            run.check(
                Math.abs(g.mx - r.ow / 2) <= 1.5 && Math.abs(g.my - r.oh / 2) <= 1.5 && g.o === "1",
                `${role} × ${f}：中心点圆心应为 (${r.ow / 2}, ${r.oh / 2})，实得 ${JSON.stringify(g)}`,
            );
            // 偏一角：设计坐标里距左 10、距上 4
            await move(r.x + (10 * r.w) / r.ow, r.y + (4 * r.h) / r.oh);
            g = await page.eval(
                `(() => { const e = document.querySelector('${target}'); return { mx: parseFloat(e.style.getPropertyValue("--mx")), my: parseFloat(e.style.getPropertyValue("--my")) }; })()`,
            );
            run.check(
                Math.abs(g.mx - 10) <= 1.5 && Math.abs(g.my - 4) <= 1.5,
                `${role} × ${f}：偏角点圆心应为 (10, 4)，实得 ${JSON.stringify(g)}`,
            );
            // 移开：熄灯
            await move(2, 2);
            run.eq(
                await page.eval(`document.querySelector('${target}').style.getPropertyValue("--glow-o")`),
                "0",
                `${role} × ${f}：指针移开后熄灯`,
            );
        }
    }

    run.check(page.errors.length === 0, `页面不应有错误：${page.errors.slice(0, 3).join("；")}`);
    const foreign = requests.filter((u) => !/^(data|blob|about):/.test(u) && new URL(u).origin !== server.url);
    run.check(foreign.length === 0, `夹具页的请求都必须同源，外部请求：${JSON.stringify(foreign)}`);
    run.check(
        sceneUrl(server, "input", "zh", "scene=window").includes("/web-preview/fixtures/shared/"),
        "夹具地址自检",
    );
} finally {
    await browser.close();
    await server.close();
}
run.finish();
