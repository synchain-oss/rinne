// SPDX-License-Identifier: GPL-3.0-or-later
//
// Output 页面的适配冒烟（无头浏览器）：真页面（夹具 web-preview/fixtures/output）× 中 / 英 / 法 × 全部场景（含首启的 6 个）与全部消息。
// 量四件事：
//   ① 所有可见文字不越出所在件、不越出外壳、不被 overflow 裁掉（含 line-clamp 截断）、互相不压住；data-gb 件不越出外壳；
//   ② 登记的「必须上屏」文字（红字、状态灯、副行、消息、页脚提示、发送行、轨道名）确实上屏，防止量了个空；
//   ③ 界面上没有裸词条键、没替换的占位符、null / undefined（文字与 title、aria-label 都查）；
//   ④ 行数：常驻红字 ≤ 2 行（设计盒里只留了两行的位置）、副行 ≤ 2 行、发送行与页脚提示 1 行、消息位文字 ≤ 3 行。
// 首启的 6 个场景（语言卡、规则页、导览 4 步）：夹具接首启控制器摆成静态画面，叠层压在整页上；叠层与下面的页面不是同一层（设计如此，只比同一层），
//   规则页另摆一个「放行后」（滚到底）的画面；每个首启画面先断言叠层真的摆出来了（种类、导览步号、规则页放行状态），再量上面四件事；
//   裸键扫描认 rules.、tour.、hint. 前缀（共用的那一份认不出）。
// 场景之外再造最坏组合：14 条消息逐条上屏（三语）、超长轨道名、多条消息同时在场（「+n」）、单声道与采样率最宽的写法。
// 末尾是坏样例：夹具的 ?defect=overflow / place3 / rawkey 必须被上面各项抓出来（量尺本身可信）；首启另有：
//   普通页面上没有叠层要报、第 N 步按第 M 步查要报、导览说明框里两段字互压要报、首启词条的裸键要扫出来、
//   首启画面带上危险态（配对冲突）或环路在流动（已连接）要报（夹具喂首启控制器的运行态写死为假，与真页面的判断分叉时要发现），
//   环路是否在流动按桥上收到的原帧求（只改原帧、不改页面快照，断言要跟着原帧走），
//   序号更小的旧帧不顶掉原帧（先断言补发的帧确实送到了，再用同一帧改成更大的序号对照：发出去之后原帧被顶掉，旧帧没翻是因为序号，不是因为没送到）。
// 浏览器缺失时整套跳过（退出码 2）。
//
// 用法：node web-preview/tests/smoke-output-fit-page.mjs [仓库根]

import { createRun, repoRoot } from "./lib/harness.mjs";
import { findBrowser, launchBrowser } from "./lib/browser.mjs";
import { startStaticServer } from "./lib/static-server.mjs";
import { EXPECTED_PROBE, missingExpected } from "./shared-measure.mjs";
import {
    LANGS,
    MESSAGE_CASES,
    FIRST_RUN_FEED_PROBE,
    OUT_FIT_PROBE,
    findLeaks,
    frameTag,
    isFirstRunScene,
    loadDesign,
    loadOutputScenes,
    openOutput,
    openOutputFrame,
    outputFrames,
    overlayFailures,
    setModel,
    lineCount,
    textOf,
} from "../fixtures/output/lib.mjs";

const ROOT = repoRoot();
const run = createRun("Output 页面适配");
if (!findBrowser()) run.skip("没有 Chromium 系浏览器");

const box = await loadDesign(ROOT);
const scenes = await loadOutputScenes(ROOT);
const frames = outputFrames(scenes);
const server = await startStaticServer(ROOT);
const browser = await launchBrowser();

/** 量一遍当前页：带标签地登记失败。返回探针结果。 */
async function measure(page, tag, allow = null) {
    const probe = await page.eval(OUT_FIT_PROBE);
    // allow：设计上就该被省略号截断的那几处（超长轨道名），量尺会把它们报成 clipped / outside，这里按名字放行，其余照报
    if (allow) probe.problems = probe.problems.filter((p) => !allow.test(p.detail));
    const expected = await page.eval(EXPECTED_PROBE);
    const bad = probe.problems.slice(0, 4).map((p) => `${p.kind}：${p.detail}`);
    run.check(probe.texts > 0, `${tag}：页上应有可见文字（防空转）`);
    run.check(probe.problems.length === 0, `${tag}：${bad.join("；")}${probe.problems.length > 4 ? "…" : ""}`);
    const lost = missingExpected(expected, probe);
    run.check(lost.length === 0, `${tag}：这些文字没上屏 ${JSON.stringify(lost.map((e) => e.text))}`);
    run.check(expected.length > 0, `${tag}：夹具应登记要上屏的文字（防空转）`);
    const leaks = findLeaks(`${probe.joined} ${probe.attrs.join(" ")}`);
    run.check(leaks.length === 0, `${tag}：界面上出现了裸键、占位符或 null：${JSON.stringify(leaks.slice(0, 5))}`);
    return probe;
}

/** 行数断言：红字 ≤ 2、副行 ≤ 2、发送行与页脚提示 1、消息位文字 ≤ 3。placeHidden：红字被用户关掉了，此时断言它确实不在，不量它的行数。 */
async function checkLines(page, tag, placeHidden = false) {
    if (placeHidden) {
        const shown = await page.eval(
            `(() => { const e = document.querySelector(".rn-place"); const b = e && e.getBoundingClientRect(); return Boolean(b && b.width > 0 && b.height > 0); })()`,
        );
        run.eq(shown, false, `${tag}：红字被关掉时不应上屏`);
    } else {
        const place = await lineCount(page, ".rn-place__text");
        run.check(place >= 1 && place <= 2, `${tag}：常驻红字应在 2 行以内，实得 ${place} 行`);
    }
    const sub = await lineCount(page, '[data-part="pair.sub"]');
    run.check(sub >= 1 && sub <= 2, `${tag}：配对卡副行应在 2 行以内，实得 ${sub} 行`);
    const send = await lineCount(page, '[data-gb="output.send"]');
    run.eq(send, 1, `${tag}：发送行应是 1 行`);
    const hint = await lineCount(page, '[data-gb="output.footer.hint"]');
    run.check(hint <= 1, `${tag}：页脚提示应是 1 行，实得 ${hint} 行`);
    const band = await lineCount(
        page,
        '[data-gb="output.band"] .rn-banner__text, [data-gb="output.band"] .rn-banner__idle',
    );
    run.check(band >= 1 && band <= 3, `${tag}：消息位文字应在 3 行以内，实得 ${band} 行`);
}

try {
    const page = await browser.newPage();
    const requests = await page.recordRequests();
    let pages = 0;
    let texts = 0;

    // ---- 全部场景 × 三语 ----
    let firstRunPages = 0;
    for (const lang of LANGS) {
        for (const frame of frames) {
            const tag = frameTag(lang, frame);
            await openOutputFrame(page, server, box, lang, frame);
            run.eq(await overlayFailures(page, frame), [], `${tag}：首启叠层应摆出来`);
            if (isFirstRunScene(frame.scene)) firstRunPages++;
            const probe = await measure(page, tag);
            pages++;
            texts += probe.texts;
            await checkLines(page, tag);
        }
    }

    // ---- 红字被用户关掉（全局位）：全部场景 × 三语再量一遍，版式上移之后各件仍不越界、不互压、没有裸键 ----
    let hiddenPages = 0;
    for (const lang of LANGS) {
        for (const frame of frames) {
            const tag = `${frameTag(lang, frame)}/红字关着`;
            await openOutputFrame(page, server, box, lang, frame, "&placeHidden=1");
            run.eq(await overlayFailures(page, frame), [], `${tag}：首启叠层应摆出来`);
            await measure(page, tag);
            await checkLines(page, tag, true);
            hiddenPages++;
        }
    }
    run.eq(hiddenPages, frames.length * LANGS.length, "红字关着：每个画面 × 每种语言都量到");

    // ---- 逐条消息 × 三语（同一张页反复改模型，省得重开） ----
    for (const lang of LANGS) {
        await openOutput(page, server, box, lang, "");
        for (const [id, [notices, state, peer]] of Object.entries(MESSAGE_CASES)) {
            const tag = `${lang}/消息 ${id}`;
            const group = state === "unassigned" ? null : "A";
            await setModel(page, { notices, state, peer: peer.version || peer.sampleRate ? peer : {}, group });
            run.check(
                (await page.eval(`document.querySelector('[data-gb="output.band"]').getAttribute("data-msg")`)) ===
                    `band.out.${id}`,
                `${tag}：消息位应显示这一条`,
            );
            const probe = await measure(page, tag);
            pages++;
            texts += probe.texts;
            await checkLines(page, tag);
        }

        // ---- 最坏组合 ----
        // 超长轨道名（宿主给的轨道名不受控）：两行都省略，不撑破配对卡
        const long = "Chœurs doublés – refrain 2 (bus stéréo) – prise alternative très longue 浓缩人声总线 final mix";
        await setModel(page, {
            state: "crossTrack",
            verify: "diff",
            peer: { name: long, channels: 1 },
            self: { name: long },
        });
        {
            const tag = `${lang}/超长轨道名`;
            const probe = await measure(page, tag, /track.(this|input).name|Chœurs doublés/);
            pages++;
            texts += probe.texts;
            await checkLines(page, tag);
            const cut = await page.eval(
                `[...document.querySelectorAll(".rn-track__name")].filter((e) => !e.hidden).map((e) => ({ ellipsis: getComputedStyle(e).textOverflow === "ellipsis", truncated: e.scrollWidth > e.clientWidth, inCard: e.getBoundingClientRect().right <= e.closest('[data-gb="output.pair"]').getBoundingClientRect().right }))`,
            );
            run.eq(
                cut,
                [1, 2].map(() => ({ ellipsis: true, truncated: true, inCard: true })),
                `${tag}：两行轨道名都应被省略号截断且不越出配对卡`,
            );
            // 省略号截断后真正看得见的那一段不能压住右边的核验徽标与声道徽标（量尺按整段文字比，这里按可见框比）
            const gap = await page.eval(
                `(() => { const row = document.querySelector('[data-part="track.input"]'); const n = row.querySelector(".rn-track__name").getBoundingClientRect(); const end = row.querySelector(".rn-track__end").getBoundingClientRect(); return { endShown: end.width > 0, gap: end.left - n.right }; })()`,
            );
            run.check(
                gap.endShown && gap.gap >= -0.5,
                `${tag}：轨道名的可见框不得压住右边的徽标，实得 ${JSON.stringify(gap)}`,
            );
            const titled = await page.eval(
                `[...document.querySelectorAll(".rn-track__name")].filter((e) => !e.hidden).map((e) => e.getAttribute("title") || "")`,
            );
            run.check(
                titled.length === 2 && titled.every((s) => s.length > 40),
                `${tag}：被省略的轨道名要在 title 里留全名，实得 ${JSON.stringify(titled.map((s) => s.length))}`,
            );
        }
        // 多条消息同时在场：「+n」chip、红色优先
        await setModel(page, {
            notices: [{ id: "ipcSandbox" }, { id: "newerProject", a: "0.11.0", b: "0.10.12" }, { id: "outOfOrder" }],
            state: "abiMismatch",
            verify: "diff",
            peer: { version: "0.10.12" },
        });
        {
            const tag = `${lang}/多条同时在场`;
            const probe = await measure(page, tag);
            pages++;
            texts += probe.texts;
            await checkLines(page, tag);
            run.eq(
                await textOf(page, '[data-gb="output.band"] [data-part="band.more"]'),
                "+4",
                `${tag}：五条消息显示「+4」`,
            );
        }
        // 单声道发送 + 44.1 kHz；对端立体声：声道徽标变成不同色的「Input→Output」
        await setModel(page, {
            state: "connected",
            send: { channels: "MONO", sampleRate: 44100 },
            peer: { channels: 2 },
        });
        {
            const tag = `${lang}/声道不同`;
            const probe = await measure(page, tag);
            pages++;
            texts += probe.texts;
            await checkLines(page, tag);
        }
    }
    run.log(`  共量 ${pages} 页、${texts} 段可见文字`);
    run.eq(firstRunPages, 7 * LANGS.length, "首启画面：6 个场景加规则页放行后一个，每种语言各量一遍");
    run.eq(frames.length, scenes.length + 1, "每个场景一个画面，规则页多一个放行后的画面（防新增场景漏测）");

    // ---- 坏样例：量尺必须抓得出 ----
    {
        await openOutput(page, server, box, "zh", "out-connected", "&defect=overflow");
        const probe = await page.eval(OUT_FIT_PROBE);
        const got = new Set(probe.problems.map((p) => p.kind));
        run.check(
            got.has("outside") || got.has("clipped"),
            `坏样例 overflow：应被量出 outside 或 clipped，实得 ${JSON.stringify([...got])}`,
        );
    }
    {
        await openOutput(page, server, box, "zh", "out-connected", "&defect=place3");
        const n = await lineCount(page, ".rn-place__text");
        run.check(n > 2, `坏样例 place3：红字撑到 3 行以上时行数应 > 2（行数量尺可信），实得 ${n}`);
        const probe = await page.eval(OUT_FIT_PROBE);
        run.check(
            probe.problems.length > 0,
            "坏样例 place3：红字撑出两行位置时，越界/裁切量尺也应报出来（红字不能被悄悄裁掉）",
        );
    }
    {
        await openOutput(page, server, box, "zh", "out-connected", "&defect=rawkey");
        const probe = await page.eval(OUT_FIT_PROBE);
        const leaks = findLeaks(`${probe.joined} ${probe.attrs.join(" ")}`);
        run.check(
            leaks.includes("out.neverDefined"),
            `坏样例 rawkey：应扫出 out.neverDefined，实得 ${JSON.stringify(leaks)}`,
        );
    }
    {
        // 登记的文字不上屏：把页脚提示改掉，必须被报缺
        await openOutput(page, server, box, "zh", "out-connected");
        await page.eval(`document.querySelector('[data-gb="output.footer.hint"]').textContent = "占位"`);
        const probe = await page.eval(OUT_FIT_PROBE);
        const expected = await page.eval(EXPECTED_PROBE);
        run.check(missingExpected(expected, probe).length > 0, "坏样例：改掉页脚提示后，登记的上屏文字应被报缺");
    }

    // ---- 首启的坏样例 ----
    const frameOf = (id, sub = "") => ({ scene: scenes.find((s) => s.id === id), sub });
    {
        // 普通页面上没有叠层：叠层没摆出来必须报（量尺不能在没有叠层的页面上空转）
        await openOutput(page, server, box, "zh", "out-connected");
        run.check(
            (await overlayFailures(page, frameOf("out-tour-2"))).length > 0,
            "坏样例：普通页面上没有叠层，应判「叠层没摆出来」",
        );
        run.eq(await overlayFailures(page, frameOf("out-connected")), [], "非首启画面不查叠层");
        // 第 N 步的页面按第 M 步查：导览步号对不上必须报
        await openOutput(page, server, box, "zh", "out-tour-3");
        run.check(
            (await overlayFailures(page, frameOf("out-tour-4"))).some((m) => /第 4 步/.test(m)),
            "坏样例：第 3 步的页面按第 4 步查，应报出导览步号不对",
        );
        // 未放行的规则页按「放行后」查：放行状态对不上必须报
        await openOutput(page, server, box, "zh", "out-rules");
        run.check(
            (await overlayFailures(page, frameOf("out-rules", "ready"))).length > 0,
            "坏样例：未放行的规则页按「放行后」画面查，应报出放行状态不对",
        );
        // 夹具喂首启控制器的运行态写死「无危险态、无环路活动」：真页面的判断（view.danger、outputFlowing）为真时必须报出来，
        // 夹具与真页面才不会悄悄分叉。配对冲突是危险态、已连接且没旁路是环路在流动，两项各摆一次，且互不牵连
        await openOutput(page, server, box, "zh", "out-tour-3");
        await setModel(page, { state: "conflict" });
        {
            const got = await overlayFailures(page, frameOf("out-tour-3"));
            run.check(
                got.some((m) => /危险态/.test(m)) && !got.some((m) => /环路/.test(m)),
                `坏样例：配对冲突的首启画面，应只报带危险态，实得 ${JSON.stringify(got)}`,
            );
        }
        await setModel(page, { state: "connected" });
        {
            const got = await overlayFailures(page, frameOf("out-tour-3"));
            run.check(
                got.some((m) => /环路/.test(m)) && !got.some((m) => /危险态/.test(m)),
                `坏样例：已连接的首启画面，应只报环路在流动，实得 ${JSON.stringify(got)}`,
            );
        }
        // 求环路是否在流动用的是桥上收到的原帧（和真页面一样），不是页面规范化之后的快照：只改其中一边，断言要跟着原帧走。
        // 每条都把「改原帧」和「求值」放进同一次页内求值：周期回声每 250 ms 会发一帧新帧顶掉原帧，分成两次求值会夹进回声。
        // 经 __fx.emit 补发帧的写法把返回值记进 emitted（夹具约定：帧送到了页面才返回 true，没送到返回 false），
        // 没发帧的写法 emitted 保持 null；补发帧的断言要同时看 emitted，不然「没送到」和「送到了但被序号拦下」分不开
        const feedAfter = (edit) =>
            page.eval(
                `(() => { let emitted = null; ${edit}; const f = ${FIRST_RUN_FEED_PROBE}; f.snapBypassed = window.__fx.page.snapshot().state.send.bypassed; f.emitted = emitted; return f; })()`,
            );
        const rawBypassed = (v) =>
            `const r = window.__fx.raw; r.state = { ...r.state, send: { ...r.state.send, bypassed: ${v} } };`;
        // 原帧说旁路了、页面快照仍是没旁路（已连接）：跟着原帧走，环路不在流动
        await setModel(page, { state: "connected", send: { bypassed: false } });
        {
            const f = await feedAfter(rawBypassed(true));
            run.eq(f.snapBypassed, false, "前提：只改了原帧，页面规范化之后的快照仍是没旁路");
            run.eq(f.flowing, false, "坏样例：原帧说旁路了，环路就不在流动（断言跟着原帧走，不是跟着页面快照走）");
        }
        // 反过来：页面快照是旁路、原帧说没旁路：跟着原帧走，环路在流动
        await setModel(page, { state: "connected", send: { bypassed: true } });
        {
            const f = await feedAfter(rawBypassed(false));
            run.eq(f.snapBypassed, true, "前提：只改了原帧，页面规范化之后的快照仍是旁路");
            run.eq(f.flowing, true, "坏样例：原帧说没旁路，环路就在流动（断言跟着原帧走，不是跟着页面快照走）");
        }
        // 乱序的旧帧不算（和真页面一样只认序号递增的帧）：补发一个序号更小、说旁路了的旧 rinne.state，原帧不该被它顶掉。
        // 「没顶掉」只有在帧确实送到了页面时才说明序号拦住了它：先断言 emit 返回 true；再用同一帧改成更大的序号对照，
        // 它发出去之后原帧被顶掉、环路不再流动，证明这条通道真能改变结果，旧帧没翻是因为序号，不是因为没送到
        const staleOrNewer = (delta) =>
            `const r = window.__fx.raw.state; emitted = window.__fx.emit("rinne.state", { ...r, seq: r.seq ${delta}, send: { ...r.send, bypassed: true } });`;
        await setModel(page, { state: "connected", send: { bypassed: false } });
        {
            const f = await feedAfter(staleOrNewer("- 5"));
            run.eq(f.emitted, true, "前提：序号更小的旧帧确实送到了页面（__fx.emit 返回 true），不是没发出去");
            run.eq(f.flowing, true, "坏样例：序号更小的旧帧（说旁路了）不该顶掉原帧，已连接的环路仍在流动");
        }
        await setModel(page, { state: "connected", send: { bypassed: false } });
        {
            const f = await feedAfter(staleOrNewer("+ 1"));
            run.eq(f.emitted, true, "对照：序号更大的同一帧也送到了页面（__fx.emit 返回 true）");
            run.eq(
                f.flowing,
                false,
                "对照：同一帧改用更大的序号，原帧被顶掉、说旁路了，环路不再流动（通道真能改变结果）",
            );
        }
    }
    {
        // 导览说明框与模态一样是浮在内容上面的一层：说明框里两段字互压必须量出 overlap
        await openOutput(page, server, box, "zh", "out-tour-2");
        const clean = await page.eval(OUT_FIT_PROBE);
        run.eq(
            clean.problems.map((p) => p.kind),
            [],
            "前提：导览第 2 步压在页面上不算互压（独立一层）",
        );
        await page.eval(
            `(() => { const t = document.querySelector(".rn-tour__title").getBoundingClientRect(); const b = document.querySelector(".rn-tour__body"); const r = b.getBoundingClientRect(); b.style.position = "relative"; b.style.top = (t.top - r.top + 2) + "px"; })()`,
        );
        const bad = await page.eval(OUT_FIT_PROBE);
        run.check(
            bad.problems.some((p) => p.kind === "overlap"),
            `坏样例：导览说明框里正文压到标题上，应量出 overlap，实得 ${JSON.stringify(bad.problems.map((p) => p.kind))}`,
        );
    }
    {
        // 首启词条缺了会直接显示键本身：rules.、tour.、hint. 前缀的裸键要扫得出
        await openOutput(page, server, box, "zh", "out-tour-2");
        await page.eval(`document.querySelector(".rn-tour__title").textContent = "tour.out.step9.title"`);
        const probe = await page.eval(OUT_FIT_PROBE);
        run.check(
            findLeaks(`${probe.joined} ${probe.attrs.join(" ")}`).includes("tour.out.step9.title"),
            "坏样例：导览标题显示成裸键 tour.out.step9.title，应被扫出",
        );
        run.eq(
            [findLeaks("规则 rules.r1.lead"), findLeaks("提示 hint.firstRun"), findLeaks("我已了解，开始导览")].map(
                (a) => a.length,
            ),
            [1, 1, 0],
            "rules.、hint. 前缀的裸键要扫出来，正常文案不误报",
        );
    }

    run.check(page.errors.length === 0, `页面不应有错误：${page.errors.slice(0, 3).join("；")}`);
    const foreign = requests.filter((u) => !/^(data|blob|about):/.test(u) && new URL(u).origin !== server.url);
    run.check(foreign.length === 0, `夹具页的请求都必须同源，外部请求：${JSON.stringify(foreign)}`);
} finally {
    await browser.close();
    await server.close();
}
run.finish();
