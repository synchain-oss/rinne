// SPDX-License-Identifier: GPL-3.0-or-later
//
// 反馈大旋钮的页面级冒烟（无头浏览器）：真源页面经预览壳页装载，指针、滚轮用 CDP 发真事件。
//   ① 滚轮：60 个 3 px 的滚轮事件不会让反馈越过 100%，到顶后反向立即有反应；
//   ② 双击读数：复位到默认值，不开数值输入；单击读数：等双击窗口过去才开；Ctrl 加单击立即开；
//   ③ 拖动：纵向拖 30 px 越过 100% 处的闸停在 100，抬起后桥收到 begin、set、end 的完整手势，值与回声一致；
//   ④ pointercancel：拖动中取消，显示与桥里的值都回到起点，桥收到最后一次写回起点值与 end，没有残留的拖动态；
//   ⑤ 发光圆心：在 0.5、1.25、2 三档，指针在旋钮盘内某处，发光圆心（--mx / --my）折回设计坐标后与指针位置重合；
//   ⑥ 坏样例：滚轮不设上限的旋钮、发光不除倍率的圆心，必须被上面各项抓出来（量尺本身可信）。
// 手势的纯逻辑由 smoke-knob-gesture 钉住，这里钉的是页面接线。浏览器缺失时整套跳过（退出码 2），不是通过。
// 本机同时只能有一个浏览器冒烟。
//
// 用法：node web-preview/tests/smoke-knob-page.mjs [仓库根]

import { createRun, repoRoot } from "./lib/harness.mjs";
import { findBrowser, launchBrowser } from "./lib/browser.mjs";
import { startStaticServer } from "./lib/static-server.mjs";
import { VIEW, centerOf, inFrame, openInput, recordBridge } from "./smoke-input-kit-page.mjs";

const ROOT = repoRoot();
const run = createRun("反馈旋钮页面");
if (!findBrowser()) run.skip("没有 Chromium 系浏览器");

const DISC = '[data-gb="input.fb.knob"] [role="slider"]';
const NUMEDIT = '[data-gb="input.numEdit"]';
const sleep = (page, ms) => page.eval(`new Promise((r) => setTimeout(r, ${ms}))`);

const shown = (page) =>
    inFrame(page, (doc, win, sel) => Number(doc.querySelector(sel).getAttribute("aria-valuenow")), DISC);
const model = (page) => page.eval("window.__RINNE_PREVIEW__.model.values.feedback");
const hasEdit = (page) => inFrame(page, (doc, win, sel) => Boolean(doc.querySelector(sel)), NUMEDIT);
const dragging = (page) =>
    inFrame(page, (doc) => doc.querySelector('[data-gb="input.fb.knob"]').getAttribute("data-state"));

/** 等键盘与滚轮的手势停手收尾（停手 300 ms 才 end），再经桥改值，等页面回声到位。 */
async function setFeedback(page, v) {
    await sleep(page, 700);
    await page.eval(`window.__RINNE_PREVIEW__.mock.setParam("feedback", ${v})`);
    await page.waitFor(
        `(() => { const e = document.querySelector("iframe").contentDocument.querySelector('${DISC}'); return !!e && Number(e.getAttribute("aria-valuenow")) === ${v}; })()`,
        { label: `反馈量应回声到 ${v}` },
    );
}

const mouse = (page, type, x, y, extra = {}) =>
    page.send("Input.dispatchMouseEvent", { type, x, y, button: "left", pointerType: "mouse", ...extra });
async function click(page, x, y, count = 1, modifiers = 0) {
    await mouse(page, "mouseMoved", x, y);
    for (let i = 1; i <= count; i++) {
        await mouse(page, "mousePressed", x, y, { clickCount: i, modifiers });
        await mouse(page, "mouseReleased", x, y, { clickCount: i, modifiers });
    }
}
const wheel = (page, x, y, deltaY) =>
    page.send("Input.dispatchMouseEvent", { type: "mouseWheel", x, y, deltaX: 0, deltaY });

/** 页内量发光圆心：把指针移到 (fx, fy)（相对旋钮盘左上、占宽高的比例），读元素自己的 --mx / --my 与点亮状态。 */
async function glowAt(page, fx, fy) {
    const r = await page.eval(
        `(() => { const e = document.querySelector("iframe").contentDocument.querySelector('${DISC}'); const b = e.getBoundingClientRect(); const o = document.querySelector("iframe").getBoundingClientRect(); return { x: o.x + b.x + b.width * ${fx}, y: o.y + b.y + b.height * ${fy} }; })()`,
    );
    await mouse(page, "mouseMoved", r.x - 6, r.y - 6);
    await mouse(page, "mouseMoved", r.x, r.y);
    await sleep(page, 60);
    return inFrame(
        page,
        (doc, win, sel) => {
            const e = doc.querySelector(sel);
            const num = (v) => parseFloat(v);
            return {
                mx: num(e.style.getPropertyValue("--mx")),
                my: num(e.style.getPropertyValue("--my")),
                lit: e.style.getPropertyValue("--glow-o"),
            };
        },
        DISC,
    );
}

const server = await startStaticServer(ROOT);
const browser = await launchBrowser({ width: VIEW.w, height: VIEW.h });
try {
    const page = await browser.newPage();
    await page.send("Emulation.setFocusEmulationEnabled", { enabled: true });

    // ================================================================ ① 滚轮
    await openInput(page, server, "in-playing", { lang: "zh" });
    await setFeedback(page, 99);
    {
        const c = await centerOf(page, DISC);
        for (let i = 0; i < 60; i++) await wheel(page, c.x, c.y, -3);
        await sleep(page, 150);
        const v = await shown(page);
        run.check(v <= 100, `60 个 3 px 的滚轮不应越过 100%，实得 ${v}`);
        run.eq(v, 100, "滚满一步，停在 100%");
        for (let i = 0; i < 60; i++) await wheel(page, c.x, c.y, -3);
        await sleep(page, 150);
        run.eq(await shown(page), 100, "到顶后继续滚，还是 100%");
        for (let i = 0; i < 40; i++) await wheel(page, c.x, c.y, 3);
        await sleep(page, 150);
        run.check((await shown(page)) < 100, `到顶后反向滚 40 个 3 px 应已往下走，实得 ${await shown(page)}`);
        run.check((await model(page)) <= 100, "桥里的值也不应越过 100");
    }

    // ================================================================ ② 双击与单击读数
    await openInput(page, server, "in-playing", { lang: "zh" });
    {
        const rd = await centerOf(page, '[data-part="fb.readout"]');
        await click(page, rd.x, rd.y, 2);
        await sleep(page, 450);
        run.check(!(await hasEdit(page)), "双击读数不应打开数值输入");
        await page.waitFor(
            `(() => { const e = document.querySelector("iframe").contentDocument.querySelector('${DISC}'); return Number(e.getAttribute("aria-valuenow")) === 0; })()`,
            { label: "双击应复位到默认值 0" },
        );
        // 单击：窗口过去才开
        await click(page, rd.x, rd.y, 1);
        await sleep(page, 100);
        run.check(!(await hasEdit(page)), "单击读数后双击窗口内不应打开数值输入");
        await page.waitFor(`!!document.querySelector("iframe").contentDocument.querySelector('${NUMEDIT}')`, {
            label: "单击读数过了双击窗口应打开数值输入",
        });
    }
    await openInput(page, server, "in-playing", { lang: "zh" });
    {
        const rd = await centerOf(page, '[data-part="fb.readout"]');
        await click(page, rd.x, rd.y, 1, 2); // 2 = Ctrl
        await page.waitFor(`!!document.querySelector("iframe").contentDocument.querySelector('${NUMEDIT}')`, {
            timeout: 250,
            label: "Ctrl 加单击应立即打开数值输入",
        });
    }

    // ================================================================ ③ 拖动
    await openInput(page, server, "in-playing", { lang: "zh" });
    {
        const rec = await recordBridge(page, ["beginParamGesture", "setParam", "endParamGesture"]);
        const c = await centerOf(page, DISC);
        const y0 = c.y + 40;
        await mouse(page, "mouseMoved", c.x, y0);
        await mouse(page, "mousePressed", c.x, y0, { clickCount: 1 });
        await mouse(page, "mouseMoved", c.x, y0 - 10);
        await mouse(page, "mouseMoved", c.x, y0 - 40);
        await sleep(page, 80);
        run.eq(await dragging(page), "drag", "拖动中旋钮应处于拖动态");
        run.eq(await shown(page), 100, "纵向拖 30 px 越过闸：显示停在 100%");
        await mouse(page, "mouseReleased", c.x, y0 - 40, { clickCount: 1 });
        await sleep(page, 200);
        const calls = (await rec.calls()).filter((x) => x[1] === "feedback");
        run.eq(calls[0] && calls[0][0], "beginParamGesture", "桥的第一条应是 beginParamGesture");
        run.eq(
            calls[calls.length - 1] && calls[calls.length - 1][0],
            "endParamGesture",
            "桥的最后一条应是 endParamGesture",
        );
        run.eq(calls.filter((x) => x[0] === "endParamGesture").length, 1, "一次拖动恰好一个 endParamGesture");
        const sets = calls.filter((x) => x[0] === "setParam");
        run.check(
            sets.length >= 1 && sets[sets.length - 1][2] === 100,
            `最后一次 setParam 应是 100，实得 ${JSON.stringify(sets.slice(-1))}`,
        );
        run.eq(await model(page), 100, "桥里的值应是 100");
        run.check((await dragging(page)) !== "drag", "抬起后不应还留着拖动态");
    }

    // ================================================================ ④ pointercancel 回滚（合成事件，页内派发）
    await openInput(page, server, "in-playing", { lang: "zh" });
    {
        const rec = await recordBridge(page, ["beginParamGesture", "setParam", "endParamGesture"]);
        const start = await shown(page);
        await inFrame(
            page,
            (doc, win, sel) => {
                const e = doc.querySelector(sel);
                const b = e.getBoundingClientRect();
                const mk = (type, y) =>
                    new win.PointerEvent(type, {
                        pointerId: 41,
                        pointerType: "mouse",
                        isPrimary: true,
                        button: 0,
                        buttons: 1,
                        clientX: b.x + b.width / 2,
                        clientY: y,
                        bubbles: true,
                        cancelable: true,
                    });
                const y0 = b.y + b.height / 2 + 30;
                e.dispatchEvent(mk("pointerdown", y0));
                e.dispatchEvent(mk("pointermove", y0 - 10));
                e.dispatchEvent(mk("pointermove", y0 - 40));
            },
            DISC,
        );
        await sleep(page, 80);
        run.check((await shown(page)) !== start, "拖动中显示应已变化（防空转）");
        await inFrame(
            page,
            (doc, win, sel) => {
                doc.querySelector(sel).dispatchEvent(
                    new win.PointerEvent("pointercancel", {
                        pointerId: 41,
                        pointerType: "mouse",
                        bubbles: true,
                        cancelable: true,
                    }),
                );
            },
            DISC,
        );
        await sleep(page, 250);
        run.eq(await shown(page), start, "pointercancel 后显示回到起点");
        run.eq(await model(page), start, "pointercancel 后桥里的值回到起点");
        const calls = (await rec.calls()).filter((x) => x[1] === "feedback");
        const sets = calls.filter((x) => x[0] === "setParam");
        run.eq(sets[sets.length - 1] && sets[sets.length - 1][2], start, "最后一次写桥应是写回起点值");
        run.eq(calls.filter((x) => x[0] === "endParamGesture").length, 1, "取消也要收尾：恰好一个 endParamGesture");
        run.check((await dragging(page)) !== "drag", "取消后不应还留着拖动态");
    }

    // ================================================================ ⑤ 发光圆心
    for (const scale of [0.5, 1.25, 2]) {
        await openInput(page, server, "in-playing", { lang: "zh", scale });
        const z = await page.eval(
            `(() => { const f = document.querySelector("iframe"); return f.getBoundingClientRect().width / 800; })()`,
        );
        run.check(Math.abs(z - scale) < 0.01, `${scale} 档：预览窗口的倍率应是 ${scale}，实得 ${z}`);
        for (const [fx, fy] of [
            [0.5, 0.5],
            [0.75, 0.3],
        ]) {
            const g = await glowAt(page, fx, fy);
            const want = { x: 140 * fx, y: 140 * fy };
            run.check(g.lit === "1", `${scale} 档：指针在旋钮盘上，发光应点亮`);
            run.check(
                Math.abs(g.mx - want.x) <= 1.5 && Math.abs(g.my - want.y) <= 1.5,
                `${scale} 档：指针在盘内 (${fx}, ${fy}) 处，发光圆心应是设计坐标 (${want.x.toFixed(1)}, ${want.y.toFixed(1)})，实得 (${g.mx}, ${g.my})`,
            );
        }
    }

    // ================================================================ ⑥ 坏样例
    // 越过 100% 的值：判据读到的显示必须能看出「超过 100」（量尺自检：滚轮判据读的就是这个显示）
    await openInput(page, server, "in-playing", { lang: "zh" });
    await setFeedback(page, 130);
    run.check((await shown(page)) > 100, "坏样例：桥给到 130 时，显示应读到超过 100，滚轮判据才有效");
    // 发光不除倍率：圆心坐标会随档位漂移，判据必须判红（在页面自己写完之后再覆盖成未除倍率的值）
    await openInput(page, server, "in-playing", { lang: "zh", scale: 2 });
    await inFrame(
        page,
        (doc, win, sel) => {
            const e = doc.querySelector(sel);
            doc.addEventListener("pointermove", (ev) =>
                win.setTimeout(() => {
                    const b = e.getBoundingClientRect();
                    e.style.setProperty("--mx", `${(ev.clientX - b.left).toFixed(1)}px`);
                    e.style.setProperty("--my", `${(ev.clientY - b.top).toFixed(1)}px`);
                }, 0),
            );
        },
        DISC,
    );
    {
        const g = await glowAt(page, 0.5, 0.5);
        run.check(Math.abs(g.mx - 70) > 5, `坏样例：不除倍率的圆心在 2 档应明显偏离 70，实得 ${g.mx}`);
    }

    run.check(page.errors.length === 0, `页面不应有错误：${page.errors.slice(0, 3).join("；")}`);
} finally {
    await browser.close();
    await server.close();
}
run.finish();
