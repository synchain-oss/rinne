// SPDX-License-Identifier: GPL-3.0-or-later
//
// Input 页面适配冒烟（无头浏览器）：范围内的 40 个 in-* 场景（含首启的 8 个）× 中 / 英 / 法，真源页面经预览壳页装载。
// 每页量四件事：
//   ① 所有可见文字不越出所在的 data-gb 件、不越出外壳、不被 overflow 裁掉、互相不压住；data-gb 件不越出外壳；消息位不折行；
//   ② 页上确有足够多的可见文字（防止量了个空）；有消息的场景消息位里确有字；
//   ③ 界面上不得出现裸词条键、没替换的占位符、null / undefined（文字与 title、aria-label、aria-valuetext 都查；首启词条的 rules.、tour. 前缀也认）；
//   ④ 导出提示的四句原因话术（本页新增，text-budget 里还没有词条）逐语言逐句上屏量宽：不越出消息位、不被裁，
//      自然宽度不超过 300 px（消息位 452 px 里还要放图标、「+n」与 ✕）。
//   ⑥ 预设面板：场景表里只有列表与重名两态，其余子态逐语言逐子态经真实交互摆出来量，那一段在 smoke-input-fit-presets-page.mjs
//      （拆出去是为了让两个文件各自跑在 run-all 单项 300 秒的上限之内，判据一条没动）；量法同 ①。
// 另有端到端的坏样例：把一句超长的话塞进消息位、把词条键与占位符写进页面，必须被上面各项抓出来（量尺本身可信）。
// 首启的 8 个场景（语言卡、规则页、导览 6 步）按静态画面量，叠层压在页面上（语言卡、规则页是模态，导览说明框与模态同按「浮在内容上面的一层」算）；
// 规则页静态摆成「未放行」，另摆一个「放行后」（滚到底）的画面再量一遍；每个首启画面先确认叠层真的摆出来了（防空转）。
// 首启的坏样例：普通页面上没有叠层要报、第 N 步按第 M 步查要报、放行状态对不上要报、导览说明框里两段字互压要报、首启词条的裸键要扫出来。
// 预设面板的两个场景（in-preset-*）在范围内。
// 浏览器缺失时整套跳过（退出码 2），不是通过。本机同时只能有一个浏览器冒烟。
//
// 用法：node web-preview/tests/smoke-input-fit-page.mjs [仓库根]

import { createRun, repoRoot } from "./lib/harness.mjs";
import { findBrowser, launchBrowser } from "./lib/browser.mjs";
import { startStaticServer } from "./lib/static-server.mjs";
import {
    FRAMES,
    LANGS,
    SCENES,
    VIEW,
    confirmAlpha,
    findLeaks,
    fitProbe,
    frameTag,
    inFrame,
    isFirstRunScene,
    openFrame,
    openInput,
    overlayProbeFailures,
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
    let firstRunPages = 0;

    // ---- ① ② ③ 每个场景每种语言（规则页多一个放行后的画面）
    for (const lang of LANGS) {
        for (const frame of FRAMES) {
            const { scene } = frame;
            const tag = frameTag(frame, lang);
            await openFrame(page, server, frame, { lang });
            // 首启的画面：叠层确实摆出来了，量的才是叠层而不是空页面
            run.eq(await overlayProbeFailures(page, frame), [], `${tag}：首启叠层应摆出来`);
            if (isFirstRunScene(scene)) firstRunPages++;
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
    run.eq(firstRunPages, 9 * LANGS.length, "首启画面：8 个场景加规则页放行后一个，每种语言各量一遍");
    run.eq(
        FRAMES.length,
        SCENES.length + 1,
        "每个场景一个画面，规则页多一个放行后的画面（FRAMES 与场景表一致，防新增场景漏测）",
    );

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

    // ---- 首启的坏样例：量尺必须抓得出
    const frameOf = (id, sub = "") => ({ scene: SCENES.find((sc) => sc.id === id), sub });
    // 普通页面上没有叠层：叠层没摆出来必须报（量尺不能在没有叠层的页面上空转）
    await openInput(page, server, "in-playing", { lang: "zh" });
    run.check(
        (await overlayProbeFailures(page, frameOf("in-tour-2"))).length > 0,
        "坏样例：普通页面上没有叠层，应判「叠层没摆出来」",
    );
    run.eq(await overlayProbeFailures(page, frameOf("in-playing")), [], "非首启画面不查叠层");
    // 第 N 步的页面按第 M 步查：导览步号对不上必须报
    await openInput(page, server, "in-tour-3", { lang: "zh" });
    run.check(
        (await overlayProbeFailures(page, frameOf("in-tour-4"))).some((m) => /第 4 步/.test(m)),
        "坏样例：第 3 步的页面按第 4 步查，应报出导览步号不对",
    );
    // 未放行的规则页按「放行后」查：放行状态对不上必须报
    await openInput(page, server, "in-rules", { lang: "zh" });
    run.check(
        (await overlayProbeFailures(page, frameOf("in-rules", "ready"))).length > 0,
        "坏样例：未放行的规则页按「放行后」画面查，应报出放行状态不对",
    );
    // 导览说明框与模态一样是浮在内容上面的一层：压在页面上不算互压，说明框里两段字互压必须量出 overlap
    await openInput(page, server, "in-tour-2", { lang: "zh" });
    {
        const clean = await inFrame(page, fitProbe);
        run.eq(
            clean.problems.map((p) => p.kind),
            [],
            "前提：导览第 2 步压在页面上不算互压（独立一层）",
        );
        await inFrame(page, (doc) => {
            const t = doc.querySelector(".rn-tour__title").getBoundingClientRect();
            const b = doc.querySelector(".rn-tour__body");
            b.style.position = "relative";
            b.style.top = t.top - b.getBoundingClientRect().top + 2 + "px";
        });
        const bad = await inFrame(page, fitProbe);
        run.check(
            bad.problems.some((p) => p.kind === "overlap"),
            `坏样例：导览说明框里正文压到标题上，应量出 overlap，实得 ${JSON.stringify(bad.problems.map((p) => p.kind))}`,
        );
    }
    // 首启词条缺了会直接显示键本身：rules.、tour.、hint. 前缀的裸键要扫得出
    await openInput(page, server, "in-tour-2", { lang: "zh" });
    await inFrame(page, (doc) => {
        doc.querySelector(".rn-tour__title").textContent = "tour.in.step9.title";
    });
    {
        const probe = await inFrame(page, fitProbe);
        run.check(
            findLeaks(`${probe.joined} ${probe.attrs.join(" ")}`).includes("tour.in.step9.title"),
            "坏样例：导览标题显示成裸键 tour.in.step9.title，应被扫出",
        );
    }

    run.check(page.errors.length === 0, `页面不应有错误：${page.errors.slice(0, 3).join("；")}`);
    const foreign = requests.filter((u) => !/^(data|blob|about):/.test(u) && new URL(u).origin !== server.url);
    run.check(foreign.length === 0, `页面的请求都必须同源，外部请求：${JSON.stringify(foreign)}`);
} finally {
    await browser.close();
    await server.close();
}
run.finish();
