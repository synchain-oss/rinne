// SPDX-License-Identifier: GPL-3.0-or-later
//
// Output 页面级冒烟的公共件：场景表、开页（夹具 web-preview/fixtures/output/index.html）、Output 的对比度分档与量尺。
// 只放可复用的函数，不自己跑断言；smoke-output-*.mjs 各自引用。探针与 PNG 解码复用 shared-measure.mjs，不另写一份。

import { readFileSync } from "node:fs";
import { pathToFileURL } from "node:url";
import path from "node:path";
import {
    CONTRAST_PROBE,
    FIT_PROBE,
    HIDE_TEXT_STYLE as BASE_HIDE_TEXT_STYLE,
    decodePng,
    itemsDrift,
    rectsDrift,
    settleScene,
    worstContrast,
} from "../../tests/shared-measure.mjs";
import {
    SVG_HIDE_STYLE,
    accessibleName,
    clipToAncestorScroller,
    expectedOverlay,
    findLeaks,
    inlineProbe,
    isFirstRunSceneId,
    overlayA11yProbe,
} from "../first-run/measure.mjs";

/** 裸词条键、占位符、null 的扫描：共用的那一份，再加首启词条的前缀（rules.、tour.、hint.），见 fixtures/first-run/measure.mjs。 */
export { findLeaks };

export const LANGS = Object.freeze(["zh", "en", "fr"]);

/** 量尺截图前把页内文字藏起来的样式：共用件的那一份，加上 SVG 文字与插入链示意的描边（理由见 SVG_HIDE_STYLE）。 */
const HIDE_TEXT_STYLE = BASE_HIDE_TEXT_STYLE + SVG_HIDE_STYLE;

/** 首启链（语言卡、规则页、导览各步）的场景：静态画面，叠层压在整页上，由夹具接首启控制器摆出来。 */
export const isFirstRunScene = (s) => isFirstRunSceneId(s.id);

/**
 * Output 的全部场景（来自预览场景表，不另抄一份 ID），含首启的 6 个。
 * 不关心首启的使用方要自己用 isFirstRunScene 排除，并在那里写明理由。
 */
export async function loadOutputScenes(root) {
    const m = await import(pathToFileURL(path.join(root, "web-preview/mock/scenarios.js")).href);
    return m.SCENES.filter((s) => s.role === "output");
}

/**
 * 一个「画面」是场景加子态：每个场景一个画面；规则页静态摆成「未放行」，再多一个「放行后」（sub 为 ready，滚到底放行）的画面，
 * 两种按钮状态都要量到。适配、对比度、无障碍三套都按画面走。
 */
export const outputFrames = (scenes) =>
    scenes.flatMap((scene) =>
        scene.id === "out-rules"
            ? [
                  { scene, sub: "" },
                  { scene, sub: "ready" },
              ]
            : [{ scene, sub: "" }],
    );
export const frameTag = (lang, frame) => `${lang}/${frame.scene.id}${frame.sub ? ":" + frame.sub : ""}`;

export async function loadDesign(root) {
    const m = await import(pathToFileURL(path.join(root, "web/shared/design-box.js")).href);
    return m.DESIGN.output;
}

export function fixtureUrl(server, lang, scenario, extra = "") {
    const q = new URLSearchParams({ lang });
    if (scenario) q.set("scenario", scenario);
    return `${server.url}/web-preview/fixtures/output/index.html?${q}${extra}`;
}

/** 打开夹具并等页面揭幕；夹具自己报错时抛出。视口取设计盒（倍率 1）。 */
export async function openOutput(page, server, box, lang, scenario, extra = "") {
    await page.setViewport(box.w, box.h);
    await page.goto(fixtureUrl(server, lang, scenario, extra));
    await page.waitFor("(window.__fx && window.__fx.ready === true) || Boolean(window.__fxError)", { timeout: 15000 });
    const err = await page.eval("window.__fxError || ''");
    if (err) throw new Error(`夹具出错（${lang}/${scenario || "默认"}）：${err}`);
    await settleScene(page);
}

/** 摆出一个画面：开场景，规则页的放行后画面再滚到底。 */
export async function openOutputFrame(page, server, box, lang, frame, extra = "") {
    await openOutput(page, server, box, lang, frame.scene.id, extra);
    if (frame.sub === "ready") await releaseRules(page);
}

/** 点一个元素（页内 click）；找不到返回 false。 */
export const click = (page, sel) =>
    page.eval(
        `(() => { const e = document.querySelector(${JSON.stringify(sel)}); if (!e) return false; e.click(); return true; })()`,
    );

export const callsOf = (page, name) =>
    page.eval(`window.__fx.calls.filter((c) => c.name === ${JSON.stringify(name)}).map((c) => c.args)`);

export const attr = (page, sel, name) =>
    page.eval(
        `(() => { const e = document.querySelector(${JSON.stringify(sel)}); return e ? e.getAttribute(${JSON.stringify(name)}) : null; })()`,
    );

export const textOf = (page, sel) =>
    page.eval(
        `(() => { const e = document.querySelector(${JSON.stringify(sel)}); return e ? (e.textContent || "").trim() : null; })()`,
    );

/** 页内文字所占的行数：逐个文本节点取行矩形，顶边去重（只看文字，不看容器自己的盒）。 */
export const lineCount = (page, sel) =>
    page.eval(
        `(() => { const e = document.querySelector(${JSON.stringify(sel)}); if (!e) return -1; const tops = new Set(); const w = document.createTreeWalker(e, NodeFilter.SHOW_TEXT); for (let n = w.nextNode(); n; n = w.nextNode()) { if (!n.nodeValue.trim()) continue; const r = document.createRange(); r.selectNodeContents(n); for (const x of r.getClientRects()) if (x.width > 0) tops.add(Math.round(x.top / 3)); } return tops.size; })()`,
    );

/**
 * 把已摆出的规则页（out-rules，静态摆成「未放行」）放行：和用户一样把正文区滚到底，页面自己放行按钮。
 * 静态场景不起 8 秒计时，所以只能走「读到底」这一条路；等到 data-state 变成 ready 才返回。
 */
export async function releaseRules(page) {
    await page.eval(
        `(() => { const b = document.querySelector('[data-gb="output.rules"] [data-part="rules.body"]'); b.scrollTop = b.scrollHeight; b.dispatchEvent(new Event("scroll")); })()`,
    );
    await page.waitFor(`document.querySelector('[data-gb="output.rules"]').getAttribute("data-state") === "ready"`, {
        timeout: 6000,
        label: "规则页滚到底后应放行（data-state=ready）",
    });
    await settleScene(page);
}

/** 叠层的无障碍现状（页内求值 overlayA11yProbe，角色取 output）。 */
export const overlayState = (page) => page.eval(`(${inlineProbe(overlayA11yProbe)})(document, window, "output")`);

/**
 * 真页面（web/output/app.js）喂给首启控制器的两个判断：危险描边 view.danger（页面自己的视图模型）与「环路这头正在回送」outputFlowing。
 * outputFlowing 在带副作用的入口模块里、不能 import，所以按源码原样取下来在页内求值（不另写一份）；
 * 它的两个参数和真页面一样是桥上收到的原始帧（夹具存在 __fx.raw 里），不是页面规范化之后的快照。
 * 源码的形状变了就抛错，不悄悄放过（和上面 derive 的锚点同一个做法）。
 */
const APP_SRC = readFileSync(new URL("../../../web/output/app.js", import.meta.url), "utf8");
const FLOWING_SRC =
    (/const FLOWING = new Set\(\[[^\]]*\]\);\s*function outputFlowing\(state, pair\) \{[\s\S]*?\n\}/.exec(APP_SRC) ||
        [])[0];
if (!FLOWING_SRC)
    throw new Error("web/output/app.js 里的 outputFlowing 变了，请同步 web-preview/fixtures/output/lib.mjs");
export const FIRST_RUN_FEED_PROBE = `(() => {
    ${FLOWING_SRC}
    const { view } = window.__fx.page.snapshot();
    const { state, pair } = window.__fx.raw;
    return { danger: view.danger === true, flowing: state && pair ? outputFlowing(state, pair) : null };
})()`;

/**
 * 首启画面的叠层必须真的摆出来（量尺不能在一张没有叠层的普通页面上空转）：叠层恰好一个、种类与导览步号对得上，
 * 规则页的放行状态与子态一致；静态首启场景不带危险态与环路活动（夹具喂首启控制器的运行态据此写死，
 * 真页面在这些场景里喂的也是假；此刻真页面的判断若为真，夹具摆的就不是真页面会摆的画面）。
 * 返回问题列表，空表示都对；不是首启场景的画面直接返回空表。
 */
export async function overlayFailures(page, frame) {
    const want = expectedOverlay(frame.scene.id);
    if (!want) return [];
    const o = await overlayState(page);
    const out = [];
    const feed = await page.eval(FIRST_RUN_FEED_PROBE);
    if (feed.danger) out.push("首启画面不该带危险态：真页面的 view.danger 为真");
    if (feed.flowing === null) out.push("夹具没有收到桥上的原始 rinne.state / rinne.pair 帧，求不出环路是否在流动");
    if (feed.flowing) out.push("首启画面的环路不该在流动：真页面的 outputFlowing 为真");
    if (o.kind !== want.kind) out.push(`叠层应是 ${want.kind}，实得 ${JSON.stringify(o.open)}`);
    if (want.kind === "tour" && o.step !== want.step) out.push(`导览应在第 ${want.step} 步，实得 ${o.step}`);
    if (want.kind === "rules") {
        const state = frame.sub === "ready" ? "ready" : "gated";
        if (!o.rules || o.rules.state !== state) out.push(`规则页应是 ${state}，实得 ${o.rules && o.rules.state}`);
    }
    return out;
}

// ---------------------------------------------------------------- 造状态

/** 每条消息单独成立的构造（与 view-model 单测同表）：[通知, 配对状态, 配对覆盖]。 */
export const MESSAGE_CASES = {
    conflict: [[], "conflict", {}],
    unassigned: [[], "unassigned", {}],
    abi: [[], "abiMismatch", { version: "0.10.12" }],
    srMismatch: [[], "srMismatch", { sampleRate: 44100 }],
    ipc: [[], "ipcUnavailable", {}],
    ipcSandbox: [[{ id: "ipcSandbox" }], "searching", {}],
    newerProject: [[{ id: "newerProject", a: "0.11.0", b: "0.9.0" }], "searching", {}],
    stateDecodeFailed: [[{ id: "stateDecodeFailed" }], "searching", {}],
    stale: [[], "stale", {}],
    crossProcess: [[], "crossProcess", {}],
    crossTrack: [[], "crossTrack", {}],
    outOfOrder: [[{ id: "outOfOrder" }], "searching", {}],
    waitingAudio: [[], "waitingAudio", {}],
    validator: [[], "validator", {}],
};

/** 页内求值：把 mock 模型改成 { notices, 配对状态, 对端覆盖, 额外 }，发一轮帧。 */
export const setModel = (
    page,
    { notices = [], state = "connected", peer = {}, self = {}, verify = "match", group = "A", send = {} },
) =>
    page.eval(
        `window.__fx.mutate((m) => {
            m.notices = ${JSON.stringify(notices)};
            m.takeover = false;
            m.group = ${JSON.stringify(group)};
            m.pair.state = ${JSON.stringify(state)};
            m.pair.verify = ${JSON.stringify(verify)};
            m.pair.peer = { name: "Vox Lead", color: "rgb(45,121,198)", channels: 2, version: null, holder: null, sampleRate: null, ...${JSON.stringify(peer)} };
            m.pair.self = { name: "Vox Lead", color: "rgb(45,121,198)", ...${JSON.stringify(self)} };
            m.send = { ...m.send, ...${JSON.stringify(send)} };
        })`,
    );

// ---------------------------------------------------------------- 对比度

/**
 * Output 的分档：在共享分档之上，把常驻红字（安全信息）与配对卡的正文（副行、轨道名）划入 A 档。
 * 共享探针里这些都落在 C（不判），而它们恰恰是用户必须读得清的字。
 */
const SHARED_ANCHOR = 'if (p.closest(".rn-pair")) return "B";';
const FX_CAP_ANCHOR = 'if (p.closest(".fx-cap")) return null;';
const MODAL_ANCHOR = 'if (p.closest(".rn-modal")) return "A";';
const SCRIM_ON_ANCHOR = 'const scrimOn = [...stage.querySelectorAll(".rn-scrim")].some(shown);';
const SCRIM_SKIP_ANCHOR = 'if (scrimOn && !p.closest(".rn-scrim")) continue;';
const COLOR_ANCHOR = "const c = parse(getComputedStyle(p).color);";
const RECT_ANCHOR = "return { x: left, y: top, w: right - left, h: bottom - top };";
const LAYER_ANCHOR = 'e.closest(".rn-scrim") ? "modal"';
const ITEM_ANCHOR = 'tone: toneEl ? toneEl.getAttribute("data-tone") : "", text: text.slice(0, 48),';

/** 在共享探针上按锚点改写；锚点变了就抛错，不悄悄放过（共享探针另有人改时，这里必须跟着看一眼）。 */
function derive(base, edits) {
    let out = base;
    for (const [anchor, text] of edits) {
        if (!out.includes(anchor))
            throw new Error("共享探针的锚点变了，请同步 web-preview/fixtures/output/lib.mjs：" + anchor);
        out = out.replace(anchor, () => text);
    }
    return out;
}

export const OUT_CONTRAST_PROBE = derive(CONTRAST_PROBE, [
    [
        FX_CAP_ANCHOR,
        `${FX_CAP_ANCHOR}
        // 宿主旁路时内容按设计降到 .55 不透明度，等同被停用的界面：只记录不判。
        // 不在此列、照常分档的：常驻红字（在 layer 层）、消息位里成立的消息（旁路时不降级；空闲的灰字提示照设计一起降，仍是 C）
        if (p.closest("[data-bypass] .rn-shell__content") && !p.closest('.rn-out--band:not([data-state="idle"])')) return "C";`,
    ],
    [
        SHARED_ANCHOR,
        `if (p.closest(".rn-place")) return "A";
        // 「（未提供轨道名）」是灰色占位，按灰色状态文字的 B 档判；有名字的轨道名与副行是正文，判 A
        if (p.closest('.rn-track__name[data-noname="1"]')) return "B";
        if (p.closest(".rn-paircard__sub, .rn-track__name")) return "A";
        ${SHARED_ANCHOR}`,
    ],
    [
        MODAL_ANCHOR,
        `// 置灰的控件（首启规则页未放行时的两颗按钮、导览第 1 步的「上一步」）整体按 0.4 降级、本来就是「不可用」的画面：按 C 只记录，不判。
        // 只认真正的 :disabled / aria-disabled，不认视觉上的淡：可用的按钮被压淡照判（与 Input 的对比度冒烟同口径）
        // 范围只限首启的规则页与导览（置灰的控件只出现在这两处）：别处置灰的控件原来怎么判还怎么判，不因为这条口径悄悄少测
        if (p.closest('[data-gb$=".rules"], [data-gb$=".tour"]') && p.closest(":disabled, [aria-disabled='true']")) return "C";
        ${MODAL_ANCHOR}
        if (p.closest(".rn-tour__callout")) return "A"; // 导览说明框：整块是模态里的字，与模态同档`,
    ],
    [
        SCRIM_ON_ANCHOR,
        // 叠层（模态遮罩、首启导览的暗幕）打开时，下面的页面被压暗，不是要判的对象；只判叠层自己的文字
        'const scrimOn = [...stage.querySelectorAll(".rn-scrim, .rn-tour")].some(shown);',
    ],
    [SCRIM_SKIP_ANCHOR, 'if (scrimOn && !p.closest(".rn-scrim, .rn-tour")) continue;'],
    [
        ITEM_ANCHOR,
        // 在叠层里、在按钮里：首启冒烟据此断言「叠层打开时只量叠层自己的字」与「按钮的分档」
        `${ITEM_ANCHOR} inOverlay: Boolean(p.closest(".rn-scrim, .rn-tour")), inBtn: Boolean(p.closest("button")),`,
    ],
    [
        COLOR_ANCHOR,
        // SVG 的文字（规则页旁的插入链示意）画的是 fill（color 只是继承来的值，与画出来的颜色无关）
        'const c = parse(p.namespaceURI === "http://www.w3.org/2000/svg" ? getComputedStyle(p).fill : getComputedStyle(p).color);',
    ],
    [
        RECT_ANCHOR,
        `// SVG 文字（插入链示意，字号 7.5 到 8.5）的字框是整行的行盒，四周各有一圈没有墨迹的留白，
        // 留白的四角会落到圆角槽的外面；只取中间的墨迹区（四周各收 1.5 px），免得把槽外的底量成字的底
        const k = p.namespaceURI === "http://www.w3.org/2000/svg" ? 1.5 : 0;
        // 文字在祖先的滚动区里（规则页正文）：只取滚动区露出来的那一段，函数与 Input 的量尺是同一份源码（理由见该函数）
        ${clipToAncestorScroller.toString()}
        const box = clipToAncestorScroller({ left, top, right, bottom }, scrollerOf(p));
        return { x: box.left + k, y: box.top + k, w: box.right - box.left - 2 * k, h: box.bottom - box.top - 2 * k };`,
    ],
]);

/** Output 的适配量尺：共享量尺之上，把首启导览的说明框与模态一样当作浮在内容上面的一层（压住下面的文字是设计，只比同一层）。 */
export const OUT_FIT_PROBE = derive(FIT_PROBE, [[LAYER_ANCHOR, 'e.closest(".rn-scrim, .rn-tour") ? "modal"']]);

const dropHideText = (page) =>
    page.eval('(() => { const s = document.getElementById("fx-hide-text"); if (s) s.remove(); })()');

/**
 * 页内量尺的完整一遍（与 shared-measure 的 measureContrastOnPage 同做法：采样两遍确认静止 → 文字透明 → 整页截图量最差点 →
 * 核对截图期间没挪位 → 滚动区里的文字逐段滚进视野单独截图），区别只是用 Output 的分档。
 */
export async function measureOutputContrast(page, probe = OUT_CONTRAST_PROBE) {
    let items;
    let png;
    let last = "";
    for (let attempt = 0; attempt < 5; attempt++) {
        await dropHideText(page);
        const first = await page.eval(probe);
        await page.eval("new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)))");
        items = await page.eval(probe);
        last = itemsDrift(first, items);
        if (!last) {
            await page.eval(
                `(() => { const s = document.createElement("style"); s.id = "fx-hide-text"; s.textContent = ${JSON.stringify(HIDE_TEXT_STYLE)}; document.head.append(s); })()`,
            );
            await page.eval("new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)))");
            png = decodePng(await page.screenshot());
            last = rectsDrift(items, await page.eval("window.__cpRects()"));
            await dropHideText(page);
            if (!last) break;
        }
        await settleScene(page);
    }
    if (last) throw new Error(`页面量不到静止画面：${last}`);
    const out = [];
    if (items.some((it) => it.scrolled)) {
        await page.eval(
            `(() => { const s = document.createElement("style"); s.id = "fx-hide-text"; s.textContent = ${JSON.stringify(HIDE_TEXT_STYLE)}; document.head.append(s); })()`,
        );
    }
    for (const it of items) {
        if (!it.scrolled) {
            if (it.r.w < 1 || it.r.h < 1) continue;
            out.push({ ...it, worst: worstContrast(png, it) });
            continue;
        }
        // 关于面板正文在滚动区里：逐段滚进视野，只截那一小块再量（此时页内文字仍是透明的）
        const r = await page.eval(`window.__cpPlace(${it.idx})`);
        if (!r || r.w < 1 || r.h < 1) continue;
        const cx = Math.max(0, Math.floor(r.x));
        const cy = Math.max(0, Math.floor(r.y));
        const cw = Math.max(1, Math.ceil(r.x + r.w) - cx);
        const ch = Math.max(1, Math.ceil(r.y + r.h) - cy);
        const shot = await page.send("Page.captureScreenshot", {
            format: "png",
            clip: { x: cx, y: cy, width: cw, height: ch, scale: 1 },
        });
        const small = decodePng(Buffer.from(shot.data, "base64"));
        out.push({ ...it, r, worst: worstContrast(small, { ...it, r: { x: r.x - cx, y: r.y - cy, w: r.w, h: r.h } }) });
    }
    await dropHideText(page);
    return out;
}

// ---------------------------------------------------------------- 键盘与名称

const KEY = {
    Tab: { key: "Tab", code: "Tab", vk: 9 },
    Escape: { key: "Escape", code: "Escape", vk: 27 },
    ArrowRight: { key: "ArrowRight", code: "ArrowRight", vk: 39 },
    ArrowLeft: { key: "ArrowLeft", code: "ArrowLeft", vk: 37 },
    ArrowDown: { key: "ArrowDown", code: "ArrowDown", vk: 40 },
    ArrowUp: { key: "ArrowUp", code: "ArrowUp", vk: 38 },
    Space: { key: " ", code: "Space", vk: 32, text: " " },
    Enter: { key: "Enter", code: "Enter", vk: 13, text: "\r" },
};

/** 发一次真实按键（keyDown + keyUp），再等 40 ms 让页面处理完。 */
export async function press(page, name) {
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

/** 无头页默认没有焦点，键盘事件会落空；强制页面处于有焦点状态。每张要按键的页开页后调一次。 */
export const enableFocus = (page) => page.send("Emulation.setFocusEmulationEnabled", { enabled: true });

/** 页内名称审计：返回 { interactive:N, missing:[描述] }。简化版可访问名称：labelledby > aria-label > 文字 > title。 */
export const NAME_AUDIT = `(() => {
    const stage = document.getElementById("stage");
    const shown = (e) => {
        for (let n = e; n && n !== document.documentElement; n = n.parentElement) {
            if (n.hidden) return false;
            const cs = getComputedStyle(n);
            if (cs.display === "none" || cs.visibility === "hidden") return false;
        }
        return true;
    };
    // 名称的取法写在 fixtures/first-run/measure.mjs 的 accessibleName，叠层按钮的名称（overlayA11yProbe）用同一个函数
    ${accessibleName.toString()}
    const nameOf = (e) => accessibleName(e, document);
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

/**
 * Tab 走一圈（只按可 Tab 到的件数），返回每一站的 { id, ring, part, action, overlay }（overlay：这一站是否在首启叠层里）。ring 只认 outline：焦点环的实现就是 outline；
 * part、action 是节点的 data-part 与 data-action（没有就是空串），用来认出具体是哪个件，不靠 id（类名）猜。
 */
export async function tabWalk(page) {
    // 顺序焦点导航的起点是上一次聚焦的节点，blur 不会重置它：首启叠层打开时焦点在叠层里，Tab 从那里往后走，
    // 叠层在文档末尾，走出文档就停了，排在叠层之前的内容层停靠点走不到；
    // 给 body 临时一个 tabindex 把焦点放上去，起点就回到文档开头，Tab 一圈才是完整的一圈
    await page.eval(
        'document.activeElement && document.activeElement !== document.body && document.activeElement.blur(); document.body.tabIndex = -1; document.body.focus(); document.body.removeAttribute("tabindex")',
    );
    const tabbable = await page.eval(
        `[...document.getElementById("stage").querySelectorAll('button, select, input, a[href], [tabindex="0"]')].filter((e) => !e.disabled && !e.hidden && !e.closest("[inert]") && e.getClientRects().length > 0).length`,
    );
    const stops = [];
    for (let i = 0; i < tabbable; i++) {
        await press(page, "Tab");
        const f = await page.eval(
            `(() => { const a = document.activeElement; if (!a || a === document.body) return null; const cs = getComputedStyle(a); const ring = cs.outlineStyle !== "none" && parseFloat(cs.outlineWidth) > 0; return { id: (a.getAttribute("data-gb") || a.getAttribute("data-group") || a.className || a.tagName) + "", ring, part: a.getAttribute("data-part") || "", action: a.getAttribute("data-action") || "", overlay: Boolean(a.closest('[data-gb="output.langStart"], [data-gb="output.rules"], [data-gb="output.tour"]')) }; })()`,
        );
        if (!f) break;
        stops.push(f);
    }
    return stops;
}

/** 当前焦点件的简述。 */
export const focusOf = (page) =>
    page.eval(
        `(() => { const a = document.activeElement; return a ? { gb: a.getAttribute("data-gb") || "", part: a.getAttribute("data-part") || "", cls: String(a.className || ""), tag: a.tagName, group: a.getAttribute("data-group") || "", lang: a.getAttribute("data-lang") || "" } : null; })()`,
    );
