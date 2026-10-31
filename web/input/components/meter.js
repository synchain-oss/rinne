// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/web.json
//
// 电平弹道：输入、回送、输出三根电平管和 Mix 推子管内的两路分量，共用一套弹道。
//
// 口径（四条，缺一条都会和别处的电平表对不上）：
//   1. 地板 -60 dB：低于地板一律按地板画（静音的电平帧给 null，也按地板）；
//   2. 上行瞬时跟随，不做 attack 平滑；下行 fast-follow，120 dB/s；
//   3. 峰值保持 2200 ms，到点后按 20 dB/s 衰减；
//   4. 峰线永不低于当前液柱，也不穿地板。
// 液柱和峰线的 CSS 过渡只能是一个事件间隔（33 ms，--dur-meter-tube）：更长的过渡会在弹道之上再叠一条更慢的弹道，
// 柱头就够不着峰线。过渡时长与 base.css 的 .sc-tube__liquid / .sc-tube__peak 成对，不在这里改。
//
// 渲染由调用方给的 write(id, state) 完成，本文件不碰 DOM：rAF 每帧按真实 dt 推进各路弹道；弹道停在不动点时
// 自停（空闲零 rAF），新的电平帧到达再起帧。没有 requestAnimationFrame 的环境（首帧回归测试里被故意删掉）
// 退化成每次电平帧到达时同步推进一次。

/** 电平地板。 */
export const METER_FLOOR_DB = -60;
/** 回落速度（dB/s）。 */
export const FALL_DB_PER_S = 120;
/** 峰值保持时长（毫秒）。 */
export const PEAK_HOLD_MS = 2200;
/** 保持期过后的峰线衰减速度（dB/s）。 */
export const PEAK_DECAY_DB_PER_S = 20;

/** dB 转 0..1 的管内比例（地板映到 0，0 dB 映到 1）。 */
export function dbToRatio(db) {
    const v = Number.isFinite(db) ? db : METER_FLOOR_DB;
    const r = (v - METER_FLOOR_DB) / -METER_FLOOR_DB;
    return r < 0 ? 0 : r > 1 ? 1 : r;
}

/** 一路的复位态：全部落到地板。 */
export function restState() {
    return { db: METER_FLOOR_DB, peakDb: METER_FLOOR_DB, peakHeldMs: 0 };
}

function floorDb(v) {
    return v < METER_FLOOR_DB ? METER_FLOOR_DB : v > 0 ? 0 : v;
}

/**
 * 单路一帧的弹道推进（纯函数）。顺序即依赖顺序，换序会算错峰值保持。
 * @param {{db:number, peakDb:number, peakHeldMs:number}|null} prev 上一帧状态
 * @param {number|null} targetDb 本帧电平（dBFS；null 或非有限按地板）
 * @param {number} dtMs 距上一帧的毫秒数
 */
export function advance(prev, targetDb, dtMs) {
    const base = prev || restState();
    const dt = Number.isFinite(dtMs) && dtMs > 0 ? dtMs : 0;
    const target = floorDb(Number.isFinite(targetDb) ? targetDb : METER_FLOOR_DB);

    const prevDb = Number.isFinite(base.db) ? base.db : METER_FLOOR_DB;
    const db = target >= prevDb ? target : Math.max(target, prevDb - (FALL_DB_PER_S * dt) / 1000);

    const prevPeak = Number.isFinite(base.peakDb) ? base.peakDb : METER_FLOOR_DB;
    let peakDb = prevPeak;
    let peakHeldMs = Number.isFinite(base.peakHeldMs) ? base.peakHeldMs : 0;
    if (target >= prevPeak) {
        peakDb = target;
        peakHeldMs = 0;
    } else {
        peakHeldMs += dt;
        if (peakHeldMs > PEAK_HOLD_MS) {
            // 只对超出保持期的那部分时间计衰减，否则跨过阈值的那一帧会多衰减一截
            const decayMs = Math.min(dt, peakHeldMs - PEAK_HOLD_MS);
            peakDb = prevPeak - (PEAK_DECAY_DB_PER_S * decayMs) / 1000;
        }
    }
    return { db: floorDb(db), peakDb: floorDb(Math.max(peakDb, db)), peakHeldMs };
}

const sameState = (a, b) => a.db === b.db && a.peakDb === b.peakDb && a.peakHeldMs === b.peakHeldMs;

/**
 * 弹道渲染器。
 * @param {object} o
 * @param {string[]} o.ids 各路 id
 * @param {(id:string, st:{db:number, peakDb:number, peakHeldMs:number})=>void} o.write 把一路的状态写到 DOM
 * @param {(cb:Function)=>number} [o.raf] 缺省取全局 requestAnimationFrame
 * @param {(id:number)=>void} [o.caf]
 * @param {()=>number} [o.now] 无 rAF 时的时钟
 */
export function createMeterRenderer(o) {
    const hasRaf = o.raf !== undefined ? Boolean(o.raf) : typeof globalThis.requestAnimationFrame === "function";
    const raf = o.raf || ((cb) => globalThis.requestAnimationFrame(cb));
    const caf = o.caf || ((id) => globalThis.cancelAnimationFrame && globalThis.cancelAnimationFrame(id));
    const now = o.now || (() => (typeof performance !== "undefined" ? performance.now() : Date.now()));
    const state = new Map(o.ids.map((id) => [id, restState()]));
    /** 各路最近一帧的目标电平。 */
    const latest = new Map(o.ids.map((id) => [id, null]));
    let rafId = 0;
    let lastMs = null;
    let settled = false;

    /** 单帧推进；返回有没有可见变化（rAF 据此续帧或自停）。 */
    function tick(ts) {
        const dt = lastMs === null ? 0 : ts - lastMs;
        lastMs = ts;
        let moved = false;
        for (const id of o.ids) {
            const prev = state.get(id);
            const next = advance(prev, latest.get(id), dt);
            state.set(id, next);
            if (!sameState(prev, next)) moved = true;
            o.write(id, next);
        }
        settled = !moved;
        return moved;
    }

    function start() {
        if (!hasRaf) {
            tick(now());
            return;
        }
        if (rafId) return;
        lastMs = null;
        // 起帧后的第一帧 dt 恒为 0，下行一步不走；它不参与自停判据，否则回落会卡在瞬态电平上
        let first = true;
        const loop = (ts) => {
            rafId = 0;
            const moved = tick(ts);
            if (moved || first) rafId = raf(loop);
            else lastMs = null; // 时间基线清掉：重新起帧的第一帧 dt 为 0
            first = false;
        };
        rafId = raf(loop);
    }

    return {
        tick,
        /** 电平帧到达：frame 是 { id: dB|null }，缺的 id 保持上一值。 */
        push(frame) {
            let same = true;
            for (const id of o.ids) {
                if (!Object.prototype.hasOwnProperty.call(frame, id)) continue;
                const v = Number.isFinite(frame[id]) ? frame[id] : null;
                if (latest.get(id) !== v) same = false;
                latest.set(id, v);
            }
            if (settled && same) return; // 弹道已停在不动点而且电平没变：起帧也画不出变化
            start();
        },
        /** 清除一路的峰值保持（点读数）：峰线落到当前液柱。 */
        clearPeak(id) {
            const st = state.get(id);
            if (!st) return;
            const next = { ...st, peakDb: st.db, peakHeldMs: 0 };
            state.set(id, next);
            o.write(id, next);
        },
        /** 全部落回地板。 */
        reset() {
            if (rafId) caf(rafId);
            rafId = 0;
            lastMs = null;
            settled = false;
            for (const id of o.ids) {
                latest.set(id, null);
                const st = restState();
                state.set(id, st);
                o.write(id, st);
            }
        },
        stateOf: (id) => state.get(id),
    };
}
