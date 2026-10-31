// SPDX-License-Identifier: GPL-3.0-or-later
//
// Input 页面「恒定输入不动 DOM」冒烟（无头浏览器）：宿主自动化跑完、各路回声照常到达，但模型没有变化时，
// 页面的 DOM 一处都不该被写。为什么要有这条：回声与周期快照每秒到达几十次，哪怕渲染层对同一个值重写一次
// 文本或属性，也会让浏览器重排重绘、让读屏重复播报、让正在进行的拖动与输入框丢焦点；这种毛病在单帧截图里完全看不出。
//   ① 静默：场景稳定后，对页面连发 1.5 s 的 rinne.state / rinne.pair / rinne.params / rinne.meters（模型不变），
//      iframe 内整棵文档树的 MutationObserver 记录数必须为 0；同时要求每路事件确实到达（防空转）；
//   ② 自动化收敛：宿主把反馈改到新值（正对照：这时必须有写入），收敛之后同样的恒定回声回到 0 写入；
//   ③ 坏样例：观察者自检（人为写一个属性必须被抓到）；一个对每帧 rinne.state 都重写文本的假渲染器必须被判红。
// 浏览器缺失时整套跳过（退出码 2），不是通过。本机同时只能有一个浏览器冒烟。
//
// 用法：node web-preview/tests/smoke-input-automation-page.mjs [仓库根]

import { createRun, repoRoot } from "./lib/harness.mjs";
import { findBrowser, launchBrowser } from "./lib/browser.mjs";
import { startStaticServer } from "./lib/static-server.mjs";
import { VIEW, inFrame, openInput } from "./smoke-input-kit-page.mjs";

const ROOT = repoRoot();
const run = createRun("Input 页面恒定回声零写入");
if (!findBrowser()) run.skip("没有 Chromium 系浏览器");

const EVENTS = ["rinne.state", "rinne.pair", "rinne.params", "rinne.meters"];
const QUIET_SCENES = ["in-playing", "in-over100", "in-guard", "in-export-mismatch", "in-delay-beats", "in-loop-off"];
const QUIET_MS = 1500;
const sleep = (page, ms) => page.eval(`new Promise((r) => setTimeout(r, ${ms}))`);

/** 页内开始观察整棵文档树；记录写成可读的短串。 */
function startObserving(doc, win) {
    const describe = (n) => {
        const e = n.nodeType === 1 ? n : n.parentElement;
        if (!e) return "?";
        const tag = e.tagName.toLowerCase();
        const part = e.getAttribute("data-part") || e.getAttribute("data-gb") || "";
        const cls = (e.getAttribute("class") || "").split(/\s+/)[0] || "";
        return `${tag}${cls ? "." + cls : ""}${part ? `[${part}]` : ""}`;
    };
    win.__mut = [];
    win.__mo = new win.MutationObserver((list) => {
        for (const m of list) {
            const what =
                m.type === "attributes"
                    ? `attr:${m.attributeName}`
                    : m.type === "characterData"
                      ? "text"
                      : `children:+${m.addedNodes.length}-${m.removedNodes.length}`;
            win.__mut.push(`${what} @ ${describe(m.target)}`);
        }
    });
    win.__mo.observe(doc.documentElement, { attributes: true, childList: true, characterData: true, subtree: true });
}

/** 页内停止观察并取走记录（先收掉尚未派发的记录）。 */
function stopObserving(doc, win) {
    for (const m of win.__mo.takeRecords()) win.__mut.push(`late:${m.type}`);
    win.__mo.disconnect();
    return win.__mut;
}

/** 在壳页里每 60 ms 把四路事件各发一帧，持续 ms；返回每路实发的帧数。 */
function pushConstant(page, ms) {
    return page.eval(`new Promise((resolve) => {
        const s = window.__RINNE_PREVIEW__;
        const names = ${JSON.stringify(EVENTS)};
        const before = Object.fromEntries(names.map((n) => [n, s.core.sent[n] || 0]));
        const timer = setInterval(() => { for (const n of names) s.core.push(n); }, 60);
        setTimeout(() => {
            clearInterval(timer);
            resolve(Object.fromEntries(names.map((n) => [n, (s.core.sent[n] || 0) - before[n]])));
        }, ${ms});
    })`);
}

/** 观察 + 恒定回声一轮：返回 { writes, sent }。 */
async function quietRun(page, ms = QUIET_MS) {
    await inFrame(page, startObserving);
    const sent = await pushConstant(page, ms);
    await sleep(page, 120);
    const writes = await inFrame(page, stopObserving);
    return { writes, sent };
}

const sample = (writes) => writes.slice(0, 5).join(" ; ") + (writes.length > 5 ? ` ……共 ${writes.length} 处` : "");

const server = await startStaticServer(ROOT);
const browser = await launchBrowser({ width: VIEW.w, height: VIEW.h });
try {
    const page = await browser.newPage();

    // ---- ① 静默
    for (const id of QUIET_SCENES) {
        await openInput(page, server, id, { lang: "zh" });
        await sleep(page, 900); // 入场的有限动画与弹道走完
        const { writes, sent } = await quietRun(page);
        for (const n of EVENTS)
            run.check(sent[n] >= 15, `${id}：${n} 在 1.5 s 内应至少发出 15 帧，实发 ${sent[n]}（防空转）`);
        run.eq(writes.length, 0, `${id}：恒定回声期间页面不应有任何 DOM 写入，抓到 ${sample(writes)}`);
    }

    // ---- ② 自动化收敛：改值时必须有写入（正对照），收敛后回到 0
    await openInput(page, server, "in-playing", { lang: "zh" });
    await sleep(page, 900);
    {
        await inFrame(page, startObserving);
        await page.eval(`window.__RINNE_PREVIEW__.mock.setParam("feedback", 55)`);
        await sleep(page, 700);
        const moved = await inFrame(page, stopObserving);
        run.check(moved.length > 0, "正对照：宿主把反馈改到新值，页面应当有写入（否则观察者没在看）");
        run.eq(
            await inFrame(page, (doc) =>
                doc.querySelector('[data-gb="input.fb.knob"] [role="slider"]').getAttribute("aria-valuenow"),
            ),
            "55",
            "宿主改值之后旋钮的读数应跟到 55",
        );
        await sleep(page, 1200); // 指数逼近收敛
        const { writes, sent } = await quietRun(page);
        run.check(
            sent["rinne.params"] >= 15,
            `自动化收敛后 rinne.params 应持续到达，实发 ${sent["rinne.params"]}（防空转）`,
        );
        run.eq(writes.length, 0, `自动化收敛后恒定回声不应再写 DOM，抓到 ${sample(writes)}`);
    }

    // ---- ③ 坏样例
    // 观察者自检：人为写一个属性必须被抓到
    await openInput(page, server, "in-playing", { lang: "zh" });
    await inFrame(page, startObserving);
    await inFrame(page, (doc) => doc.body.setAttribute("data-probe", "1"));
    await sleep(page, 50);
    {
        const writes = await inFrame(page, stopObserving);
        run.check(
            writes.length === 1 && /attr:data-probe/.test(writes[0]),
            `观察者自检：应抓到刚写的属性，实得 ${sample(writes)}`,
        );
    }
    // 对每帧 rinne.state 都重写一次文本的假渲染器：恒定回声里必须被抓出来
    await openInput(page, server, "in-playing", { lang: "zh" });
    await sleep(page, 900);
    await page.eval(`(() => {
        const f = document.querySelector("iframe");
        const doc = f.contentDocument;
        window.__RINNE_PREVIEW__.core.addEventListener("rinne.state", () => {
            const e = doc.querySelector('[data-gb="input.band"]');
            if (e) e.setAttribute("data-badsample", String(Date.now() % 7));
            const t = doc.querySelector(".in-meter__readout");
            if (t) t.textContent = t.textContent;
        });
    })()`);
    {
        const { writes } = await quietRun(page, 800);
        run.check(writes.length > 0, "坏样例：每帧重写文本与属性的假渲染器必须被判红");
        run.check(
            writes.some((w) => /children/.test(w)) && writes.some((w) => /attr:data-badsample/.test(w)),
            `坏样例：文本重写与属性重写两种写入都应被抓到，实得 ${sample(writes)}`,
        );
    }

    run.check(page.errors.length === 0, `页面不应有错误：${page.errors.slice(0, 3).join("；")}`);
} finally {
    await browser.close();
    await server.close();
}
run.finish();
