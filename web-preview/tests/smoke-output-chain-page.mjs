// SPDX-License-Identifier: GPL-3.0-or-later
//
// Output 信号链路版式的页面冒烟（无头浏览器）：真页面（夹具 web-preview/fixtures/output）× 中 / 英 / 法。
// 配对卡画成 Input 节点 — 连线 — 本轨节点，连线颜色与虚实表示配对状态，全窗沿中轴对称。量这些事：
//   ① 三段结构：两个节点各 140 宽、Input 在左、本轨在右（DOM 顺序仍是本轨在前）、左右留白相等；连线正好填满两个节点之间的空档；
//      声道徽标骑在线上、核验徽标在线下，都在两个节点之间且水平居中；未找到 Input 时左节点放插入链示意、加宽，连线从它的右缘起；
//   ② 连线的颜色与虚实跟配对状态走（15 个状态逐个量，颜色分组取自契约的 PAIR_COLORS）：绿实线、琥珀实线、红虚线、灰虚线；
//      同一页切状态后立刻跟着变（不是只在首帧画对）；
//   ③ 组胶囊不随「自动」徽标出现与消失而左右移动：徽标开 / 关两种情形下胶囊组的矩形逐像素相同，且在窗口正中；
//   ④ 发送行无数据时不再只画一个「—」：没接入环路的七个状态都写「未接入环路」一类的话（三语），发送行压在配对卡底部、居中；
//   ⑤ 消息位 42 高（提示关掉后 44），按钮横排：确认条的两个按钮、「+n」与动作按钮都在同一排；
//   ⑥ 字体分工：位置提示、组、配对卡的文字标签用 Rinne Sans，Mono 只留给发送行、版本、缩放、声道徽标、插入链与页眉小字。
// 末尾是坏样例：量尺必须抓得出（连线颜色错、徽标挪位、发送行只剩破折号、消息位高度不对、标签错用 Mono）。
// 浏览器缺失时整套跳过（退出码 2）。
//
// 用法：node web-preview/tests/smoke-output-chain-page.mjs [仓库根]

import path from "node:path";
import { pathToFileURL } from "node:url";
import { createRun, repoRoot } from "./lib/harness.mjs";
import { findBrowser, launchBrowser } from "./lib/browser.mjs";
import { startStaticServer } from "./lib/static-server.mjs";
import { LANGS, loadDesign, openOutput, setModel } from "../fixtures/output/lib.mjs";

const ROOT = repoRoot();
const run = createRun("Output 信号链路版式");
const load = (rel) => import(pathToFileURL(path.join(ROOT, rel)).href);
const OUT_DICT = (await load("web/i18n/output.js")).default;
const { PAIR_STATES, PAIR_COLORS } = await load("web-preview/mock/contract.js");
if (!findBrowser()) run.skip("没有 Chromium 系浏览器");

const box = await loadDesign(ROOT);
const server = await startStaticServer(ROOT);
const browser = await launchBrowser();

/** 颜色分组 → 连线的期望：取色的 token 与线型。绿、琥珀实线；红、灰虚线。 */
const LINE_WANT = {
    green: { token: "--sem-green", style: "solid" },
    amber: { token: "--sem-amber", style: "solid" },
    red: { token: "--sem-red", style: "dashed" },
    gray: { token: "--sem-gray", style: "dashed" },
};
/** 没接入环路的七个状态（发送行写「未接入环路」）。 */
const NOT_IN_LOOP = ["searching", "conflict", "unassigned", "ipcUnavailable", "abiMismatch", "srMismatch", "validator"];
const TRACKS = '[data-part="pair.tracks"]';
const CARD = '[data-gb="output.pair"]';
const SEND = '[data-gb="output.send"]';
const BAND = '[data-gb="output.band"]';
const PLACE_OFF = "&placeHidden=1";

// 判据写成纯函数，正例与坏样例调同一个：判据删掉或写错，坏样例就跟着变红
/** 连线的线型与颜色是否等于期望（want 来自 LINE_WANT，rgb 是 token 的数值）。 */
const lineOk = (ln, want, rgb) => ln.style === want.style && JSON.stringify(ln.rgb) === JSON.stringify(rgb);
/** 发送行是否写的是空态词条（文字与 data-state 都要对）。 */
const sendNoneOk = (got, noneText) => got.text === noneText && got.st === "none";
/** 消息位高度是否等于期望。 */
const bandOk = (h, want) => h === want;
/** 两个徽标的容器是否正好跨在两个节点之间的空档上（左右各差不超过 1 px）。 */
const spansGap = (end, input, self) => Math.abs(end.x - input.r) <= 1 && Math.abs(end.r - self.x) <= 1;

/** 没匹配到元素时的占位（hidden），省得对 null 取字段。 */
const GONE = Object.freeze({ x: 0, y: 0, w: 0, h: 0, r: 0, b: 0, hidden: true });
const rectOr = async (page, sel) => (await rectOf(page, sel)) || GONE;

const rectOf = (page, sel) =>
    page.eval(
        `(() => { const e = document.querySelector(${JSON.stringify(sel)}); if (!e) return null; const b = e.getBoundingClientRect(); return { x: b.left, y: b.top, w: b.width, h: b.height, r: b.right, b: b.bottom, hidden: e.hidden || b.width === 0 || b.height === 0 }; })()`,
    );

/** 连线（.rn-paircard__tracks 的 ::before）的计算样式与它在页面里的横向范围、竖向位置。 */
const lineOf = (page) =>
    page.eval(`(() => {
        const t = document.querySelector(${JSON.stringify(TRACKS)});
        const tr = t.getBoundingClientRect();
        const cs = getComputedStyle(t, "::before");
        const m = /rgba?\\(([^)]*)\\)/.exec(cs.borderTopColor);
        const rgb = m ? m[1].split(",").map((x) => Number(x.trim())) : [];
        const left = tr.left + parseFloat(cs.left);
        return { style: cs.borderTopStyle, width: parseFloat(cs.borderTopWidth), rgb: rgb.slice(0, 3), alpha: rgb.length > 3 ? rgb[3] : 1,
                 left, right: left + parseFloat(cs.width), y: tr.top + parseFloat(cs.top) };
    })()`);

/** 某个 token（r, g, b 三元）的数值。 */
const tokenRgb = (page, token) =>
    page.eval(
        `getComputedStyle(document.documentElement).getPropertyValue(${JSON.stringify(token)}).split(",").map((x) => Number(x.trim()))`,
    );

/** 计算样式里第一个字体族（去引号）。 */
const familyOf = (page, sel) =>
    page.eval(
        `(() => { const e = document.querySelector(${JSON.stringify(sel)}); if (!e) return null; return getComputedStyle(e).fontFamily.split(",")[0].replace(/["']/g, "").trim(); })()`,
    );

/** 页面的快照里让「自动」徽标出现 / 消失：groupOrigin 是模型字段。 */
const setOrigin = (page, origin) =>
    page.eval(`window.__fx.mutate((m) => { m.groupOrigin = ${JSON.stringify(origin)}; })`);

try {
    const page = await browser.newPage();
    for (const lang of LANGS) {
        const tag = lang;

        // ---- ① 三段结构 ----
        await openOutput(page, server, box, lang, "out-connected");
        {
            const card = await rectOf(page, CARD);
            const input = await rectOf(page, '[data-part="track.input"]');
            const self = await rectOf(page, '[data-part="track.this"]');
            const ln = await lineOf(page);
            run.check(!input.hidden && !self.hidden, `${tag}：两个节点都应上屏`);
            run.check(
                input.r < self.x - 100,
                `${tag}：Input 的节点在左、本轨的节点在右，中间有空档（${input.r} / ${self.x}）`,
            );
            run.eq([input.w, self.w], [140, 140], `${tag}：已连接时两个节点各 140 宽`);
            run.check(
                Math.abs(input.x - card.x - (card.r - self.r)) <= 1,
                `${tag}：两个节点沿卡的中轴对称（左留白 ${input.x - card.x}，右留白 ${card.r - self.r}）`,
            );
            run.eq(
                await page.eval(
                    `document.querySelector(${JSON.stringify(TRACKS)}).firstElementChild.getAttribute("data-part")`,
                ),
                "track.this",
                `${tag}：DOM 顺序仍是本轨在前（读屏顺序不变，左右换位只在样式里）`,
            );
            run.check(
                Math.abs(ln.left - input.r) <= 1 && Math.abs(ln.right - self.x) <= 1,
                `${tag}：连线应正好填满两个节点之间的空档（线 ${ln.left}–${ln.right}，空档 ${input.r}–${self.x}）`,
            );
            const nodeMidY = (input.y + input.b) / 2;
            run.check(
                ln.y > input.y && ln.y < input.b && Math.abs(ln.y - nodeMidY) <= 6,
                `${tag}：连线在节点的竖向中部附近（线 y=${ln.y}，节点 ${input.y}–${input.b}）`,
            );
            const ch = await rectOr(page, `${TRACKS} .rn-ch:not([hidden])`);
            const verify = await rectOr(page, `${TRACKS} .rn-verify:not([hidden])`);
            const midX = (input.r + self.x) / 2;
            run.check(!ch.hidden && !verify.hidden, `${tag}：已连接时声道徽标与核验徽标都应上屏`);
            run.check(
                Math.abs((ch.y + ch.b) / 2 - ln.y) <= 2,
                `${tag}：声道徽标骑在线上（徽标中心 ${(ch.y + ch.b) / 2}，线 ${ln.y}）`,
            );
            run.check(verify.y >= ln.y + 3, `${tag}：核验徽标在线下（徽标顶 ${verify.y}，线 ${ln.y}）`);
            run.check(
                Math.abs((ch.x + ch.r) / 2 - midX) <= 1 && Math.abs((verify.x + verify.r) / 2 - midX) <= 1,
                `${tag}：两个徽标水平居中在两个节点之间`,
            );
            run.check(
                ch.x >= input.r - 0.5 && ch.r <= self.x + 0.5 && verify.x >= input.r - 0.5 && verify.r <= self.x + 0.5,
                `${tag}：两个徽标都落在两个节点之间的空档里，不压节点`,
            );
            run.check(
                verify.b <= Math.min(input.b, self.b) + 0.5,
                `${tag}：核验徽标不越出节点的下沿（${verify.b} / ${input.b}）`,
            );
        }
        // 只有核验徽标、没有声道徽标（对端声道数未知）时，核验徽标仍在线下，不压到线上
        await setModel(page, { state: "connected", verify: "match", peer: { channels: null } });
        {
            const ln = await lineOf(page);
            const ch = await rectOr(page, `${TRACKS} .rn-ch:not([hidden])`);
            const verify = await rectOr(page, `${TRACKS} .rn-verify:not([hidden])`);
            run.check(ch === null || ch.hidden, `${tag}：对端声道数未知时不画声道徽标`);
            run.check(verify && !verify.hidden && verify.y >= ln.y + 3, `${tag}：只有核验徽标时它仍在线下`);
        }
        // 未找到 Input：左节点是插入链示意，加宽，连线从它的右缘起
        await openOutput(page, server, box, lang, "out-searching");
        {
            const input = await rectOf(page, '[data-part="track.input"]');
            const self = await rectOf(page, '[data-part="track.this"]');
            const ln = await lineOf(page);
            run.eq(input.w, 156, `${tag}/未找到：放插入链示意的左节点加宽到 156`);
            run.check(
                Math.abs(ln.left - input.r) <= 1 && Math.abs(ln.right - self.x) <= 1,
                `${tag}/未找到：连线从加宽后的左节点右缘起（线 ${ln.left}–${ln.right}，空档 ${input.r}–${self.x}）`,
            );
            // 徽标容器与连线读同一组宽度变量：这个状态下徽标不显示，但容器也要跟着让开，以后显示时才居中
            const ends = await page.eval(
                `[...document.querySelectorAll(${JSON.stringify(TRACKS + " .rn-track__end")})].map((e) => { const b = e.getBoundingClientRect(); return { x: b.left, r: b.right }; })`,
            );
            run.check(
                ends.length > 0 && ends.every((e) => spansGap(e, input, self)),
                `${tag}/未找到：徽标容器跨在加宽后的空档上（${JSON.stringify(ends)}，空档 ${input.r}–${self.x}）`,
            );
            const chain = await rectOf(page, `[data-part="track.input"] .rn-track__chain`);
            run.check(
                chain &&
                    !chain.hidden &&
                    chain.x >= input.x &&
                    chain.r <= input.r &&
                    chain.y >= input.y &&
                    chain.b <= input.b,
                `${tag}/未找到：插入链示意完整地画在左节点里`,
            );
        }

        // ---- ② 连线颜色与虚实跟状态走 ----
        await openOutput(page, server, box, lang, "out-connected");
        const seen = new Map();
        for (const state of PAIR_STATES) {
            await setModel(page, { state });
            run.eq(
                await page.eval(`document.querySelector(${JSON.stringify(CARD)}).getAttribute("data-state")`),
                state,
                `${tag}/${state}：配对卡应带这个状态`,
            );
            const want = LINE_WANT[PAIR_COLORS[state]];
            const ln = await lineOf(page);
            const rgb = await tokenRgb(page, want.token);
            run.check(
                lineOk(ln, want, rgb),
                `${tag}/${state}（${PAIR_COLORS[state]}）：连线应是 ${want.style} ${want.token}，实得 ${ln.style} ${ln.rgb}`,
            );
            run.check(
                ln.alpha >= 0.7 && ln.width >= 1,
                `${tag}/${state}：连线应看得见（透明度 ${ln.alpha}，线宽 ${ln.width}）`,
            );
            seen.set(PAIR_COLORS[state], `${ln.style}/${ln.rgb.join(",")}`);
        }
        run.eq(seen.size, 4, `${tag}：四组状态（绿、琥珀、红、灰）的连线两两不同，实得 ${JSON.stringify([...seen])}`);
        run.eq(new Set(seen.values()).size, 4, `${tag}：四组连线的（线型, 颜色）两两不同`);

        // ---- ③ 组胶囊不随「自动」徽标移动 ----
        for (const hint of ["", PLACE_OFF]) {
            const where = `${tag}${hint ? "/提示关着" : ""}`;
            await openOutput(page, server, box, lang, "out-connected", hint);
            const bars = [];
            for (const origin of ["auto", "manual", "auto"]) {
                await setOrigin(page, origin);
                const bar = await rectOf(page, '[data-gb="output.group"] [role="radiogroup"]');
                const badge = await rectOf(page, '[data-gb="output.group"] .rn-group__auto');
                run.eq(
                    badge === null ? true : badge.hidden,
                    origin === "manual",
                    `${where}/${origin}：「自动」徽标${origin === "manual" ? "应消失" : "应出现"}`,
                );
                bars.push(bar);
                if (origin === "auto") {
                    const label = await rectOf(page, '[data-gb="output.group"] .rn-group__label');
                    run.check(
                        label.r <= bar.x && badge.x >= bar.r,
                        `${where}：小标在胶囊组左、徽标在右（${label.r} / ${bar.x}–${bar.r} / ${badge.x}）`,
                    );
                    run.check(badge.r <= 442 + 0.5 && label.x >= 18 - 0.5, `${where}：小标与徽标都在 424 宽的组行里`);
                }
            }
            run.eq(
                [bars[1].x, bars[1].w, bars[1].y],
                [bars[0].x, bars[0].w, bars[0].y],
                `${where}：徽标消失后胶囊组不动（位置与宽度逐像素相同）`,
            );
            run.eq([bars[2].x, bars[2].w], [bars[0].x, bars[0].w], `${where}：徽标再出现后胶囊组仍不动`);
            // 窗口里所有件共用同一列（424 宽，左 18、右 16，沿用原版面），所以「正中」以这一列的中轴为准，用配对卡当尺
            const card = await rectOf(page, CARD);
            run.check(
                Math.abs((bars[0].x + bars[0].r) / 2 - (card.x + card.r) / 2) <= 0.5,
                `${where}：胶囊组在这一列的中轴上（中心 ${(bars[0].x + bars[0].r) / 2}，配对卡中心 ${(card.x + card.r) / 2}）`,
            );
        }

        // ---- ④ 发送行空态 ----
        const noneText = OUT_DICT[lang]["out.send.none"];
        run.check(
            noneText.trim().length >= 4 && noneText.trim() !== "—",
            `${tag}：词条 out.send.none 不能只是一个破折号，实得「${noneText}」`,
        );
        for (const hint of ["", PLACE_OFF]) {
            const where = `${tag}${hint ? "/提示关着" : ""}`;
            await openOutput(page, server, box, lang, "out-connected", hint);
            for (const state of NOT_IN_LOOP) {
                await setModel(page, { state });
                const got = await page.eval(
                    `(() => { const e = document.querySelector(${JSON.stringify(SEND)}); return { text: e.textContent.trim(), st: e.getAttribute("data-state") }; })()`,
                );
                run.check(
                    sendNoneOk(got, noneText),
                    `${where}/${state}：发送行应写空态文案，不是破折号，实得 ${JSON.stringify(got)}`,
                );
            }
            await setModel(page, { state: "connected" });
            const ok = await page.eval(`document.querySelector(${JSON.stringify(SEND)}).textContent`);
            run.check(/kHz/.test(ok) && !ok.includes(noneText), `${where}：发送中仍写声道与采样率，实得「${ok}」`);
            // 发送行压在配对卡底部、水平居中、在卡里
            const card = await rectOf(page, CARD);
            const send = await rectOf(page, SEND);
            const text = await page.eval(
                `(() => { const e = document.querySelector(${JSON.stringify(SEND)}); const r = document.createRange(); r.selectNodeContents(e); const b = r.getBoundingClientRect(); return { x: b.left, r: b.right }; })()`,
            );
            run.check(
                send.y >= card.y && send.b <= card.b && send.b >= card.b - 20,
                `${where}：发送行压在配对卡的底部（发送行 ${send.y}–${send.b}，卡 ${card.y}–${card.b}）`,
            );
            run.check(
                Math.abs((text.x + text.r) / 2 - box.w / 2) <= 1,
                `${where}：发送行文字水平居中（中心 ${(text.x + text.r) / 2}）`,
            );
            const sub = await rectOf(page, '[data-part="pair.sub"]');
            run.check(send.y >= sub.b - 0.5, `${where}：发送行不压副行（${send.y} / ${sub.b}）`);
        }

        // ---- ⑤ 消息位 42 / 44 高，按钮横排 ----
        for (const [hint, want] of [
            ["", 42],
            [PLACE_OFF, 44],
        ]) {
            const where = `${tag}${hint ? "/提示关着" : ""}`;
            await openOutput(page, server, box, lang, "out-connected", hint);
            const idle = await rectOf(page, BAND);
            run.check(bandOk(idle.h, want), `${where}：消息位高度（空闲）应是 ${want}，实得 ${idle.h}`);
            await setModel(page, { notices: [], state: "conflict" });
            const conflict = await rectOf(page, BAND);
            const foot = await rectOf(page, '[data-gb="output.footer"]');
            run.check(bandOk(conflict.h, want), `${where}：消息位高度（有消息）应是 ${want}，实得 ${conflict.h}`);
            run.check(conflict.b <= foot.y, `${where}：消息位不压页脚（${conflict.b} / ${foot.y}）`);
            // 「+n」与动作按钮同一排（冲突 + 两条灰 / 琥珀通知，冲突带「修复」按钮）
            await setModel(page, {
                notices: [{ id: "ipcSandbox" }, { id: "outOfOrder" }],
                state: "conflict",
            });
            const row = await page.eval(
                `(() => { const b = (s) => { const e = document.querySelector(${JSON.stringify(BAND + " ")} + s); if (!e) return null; const r = e.getBoundingClientRect(); return { x: r.left, y: r.top, h: r.height, r: r.right }; }; return { chip: b('[data-part="band.more"]'), act: b('[data-part="band.action"]') }; })()`,
            );
            run.check(row.chip && row.act, `${where}：应同时有「+n」与动作按钮，实得 ${JSON.stringify(row)}`);
            if (row.chip && row.act) {
                run.check(
                    Math.abs(row.chip.y + row.chip.h / 2 - (row.act.y + row.act.h / 2)) <= 1,
                    `${where}：「+n」与动作按钮在同一排（竖向中心 ${row.chip.y + row.chip.h / 2} / ${row.act.y + row.act.h / 2}）`,
                );
                run.check(
                    row.chip.r <= row.act.x || row.act.r <= row.chip.x,
                    `${where}：「+n」与动作按钮左右并排，不重叠`,
                );
            }
        }
        // 确认条的两个按钮横排
        await openOutput(page, server, box, lang, "out-takeover-confirm");
        {
            const cancel = await rectOf(page, `${BAND} [data-part="confirm.cancel"]`);
            const act = await rectOf(page, `${BAND} [data-part="band.action"]`);
            const band = await rectOf(page, BAND);
            run.check(cancel && act && !cancel.hidden && !act.hidden, `${tag}：确认条应有取消与接管两个按钮`);
            run.check(
                Math.abs(cancel.y + cancel.h / 2 - (act.y + act.h / 2)) <= 1 &&
                    (cancel.r <= act.x || act.r <= cancel.x),
                `${tag}：确认条的两个按钮横排在同一排`,
            );
            run.check(
                cancel.y >= band.y && cancel.b <= band.b && act.y >= band.y && act.b <= band.b,
                `${tag}：确认条的按钮都在消息位里`,
            );
            run.eq(band.h, 42, `${tag}：确认条的消息位高度`);
        }

        // ---- ⑥ 字体分工 ----
        await openOutput(page, server, box, lang, "out-connected");
        for (const [what, sel] of [
            ["位置提示文字", ".rn-place__text"],
            ["「知道了」按钮", ".rn-place__ok"],
            ["组小标", ".rn-group__label"],
            ["组胶囊的字母", ".rn-group__letter"],
            ["「自动」徽标", ".rn-group__auto"],
            ["「配对」小标", ".rn-paircard__eyebrow"],
            ["状态胶囊", ".rn-pair"],
            ["节点小标", '[data-part="track.this.label"]'],
            ["轨道名", '[data-part="track.this.name"]'],
            ["核验徽标", ".rn-verify"],
            ["副行", '[data-part="pair.sub"]'],
        ]) {
            run.eq(await familyOf(page, sel), "Rinne Sans", `${tag}：${what}应是 Rinne Sans`);
        }
        run.eq(await familyOf(page, ".rn-place__text"), "Rinne Sans", `${tag}：位置提示用 Rinne Sans`);
        run.eq(
            await page.eval(`getComputedStyle(document.querySelector(".rn-place__text")).fontWeight`),
            "600",
            `${tag}：位置提示字重 600`,
        );
        run.eq(
            await page.eval(`getComputedStyle(document.querySelector(".rn-place__text")).fontSize`),
            lang === "fr" ? "10.5px" : "11px",
            `${tag}：位置提示字号（法语这一句 10.5 px，其余 11 px）`,
        );
        run.eq(await familyOf(page, SEND), "Rinne Mono", `${tag}：发送行仍用 Rinne Mono`);
        // 页面上所有用 Mono 的文字节点都必须落在允许的几处：发送行、版本、缩放、声道徽标、插入链、页眉小字
        const monoOut = await page.eval(`(() => {
            const allowed = [".rn-send", '[data-gb="output.footer.version"]', '[data-gb="output.footer.scale"]', ".rn-ch", ".rn-chain", '[data-gb="output.header"]'];
            const bad = [];
            const w = document.createTreeWalker(document.querySelector(".rn-shell__content"), NodeFilter.SHOW_TEXT);
            for (let n = w.nextNode(); n; n = w.nextNode()) {
                if (!n.nodeValue.trim()) continue;
                const e = n.parentElement;
                const b = e.getBoundingClientRect();
                if (b.width === 0 || b.height === 0) continue;
                const fam = getComputedStyle(e).fontFamily.split(",")[0].replace(/["']/g, "").trim();
                if (fam === "Rinne Mono" && !allowed.some((s) => e.closest(s))) bad.push((e.className || e.tagName) + ":" + n.nodeValue.trim().slice(0, 16));
            }
            return bad;
        })()`);
        run.eq(monoOut, [], `${tag}：Mono 只留给发送行、版本、缩放、声道徽标、插入链与页眉小字`);
    }

    // ---- 坏样例：量尺必须抓得出 ----
    await openOutput(page, server, box, "zh", "out-connected");
    {
        // 连线改成灰虚线：状态是「已连接」，应抓出颜色与线型都不对
        await page.eval(
            `(() => { const s = document.createElement("style"); s.textContent = '.rn-paircard[data-state="connected"] .rn-paircard__tracks::before{border-top:1.6px dashed rgba(var(--sem-gray),.7) !important}'; document.head.append(s); })()`,
        );
        const ln = await lineOf(page);
        const green = await tokenRgb(page, "--sem-green");
        run.check(
            !lineOk(ln, LINE_WANT.green, green),
            `坏样例：连线被改成灰虚线，判据应判不对（线型 ${ln.style}，颜色 ${ln.rgb}）`,
        );
        // 只改颜色、线型仍是实线：判据要连颜色一起看
        await page.eval(
            `(() => { const s = document.createElement("style"); s.textContent = '.rn-paircard[data-state="connected"] .rn-paircard__tracks::before{border-top:1.6px solid rgba(var(--sem-amber),.9) !important}'; document.head.append(s); })()`,
        );
        const amberSolid = await lineOf(page);
        run.check(
            !lineOk(amberSolid, LINE_WANT.green, green),
            `坏样例：连线线型对、颜色错，判据应判不对（${amberSolid.style} ${amberSolid.rgb}）`,
        );
        // 颜色对、线型错（绿虚线）：判据要连线型一起看
        await page.eval(
            `(() => { const s = document.createElement("style"); s.textContent = '.rn-paircard[data-state="connected"] .rn-paircard__tracks::before{border-top:1.6px dashed rgba(var(--sem-green),.9) !important}'; document.head.append(s); })()`,
        );
        const greenDashed = await lineOf(page);
        run.check(
            !lineOk(greenDashed, LINE_WANT.green, green),
            `坏样例：连线颜色对、线型错，判据应判不对（${greenDashed.style} ${greenDashed.rgb}）`,
        );
    }
    await openOutput(page, server, box, "zh", "out-connected");
    {
        // 「自动」徽标出现时把它放回流里（旧版式：整行居中、胶囊组被徽标顶偏）
        await page.eval(
            `(() => { const s = document.createElement("style"); s.textContent = '.rn-out--group{display:flex !important;justify-content:center !important}'; document.head.append(s); })()`,
        );
        await setOrigin(page, "auto");
        const a = await rectOf(page, '[data-gb="output.group"] [role="radiogroup"]');
        await setOrigin(page, "manual");
        const b = await rectOf(page, '[data-gb="output.group"] [role="radiogroup"]');
        run.check(a.x !== b.x, `坏样例：徽标放回流里时胶囊组会被顶偏，量尺应看得出（${a.x} → ${b.x}）`);
    }
    await openOutput(page, server, box, "zh", "out-searching");
    {
        // 徽标容器没跟着左节点加宽（回到写死 140 的旧写法）：判据应判不对
        const inputW = await rectOf(page, '[data-part="track.input"]');
        const selfW = await rectOf(page, '[data-part="track.this"]');
        await page.eval(
            `(() => { const s = document.createElement("style"); s.id = "bad-end"; s.textContent = '.rn-track__end{left:140px !important}'; document.head.append(s); })()`,
        );
        const stuck = await page.eval(
            `[...document.querySelectorAll(${JSON.stringify(TRACKS + " .rn-track__end")})].map((e) => { const b = e.getBoundingClientRect(); return { x: b.left, r: b.right }; })`,
        );
        run.check(
            stuck.length > 0 && !stuck.every((e) => spansGap(e, inputW, selfW)),
            `坏样例：徽标容器停在 140，判据应判不对（${JSON.stringify(stuck)}，空档 ${inputW.r}–${selfW.x}）`,
        );
        await page.eval(`document.getElementById("bad-end").remove()`);
        // 发送行只剩破折号（data-state 仍是 none）：判据要看文字，不能只看状态
        const noneZh = OUT_DICT.zh["out.send.none"];
        const sendNow = `(() => { const e = document.querySelector(${JSON.stringify(SEND)}); return { text: e.textContent.trim(), st: e.getAttribute("data-state") }; })()`;
        run.check(sendNoneOk(await page.eval(sendNow), noneZh), "坏样例的前提：未找到时发送行本来写的是空态词条");
        await page.eval(`document.querySelector(${JSON.stringify(SEND)}).textContent = "—"`);
        const dash = await page.eval(sendNow);
        run.check(!sendNoneOk(dash, noneZh), `坏样例：发送行只剩破折号，判据应判不对（${JSON.stringify(dash)}）`);
        // 文字对、状态错：判据要连 data-state 一起看
        await page.eval(
            `(() => { const e = document.querySelector(${JSON.stringify(SEND)}); e.textContent = ${JSON.stringify(noneZh)}; e.setAttribute("data-state", "ok"); })()`,
        );
        const wrongState = await page.eval(sendNow);
        run.check(
            !sendNoneOk(wrongState, noneZh),
            `坏样例：发送行文字对、状态不是 none，判据应判不对（${JSON.stringify(wrongState)}）`,
        );
        run.check(bandOk((await rectOf(page, BAND)).h, 42), "坏样例的前提：提示开着时消息位本来是 42 高");
        await page.eval(
            `(() => { const s = document.createElement("style"); s.textContent = '.rn-msgline--block{height:58px !important}'; document.head.append(s); })()`,
        );
        const band = await rectOf(page, BAND);
        run.check(!bandOk(band.h, 42), `坏样例：消息位被改成 58 高，判据应判不对（${band.h}）`);
        await page.eval(
            `(() => { const s = document.createElement("style"); s.textContent = '.rn-paircard__eyebrow{font-family: var(--ff-mono) !important}'; document.head.append(s); })()`,
        );
        run.eq(await familyOf(page, ".rn-paircard__eyebrow"), "Rinne Mono", "坏样例：标签被改回 Mono，量尺应看得出");
    }

    run.check(page.errors.length === 0, `页面不应有错误：${page.errors.slice(0, 3).join("；")}`);
} finally {
    await browser.close();
    await server.close();
}
run.finish();
