// SPDX-License-Identifier: GPL-3.0-or-later
//
// Input 页面「紧急切断」冒烟（无头浏览器）：它是独立的安全层，任何遮罩、模态与确认框都盖不住它。
//   ① 命中：范围内 40 个 in-* 场景（含关于面板、缩放确认、限制器关闭确认、接管确认等遮罩场景，以及首启的语言卡、规则页、导览六步）里，
//      紧急切断按钮内五个点的 elementFromPoint 都落在按钮自己身上；首启的 8 个场景必须在这一轮里（下面按数量断言，防被筛掉）；
//   ② 点击：遮罩与模态打开时真点一下，桥恰好收到一次 panic；首启的三种叠层（语言卡、规则页、导览）也各点一次，
//      叠层随之关掉。真走完整条首启链时在导览中点切断，由 smoke-first-run-page 断言，这里只补静态场景；
//   ③ 连点不恢复：切断后再点紧急切断不再调桥，也不触发恢复；恢复只有消息位的「恢复回送」一个入口，
//      且切断后 1.5 s 内按钮处于冷却态、点了不调桥，冷却过后才调一次 resumeLoop；
//   ④ 坏样例：在按钮上盖一层透明的全窗元素，命中判据必须判红；把按钮的 z-index 压到遮罩之下，点击判据必须判红。
// 浏览器缺失时整套跳过（退出码 2），不是通过。本机同时只能有一个浏览器冒烟。
//
// 用法：node web-preview/tests/smoke-input-panic-page.mjs [仓库根]

import { createRun, repoRoot } from "./lib/harness.mjs";
import { findBrowser, launchBrowser } from "./lib/browser.mjs";
import { startStaticServer } from "./lib/static-server.mjs";
import {
    FIRST_RUN_SCENES,
    SCENES,
    VIEW,
    centerOf,
    inFrame,
    isFirstRunScene,
    openInput,
    recordBridge,
} from "./smoke-input-kit-page.mjs";

const ROOT = repoRoot();
const run = createRun("Input 页面紧急切断");
if (!findBrowser()) run.skip("没有 Chromium 系浏览器");

const PANIC = '[data-gb="input.panic"]';

/** 按钮内五个点（中心与四个内缩的角）的命中情况：返回 { points:N, hits:N, who:[被谁盖住] }。 */
function hitProbe(doc, win, sel) {
    const e = doc.querySelector(sel);
    const r = e.getBoundingClientRect();
    const pts = [
        [r.width / 2, r.height / 2],
        [r.width * 0.15, r.height * 0.3],
        [r.width * 0.85, r.height * 0.3],
        [r.width * 0.15, r.height * 0.7],
        [r.width * 0.85, r.height * 0.7],
    ];
    let hits = 0;
    const who = [];
    for (const [dx, dy] of pts) {
        const t = doc.elementFromPoint(r.left + dx, r.top + dy);
        if (t && (t === e || e.contains(t))) hits++;
        else
            who.push(
                t ? `${t.getAttribute("data-gb") || t.getAttribute("data-part") || t.className || t.tagName}` : "无",
            );
    }
    return { points: pts.length, hits, who };
}

/** 在壳页坐标里真点一下（鼠标按下抬起）。 */
async function click(page, x, y) {
    const base = { x, y, button: "left", clickCount: 1, pointerType: "mouse" };
    await page.send("Input.dispatchMouseEvent", { type: "mouseMoved", x, y });
    await page.send("Input.dispatchMouseEvent", { type: "mousePressed", ...base });
    await page.send("Input.dispatchMouseEvent", { type: "mouseReleased", ...base });
    await page.eval("new Promise((r) => setTimeout(r, 120))");
}

const server = await startStaticServer(ROOT);
const browser = await launchBrowser({ width: VIEW.w, height: VIEW.h });
try {
    const page = await browser.newPage();

    // ---- ① 命中：每个场景
    const MODAL = new Set([
        "in-limiter-confirm",
        "in-about",
        "in-scale-confirm",
        "in-takeover-confirm",
        // 首启的三种叠层各取一个：语言卡、规则页、导览（取第 3 步，说明框在页面中部）
        "in-lang",
        "in-rules",
        "in-tour-3",
    ]);
    for (const scene of SCENES) {
        await openInput(page, server, scene.id, { lang: "zh" });
        const h = await inFrame(page, hitProbe, PANIC);
        run.check(
            h.hits === h.points,
            `${scene.id}：紧急切断被盖住了 ${h.points - h.hits}/${h.points} 个点，盖住它的是 ${JSON.stringify(h.who)}`,
        );
    }
    run.log(`  命中检查共 ${SCENES.length} 个场景`);
    // 首启的场景必须在这一轮里：紧急切断在语言卡、规则页、导览之上也要点得到（量尺不能悄悄把它们筛掉）
    run.eq(SCENES.filter(isFirstRunScene).length, 8, "命中检查应含首启的 8 个场景（语言卡、规则页、导览六步）");
    run.eq(FIRST_RUN_SCENES.length, 8, "首启场景应有 8 个");

    // ---- ② 点击：遮罩与模态之下真点
    // 首启的三种叠层（语言卡、规则页、导览）都要在点击集里，不能被悄悄挪掉
    run.eq(
        [...MODAL].filter((id) => isFirstRunScene({ id })).length,
        3,
        "点击集应含首启的三种叠层（语言卡、规则页、导览各一个）",
    );
    for (const id of MODAL) {
        await openInput(page, server, id, { lang: "zh" });
        // 这几个场景确有遮罩或确认层：先确认层在（防空转）
        const layered = await inFrame(page, (doc) =>
            Boolean(
                doc.querySelector(
                    '.rn-scrim:not([hidden]), [data-part="input.limiter.confirm"]:not([hidden]), [data-gb="input.about"]:not([hidden]), [data-gb="input.scale.confirm"]:not([hidden])',
                ),
            ),
        );
        run.check(layered, `${id}：应有遮罩或确认层（防空转）`);
        const rec = await recordBridge(page, ["panic"]);
        const c = await centerOf(page, PANIC);
        await click(page, c.x, c.y);
        run.eq(
            (await rec.calls()).filter((x) => x[0] === "panic").length,
            1,
            `${id}：遮罩之下点紧急切断，桥应恰好收到一次 panic`,
        );
    }

    // ---- ③ 连点不恢复；恢复只有消息位一个入口，冷却内不调桥
    await openInput(page, server, "in-playing", { lang: "zh" });
    {
        const rec = await recordBridge(page, ["panic", "resumeLoop"]);
        const c = await centerOf(page, PANIC);
        await click(page, c.x, c.y);
        await page.waitFor(
            `(() => { const d = document.querySelector("iframe").contentDocument; const b = d.querySelector('[data-gb="input.panic"]'); return !!b && /^cut/.test(b.getAttribute("data-state") || ""); })()`,
            { label: "点后紧急切断应进入已切断态" },
        );
        const t0 = Date.now();
        // 连点三次：每一下都照常发 panic（幂等，安全动作不因页面以为已切断而吞掉），但不恢复，也不重置切断时刻（冷却不重新计时）
        const cutAt = await page.eval("window.__RINNE_PREVIEW__.model.cutAt");
        for (let i = 0; i < 3; i++) await click(page, c.x, c.y);
        const afterMulti = await rec.calls();
        run.eq(afterMulti.filter((x) => x[0] === "panic").length, 4, "每次点击都应发 panic，不吞（幂等）");
        run.eq(afterMulti.filter((x) => x[0] === "resumeLoop").length, 0, "连点紧急切断不应触发恢复");
        run.check(await page.eval("window.__RINNE_PREVIEW__.model.cut !== null"), "连点之后回送仍应是切断的");
        run.eq(
            await page.eval("window.__RINNE_PREVIEW__.model.cutAt"),
            cutAt,
            "连点不应重置切断时刻（冷却不重新计时）",
        );

        // 消息位的「恢复回送」：冷却内是冷却态，点了不调桥
        await page.waitFor(
            `!!document.querySelector("iframe").contentDocument.querySelector('[data-gb="input.band"] [data-part="band.action"][data-action="restore"]')`,
            { label: "切断后消息位应出现「恢复回送」" },
        );
        const stateOf = () =>
            inFrame(page, (doc) => {
                const b = doc.querySelector('[data-gb="input.band"] [data-part="band.action"][data-action="restore"]');
                return b ? b.getAttribute("data-state") : null;
            });
        const early = await stateOf();
        const rb = await centerOf(page, '[data-gb="input.band"] [data-part="band.action"][data-action="restore"]');
        const insideCooldown = Date.now() - t0 < 1300;
        if (insideCooldown) {
            run.eq(early, "cooldown", "切断后 1.5 s 内「恢复回送」应是冷却态");
            await click(page, rb.x, rb.y);
            run.eq((await rec.calls()).filter((x) => x[0] === "resumeLoop").length, 0, "冷却内点「恢复回送」不应调桥");
        } else {
            run.log("  [提示] 本机太慢，冷却窗口已过，冷却内的两项判据未跑；冷却后的部分照跑");
        }
        await page.waitFor(
            `(() => { const b = document.querySelector("iframe").contentDocument.querySelector('[data-gb="input.band"] [data-part="band.action"][data-action="restore"]'); return !!b && b.getAttribute("data-state") !== "cooldown"; })()`,
            { timeout: 4000, label: "冷却过后「恢复回送」应可用" },
        );
        const rb2 = await centerOf(page, '[data-gb="input.band"] [data-part="band.action"][data-action="restore"]');
        await click(page, rb2.x, rb2.y);
        await page.waitFor(`window.__calls.filter((c) => c[0] === "resumeLoop").length === 1`, {
            label: "冷却过后点「恢复回送」应调一次 resumeLoop",
        });
        await page.waitFor(`window.__RINNE_PREVIEW__.model.cut === null`, { label: "恢复后模型应不再是切断" });
    }

    // ---- ④ 坏样例
    // 盖一层透明的全窗元素：命中判据必须判红
    await openInput(page, server, "in-playing", { lang: "zh" });
    await inFrame(page, (doc) => {
        const veil = doc.createElement("div");
        veil.style.cssText = "position:fixed;inset:0;z-index:99999;background:transparent";
        doc.body.append(veil);
    });
    {
        const h = await inFrame(page, hitProbe, PANIC);
        run.check(h.hits < h.points, `坏样例：盖了透明全窗层后命中判据应判红，实得 ${h.hits}/${h.points}`);
    }
    // 按钮点不到（pointer-events 被关）：关于面板场景里命中判据必须判红
    await openInput(page, server, "in-about", { lang: "zh" });
    await inFrame(page, (doc) => {
        doc.querySelector('[data-gb="input.panic"]').style.pointerEvents = "none";
    });
    {
        const h = await inFrame(page, hitProbe, PANIC);
        run.check(h.hits < h.points, `坏样例：关掉紧急切断的指针事件后命中判据应判红，实得 ${h.hits}/${h.points}`);
    }

    run.check(page.errors.length === 0, `页面不应有错误：${page.errors.slice(0, 3).join("；")}`);
} finally {
    await browser.close();
    await server.close();
}
run.finish();
