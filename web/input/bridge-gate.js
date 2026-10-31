// SPDX-License-Identifier: GPL-3.0-or-later
//
// 写入闸：宿主的第一批快照到齐之前，页面不对宿主写任何东西。
//
// 为什么要闸：页面此时手里没有宿主的真值。任何一次写入（拖一下旋钮、点一下开关、选一个组）都是在一份
// 不存在的状态上做决定，写过去的不是用户想要的结果。所以这一批桥函数在快照到齐之前一律回「没准备好」，
// 请求根本不发给宿主；界面上对应的控件另有置灰与 inert（见 app.js 的 setLocked），这里是兜底的那一层，
// 不依赖任何一个控件记得去查「现在能不能写」。
//
// 不在名单里的：requestInitialState（要靠它拿快照）、panic（任何时刻都必须能切断）、setLang（写的是用户刚选的语言，
// 没有要回退的旧值）、setRulesAck / setGuideSeen（首启确认，不读快照）、以及只读或外跳的 copyDiagnostics、
// openExternal、showLicense。缩放（setUiScale、commitUiScale）在名单里：选档要记下选档前的档位，倒计时到点或点
// 取消时写回去，快照没到时这个回退目标是编出来的，写回去的就是假值。

/** 快照到齐之前被挡住的桥函数：参数写入、限制器、守卫、组、恢复与接管、预设、缩放、关于面板里的高级设置。 */
export const GATED_WRITES = Object.freeze([
    "beginParamGesture",
    "setParam",
    "endParamGesture",
    "setLimiter",
    "setGuardOn",
    "setGroup",
    "resumeLoop",
    "moveToFreeGroup",
    "resolveTakeover",
    "dismissHint",
    "loadPreset",
    "savePreset",
    "deletePreset",
    "setResetOnPlay",
    "setTailMode",
    "setDiagLog",
    "setUiScale",
    "commitUiScale",
]);

/**
 * 包一层：名单里的函数在 isLocked() 为真时直接回 { ok: false, reason: "not-ready" }，不转给原桥；
 * 其余成员（on、role、requestInitialState、panic……）原样透传。原桥上没有的名字不补，保持特性探测的结果。
 * @param {object} raw createBridge 的返回值（或同形替身）
 * @param {()=>boolean} isLocked
 */
export function gateBridge(raw, isLocked) {
    const gated = { ...raw };
    for (const name of GATED_WRITES) {
        const fn = raw[name];
        if (typeof fn !== "function") continue;
        gated[name] = (...args) =>
            isLocked() ? Promise.resolve({ ok: false, reason: "not-ready" }) : fn.apply(raw, args);
    }
    return gated;
}
