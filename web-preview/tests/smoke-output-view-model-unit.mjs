// SPDX-License-Identifier: GPL-3.0-or-later
//
// Output 视图模型的纯函数单测（Node，无 DOM）：web/output/view-model.js 把桥的两路快照整理成页面入参。
//   ① 快照校验：非对象、乱序（seq 不比已收到的大）整帧丢弃；轨道名清洗；颜色只认白名单写法；声道、核验、通知的白名单；
//   ② 配对状态 → 副行 / 页脚提示 / 发送行 / 危险描边 的 15 态全表；
//   ③ 消息推导：14 条各自可达、同时成立时的优先顺序、占位符取值；
//   ④ 词条：上面用到的键三语齐全，带上取值后没有残留的 {x}；
//   ⑤ 坏样例：把规则故意写错的替身（跳过 seq 比较、颜色不过滤、优先级反了）必须被同一组断言抓出来。
//
// 用法：node web-preview/tests/smoke-output-view-model-unit.mjs [仓库根]

import { pathToFileURL } from "node:url";
import path from "node:path";
import { createRun, repoRoot } from "./lib/harness.mjs";

const ROOT = repoRoot();
const run = createRun("Output 视图模型单测");
const load = (rel) => import(pathToFileURL(path.join(ROOT, rel)).href);

const vm = await load("web/output/view-model.js");
const i18n = await load("web/shared/i18n.js");
const dom = await load("web/shared/dom.js");
const pairStatus = await load("web/shared/pair-status.js");
const common = (await load("web/i18n/common.js")).default;
const output = (await load("web/i18n/output.js")).default;
i18n.installDicts(common, output);

const ALL_PAIR = pairStatus.statesFor("output");

/** 造一帧 pair / state：覆盖项逐层合并。 */
const mkState = (over = {}) => ({
    ...vm.defaultState(),
    seq: 1,
    version: "0.9.0",
    group: "A",
    sampleRate: 48000,
    ...over,
    send: { ...vm.defaultState().send, sampleRate: 48000, ...(over.send || {}) },
});
const mkPair = (state, over = {}) => ({
    ...vm.defaultPair(),
    seq: 1,
    state,
    ...over,
    peer: { ...vm.defaultPair().peer, ...(over.peer || {}) },
    self: { ...vm.defaultPair().self, ...(over.self || {}) },
});

// ---------------------------------------------------------------- ① 快照校验

run.eq(vm.readState(null, null), null, "readState：null 整帧丢弃");
run.eq(vm.readState("x", null), null, "readState：非对象整帧丢弃");
run.eq(vm.readPair(undefined, null), null, "readPair：undefined 整帧丢弃");
run.eq(vm.readPair({ seq: 1, state: "madeUp" }, null), null, "readPair：未知配对状态整帧丢弃，不猜");

{
    const first = vm.readState({ seq: 5, version: "0.9.0", group: "C" }, null);
    run.eq(first && first.group, "C", "readState：首帧收下");
    run.eq(vm.readState({ seq: 5, group: "D" }, first), null, "readState：seq 相等丢弃（重复帧）");
    run.eq(vm.readState({ seq: 4, group: "D" }, first), null, "readState：seq 更小丢弃（乱序的旧帧）");
    const next = vm.readState({ seq: 6, group: "D" }, first);
    run.eq(next && next.group, "D", "readState：seq 更大收下");
    run.eq(next && next.version, "0.9.0", "readState：缺省的字段沿用上一帧，不回落成空");
}
{
    const first = vm.readPair({ seq: 3, state: "connected" }, null);
    run.eq(vm.readPair({ seq: 3, state: "stale" }, first), null, "readPair：seq 相等丢弃");
    run.eq(vm.readPair({ seq: 2, state: "stale" }, first), null, "readPair：seq 更小丢弃");
    run.eq((vm.readPair({ seq: 4, state: "stale" }, first) || {}).state, "stale", "readPair：seq 更大收下");
}

{
    const s = vm.readState(
        {
            seq: 1,
            lang: "de",
            group: "Z",
            groupOrigin: "wild",
            takeover: "yes",
            uiScale: -2,
            sampleRate: "48000",
            send: { channels: "weird", bypassed: 1, hostPaused: "true" },
            notices: [
                { id: "ipcSandbox" },
                { id: "ipcSandbox" },
                { id: "notAThing" },
                null,
                { id: "newerProject", a: "0.11.0", b: "0.9.0" },
            ],
            build: { sha: "a1b2c3d4e5f6a7b8c9d0e1f2", fmt: "VST3", arch: "x\u0007y" },
        },
        null,
    );
    run.eq(s.lang, "zh", "readState：不认识的语言回落中文");
    run.eq(s.group, null, "readState：不在 A–H 的组当没有");
    run.eq(s.groupOrigin, "manual", "readState：不认识的 groupOrigin 回落 manual");
    run.eq(s.takeover, false, "readState：takeover 只认布尔 true（字符串 yes 不算）");
    run.eq(s.uiScale, 1, "readState：负的缩放回落 1");
    run.eq(s.sampleRate, 0, "readState：字符串采样率不收");
    run.eq(
        s.send,
        { channels: "ST", sampleRate: 0, bypassed: false, hostPaused: false },
        "readState：send 字段只认合法类型",
    );
    run.eq(
        s.notices.map((n) => n.id),
        ["ipcSandbox", "newerProject"],
        "readState：notices 去重、去掉未知 id 与空项",
    );
    run.eq(s.build.sha.length <= 16, true, "readState：build.sha 截断到 16 字符");
    run.eq(/[\u0000-\u001f]/.test(s.build.arch), false, "readState：build 里的控制字符被清掉");
}
run.eq(
    vm.readState({ seq: 1, send: { channels: "MONO" } }, null).send.channels,
    "MONO",
    "readState：send.channels 'MONO' 认作单声道",
);
run.eq(
    vm.readState({ seq: 1, send: { channels: 1 } }, null).send.channels,
    "MONO",
    "readState：send.channels 1 认作单声道",
);
run.eq(
    vm.readState({ seq: 1, send: { channels: 2 } }, null).send.channels,
    "ST",
    "readState：send.channels 2 认作立体声",
);

{
    const p = vm.readPair(
        {
            seq: 1,
            state: "connected",
            groupMask: 0x1ff,
            peer: {
                name: "  Vox\u0000 \n Lead\t",
                color: "red; background:url(x)",
                channels: "ST",
                version: "0.9.0",
                sampleRate: 44100,
            },
            self: { name: "x".repeat(300), color: "rgb(45,121,198)" },
            verify: "maybe",
        },
        null,
    );
    run.eq(p.peer.name, "Vox Lead", "readPair：轨道名去控制字符、折叠空白");
    run.eq(p.peer.color, null, "readPair：带分号与 url 的颜色当没有（颜色只写进点的 background）");
    run.eq(p.self.color, "rgb(45,121,198)", "readPair：rgb() 数字写法收下");
    run.eq(p.self.name.length, 200, "readPair：超长轨道名截断到 200 字符");
    run.eq(p.groupMask, 0xff, "readPair：groupMask 只留低 8 位");
    run.eq(p.verify, "unknown", "readPair：不认识的核验结果回落 unknown");
    run.eq(p.peer.channels, "st", "readPair：peer.channels 'ST' 认作立体声");
    run.eq(p.peer.sampleRate, 44100, "readPair：peer.sampleRate 收下");
}
for (const [raw, want] of [
    [1, "mono"],
    [2, "st"],
    ["MONO", "mono"],
    ["mono", "mono"],
    ["ST", "st"],
    [3, null],
    ["quad", null],
    [null, null],
]) {
    run.eq(vm.channelKind(raw), want, `channelKind(${JSON.stringify(raw)})`);
}
for (const [raw, want] of [
    ["#abc", "#abc"],
    ["#a1b2c3", "#a1b2c3"],
    ["rgb(1, 2, 3)", "rgb(1, 2, 3)"],
    ["rgba(1,2,3,0.5)", "rgba(1,2,3,0.5)"],
    ["#abcd", null],
    ["red", null],
    ["rgb(1,2,3); x:y", null],
    ["url(javascript:alert(1))", null],
    [12, null],
]) {
    run.eq(vm.cleanColor(raw), want, `cleanColor(${JSON.stringify(raw)})`);
}

// ---------------------------------------------------------------- ② 15 态全表

const TABLE = {
    // 状态: [副行, 页脚提示, 发送行, 危险描边]
    ipcUnavailable: ["notice", "unpaired", "none", false],
    abiMismatch: ["notice", "unpaired", "none", true],
    srMismatch: ["notice", "unpaired", "none", false],
    conflict: ["notice", "conflict", "none", true],
    unassigned: ["notice", "unpaired", "none", false],
    stale: ["stale", "connected", "ok", false],
    crossProcess: ["notice", "connected", "ok", false],
    crossTrack: ["notice", "connected", "ok", false],
    searching: ["searching", "unpaired", "none", false],
    waitingAudio: ["notice", "unpaired", "idle", false],
    validator: ["notice", "unpaired", "none", false],
    bypassed: ["bypassed", "connected", "bypassed", false],
    connectedUnverified: ["connectedUnverified", "connected", "ok", false],
    connected: ["connected", "connected", "ok", false],
    connectedStill: ["inputMuted", "connected", "ok", false],
};
run.eq(Object.keys(TABLE).sort(), [...ALL_PAIR].sort(), "全表覆盖 Output 侧的 15 个配对状态（防漏态）");
for (const [st, [sub, hint, send, danger]] of Object.entries(TABLE)) {
    const v = vm.deriveView(mkState(), mkPair(st));
    run.eq(v.pair.sub, sub, `${st}：副行`);
    run.eq(v.hint, hint, `${st}：页脚提示`);
    run.eq(v.send.kind, send, `${st}：发送行`);
    run.eq(v.danger, danger, `${st}：危险描边（只有冲突与版本不匹配）`);
}
{
    const v = vm.deriveView(mkState({ send: { bypassed: true } }), mkPair("connected"));
    run.eq([v.send.kind, v.hostBypass], ["bypassed", true], "send.bypassed 优先于配对状态：发送行旁路、外壳旁路态");
    run.eq(
        vm.deriveView(mkState({ send: { hostPaused: true } }), mkPair("connected")).send.kind,
        "idle",
        "宿主暂停：暂停写入",
    );
    run.eq(vm.deriveView(mkState({ send: { channels: "MONO" } }), mkPair("connected")).send.ch, "MONO", "单声道发送");
    run.eq(
        vm.deriveView(mkState({ send: { sampleRate: 44100 } }), mkPair("connected")).send.sr,
        "44.1",
        "采样率写成 kHz，保留一位小数",
    );
    run.eq(vm.srLabel(48000), "48", "srLabel(48000)");
    run.eq(vm.srLabel(0), "—", "srLabel(0) 写破折号");
}
{
    const searching = vm.deriveView(mkState(), mkPair("searching"));
    run.eq(searching.tracks.input.mode, "chain", "searching 且没有对端名：第二行画插入链");
    run.eq(
        vm.deriveView(mkState(), mkPair("searching", { peer: { name: "Gtr" } })).tracks.input.mode,
        "name",
        "searching 但有对端名：按名字画，不画插入链",
    );
    run.eq(
        vm.deriveView(mkState(), mkPair("conflict")).tracks.input.mode,
        "noName",
        "非 searching 且没有对端名：画「未提供轨道名」",
    );
    run.eq(vm.deriveView(mkState(), mkPair("searching")).tracks.verify, null, "没有对端：不带核验徽标");
    run.eq(
        vm.deriveView(mkState(), mkPair("connected", { verify: "match" })).tracks.verify,
        "match",
        "有对端：带核验结果",
    );
    run.eq(
        vm.deriveView(mkState({ groupOrigin: "auto" }), mkPair("searching")).group.auto,
        "fallback",
        "searching 时自动组是「兜底」写法",
    );
    run.eq(
        vm.deriveView(mkState({ groupOrigin: "auto" }), mkPair("connected")).group.auto,
        true,
        "已连接时自动组是「自动」",
    );
    run.eq(
        vm.deriveView(mkState({ groupOrigin: "manual" }), mkPair("connected")).group.auto,
        false,
        "手动选的组没有自动徽标",
    );
    run.eq(
        vm.deriveView(mkState({ takeover: true }), mkPair("searching")).group.disabled,
        true,
        "确认条展开：组胶囊禁用",
    );
    run.eq(
        vm.deriveView(mkState({ takeover: true, group: "B" }), mkPair("searching")).confirm,
        { key: "band.out.takeover", g: "B" },
        "接管确认条带组字母",
    );
    run.eq(vm.deriveView(mkState(), mkPair("unassigned")).group.unassigned, true, "unassigned 态：组选择整排不选");
}

// ---------------------------------------------------------------- ③ 消息推导

/** 14 条各自可达的构造：[状态, 配对状态, 配对覆盖]。 */
const REACH = {
    conflict: [{}, "conflict", {}],
    unassigned: [{ group: null }, "unassigned", {}],
    abi: [{}, "abiMismatch", { peer: { version: "0.10.12" } }],
    srMismatch: [{}, "srMismatch", { peer: { sampleRate: 44100 } }],
    ipc: [{}, "ipcUnavailable", {}],
    ipcSandbox: [{ notices: [{ id: "ipcSandbox", a: null, b: null }] }, "searching", {}],
    newerProject: [{ notices: [{ id: "newerProject", a: "0.11.0", b: "0.9.0" }] }, "searching", {}],
    stateDecodeFailed: [{ notices: [{ id: "stateDecodeFailed", a: null, b: null }] }, "searching", {}],
    stale: [{}, "stale", {}],
    crossProcess: [{}, "crossProcess", {}],
    crossTrack: [{}, "crossTrack", {}],
    outOfOrder: [{ notices: [{ id: "outOfOrder", a: null, b: null }] }, "searching", {}],
    waitingAudio: [{}, "waitingAudio", {}],
    validator: [{}, "validator", {}],
};
run.eq(
    Object.keys(REACH),
    vm.MESSAGES.map((m) => m.id),
    "REACH 覆盖消息表的 14 条，顺序就是优先级（防漏条）",
);
for (const [id, [st, ps, over]] of Object.entries(REACH)) {
    const msgs = vm.messagesFor(mkState(st), mkPair(ps, over));
    run.eq(
        msgs.map((m) => m.id),
        [id],
        `${id}：单独成立时只出这一条`,
    );
}
run.eq(
    vm.messagesFor(mkState(), mkPair("connected", { verify: "diff", peer: { name: "A" } })).map((m) => m.id),
    ["crossTrack"],
    "已连接但核验结果「不同」：也出跨轨消息",
);
run.eq(
    vm.messagesFor(mkState(), mkPair("searching", { verify: "diff" })).map((m) => m.id),
    [],
    "没有对端时核验结果不触发跨轨消息",
);
run.eq(
    vm
        .messagesFor(mkState({ notices: [{ id: "ipcSandbox", a: null, b: null }] }), mkPair("ipcUnavailable"))
        .map((m) => m.id),
    ["ipcSandbox"],
    "共享内存不可用且有沙盒通知：只报沙盒那条，不叠泛泛的 ipc",
);
{
    // 多条同时成立：顺序就是表序（红 > 琥珀 > 灰，同色按表序）
    const many = vm.messagesFor(
        mkState({
            notices: [
                { id: "outOfOrder", a: null, b: null },
                { id: "newerProject", a: "0.11.0", b: "0.9.0" },
                { id: "ipcSandbox", a: null, b: null },
            ],
        }),
        mkPair("abiMismatch", { verify: "diff", peer: { name: "A", version: "0.10.0" } }),
    );
    run.eq(
        many.map((m) => m.id),
        ["abi", "ipcSandbox", "newerProject", "crossTrack", "outOfOrder"],
        "多条同时成立：abi、ipcSandbox（红）在前，newerProject、crossTrack、outOfOrder（琥珀）在后",
    );
}
run.eq(vm.messagesFor(mkState({ group: "C" }), mkPair("conflict"))[0].vals, { g: "C" }, "conflict 的 {g} 取当前组");
run.eq(
    vm.messagesFor(mkState({ version: "0.9.0" }), mkPair("abiMismatch", { peer: { version: "0.10.12" } }))[0].vals,
    { a: "0.9.0", b: "0.10.12" },
    "abi 的 {a} 是本端版本、{b} 是对端版本",
);
run.eq(
    vm.messagesFor(mkState({ sampleRate: 48000 }), mkPair("srMismatch", { peer: { sampleRate: 44100 } }))[0].vals,
    { a: "48000", b: "44100" },
    "srMismatch 的 {a}{b} 是两端采样率（Hz）",
);
run.eq(
    vm.messagesFor(mkState(), mkPair("srMismatch"))[0].vals,
    { a: "48000", b: "—" },
    "对端采样率缺失：写破折号，不让 {b} 原样上屏",
);
run.eq(
    vm.messagesFor(mkState({ notices: [{ id: "newerProject", a: null, b: null }] }), mkPair("searching"))[0].vals,
    { a: "—", b: "—" },
    "newerProject 缺版本：写破折号",
);
run.eq(vm.messagesFor(mkState({ group: null }), mkPair("conflict"))[0].vals, { g: "—" }, "conflict 缺组号：写破折号");
run.eq(
    vm.messagesFor(mkState({ version: "" }), mkPair("abiMismatch", { peer: { version: null } }))[0].vals,
    { a: "—", b: "—" },
    "abi 缺本端与对端版本：写破折号",
);
run.eq(
    vm.messagesFor(mkState({ version: "0.9.0" }), mkPair("abiMismatch", { peer: { version: null } }))[0].vals,
    { a: "0.9.0", b: "—" },
    "abi 只缺对端版本：只有那一格写破折号",
);
run.eq(
    vm.deriveView(mkState({ takeover: true, group: null }), mkPair("searching")).confirm,
    { key: "band.out.takeover", g: "—" },
    "接管确认条缺组号：写破折号",
);
{
    // 缺省占位统一：三语下都不留原样的 {x}，也不出现问号占位
    for (const lang of ["zh", "en", "fr"]) {
        const t = dom.makeT(lang);
        const miss = [
            ["band.out.conflict", vm.messagesFor(mkState({ group: null }), mkPair("conflict"))[0]],
            [
                "band.out.abi",
                vm.messagesFor(mkState({ version: "" }), mkPair("abiMismatch", { peer: { version: null } }))[0],
            ],
            ["band.out.srMismatch", vm.messagesFor(mkState(), mkPair("srMismatch"))[0]],
            [
                "band.out.newerProject",
                vm.messagesFor(
                    mkState({ notices: [{ id: "newerProject", a: null, b: null }] }),
                    mkPair("searching"),
                )[0],
            ],
        ];
        for (const [key, msg] of miss) {
            const s = t(msg.key, msg.vals);
            run.check(!/{w+}/.test(s) && !s.includes("?"), `${lang} / ${key}：缺值时没有残留占位符与问号：${s}`);
        }
    }
}
run.eq(
    vm.messagesFor(mkState(), mkPair("conflict")).map((m) => m.action && m.action.name),
    ["repair"],
    "conflict 的动作是 repair",
);
for (const id of ["abi", "ipc", "ipcSandbox", "crossProcess"]) {
    const [st, ps, over] = REACH[id];
    run.eq(vm.messagesFor(mkState(st), mkPair(ps, over))[0].action, { name: "guide" }, `${id} 的动作是 guide`);
}
run.eq(
    vm.MESSAGES.filter((m) => m.action).map((m) => m.id),
    ["conflict", "abi", "ipc", "ipcSandbox", "crossProcess"],
    "带动作按钮的恰好这五条",
);
run.eq(
    vm.MESSAGES.map((m) => m.tone),
    ["red", "red", "red", "red", "red", "red", "amber", "amber", "amber", "amber", "amber", "amber", "gray", "gray"],
    "颜色表：红六条、琥珀六条、灰两条",
);

// ---------------------------------------------------------------- ④ 词条

const placeholdersLeft = (s) => s.match(/\{\w+\}/g) || [];
for (const lang of ["zh", "en", "fr"]) {
    const t = dom.makeT(lang);
    const bare = [];
    const leaks = [];
    const check = (key, vals) => {
        const s = t(key, vals);
        if (s === key) bare.push(key);
        if (placeholdersLeft(s).length) leaks.push(`${key} → ${s}`);
    };
    for (const k of Object.values(vm.SUB_KEYS)) check(k);
    for (const k of Object.values(vm.HINT_KEYS)) check(k);
    check(vm.IDLE_KEY);
    check("out.place");
    check("out.pair.eyebrow");
    check("out.pair.thisTrack");
    check("out.pair.inputTrack");
    check("out.pair.noName");
    check("out.pair.searching");
    check("out.pair.bypassed");
    for (const k of Object.values(vm.SEND_KEYS)) check(k, { ch: t("common.ch.st"), sr: "48" });
    for (const [id, [st, ps, over]] of Object.entries(REACH)) {
        const m = vm.messagesFor(mkState(st), mkPair(ps, over))[0];
        check(m.key, m.vals);
        run.check(t(m.key, m.vals).trim().length > 0, `${lang}：${id} 有文字`);
    }
    check(vm.TAKEOVER_KEY, { g: "A" });
    run.eq(bare, [], `${lang}：这些键在词典里没有（上屏会是裸键）`);
    run.eq(leaks, [], `${lang}：带取值后仍有残留占位符`);
}

// ---------------------------------------------------------------- ⑤ 坏样例：把规则写错的替身必须被抓出来

/** 用一组断言去量一个被测实现；返回失败数。 */
function failuresOf(impl) {
    let bad = 0;
    const eq = (a, b) => {
        if (JSON.stringify(a) !== JSON.stringify(b)) bad++;
    };
    const first = impl.readState({ seq: 5, group: "C" }, null);
    eq(impl.readState({ seq: 5, group: "D" }, first), null);
    eq(impl.readState({ seq: 4, group: "D" }, first), null);
    eq(impl.cleanColor("red; background:url(x)"), null);
    eq(
        impl
            .messagesFor(mkState(), mkPair("abiMismatch", { verify: "diff", peer: { name: "A", version: "1" } }))
            .map((m) => m.id),
        ["abi", "crossTrack"],
    );
    return bad;
}
run.eq(failuresOf(vm), 0, "基线：真实现通过坏样例用的断言");
run.check(
    failuresOf({ ...vm, readState: (raw, prev) => vm.readState(raw, null) }) > 0,
    "坏样例：readState 不比 seq（旧帧也收）应被抓出来",
);
run.check(
    failuresOf({ ...vm, cleanColor: (v) => (typeof v === "string" ? v : null) }) > 0,
    "坏样例：颜色不过滤应被抓出来",
);
run.check(
    failuresOf({
        ...vm,
        messagesFor: (s, p) => vm.messagesFor(s, p).reverse(),
    }) > 0,
    "坏样例：消息优先级反了应被抓出来",
);

run.finish();
