// SPDX-License-Identifier: GPL-3.0-or-later
//
// Output 常驻位置提示可手动关掉的页面冒烟（无头浏览器）：真页面（夹具 web-preview/fixtures/output）× 中 / 英 / 法。
// 提示右侧有一颗「知道了」按钮；点了以后经桥写本机全局位，提示不再画、版式上移；「关于 → 高级设置」里的开关可以让它回来。
// 量这些事：
//   ① 按钮本身：是 button、在 layer 层、文字与 aria-label 三语对得上、不压住红字、不越出红字块、中心点上命中的就是它、一行；
//      红字仍 ≤ 2 行；未关闭时外壳 data-place=on，六件之间的缝都是 4 px；
//   ② 点它（鼠标 / 回车 / 空格，三语各用一种）：桥收到恰好一次 setPlaceHintHidden(true)；红字不再画；
//      版式整体上移（组、配对卡、发送行、消息位各自的宽高与左缘不变，五道缝从 4 变 12，页脚不动，全在 460 x 364 内，互不重叠）；
//      键盘用户焦点不丢（落到内容层第一个停靠点）；回声之后仍是关着的；全局位真的写进去了；
//   ③ 键盘：Tab 走得到它且有焦点环；关于面板打开时它是 inert（Tab 走不到），关上后恢复；
//   ④ 关于面板：Output 有「显示位置提示」开关，位置在诊断日志之后；关着时开关为关；点它写 false、红字回来、版式复原；
//      开着时开关为开；Input 的关于面板没有这一行；
//   ⑤ 写不进去（?writeFails=1）：点「知道了」桥被调用，但红字仍在、焦点仍在按钮上；关于里的开关也留在原位；
//   ⑥ 首帧不闪：?placeHidden=1 加 ?delayFirst（首个快照晚到），快照到达之前页面就是关着的版式，逐帧记录里红字一帧都没出现过，
//      快照到了也不冒出来；没关过的用户首帧红字就在（不比以前晚）；
//   ⑦ 多窗口同步：别处把全局位改了（夹具直接改全局位再推一轮快照），这个窗口的红字随快照消失 / 回来；
//   ⑧ 关着时消息位不受影响：消息位照常显示（错序、跨轨），旁路时按钮与红字仍不随内容层降级（有效不透明度 1）；
//   ⑨ 关着时所有可见文字不越出所在件、不被裁、不互压，且没有裸词条键。
// 末尾是坏样例：夹具的 ?defect=place3 在关着的页面上不起作用（红字不在），在没关的页面上必须被行数断言抓出。
// 浏览器缺失时整套跳过（退出码 2）。
//
// 用法：node web-preview/tests/smoke-output-place-hint-page.mjs [仓库根]

import { pathToFileURL } from "node:url";
import path from "node:path";
import { createRun, repoRoot } from "./lib/harness.mjs";
import { findBrowser, launchBrowser } from "./lib/browser.mjs";
import { startStaticServer } from "./lib/static-server.mjs";
import {
    EXPECTED_PROBE,
    SCENES as SHARED_SCENES,
    loadDesign as loadSharedDesign,
    missingExpected,
    openScene,
    settleScene,
} from "./shared-measure.mjs";
import {
    LANGS,
    OUT_FIT_PROBE,
    callsOf,
    click,
    enableFocus,
    findLeaks,
    focusOf,
    lineCount,
    loadDesign,
    openOutput,
    press,
    setModel,
    tabWalk,
} from "../fixtures/output/lib.mjs";

const ROOT = repoRoot();
const run = createRun("Output 位置提示可关掉");
if (!findBrowser()) run.skip("没有 Chromium 系浏览器");

const load = (rel) => import(pathToFileURL(path.join(ROOT, rel)).href);
const OUT_DICT = (await load("web/i18n/output.js")).default;
const COMMON_DICT = (await load("web/i18n/common.js")).default;

const box = await loadDesign(ROOT);
const sharedDesign = await loadSharedDesign(ROOT);
const server = await startStaticServer(ROOT);
const browser = await launchBrowser();

const PLACE = ".rn-place";
const OK = '.rn-place [data-part="place.dismiss"]';
const SHELL = ".rn-shell";
const ADV = '[data-adv="placeHint"]';
const ROW = '[data-row="placeHint"]';
const VERSION = '[data-gb="output.footer.version"]';
/** 版式里从上到下的件（红字块在关着时不占位）。 */
const STACK_SHOWN = ["header", "place", "group", "pair", "send", "band", "foot"];
const STACK_HIDDEN = ["header", "group", "pair", "send", "band", "foot"];

const sleep = (page, ms) => page.eval(`new Promise((r) => setTimeout(r, ${ms}))`);

/** 各件的矩形与外壳的 data-place。 */
const geom = (page) =>
    page.eval(`(() => {
        const r = (sel) => {
            const e = document.querySelector(sel);
            if (!e) return null;
            const b = e.getBoundingClientRect();
            return { x: b.left, y: b.top, w: b.width, h: b.height, b: b.bottom, r: b.right };
        };
        const shell = document.querySelector("${SHELL}");
        return {
            header: r(".rn-out--header"), place: r(".rn-out--place"), group: r(".rn-out--group"), pair: r(".rn-out--pair"),
            send: r(".rn-out--send"), band: r(".rn-out--band"), foot: r(".rn-out--foot"),
            dataPlace: shell ? shell.getAttribute("data-place") : null,
        };
    })()`);

const placeShown = (page) =>
    page.eval(
        `(() => { const e = document.querySelector("${PLACE}"); if (!e) return false; const b = e.getBoundingClientRect(); return b.width > 0 && b.height > 0; })()`,
    );

/** 相邻两件的缝（下一件的顶减上一件的底）。 */
const gaps = (g, stack) => stack.slice(1).map((k, i) => Math.round((g[k].y - g[stack[i]].b) * 10) / 10);

/** 沿祖先链乘出来的有效不透明度。 */
const effOpacity = (page, sel) =>
    page.eval(
        `(() => { let o = 1; for (let n = document.querySelector(${JSON.stringify(sel)}); n; n = n.parentElement) o *= parseFloat(getComputedStyle(n).opacity); return o; })()`,
    );

/** 真实的鼠标点击（移动、按下、抬起），点在元素中心。 */
async function mouseClick(page, sel) {
    const c = await page.eval(
        `(() => { const b = document.querySelector(${JSON.stringify(sel)}).getBoundingClientRect(); return { x: b.left + b.width / 2, y: b.top + b.height / 2 }; })()`,
    );
    await page.send("Input.dispatchMouseEvent", { type: "mouseMoved", x: c.x, y: c.y });
    await page.send("Input.dispatchMouseEvent", {
        type: "mousePressed",
        x: c.x,
        y: c.y,
        button: "left",
        clickCount: 1,
    });
    await page.send("Input.dispatchMouseEvent", {
        type: "mouseReleased",
        x: c.x,
        y: c.y,
        button: "left",
        clickCount: 1,
    });
    await sleep(page, 80);
}

/** 当前快照里的开关位。 */
const hiddenInState = (page) => page.eval("window.__fx.page.snapshot().state.placeHintHidden");
const globalHidden = (page) => page.eval("window.__fx.session.core.defaults.get().placeHintHidden");

/** 量一遍适配（红字、副行、消息、页脚…）：关着时用来证明没有文字越界；返回问题条数。 */
async function fitProblems(page, tag) {
    const probe = await page.eval(OUT_FIT_PROBE);
    const expected = await page.eval(EXPECTED_PROBE);
    const bad = probe.problems.slice(0, 4).map((p) => `${p.kind}：${p.detail}`);
    run.check(probe.texts > 0, `${tag}：页上应有可见文字（防空转）`);
    run.check(probe.problems.length === 0, `${tag}：${bad.join("；")}`);
    const lost = missingExpected(expected, probe);
    run.check(lost.length === 0, `${tag}：这些文字没上屏 ${JSON.stringify(lost.map((e) => e.text))}`);
    const leaks = findLeaks(`${probe.joined} ${probe.attrs.join(" ")}`);
    run.check(leaks.length === 0, `${tag}：界面上出现了裸键、占位符或 null：${JSON.stringify(leaks.slice(0, 5))}`);
}

/** 版式上移的几何断言：before 是同一页关之前量的，after 是关之后量的。 */
function checkMovedUp(before, after, tag) {
    run.eq(after.dataPlace, "off", `${tag}：外壳应带 data-place=off`);
    run.eq(gaps(after, STACK_HIDDEN), [12, 12, 12, 12, 12], `${tag}：关着时五道缝应从 4 摊到 12 且均匀`);
    for (const k of ["group", "pair", "send", "band"]) {
        run.eq(
            [after[k].x, after[k].w, after[k].h],
            [before[k].x, before[k].w, before[k].h],
            `${tag}：${k} 的左缘与宽高不应变（只挪 top）`,
        );
        run.check(after[k].y < before[k].y, `${tag}：${k} 应上移（${before[k].y} → ${after[k].y}）`);
    }
    run.eq([after.header.y, after.header.h], [before.header.y, before.header.h], `${tag}：页眉不动`);
    run.eq([after.foot.y, after.foot.h], [before.foot.y, before.foot.h], `${tag}：页脚不动`);
    for (const k of STACK_HIDDEN) {
        const g = after[k];
        run.check(
            g.x >= 0 && g.y >= 0 && g.r <= box.w && g.b <= box.h,
            `${tag}：${k} 应在 ${box.w} x ${box.h} 的设计盒内，实得 ${JSON.stringify(g)}`,
        );
    }
    for (let i = 1; i < STACK_HIDDEN.length; i++) {
        const a = after[STACK_HIDDEN[i - 1]];
        const b = after[STACK_HIDDEN[i]];
        run.check(b.y >= a.b, `${tag}：${STACK_HIDDEN[i]} 不应压住 ${STACK_HIDDEN[i - 1]}`);
    }
}

try {
    const flows = { zh: "Enter", en: "Space", fr: "mouse" };

    for (const lang of LANGS) {
        const page = await browser.newPage();
        await enableFocus(page);
        const tag = lang;
        const okText = OUT_DICT[lang]["out.place.ok"];
        const label = OUT_DICT[lang]["out.place.dismissLabel"];
        const tip = OUT_DICT[lang]["out.place.dismissTip"];

        // ---- ① 没关过：按钮与版式 ----
        await openOutput(page, server, box, lang, "");
        const shown = await geom(page);
        run.eq(shown.dataPlace, "on", `${tag}：没关过时外壳应带 data-place=on`);
        run.eq(gaps(shown, STACK_SHOWN), [4, 4, 4, 4, 4, 4], `${tag}：没关过时六件之间的缝都是 4`);
        run.eq(await placeShown(page), true, `${tag}：没关过时红字应在`);
        const info = await page.eval(`(() => {
            const b = document.querySelector(${JSON.stringify(OK)});
            if (!b) return null;
            const r = b.getBoundingClientRect();
            const p = document.querySelector("${PLACE}").getBoundingClientRect();
            const t = document.querySelector(".rn-place__text");
            let textRight = 0;
            const w = document.createTreeWalker(t, NodeFilter.SHOW_TEXT);
            for (let n = w.nextNode(); n; n = w.nextNode()) {
                const rg = document.createRange();
                rg.selectNodeContents(n);
                for (const x of rg.getClientRects()) textRight = Math.max(textRight, x.right);
            }
            const hit = document.elementFromPoint(r.left + r.width / 2, r.top + r.height / 2);
            return {
                tag: b.tagName, type: b.getAttribute("type"), text: b.textContent.trim(), aria: b.getAttribute("aria-label"),
                title: b.getAttribute("title"), inLayer: Boolean(b.closest(".rn-shell__layer")), inContent: Boolean(b.closest(".rn-shell__content")),
                w: r.width, h: r.height, left: r.left, right: r.right, top: r.top, bottom: r.bottom,
                insidePlace: r.left >= p.left && r.right <= p.right && r.top >= p.top && r.bottom <= p.bottom,
                textRight, hitIsButton: hit === b || b.contains(hit), overflow: b.scrollWidth > b.clientWidth,
                glow: b.getAttribute("data-glow"), tabindex: b.getAttribute("tabindex"),
            };
        })()`);
        run.check(info !== null, `${tag}：红字块里应有「知道了」按钮`);
        if (info) {
            run.eq(info.tag, "BUTTON", `${tag}：应是真 button`);
            run.eq(info.type, "button", `${tag}：type 应是 button`);
            run.eq(info.text, okText, `${tag}：按钮文字`);
            run.eq(info.aria, label, `${tag}：aria-label`);
            run.eq(info.title, tip, `${tag}：title`);
            run.eq(info.inLayer, true, `${tag}：按钮应在 layer 层（旁路时不随内容层降级）`);
            run.eq(info.inContent, false, `${tag}：按钮不在 content 层`);
            run.check(info.tabindex === null || Number(info.tabindex) >= 0, `${tag}：按钮不应被移出 Tab 顺序`);
            run.eq([info.w, info.h], [56, 22], `${tag}：按钮 56 x 22`);
            run.eq(info.insidePlace, true, `${tag}：按钮应在红字块内`);
            run.check(
                info.textRight <= info.left - 2,
                `${tag}：红字不应压住按钮（字右缘 ${info.textRight}，按钮左缘 ${info.left}）`,
            );
            run.eq(info.hitIsButton, true, `${tag}：按钮中心点上命中的应是它自己`);
            run.eq(info.overflow, false, `${tag}：按钮文字不应撑破按钮`);
            run.eq(info.glow, "1", `${tag}：按钮接柔光（data-glow）`);
        }
        run.eq(await lineCount(page, OK), 1, `${tag}：按钮文字应 1 行`);
        const lines = await lineCount(page, ".rn-place__text");
        run.check(lines >= 1 && lines <= 2, `${tag}：红字应在 2 行以内，实得 ${lines} 行`);
        // 对比度分档与适配：没关着的页面上也量一遍（按钮是新增的件）
        await fitProblems(page, `${tag}/未关闭`);

        // ---- ③ 键盘：Tab 走得到、有焦点环；关于打开时 inert ----
        const stops = await tabWalk(page);
        const stop = stops.find((s) => s.part === "place.dismiss");
        run.check(
            Boolean(stop),
            `${tag}：Tab 应走得到「知道了」，实得 ${JSON.stringify(stops.map((s) => s.part || s.id))}`,
        );
        if (stop) run.eq(stop.ring, true, `${tag}：「知道了」获得焦点时应有焦点环`);

        await click(page, VERSION);
        await sleep(page, 150);
        await settleScene(page);
        run.eq(
            await page.eval(`document.querySelector("${PLACE}").hasAttribute("inert")`),
            true,
            `${tag}：关于打开时红字块应 inert`,
        );
        const stopsAbout = await tabWalk(page);
        run.check(!stopsAbout.some((s) => s.part === "place.dismiss"), `${tag}：关于打开时 Tab 不应走到「知道了」`);
        // ④ 关于里的开关：开着（提示显示中）
        run.eq(await attrOf(page, ADV, "aria-checked"), "true", `${tag}：提示显示中时「显示位置提示」应是开`);
        const rowInfo = await page.eval(`(() => {
            const row = document.querySelector('${ROW}');
            const diag = document.querySelector('[data-row="diagLog"]');
            const lab = row && row.querySelector(".rn-about__rowLabel");
            return {
                has: Boolean(row), label: lab ? lab.textContent : null, title: lab ? lab.getAttribute("title") : null,
                afterDiag: Boolean(row && diag && (diag.compareDocumentPosition(row) & Node.DOCUMENT_POSITION_FOLLOWING)),
            };
        })()`);
        run.eq(rowInfo.has, true, `${tag}：Output 的关于面板应有「显示位置提示」行`);
        run.eq(rowInfo.label, COMMON_DICT[lang]["about.adv.placeHint"], `${tag}：行标签`);
        run.eq(rowInfo.title, COMMON_DICT[lang]["about.adv.placeHintTip"], `${tag}：行说明`);
        run.eq(rowInfo.afterDiag, true, `${tag}：该行应排在「记录诊断日志」之后`);
        run.eq(await lineCount(page, `${ROW} .rn-about__rowLabel`), 1, `${tag}：行标签应 1 行`);
        await click(page, '[data-part="about.close"]');
        await sleep(page, 150);
        await settleScene(page);
        run.eq(
            await page.eval(`document.querySelector("${PLACE}").hasAttribute("inert")`),
            false,
            `${tag}：关于关上后红字块不应再 inert`,
        );

        // ---- ② 点「知道了」 ----
        await page.eval(`document.querySelector(${JSON.stringify(OK)}).focus()`);
        const how = flows[lang];
        if (how === "mouse") await mouseClick(page, OK);
        else {
            run.eq((await focusOf(page)).part, "place.dismiss", `${tag}：键盘先聚焦在「知道了」上`);
            await press(page, how);
        }
        await settleScene(page);
        run.eq(
            await callsOf(page, "setPlaceHintHidden"),
            [[true]],
            `${tag}（${how}）：桥应恰好收到一次 setPlaceHintHidden(true)`,
        );
        run.eq(await placeShown(page), false, `${tag}（${how}）：点了以后红字不再画`);
        run.eq(await hiddenInState(page), true, `${tag}（${how}）：页面状态应是关着`);
        run.eq(await globalHidden(page), true, `${tag}（${how}）：全局位应写进去了`);
        const focus = await page.eval(
            `(() => { const a = document.activeElement; return { body: !a || a === document.body, inContent: Boolean(a && a.closest(".rn-shell__content")) }; })()`,
        );
        run.eq(focus, { body: false, inContent: true }, `${tag}（${how}）：焦点不应丢到 body，应落到内容层的停靠点`);
        const hidden = await geom(page);
        checkMovedUp(shown, hidden, `${tag}（${how}）`);
        // 回声（含乱序的旧帧）之后仍是关着
        await sleep(page, 700);
        run.eq(await placeShown(page), false, `${tag}（${how}）：回声之后红字不应冒回来`);
        run.eq(await callsOf(page, "setPlaceHintHidden"), [[true]], `${tag}（${how}）：之后不应再有写入`);
        await fitProblems(page, `${tag}/关着`);
        run.eq(
            (await lineCount(page, ".rn-out--band .rn-banner__idle, .rn-out--band .rn-banner__text")) >= 1,
            true,
            `${tag}：关着时消息位文字仍在`,
        );

        // ---- ④ 关于里恢复 ----
        await click(page, VERSION);
        await sleep(page, 150);
        await settleScene(page);
        run.eq(await attrOf(page, ADV, "aria-checked"), "false", `${tag}：关着时「显示位置提示」应是关`);
        await click(page, ADV);
        await sleep(page, 200);
        await settleScene(page);
        run.eq(await callsOf(page, "setPlaceHintHidden"), [[true], [false]], `${tag}：开关应写 false`);
        run.eq(await attrOf(page, ADV, "aria-checked"), "true", `${tag}：写成功后开关应是开`);
        await click(page, '[data-part="about.close"]');
        await sleep(page, 150);
        await settleScene(page);
        run.eq(await placeShown(page), true, `${tag}：恢复后红字应回来`);
        const back = await geom(page);
        run.eq(back.dataPlace, "on", `${tag}：恢复后外壳应带 data-place=on`);
        run.eq(
            STACK_SHOWN.map((k) => [back[k].x, back[k].y, back[k].w, back[k].h]),
            STACK_SHOWN.map((k) => [shown[k].x, shown[k].y, shown[k].w, shown[k].h]),
            `${tag}：恢复后版式应与最初一模一样`,
        );
        run.eq(await globalHidden(page), false, `${tag}：恢复后全局位应是显示`);

        run.check(page.errors.length === 0, `${tag}：页面不应有错误：${page.errors.slice(0, 3).join("；")}`);
        await page.close();
    }

    // ---- ⑥ 首帧不闪（中文一遍，与语言无关） ----
    {
        const page = await browser.newPage();
        // 关过的用户重开：首个快照晚 2.5 秒到，此间只能靠首帧预置
        await openOutput(page, server, box, "zh", "", "&placeHidden=1&delayFirst=2500");
        run.eq(
            await page.eval("window.__fx.page.snapshot().state.seq"),
            -1,
            "首帧（关着）：此刻首个快照还没到（防空转）",
        );
        run.eq(await placeShown(page), false, "首帧（关着）：快照到之前红字就不在");
        const pre = await geom(page);
        run.eq(pre.dataPlace, "off", "首帧（关着）：快照到之前外壳就是 data-place=off");
        run.eq(gaps(pre, STACK_HIDDEN), [12, 12, 12, 12, 12], "首帧（关着）：快照到之前版式就是上移后的");
        await page.waitFor("window.__fx.page.snapshot().state.seq >= 0", { timeout: 8000, label: "首个快照应到" });
        await sleep(page, 300);
        run.eq(await placeShown(page), false, "首帧（关着）：快照到了以后红字也不冒出来");
        const frames = await page.eval("window.__fx.placeFrames");
        run.check(frames.length > 20, `首帧（关着）：逐帧记录应有足够多的帧（防空转），实得 ${frames.length}`);
        run.eq(frames.filter((f) => f.shown).length, 0, "首帧（关着）：逐帧记录里红字一帧都不该出现");
        await page.close();
    }
    {
        const page = await browser.newPage();
        // 没关过的用户：首个快照晚到，红字从第一帧起就在（不比以前晚）
        await openOutput(page, server, box, "zh", "", "&delayFirst=2500");
        run.eq(
            await page.eval("window.__fx.page.snapshot().state.seq"),
            -1,
            "首帧（没关）：此刻首个快照还没到（防空转）",
        );
        run.eq(await placeShown(page), true, "首帧（没关）：快照到之前红字就在");
        await page.waitFor("window.__fx.page.snapshot().state.seq >= 0", { timeout: 8000, label: "首个快照应到" });
        await sleep(page, 200);
        const frames = await page.eval("window.__fx.placeFrames");
        run.check(frames.length > 20, `首帧（没关）：逐帧记录应有足够多的帧，实得 ${frames.length}`);
        run.eq(frames[0].shown, true, "首帧（没关）：第一帧红字就在");
        run.eq(frames.filter((f) => !f.shown).length, 0, "首帧（没关）：每一帧红字都在");
        await page.close();
    }

    // ---- ⑤ 写不进去：红字留着，焦点留着，开关留着 ----
    {
        const page = await browser.newPage();
        await enableFocus(page);
        await openOutput(page, server, box, "zh", "", "&writeFails=1");
        await page.eval(`document.querySelector(${JSON.stringify(OK)}).focus()`);
        // 逐帧记红字是否在屏：「先消失、下一帧快照又把它带回来」的乐观写法，事后一次性看是看不出来的
        await page.eval(`(() => {
            window.__pf = [];
            const f = () => {
                const e = document.querySelector("${PLACE}");
                const b = e && e.getBoundingClientRect();
                window.__pf.push(Boolean(b && b.width > 0 && b.height > 0));
                if (window.__pf.length < 150) requestAnimationFrame(f);
            };
            requestAnimationFrame(f);
        })()`);
        await press(page, "Enter");
        await sleep(page, 1500);
        await settleScene(page);
        const pf = await page.eval("window.__pf");
        run.check(pf.length > 60, `写不进去：逐帧记录应有足够多的帧（防空转），实得 ${pf.length}`);
        run.eq(pf.filter((x) => !x).length, 0, "写不进去：红字一帧都不该消失（没存住就不能先装作关掉了）");
        run.eq(await callsOf(page, "setPlaceHintHidden"), [[true]], "写不进去：桥仍应被调用一次");
        run.eq(await placeShown(page), true, "写不进去：红字应留着（没存住就不能装作关掉了）");
        run.eq((await geom(page)).dataPlace, "on", "写不进去：版式不应上移");
        run.eq((await focusOf(page)).part, "place.dismiss", "写不进去：焦点应仍在按钮上");
        run.eq(await hiddenInState(page), false, "写不进去：页面状态仍是显示");
        // 关于里：先让它是「关着」的才有开关可点——这里是显示态，点开关想隐藏不可能，所以从关于面板里把开关点一下看它留在原位
        await click(page, VERSION);
        await sleep(page, 150);
        await settleScene(page);
        await click(page, ADV); // 想关掉提示（开 -> 关）
        await sleep(page, 200);
        run.eq(await callsOf(page, "setPlaceHintHidden"), [[true], [true]], "写不进去：关于里的开关也走同一个桥函数");
        run.eq(await attrOf(page, ADV, "aria-checked"), "true", "写不进去：开关应留在原位（开）");
        run.eq(await placeShown(page), true, "写不进去：关于里点开关后红字仍在");
        run.check(page.errors.length === 0, `写不进去：页面不应有错误：${page.errors.slice(0, 3).join("；")}`);
        await page.close();
    }

    // ---- ⑦ 多窗口同步：别处改了全局位，随下一轮快照跟上 ----
    {
        const page = await browser.newPage();
        await openOutput(page, server, box, "zh", "");
        run.eq(await placeShown(page), true, "同步：起始红字在");
        await page.eval(
            "(() => { window.__fx.session.core.defaults.set({ placeHintHidden: true }); window.__fx.session.core.pushAll(); })()",
        );
        await page.waitFor("window.__fx.page.snapshot().state.placeHintHidden === true", {
            timeout: 4000,
            label: "别处关掉后快照应带 true",
        });
        await settleScene(page);
        run.eq(await placeShown(page), false, "同步：别处关掉后这个窗口的红字随快照消失");
        run.eq((await geom(page)).dataPlace, "off", "同步：版式随之上移");
        run.eq(await callsOf(page, "setPlaceHintHidden"), [], "同步：这个窗口自己没有写过（不回写）");
        await page.eval(
            "(() => { window.__fx.session.core.defaults.set({ placeHintHidden: false }); window.__fx.session.core.pushAll(); })()",
        );
        await page.waitFor("window.__fx.page.snapshot().state.placeHintHidden === false", {
            timeout: 4000,
            label: "别处恢复后快照应带 false",
        });
        await settleScene(page);
        run.eq(await placeShown(page), true, "同步：别处恢复后这个窗口的红字回来");
        await page.close();
    }

    // ---- ⑧ 关着时消息位与旁路 ----
    {
        const page = await browser.newPage();
        for (const lang of LANGS) {
            // 错序：消息位照常显示这条琥珀提示，位置在上移后的消息位里
            await openOutput(page, server, box, lang, "", "&placeHidden=1");
            await setModel(page, { notices: [{ id: "outOfOrder" }], state: "searching", group: "A" });
            const msg = await page.eval(
                `(() => { const b = document.querySelector('[data-gb="output.band"]'); const r = b.getBoundingClientRect(); return { msg: b.getAttribute("data-msg"), shown: r.width > 0 && r.height > 0, text: (b.textContent || "").trim().length, bottom: r.bottom }; })()`,
            );
            run.eq(msg.msg, "band.out.outOfOrder", `${lang}：红字关着时错序提示仍应显示`);
            run.eq(msg.shown, true, `${lang}：错序提示的消息位应可见`);
            run.check(msg.text > 0, `${lang}：错序提示应有文字`);
            run.eq(await placeShown(page), false, `${lang}：此时红字仍是关着`);
            await fitProblems(page, `${lang}/关着/错序`);
        }
        // 跨轨：整套场景
        await openOutput(page, server, box, "zh", "out-cross-track", "&placeHidden=1");
        run.eq(
            await page.eval(`document.querySelector('[data-gb="output.band"]').getAttribute("data-msg")`),
            "band.out.crossTrack",
            "关着：跨轨场景的消息位仍应显示跨轨提示",
        );
        await fitProblems(page, "zh/关着/跨轨场景");
        // 旁路：内容层降到 .55，红字与按钮（layer 层）不降
        await openOutput(page, server, box, "zh", "out-bypassed");
        run.check((await effOpacity(page, ".rn-out--pair")) < 0.9, "旁路：内容层应被降级（防空转）");
        run.eq(await effOpacity(page, PLACE), 1, "旁路：红字不随内容层降级");
        run.eq(await effOpacity(page, OK), 1, "旁路：「知道了」不随内容层降级");
        await page.close();
    }

    // ---- ④ Input 的关于面板没有这一行 ----
    {
        const page = await browser.newPage();
        const about = SHARED_SCENES.find((s) => s.id === "about");
        for (const lang of LANGS) {
            await openScene(page, server, sharedDesign, "input", lang, about);
            const rows = await page.eval(
                `({ diag: Boolean(document.querySelector('[data-adv="diagLog"]')), place: Boolean(document.querySelector('${ADV}')), placeRow: Boolean(document.querySelector('${ROW}')) })`,
            );
            run.eq(rows.diag, true, `Input/${lang}：关于面板应有诊断日志开关（防空转）`);
            run.eq([rows.place, rows.placeRow], [false, false], `Input/${lang}：关于面板不应有「显示位置提示」`);
        }
        await page.close();
    }

    // ---- 坏样例：place3（红字翻倍成三行）在没关的页面上必须被行数断言抓出，在关着的页面上不起作用 ----
    {
        const page = await browser.newPage();
        await openOutput(page, server, box, "zh", "", "&defect=place3");
        const lines = await lineCount(page, ".rn-place__text");
        run.check(lines > 2, `坏样例 place3：红字应被撑到 2 行以上（量尺可信），实得 ${lines} 行`);
        await openOutput(page, server, box, "zh", "", "&defect=place3&placeHidden=1");
        run.eq(await placeShown(page), false, "坏样例 place3 + 关着：红字不在");
        await page.close();
    }
} finally {
    await browser.close();
    await server.close();
}

async function attrOf(page, sel, name) {
    return page.eval(
        `(() => { const e = document.querySelector(${JSON.stringify(sel)}); return e ? e.getAttribute(${JSON.stringify(name)}) : null; })()`,
    );
}

run.finish();
