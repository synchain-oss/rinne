// SPDX-License-Identifier: GPL-3.0-or-later
//
// 首启的端到端冒烟（无头浏览器）：从全新的 Input（?fresh=1）起，用户最多点 3 次、拖 1 次就能把反馈拉到 40%。
//   ① 走导览的路径：选语言（1）→ 滚轮读完规则 → [开始导览]（2）→ 导览里 [跳过]（3）→ 拖反馈大旋钮到 40%；
//   ② 不走导览的路径：选语言（1）→ 滚轮读完规则 → [跳过导览]（2）→ 拖到 40%；
//   两条路径都断言：点击次数不超过 3、拖动恰好 1 次，宿主里的反馈是 40，首启链关掉后内容层可操作；
//   ③ 走完 Input 的首启之后打开 Output（不清全局位）：规则确认过、Input 的导览看过，Output 不弹任何叠层。
// 「点击」只数鼠标按下抬起；读规则用滚轮（不是点击），8 秒放行另由 smoke-first-run-page 量。
// 浏览器缺失时整套跳过（退出码 2），不是通过。本机同时只能有一个浏览器冒烟。
//
// 用法：node web-preview/tests/smoke-first-run-e2e-page.mjs [仓库根]

import { createRun, repoRoot } from "./lib/harness.mjs";
import { findBrowser, launchBrowser } from "./lib/browser.mjs";
import { startStaticServer } from "./lib/static-server.mjs";
import {
    VIEW,
    centerIn,
    clickAt,
    drag,
    inFrame,
    openRole,
    stateOf,
    uiDefaults,
    waitOverlay,
} from "./smoke-first-run-page.mjs";

const ROOT = repoRoot();
const run = createRun("首启端到端");
if (!findBrowser()) run.skip("没有 Chromium 系浏览器");

const DISC = '[data-gb="input.fb.knob"] [role="slider"]';
const TARGET = 40;
const sleep = (page, ms) => page.eval(`new Promise((r) => setTimeout(r, ${ms}))`);

/** 一位用户：只经这里点击与拖动，顺便计数。 */
function user(page) {
    const n = { clicks: 0, drags: 0 };
    return {
        n,
        async click(selector) {
            const c = await centerIn(page, selector);
            if (!c) throw new Error(`页面里没有 ${selector}`);
            n.clicks++;
            await clickAt(page, c.x, c.y);
        },
        /** 在规则页正文上滚滚轮，直到放行（最多 60 下）。 */
        async readRules() {
            const c = await centerIn(page, '[data-gb="input.rules"] [data-part="rules.body"]');
            for (let i = 0; i < 60; i++) {
                await page.send("Input.dispatchMouseEvent", {
                    type: "mouseWheel",
                    x: c.x,
                    y: c.y,
                    deltaX: 0,
                    deltaY: 120,
                });
                await sleep(page, 60);
                if ((await stateOf(page, "input")).rules === "ready") return true;
            }
            return false;
        },
        /** 纵向拖反馈大旋钮，1 px 一步往上，读数到 target 就松手。 */
        async dragFeedbackTo(target) {
            const c = await centerIn(page, DISC);
            n.drags++;
            const steps = Array.from({ length: 200 }, () => [0, -1]);
            await drag(page, c.x, c.y + 40, steps, async () =>
                inFrame(
                    page,
                    (doc, win, sel, t) => Number(doc.querySelector(sel).getAttribute("aria-valuenow")) >= t,
                    DISC,
                    target,
                ),
            );
        },
    };
}

async function chainThenDrag(page, server, viaTour) {
    const where = viaTour ? "走导览" : "不走导览";
    await openRole(page, server, "input");
    const u = user(page);
    await waitOverlay(page, "input", "(s) => s.lang", `${where}：全新应出语言卡`);
    await u.click('[data-lang-pick="zh"]');
    await waitOverlay(page, "input", '(s) => s.rules === "gated"', `${where}：应进规则页`);
    run.check(await u.readRules(), `${where}：滚轮读到底应放行`);
    if (viaTour) {
        await u.click('[data-gb="input.rules"] [data-part="rules.start"]');
        await waitOverlay(page, "input", "(s) => s.tour === 1", `${where}：应进导览`);
        await u.click('[data-gb="input.tour"] [data-part="tour.skip"]');
    } else {
        await u.click('[data-gb="input.rules"] [data-part="rules.skip"]');
    }
    await waitOverlay(
        page,
        "input",
        "(s) => !s.lang && s.rules === null && s.tour === null && !s.inert",
        `${where}：首启链应关掉`,
    );
    await u.dragFeedbackTo(TARGET);
    await page.waitFor(`window.__RINNE_PREVIEW__.model.values.feedback === ${TARGET}`, {
        label: `${where}：宿主里的反馈应是 ${TARGET}`,
    });
    run.check(u.n.clicks <= 3, `${where}：点了 ${u.n.clicks} 次，应不超过 3 次`);
    run.eq(u.n.drags, 1, `${where}：拖动 1 次`);
    run.eq(await page.eval("window.__RINNE_PREVIEW__.model.values.feedback"), TARGET, `${where}：反馈是 ${TARGET}%`);
    const d = await uiDefaults(page);
    run.check(
        d.rulesAckVersion === 1 && d.guideSeenInput === true,
        `${where}：规则已确认、引导已看：${JSON.stringify(d)}`,
    );
    run.log(`  ${where}：${u.n.clicks} 次点击 + ${u.n.drags} 次拖动`);
}

const server = await startStaticServer(ROOT);
const browser = await launchBrowser({ width: VIEW.w, height: VIEW.h });
try {
    const page = await browser.newPage();
    await chainThenDrag(page, server, true);
    await chainThenDrag(page, server, false);

    // ③ Output 在 Input 走完首启之后：不弹
    await openRole(page, server, "output", { fresh: false });
    await sleep(page, 900);
    const s = await stateOf(page, "output");
    run.check(!s.lang && s.rules === null && s.tour === null, `Input 走完首启后 Output 不弹：${JSON.stringify(s)}`);

    run.check(page.errors.length === 0, `页面不应有错误：${page.errors.slice(0, 3).join("；")}`);
} finally {
    await browser.close();
    await server.close();
}
run.finish();
