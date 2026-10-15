// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/web.json
//
// scale-ui.js：页脚缩放下拉的状态机，两个插件共用。只含状态与时序，不建 DOM、不读词条，
// 页面把下拉、确认框和倒计时数字接到本模块上。
//
// 流程：
//   1. options() 按 DESIGN[role].presets（Input 7 档、Output 10 档）生成下拉选项；放不下当前屏幕的档位
//      置灰（判据见 scaleOverflows），并带上提示词条的键；旧 rect 语义下只剩 1 档（见 shell-fit.js）。
//   2. 选档：preview(f) 记下选档前的档位，调 setUiScale(f)，C++ 立即 setSize，同时进入确认态，
//      开始 10 秒倒计时。确认态里再选别的档位只重置倒计时，回退目标仍是进入确认态之前的档位。
//   3. 保存：keep() 停倒计时并调 commitUiScale()，档位随实例 state 保存。
//   4. 回退：revert() 或倒计时归零，停倒计时并调 setUiScale(选档前的档位)。
//   5. pagehide：只停计时，不发任何桥调用。此时编辑器正在析构，未保存的预览档自然丢弃。
// 倍率本身不在这里：页面缩放由 shell-fit.js 按实际视口算出，档位只决定窗口尺寸。

import { DESIGN } from "./design-box.js";
import { scalePresets } from "./shell-fit.js";

/** 确认态的倒计时秒数。 */
export const CONFIRM_SECONDS = 10;

/** 本状态机用到的词条键，页面照键取词条；keep 的值与 common.scale.keep 同义。 */
export const SCALE_KEYS = Object.freeze({
    overflow: "common.scale.overflow",
    confirmBody: "common.scale.confirmBody",
    keep: "common.scale.keep",
    revert: "common.cancel",
    lockedLegacy: "common.scale.lockedLegacy",
});

/**
 * 某个档位在当前屏幕上是否放不下。窗口尺寸是设计盒乘档位，外加宿主给插件窗加的框。
 * 屏幕可用区未知（缺 availWidth 或 availHeight）时不判溢出。
 * @param {"input"|"output"} role
 * @param {number} f 档位
 * @param {{availWidth?:number, availHeight?:number}|null} screen
 */
export function scaleOverflows(role, f, screen) {
    const box = DESIGN[role];
    const frame = DESIGN.hostFrame;
    if (!box || !screen || !screen.availWidth || !screen.availHeight) {
        return false;
    }
    return box.w * f + frame.w > screen.availWidth || box.h * f + frame.h > screen.availHeight;
}

/**
 * @param {object} o
 * @param {"input"|"output"} o.role
 * @param {{setUiScale:(f:number)=>Promise<any>, commitUiScale:()=>unknown}} o.bridge
 * @param {()=>number} o.getScale 当前已生效的档位（来自 state 回推），取不到时按 1
 * @param {()=>({availWidth?:number, availHeight?:number}|null)} [o.getScreen]
 * @param {(s:{confirming:boolean, left:number})=>void} [o.onChange] 确认态或剩余秒数变了就回调
 * @param {(e:unknown)=>void} [o.onError] 倒计时到点回退时桥调用失败（比如原生侧还没登记这个函数）就回调
 * @param {{setInterval:Function, clearInterval:Function}} [o.timers] 注入用，缺省取 globalThis
 */
export function createScaleUi(o) {
    const { role, bridge } = o;
    if (!DESIGN[role] || !Array.isArray(DESIGN[role].presets)) {
        throw new RangeError(`createScaleUi：未知角色 ${role}`);
    }
    const timers = o.timers || globalThis;
    const getScale = typeof o.getScale === "function" ? o.getScale : () => 1;
    const getScreen = typeof o.getScreen === "function" ? o.getScreen : () => null;
    const onChange = typeof o.onChange === "function" ? o.onChange : () => {};
    const onError = typeof o.onError === "function" ? o.onError : () => {};

    // 确认态从发出预览的那一刻起就算，不等桥调用返回：桥调用返回之前用户可能连按方向键再选档位，
    // 或者点了保存。gen 是代次，每次进入、保存、取消、卸载都加一，过期的预览返回后据此放弃，
    // 不再起计时器，也不改写回退目标。
    let active = false;
    let gen = 0;
    let timer = 0;
    let prev = 1;
    let left = 0;

    const notify = () => onChange({ confirming: active, left });

    function stop() {
        gen += 1;
        active = false;
        if (timer) timers.clearInterval(timer);
        timer = 0;
        left = 0;
        notify();
    }

    async function revert() {
        if (!active) return;
        const target = prev;
        stop();
        await bridge.setUiScale(target);
    }

    return {
        /** 下拉选项：[{ factor, label, disabled, titleKey }]；titleKey 只在置灰时有值。 */
        options() {
            const screen = getScreen();
            return scalePresets(DESIGN[role].presets).map((f) => {
                const disabled = scaleOverflows(role, f, screen);
                return {
                    factor: f,
                    label: Math.round(f * 100) + "%",
                    disabled,
                    titleKey: disabled ? SCALE_KEYS.overflow : null,
                };
            });
        },

        /**
         * 选档：立即预览，进入确认态并开始倒计时。回退目标是进入确认态之前的档位，确认态里再选档位不会改写它。
         * 桥调用被拒（ok 为 false）或抛错时：从未起过倒计时就退出确认态；已经有倒计时在跑就让它继续，
         * 此时窗口停在上一次成功预览的档位上，仍然会被倒计时或用户的取消带回去。
         * 返回 true 表示这次预览生效并起了倒计时；桥调用抛错时交给 onError，返回 false。
         */
        async preview(f) {
            if (!active) {
                prev = getScale();
                active = true;
            }
            const mine = ++gen;
            let res;
            try {
                res = await bridge.setUiScale(f);
            } catch (e) {
                if (mine === gen && !timer) stop();
                onError(e);
                return false;
            }
            if (mine !== gen) return false;
            if (res && res.ok === false) {
                if (!timer) stop();
                return false;
            }
            left = CONFIRM_SECONDS;
            if (timer) timers.clearInterval(timer);
            timer = timers.setInterval(() => {
                left -= 1;
                if (left <= 0) {
                    revert().catch(onError);
                } else {
                    notify();
                }
            }, 1000);
            notify();
            return true;
        },

        /** 保存：停倒计时，档位随实例 state 保存。预览还没返回时保存，会让那次预览返回后不再起倒计时。 */
        keep() {
            stop();
            return bridge.commitUiScale();
        },

        /** 取消：停倒计时，回到选档前的档位。不在确认态时什么也不做。 */
        revert,

        /** 页面卸载：只停计时，不发任何桥调用。 */
        pagehide: stop,

        /** 当前状态的快照。 */
        state() {
            return { confirming: active, left, prev };
        },
    };
}
