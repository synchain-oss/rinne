// SPDX-License-Identifier: GPL-3.0-or-later
//
// Output 页面的行为冒烟（无头浏览器）：真页面（夹具 web-preview/fixtures/output）接 mock 桥，逐个场景与交互点一遍。
//   ① 场景表：15 个场景各自的配对状态灯、色语义、副行、轨道行（名字 / 无名 / 插入链）、核验与声道徽标、消息位、发送行、
//      页脚提示、外壳状态与危险描边、组选择、覆盖层；
//   ② 三语：状态灯与核验徽标的文字取自词典，不是写死；<html lang> 跟界面语言；
//   ③ 交互：选组（同组不重复调用、成功后弹提示）、接管确认条的两个按钮与组号（读 takeoverGroup，缺省或不认识时读 group）、
//      重新配对、「查看说明」只发白名单里的地址、语言胶囊、缩放确认的保存与取消、关于面板（只有 tail 一个选项组、备注跟着
//      tail 行、没有起播清空）与复制诊断、「?」在首启链接入前不动作也不报错；
//   ④ 重绘纪律：回声每秒来好几次同样的状态（含乱序的旧帧），页面不写任何 DOM；乱序旧帧与未知配对状态整帧丢弃；
//   ⑤ 外壳：冲突与版本不匹配是危险描边、旁路是旁路态且只降内容层（常驻红字不降）、覆盖层打开时内容 inert、脉冲只在音频流动时；
//   ⑥ 桥不通：requestInitialState 一直失败时 400 ms 兜底揭幕、重试 3 次；无桥模式不报错、照常能选组；首帧慢：兜底揭幕后两路
//      首帧到齐前组胶囊、重新配对、接管确认与缩放都不写（缩放下拉置灰只显示「—」，语言照常），到齐后照常选组与选档；销毁后不留定时器、不再去问桥、pagehide 监听摘掉；
//   ⑦ 坏样例：把红字挪进内容层、把危险描边关掉，上面对应的断言必须变红（见各自的替身断言）。
// 浏览器缺失时整套跳过（退出码 2）。
//
// 用法：node web-preview/tests/smoke-output-scenarios-page.mjs [仓库根]

import { pathToFileURL } from "node:url";
import path from "node:path";
import { createRun, repoRoot } from "./lib/harness.mjs";
import { findBrowser, launchBrowser } from "./lib/browser.mjs";
import { startStaticServer } from "./lib/static-server.mjs";
import { awaitState } from "./lib/await-state.mjs";
import {
    LANGS,
    attr,
    callsOf,
    click,
    loadDesign,
    loadOutputScenes,
    openOutput,
    setModel,
    textOf,
} from "../fixtures/output/lib.mjs";

const ROOT = repoRoot();
const run = createRun("Output 页面行为");
if (!findBrowser()) run.skip("没有 Chromium 系浏览器");

const box = await loadDesign(ROOT);
const scenes = await loadOutputScenes(ROOT);
const load = (rel) => import(pathToFileURL(path.join(ROOT, rel)).href);
const common = (await load("web/i18n/common.js")).default;
const outDict = (await load("web/i18n/output.js")).default;
const server = await startStaticServer(ROOT);
const browser = await launchBrowser();

const sleep = (page, ms) => page.eval(`new Promise((r) => setTimeout(r, ${ms}))`);
const gb = (part) => `[data-gb="output.${part}"]`;
const q1 = (page, sel, expr) =>
    page.eval(
        `(() => { const e = document.querySelector(${JSON.stringify(sel)}); return e ? (${expr})(e) : null; })()`,
    );

/** 一页的快照：页上看得见的各个状态。 */
const readPage = (page) =>
    page.eval(`(() => {
        const $ = (s) => document.querySelector(s);
        const at = (s, a) => { const e = $(s); return e ? e.getAttribute(a) : null; };
        const row = (part) => { const e = $('[data-part="' + part + '"]'); return e ? e.getAttribute("data-state") : null; };
        const v = $('[data-part="track.input"] .rn-verify');
        const c = $('[data-part="track.input"] .rn-ch');
        const sel = $('${gb("group")} [role="radio"][aria-checked="true"]');
        return {
            pill: at('${gb("pair.pill")}', "data-state"),
            tone: at('${gb("pair.pill")}', "data-tone"),
            pulse: at('${gb("pair.pill")}', "data-pulse"),
            sub: at('[data-part="pair.sub"]', "data-state"),
            thisRow: row("track.this"),
            inputRow: row("track.input"),
            verify: v && !v.hidden ? v.getAttribute("data-verify") : null,
            channel: c && !c.hidden ? c.getAttribute("data-ch") : null,
            band: at('${gb("band")}', "data-state"),
            bandMsg: at('${gb("band")}', "data-msg"),
            bandRole: at('${gb("band")}', "role"),
            send: at('${gb("send")}', "data-state"),
            hint: at('${gb("footer.hint")}', "data-state"),
            shell: at('${gb("shell")}', "data-state"),
            danger: at('${gb("shell")}', "data-danger"),
            group: sel ? sel.getAttribute("data-group") : null,
            groupState: at('${gb("group")}', "data-state"),
            lang: document.documentElement.lang,
            about: Boolean($('${gb("about")}') && !$('${gb("about")}').closest("[hidden]")),
            scaleConfirm: Boolean($('${gb("scale.confirm")}') && !$('${gb("scale.confirm")}').closest("[hidden]")),
            placeInLayer: Boolean($('.rn-shell__layer .rn-place')),
            chip: ($('${gb("band")} [data-part="band.more"]') || {}).textContent || null,
        };
    })()`);

// ---------------------------------------------------------------- ① 场景表

/** 各场景应有的状态；缺省项不查（只写这个场景关心的）。 */
const WANT = {
    "out-searching": {
        pill: "searching",
        tone: "gray",
        pulse: "0",
        sub: "searching",
        inputRow: "chain",
        verify: null,
        channel: null,
        band: "idle",
        send: "none",
        hint: "unpaired",
        shell: "normal",
        danger: "0",
        group: "A",
        groupState: "autoFallback",
    },
    "out-connected": {
        pill: "connected",
        tone: "green",
        pulse: "1",
        sub: "connected",
        thisRow: "name",
        inputRow: "name",
        verify: "match",
        channel: "st",
        band: "idle",
        send: "ok",
        hint: "connected",
        shell: "normal",
        danger: "0",
        group: "A",
        groupState: "auto",
    },
    "out-unverified": {
        pill: "connectedUnverified",
        tone: "green",
        pulse: "1",
        sub: "connectedUnverified",
        thisRow: "noName",
        inputRow: "noName",
        verify: "unknown",
        channel: "st",
        band: "idle",
        send: "ok",
        hint: "connected",
    },
    "out-cross-track": {
        pill: "crossTrack",
        tone: "amber",
        pulse: "0",
        sub: "notice",
        verify: "diff",
        bandMsg: "band.out.crossTrack",
        bandRole: "status",
        send: "ok",
        hint: "connected",
        danger: "0",
    },
    "out-stale": {
        pill: "stale",
        tone: "amber",
        pulse: "0",
        sub: "stale",
        bandMsg: "band.out.stale",
        send: "ok",
        hint: "connected",
    },
    "out-conflict": {
        pill: "conflict",
        tone: "red",
        sub: "notice",
        inputRow: "noName",
        verify: null,
        bandMsg: "band.out.conflict",
        bandRole: "alert",
        send: "none",
        hint: "conflict",
        shell: "danger",
        danger: "1",
    },
    "out-takeover-confirm": {
        pill: "searching",
        band: "confirm",
        bandMsg: "band.out.takeover",
        send: "none",
        hint: "unpaired",
        shell: "normal",
    },
    "out-abi": {
        pill: "abiMismatch",
        tone: "red",
        sub: "notice",
        bandMsg: "band.out.abi",
        bandRole: "alert",
        send: "none",
        hint: "unpaired",
        shell: "danger",
        danger: "1",
    },
    "out-bypassed": {
        pill: "bypassed",
        tone: "gray",
        pulse: "0",
        sub: "bypassed",
        send: "bypassed",
        hint: "connected",
        shell: "bypassed",
    },
    "out-input-muted": {
        pill: "connectedStill",
        tone: "green",
        pulse: "0",
        sub: "inputMuted",
        send: "ok",
        hint: "connected",
    },
    "out-waiting-audio": {
        pill: "waitingAudio",
        tone: "gray",
        pulse: "0",
        sub: "notice",
        bandMsg: "band.out.waitingAudio",
        bandRole: "status",
        send: "idle",
        hint: "unpaired",
        group: null,
    },
    "out-about": { pill: "searching", about: true, shell: "modal" },
    "out-scale-confirm": { scaleConfirm: true, shell: "modal" },
    "out-en": { pill: "connected", lang: "en", inputRow: "name", verify: "match" },
    "out-fr-worst": {
        pill: "abiMismatch",
        tone: "red",
        lang: "fr",
        sub: "notice",
        bandMsg: "band.out.abi",
        chip: "+1",
        shell: "danger",
        verify: "diff",
        inputRow: "noName",
    },
};

run.eq(
    scenes.map((s) => s.id).sort(),
    Object.keys(WANT).sort(),
    "场景表与预览场景表一致：本页负责的场景一个不多一个不少（防新增场景漏测）",
);

try {
    const page = await browser.newPage();
    const requests = await page.recordRequests();

    for (const sc of scenes) {
        await openOutput(page, server, box, sc.lang, sc.id);
        const got = await readPage(page);
        const want = WANT[sc.id];
        for (const [k, v] of Object.entries(want)) run.eq(got[k], v, `${sc.id}：${k}`);
        run.eq(got.placeInLayer, true, `${sc.id}：常驻红字在外壳的 layer 层（旁路时不随内容降级）`);
        run.eq(got.lang, sc.lang, `${sc.id}：<html lang> 跟界面语言`);
    }

    // ---------------------------------------------------------------- ② 三语：文字取自词典
    const PILL_KEY = {
        connected: "common.pair.connected",
        conflict: "common.pair.conflict",
        searching: "out.pair.searching",
    };
    const VERIFY_KEY = { match: "common.verify.match", diff: "common.verify.diff", unknown: "common.verify.unknown" };
    const dictOf = (lang) => ({ ...common[lang], ...outDict[lang] });
    for (const lang of LANGS) {
        const d = dictOf(lang);
        for (const [scene, st] of [
            ["out-connected", "connected"],
            ["out-conflict", "conflict"],
            ["out-searching", "searching"],
        ]) {
            await openOutput(page, server, box, lang, scene);
            run.eq(await textOf(page, gb("pair.pill")), d[PILL_KEY[st]], `${lang}/${scene}：状态灯文字取自词典`);
            run.eq(
                await textOf(page, '[data-part="pair.sub"]').then((s) => s.length > 0),
                true,
                `${lang}/${scene}：副行有文字`,
            );
            run.eq(await attr(page, "html", "lang"), lang, `${lang}/${scene}：<html lang>`);
        }
        await openOutput(page, server, box, lang, "out-cross-track");
        run.eq(
            await textOf(page, '[data-part="track.input"] .rn-verify'),
            d[VERIFY_KEY.diff],
            `${lang}：核验徽标文字取自词典`,
        );
        // 中文界面页眉带「回环」后缀，其余语言不带
        const suffixShown = await page.eval(
            `(() => { const s = document.querySelector(".rn-brand__suffix"); return Boolean(s) && !s.hidden; })()`,
        );
        run.eq(suffixShown, lang === "zh", `${lang}：页眉「回环」后缀只在中文出现`);
    }

    // ---------------------------------------------------------------- ③ 交互
    // 选组：点 C → setGroup("C")，等确认帧后弹「已切到组 C」；点已选的 A 不重复调用
    await openOutput(page, server, box, "zh", "out-connected");
    await click(page, `${gb("group")} [data-group="A"]`);
    run.eq(await callsOf(page, "setGroup"), [], "点已选中的组不调 setGroup");
    await click(page, `${gb("group")} [data-group="C"]`);
    run.eq(await callsOf(page, "setGroup"), [["C"]], "点 C 应调 setGroup('C')");
    await awaitState(
        () => readPage(page),
        (p) => p.group === "C",
        "选中态跟到 C",
    );
    await awaitState(
        () => textOf(page, gb("toast")),
        (s) => typeof s === "string" && s.includes("C"),
        "选组成功的提示",
    );
    run.eq(await q1(page, gb("toast"), "(e) => e.hidden"), false, "选组成功后轻提示可见");

    // 接管确认条：两枚按钮各回调一次；确认条展开期间其余组胶囊禁用
    await openOutput(page, server, box, "zh", "out-takeover-confirm");
    const disabled = await page.eval(
        `[...document.querySelectorAll('${gb("group")} [role="radio"]')].filter((r) => r.getAttribute("aria-disabled") === "true").length`,
    );
    run.eq(disabled, 7, "确认条展开：除选中的组以外 7 枚胶囊禁用");
    await click(page, `${gb("band")} [data-part="band.action"]`);
    run.eq(await callsOf(page, "resolveTakeover"), [[true]], "点「接管」应调 resolveTakeover(true)");
    await openOutput(page, server, box, "zh", "out-takeover-confirm");
    await click(page, `${gb("band")} [data-part="confirm.cancel"]`);
    run.eq(await callsOf(page, "resolveTakeover"), [[false]], "点「取消」应调 resolveTakeover(false)");
    // 确认条的组号：读 takeoverGroup（与 group 不同时也读它），缺省或不认识时读 group；takeover 为假时 takeoverGroup 不起作用。
    // 同一次求值里先让 mock 现造一帧当底稿，再逐个改字段同步发出、立刻读（镜像回声每 250 ms 覆盖一次）
    await openOutput(page, server, box, "zh", "out-takeover-confirm");
    {
        const shown = await page.eval(`(() => {
            const core = window.__fx.session.core;
            let base = null;
            core.addEventListener("rinne.state", (p) => { if (!base) base = p; });
            core.push("rinne.state");
            const read = (extra) => {
                core.emit("rinne.state", { ...base, seq: core.nextSeq(), ...extra });
                const b = document.querySelector('${gb("band")}');
                return { confirm: Boolean(b.querySelector('[data-part="confirm.cancel"]')), text: b.textContent };
            };
            return {
                mock: [base.group, base.takeoverGroup],
                other: read({ takeoverGroup: "E" }),
                missing: read({ takeoverGroup: undefined }),
                bogus: read({ takeoverGroup: "Z" }),
                off: read({ takeover: false, takeoverGroup: "E" }),
            };
        })()`);
        const bar = (g) => outDict.zh["band.out.takeover"].replace("{g}", g);
        run.eq(shown.mock, ["A", "A"], "mock：待接管时 rinne.state 带 takeoverGroup（预览里就是本组）");
        run.check(
            shown.other.confirm && shown.other.text.includes(bar("E")),
            "确认条的组号读 takeoverGroup（E，不是 group 的 A）",
        );
        run.check(shown.missing.confirm && shown.missing.text.includes(bar("A")), "takeoverGroup 缺省：组号读 group");
        run.check(shown.bogus.confirm && shown.bogus.text.includes(bar("A")), "takeoverGroup 不认识：组号读 group");
        run.eq(shown.off.confirm, false, "takeover 为假：带了 takeoverGroup 也不出确认条");
    }
    await openOutput(page, server, box, "zh", "out-connected");
    run.eq(
        await page.eval(`(() => {
            const core = window.__fx.session.core;
            let f = null;
            core.addEventListener("rinne.state", (p) => { if (!f) f = p; });
            core.push("rinne.state");
            return [f.takeover, "takeoverGroup" in f];
        })()`),
        [false, false],
        "mock：没有待接管的组时 rinne.state 不带 takeoverGroup",
    );

    // 重新配对：冲突态的动作按钮 → repairPair；成功后切到新组并弹提示
    await openOutput(page, server, box, "zh", "out-conflict");
    await click(page, `${gb("band")} [data-part="band.action"]`);
    run.eq((await callsOf(page, "repairPair")).length, 1, "点「重新配对」应调 repairPair 一次");
    await awaitState(
        () => readPage(page),
        (p) => p.pill === "searching" && p.group !== "A",
        "重新配对后进入搜索态并换了组",
    );

    // 查看说明：只发白名单里的地址，不带 # 锚点
    for (const sc of ["out-abi", "out-cross-track"]) {
        // out-cross-track 没有说明按钮：只查 abi；cross-track 只验证没有多余的按钮
        await openOutput(page, server, box, "zh", sc);
        const has = await q1(page, `${gb("band")} [data-part="band.action"]`, "(e) => e.getAttribute('data-action')");
        if (sc === "out-abi") {
            run.eq(has, "guide", "abi 消息的动作是「查看说明」");
            await click(page, `${gb("band")} [data-part="band.action"]`);
            const opened = await callsOf(page, "openExternal");
            run.eq(opened.length, 1, "点「查看说明」应调 openExternal 一次");
            run.check(
                /^https:\/\/github\.com\/synchain-oss\/rinne\/blob\/[^#?\s]+\/docs\/USER_GUIDE\.zh-CN\.md$/.test(
                    String((opened[0] || [])[0]),
                ),
                `「查看说明」的地址应是钉版本的用户指南、不带锚点，实得 ${JSON.stringify(opened[0])}`,
            );
        } else run.eq(has, null, "crossTrack 消息没有动作按钮");
    }

    // 语言胶囊：点 EN → setLang('en') 且立即换语言
    await openOutput(page, server, box, "zh", "out-connected");
    // 预览的 ?lang= 是「不落盘的覆盖」，会让回声一直带 zh；先撤掉覆盖，回声才跟 setLang 走（真桥没有这个覆盖）
    await page.eval(`window.__fx.mutate((m) => { m.lang = null; })`);
    await click(page, `${gb("lang")} [data-lang="en"]`);
    run.eq(await callsOf(page, "setLang"), [["en"]], "点 EN 应调 setLang('en')");
    await awaitState(
        () => readPage(page),
        (p) => p.lang === "en",
        "点 EN 后页面换成英文",
    );
    run.eq(await textOf(page, gb("pair.pill")), common.en["common.pair.connected"], "切到英文后状态灯是英文");

    // 缩放：选档 → 缩放确认出现 → 保存调 commitUiScale；再选档 → 取消回到选档前
    await openOutput(page, server, box, "zh", "out-connected");
    const pickScale = (v) =>
        page.eval(
            `(() => { const s = document.querySelector('${gb("footer.scale")} select'); s.value = ${JSON.stringify(v)}; s.dispatchEvent(new Event("change", { bubbles: true })); return s.value; })()`,
        );
    run.eq(await pickScale("1.5"), "1.5", "缩放下拉里有 150% 档");
    await awaitState(
        () => readPage(page),
        (p) => p.scaleConfirm,
        "选档后出现缩放确认",
    );
    run.eq(await callsOf(page, "setUiScale"), [[1.5]], "选档应调 setUiScale(1.5)");
    run.eq((await readPage(page)).shell, "modal", "缩放确认打开：外壳是模态态");
    await click(page, `${gb("scale.confirm")} [data-part="confirm.keep"]`);
    run.eq((await callsOf(page, "commitUiScale")).length, 1, "点「保存」应调 commitUiScale");
    await awaitState(
        () => readPage(page),
        (p) => !p.scaleConfirm,
        "保存后缩放确认收起",
    );
    await pickScale("2");
    await awaitState(
        () => readPage(page),
        (p) => p.scaleConfirm,
        "再选档后出现缩放确认",
    );
    await click(page, `${gb("scale.confirm")} [data-part="confirm.cancel"]`);
    await awaitState(
        () => callsOf(page, "setUiScale"),
        (c) => c.length === 3,
        "取消应把档位调回去",
    );
    run.eq(
        (await callsOf(page, "setUiScale")).map((a) => a[0]),
        [1.5, 2, 1.5],
        "取消回到选档前的档位（上一次保存的 1.5）",
    );

    // 关于面板：点版本按钮打开，内容层 inert，关闭后焦点回到版本按钮；复制诊断弹提示
    await openOutput(page, server, box, "zh", "out-connected");
    await page.eval(`(() => { const v = document.querySelector('${gb("footer.version")}'); v.focus(); v.click(); })()`);
    await awaitState(
        () => readPage(page),
        (p) => p.about,
        "点版本按钮后关于面板打开",
    );
    run.eq(await q1(page, ".rn-shell__content", "(e) => e.hasAttribute('inert')"), true, "关于面板打开：内容层 inert");
    run.check(
        (await textOf(page, `${gb("about")} .rn-about__build`)).includes("a1b2c3d"),
        "关于面板的构建行取自 rinne.state 的 build 载荷",
    );
    // Output 有 tail 行（setTailMode），没有起播清空；备注是 Output 那句，并且只跟着 tail 行出现
    {
        const adv = await page.eval(`(() => {
            const a = document.querySelector('${gb("about")}');
            const note = a.querySelector('[data-part="about.advNote"]');
            const tailRow = a.querySelector('[data-row="tail"]');
            return {
                groups: a.querySelectorAll('[role="radiogroup"]').length,
                tailGroups: a.querySelectorAll('[data-row="tail"] [role="radiogroup"]').length,
                reset: a.querySelectorAll('[data-adv="resetOnPlay"]').length,
                note: note && !note.hidden ? note.textContent : null,
                noteWithTail: Boolean(note && !note.hidden) === Boolean(tailRow),
            };
        })()`);
        run.eq(
            adv,
            { groups: 1, tailGroups: 1, reset: 0, note: common.zh["about.adv.noteOut"], noteWithTail: true },
            "Output 关于面板：有且只有一个 tail 选项组，备注是 Output 那句且跟着 tail 行，没有起播清空开关",
        );
    }
    const copyBtn = `${gb("about")} [data-part="about.copy"]`;
    if (await page.eval(`Boolean(document.querySelector('${copyBtn}'))`)) {
        await click(page, copyBtn);
        await awaitState(
            () => callsOf(page, "copyDiagnostics"),
            (c) => c.length >= 1,
            "点复制诊断应调 copyDiagnostics",
        );
    } else run.skipItem("关于面板的复制按钮", "选择器没有命中（按钮的 data-part 变了？）");
    await page.send("Input.dispatchKeyEvent", {
        type: "rawKeyDown",
        key: "Escape",
        code: "Escape",
        windowsVirtualKeyCode: 27,
    });
    await page.send("Input.dispatchKeyEvent", {
        type: "keyUp",
        key: "Escape",
        code: "Escape",
        windowsVirtualKeyCode: 27,
    });
    await awaitState(
        () => readPage(page),
        (p) => !p.about,
        "Esc 关闭关于面板",
    );
    run.eq(await q1(page, ".rn-shell__content", "(e) => e.hasAttribute('inert')"), false, "关闭后内容层不再 inert");

    // 「?」在首启链接入前：不调任何桥函数，也不报错
    await openOutput(page, server, box, "zh", "out-connected");
    const before = (await page.eval("window.__fx.calls.length")) + 0;
    await click(page, gb("footer.help"));
    await sleep(page, 100);
    run.eq(await page.eval("window.__fx.calls.length"), before, "「?」当前无动作：不调桥函数");

    // ---------------------------------------------------------------- ④ 重绘纪律
    await openOutput(page, server, box, "zh", "out-connected");
    const mutations = await page.eval(`new Promise((resolve) => {
        const n = [];
        const mo = new MutationObserver((list) => { for (const m of list) n.push(m.type + ":" + (m.attributeName || "") + ":" + (m.target.className || m.target.nodeName)); });
        mo.observe(document.getElementById("stage"), { subtree: true, childList: true, attributes: true, characterData: true });
        setTimeout(() => { mo.disconnect(); resolve({ n: n.length, sample: n.slice(0, 5), echoes: window.__fx.session.core.sent["rinne.state"] }); }, 1700);
    })`);
    run.check(mutations.echoes >= 3, `观察期内应收到足够多帧回声（防空转），实得 ${mutations.echoes}`);
    run.eq(mutations.n, 0, `回声重复同一状态时页面不应写任何 DOM，实得 ${JSON.stringify(mutations.sample)}`);

    // 乱序旧帧与未知配对状态整帧丢弃
    await openOutput(page, server, box, "zh", "out-connected");
    // emit 是同步派发：在同一次求值里发完立刻读，镜像回声（每 250 ms 覆盖一次）不会把「被错收的帧」悄悄冲掉
    {
        const shown = await page.eval(`(() => {
            window.__fx.emit("rinne.state", { seq: 1, role: "output", group: "H", lang: "en", version: "9.9.9" });
            window.__fx.emit("rinne.pair", { seq: 99999, state: "notARealState", peer: { name: "evil" } });
            const sel = document.querySelector('${gb("group")} [role="radio"][aria-checked="true"]');
            return [
                sel ? sel.getAttribute("data-group") : null,
                document.documentElement.lang,
                document.querySelector('${gb("pair.pill")}').getAttribute("data-state"),
            ];
        })()`);
        run.eq(shown, ["A", "zh", "connected"], "seq 更小的旧状态帧与未知配对状态都被丢弃");
    }
    // 坏载荷：缺字段、错类型不抛错、不白屏
    await page.eval(`window.__fx.emit("rinne.state", { seq: 99998, send: "oops", notices: 7, build: [] })`);
    await page.eval(
        `window.__fx.emit("rinne.pair", { seq: 99999, state: "connected", peer: 5, self: "x", verify: 7 })`,
    );
    await sleep(page, 120);
    run.eq((await readPage(page)).pill, "connected", "坏载荷不影响显示，页面不抛错");

    // ---------------------------------------------------------------- ⑤ 外壳
    await openOutput(page, server, box, "zh", "out-conflict");
    run.eq(
        await page.eval(`getComputedStyle(document.querySelector('${gb("shell")}'), "::after").opacity`),
        "1",
        "冲突态：危险描边可见",
    );
    await openOutput(page, server, box, "zh", "out-connected");
    run.eq(
        await page.eval(`getComputedStyle(document.querySelector('${gb("shell")}'), "::after").opacity`),
        "0",
        "常态：危险描边不显示",
    );
    // 旁路：内容按设计降到 .55（降在各件上，容器自己不降）；常驻红字与 layer 层不降；成立的消息不降，空闲的灰字提示照设计降
    await openOutput(page, server, box, "zh", "out-bypassed");
    const effective = (sel) =>
        page.eval(
            `(() => { let v = 1; for (let e = document.querySelector(${JSON.stringify(sel)}); e; e = e.parentElement) v *= Number(getComputedStyle(e).opacity); return Number(v.toFixed(3)); })()`,
        );
    run.eq(
        {
            header: await effective(".rn-out--header"),
            pair: await effective(".rn-out--pair"),
            band: await effective(gb("band")),
            foot: await effective(".rn-out--foot"),
            place: await effective(".rn-place"),
            layer: await effective(".rn-shell__layer"),
        },
        { header: 0.55, pair: 0.55, band: 0.55, foot: 0.55, place: 1, layer: 1 },
        "旁路（空闲）：内容各件 .55，空闲的灰字提示一起降，常驻红字与 layer 层不降级",
    );
    // 旁路时有红字消息：消息位不降级（宿主旁路与配对冲突同时成立）
    await setModel(page, { state: "conflict", send: { bypassed: true } });
    run.eq(
        await page.eval(`document.querySelector(".rn-shell").hasAttribute("data-bypass")`),
        true,
        "旁路 + 冲突：外壳带 data-bypass",
    );
    run.eq(
        [await effective(gb("band")), await effective(".rn-out--pair"), await effective(".rn-place")],
        [1, 0.55, 1],
        "旁路 + 红字消息：消息位与常驻红字不降级，其余内容仍 .55",
    );
    // 脉冲：只有连通且音频在流动的两态
    await openOutput(page, server, box, "zh", "out-connected");
    const pulseOf = async () => (await readPage(page)).pulse;
    run.eq(await pulseOf(), "1", "已连接且音频在流动：脉冲");
    await setModel(page, { state: "connected", send: { hostPaused: true } });
    run.eq(await pulseOf(), "0", "宿主暂停：不脉冲");
    await setModel(page, { state: "connectedUnverified", verify: "unknown", send: { hostPaused: false } });
    run.eq(await pulseOf(), "1", "已连接（未核验）且音频在流动：脉冲");
    await setModel(page, { state: "stale" });
    run.eq(await pulseOf(), "0", "失联：不脉冲");
    await setModel(page, { state: "connectedStill" });
    run.eq(await pulseOf(), "0", "已连接但 Input 回送静音：不脉冲");

    // 配对状态逐个切一遍：状态灯色语义与页面推导一致
    const TONES = {
        ipcUnavailable: "red",
        abiMismatch: "red",
        srMismatch: "red",
        conflict: "red",
        unassigned: "red",
        stale: "amber",
        crossProcess: "amber",
        crossTrack: "amber",
        searching: "gray",
        waitingAudio: "gray",
        validator: "gray",
        bypassed: "gray",
        connectedUnverified: "green",
        connected: "green",
        connectedStill: "green",
    };
    for (const [st, tone] of Object.entries(TONES)) {
        await setModel(page, { state: st, group: st === "unassigned" ? null : "A" });
        const p = await readPage(page);
        run.eq([p.pill, p.tone], [st, tone], `配对状态 ${st}：状态灯与色语义`);
        run.eq(
            p.danger,
            st === "conflict" || st === "abiMismatch" ? "1" : "0",
            `配对状态 ${st}：只有冲突与版本不匹配是危险描边`,
        );
    }
    // unassigned：8 个组都不选
    await setModel(page, { state: "unassigned", group: null });
    run.eq((await readPage(page)).groupState, "unassigned", "unassigned：组选择整排不选");

    // ---------------------------------------------------------------- ⑥ 桥不通
    {
        const r = await page.eval(`(async () => {
            const { mountOutputPage } = await import("/web/output/page.js");
            const host = document.createElement("div");
            document.body.append(host);
            let asked = 0;
            const noop = async () => ({ ok: false, reason: "not-ready" });
            const bridge = { role: "output", isPreview: false, on: () => () => {}, requestInitialState: async () => { asked++; return { ok: false }; }, setUiScale: noop, commitUiScale: noop, setLang: noop, setGroup: noop, openExternal: noop, copyDiagnostics: noop, setDiagLog: noop, setRulesAck: noop, setGuideSeen: noop, resolveTakeover: noop, showLicense: noop, repairPair: noop };
            const t0 = performance.now();
            const page = mountOutputPage({ host, bridge });
            const hiddenAtStart = !page.isRevealed();
            await new Promise((r) => setTimeout(r, 650));
            const revealedAt650 = page.isRevealed();
            await new Promise((r) => setTimeout(r, 1300));
            const out = { hiddenAtStart, revealedAt650, asked, texts: host.textContent.length > 20 };
            page.destroy();
            host.remove();
            return out;
        })()`);
        run.eq(r.hiddenAtStart, true, "桥还没给快照：内容层先藏着");
        run.eq(r.revealedAt650, true, "桥一直不给快照：400 ms 兜底揭幕（不能一直空着）");
        run.check(r.asked >= 2, `requestInitialState 失败应重试，实得调用 ${r.asked} 次`);
        run.eq(r.texts, true, "兜底揭幕后按默认状态显示文字");
    }
    {
        const r = await page.eval(`(async () => {
            const { mountOutputPage } = await import("/web/output/page.js");
            const host = document.createElement("div");
            document.body.append(host);
            const bridge = { role: "output", isPreview: false, on: () => () => {} };
            for (const n of ["requestInitialState", "setUiScale", "commitUiScale", "setLang", "setGroup", "openExternal", "copyDiagnostics", "setDiagLog", "setRulesAck", "setGuideSeen", "resolveTakeover", "showLicense", "repairPair"]) bridge[n] = async () => ({ ok: false, reason: "not-ready" });
            let picked = 0;
            bridge.setGroup = async () => { picked++; return { ok: false, reason: "not-ready" }; };
            const page = mountOutputPage({ host, bridge, offline: true });
            const pills = [...host.querySelectorAll('[data-gb="output.group"] [role="radio"]')];
            const out = { revealed: page.isRevealed(), texts: host.textContent.length > 20, disabled: pills.filter((n) => n.hasAttribute("aria-disabled")).length };
            pills[2].click();
            await new Promise((r) => setTimeout(r, 30));
            out.picked = picked;
            page.destroy();
            host.remove();
            return out;
        })()`);
        run.eq(
            r,
            { revealed: true, texts: true, disabled: 0, picked: 1 },
            "无桥模式：立即揭幕、按默认状态显示，组胶囊照常可点（没有首帧可等）",
        );
    }
    {
        // 首帧慢：requestInitialState 回执成功但帧迟迟不来。兜底揭幕只为显示外壳，两路首帧到齐前组胶囊、重新配对、
        // 接管确认与缩放这几处写入都不发（都基于默认值）；语言照常；到齐后照常选组、选档。三个小页各验一处，帧由测试手动派发
        const r = await page.eval(`(async () => {
            const { mountOutputPage } = await import("/web/output/page.js");
            const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
            const mount = () => {
                const host = document.createElement("div");
                document.body.append(host);
                const calls = [];
                const handlers = {};
                const rec = (name) => async () => { calls.push(name); return { ok: true }; };
                const bridge = { role: "output", isPreview: false, on: (name, cb) => { handlers[name] = cb; return () => { delete handlers[name]; }; } };
                for (const n of ["requestInitialState", "setUiScale", "commitUiScale", "setLang", "setGroup", "openExternal", "copyDiagnostics", "setDiagLog", "setRulesAck", "setGuideSeen", "resolveTakeover", "showLicense", "repairPair", "setTailMode"]) bridge[n] = rec(n);
                const p = mountOutputPage({ host, bridge });
                const $ = (s) => host.querySelector(s);
                const pill = (g) => $('[data-gb="output.group"] [role="radio"][data-group="' + g + '"]');
                const action = () => $('[data-gb="output.band"] [data-part="band.action"]');
                const scaleSel = () => $('[data-gb="output.footer.scale"] select');
                const scaleConfirmShown = () => { const c = $('[data-gb="output.scale.confirm"]'); return Boolean(c && !c.closest("[hidden]")); };
                const count = (name) => calls.filter((n) => n === name).length;
                return { host, handlers, p, $, pill, action, scaleSel, scaleConfirmShown, count, done: () => { p.destroy(); host.remove(); } };
            };
            const state = (extra) => ({ seq: 1, role: "output", group: "A", lang: "zh", version: "1.0.0", ...extra });
            const out = {};
            // 组胶囊：兜底揭幕后整排禁用，点了不发 setGroup；语言照常；两路首帧到齐后解禁、能选组
            const A = mount();
            await sleep(650);
            out.revealed = A.p.isRevealed();
            out.disabled = [...A.host.querySelectorAll('[data-gb="output.group"] [role="radio"]')].filter((n) => n.getAttribute("aria-disabled") === "true").length;
            const sel = A.scaleSel();
            const enabledOptions = () => [...sel.options].filter((n) => !n.disabled).length;
            out.scaleBefore = { disabled: sel.disabled, shown: sel.selectedIndex >= 0 ? sel.options[sel.selectedIndex].textContent : null, enabled: enabledOptions() };
            A.pill("C").click();
            A.$('[data-gb="output.lang"] [data-lang="en"]').click();
            // 下拉置灰之外再验页面自己这一道：即使来了一次选档（合成的 change），首帧前也不进缩放预览
            const probe = document.createElement("option");
            probe.value = "1.5";
            sel.append(probe);
            sel.value = "1.5";
            sel.dispatchEvent(new Event("change"));
            probe.remove();
            await sleep(30);
            out.before = { setGroup: A.count("setGroup"), setLang: A.count("setLang"), asked: A.count("requestInitialState"), setUiScale: A.count("setUiScale"), commitUiScale: A.count("commitUiScale"), scaleConfirm: A.scaleConfirmShown() };
            A.handlers["rinne.state"](state());
            await sleep(30);
            A.pill("C").click();
            await sleep(30);
            out.stateOnly = A.count("setGroup");
            A.handlers["rinne.pair"]({ seq: 1, state: "searching" });
            out.disabledAfter = A.pill("C").hasAttribute("aria-disabled");
            A.pill("C").click();
            await sleep(30);
            out.after = A.count("setGroup");
            out.scaleAfter = { disabled: sel.disabled, value: sel.value, enabled: enabledOptions() };
            const pick = [...sel.options].find((n) => !n.disabled && n.value !== sel.value);
            if (pick) {
                sel.value = pick.value;
                sel.dispatchEvent(new Event("change"));
            }
            await sleep(30);
            out.scalePicked = { setUiScale: A.count("setUiScale"), scaleConfirm: A.scaleConfirmShown() };
            A.done();
            // 重新配对：只来了 rinne.pair（冲突）时不发 repairPair，rinne.state 也到了才发
            const B = mount();
            B.handlers["rinne.pair"]({ seq: 1, state: "conflict" });
            await sleep(450);
            out.repairAction = B.action() ? B.action().getAttribute("data-action") : null;
            if (B.action()) B.action().click();
            await sleep(30);
            out.repairBefore = B.count("repairPair");
            B.handlers["rinne.state"](state());
            if (B.action()) B.action().click();
            await sleep(30);
            out.repairAfter = B.count("repairPair");
            B.done();
            // 接管确认：只来了 rinne.state（待接管）时两个按钮都不发 resolveTakeover，rinne.pair 也到了才发
            const C = mount();
            C.handlers["rinne.state"](state({ takeover: true }));
            await sleep(450);
            out.confirmAction = C.action() ? C.action().getAttribute("data-action") : null;
            if (C.action()) C.action().click();
            const cancel = C.$('[data-gb="output.band"] [data-part="confirm.cancel"]');
            if (cancel) cancel.click();
            await sleep(30);
            out.takeoverBefore = C.count("resolveTakeover");
            C.handlers["rinne.pair"]({ seq: 1, state: "searching" });
            if (C.action()) C.action().click();
            await sleep(30);
            out.takeoverAfter = C.count("resolveTakeover");
            C.done();
            return out;
        })()`);
        run.eq(r.revealed, true, "首帧慢：400 ms 兜底揭幕照常（外壳不能一直空着）");
        run.eq(r.disabled, 8, "首帧慢：兜底揭幕后两路首帧到齐前组胶囊整排禁用");
        run.eq(
            r.scaleBefore,
            { disabled: true, shown: "—", enabled: 0 },
            "首帧慢：缩放下拉置灰，读数是「—」（不显示默认的 100%），没有可选的档位",
        );
        run.eq(
            r.before,
            { setGroup: 0, setLang: 1, asked: 1, setUiScale: 0, commitUiScale: 0, scaleConfirm: false },
            "首帧慢：点组胶囊不发 setGroup、选档不发 setUiScale / commitUiScale 也不出缩放确认条，语言照常发 setLang（回执成功不重试）",
        );
        run.eq(r.stateOnly, 0, "首帧慢：只到了 rinne.state 仍不发 setGroup");
        run.eq([r.disabledAfter, r.after], [false, 1], "首帧慢：两路首帧到齐后组胶囊解禁，点 C 发 setGroup");
        run.eq(
            [r.scaleAfter.disabled, r.scaleAfter.value, r.scaleAfter.enabled > 1],
            [false, "1", true],
            "首帧慢：两路首帧到齐后缩放下拉可用，读数是快照里的档位",
        );
        run.eq(r.scalePicked, { setUiScale: 1, scaleConfirm: true }, "首帧慢：就绪后选档发 setUiScale 并出缩放确认条");
        run.eq(r.repairAction, "repair", "首帧慢（对照）：只到了冲突的 rinne.pair 时消息位已有「重新配对」");
        run.eq([r.repairBefore, r.repairAfter], [0, 1], "首帧慢：rinne.state 没到时不发 repairPair，到了才发");
        run.eq(r.confirmAction, "takeover", "首帧慢（对照）：只到了待接管的 rinne.state 时确认条已展开");
        run.eq([r.takeoverBefore, r.takeoverAfter], [0, 1], "首帧慢：rinne.pair 没到时接管确认不发，到了才发");
    }

    {
        // 销毁：首帧快照一直不来、重试排着的时候销毁（A：挂载后立刻销毁，回执还没到；B：回执到了、重试已排上再销毁），
        // 页面自己排的定时器一个不留，等过整个重试窗口也不再调 requestInitialState，pagehide 监听摘掉
        const r = await page.eval(`(async () => {
            const { mountOutputPage } = await import("/web/output/page.js");
            const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
            const noop = async () => ({ ok: false, reason: "not-ready" });
            const make = () => {
                const live = new Set();
                const st = { asked: 0 };
                const timers = {
                    setTimeout: (fn, ms) => {
                        const id = setTimeout(() => { live.delete(id); fn(); }, ms);
                        live.add(id);
                        return id;
                    },
                    clearTimeout: (id) => { live.delete(id); clearTimeout(id); },
                };
                const bridge = { role: "output", isPreview: false, on: () => () => {}, requestInitialState: async () => { st.asked++; return { ok: false }; }, setUiScale: noop, commitUiScale: noop, setLang: noop, setGroup: noop, openExternal: noop, copyDiagnostics: noop, setDiagLog: noop, setRulesAck: noop, setGuideSeen: noop, resolveTakeover: noop, showLicense: noop, repairPair: noop };
                const hooks = new Set();
                const win = new EventTarget();
                win.document = document;
                win.screen = window.screen;
                const add = win.addEventListener.bind(win);
                const rm = win.removeEventListener.bind(win);
                win.addEventListener = (t, h, o) => { if (t === "pagehide") hooks.add(h); add(t, h, o); };
                win.removeEventListener = (t, h, o) => { if (t === "pagehide") hooks.delete(h); rm(t, h, o); };
                const host = document.createElement("div");
                document.body.append(host);
                const p = mountOutputPage({ host, bridge, win, timers });
                let hidden = 0;
                p.parts.scaleUi.pagehide = () => { hidden++; };
                return { live, st, hooks, win, host, p, hidden: () => hidden };
            };
            const A = make();
            const out = { A: {}, B: {} };
            out.A.askedAtMount = A.st.asked;
            out.A.pendingAtMount = A.live.size;
            out.A.hooksAtMount = A.hooks.size;
            A.win.dispatchEvent(new Event("pagehide"));
            out.A.hiddenWired = A.hidden();
            A.p.destroy();
            out.A.pendingAtDestroy = A.live.size;
            const B = make();
            await sleep(100);
            out.A.pendingAfterTick = A.live.size;
            out.B.pendingBeforeDestroy = B.live.size;
            B.p.destroy();
            B.p.destroy();
            out.B.pendingAtDestroy = B.live.size;
            await sleep(1700);
            out.A.askedFinal = A.st.asked;
            out.B.askedFinal = B.st.asked;
            out.A.hooksAfter = A.hooks.size;
            out.B.hooksAfter = B.hooks.size;
            A.win.dispatchEvent(new Event("pagehide"));
            out.A.hiddenAfter = A.hidden();
            A.host.remove();
            B.host.remove();
            return out;
        })()`);
        run.eq(r.A.askedAtMount, 1, "销毁：挂载时问了桥一次（对照）");
        run.eq(r.A.hooksAtMount, 1, "销毁：挂载时 pagehide 监听挂上（对照）");
        run.eq(r.A.hiddenWired, 1, "销毁：销毁前 pagehide 会转给缩放件（对照）");
        run.eq(r.A.pendingAtMount, 1, "销毁：挂载时排着兜底揭幕计时器一个（对照）");
        run.eq(r.B.pendingBeforeDestroy, 2, "销毁：回执失败后排着兜底揭幕与重试两个计时器（对照）");
        run.eq(r.A.pendingAtDestroy, 0, "销毁（回执未到）：页面自己排的定时器一个不留");
        run.eq(r.A.pendingAfterTick, 0, "销毁（回执未到）：回执到了也不再排重试");
        run.eq(r.B.pendingAtDestroy, 0, "销毁（重试已排上）：重试与兜底揭幕计时器都清掉");
        run.eq(r.A.askedFinal, 1, "销毁（回执未到）：等过整个重试窗口也不再调 requestInitialState");
        run.eq(r.B.askedFinal, 1, "销毁（重试已排上）：等过整个重试窗口也不再调 requestInitialState");
        run.eq([r.A.hooksAfter, r.B.hooksAfter], [0, 0], "销毁：pagehide 监听已摘掉");
        run.eq(r.A.hiddenAfter, 1, "销毁后 pagehide 不再转给缩放件");
    }

    // ---------------------------------------------------------------- ⑦ 坏样例：替身必须被同一组断言抓出来
    {
        // 红字挪进内容层：旁路时会跟着降到 .55，「红字在 layer 层」的断言必须变红
        await openOutput(page, server, box, "zh", "out-bypassed");
        await page.eval(`document.querySelector(".rn-shell__content").append(document.querySelector(".rn-place"))`);
        const p = await readPage(page);
        run.eq(p.placeInLayer, false, "坏样例：红字挪进内容层后 placeInLayer 应变假（断言可信）");
        const op = await page.eval(
            `(() => { let v = 1; for (let e = document.querySelector(".rn-place"); e; e = e.parentElement) v *= Number(getComputedStyle(e).opacity); return v; })()`,
        );
        run.check(Number(op) < 1, `坏样例：红字在内容层时旁路会把它降级（opacity ${op}），这正是 layer 层存在的理由`);
    }
    {
        // 危险描边关掉：::after 不可见，「冲突态描边可见」的断言必须变红
        await openOutput(page, server, box, "zh", "out-conflict");
        await page.eval(`document.querySelector('${gb("shell")}').setAttribute("data-danger", "0")`);
        await sleep(page, 400); // 描边的 opacity 有过渡，等它走完再读
        const op = await page.eval(`getComputedStyle(document.querySelector('${gb("shell")}'), "::after").opacity`);
        run.check(op !== "1", `坏样例：data-danger=0 时描边应不可见（实得 ${op}），冲突态的描边断言由此可信`);
    }

    run.check(page.errors.length === 0, `页面不应有错误：${page.errors.slice(0, 3).join("；")}`);
    const foreign = requests.filter((u) => !/^(data|blob|about):/.test(u) && new URL(u).origin !== server.url);
    run.check(foreign.length === 0, `夹具页的请求都必须同源，外部请求：${JSON.stringify(foreign)}`);
} finally {
    await browser.close();
    await server.close();
}
run.finish();
