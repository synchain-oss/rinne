// SPDX-License-Identifier: GPL-3.0-or-later
//
// 消息位的键盘焦点冒烟（无头浏览器）：真页面（夹具 web-preview/fixtures/output）。
// 消息位重画会把槽里的节点整个换掉（「+n」连按轮换、8 秒到点自动回位、确认条收起），正持有焦点的按钮随之消失，
// 焦点掉到 body，键盘用户的位置就丢了。共享消息位在重建前后接力焦点；这里量三件事：
//   ① 键盘连续轮换：焦点在「+n」上按回车 / 空格，连按多次，每次轮换后焦点都还在（新的）「+n」上，且确实换了一条消息；
//   ② 自动回位：焦点在「+n」上，8 秒到点回到第一条，焦点仍在「+n」上（节点已换新）；
//      焦点在槽外（版本按钮）时，到点回位不抢焦点；
//   ③ 找不到同名件时落到槽本身：确认条上的焦点在确认条收起后落到消息位，不掉到 body。
// 末尾是坏样例：在页内故意用裸 replaceChildren 重建槽，焦点必须掉到 body（量尺看得见焦点丢失）。
// 浏览器缺失时整套跳过（退出码 2）。
//
// 用法：node web-preview/tests/smoke-output-msgline-focus-page.mjs [仓库根]

import { createRun, repoRoot } from "./lib/harness.mjs";
import { findBrowser, launchBrowser } from "./lib/browser.mjs";
import { startStaticServer } from "./lib/static-server.mjs";
import { ROTATE_RESET_MS } from "../../web/shared/msgline.js";
import { LANGS, enableFocus, loadDesign, openOutput, press, setModel } from "../fixtures/output/lib.mjs";

const ROOT = repoRoot();
const run = createRun("Output 消息位键盘焦点");
if (!findBrowser()) run.skip("没有 Chromium 系浏览器");

const box = await loadDesign(ROOT);
const server = await startStaticServer(ROOT);
const browser = await launchBrowser();

const BAND = '[data-gb="output.band"]';
const CHIP = `${BAND} [data-part="band.more"]`;
const sleep = (page, ms) => page.eval(`new Promise((r) => setTimeout(r, ${ms}))`);
/** 当前焦点在哪：chip / band（槽本身）/ other:<描述>。 */
const where = (page) =>
    page.eval(`(() => {
        const a = document.activeElement;
        if (!a || a === document.body) return "body";
        if (a.matches('${CHIP}')) return "chip";
        if (a.matches('${BAND}')) return "band";
        return "other:" + (a.getAttribute("data-gb") || a.getAttribute("data-part") || a.tagName);
    })()`);
const shownMsg = (page) => page.eval(`document.querySelector('${BAND}').getAttribute("data-msg")`);

/** 三条灰 / 琥珀消息同时在场（chip「+2」），都不带动作按钮，轮换只靠 chip。 */
const THREE = [{ id: "ipcSandbox" }, { id: "newerProject", a: "0.11.0", b: "0.9.0" }, { id: "outOfOrder" }];

try {
    for (const lang of LANGS) {
        const page = await browser.newPage();
        await enableFocus(page);
        const tag = lang;

        // ---- ① 键盘连续轮换 ----
        await openOutput(page, server, box, lang, "");
        await setModel(page, { notices: THREE, state: "searching", group: "A" });
        run.check(
            (await page.eval(`Boolean(document.querySelector('${CHIP}'))`)) === true,
            `${tag}：三条消息同时在场应有「+n」`,
        );
        const seen = [await shownMsg(page)];
        await page.eval(`document.querySelector('${CHIP}').focus()`);
        run.eq(await where(page), "chip", `${tag}：焦点先放在「+n」上`);
        const markBefore = await page.eval(`(window.__chip0 = document.querySelector('${CHIP}'), true)`);
        run.eq(markBefore, true, `${tag}：记下旧「+n」节点`);
        for (const [i, key] of ["Enter", "Space", "Enter"].entries()) {
            await press(page, key);
            seen.push(await shownMsg(page));
            run.eq(await where(page), "chip", `${tag}：第 ${i + 1} 次键盘轮换（${key}）后焦点应还在「+n」上`);
        }
        run.eq(new Set(seen.slice(0, 3)).size, 3, `${tag}：前三次显示的是三条不同的消息，实得 ${JSON.stringify(seen)}`);
        run.eq(seen[3], seen[0], `${tag}：轮换三次回到第一条，实得 ${JSON.stringify(seen)}`);
        run.eq(
            await page.eval(`window.__chip0 !== document.querySelector('${CHIP}') && !window.__chip0.isConnected`),
            true,
            `${tag}：轮换确实重建了「+n」节点（不然焦点留着是白留）`,
        );

        // ---- ③ 确认条收起：同名件没了，焦点落到消息位本身 ----
        await openOutput(page, server, box, lang, "out-takeover-confirm");
        await page.eval(`document.querySelector('${BAND} [data-part="confirm.cancel"]').focus()`);
        run.eq(
            await page.eval(`document.activeElement.getAttribute("data-part")`),
            "confirm.cancel",
            `${tag}：焦点先放在确认条的取消上`,
        );
        await setModel(page, { notices: [], state: "searching", group: "A" }); // takeover 在 setModel 里被清掉
        await page.eval(`new Promise((r) => setTimeout(r, 60))`);
        run.eq(
            await page.eval(`document.querySelector('${BAND}').getAttribute("data-state")`),
            "idle",
            `${tag}：确认条收起后消息位回到空闲`,
        );
        run.eq(await where(page), "band", `${tag}：确认条收起后焦点应落到消息位本身（不能掉到 body）`);

        // ---- 坏样例：裸 replaceChildren 重建，焦点必须掉到 body（量尺看得见焦点丢失）----
        await openOutput(page, server, box, lang, "");
        await setModel(page, { notices: THREE, state: "searching", group: "A" });
        await page.eval(`document.querySelector('${CHIP}').focus()`);
        await page.eval(
            `(() => { const b = document.querySelector('${BAND}'); b.replaceChildren(...[...b.childNodes].map((n) => n.cloneNode(true))); })()`,
        );
        run.eq(await where(page), "body", `${tag}：坏样例（裸 replaceChildren）焦点应掉到 body，量尺才可信`);

        // ---- ② 自动回位（等 8 秒，只在中文跑一遍，焦点逻辑与语言无关）----
        if (lang === "zh") {
            // 焦点在「+n」：轮换一次（回位计时从这次点击起算），到点回到第一条，焦点仍在新的「+n」上
            await openOutput(page, server, box, lang, "");
            await setModel(page, { notices: THREE, state: "searching", group: "A" });
            const first = await shownMsg(page);
            await page.eval(`document.querySelector('${CHIP}').focus()`);
            await press(page, "Enter");
            const rotated = await shownMsg(page);
            run.check(rotated !== first, `${tag}：回车后应轮换到另一条`);
            await page.eval(`(window.__chip1 = document.querySelector('${CHIP}'), true)`);
            await sleep(page, ROTATE_RESET_MS + 600);
            run.eq(await shownMsg(page), first, `${tag}：${ROTATE_RESET_MS / 1000} 秒没人再点应回到第一条`);
            run.eq(await where(page), "chip", `${tag}：到点自动回位后焦点应还在「+n」上`);
            run.eq(
                await page.eval(`window.__chip1 !== document.querySelector('${CHIP}') && !window.__chip1.isConnected`),
                true,
                `${tag}：自动回位确实重建了「+n」节点`,
            );

            // 焦点在槽外（版本按钮）：到点回位不抢焦点
            await openOutput(page, server, box, lang, "");
            await setModel(page, { notices: THREE, state: "searching", group: "A" });
            const first2 = await shownMsg(page);
            await page.eval(`document.querySelector('${CHIP}').click()`); // 脚本点击不移焦点
            run.check((await shownMsg(page)) !== first2, `${tag}：脚本点击「+n」应轮换`);
            await page.eval(`document.querySelector('[data-gb="output.footer.version"]').focus()`);
            await sleep(page, ROTATE_RESET_MS + 600);
            run.eq(await shownMsg(page), first2, `${tag}：到点回到第一条（焦点在槽外的情形）`);
            run.eq(await where(page), "other:output.footer.version", `${tag}：焦点在槽外时到点回位不得抢焦点`);
        }

        run.check(page.errors.length === 0, `${tag}：页面不应有错误：${page.errors.slice(0, 3).join("；")}`);
        await page.close();
    }
} finally {
    await browser.close();
    await server.close();
}
run.finish();
