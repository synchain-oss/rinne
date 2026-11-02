// SPDX-License-Identifier: GPL-3.0-or-later
//
// 首启链的纯函数与状态机单测（Node，无 DOM）：web/shared/first-run.js、rules-page.js 的放行判据、tour-paint.js 的摆放判据。
//   ① 链的顺序：没选过语言、没确认过规则、没看过导览时是 语言卡 → 规则页 → 导览；缺哪环走哪环，都不缺整条链不出；
//      规则页后面还接导览用 first 按钮栏，否则 ack；Output 在 Input 的导览看过时不弹自己的导览；
//   ② 恢复的实例（restored）不弹链，页脚换成 hint.firstUseRestored，确认了当前版本之后不再换；
//   ③ 规则改版重弹：确认过的版本小于当前版本就再弹规则页（导览看过就只弹规则页），版本相等不弹；
//   ④ 首启进行中注入 L2 或失控灯亮，叠层立即关、不写位，本会话不再自动弹；开局就在危险态也不弹，危险解除后不补弹；
//      用户自己从「?」打开的规则页与接着重看的导览是普通模态，危险态不自动关，紧急切断照常关；
//   ⑤ 写位：选语言 setLang，[开始导览] setRulesAck 后进导览，导览结束 setGuideSeen(true)；[跳过导览] 两位都写；
//      [我已了解] 只写 setRulesAck；重看（「?」→ [继续导览]）不写任何位；Input 写入闸不拦这三个写位；
//   ⑥ Esc 与放行：重看随时可以 Esc，要确认的只在环路激活时可以，Esc 不算确认；按钮在读到底或打开满 8 秒时放行，重看不设闸；
//   ⑦ 说明框摆放：首选位置合格就用，压到亮区、越出设计盒、压到紧急切断时换方位，全都不合格时报 ok 为假；
//      导览步数 Input 6、Output 4，每步的标题与正文三语齐全，规则页按钮的词条三语齐全；
//   坏样例：把判据故意写错的替身（忽略 restored、忽略改版、L2 不关、用户打开的规则页也自动关、重看也写位、要确认的规则页随时可 Esc、
//      压到亮区也算合格）必须被同一组断言抓出来。
//
// 用法：node web-preview/tests/smoke-first-run-unit.mjs [仓库根]

import { pathToFileURL } from "node:url";
import path from "node:path";
import { createRun, repoRoot } from "./lib/harness.mjs";

const ROOT = repoRoot();
const run = createRun("首启链单测");
const load = (rel) => import(pathToFileURL(path.join(ROOT, rel)).href);

const fr = await load("web/shared/first-run.js");
const rp = await load("web/shared/rules-page.js");
const tp = await load("web/shared/tour-paint.js");
const gate = await load("web/input/bridge-gate.js");
const { TOUR_IN_STEPS } = await load("web/input/tour-in.js");
const { TOUR_OUT_STEPS } = await load("web/output/tour-out.js");
const { BUTTON_KEYS } = await load("web/shared/common-keys.js");
const common = (await load("web/i18n/common.js")).default;
const guide = (await load("web/i18n/guide.js")).default;

const V = fr.RULES_VERSION;
/** 全新的本机：什么都没做过。 */
const FRESH = Object.freeze({
    langChosen: false,
    rulesAck: 0,
    guideSeen: false,
    inputGuideSeen: false,
    restored: false,
});
const snapOf = (over = {}) => ({ ...FRESH, ...over });

/** 记下状态机写了哪些位。 */
function recorder() {
    const calls = [];
    return { calls, write: (name, arg) => calls.push([name, arg]) };
}

// ---------------------------------------------------------------- 各组断言：impl 是被测的一套实现，expect 记一条
// 实现用同一组断言跑两遍：真实现必须全过，坏样例替身必须至少挂一条。

function groupOrder(impl, expect) {
    const steps = (role, over, closed = false) => impl.firstRunSteps(role, snapOf(over), closed, V);
    expect(JSON.stringify(steps("input", {})) === '["lang","rules","tour"]', "全新：语言卡 → 规则页 → 导览");
    expect(JSON.stringify(steps("output", {})) === '["lang","rules","tour"]', "Output 全新：同样三环");
    expect(JSON.stringify(steps("input", { langChosen: true })) === '["rules","tour"]', "选过语言：不出语言卡");
    expect(
        JSON.stringify(steps("input", { langChosen: true, rulesAck: V })) === '["tour"]',
        "确认过当前版本：只剩导览",
    );
    expect(
        JSON.stringify(steps("input", { langChosen: true, rulesAck: V, guideSeen: true })) === "[]",
        "全都做过：整条链不出",
    );
    expect(
        JSON.stringify(steps("input", { rulesAck: V, guideSeen: true })) === "[]",
        "没选过语言但两样都不需要：语言卡也不出",
    );
    expect(
        JSON.stringify(steps("output", { langChosen: true, inputGuideSeen: true })) === '["rules"]',
        "Output：Input 的导览看过就跳过自己的导览",
    );
    expect(
        JSON.stringify(steps("input", { langChosen: true, inputGuideSeen: true })) === '["rules","tour"]',
        "Input 不看 inputGuideSeen（那是给 Output 的）",
    );
    expect(JSON.stringify(steps("input", {}, true)) === "[]", "本会话关过：不再弹");
    expect(impl.firstRunSteps("input", null, false, V).length === 0, "快照没到：不弹");
    expect(impl.chainRulesVariant(["lang", "rules", "tour"]) === "first", "后面接导览：first 按钮栏");
    expect(impl.chainRulesVariant(["rules"]) === "ack", "后面不接导览：ack 按钮栏");
    // 状态机按同样的顺序走
    const r = recorder();
    const m = impl.createFirstRunMachine({ role: "input", write: r.write, version: V });
    expect(m.begin(snapOf(), {}) === true, "全新：begin 开链");
    expect(m.state().phase === "lang", "第一环是语言卡");
    m.pickLang("en");
    expect(m.state().phase === "rules" && m.state().variant === "first", "选完语言进规则页（first）");
    m.rulesAction("start");
    expect(m.state().phase === "tour", "[开始导览] 进导览");
    m.tourEnd();
    expect(m.state().phase === "idle" && m.state().closedThisSession === true, "导览结束：链关掉，本会话关过");
    expect(m.begin(snapOf(), {}) === false, "begin 只判一次");
}

function groupRestored(impl, expect) {
    expect(impl.firstRunSteps("input", snapOf({ restored: true }), false, V).length === 0, "restored：Input 不弹");
    expect(impl.firstRunSteps("output", snapOf({ restored: true }), false, V).length === 0, "restored：Output 不弹");
    expect(impl.needsRestoredHint(snapOf({ restored: true }), V) === true, "restored 而没确认当前版本：页脚换提示");
    expect(impl.needsRestoredHint(snapOf({ restored: true, rulesAck: V }), V) === false, "确认了当前版本：不再换提示");
    expect(impl.needsRestoredHint(snapOf({ restored: false }), V) === false, "不是恢复的实例：不换提示");
    expect(impl.needsRestoredHint(null, V) === false, "快照没到：不换提示");
    const r = recorder();
    const m = impl.createFirstRunMachine({ role: "input", write: r.write, version: V });
    expect(m.begin(snapOf({ restored: true }), {}) === false && m.state().phase === "idle", "restored：状态机不开链");
    expect(r.calls.length === 0, "restored：不写任何位");
    // 从「?」打开规则页：没确认当前版本是 ack，确认后写 setRulesAck
    m.reopen(snapOf({ restored: true }));
    expect(m.state().phase === "rules" && m.state().variant === "ack", "restored 从「?」进规则页：要确认（ack）");
    m.rulesAction("ack");
    expect(JSON.stringify(r.calls) === JSON.stringify([["setRulesAck", V]]), "确认：只写 setRulesAck(当前版本)");
}

function groupVersion(impl, expect) {
    const seen = { langChosen: true, guideSeen: true };
    expect(
        JSON.stringify(impl.firstRunSteps("input", snapOf({ ...seen, rulesAck: V - 1 }), false, V)) === '["rules"]',
        "确认过旧版本：重弹规则页（导览看过就只弹规则页）",
    );
    expect(
        JSON.stringify(impl.firstRunSteps("input", snapOf({ ...seen, rulesAck: V }), false, V)) === "[]",
        "确认过当前版本：不弹",
    );
    // 规则改版：版本加 1 后，确认过上一版的本机重弹
    const next = V + 1;
    expect(
        JSON.stringify(impl.firstRunSteps("input", snapOf({ ...seen, rulesAck: V }), false, next)) === '["rules"]',
        "版本加 1：确认过上一版的重弹",
    );
    const r = recorder();
    const m = impl.createFirstRunMachine({ role: "output", write: r.write, version: next });
    expect(m.begin(snapOf({ ...seen, rulesAck: V }), {}) === true, "改版：状态机开链");
    expect(m.state().phase === "rules" && m.state().variant === "ack", "改版：直接进规则页，按钮栏是 ack");
    m.rulesAction("ack");
    expect(JSON.stringify(r.calls) === JSON.stringify([["setRulesAck", next]]), "改版确认：写的是新版本号");
    expect(impl.reopenRulesVariant(snapOf({ rulesAck: V }), V) === "review", "确认过当前版本再打开：重看");
    expect(impl.reopenRulesVariant(snapOf({ rulesAck: V }), next) === "ack", "确认过旧版本再打开：要确认");
}

function groupDanger(impl, expect) {
    expect(impl.mustClose({ l2: true }) === true, "L2：必须关");
    expect(impl.mustClose({ guardLit: true }) === true, "失控灯亮：必须关");
    expect(impl.mustClose({ l2: false, guardLit: false }) === false, "都没有：不关");
    for (const phase of ["lang", "rules", "tour"]) {
        const r = recorder();
        const m = impl.createFirstRunMachine({ role: "input", write: r.write, version: V });
        m.begin(snapOf(), {});
        if (phase !== "lang") m.pickLang("zh");
        if (phase === "tour") m.rulesAction("start");
        expect(m.state().phase === phase, `走到 ${phase}`);
        const before = r.calls.length;
        expect(m.frame({ l2: false, guardLit: false }) === false, `${phase}：安全帧不关`);
        expect(m.frame({ l2: true }) === true, `${phase}：注入 L2 叠层立即关`);
        expect(m.state().phase === "idle", `${phase}：关后没有叠层`);
        expect(r.calls.length === before, `${phase}：L2 关掉不写位`);
        expect(m.begin(snapOf(), {}) === false, `${phase}：本会话不再自动弹`);
    }
    {
        const r = recorder();
        const m = impl.createFirstRunMachine({ role: "input", write: r.write, version: V });
        m.begin(snapOf(), {});
        m.pickLang("fr");
        m.rulesAction("start");
        const before = r.calls.length;
        expect(m.frame({ guardLit: true }) === true && m.state().phase === "idle", "导览中失控灯亮：立即关");
        expect(r.calls.length === before, "失控灯关掉不写位");
    }
    {
        // 紧急切断：abort 立即关，不写位
        const r = recorder();
        const m = impl.createFirstRunMachine({ role: "input", write: r.write, version: V });
        m.begin(snapOf(), {});
        m.abort();
        expect(m.state().phase === "idle" && r.calls.length === 0, "紧急切断：立即关，不写位");
    }
    {
        // 开局就在危险态：不弹，危险解除后也不补弹
        const r = recorder();
        const m = impl.createFirstRunMachine({ role: "input", write: r.write, version: V });
        expect(m.begin(snapOf(), { l2: true }) === false, "开局 L2：不弹");
        expect(m.begin(snapOf(), {}) === false, "危险解除后不补弹");
        // 用户自己从「?」打开的规则页与接着重看的导览是普通模态：危险态不自动关，紧急切断照常关
        const done = snapOf({ langChosen: true, rulesAck: V, guideSeen: true });
        m.reopen(done);
        expect(m.frame({ l2: true }) === false && m.state().phase === "rules", "「?」打开的规则页：L2 不自动关");
        m.rulesAction("continue");
        expect(m.frame({ guardLit: true }) === false && m.state().phase === "tour", "重看的导览：失控灯亮不自动关");
        m.abort();
        expect(m.state().phase === "idle" && r.calls.length === 0, "重看中按紧急切断：关掉，不写位");
    }
}

function groupWrites(impl, expect, gated) {
    {
        const r = recorder();
        const m = impl.createFirstRunMachine({ role: "input", write: r.write, version: V });
        m.begin(snapOf(), {});
        m.pickLang("fr");
        m.rulesAction("start");
        m.tourEnd();
        expect(
            JSON.stringify(r.calls) ===
                JSON.stringify([
                    ["setLang", "fr"],
                    ["setRulesAck", V],
                    ["setGuideSeen", true],
                ]),
            "完整走完：setLang → setRulesAck → setGuideSeen",
        );
    }
    {
        const r = recorder();
        const m = impl.createFirstRunMachine({ role: "input", write: r.write, version: V });
        m.begin(snapOf({ langChosen: true }), {});
        m.rulesAction("skip");
        expect(
            JSON.stringify(r.calls) ===
                JSON.stringify([
                    ["setRulesAck", V],
                    ["setGuideSeen", true],
                ]),
            "[跳过导览]：setRulesAck 再 setGuideSeen(true)",
        );
        expect(m.state().phase === "idle", "[跳过导览]：链关掉");
    }
    {
        // 放错变体的按钮不响应（first 上按 ack、review 的按钮）
        const r = recorder();
        const m = impl.createFirstRunMachine({ role: "input", write: r.write, version: V });
        m.begin(snapOf({ langChosen: true }), {});
        m.rulesAction("ack");
        m.rulesAction("close");
        m.rulesAction("continue");
        expect(r.calls.length === 0 && m.state().phase === "rules", "first 按钮栏不响应别的变体的按钮");
    }
    {
        // 重看：「?」→ [继续导览] → 导览结束，不写任何位
        const r = recorder();
        const m = impl.createFirstRunMachine({ role: "output", write: r.write, version: V });
        m.begin(snapOf({ langChosen: true, rulesAck: V, guideSeen: true }), {});
        m.reopen(snapOf({ langChosen: true, rulesAck: V, guideSeen: true }));
        expect(m.state().variant === "review", "确认过当前版本：重看");
        m.rulesAction("continue");
        expect(m.state().phase === "tour" && m.state().replay === true, "[继续导览] 进导览（重看）");
        m.tourEnd();
        expect(r.calls.length === 0, "重看：导览结束不写任何位");
        m.reopen(snapOf({ rulesAck: V }));
        m.rulesAction("close");
        expect(r.calls.length === 0 && m.state().phase === "idle", "重看 [关闭]：不写位");
    }
    {
        // 导览按 Esc、跳过都算看过
        const r = recorder();
        const m = impl.createFirstRunMachine({ role: "input", write: r.write, version: V });
        m.begin(snapOf({ langChosen: true, rulesAck: V }), {});
        expect(m.state().phase === "tour", "只剩导览：直接进导览");
        m.tourEnd("esc");
        expect(JSON.stringify(r.calls) === JSON.stringify([["setGuideSeen", true]]), "导览中 Esc：setGuideSeen(true)");
    }
    for (const name of ["setLang", "setRulesAck", "setGuideSeen"])
        expect(!gated.includes(name), `Input 写入闸不拦 ${name}`);
}

function groupEsc(impl, expect) {
    expect(impl.rulesEscapable("review", false) === true, "重看：随时可以 Esc");
    expect(impl.rulesEscapable("first", false) === false, "要确认（first）、环路没激活：不能 Esc");
    expect(impl.rulesEscapable("ack", false) === false, "要确认（ack）、环路没激活：不能 Esc");
    expect(impl.rulesEscapable("first", true) === true, "要确认、环路激活：可以 Esc");
    {
        const r = recorder();
        const m = impl.createFirstRunMachine({ role: "input", write: r.write, version: V });
        m.begin(snapOf({ langChosen: true }), {});
        expect(m.escape(false) === false && m.state().phase === "rules", "环路没激活：Esc 不关规则页");
        expect(m.escape(true) === true && m.state().phase === "idle", "环路激活：Esc 关掉规则页");
        expect(r.calls.length === 0, "Esc 不算确认，不写位");
    }
    {
        const r = recorder();
        const m = impl.createFirstRunMachine({ role: "input", write: r.write, version: V });
        m.begin(snapOf(), {});
        expect(m.escape(true) === false && m.state().phase === "lang", "语言卡不响应 Esc");
    }
    const open = impl.rulesGateOpen;
    expect(open("first", { elapsedMs: 0 }) === false, "刚打开：按钮不放行");
    expect(open("ack", { elapsedMs: impl.RULES_GATE_MS - 1 }) === false, "差 1 ms 满 8 秒：不放行");
    expect(open("first", { elapsedMs: impl.RULES_GATE_MS }) === true, "满 8 秒：放行");
    expect(open("ack", { readToEnd: true }) === true, "读到底：放行");
    expect(open("review", {}) === true, "重看：不设闸");
    expect(impl.RULES_GATE_MS === 8000, "放行时长是 8 秒");
}

function groupPlacement(impl, expect) {
    const box = { w: 460, h: 364 };
    const size = { w: 360, h: 150 };
    const spot = { x: 16, y: 123, w: 426, h: 76 };
    const a = impl.pickCalloutPos(box, size, spot, { x: 50, below: 7 });
    expect(a.ok && a.how === "pref" && a.y === 206, "首选位置合格：贴亮区下沿 7");
    const r = { x: a.x, y: a.y, w: size.w, h: size.h };
    expect(!impl.rectsOverlap(r, spot), "说明框不压亮区");
    expect(r.x >= 0 && r.y >= 0 && r.x + r.w <= box.w && r.y + r.h <= box.h, "说明框在设计盒内");
    // 首选位置压到亮区：换到下方
    const b = impl.pickCalloutPos(box, size, { x: 16, y: 20, w: 426, h: 40 }, { x: 50, y: 30 });
    expect(b.ok && b.how === "below" && b.y >= 60, "首选位置压到亮区：换方位");
    // 首选位置越出设计盒：换方位
    const c = impl.pickCalloutPos({ w: 800, h: 576 }, { w: 360, h: 204 }, spot, { x: 20, below: 400 });
    expect(c.ok && c.how !== "pref" && c.y + 204 <= 576, "首选位置越出设计盒：换方位");
    // 压到紧急切断：换方位
    const panic = { x: 300, y: 300, w: 200, h: 40 };
    const d = impl.pickCalloutPos({ w: 800, h: 576 }, size, { x: 10, y: 124, w: 267, h: 160 }, { x: 154, y: 260 }, [
        panic,
    ]);
    expect(d.ok && !impl.rectsOverlap({ x: d.x, y: d.y, ...size }, panic), "压到紧急切断：换方位");
    // 全都放不下：ok 为假（冒烟据此报出来）
    const e = impl.pickCalloutPos(box, { w: 360, h: 300 }, spot, { x: 50, below: 7 });
    expect(e.ok === false, "四向都放不下：ok 为假");
    expect(impl.rectsOverlap({ x: 0, y: 0, w: 10, h: 10 }, { x: 10, y: 0, w: 10, h: 10 }) === false, "贴边不算相交");
    // 遮罩图：洞内透明、洞外不透明的两条渐变
    const m = impl.holeMask({ x: 10, y: 20, w: 100, h: 50 }, 7);
    expect(/to right, #000 3px, transparent 17px, transparent 103px, #000 117px/.test(m), "遮罩图横向羽化 7");
    expect(/to bottom, #000 13px, transparent 27px, transparent 63px, #000 77px/.test(m), "遮罩图纵向羽化 7");
}

const GROUPS = [
    ["① 链的顺序", groupOrder],
    ["② 恢复的实例", groupRestored],
    ["③ 规则改版重弹", groupVersion],
    ["④ 危险态立即关", groupDanger],
    ["⑤ 写位", groupWrites],
    ["⑥ Esc 与放行", groupEsc],
    ["⑦ 说明框摆放", groupPlacement],
];

const REAL = { ...fr, ...rp, ...tp };
for (const [name, fn] of GROUPS) {
    let n = 0;
    fn(REAL, (cond, msg) => (n++, run.check(cond, `${name}：${msg}`)), gate.GATED_WRITES);
    run.log(`  ${name}：${n} 项`);
}

// ---------------------------------------------------------------- 词条与步骤表
const DICTS = {
    zh: { ...common.zh, ...guide.zh },
    en: { ...common.en, ...guide.en },
    fr: { ...common.fr, ...guide.fr },
};
run.eq(TOUR_IN_STEPS.length, 6, "Input 导览 6 步");
run.eq(TOUR_OUT_STEPS.length, 4, "Output 导览 4 步");
const keys = [
    ...[...TOUR_IN_STEPS, ...TOUR_OUT_STEPS].flatMap((s) => [s.keys + "title", s.keys + "body"]),
    ...Object.values(rp.RULES_BUTTONS).flatMap((list) => list.map((b) => b[1])),
    "rules.title",
    "rules.footnote",
    "rules.readFirst",
    "hint.firstUseRestored",
    "tour.common.clickAnywhere",
    "tour.common.skip",
    "tour.common.prev",
    "tour.common.next",
    "tour.common.done",
    "tour.common.step",
    "common.langStart.title",
];
for (const lang of ["zh", "en", "fr"])
    for (const k of keys)
        run.check(typeof DICTS[lang][k] === "string" && DICTS[lang][k].trim() !== "", `${lang} 缺词条 ${k}`);
run.check(rp.RULES_BUTTONS.review[0][1] === BUTTON_KEYS.close, "重看的 [关闭] 用共用的关闭词条");

// ---------------------------------------------------------------- 坏样例：把判据写错的替身必须被抓出来
const stand = (over) => ({ ...REAL, ...over });
const wrap = (machineOver) => ({
    createFirstRunMachine: (o) => {
        const m = fr.createFirstRunMachine(o);
        return { ...m, ...machineOver(m, o) };
    },
});
const BAD = [
    [
        "忽略 restored",
        groupRestored,
        stand({
            firstRunSteps: (role, snap, c, v) => fr.firstRunSteps(role, snap && { ...snap, restored: false }, c, v),
        }),
    ],
    [
        "忽略改版（只要确认过就算）",
        groupVersion,
        stand({ firstRunSteps: (role, snap, c, v) => fr.firstRunSteps(role, snap && { ...snap, rulesAck: v }, c, v) }),
    ],
    ["L2 不关", groupDanger, stand(wrap(() => ({ frame: () => false })))],
    [
        "用户打开的规则页也因 L2 自动关",
        groupDanger,
        stand(
            wrap((m) => ({
                frame: (live) => {
                    if (m.state().phase === "idle" || !fr.mustClose(live)) return false;
                    m.abort();
                    return true;
                },
            })),
        ),
    ],
    [
        "重看也写位",
        groupWrites,
        stand(
            wrap((m, o) => ({
                tourEnd: () => {
                    if (m.state().phase === "tour") o.write("setGuideSeen", true);
                    m.tourEnd();
                },
            })),
        ),
    ],
    ["要确认的规则页随时可 Esc", groupEsc, stand({ rulesEscapable: () => true })],
    [
        "8 秒闸写成 8 秒之前就放行",
        groupEsc,
        stand({ rulesGateOpen: (v, p = {}) => rp.rulesGateOpen(v, { ...p, elapsedMs: (p.elapsedMs || 0) + 1 }) }),
    ],
    [
        "压到亮区也算合格",
        groupPlacement,
        stand({ pickCalloutPos: (box, size, spot, pref, avoid) => tp.pickCalloutPos(box, size, null, pref, avoid) }),
    ],
    [
        "Output 不看 inputGuideSeen",
        groupOrder,
        stand({ firstRunSteps: (role, ...a) => fr.firstRunSteps("input", ...a) }),
    ],
];
for (const [name, fn, impl] of BAD) {
    let fails = 0;
    fn(impl, (cond) => (cond ? true : (fails++, false)), gate.GATED_WRITES);
    run.check(fails > 0, `坏样例「${name}」应被抓出来`);
}
// 写入闸拦了首启写位：同一组断言必须判红
{
    let fails = 0;
    groupWrites(REAL, (cond) => (cond ? true : (fails++, false)), [...gate.GATED_WRITES, "setRulesAck"]);
    run.check(fails > 0, "坏样例「写入闸拦 setRulesAck」应被抓出来");
}

run.finish();
