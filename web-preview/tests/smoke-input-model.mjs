// SPDX-License-Identifier: GPL-3.0-or-later
//
// Input 页面纯函数单测（Node，无 DOM）：web/input/model.js 的数字格式化、取值映射、各卡展示模型、消息表、导出提示的关闭记录。
// 每组断言都带坏样例：把被测函数的某条规则故意写错的替身必须被同一组断言抓出来（见 caught）；
// 删掉 model.js 里对应判据，同一组断言必须变红（记录在 mutation-evidence.md）。
//
// 用法：node web-preview/tests/smoke-input-model.mjs [仓库根]

import fs from "node:fs";
import { pathToFileURL } from "node:url";
import path from "node:path";
import { createRun, repoRoot } from "./lib/harness.mjs";

const ROOT = repoRoot();
const run = createRun("Input 页面纯函数");
const real = await import(pathToFileURL(path.join(ROOT, "web/input/model.js")).href);
const M_parse = real.parseFeedbackInput;

/** 一组断言：对给定的模块替身跑，返回失败描述数组。 */
function suite(M) {
    const f = [];
    const ok = (c, m) => {
        if (!c) f.push(m);
    };
    const eq = (a, b, m) => {
        if (JSON.stringify(a) !== JSON.stringify(b))
            f.push(`${m}：实得 ${JSON.stringify(a)}，应为 ${JSON.stringify(b)}`);
    };
    return { f, ok, eq, M };
}

/** 对一个模块替身跑一个套件，返回失败条数。bad 为真时不把失败计入总账（坏样例本来就该失败）。 */
function group(name, fn) {
    const s = suite(real);
    fn(s);
    run.eq(s.f, [], `${name}：全部断言通过`);
}
function caught(name, fn, patch) {
    const s = suite({ ...real, ...patch });
    try {
        fn(s);
    } catch (e) {
        s.f.push(`抛出异常：${e.message}`); // 坏样例让被测代码崩掉，同样算被抓出
    }
    run.check(s.f.length > 0, `${name}：坏样例被抓出（${Object.keys(patch).join("、")}）`);
}

const MINUS = "−";

// ---------------------------------------------------------------- 数字格式化

const formatting = ({ M, ok, eq }) => {
    eq(M.fmtFixed(-2.94, 1), `${MINUS}2.9`, "负数用 U+2212");
    eq(M.fmtFixed(-0.02, 1), "0.0", "舍入为 0 的不带负号");
    eq(M.fmtSigned(1.44, 1), "+1.4", "正数带加号");
    eq(M.fmtSigned(0, 1), "0.0", "零不带符号");
    eq(M.fmtSigned(-2.9, 1), `${MINUS}2.9`, "带符号的负数");
    eq(M.fmtLevel(null), `${MINUS}∞`, "静音写 −∞");
    eq(M.fmtLevel(-12.4), `${MINUS}12.4`, "电平一位小数");
    eq(M.fmtKHz(48000), "48", "48 kHz 不带小数");
    eq(M.fmtKHz(44100), "44.1", "44.1 kHz 一位小数");
    eq(M.fmtHz(6000), "6.0 kHz", "千赫一位小数");
    eq(M.fmtHz(250), "250 Hz", "赫兹取整");
    ok(!/-/.test(M.fmtFixed(-5, 1)), "不出现半角减号");
};
group("格式化", formatting);
caught("格式化", formatting, { fmtFixed: (x, d = 1) => (x < 0 ? "-" : "") + Math.abs(x).toFixed(d) });
caught("格式化", formatting, { fmtSigned: (x, d = 1) => (x < 0 ? "−" : "") + Math.abs(x).toFixed(d) });

// ---------------------------------------------------------------- 反馈

const feedback = ({ M, ok, eq }) => {
    eq(M.fbAngle(0), -135, "0% 在 -135 度");
    eq(M.fbAngle(100), 45, "100% 在 +45 度");
    eq(M.fbAngle(150), 135, "150% 在 +135 度");
    eq(M.fbAngle(200), 135, "超出范围夹在 150%");
    eq(M.feedbackReadout(72, "pct"), { main: "72%", sub: `${MINUS}2.9 dB` }, "百分比读数");
    eq(M.feedbackReadout(72, "db"), { main: `${MINUS}2.9 dB`, sub: "72%" }, "dB 读数");
    eq(M.feedbackReadout(0, "pct").sub, `${MINUS}∞ dB`, "0% 的 dB 写 −∞");
    eq(M.feedbackReadout(118, "pct").sub, "+1.4 dB", "超过 100% 的 dB 带加号");
    eq(M.feedbackValueText(62, "zh"), `62%，${MINUS}4.2 dB`, "aria-valuetext 中文用全角逗号");
    eq(M.feedbackValueText(62, "en"), `62%, ${MINUS}4.2 dB`, "aria-valuetext 英文用半角逗号");
    eq(M.parseFeedbackInput("72", "pct"), { ok: true, value: 72 }, "百分比输入");
    eq(M.parseFeedbackInput("72%", "pct"), { ok: true, value: 72 }, "百分比输入带 %");
    eq(M.parseFeedbackInput("151", "pct").ok, false, "超过 150 报错");
    eq(M.parseFeedbackInput("-1", "pct").ok, false, "负数报错");
    eq(M.parseFeedbackInput("abc", "pct").ok, false, "非数字报错");
    eq(M.parseFeedbackInput("151", "pct"), { ok: false, min: "0", max: "150" }, "百分比范围文字");
    ok(Math.abs(M.parseFeedbackInput("0", "db").value - 100) < 1e-9, "0 dB 是 100%");
    eq(M.parseFeedbackInput(`${MINUS}∞`, "db"), { ok: true, value: 0 }, "−∞ 输入是 0%");
    eq(M.parseFeedbackInput("5", "db").ok, false, "超过 +3.5 dB 报错");
    // 写入精度 0.1 %（契约里 feedback 的步进）：72.5 % 不被取成 73 %
    eq(M.roundFeedback(72.5), 72.5, "72.5 % 保留 0.1 %");
    eq(M.roundFeedback(72.54), 72.5, "72.54 % 取到 72.5 %");
    eq(M.roundFeedback(72.56), 72.6, "72.56 % 取到 72.6 %");
    eq(M.roundFeedback(0.04), 0, "0.04 % 取到 0");
    eq(M.roundFeedback(150), 150, "150 % 不变");
    eq(M.roundFeedback(M.parseFeedbackInput("72.5", "pct").value), 72.5, "按百分比输入 72.5 % 经取整后仍是 72.5 %");
    // 按 dB 输入：常用范围（−12 到 +3.5 dB，每 0.1 dB）经 0.1 % 取整后换回 dB 读数不变
    const lossy = [];
    for (let t = -120; t <= 35; t++) {
        const db = t / 10;
        const pct = M.roundFeedback(M.parseFeedbackInput(M.fmtFixed(db, 1), "db").value);
        if (M.fbDbText(pct) !== M.fmtSigned(db, 1)) lossy.push(M.fmtSigned(db, 1));
    }
    eq(lossy, [], "按 dB 输入（−12 到 +3.5，每 0.1 dB）取整后 dB 读数应不变");
    eq(M.fbDbText(M.roundFeedback(M.dbToPct(-6))), M.fmtSigned(-6, 1), "−6 dB 输入后读回 −6.0");
    // 打开数值输入框、什么也不改就回车：值不变（框里的文字与初值一样就不写）。dB 档初值只有 1 位小数，
    // 照常解析再取整会把 72.5 % 改成 72.4 %，所以要逐个 0.1 % 的格点查，百分比与 dB 两档都查
    for (const unit of ["db", "pct"]) {
        const moved = [];
        for (let i = 0; i <= 1500; i++) {
            const pct = i / 10;
            const initial = unit === "db" ? M.fbDbText(pct) : String(M.roundFeedback(pct));
            const res = M.parseFeedbackEdit(initial, initial, unit);
            const after = res.ok && res.unchanged ? pct : res.ok ? M.roundFeedback(res.value) : null;
            if (after !== pct) moved.push(`${pct}->${after}`);
        }
        eq(moved.slice(0, 5), [], `${unit} 档：每个 0.1 % 格点打开输入框原样回车，值应不变`);
    }
    eq(M.parseFeedbackEdit("−2.8", "−2.8", "db"), { ok: true, unchanged: true }, "文字与初值一样：不写");
    eq(M.parseFeedbackEdit(" 72.5 ", "72.5", "pct"), { ok: true, unchanged: true }, "首尾空格不算改动");
    eq(M.parseFeedbackEdit("−3.0", "−2.8", "db").ok, true, "改了文字：照常解析");
    ok(M.parseFeedbackEdit("−3.0", "−2.8", "db").unchanged !== true, "改了文字：不是『没改』");
    eq(M.roundFeedback(M.parseFeedbackEdit("−3.0", "−2.8", "db").value), 70.8, "改成 −3.0 dB 写 70.8 %");
    eq(M.parseFeedbackEdit("5", "−2.8", "db").ok, false, "改了文字且超范围：报错");
    eq(M.feedbackChip({ cut: true, fadeIn: true, limiterOn: false, feedback: 130 }).kind, "cut", "切断压过其余芯片");
    eq(
        M.feedbackChip({ cut: false, fadeIn: true, limiterOn: false, feedback: 130 }).kind,
        "fadeIn",
        "淡入压过上限芯片",
    );
    eq(
        M.feedbackChip({ cut: false, fadeIn: false, limiterOn: false, feedback: 130 }),
        {
            kind: "cap",
            key: "in.fb.capNote",
            tone: "amber",
            vals: { s: "130" },
        },
        "限制器关且设定超过 100%：上限芯片",
    );
    eq(
        M.feedbackChip({ cut: false, fadeIn: false, limiterOn: true, feedback: 118, mix: 50 }).kind,
        "selfOsc",
        "超过 100%、限制器开、混合 50%：自激区",
    );
    eq(
        M.feedbackChip({ cut: false, fadeIn: false, limiterOn: true, feedback: 118, mix: 49 }),
        null,
        "混合低于 50% 时越过 100% 不显示自激区",
    );
    eq(M.feedbackChip({ cut: false, fadeIn: false, limiterOn: true, feedback: 100 }), null, "100% 以内没有芯片");
    eq(
        M.feedbackChip({ cut: false, fadeIn: false, limiterOn: false, feedback: 100 }),
        null,
        "限制器关但不超 100%：没有芯片",
    );
    eq(M.fbUnitOf({ fbUnit: "db" }), "db", "cfg.fbUnit 为 db");
    eq(M.fbUnitOf({ fbUnit: "pct" }), "pct", "cfg.fbUnit 为 pct");
    eq(M.fbUnitOf(undefined), "pct", "缺 cfg 按百分比");
    eq(M.delayUnitOf({ delayUnit: "samples" }), "samples", "cfg.delayUnit 为样本");
    eq(M.delayUnitOf({ delayUnit: "ms" }), "ms", "cfg.delayUnit 为毫秒");
};
group("反馈", feedback);
caught("反馈", feedback, { fbAngle: (p) => -135 + p * 2.7 });
caught("反馈", feedback, {
    feedbackChip: ({ cut, limiterOn, feedback }) =>
        feedback > 100 ? { kind: limiterOn ? "selfOsc" : "cap" } : cut ? { kind: "cut" } : null,
});
caught("反馈", feedback, {
    feedbackChip: ({ cut, limiterOn, feedback, mix }) =>
        cut
            ? { kind: "cut", key: "in.fb.chip.cut", tone: "red" }
            : feedback > 100
              ? limiterOn
                  ? { kind: "selfOsc", key: "in.fb.chip.selfOsc", tone: "amber" }
                  : { kind: "cap", key: "in.fb.capNote", tone: "amber", vals: { s: String(Math.round(feedback)) } }
              : mix < 0
                ? { kind: "x" }
                : null,
});
caught("反馈", feedback, { parseFeedbackInput: (t) => ({ ok: true, value: Number(t) }) });
caught("反馈", feedback, { roundFeedback: Math.round });
// 不做「没改就不写」，一律照常解析：dB 档原样回车会改动格点
caught("反馈", feedback, { parseFeedbackEdit: (text, initial, unit) => M_parse(text, unit) });

// ---------------------------------------------------------------- Mix 与音色

const mixTone = ({ M, ok, eq }) => {
    eq(M.mixGains(25), { dry: 1, ret: 0.5 }, "混合 25%：干声满、回送一半");
    eq(M.mixGains(50), { dry: 1, ret: 1 }, "混合 50%：两路都满");
    eq(M.mixGains(75), { dry: 0.5, ret: 1 }, "混合 75%：干声一半");
    eq(M.mixGains(0).ret, 0, "混合 0%：回送为零");
    eq(M.mixGains(100).dry, 0, "混合 100%：干声为零");
    eq(M.mixFill(0), 50, "0 dBFS 占半管");
    eq(M.mixFill(-60), 0, "-60 dBFS 为零");
    eq(M.mixFill(-30), 25, "-30 dBFS 占四分之一");
    eq(M.mixFill(null), 0, "静音为零");
    eq(M.mixFill(6), 50, "超过 0 dBFS 不越过中线");
    eq(M.lowCutPos(20), 0, "低切 20 Hz（关）在起点");
    eq(M.lowCutPos(2000), 1, "低切 2000 Hz 在终点");
    ok(Math.abs(M.lowCutPos(200) - 0.5) < 1e-9, "低切对数中点是 200 Hz");
    eq(M.highCutPos(20000), 1, "高切 20000 Hz（关）在终点");
    eq(M.highCutPos(1000), 0, "高切 1000 Hz 在起点");
    ok(Math.abs(M.highCutFromPos(M.highCutPos(6000)) - 6000) < 1e-6, "高切位置往返");
    ok(Math.abs(M.lowCutFromPos(M.lowCutPos(350)) - 350) < 1e-6, "低切位置往返");
    eq(M.isLowCutOff(20), true, "20 Hz 是关");
    eq(M.isLowCutOff(21), false, "21 Hz 不是关");
    eq(M.isHighCutOff(20000), true, "20000 Hz 是关");
    eq(M.isHighCutOff(19999), false, "19999 Hz 不是关");
    eq(M.ceilingPos(-24), 0, "上限 -24 在起点");
    eq(M.ceilingPos(0), 1, "上限 0 在终点");
    eq(M.ceilingFromPos(0.25), -18, "上限位置换算");
    eq(M.highCutClampKHz(32000), "14.4", "32 kHz 采样率的高切上限 14.4 kHz");
    eq(M.highCutClampKHz(44100), "19.8", "44.1 kHz 采样率的高切上限 19.8 kHz");
};
group("Mix 与音色", mixTone);
caught("Mix 与音色", mixTone, { mixGains: (m) => ({ dry: 1 - m / 100, ret: m / 100 }) });
caught("Mix 与音色", mixTone, { highCutPos: (hz) => (hz - 1000) / 19000 });
caught("Mix 与音色", mixTone, { mixFill: (db) => (db == null ? 0 : 100 * Math.max(0, Math.min(1, (db + 60) / 60))) });

// ---------------------------------------------------------------- GR 与灯

const safety = ({ M, ok, eq }) => {
    eq(M.grView(-2.1, true).state, "work", "小幅压限：work");
    eq(M.grView(0, true).state, "idle", "无压限：idle");
    eq(M.grView(-6.8, true).state, "heavy", "压得重：heavy");
    eq(M.grView(-6.8, false).state, "hidden", "限制器关：隐藏");
    eq(M.grView(-6.8, false).readout, "—", "隐藏时读数写破折号");
    eq(M.grView(-24, true).fillPct, 100, "满量程填满");
    eq(M.grView(-60, true).fillPct, 100, "超量程不越过");
    eq(M.grView(-12, true).fillPct, 50, "半量程");
    const l = M.lampsView({ dc: "blocked", nonfinite: "zeroed", nonfiniteCount: 12, guard: "reduced", guardDb: 4.5 });
    eq([l.dc.state, l.dc.tone], ["blocked", "amber"], "直流灯阻断：琥珀");
    eq([l.nan.state, l.nan.tone, l.nan.vals], ["zeroed", "amber", { n: "12" }], "数值灯置零：琥珀并带计数");
    eq([l.run.state, l.run.tone, l.run.vals], ["reduced", "amber", { x: "4.5" }], "失控灯降低：琥珀并带 dB");
    eq(M.lampsView({ nonfinite: "latched", guard: "cut" }).nan.tone, "red", "锁定：红");
    eq(M.lampsView({ guard: "cut" }).run.tone, "red", "切断：红");
    eq(M.lampsView({ dc: "weird" }).dc.state, "ok", "不认识的状态当 ok");
    eq(M.lampsView({}).run.key, "in.lamp.ok", "全默认是 ok 词条");
    eq(M.lampsView({ guard: "reduced" }).run.vals, { x: "—" }, "缺 guardDb 写破折号而不是占位符");
    ok(M.lampsView({ guard: "limiting" }).run.key === "in.lamp.run.limiting", "压限灯词条");
};
group("GR 与灯", safety);
caught("GR 与灯", safety, { grView: (gr, on) => ({ state: gr === 0 ? "idle" : "work", fillPct: 0, readout: "0" }) });
caught("GR 与灯", safety, {
    lampsView: () => ({ dc: { state: "ok", tone: "gray" }, nan: { state: "ok" }, run: { state: "ok", key: "x" } }),
});

// ---------------------------------------------------------------- 延迟卡

const DELAY = { actualMs: 100, minMs: 10.7, reason: "ok", maxBlock: 512 };
const delay = ({ M, ok, eq }) => {
    const ms = M.delayView({ mode: "time", unit: "ms", delayMs: 100, note: "n4", delay: DELAY, sampleRate: 48000 });
    eq(
        [ms.segment, ms.readout, ms.sub],
        [
            "ms",
            { kind: "ms", text: "100.0", unitKey: "in.dly.unitMs" },
            { key: "in.dly.sub.ms", vals: { n: "4800", sr: "48" } },
        ],
        "毫秒档读数与副行",
    );
    ok(Math.abs(ms.sliderPct - 55.5) < 0.1, "100 ms 在对数滑杆约 55.5%");
    eq(ms.raised, false, "实际等于设定：不画抬升箭头");
    eq(ms.inHatch, false, "设定大于最小值：不在斜纹里");
    eq(ms.badge, null, "没有徽标");
    const sp = M.delayView({
        mode: "time",
        unit: "samples",
        delayMs: 100,
        note: "n4",
        delay: DELAY,
        sampleRate: 48000,
    });
    eq([sp.segment, sp.readout.text, sp.sub.key], ["samples", "4800", "in.dly.sub.samples"], "样本档读数");
    const sy = M.delayView({
        mode: "sync",
        unit: "ms",
        delayMs: 100,
        note: "n8d",
        delay: { ...DELAY, actualMs: 375, bpm: 120 },
        sampleRate: 48000,
    });
    eq(
        [sy.segment, sy.readout, sy.sub.vals],
        ["sync", { kind: "note", noteKey: "in.dly.note.n8d" }, { d: "375.0", bpm: "120" }],
        "同步档：1/8 附点在 120 BPM 是 375 ms",
    );
    eq(sy.inHatch, false, "同步档不画斜纹");
    const raised = M.delayView({
        mode: "time",
        unit: "ms",
        delayMs: 5,
        note: "n4",
        delay: { ...DELAY, actualMs: 10.7, reason: "raised" },
        sampleRate: 48000,
    });
    eq([raised.raised, raised.inHatch, raised.badge], [true, true, "raised"], "设定低于最小值：抬升箭头、斜纹、徽标");
    eq(raised.actualVals, { d: "10.7" }, "实际值");
    // 写入取整：毫秒档 0.1 ms，夹在 1 到 4000 ms
    eq(M.quantizeDelayMs(12.34, "ms", 48000), 12.3, "毫秒档取到 0.1 ms");
    eq(M.quantizeDelayMs(0.2, "ms", 48000), 1, "毫秒档不低于 1 ms");
    eq(M.quantizeDelayMs(5000, "ms", 48000), 4000, "毫秒档不高于 4000 ms");
    // 按样本数设的值来回换算不变：样本数 -> 毫秒（0.001 ms）-> 样本数，仍是原来的样本数，且落在 0.001 ms 的格点上
    const drift = [];
    const offGrid = [];
    for (const sr of [44100, 48000, 88200, 96000, 192000]) {
        const lo = M.msToSamples(1, sr);
        const hi = M.msToSamples(4000, sr);
        const ns = [];
        for (let n = lo; n < lo + 400; n++) ns.push(n);
        for (let n = lo + 400; n < hi; n += 997) ns.push(n);
        ns.push(hi);
        for (const n of ns) {
            const q = M.quantizeDelayMs(M.samplesToMs(n, sr), "samples", sr);
            if (M.msToSamples(q, sr) !== n) drift.push(`${n}@${sr}->${M.msToSamples(q, sr)}`);
            if (Math.abs(q * 1000 - Math.round(q * 1000)) > 1e-6) offGrid.push(`${n}@${sr}`);
        }
    }
    eq(drift.slice(0, 5), [], "按样本数设的值经取整再换回样本数应不变");
    eq(offGrid.slice(0, 5), [], "样本档取整后的毫秒值应落在 0.001 ms 的格点上");
    eq(
        M.delayView({
            mode: "time",
            unit: "ms",
            delayMs: 100,
            note: "n4",
            delay: { ...DELAY, reason: "overrun" },
            sampleRate: 48000,
        }).badge,
        "overrun",
        "违约徽标",
    );
    eq(
        M.delayView({
            mode: "time",
            unit: "ms",
            delayMs: 4000,
            note: "n4",
            delay: { ...DELAY, reason: "capped" },
            sampleRate: 48000,
        }).badge,
        "clipped",
        "截断徽标",
    );
    eq(
        M.delayView({
            mode: "sync",
            unit: "ms",
            delayMs: 100,
            note: "n4",
            delay: { ...DELAY, noTempo: true },
            sampleRate: 48000,
        }).badge,
        "noTempo",
        "无速度徽标",
    );
    eq(
        M.delayView({
            mode: "time",
            unit: "ms",
            delayMs: 100,
            note: "n4",
            delay: { ...DELAY, noTempo: true },
            sampleRate: 48000,
        }).badge,
        null,
        "毫秒档不显示无速度徽标",
    );
    eq(
        M.delayView({
            mode: "time",
            unit: "ms",
            delayMs: 100,
            note: "n4",
            delay: { ...DELAY, reason: "overrun", noTempo: true },
            sampleRate: 48000,
        }).badge,
        "overrun",
        "徽标只出一个，违约优先",
    );
    const unknown = M.delayView({
        mode: "time",
        unit: "ms",
        delayMs: 100,
        note: "n4",
        delay: { ...DELAY, minMs: 0 },
        sampleRate: 48000,
    });
    eq(
        [unknown.minKnown, unknown.minPct, unknown.inHatch, unknown.state],
        [false, null, false, "minUnknown"],
        "最小值未知：不画刻线与斜纹",
    );
    eq(
        M.delayView({ mode: "time", unit: "ms", delayMs: 100, note: "n4", delay: DELAY, sampleRate: 44100 })
            .maxBlockVals,
        { n: "512", sr: "44.1" },
        "采样率千赫写法",
    );
    eq(M.parseDelayInput("250", "ms", 48000), { ok: true, ms: 250 }, "毫秒输入");
    eq(M.parseDelayInput("4800", "samples", 48000), { ok: true, ms: 100 }, "样本输入换算成毫秒");
    eq(M.parseDelayInput("0.5", "ms", 48000), { ok: false, min: "1", max: "4000" }, "低于 1 ms 报错并给范围");
    eq(M.parseDelayInput("4001", "ms", 48000).ok, false, "高于 4000 ms 报错");
    eq(M.parseDelayInput("x", "ms", 48000).ok, false, "非数字报错");
    eq(M.parseDelayInput("1", "samples", 48000).ok, false, "1 个样本低于 1 ms 报错");
    eq(M.delayPos(1), 0, "1 ms 在滑杆起点");
    eq(M.delayPos(4000), 100, "4000 ms 在滑杆终点");
    ok(Math.abs(M.delayFromPos(M.delayPos(37)) - 37) < 1e-6, "滑杆位置往返");
};
group("延迟卡", delay);
caught("延迟卡", delay, {
    // 样本档只取到 0.1 ms：按样本数设的值换回来会漂
    quantizeDelayMs: (ms, unit, sr) => Math.max(1, Math.min(4000, Math.round(ms * 10) / 10)),
});
caught("延迟卡", delay, { delayView: (p) => real.delayView({ ...p, note: "n4" }) });
caught("延迟卡", delay, { delayPos: (ms) => (ms / 4000) * 100 });
caught("延迟卡", delay, { parseDelayInput: (t) => ({ ok: true, ms: Number(t) }) });

// ---------------------------------------------------------------- 配对、对端、页脚

const pairing = ({ M, ok, eq }) => {
    eq(M.peerPresent({ state: "searching", peer: {} }), false, "没找到对端：空");
    eq(M.peerPresent({ state: "connected", peer: { name: "Vox" } }), true, "有轨道名：有对端");
    eq(M.peerPresent({ state: "connectedUnverified", peer: {} }), true, "宿主不给名字的已连接：有对端行");
    eq(M.channelKind(1), "mono", "1 声道");
    eq(M.channelKind(2), "st", "2 声道");
    eq(M.channelKind(6), null, "其它声道数没有徽标");
    eq(M.trackColor("#2d79c6"), "#2d79c6", "合法颜色");
    eq(M.trackColor("red"), null, "颜色名不收");
    eq(M.trackColor("#2d79c6; background:url(x)"), null, "夹带样式的值不收");
    eq(M.trackColor(null), null, "没有颜色");
    eq(M.verifyKind("diff"), "diff", "核验不同");
    eq(M.verifyKind("???"), "unknown", "未知核验当无法核验");
    eq(M.isFlowing({ pairState: "connected", cut: null, loopOn: true, silentCause: null }), true, "已连接且流动");
    eq(
        M.isFlowing({ pairState: "connectedStill", cut: null, loopOn: true, silentCause: null }),
        false,
        "回送静音不脉冲",
    );
    eq(M.isFlowing({ pairState: "connected", cut: "panic", loopOn: true, silentCause: null }), false, "切断后不脉冲");
    eq(M.isFlowing({ pairState: "connected", cut: null, loopOn: false, silentCause: null }), false, "环路关闭不脉冲");
    eq(
        M.isFlowing({ pairState: "connected", cut: null, loopOn: true, silentCause: "hostBypass" }),
        false,
        "旁路不脉冲",
    );
    eq(M.isDanger({ cut: "nan", pairState: "connected" }), true, "切断：危险描边");
    eq(M.isDanger({ cut: null, pairState: "conflict" }), true, "组冲突：危险描边");
    eq(M.isDanger({ cut: null, pairState: "abiMismatch" }), true, "版本不匹配：危险描边");
    eq(M.isDanger({ cut: null, pairState: "stale" }), false, "失联不是 L2");
    eq(M.isDanger({ cut: null, pairState: "unassigned" }), false, "未分组不画描边");
    eq(
        M.footerHint({ cut: "panic", pairState: "connected", feedback: 0, dragging: true }).state,
        "cut",
        "切断提示优先",
    );
    eq(
        M.footerHint({ cut: null, pairState: "searching", feedback: 0, dragging: true }).state,
        "unpaired",
        "没配上提示优先于拖动",
    );
    eq(M.footerHint({ cut: null, pairState: "connected", feedback: 0, dragging: true }).state, "drag", "拖动中提示");
    eq(
        M.footerHint({ cut: null, pairState: "connected", feedback: 0, dragging: false }).state,
        "zero",
        "已连接而反馈为 0",
    );
    eq(M.footerHint({ cut: null, pairState: "connected", feedback: 72, dragging: false }).state, "ready", "就绪");
    eq(M.footerHint({ cut: null, pairState: "stale", feedback: 72, dragging: false }).state, "ready", "失联时仍是就绪");
    eq(M.groupsWithPeer(0b00000101), ["A", "C"], "位图展开");
    eq(M.otherGroupsWithPeer(0b00000101, "A"), ["C"], "不含本组");
};
group("配对与页脚", pairing);
caught("配对与页脚", pairing, { trackColor: (c) => c });
caught("配对与页脚", pairing, { isDanger: ({ cut }) => Boolean(cut) });
caught("配对与页脚", pairing, { footerHint: () => ({ state: "ready", key: "in.hint.ready" }) });

// ---------------------------------------------------------------- 导出提示

const exportRec = ({ M, ok, eq }) => {
    eq(M.exportHint({ bits: 0, count: 0 }, null), { show: false, rec: null }, "没有锁存：不显示");
    eq(M.exportHint({ bits: 3, count: 2 }, null), { show: true, rec: null }, "有锁存、没关过：显示");
    eq(M.exportHint({ bits: 3, count: 2 }, { bits: 3, count: 2 }).show, false, "关掉后同一句话不再出现");
    eq(M.exportHint({ bits: 3, count: 5 }, { bits: 3, count: 2 }).show, true, "计数增加：再出现");
    eq(M.exportHint({ bits: 7, count: 2 }, { bits: 3, count: 2 }).show, true, "出现新的原因位：再出现");
    eq(M.exportHint({ bits: 1, count: 2 }, { bits: 3, count: 2 }).show, false, "原因位变少不算新的");
    eq(M.exportHint({ bits: 0, count: 0 }, { bits: 3, count: 2 }), { show: false, rec: null }, "条件消失：清掉记录");
    eq(M.exportHint({ bits: 3, count: 1 }, { bits: 3, count: 4294967295 }).show, true, "计数回绕后仍判为更新");
    eq(
        M.exportHint({ bits: 3, count: 4294967295 }, { bits: 3, count: 1 }).show,
        false,
        "计数更小（回绕之前）不算更新的反例",
    );
    const ids = (bits) => M.exportMessages(bits).map((m) => m.id);
    eq(
        ids(15),
        ["exportViolation", "exportOverrun", "exportPeer", "exportResync"],
        "四个原因按优先级各出一条，由「+n」轮换",
    );
    eq(ids(12), ["exportPeer", "exportResync"], "没有前两个：对端违约排在重同步前");
    eq(ids(4), ["exportPeer"], "只有对端违约");
    eq(M.exportMessages(0), [], "没有原因位没有消息");
};
group("导出提示", exportRec);
caught("导出提示", exportRec, { exportHint: (cur, rec) => ({ show: cur.bits !== 0 && rec === null, rec }) });
caught("导出提示", exportRec, { exportMessages: (b) => (b ? [{ id: "exportViolation", key: "k" }] : []) });
caught("导出提示", exportRec, {
    // 只取优先级最高的一条：其余原因没有「+n」可轮换
    exportMessages: (b) =>
        b & 1 ? [{ id: "exportViolation", key: "k" }] : b & 4 ? [{ id: "exportPeer", key: "k" }] : [],
});
caught("导出提示", exportRec, {
    exportHint: (cur, rec) => ({ show: Boolean(cur.bits) && (!rec || cur.bits !== rec.bits), rec }),
});

// ---------------------------------------------------------------- 消息表

const base = () => ({
    state: {
        version: "0.9.0",
        sampleRate: 48000,
        group: "A",
        groupOrigin: "manual",
        takeover: false,
        delay: { actualMs: 100, minMs: 10.7, reason: "ok", maxBlock: 512 },
        safety: { cut: null, cooldownMs: 0, l2: false },
        dismissedHints: [],
        exportCauses: { bits: 0, count: 0 },
        silentCause: null,
    },
    pair: {
        state: "connected",
        groupMask: 0,
        peer: { name: "Vox Lead", channels: 2, version: "0.9.0" },
        self: { name: "Vox Lead" },
    },
    values: { feedback: 72, loop_on: true, limiter: true, ceiling: -6, mix: 25 },
    meters: { lamps: { dc: "ok", nonfinite: "ok", guard: "ok" }, highCutLimited: false },
    exportRec: null,
    touchedFeedback: true,
    smartActive: false,
});
const derive = (M, patch) => {
    const c = base();
    patch(c);
    return M.deriveMessages(c);
};
const ids = (r) => r.messages.map((m) => m.id);

const messages = ({ M, ok, eq }) => {
    eq(ids(derive(M, () => {})), ["summary"], "正常工作：只有兜底的摘要");
    eq(derive(M, () => {}).messages[0].vals, { d: "100.0", c: `${MINUS}6.0` }, "摘要的占位符取值");
    eq(derive(M, () => {}).messages[0].tone, "neutral", "摘要是中性");
    const cut = derive(M, (c) => {
        c.state.safety = { cut: "panic", cooldownMs: 900, l2: true };
        c.values.feedback = 50;
    });
    eq(
        [cut.messages[0].id, cut.messages[0].action],
        ["cutPanic", { name: "restore", cooldown: true }],
        "紧急切断：恢复按钮带冷却",
    );
    eq(
        derive(M, (c) => {
            c.state.safety = { cut: "panic", cooldownMs: 0, l2: true };
            c.values.feedback = 130;
        }).messages[0].id,
        "cutPanicHot",
        "切断时反馈高于 100%：热切断",
    );
    eq(
        ids(
            derive(M, (c) => {
                c.state.safety = { cut: "nan", cooldownMs: 0 };
            }),
        ).slice(0, 1),
        ["cutNan"],
        "异常值切断",
    );
    eq(
        ids(
            derive(M, (c) => {
                c.state.safety = { cut: "huge", cooldownMs: 0 };
            }),
        ).slice(0, 1),
        ["cutHuge"],
        "超大值切断",
    );
    eq(
        ids(
            derive(M, (c) => {
                c.state.safety = { cut: "runaway", cooldownMs: 0 };
            }),
        ).slice(0, 1),
        ["cutRunaway"],
        "失控切断",
    );
    eq(
        derive(M, (c) => {
            c.state.safety = { cut: "nan", cooldownMs: 0 };
        }).messages[0].dismiss,
        undefined,
        "红色消息不带 ✕",
    );
    const abi = derive(M, (c) => {
        c.pair.state = "abiMismatch";
        c.pair.peer.version = "0.10.0";
    });
    eq([abi.messages[0].id, abi.messages[0].vals], ["abi", { a: "0.9.0", b: "0.10.0" }], "版本不匹配带两端版本");
    const sr = derive(M, (c) => {
        c.pair.state = "srMismatch";
        c.pair.peer.sampleRate = 44100;
    });
    eq(sr.messages[0].vals, { a: "48000", b: "44100" }, "采样率不一致带两端采样率");
    eq(
        derive(M, (c) => {
            c.pair.state = "srMismatch";
        }).messages[0].vals.b,
        "—",
        "缺对端采样率写破折号",
    );
    const cf = derive(M, (c) => {
        c.pair.state = "conflict";
    });
    eq(
        [cf.messages[0].id, cf.messages[0].action.name, cf.messages[0].vals],
        ["conflict", "moveFree", { g: "A" }],
        "组冲突：一键移到空闲组",
    );
    eq(
        ids(
            derive(M, (c) => {
                c.pair.state = "unassigned";
            }),
        ),
        ["unassigned"],
        "未分组",
    );
    eq(
        ids(
            derive(M, (c) => {
                c.pair.state = "ipcUnavailable";
            }),
        ),
        ["ipc"],
        "共享内存不可用",
    );
    eq(
        ids(
            derive(M, (c) => {
                c.state.delay.maxBlock = 65536;
            }),
        ).includes("unsupportedBlock"),
        true,
        "块长超过上限",
    );
    // 琥珀
    eq(
        derive(M, (c) => {
            c.meters.lamps.guard = "reduced";
            c.meters.lamps.guardDb = 4.5;
        }).messages[0].vals,
        { x: "4.5" },
        "失控保护介入带 dB",
    );
    eq(
        ids(
            derive(M, (c) => {
                c.meters.lamps.guard = "limiting";
            }),
        ),
        ["limiting"],
        "持续压限",
    );
    eq(
        ids(
            derive(M, (c) => {
                c.pair.state = "stale";
            }),
        ),
        ["stale"],
        "失联",
    );
    const hv = derive(M, (c) => {
        c.state.delay = { actualMs: 21.3, minMs: 21.3, reason: "overrun", maxBlock: 1024 };
    });
    eq([hv.messages[0].id, hv.messages[0].vals], ["hostViolation", { n: "1024", d: "21.3" }], "宿主违约");
    const dr = derive(M, (c) => {
        c.state.delay = { actualMs: 10.7, minMs: 10.7, reason: "raised", maxBlock: 512 };
    });
    eq([dr.messages[0].id, dr.messages[0].vals], ["dRaised", { m: "10.7" }], "D 被抬升");
    const lo = derive(M, (c) => {
        c.values.limiter = false;
        c.values.feedback = 130;
    });
    eq(ids(lo), ["limiterOff", "limiterCap100"], "限制器关：先琥珀后中性");
    eq(lo.messages[0].action, { name: "limOn" }, "限制器关带一键打开");
    eq(lo.messages[1].vals, { s: "130" }, "上限提示带设定值");
    eq(
        derive(M, (c) => {
            c.meters.lamps.nonfinite = "zeroed";
            c.meters.lamps.nonfiniteCount = 12;
        }).messages[0].vals,
        { n: "12" },
        "环内异常值已置零",
    );
    eq(
        ids(
            derive(M, (c) => {
                c.pair.state = "crossProcess";
            }),
        ),
        ["crossProcess"],
        "跨进程",
    );
    eq(
        derive(M, (c) => {
            c.pair.state = "crossTrack";
            c.pair.peer.name = "Gtr DI";
        }).messages[0].vals,
        { peer: "Gtr DI", self: "Vox Lead" },
        "轨道名不同",
    );
    // 导出
    const ex = derive(M, (c) => {
        c.state.exportCauses = { bits: 1 | 4, count: 2 };
    });
    eq(ids(ex), ["exportViolation", "exportPeer"], "导出提示按原因拆开");
    eq(
        ex.messages.every((m) => m.dismiss === true && m.tone === "amber"),
        true,
        "导出提示琥珀、带 ✕",
    );
    eq(
        ids(
            derive(M, (c) => {
                c.state.exportCauses = { bits: 1, count: 2 };
                c.exportRec = { bits: 1, count: 2 };
            }),
        ),
        ["summary"],
        "关掉之后不再出现",
    );
    eq(
        ids(
            derive(M, (c) => {
                c.state.exportCauses = { bits: 1, count: 3 };
                c.exportRec = { bits: 1, count: 2 };
            }),
        ),
        ["exportViolation"],
        "计数增加后再出现",
    );
    eq(
        ids(
            derive(M, (c) => {
                c.pair.state = "srMismatch";
                c.state.exportCauses = { bits: 4, count: 1 };
            }),
        ),
        ["srMismatch", "exportPeer"],
        "采样率不一致期间对端违约照常计入",
    );
    // 灰
    eq(
        ids(
            derive(M, (c) => {
                c.values.loop_on = false;
            }),
        ),
        ["loopOff"],
        "环路关闭",
    );
    eq(
        ids(
            derive(M, (c) => {
                c.state.silentCause = "hostBypass";
                c.pair.state = "connectedStill";
            }),
        ),
        ["bypassed"],
        "宿主旁路",
    );
    eq(
        ids(
            derive(M, (c) => {
                c.state.silentCause = "peerBypass";
            }),
        ),
        ["outputBypassed"],
        "对端旁路",
    );
    eq(
        ids(
            derive(M, (c) => {
                c.state.silentCause = "hostPaused";
            }),
        ),
        ["hostIdle"],
        "宿主暂停",
    );
    eq(
        ids(
            derive(M, (c) => {
                c.state.silentCause = "stalled";
            }),
        ),
        ["stalled"],
        "断档",
    );
    eq(
        ids(
            derive(M, (c) => {
                c.pair.state = "waitingAudio";
            }),
        ),
        ["waitingAudio"],
        "等待宿主送来音频",
    );
    eq(
        ids(
            derive(M, (c) => {
                c.pair.state = "validator";
            }),
        ),
        ["validator"],
        "验证工具",
    );
    // 紫
    const ns = derive(M, (c) => {
        c.pair.state = "searching";
    });
    eq(
        [ns.messages[0].id, ns.messages[0].tone, ns.messages[0].dismiss, ns.messages[0].chain],
        ["nextStep", "violet", true, true],
        "下一步：紫、可关、带链图",
    );
    const sw = derive(M, (c) => {
        c.pair.state = "searching";
        c.pair.groupMask = 0b100;
    });
    eq(
        [sw.messages[0].id, sw.messages[0].vals, sw.messages[0].action],
        ["switchHint", { g: "A", y: "C" }, { name: "moveTo", vals: { y: "C" } }],
        "别组有未配对的 Output：移过去",
    );
    eq(
        ids(
            derive(M, (c) => {
                c.pair.state = "searching";
                c.state.dismissedHints = ["nextStep"];
            }),
        ),
        [],
        "关掉的紫色提示不再出现",
    );
    eq(
        ids(
            derive(M, (c) => {
                c.values.feedback = 0;
                c.touchedFeedback = false;
            }),
        ),
        ["linkedZero"],
        "已连接、反馈 0、没调过：提示",
    );
    eq(
        ids(
            derive(M, (c) => {
                c.values.feedback = 0;
                c.touchedFeedback = true;
            }),
        ),
        ["summary"],
        "调过反馈之后不再提示",
    );
    eq(
        ids(
            derive(M, (c) => {
                c.values.feedback = 0;
                c.touchedFeedback = false;
                c.smartActive = true;
                c.state.groupOrigin = "auto";
            }),
        ),
        ["smartDefault", "linkedZero"],
        "智能默认先于已连接提示",
    );
    // 中性
    eq(
        derive(M, (c) => {
            c.meters.highCutLimited = true;
            c.state.sampleRate = 32000;
        }).messages[0].vals,
        { f: "14.4" },
        "高切被采样率截断",
    );
    // 通知（rinne.state.notices）：配对状态与数值表达不了的五条
    const note = (list, patch) =>
        derive(M, (c) => {
            c.state.notices = list;
            if (patch) patch(c);
        });
    const sandbox = note([{ id: "ipcSandbox" }]);
    eq(
        [sandbox.messages[0].id, sandbox.messages[0].tone, sandbox.messages[0].action],
        ["ipcSandbox", "red", { name: "guide" }],
        "沙盒拒绝共享内存：红、带指南",
    );
    eq(
        ids(
            note([{ id: "ipcSandbox" }], (c) => {
                c.pair.state = "ipcUnavailable";
            }),
        ),
        ["ipcSandbox"],
        "沙盒通知在时不再叠一条泛泛的共享内存不可用",
    );
    eq(
        ids(
            note([], (c) => {
                c.pair.state = "ipcUnavailable";
            }),
        ),
        ["ipc"],
        "没有沙盒通知时仍是泛泛的共享内存不可用",
    );
    for (const [id, tone] of [
        ["outOfOrder", "amber"],
        ["unstable", "amber"],
        ["stateDecodeFailed", "amber"],
    ]) {
        const r = note([{ id }]);
        eq([ids(r), r.messages[0].tone, r.messages[0].key], [[id], tone, "band.in." + id], "通知 " + id + "：琥珀");
    }
    eq(
        note([{ id: "newerProject", a: "1.2.0", b: "0.9.0" }]).messages[0].vals,
        { a: "0.9.0", b: "1.2.0" },
        "较新工程：通知里 a 是工程版本、b 是本机版本，文案写「本机 {a} / 工程 {b}」，取值对调",
    );
    eq(
        note([{ id: "newerProject" }]).messages[0].vals,
        { a: "—", b: "—" },
        "较新工程缺版本号：破折号，占位符不原样上屏",
    );
    eq(
        ids(
            note([
                { id: "ipcSandbox" },
                { id: "stateDecodeFailed" },
                { id: "unstable" },
                { id: "outOfOrder" },
                { id: "newerProject" },
            ]),
        ),
        ["ipcSandbox", "outOfOrder", "unstable", "newerProject", "stateDecodeFailed"],
        "五条同时在：按固定优先级排，不按到达顺序",
    );
    eq(
        ids(
            note(
                [{ id: "unstable" }, { id: "outOfOrder" }, { id: "newerProject" }, { id: "stateDecodeFailed" }],
                (c) => {
                    c.values.limiter = false;
                    c.pair.state = "crossTrack";
                    c.pair.peer.name = "Gtr DI";
                    c.meters.lamps.nonfinite = "zeroed";
                },
            ),
        ),
        ["outOfOrder", "unstable", "limiterOff", "crossTrack", "newerProject", "stateDecodeFailed", "ringNonfinite"],
        "通知夹在限制器关闭之前与环内非有限之间，次序与字串表一致",
    );
    eq(ids(note([{ id: "outOfOrder" }, { id: "outOfOrder" }])), ["outOfOrder"], "同一个 id 重复只出一条");
    for (const bad of [
        undefined,
        null,
        "outOfOrder",
        7,
        {},
        { id: "outOfOrder" },
        [null, 3, "x", {}, { id: 5 }, { id: "nope" }],
    ]) {
        eq(ids(note(bad)), ["summary"], "通知缺省、不是数组、元素残缺或 id 不认识：当没有 " + JSON.stringify(bad));
    }
    // 确认条
    eq(
        derive(M, (c) => {
            c.state.takeover = true;
        }).confirm,
        { key: "band.in.takeover", vals: { g: "A" } },
        "接管确认条",
    );
    eq(derive(M, () => {}).confirm, null, "平时没有确认条");
    // 占位符绝不原样出现
    const all = derive(M, (c) => {
        c.pair.state = "srMismatch";
        c.state.version = undefined;
        c.pair.peer = {};
        c.state.group = null;
    });
    ok(
        all.messages.every((m) => Object.values(m.vals || {}).every((v) => typeof v === "string" && v !== "")),
        "缺数据时占位符取值是非空字符串",
    );
};
group("消息表", messages);
caught("消息表", messages, { deriveMessages: (c) => ({ messages: [], confirm: null }) });
caught("消息表", messages, {
    deriveMessages: (c) => {
        const r = real.deriveMessages(c);
        return { ...r, messages: r.messages.map((m) => (m.tone === "red" ? { ...m, dismiss: true } : m)) };
    },
});
caught("消息表", messages, {
    deriveMessages: (c) => real.deriveMessages({ ...c, exportRec: null }),
});
caught("消息表", messages, {
    // 不认通知
    deriveMessages: (c) => real.deriveMessages({ ...c, state: { ...c.state, notices: [] } }),
});
caught("消息表", messages, {
    // 较新工程的版本号不对调
    deriveMessages: (c) => {
        const n = (c.state.notices || []).map((x) => (x && x.id === "newerProject" ? { ...x, a: x.b, b: x.a } : x));
        return real.deriveMessages({ ...c, state: { ...c.state, notices: n } });
    },
});
caught("消息表", messages, {
    // 沙盒通知在时又叠一条泛泛的 ipc
    deriveMessages: (c) => {
        const r = real.deriveMessages(c);
        const hasBox = r.messages.some((m) => m.id === "ipcSandbox");
        return hasBox ? { ...r, messages: [{ ...r.messages[0], id: "ipc" }, ...r.messages] } : r;
    },
});

// ---------------------------------------------------------------- 单位写入唯一落点

/** 源码里对 mem 的赋值（mem.x = …、mem[x] = …），不含比较。 */
const MEM_WRITE = new RegExp(String.raw`\bmem(?:\.\w+|\[[^\]\n]+\])\s*=(?!=)`, "g");
const memWrites = (src) => src.match(MEM_WRITE) || [];
const inputSources = (dir) =>
    fs.readdirSync(dir, { withFileTypes: true }).flatMap((e) => {
        const p = path.join(dir, e.name);
        if (e.isDirectory()) return inputSources(p);
        return e.name.endsWith(".js") ? [p] : [];
    });
{
    const writes = [];
    for (const file of inputSources(path.join(ROOT, "web/input"))) {
        for (const w of memWrites(fs.readFileSync(file, "utf8")))
            writes.push(`${path.relative(ROOT, file).split(path.sep).join("/")}：${w}`);
    }
    // 单位（反馈 %/dB、延迟 ms/样本）只能由 app.js 里的 setUnit 写；卡片和别处一律经 ctx.setUnit
    run.check(
        writes.length === 2 && writes.every((w) => w.startsWith("web/input/app.js：mem.")),
        `页面内存里的单位只在 app.js 的 setUnit 里写（fb、delay 各一处），实得 ${JSON.stringify(writes)}`,
    );
    run.check(
        memWrites("ctx.mem.fbUnit = id;\nmem.delayUnit = x;\nif (mem.fbUnit === 'db') {}").length === 2,
        "坏样例：卡片里直接写 ctx.mem.fbUnit 应被扫描抓出，比较不算",
    );
}

run.finish();
