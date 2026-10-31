// SPDX-License-Identifier: GPL-3.0-or-later
//
// Input 页面适配冒烟（无头浏览器）：范围内的 32 个 in-* 场景 × 中 / 英 / 法，真源页面经预览壳页装载。
// 每页量四件事：
//   ① 所有可见文字不越出所在的 data-gb 件、不越出外壳、不被 overflow 裁掉、互相不压住；data-gb 件不越出外壳；消息位不折行；
//   ② 页上确有足够多的可见文字（防止量了个空）；有消息的场景消息位里确有字；
//   ③ 界面上不得出现裸词条键、没替换的占位符、null / undefined（文字与 title、aria-label、aria-valuetext 都查）；
//   ④ 导出提示的四句原因话术（本页新增，text-budget 里还没有词条）逐语言逐句上屏量宽：不越出消息位、不被裁，
//      自然宽度不超过 300 px（消息位 452 px 里还要放图标、「+n」与 ✕）。
//   ⑥ 预设面板：场景表里只有列表与重名两态，其余子态（长名当前、保存输入、名称为空、非法字符、保存失败、删除确认）
//      逐语言逐子态经真实交互摆出来，量法同 ①（面板是独立一层，带完整 title 的预设名用省略号截短不算被裁）。
// 另有端到端的坏样例：把一句超长的话塞进消息位、把词条键与占位符写进页面，必须被上面各项抓出来（量尺本身可信）。
// 首启（I-30 到 I-32）不在本页范围，场景表里的对应场景不测；预设面板的两个场景（in-preset-*）在范围内。
// 浏览器缺失时整套跳过（退出码 2），不是通过。本机同时只能有一个浏览器冒烟。
//
// 用法：node web-preview/tests/smoke-input-fit-page.mjs [仓库根]

import { createRun, repoRoot } from "./lib/harness.mjs";
import { findBrowser, launchBrowser } from "./lib/browser.mjs";
import { startStaticServer } from "./lib/static-server.mjs";
import {
    LANGS,
    PRESET_STATES,
    SCENES,
    VIEW,
    confirmAlpha,
    findLeaks,
    fitProbe,
    inFrame,
    openInput,
    openPresetState,
} from "./smoke-input-kit-page.mjs";

const ROOT = repoRoot();
const run = createRun("Input 页面适配");
if (!findBrowser()) run.skip("没有 Chromium 系浏览器");

/** 导出提示四句：原因位 → 词条键。 */
const EXPORT_CAUSES = [
    { bit: 1, key: "band.in.exportViolation" },
    { bit: 2, key: "band.in.exportOverrun" },
    { bit: 4, key: "band.in.exportPeer" },
    { bit: 8, key: "band.in.exportResync" },
];
const EXPORT_MAX_PX = 300;

/** 通知五条（rinne.state.notices）：id、词条键、色调，以及 newerProject 要带的两个版本号。 */
const NOTICES = [
    { id: "ipcSandbox", key: "band.in.ipcSandbox", tone: "red" },
    { id: "outOfOrder", key: "band.in.outOfOrder", tone: "amber" },
    { id: "unstable", key: "band.in.unstable", tone: "amber" },
    { id: "newerProject", key: "band.in.newerProject", tone: "amber", a: "0.11.0", b: "0.9.0" },
    { id: "stateDecodeFailed", key: "band.in.stateDecodeFailed", tone: "amber" },
];

const server = await startStaticServer(ROOT);
const browser = await launchBrowser({ width: VIEW.w, height: VIEW.h });
try {
    const page = await browser.newPage();
    const requests = await page.recordRequests();
    let pages = 0;
    let texts = 0;

    // ---- ① ② ③ 每个场景每种语言
    for (const lang of LANGS) {
        for (const scene of SCENES) {
            const tag = `${scene.id}/${lang}`;
            await openInput(page, server, scene.id, { lang });
            const probe = await inFrame(page, fitProbe);
            pages++;
            texts += probe.texts;
            run.check(probe.texts > 30, `${tag}：页上应有足够多的可见文字（防空转），实得 ${probe.texts}`);
            const bad = probe.problems.slice(0, 4).map((p) => `${p.kind}：${p.detail}`);
            run.check(probe.problems.length === 0, `${tag}：${bad.join("；")}${probe.problems.length > 4 ? "…" : ""}`);
            const leaks = findLeaks(`${probe.joined} ${probe.attrs.join(" ")}`);
            run.check(
                leaks.length === 0,
                `${tag}：界面上出现了裸键、占位符或 null：${JSON.stringify(leaks.slice(0, 5))}`,
            );
            const bandText = await inFrame(page, (doc) => {
                const e = doc.querySelector('[data-gb="input.band"] .rn-banner__text');
                return e ? e.textContent.trim() : "";
            });
            const wantsBand = Array.isArray(scene.state.band) && scene.state.band.length > 0;
            if (wantsBand) run.check(bandText.length > 0, `${tag}：场景带消息，消息位里应有字`);
            if (scene.id === "in-limiter-confirm")
                run.check((await inFrame(page, confirmAlpha)) >= 0.95, `${tag}：确认框应不透明（量尺把它当独立一层）`);
        }
    }
    run.log(`  共量 ${pages} 页、${texts} 段可见文字`);

    // ---- ⑥ 预设面板的子态
    let presetPages = 0;
    for (const lang of LANGS) {
        for (const state of PRESET_STATES) {
            const tag = `预设面板 ${state}/${lang}`;
            await openInput(page, server, "in-playing", { lang });
            await openPresetState(page, state);
            const probe = await inFrame(page, fitProbe);
            presetPages++;
            run.check(probe.texts > 30, `${tag}：页上应有足够多的可见文字（防空转），实得 ${probe.texts}`);
            const bad = probe.problems.slice(0, 4).map((p) => `${p.kind}：${p.detail}`);
            run.check(probe.problems.length === 0, `${tag}：${bad.join("；")}${probe.problems.length > 4 ? "…" : ""}`);
            const leaks = findLeaks(`${probe.joined} ${probe.attrs.join(" ")}`);
            run.check(
                leaks.length === 0,
                `${tag}：界面上出现了裸键、占位符或 null：${JSON.stringify(leaks.slice(0, 5))}`,
            );
        }
    }
    run.log(`  预设面板子态共量 ${presetPages} 页`);

    // ---- ④ 导出提示四句
    const widths = [];
    for (const lang of LANGS) {
        await openInput(page, server, "in-playing", { lang });
        for (const c of EXPORT_CAUSES) {
            const tag = `导出提示 ${c.key}/${lang}`;
            // 让桥只报这一个原因位；计数每次加一，免得被「已关掉」的记录挡住
            await page.eval(`(() => {
                const s = window.__RINNE_PREVIEW__;
                s.model.exportCauses = { bits: ${c.bit}, count: ${c.bit} + 100 };
                s.core.push("rinne.state");
            })()`);
            await page.waitFor(
                `(() => { const e = document.querySelector("iframe").contentDocument.querySelector('[data-gb="input.band"]'); return !!e && e.getAttribute("data-msg") === "${c.key}"; })()`,
                { label: `${tag}：消息位应显示这一句` },
            );
            const m = await inFrame(page, (doc, win) => {
                const band = doc.querySelector('[data-gb="input.band"]');
                const t = band.querySelector(".rn-banner__text");
                const cs = win.getComputedStyle(t);
                const g = doc.createElement("canvas").getContext("2d");
                g.font = `${cs.fontStyle} ${cs.fontWeight} ${cs.fontSize} ${cs.fontFamily}`;
                const br = band.getBoundingClientRect();
                const tr = t.getBoundingClientRect();
                return {
                    text: t.textContent.trim(),
                    natural: g.measureText(t.textContent.trim()).width,
                    inside: tr.left >= br.left - 1 && tr.right <= br.right + 1,
                    clipped: t.scrollWidth > t.clientWidth + 1,
                };
            });
            widths.push(`${c.key.slice(-9)}/${lang} ${m.natural.toFixed(0)}`);
            run.check(m.text.length > 0, `${tag}：消息位里应有字`);
            run.check(m.inside, `${tag}：文字不应越出消息位`);
            run.check(!m.clipped, `${tag}：文字不应被省略号裁掉（${m.text}）`);
            run.check(
                m.natural <= EXPORT_MAX_PX,
                `${tag}：自然宽度 ${m.natural.toFixed(1)} px 超过目标 ${EXPORT_MAX_PX} px（${m.text}）`,
            );
        }
    }

    run.log(`  导出提示自然宽度（px）：${widths.join("，")}`);

    // ---- ⑤ 通知五条：逐语言上屏，不越出、不被裁、占位符被替换成版本号
    const noticeWidths = [];
    for (const lang of LANGS) {
        await openInput(page, server, "in-playing", { lang });
        for (const n of NOTICES) {
            const tag = `通知 ${n.id}/${lang}`;
            await page.eval(`(() => {
                const s = window.__RINNE_PREVIEW__;
                s.model.notices = [${JSON.stringify({ id: n.id, a: n.a, b: n.b })}];
                s.core.push("rinne.state");
            })()`);
            await page.waitFor(
                `(() => { const e = document.querySelector("iframe").contentDocument.querySelector('[data-gb="input.band"]'); return !!e && e.getAttribute("data-msg") === "${n.key}"; })()`,
                { label: `${tag}：消息位应显示这一句` },
            );
            const m = await inFrame(page, (doc, win) => {
                const band = doc.querySelector('[data-gb="input.band"]');
                const t = band.querySelector(".rn-banner__text");
                const cs = win.getComputedStyle(t);
                const g = doc.createElement("canvas").getContext("2d");
                g.font = `${cs.fontStyle} ${cs.fontWeight} ${cs.fontSize} ${cs.fontFamily}`;
                const br = band.getBoundingClientRect();
                const tr = t.getBoundingClientRect();
                return {
                    text: t.textContent.trim(),
                    natural: g.measureText(t.textContent.trim()).width,
                    inside: tr.left >= br.left - 1 && tr.right <= br.right + 1,
                    clipped: t.scrollWidth > t.clientWidth + 1,
                    tone: band.getAttribute("data-tone"),
                };
            });
            noticeWidths.push(`${n.id.slice(0, 6)}/${lang} ${m.natural.toFixed(0)}`);
            run.check(m.text.length > 0, `${tag}：消息位里应有字`);
            run.check(m.tone === n.tone, `${tag}：色调应是 ${n.tone}，实得 ${m.tone}`);
            run.check(m.inside, `${tag}：文字不应越出消息位`);
            run.check(!m.clipped, `${tag}：文字不应被省略号裁掉（${m.text}）`);
            run.check(
                !/[{}]/.test(m.text) && !/null|undefined/.test(m.text),
                `${tag}：不应带占位符或 null（${m.text}）`,
            );
            if (n.a) {
                // 文案写作「本机 {a} / 工程 {b}」：通知里 a 是工程版本、b 是本机版本，对调后本机版本在前
                const iLocal = m.text.indexOf(n.b);
                const iProject = m.text.indexOf(n.a);
                run.check(
                    iLocal >= 0 && iProject >= 0 && iLocal < iProject,
                    `${tag}：应是本机版本 ${n.b} 在前、工程版本 ${n.a} 在后（${m.text}）`,
                );
            }
        }
    }
    run.log(`  通知自然宽度（px）：${noticeWidths.join("，")}`);
    // ---- 坏样例：量尺必须抓得出
    await openInput(page, server, "in-conflict", { lang: "fr" });
    await inFrame(page, (doc) => {
        const t = doc.querySelector('[data-gb="input.band"] .rn-banner__text');
        t.textContent =
            "Ceci est une phrase volontairement beaucoup trop longue pour tenir dans la ligne du message unique. ".repeat(
                3,
            );
        t.style.overflow = "visible";
        t.style.textOverflow = "clip";
    });
    {
        const probe = await inFrame(page, fitProbe);
        run.check(
            probe.problems.some((p) => p.kind === "outside"),
            `坏样例（消息位里一句超长的话）应量出 outside，实得 ${JSON.stringify(probe.problems.map((p) => p.kind))}`,
        );
    }
    for (const [label, bad, want] of [
        ["裸词条键", "in.peer.neverDefined", "in.peer.neverDefined"],
        ["占位符", "组 {g} 已有另一个 Input", "{g}"],
        ["null", "回送 null dB", "null"],
    ]) {
        await openInput(page, server, "in-playing", { lang: "zh" });
        await inFrame(
            page,
            (doc, win, text) => {
                doc.querySelector(".in-peer__name, [data-part='peer.name']").textContent = text;
            },
            bad,
        );
        const probe = await inFrame(page, fitProbe);
        const leaks = findLeaks(`${probe.joined} ${probe.attrs.join(" ")}`);
        run.check(leaks.includes(want), `坏样例（${label}）：应扫出 ${want}，实得 ${JSON.stringify(leaks)}`);
    }

    run.check(page.errors.length === 0, `页面不应有错误：${page.errors.slice(0, 3).join("；")}`);
    const foreign = requests.filter((u) => !/^(data|blob|about):/.test(u) && new URL(u).origin !== server.url);
    run.check(foreign.length === 0, `页面的请求都必须同源，外部请求：${JSON.stringify(foreign)}`);
} finally {
    await browser.close();
    await server.close();
}
run.finish();
