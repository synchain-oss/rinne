// SPDX-License-Identifier: GPL-3.0-or-later
//
// 没有桥时的替身与默认快照。
//
// 三种情形里只有一种用得到它：既没有预览注入的 mock、也没有 JUCE 后端（纯浏览器直接打开页面，比如首帧冒烟与离线扫描），
// 没有桥可接，页面以默认状态显示，不报错。真宿主里 createBridge 失败是真故障，由 app.js 让异常冒出去，不走这里；
// 桥已接上但迟迟不给快照时也不用默认快照：那是宿主的值还没到，不是「没有值」，默认值当成真值会让用户在一份不存在的状态上操作，
// 页面改为只画标签与「—」并关掉所有写入（见 app.js 的 showPending），等真快照。

import { BRIDGE_FUNCTIONS } from "../shared/bridge.js";

/** 默认快照的序号：比任何真快照的序号都小，真快照一到就盖过它。 */
const DEFAULT_SEQ = -1;

/** 默认的四路快照：未配对、各参数取默认值、没有电平。 */
export function defaultFrames() {
    return {
        state: {
            seq: DEFAULT_SEQ,
            version: "",
            lang: "zh",
            langChosen: true,
            rulesAck: null,
            guideSeen: true,
            uiScale: 1,
            restored: false,
            takeover: false,
            group: null,
            groupOrigin: "manual",
            diagLog: false,
            sampleRate: 48000,
            cfg: { resetOnPlay: true, tailMode: "auto", delayUnit: "ms", fbUnit: "pct", guardOn: true },
            preset: { id: "init", dirty: false, name: null },
            delay: { actualMs: 100, minMs: 0, reason: "ok", maxBlock: 0 },
            safety: { cut: null, cooldownMs: 0, l2: false },
            dismissedHints: [],
            exportCauses: { bits: 0, count: 0 },
            silentCause: null,
            notices: [],
        },
        pair: {
            seq: DEFAULT_SEQ,
            state: "searching",
            groupMask: 0,
            peer: { name: null, color: null, channels: null, version: null, holder: null },
            self: { name: null },
            verify: "unknown",
        },
        params: {
            seq: DEFAULT_SEQ,
            values: {
                feedback: 0,
                loop_on: true,
                mix: 25,
                delay_mode: "time",
                delay_ms: 100,
                delay_note: "n4",
                low_cut: 20,
                high_cut: 20000,
                polarity: false,
                limiter: true,
                ceiling: -6,
            },
        },
        meters: {
            seq: DEFAULT_SEQ,
            in: null,
            ret: null,
            out: null,
            dryPart: null,
            retPart: null,
            gr: 0,
            highCutLimited: false,
            lamps: { dc: "ok", nonfinite: "ok", guard: "ok" },
        },
    };
}

/** 无桥时的替身：每个函数都回「没准备好」，事件订阅是空操作。 */
export function createOfflineBridge() {
    const bridge = { role: "input", isPreview: false, on: () => () => {} };
    for (const name of BRIDGE_FUNCTIONS.input) bridge[name] = async () => ({ ok: false, reason: "not-ready" });
    return bridge;
}
