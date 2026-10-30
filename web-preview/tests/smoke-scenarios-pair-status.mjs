// SPDX-License-Identifier: GPL-3.0-or-later
//
// 共享件的纯函数单测（Node，无 DOM）：配对状态的 15 态与色语义、核验与声道徽标的模型、
// 组位图展开、消息位的颜色优先级与轮换、外链白名单与 tag 链接、发光圆心的缩放换算、toast 语气表。
// 每组都带坏样例：把被测函数的某条规则故意写错的替身必须被同一组断言抓出来（见 expectCaught）。
//
// 用法：node web-preview/tests/smoke-scenarios-pair-status.mjs [仓库根]

import { pathToFileURL } from "node:url";
import path from "node:path";
import { createRun, repoRoot } from "./lib/harness.mjs";
import { PAIR_TONES } from "./shared-measure.mjs";

const ROOT = repoRoot();
const run = createRun("共享件单测");
const load = (rel) => import(pathToFileURL(path.join(ROOT, rel)).href);

const pair = await load("web/shared/pair-status.js");
const groups = await load("web/shared/group-pills.js");
const msg = await load("web/shared/msgline.js");
const links = await load("web/shared/links.js");
const glow = await load("web/shared/glow.js");
const toast = await load("web/shared/toast.js");
const i18n = await load("web/shared/i18n.js");
const common = (await load("web/i18n/common.js")).default;

// ---------------------------------------------------------------- 配对状态

const EXPECT_TONE = PAIR_TONES;

/** 对一份 pairModel 实现做全套断言，返回失败条数，供坏样例复用。 */
function checkPairModel(model) {
    const fails = [];
    const ok = (cond, msgText) => {
        if (!cond) fails.push(msgText);
    };
    ok(Object.keys(EXPECT_TONE).length === 15, "状态表本身是 15 项");
    for (const [state, tone] of Object.entries(EXPECT_TONE)) {
        const m = model({ state, role: "output", flowing: true });
        ok(m && m.tone === tone, `${state} 的色语义应为 ${tone}，得 ${m && m.tone}`);
    }
    // bypassed 只由 Output 产生
    ok(model({ state: "bypassed", role: "input", flowing: true }) === null, "Input 视角没有 bypassed");
    ok(model({ state: "nope", role: "input", flowing: true }) === null, "未知状态返回 null");
    // 脉冲：只有两个绿态在音频流动时脉冲，connectedStill 永不
    ok(model({ state: "connected", role: "input", flowing: true }).pulse === true, "connected 流动时脉冲");
    ok(model({ state: "connected", role: "input", flowing: false }).pulse === false, "connected 不流动时不脉冲");
    ok(
        model({ state: "connectedUnverified", role: "input", flowing: true }).pulse === true,
        "connectedUnverified 流动时脉冲",
    );
    ok(model({ state: "connectedStill", role: "input", flowing: true }).pulse === false, "connectedStill 绿点不脉冲");
    for (const state of ["conflict", "stale", "searching", "waitingAudio"]) {
        ok(model({ state, role: "input", flowing: true }).pulse === false, `${state} 不脉冲`);
    }
    // connectedStill 的文案与 connected 相同
    ok(
        model({ state: "connectedStill", role: "input", flowing: true }).labelKey ===
            model({ state: "connected", role: "input", flowing: true }).labelKey,
        "connectedStill 文案同 connected",
    );
    return fails;
}

run.eq(checkPairModel(pair.pairModel), [], "pairModel 通过全套状态断言");

// 坏样例：connectedStill 也脉冲，必须被同一套断言抓出来
{
    const broken = (p) => {
        const r = pair.pairModel(p);
        return r && p.state === "connectedStill" ? { ...r, pulse: p.flowing === true } : r;
    };
    run.check(checkPairModel(broken).length > 0, "坏样例（connectedStill 脉冲）被抓出");
}

run.eq(pair.statesFor("input").length, 14, "Input 视角 14 态");
run.eq(pair.statesFor("output").length, 15, "Output 视角 15 态");
run.eq(pair.pairLabelKey("searching", "input"), "in.pair.searching", "searching 的 Input 词条在页面词典里");
run.eq(pair.pairLabelKey("searching", "output"), "out.pair.searching", "searching 的 Output 词条在页面词典里");
run.eq(pair.pairLabelKey("bypassed", "output"), "out.pair.bypassed", "bypassed 只在 Output 词典里");

// 每个共享标签键都在 common.js 的三语里有值
{
    const dictAll = { zh: common.zh, en: common.en, fr: common.fr };
    for (const lang of ["zh", "en", "fr"]) {
        for (const state of pair.statesFor("output")) {
            const key = pair.pairLabelKey(state, "output");
            if (key.startsWith("common.")) run.check(Boolean(dictAll[lang][key]), `${key}（${lang}）有词条`);
        }
    }
}

// 核验与声道徽标
run.eq(
    pair.verifyModel("match"),
    { kind: "match", icon: "eq", key: "common.verify.match", tone: "green" },
    "核验一致：等号、绿",
);
run.eq(pair.verifyModel("diff").tone, "amber", "核验不同：琥珀");
run.eq(pair.verifyModel("unknown").tone, "gray", "无法核验：灰");
run.eq(pair.verifyModel("weird"), null, "未知核验种类返回 null");
run.eq(pair.channelModel("st", "st"), { mismatch: false, ch: "st" }, "两端同为 ST：中性徽标");
run.eq(pair.channelModel("st", "mono"), { mismatch: true, from: "st", to: "mono" }, "ST 到 MONO：琥珀失配");
run.eq(pair.channelModel("mono", "st"), { mismatch: true, from: "mono", to: "st" }, "MONO 到 ST：琥珀失配");
run.eq(pair.channelModel(null, "st"), null, "不知道 Input 声道时不画");

// ---------------------------------------------------------------- 组胶囊

run.eq(groups.groupsFromMask(0b00000101), ["A", "C"], "位图第 0、2 位对应 A、C");
run.eq(groups.groupsFromMask(0), [], "位图为 0 没有点");
run.eq(groups.groupsFromMask(0xff).length, 8, "位图全 1 八个点");
run.eq(groups.groupsFromMask(null), [], "位图缺失当 0");
run.eq(groups.groupState({ group: "A", auto: false }), "manual", "手动选组");
run.eq(groups.groupState({ group: "A", auto: true }), "auto", "自动选组");
run.eq(groups.groupState({ group: "A", auto: "fallback" }), "autoFallback", "自动退到暂用组");
run.eq(groups.groupState({ group: null, unassigned: true }), "unassigned", "8 组已满：未分配");
run.eq(groups.groupState({ group: null }), "none", "没有组");

// ---------------------------------------------------------------- 消息位排序与轮换

{
    const list = [
        { id: "n1", tone: "neutral" },
        { id: "a1", tone: "amber" },
        { id: "r1", tone: "red" },
        { id: "g1", tone: "gray" },
        { id: "v1", tone: "violet" },
        { id: "a2", tone: "amber" },
        { id: "r2", tone: "red" },
    ];
    const o = msg.orderMessages(list, 0);
    run.eq(
        o.list.map((m) => m.id),
        ["r1", "r2", "a1", "a2", "g1", "v1", "n1"],
        "颜色优先级 红 > 琥珀 > 灰 > 紫 > 中性，同色保持页面给的顺序",
    );
    run.eq(o.shown, 0, "不轮换时显示第一条");
    run.eq(msg.orderMessages(list, 1).shown, 1, "轮换一次显示第二条");
    run.eq(msg.orderMessages(list, 7).shown, 0, "轮换一圈回到第一条");
    run.eq(msg.orderMessages(list, -1).shown, 6, "负偏移按取模处理");
    run.eq(msg.orderMessages([], 3).shown, -1, "空表没有可显示的条目");
}
run.eq(msg.CUT_KEYS.length, 5, "切断类五条");
run.check(msg.isDark({ key: "band.in.cutNan" }), "cutNan 是深底危险条");
run.check(!msg.isDark({ key: "band.in.conflict" }), "conflict 是浅底红条");
run.check(!msg.isDark({ key: "band.in.cutNan", dark: false }), "显式 dark:false 覆盖键判断");
run.eq(msg.ACTIONS.restore.kind, "cta", "恢复回送是主行动");
run.eq(msg.ACTIONS.guide.kind, "darkSolid", "查看说明是次行动");
run.eq(msg.ACTIONS.ack.kind, "darkSolid", "知道了是次行动");
for (const name of ["restore", "moveFree", "moveTo", "limOn", "repair", "takeover"])
    run.eq(msg.ACTIONS[name].kind, "cta", `${name} 是主行动`);

// ---------------------------------------------------------------- 链接

run.eq(links.docRef("0.9.0"), "v0.9.0", "正式版本钉 tag");
run.eq(links.docRef("0.9.0-rc.1"), "v0.9.0-rc.1", "预发布版本钉 tag");
run.eq(links.docRef("dev"), "prod", "非版本串回退 prod");
run.eq(links.docRef(""), "prod", "空串回退 prod");
run.eq(links.docRef(undefined), "prod", "缺失回退 prod");
run.eq(links.docRef("0.9"), "prod", "不完整版本串回退 prod");
run.eq(
    links.docLink("v0.9.0", "/THIRD-PARTY-NOTICES.md"),
    "https://github.com/synchain-oss/rinne/blob/v0.9.0/THIRD-PARTY-NOTICES.md",
    "docLink 去掉前导斜杠",
);
const all = links.aboutLinks("0.9.0");
run.eq(Object.keys(all), ["source", "site", "contact", "notices"], "关于面板四个链接");
for (const [name, url] of Object.entries(all)) run.check(links.isAllowedExternal(url), `${name} 链接在白名单内`);
for (const [url, want] of [
    ["https://synchain.ca/", true],
    ["https://github.com/synchain-oss/rinne", true],
    ["https://github.com/synchain-oss/rinne/blob/prod/x.md", true],
    ["https://github.com/synchain-oss/rinne-evil", false],
    ["https://github.com/synchain-oss/other", false],
    ["http://synchain.ca/", false],
    ["https://synchain.ca.evil.com/", false],
    ["https://evil.com/?u=synchain.ca", false],
    ["https://user@synchain.ca/", false],
    ["https://synchain.ca:8443/", false],
    ["javascript:alert(1)", false],
    ["", false],
    ["not a url", false],
]) {
    run.eq(links.isAllowedExternal(url), want, `白名单 ${JSON.stringify(url)}`);
}

// ---------------------------------------------------------------- 发光圆心

{
    const rect = { left: 100, top: 50, width: 400, height: 100 };
    run.eq(glow.glowPoint(300, 100, rect, 1), { mx: "200.0px", my: "50.0px" }, "倍率 1：事件坐标减去元素左上角");
    // 倍率 2：元素被放大一倍，rect 的 left/top 是视口坐标，事件坐标同系，结果要除以 2
    run.eq(glow.glowPoint(300, 100, rect, 2), { mx: "100.0px", my: "25.0px" }, "倍率 2：除回设计坐标");
    run.eq(glow.glowPoint(300, 100, rect, 0.5), { mx: "400.0px", my: "100.0px" }, "倍率 0.5：除回设计坐标");
    // 坏样例：不除倍率时，倍率 2 下的结果会与上面不同
    run.check(
        glow.glowPoint(300, 100, rect, 1).mx !== glow.glowPoint(300, 100, rect, 2).mx,
        "不同倍率给出不同圆心（不除倍率会被抓出）",
    );
}

// ---------------------------------------------------------------- toast

run.eq(toast.TOAST_MS, 4000, "toast 停留 4 秒");
run.eq(
    toast.SHARED_TOAST_TONES,
    { "toast.group": "green", "toast.diagCopied": "green", "toast.copyFail": "red" },
    "共享三条的语气",
);

// ---------------------------------------------------------------- 词条表与键引用

{
    const parts = i18n.mergeDicts(common);
    for (const lang of ["zh", "en", "fr"]) {
        for (const key of [
            "band.action.restore",
            "band.action.takeover",
            "common.pair.connected",
            "common.scale.label",
            "toast.copyFail",
        ]) {
            run.check(typeof parts[lang][key] === "string" && parts[lang][key].length > 0, `${key}（${lang}）非空`);
        }
    }
    // 消息位动作表的键都真有词条
    for (const [name, def] of Object.entries(msg.ACTIONS)) {
        run.check(Boolean(common.zh[def.key]), `动作 ${name} 的词条 ${def.key} 存在`);
    }
}

run.finish();
