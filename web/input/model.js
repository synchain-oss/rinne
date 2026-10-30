// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rinne Input 页面的纯函数：数字格式化、旋钮与滑杆的取值映射、各卡的展示模型、消息位的消息表、导出提示的关闭记录。
// 不碰 DOM、不碰 window，Node 里可以直接 import（smoke-input-model.mjs 单测）。页面的元件和 app.js 只负责把这里算出的
// 结果画出来，判据都在这里，一处改一处准。
//
// 数字规则（与设计稿一致）：等宽数字，负号用 U+2212，dB 一位小数，静音写 −∞。

/** U+2212 减号。 */
export const MINUS = "−";

export const FB_MAX = 150;
/** 限制器关闭时反馈的有效上限。 */
export const FB_CAP = 100;
export const DELAY_MIN_MS = 1;
export const DELAY_MAX_MS = 4000;
export const LOW_CUT = Object.freeze({ min: 20, max: 2000 });
export const HIGH_CUT = Object.freeze({ min: 1000, max: 20000 });
export const CEILING = Object.freeze({ min: -24, max: 0, step: 0.1 });
/** 电平表与 Mix 管的量程下沿（dBFS）。 */
export const LEVEL_FLOOR_DB = -60;
/** 电平表峰值线变红的下沿（dBFS）：高于它画警示色，高于 ALERT_READOUT_DB 时读数也变红。 */
export const PEAK_ALERT_DB = -3;
export const READOUT_ALERT_DB = -0.1;
/** GR 表量程（dB）。 */
export const GR_FULL_DB = 24;
/** GR 超过它画成「压得重」的琥珀。 */
export const GR_HEAVY_DB = 6;
/** 高切允许的上限占采样率的比例（核心按它截断）。 */
export const HIGH_CUT_NYQUIST = 0.45;
/** 超过它的块长，环路停摆（核心的硬上限）。 */
export const MAX_SUPPORTED_BLOCK = 32768;
/** 智能默认提示保留多久（毫秒）。 */
export const SMART_DEFAULT_MS = 10000;
/** 步进回数的 UI 范围：滚轮每 100 px 一格。 */
export const WHEEL_STEP_PX = 100;
/** 拖动一整圈要走的像素数。 */
export const DRAG_FULL_PX = 150;

export const clamp = (x, lo, hi) => Math.min(hi, Math.max(lo, x));
const finite = (x) => typeof x === "number" && Number.isFinite(x);

// ---------------------------------------------------------------- 数字格式化

/** 定点数；负数用 U+2212，且舍入后为 0 的不带负号。 */
export function fmtFixed(x, digits = 1) {
    if (!finite(x)) return "—";
    const s = Math.abs(x).toFixed(digits);
    return (x < 0 && Number(s) !== 0 ? MINUS : "") + s;
}

/** 带符号的定点数：正数带 +，零不带符号。 */
export function fmtSigned(x, digits = 1) {
    if (!finite(x)) return "—";
    const s = Math.abs(x).toFixed(digits);
    if (Number(s) === 0) return s;
    return (x < 0 ? MINUS : "+") + s;
}

/** dBFS 读数：null、非有限或低于量程下沿一律写 −∞。 */
export function fmtLevel(db) {
    if (!finite(db) || db <= LEVEL_FLOOR_DB - 36) return MINUS + "∞";
    return fmtFixed(db, 1);
}

/** 采样率的千赫写法：48000 -> "48"，44100 -> "44.1"，192000 -> "192"。 */
export function fmtKHz(hz) {
    if (!finite(hz) || hz <= 0) return "—";
    return String(Number((hz / 1000).toFixed(1)));
}

/** 频率读数：>= 1000 写千赫一位小数，否则赫兹取整。 */
export function fmtHz(hz) {
    if (!finite(hz)) return "—";
    return hz >= 1000 ? `${(hz / 1000).toFixed(1)} kHz` : `${Math.round(hz)} Hz`;
}

// ---------------------------------------------------------------- 反馈

/** 百分比 -> dB（相对 100% 的增益）；0 给 -Infinity。 */
export function pctToDb(pct) {
    return pct > 0 ? 20 * Math.log10(pct / 100) : -Infinity;
}

/** dB -> 百分比。 */
export function dbToPct(db) {
    return 100 * Math.pow(10, db / 20);
}

/** 反馈的 dB 读数文字（带符号，0 写 −∞）。 */
export function fbDbText(pct) {
    return pct > 0 ? fmtSigned(pctToDb(pct), 1) : MINUS + "∞";
}

/**
 * 反馈读数：主读数、单位、副行。unit 取 pct 或 db。
 * 百分比取整显示；副行永远是另一种单位。
 */
export function feedbackReadout(pct, unit) {
    const p = `${Math.round(pct)}%`;
    const d = `${fbDbText(pct)} dB`;
    return unit === "db" ? { main: d, sub: p } : { main: p, sub: d };
}

/** aria-valuetext：与读数同一套格式化，百分比在前、dB 在后。 */
export function feedbackValueText(pct, lang) {
    return `${Math.round(pct)}%${lang === "zh" ? "，" : ", "}${fbDbText(pct)} dB`;
}

/** 表盘角度：0% 在 -135 度，每 1% 走 1.8 度，100% 在 +45 度，150% 在 +135 度。 */
export function fbAngle(pct) {
    return -135 + clamp(pct, 0, FB_MAX) * 1.8;
}

/** 反馈数值输入：按当前读数单位解析，返回 { value, ok }；超出范围 ok 为假并给出该单位的范围文字。 */
export function parseFeedbackInput(text, unit) {
    const raw = String(text).trim().replace(/−/g, "-");
    if (unit === "db") {
        if (/^-?(inf|∞)$/i.test(raw)) return { ok: true, value: 0 };
        const n = Number(raw.replace(/db$/i, "").trim());
        const lo = -60;
        const hi = pctToDb(FB_MAX);
        if (!finite(n) || raw === "") return { ok: false, min: fmtFixed(lo, 0), max: fmtSigned(hi, 1) };
        if (n < lo || n > hi + 1e-9) return { ok: false, min: fmtFixed(lo, 0), max: fmtSigned(hi, 1) };
        return { ok: true, value: clamp(dbToPct(n), 0, FB_MAX) };
    }
    const n = Number(raw.replace(/%$/, "").trim());
    if (!finite(n) || raw === "" || n < 0 || n > FB_MAX) return { ok: false, min: "0", max: String(FB_MAX) };
    return { ok: true, value: n };
}

/** 反馈单位：cfg.fbUnit 里写 db（任意大小写）是 dB，其余按百分比。 */
export function fbUnitOf(cfg) {
    return cfg && typeof cfg.fbUnit === "string" && cfg.fbUnit.toLowerCase() === "db" ? "db" : "pct";
}

/** 延迟单位：cfg.delayUnit 里写 samples / smp / sample 是样本，其余按毫秒。 */
export function delayUnitOf(cfg) {
    const u = cfg && typeof cfg.delayUnit === "string" ? cfg.delayUnit.toLowerCase() : "";
    return u === "samples" || u === "sample" || u === "smp" ? "samples" : "ms";
}

/** 自激区芯片的混合下限：混合在 50% 及以上时环路增益才可能超过 1。 */
export const SELF_OSC_MIX = 50;

/**
 * 反馈芯片：切断 > 淡入 > 限制器关而设定超过 100%（设定 x%，有效 100%）> 自激区。
 * 自激区只在混合 >= 50% 且反馈超过 100% 时出现；混合较低时越过 100% 只显示红段。
 * 返回 { kind, key, tone, vals } 或 null。
 */
export function feedbackChip({ cut, fadeIn, limiterOn, feedback, mix }) {
    if (cut) return { kind: "cut", key: "in.fb.chip.cut", tone: "red" };
    if (fadeIn) return { kind: "fadeIn", key: "in.fb.chip.fadeIn", tone: "gray" };
    if (!limiterOn && feedback > FB_CAP + 1e-9)
        return { kind: "cap", key: "in.fb.capNote", tone: "amber", vals: { s: String(Math.round(feedback)) } };
    if (feedback > FB_CAP + 1e-9 && mix >= SELF_OSC_MIX)
        return { kind: "selfOsc", key: "in.fb.chip.selfOsc", tone: "amber" };
    return null;
}

// ---------------------------------------------------------------- Mix 推子

/** Mix 的两路比例（线性）：g_dry = min(1, 2(1 - m))，g_ret = min(1, 2m)。 */
export function mixGains(mixPct) {
    const m = clamp(mixPct, 0, 100) / 100;
    return { dry: Math.min(1, 2 * (1 - m)), ret: Math.min(1, 2 * m) };
}

/** 管内一路分量的液柱宽度（占整管的百分比）：量程 -60..0 dBFS 线性，最多占一半，null 为 0。 */
export function mixFill(db) {
    if (!finite(db)) return 0;
    return 50 * clamp((db - LEVEL_FLOOR_DB) / -LEVEL_FLOOR_DB, 0, 1);
}

// ---------------------------------------------------------------- 音色与限制器

/** 低切的表盘位置 0..1（对数，20..2000 Hz）；20 Hz 是「关」，位置为 0。 */
export function lowCutPos(hz) {
    return clamp(
        Math.log(clamp(hz, LOW_CUT.min, LOW_CUT.max) / LOW_CUT.min) / Math.log(LOW_CUT.max / LOW_CUT.min),
        0,
        1,
    );
}

export function lowCutFromPos(p) {
    return LOW_CUT.min * Math.pow(LOW_CUT.max / LOW_CUT.min, clamp(p, 0, 1));
}

/** 高切的表盘位置 0..1（对数，1000..20000 Hz）；20000 Hz 是「关」，位置为 1，弧从末端往回画。 */
export function highCutPos(hz) {
    return clamp(
        Math.log(clamp(hz, HIGH_CUT.min, HIGH_CUT.max) / HIGH_CUT.min) / Math.log(HIGH_CUT.max / HIGH_CUT.min),
        0,
        1,
    );
}

export function highCutFromPos(p) {
    return HIGH_CUT.min * Math.pow(HIGH_CUT.max / HIGH_CUT.min, clamp(p, 0, 1));
}

export const isLowCutOff = (hz) => !(hz > LOW_CUT.min + 1e-6);
export const isHighCutOff = (hz) => !(hz < HIGH_CUT.max - 1e-6);

/** 上限小表盘位置 0..1（-24..0 dBFS）。 */
export function ceilingPos(db) {
    return clamp((db - CEILING.min) / (CEILING.max - CEILING.min), 0, 1);
}

export function ceilingFromPos(p) {
    return Math.round((CEILING.min + clamp(p, 0, 1) * (CEILING.max - CEILING.min)) / CEILING.step) * CEILING.step;
}

/** 小表盘的 aria-valuetext：关写 off 对应的词条由调用方给，这里只出数。 */
export function toneValueText(kind, hz, offText) {
    if (kind === "low" ? isLowCutOff(hz) : isHighCutOff(hz)) return offText;
    return fmtHz(hz);
}

/** 高切被采样率截断后的实际上限（kHz 文字，一位小数）。 */
export function highCutClampKHz(sampleRate) {
    return fmtFixed((HIGH_CUT_NYQUIST * sampleRate) / 1000, 1);
}

/** GR 表：关闭时隐藏；增益缩减为 0 是空闲；压得重（>= 6 dB）画琥珀。gr 为 dB（负数或 0）。 */
export function grView(gr, limiterOn) {
    const mag = finite(gr) ? Math.abs(gr) : 0;
    const state = !limiterOn ? "hidden" : mag === 0 ? "idle" : mag >= GR_HEAVY_DB ? "heavy" : "work";
    return {
        state,
        fillPct: clamp(mag / GR_FULL_DB, 0, 1) * 100,
        readout: state === "hidden" ? "—" : fmtFixed(-mag, 1),
    };
}

/** 三盏灯各状态的颜色与词条键（键写全，方便按字面量查引用）。 */
const LAMP_STATES = {
    dc: {
        ok: { tone: "gray", key: "in.lamp.ok" },
        blocked: { tone: "amber", key: "in.lamp.dc.blocked" },
    },
    nan: {
        ok: { tone: "gray", key: "in.lamp.ok" },
        zeroed: { tone: "amber", key: "in.lamp.nan.zeroed" },
        latched: { tone: "red", key: "in.lamp.nan.latched" },
    },
    run: {
        ok: { tone: "gray", key: "in.lamp.ok" },
        limiting: { tone: "amber", key: "in.lamp.run.limiting" },
        reduced: { tone: "amber", key: "in.lamp.run.reduced" },
        cut: { tone: "red", key: "in.lamp.run.cut" },
    },
};

/** 三盏灯的名字与说明的词条键。 */
export const LAMP_META = Object.freeze({
    dc: { label: "in.lamp.dc", tip: "in.lamp.dc.tip" },
    nan: { label: "in.lamp.nan", tip: "in.lamp.nan.tip" },
    run: { label: "in.lamp.run", tip: "in.lamp.run.tip" },
});

/**
 * 三盏灯的展示模型。lamps 是 rinne.meters 里的 { dc, nonfinite, guard, nonfiniteCount, guardDb }。
 * 不认识的状态串一律当 ok，免得原生侧新增状态时页面画出一个没有词条的灯。
 */
export function lampsView(lamps) {
    const l = lamps || {};
    const pick = (id, raw) => (Object.prototype.hasOwnProperty.call(LAMP_STATES[id], raw) ? raw : "ok");
    const dc = pick("dc", l.dc);
    const nan = pick("nan", l.nonfinite);
    const run = pick("run", l.guard);
    return {
        dc: { id: "dc", state: dc, tone: LAMP_STATES.dc[dc].tone, key: LAMP_STATES.dc[dc].key },
        nan: {
            id: "nan",
            state: nan,
            tone: LAMP_STATES.nan[nan].tone,
            key: LAMP_STATES.nan[nan].key,
            vals: nan === "zeroed" ? { n: String(finite(l.nonfiniteCount) ? l.nonfiniteCount : 0) } : undefined,
        },
        run: {
            id: "run",
            state: run,
            tone: LAMP_STATES.run[run].tone,
            key: LAMP_STATES.run[run].key,
            vals: run === "reduced" ? { x: finite(l.guardDb) ? fmtFixed(l.guardDb, 1) : "—" } : undefined,
        },
    };
}

// ---------------------------------------------------------------- 延迟卡

const NOTE_BEATS = Object.freeze({
    n64: 1 / 16,
    n32t: 1 / 12,
    n32: 1 / 8,
    n16t: 1 / 6,
    n16: 1 / 4,
    n8t: 1 / 3,
    n16d: 3 / 8,
    n8: 1 / 2,
    n4t: 2 / 3,
    n8d: 3 / 4,
    n4: 1,
    n2t: 4 / 3,
    n4d: 3 / 2,
    n2: 2,
    n2d: 3,
    n1: 4,
});

/** 一个时值占几拍；未知时值给 1。 */
export function noteBeats(id) {
    return Object.prototype.hasOwnProperty.call(NOTE_BEATS, id) ? NOTE_BEATS[id] : 1;
}

/** 延迟滑杆位置 0..100（对数，1..4000 ms）。 */
export function delayPos(ms) {
    return (Math.log(clamp(ms, DELAY_MIN_MS, DELAY_MAX_MS)) / Math.log(DELAY_MAX_MS)) * 100;
}

export function delayFromPos(pct) {
    return Math.pow(DELAY_MAX_MS, clamp(pct, 0, 100) / 100);
}

/** 毫秒 <-> 样本。 */
export const msToSamples = (ms, sr) => Math.round((ms * sr) / 1000);
export const samplesToMs = (n, sr) => (n * 1000) / sr;

/**
 * 延迟卡的展示模型。
 * @param {object} p
 * @param {"time"|"sync"} p.mode delay_mode
 * @param {"ms"|"samples"} p.unit 毫秒档的显示单位（页面内存或 cfg.delayUnit）
 * @param {number} p.delayMs delay_ms 的设定值
 * @param {string} p.note delay_note
 * @param {{actualMs:number, minMs:number, reason:string, maxBlock:number, bpm?:number, noTempo?:boolean}} p.delay
 * @param {number} p.sampleRate
 */
export function delayView({ mode, unit, delayMs, note, delay, sampleRate }) {
    const d = delay || {};
    const sr = finite(sampleRate) && sampleRate > 0 ? sampleRate : 48000;
    const srLabel = fmtKHz(sr);
    const bpm = finite(d.bpm) && d.bpm > 0 ? d.bpm : 120;
    const sync = mode === "sync";
    const syncMs = (noteBeats(note) * 60000) / bpm;
    const setMs = sync ? syncMs : delayMs;
    const actualMs = finite(d.actualMs) ? d.actualMs : setMs;
    const minMs = finite(d.minMs) && d.minMs > 0 ? d.minMs : null;
    const segment = sync ? "sync" : unit === "samples" ? "samples" : "ms";

    let readout;
    let sub;
    if (sync) {
        readout = { kind: "note", noteKey: "in.dly.note." + note };
        sub = { key: "in.dly.sub.sync", vals: { d: fmtFixed(syncMs, 1), bpm: String(Math.round(bpm * 100) / 100) } };
    } else if (unit === "samples") {
        readout = { kind: "samples", text: String(msToSamples(setMs, sr)), unitKey: "in.dly.unitSmp" };
        sub = { key: "in.dly.sub.samples", vals: { d: fmtFixed(setMs, 1), sr: srLabel } };
    } else {
        readout = { kind: "ms", text: fmtFixed(setMs, 1), unitKey: "in.dly.unitMs" };
        sub = { key: "in.dly.sub.ms", vals: { n: String(msToSamples(setMs, sr)), sr: srLabel } };
    }

    const raised = actualMs > setMs + 1e-6;
    let badge = null;
    if (d.reason === "overrun") badge = "overrun";
    else if (d.reason === "raised") badge = "raised";
    else if (d.reason === "capped") badge = "clipped";
    else if (sync && d.noTempo === true) badge = "noTempo";

    return {
        segment,
        sync,
        readout,
        sub,
        setMs,
        sliderPct: delayPos(setMs),
        // 最小值刻线与斜纹：最小值未知时不画
        minPct: minMs === null ? null : delayPos(minMs),
        inHatch: minMs !== null && !sync && setMs < minMs - 1e-6,
        raised: raised || d.reason === "raised",
        actualVals: { d: fmtFixed(actualMs, 1) },
        minKnown: minMs !== null,
        minVals: { m: minMs === null ? "—" : fmtFixed(minMs, 1) },
        maxBlockVals: { n: String(finite(d.maxBlock) ? d.maxBlock : 0), sr: srLabel },
        badge,
        state: badge || (minMs === null ? "minUnknown" : segment),
    };
}

/** 延迟数值输入：毫秒档按毫秒、样本档按样本。返回 { ok, ms } 或 { ok:false, min, max }。 */
export function parseDelayInput(text, unit, sampleRate) {
    const raw = String(text).trim().replace(/−/g, "-");
    const n = Number(raw.replace(/\s*(ms|smp|samples?)$/i, ""));
    const sr = finite(sampleRate) && sampleRate > 0 ? sampleRate : 48000;
    const maxSamples = msToSamples(DELAY_MAX_MS, sr);
    const bad =
        unit === "samples"
            ? { ok: false, min: String(Math.max(1, msToSamples(DELAY_MIN_MS, sr))), max: String(maxSamples) }
            : { ok: false, min: String(DELAY_MIN_MS), max: String(DELAY_MAX_MS) };
    if (raw === "" || !finite(n)) return bad;
    const ms = unit === "samples" ? samplesToMs(n, sr) : n;
    if (ms < DELAY_MIN_MS - 1e-9 || ms > DELAY_MAX_MS + 1e-9) return bad;
    return { ok: true, ms: Math.round(clamp(ms, DELAY_MIN_MS, DELAY_MAX_MS) * 1000) / 1000 };
}

// ---------------------------------------------------------------- 对端、声道、配对

/** 对端行里「有对端信息」的判据：有轨道名、颜色或声道数，或者是已连接但宿主不给轨道名。 */
export function peerPresent(pair) {
    if (!pair) return false;
    const p = pair.peer || {};
    return Boolean(p.name || p.color || p.channels || pair.state === "connectedUnverified");
}

/** 声道数 1/2 -> 徽标用的 mono/st；其余给 null。 */
export function channelKind(n) {
    return n === 1 ? "mono" : n === 2 ? "st" : null;
}

/** 轨道颜色只认 #rrggbb，其余当没有。 */
export function trackColor(c) {
    return typeof c === "string" && /^#[0-9a-fA-F]{6}$/.test(c) ? c : null;
}

/** 核验种类：match / diff / unknown，其余当 unknown。 */
export function verifyKind(v) {
    return v === "match" || v === "diff" ? v : "unknown";
}

/** 没有对端的配对态：脉冲不亮，页脚给「下一步」提示。 */
const UNPAIRED = [
    "ipcUnavailable",
    "abiMismatch",
    "srMismatch",
    "conflict",
    "unassigned",
    "searching",
    "waitingAudio",
    "validator",
];
const CONNECTED = ["connected", "connectedUnverified", "connectedStill"];

export const isConnected = (state) => CONNECTED.includes(state);

/** 回送是否在流动：已连接且不是 connectedStill，且没被切断、环路没关、没被宿主旁路。pill 的脉冲靠它。 */
export function isFlowing({ pairState, cut, loopOn, silentCause }) {
    if (cut || loopOn === false || silentCause) return false;
    return pairState === "connected" || pairState === "connectedUnverified";
}

/** 危险描边（L2）：切断、组冲突、版本不匹配。 */
export function isDanger({ cut, pairState, l2 }) {
    return Boolean(cut) || Boolean(l2) || pairState === "conflict" || pairState === "abiMismatch";
}

/**
 * 页脚提示：切断 > 没配上 > 已连接而反馈为 0 > 拖动中 > 就绪。
 * 返回 { state, key }，state 是页脚锚点的 data-state。
 */
export function footerHint({ cut, pairState, feedback, dragging }) {
    if (cut) return { state: "cut", key: "in.hint.cut" };
    if (UNPAIRED.includes(pairState)) return { state: "unpaired", key: "in.hint.unpaired" };
    if (dragging) return { state: "drag", key: "in.hint.drag" };
    if (isConnected(pairState) && feedback === 0) return { state: "zero", key: "in.hint.zero" };
    return { state: "ready", key: "in.hint.ready" };
}

// ---------------------------------------------------------------- 导出提示

/** 原因位。 */
export const EXPORT_BIT = Object.freeze({ violation: 1, overrun: 2, peer: 4, resync: 8 });

/**
 * 导出提示该不该显示：返回 { show, rec }。
 * cur 是桥给的 { bits, count }；rec 是用户关掉时记下的 { bits, count } 或 null（只在本次页面会话内有效）。
 * 条件消失（count 或 bits 为 0）时记录清掉；出现新的原因位、或计数比关掉时更新（无符号 32 位回绕安全），提示重新出现。
 */
export function exportHint(cur, rec) {
    const bits = cur && finite(cur.bits) ? cur.bits >>> 0 : 0;
    const count = cur && finite(cur.count) ? cur.count >>> 0 : 0;
    if (bits === 0 || count === 0) return { show: false, rec: null };
    if (rec === null || rec === undefined) return { show: true, rec: null };
    const newBits = (bits & ~rec.bits) !== 0;
    const newer = ((count - rec.count) | 0) > 0;
    return { show: newBits || newer, rec };
}

/**
 * 导出提示按原因拆成消息，顺序即优先级（宿主违约 > 补 0 > 对端违约 > 重同步）：消息位先显示第一条，
 * 其余的由「+n」轮换。每条都能单独关掉，关掉记的是当下的原因位与计数（见 exportHint）。
 */
export function exportMessages(bits) {
    const b = bits >>> 0;
    const out = [];
    if (b & EXPORT_BIT.violation) out.push({ id: "exportViolation", key: "band.in.exportViolation" });
    if (b & EXPORT_BIT.overrun) out.push({ id: "exportOverrun", key: "band.in.exportOverrun" });
    if (b & EXPORT_BIT.peer) out.push({ id: "exportPeer", key: "band.in.exportPeer" });
    if (b & EXPORT_BIT.resync) out.push({ id: "exportResync", key: "band.in.exportResync" });
    return out;
}

// ---------------------------------------------------------------- 消息位

const CUT_KEYS = Object.freeze({
    panic: "band.in.cutPanic",
    nan: "band.in.cutNan",
    huge: "band.in.cutHuge",
    runaway: "band.in.cutRunaway",
});

const msg = (id, key, tone, extra) => ({ id, key, tone, ...extra });

/**
 * 当前成立的全部消息（红、琥珀、灰、紫、中性各自按这里给的顺序排；跨色的优先级由消息位组件排）。
 *
 * @param {object} c
 * @param {object} c.state rinne.state 最近一帧
 * @param {object} c.pair rinne.pair 最近一帧
 * @param {object} c.values 参数当前值（回声）
 * @param {object} c.meters rinne.meters 最近一帧（可为 null）
 * @param {{bits:number,count:number}|null} c.exportRec 用户关掉导出提示时的记录
 * @param {boolean} c.touchedFeedback 本会话用户调过反馈
 * @param {boolean} c.smartActive 智能默认刚认领组、还在 10 秒内
 * @returns {{messages:Array, confirm:null|{key:string, vals?:object}}}
 */
export function deriveMessages(c) {
    const state = c.state || {};
    const pair = c.pair || {};
    const values = c.values || {};
    const meters = c.meters || {};
    const lamps = meters.lamps || {};
    const delay = state.delay || {};
    const safety = state.safety || {};
    const dismissed = new Set(Array.isArray(state.dismissedHints) ? state.dismissedHints : []);
    const peer = pair.peer || {};
    const ps = pair.state;
    const out = [];

    // ---- 红
    if (safety.cut) {
        const hot = safety.cut === "panic" && values.feedback > FB_CAP;
        const key = hot ? "band.in.cutPanicHot" : CUT_KEYS[safety.cut] || CUT_KEYS.panic;
        out.push(
            msg(hot ? "cutPanicHot" : `cut${safety.cut[0].toUpperCase()}${safety.cut.slice(1)}`, key, "red", {
                action: { name: "restore", cooldown: finite(safety.cooldownMs) && safety.cooldownMs > 0 },
            }),
        );
    }
    if (ps === "ipcUnavailable") out.push(msg("ipc", "band.in.ipc", "red", { action: { name: "guide" } }));
    if (ps === "abiMismatch")
        out.push(
            msg("abi", "band.in.abi", "red", {
                vals: { a: textOr(state.version), b: textOr(peer.version) },
                action: { name: "guide" },
            }),
        );
    if (ps === "srMismatch")
        out.push(
            msg("srMismatch", "band.in.srMismatch", "red", {
                vals: { a: textOr(state.sampleRate), b: textOr(peer.sampleRate) },
            }),
        );
    if (ps === "conflict")
        out.push(
            msg("conflict", "band.in.conflict", "red", {
                vals: { g: textOr(state.group) },
                action: { name: "moveFree" },
            }),
        );
    if (ps === "unassigned") out.push(msg("unassigned", "band.in.unassigned", "red"));
    if (finite(delay.maxBlock) && delay.maxBlock > MAX_SUPPORTED_BLOCK)
        out.push(msg("unsupportedBlock", "band.in.unsupportedBlock", "red", { vals: { n: String(delay.maxBlock) } }));

    // ---- 琥珀
    if (lamps.guard === "reduced")
        out.push(
            msg("guardActive", "band.in.guardActive", "amber", {
                vals: { x: finite(lamps.guardDb) ? fmtFixed(lamps.guardDb, 1) : "—" },
            }),
        );
    else if (lamps.guard === "limiting") out.push(msg("limiting", "band.in.limiting", "amber"));
    if (ps === "stale") out.push(msg("stale", "band.in.stale", "amber"));
    if (state.silentCause === "stalled") out.push(msg("stalled", "band.in.stalled", "amber"));
    if (delay.reason === "overrun")
        out.push(
            msg("hostViolation", "band.in.hostViolation", "amber", {
                vals: { n: String(finite(delay.maxBlock) ? delay.maxBlock : 0), d: fmtFixed(delay.actualMs, 1) },
                action: { name: "guide" },
            }),
        );
    else if (delay.reason === "raised")
        out.push(
            msg("dRaised", "band.in.dRaised", "amber", {
                vals: { m: fmtFixed(delay.minMs, 1) },
                action: { name: "guide" },
            }),
        );
    const exp = exportHint(state.exportCauses, c.exportRec === undefined ? null : c.exportRec);
    if (exp.show) {
        for (const m of exportMessages(state.exportCauses.bits)) out.push(msg(m.id, m.key, "amber", { dismiss: true }));
    }
    if (values.limiter === false)
        out.push(msg("limiterOff", "band.in.limiterOff", "amber", { action: { name: "limOn" } }));
    if (ps === "crossProcess")
        out.push(msg("crossProcess", "band.in.crossProcess", "amber", { action: { name: "guide" } }));
    if (ps === "crossTrack")
        out.push(
            msg("crossTrack", "band.in.crossTrack", "amber", {
                vals: { peer: textOr(peer.name), self: textOr((pair.self || {}).name) },
            }),
        );
    if (lamps.nonfinite === "zeroed")
        out.push(
            msg("ringNonfinite", "band.in.ringNonfinite", "amber", {
                vals: { n: String(finite(lamps.nonfiniteCount) ? lamps.nonfiniteCount : 0) },
            }),
        );

    // ---- 灰
    if (state.silentCause === "hostBypass") out.push(msg("bypassed", "band.in.bypassed", "gray"));
    else if (state.silentCause === "peerBypass") out.push(msg("outputBypassed", "band.in.outputBypassed", "gray"));
    else if (state.silentCause === "hostPaused") out.push(msg("hostIdle", "band.in.hostIdle", "gray"));
    if (values.loop_on === false) out.push(msg("loopOff", "band.in.loopOff", "gray"));
    if (ps === "waitingAudio") out.push(msg("waitingAudio", "band.in.waitingAudio", "gray"));
    if (ps === "validator") out.push(msg("validator", "band.in.validator", "gray"));

    // ---- 紫（可以关掉：id 同时是 dismissHint 的参数）
    const violet = (id, key, extra) => {
        if (!dismissed.has(id)) out.push(msg(id, key, "violet", { dismiss: true, ...extra }));
    };
    if (ps === "searching") {
        const others = otherGroupsWithPeer(pair.groupMask, state.group);
        if (others.length > 0) {
            violet("switchHint", "band.in.switchHint", {
                vals: { g: textOr(state.group), y: others[0] },
                action: { name: "moveTo", vals: { y: others[0] } },
                target: others[0],
            });
        } else {
            violet("nextStep", "band.in.nextStep", { vals: { g: textOr(state.group) }, chain: true });
        }
    }
    if (c.smartActive && isConnected(ps) && state.groupOrigin === "auto" && state.group)
        violet("smartDefault", "band.in.smartDefault", { vals: { g: state.group } });
    if (isConnected(ps) && values.feedback === 0 && !c.touchedFeedback) violet("linkedZero", "band.in.linkedZero");

    // ---- 中性
    if (meters.highCutLimited === true)
        out.push(
            msg("highCutClamped", "band.in.highCutClamped", "neutral", {
                vals: { f: highCutClampKHz(state.sampleRate) },
            }),
        );
    if (values.limiter === false && values.feedback > FB_CAP + 1e-9)
        out.push(
            msg("limiterCap100", "band.in.limiterCap100", "neutral", {
                vals: { s: String(Math.round(values.feedback)) },
            }),
        );
    if (out.length === 0 && (ps === "connected" || ps === "connectedUnverified" || ps === "connectedStill")) {
        out.push(
            msg("summary", "band.in.summary", "neutral", {
                vals: { d: fmtFixed(delay.actualMs, 1), c: fmtFixed(values.ceiling, 1) },
            }),
        );
    }

    const confirm = state.takeover === true ? { key: "band.in.takeover", vals: { g: textOr(state.group) } } : null;
    return { messages: out, confirm };
}

/** 占位符取值：数字或非空字符串转文字，其余写破折号（占位符绝不能原样上屏）。 */
function textOr(v) {
    if (typeof v === "number" && Number.isFinite(v)) return String(v);
    return typeof v === "string" && v !== "" ? v : "—";
}

const LETTERS = ["A", "B", "C", "D", "E", "F", "G", "H"];

/** groupMask 里有未配对对端等候、且不是本组的组字母，按字母序。 */
export function otherGroupsWithPeer(mask, group) {
    const m = finite(mask) ? mask >>> 0 : 0;
    return LETTERS.filter((g, i) => g !== group && (m & (1 << i)) !== 0);
}

/** groupMask 里全部有等候对端的组字母（组选择条的小点）。 */
export function groupsWithPeer(mask) {
    const m = finite(mask) ? mask >>> 0 : 0;
    return LETTERS.filter((_, i) => (m & (1 << i)) !== 0);
}

// ---------------------------------------------------------------- 电平

/** 电平条填充比例 0..100。 */
export function levelPct(db) {
    return finite(db) ? clamp(((db - LEVEL_FLOOR_DB) / -LEVEL_FLOOR_DB) * 100, 0, 100) : 0;
}

/** 峰值线状态：高于 -3 dBFS 为 near（警示色），高于 -0.1 为 over（读数也变红）。 */
export function peakState(peakDb) {
    if (!finite(peakDb)) return "none";
    return peakDb > READOUT_ALERT_DB ? "over" : peakDb > PEAK_ALERT_DB ? "near" : "normal";
}

// ---------------------------------------------------------------- 音色数值输入

/**
 * 低切、高切的数值输入：纯数字按赫兹，也认「250 Hz」「6k」「6.5 kHz」；「关」「off」回到不切的一端。
 * 返回 { ok:true, value } 或 { ok:false, min, max }（范围文字按赫兹）。
 */
export function parseToneInput(text, kind) {
    const range = kind === "low" ? LOW_CUT : HIGH_CUT;
    const bad = { ok: false, min: String(range.min), max: String(range.max) };
    const raw = String(text).trim().toLowerCase();
    if (raw === "关" || raw === "off") return { ok: true, value: kind === "low" ? LOW_CUT.min : HIGH_CUT.max };
    const m = /^(\d+(?:\.\d+)?)\s*(k|khz|hz)?$/.exec(raw);
    if (!m) return bad;
    const hz = Number(m[1]) * (m[2] === "k" || m[2] === "khz" ? 1000 : 1);
    if (!finite(hz) || hz < range.min - 1e-9 || hz > range.max + 1e-9) return bad;
    return { ok: true, value: Math.round(clamp(hz, range.min, range.max)) };
}
