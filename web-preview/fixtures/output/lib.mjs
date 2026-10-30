// SPDX-License-Identifier: GPL-3.0-or-later
//
// Output 页面级冒烟的公共件：场景表、开页（夹具 web-preview/fixtures/output/index.html）、Output 的对比度分档与量尺。
// 只放可复用的函数，不自己跑断言；smoke-output-*.mjs 各自引用。探针与 PNG 解码复用 shared-measure.mjs，不另写一份。

import { pathToFileURL } from "node:url";
import path from "node:path";
import {
    CONTRAST_PROBE,
    HIDE_TEXT_STYLE,
    decodePng,
    itemsDrift,
    rectsDrift,
    settleScene,
    worstContrast,
} from "../../tests/shared-measure.mjs";

export const LANGS = Object.freeze(["zh", "en", "fr"]);

/** 首启链（语言选择、使用规则、引导）不在本页：它们的场景由首启那一块接手，这里不摆。 */
const FIRST_LAUNCH = new Set(["out-lang", "out-rules", "out-tour-1", "out-tour-2", "out-tour-3", "out-tour-4"]);

/** 本页负责的全部场景（来自预览场景表，不另抄一份 ID）。 */
export async function loadOutputScenes(root) {
    const m = await import(pathToFileURL(path.join(root, "web-preview/mock/scenarios.js")).href);
    return m.SCENES.filter((s) => s.role === "output" && !FIRST_LAUNCH.has(s.id));
}

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
export const OUT_CONTRAST_PROBE = (() => {
    for (const a of [SHARED_ANCHOR, FX_CAP_ANCHOR])
        if (!CONTRAST_PROBE.includes(a))
            throw new Error("共享对比度探针的分档锚点变了，请同步 web-preview/fixtures/output/lib.mjs");
    return CONTRAST_PROBE.replace(
        FX_CAP_ANCHOR,
        `${FX_CAP_ANCHOR}
        // 宿主旁路时内容按设计降到 .55 不透明度，等同被停用的界面：只记录不判。
        // 不在此列、照常分档的：常驻红字（在 layer 层）、消息位里成立的消息（旁路时不降级；空闲的灰字提示照设计一起降，仍是 C）
        if (p.closest("[data-bypass] .rn-shell__content") && !p.closest('.rn-out--band:not([data-state="idle"])')) return "C";`,
    ).replace(
        SHARED_ANCHOR,
        `if (p.closest(".rn-place")) return "A";
        // 「（未提供轨道名）」是灰色占位，按灰色状态文字的 B 档判；有名字的轨道名与副行是正文，判 A
        if (p.closest('.rn-track__name[data-noname="1"]')) return "B";
        if (p.closest(".rn-paircard__sub, .rn-track__name")) return "A";
        ${SHARED_ANCHOR}`,
    );
})();

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
    const nameOf = (e) => {
        const by = e.getAttribute("aria-labelledby");
        if (by) {
            const txt = by.split(/\s+/).map((id) => (document.getElementById(id) || {}).textContent || "").join(" ").trim();
            if (txt) return txt;
        }
        const al = (e.getAttribute("aria-label") || "").trim();
        if (al) return al;
        const tc = (e.textContent || "").trim();
        if (tc && e.tagName !== "SELECT") return tc;
        return (e.getAttribute("title") || "").trim();
    };
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

/** Tab 走一圈（只按可 Tab 到的件数），返回每一站的 { id, ring }。ring 只认 outline：焦点环的实现就是 outline。 */
export async function tabWalk(page) {
    await page.eval("document.body.focus(); document.activeElement && document.activeElement.blur()");
    const tabbable = await page.eval(
        `[...document.getElementById("stage").querySelectorAll('button, select, input, a[href], [tabindex="0"]')].filter((e) => !e.disabled && !e.hidden && !e.closest("[inert]") && e.getClientRects().length > 0).length`,
    );
    const stops = [];
    for (let i = 0; i < tabbable; i++) {
        await press(page, "Tab");
        const f = await page.eval(
            `(() => { const a = document.activeElement; if (!a || a === document.body) return null; const cs = getComputedStyle(a); const ring = cs.outlineStyle !== "none" && parseFloat(cs.outlineWidth) > 0; return { id: (a.getAttribute("data-gb") || a.getAttribute("data-group") || a.className || a.tagName) + "", ring }; })()`,
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
