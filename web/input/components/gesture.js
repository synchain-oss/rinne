// SPDX-License-Identifier: GPL-3.0-or-later
//
// 旋钮与推子的通用交互：拖动、滚轮、键盘、双击复位、进入数值输入。反馈大旋钮、两只音色小表盘、上限小表盘、Mix 推子、
// 延迟滑杆都挂它，差别只在取值空间（位置 <-> 数值的映射）和几个开关。
//
// 全部在「位置空间」里算，位置是 0..1 的小数：
//   拖动   纵向（旋钮）或横向（推子）拖 150 视口 px 走完全程，Shift 为 0.1 倍。用 clientX / clientY 的增量，不随外壳缩放变。
//          推子按下就跟手（卡箍落到指针处）；Shift 时改为相对微调。
//          gate：位置空间里的一道闸（反馈的 100%），越过它要多拖 gate.px 个视口 px，让用户能停在 100%。
//   滚轮   先经 wheelPx 归一，累计满 wheelPxPerStep 走一步；到上限（cap）后累计清零，不会攒着等反向。
//   键盘   方向键 1 步，PgUp / PgDn 10 步，Home / End 到两端；cap 只限制向上的走法（反馈的 100% 闸）。
//   复位   双击回默认值；单击读数要等双击判定窗口过去才进入数值输入；Ctrl（mac：Cmd）+ 单击立即进入；有焦点时按 Enter 也进入。
//   回滚   pointercancel 时显示回到起点并把起点写回。
// 拖动、滚轮、键盘都经 param-echo 的手势协议写桥。

import { wheelPx } from "../../shared/wheel.js";
import { DRAG_FULL_PX } from "../model.js";
/** 拖动超过这个距离才算拖动，之前的按下抬起算点击。 */
export const DRAG_SLOP_PX = 3;
/** 单击读数后等多久才进入数值输入（毫秒），在这之内再点一下就是双击。 */
export const DBLCLICK_MS = 300;

const clamp01 = (x) => Math.min(1, Math.max(0, x));

/**
 * 拖动时的位置推进。纯函数，状态（hold）由调用方持有，方便单测。
 *
 * @param {{pos:number, hold:number, dir:number}} s 当前位置、闸上还要停留的 px、停留时的前进方向
 * @param {number} dpx 本次沿拖动轴的视口位移（向上或向右为正）
 * @param {number} k 精细系数（Shift 为 0.1）
 * @param {{pos:number, px:number}} [gate] 闸的位置与宽度
 * @returns {{pos:number, hold:number, dir:number}}
 */
export function advanceDrag(s, dpx, k, gate) {
    if (dpx === 0) return s;
    let pos = s.pos;
    let hold = s.hold;
    let dir = s.dir;
    let move = dpx;
    if (hold > 0) {
        if (Math.sign(move) === dir) {
            // 还在顶着闸往同一方向拖：吃掉停留量
            const left = hold - Math.abs(move);
            if (left > 0) return { pos: gate.pos, hold: left, dir };
            move = dir * -left;
            hold = 0;
            pos = gate.pos;
        } else {
            hold = 0; // 反向：直接离开闸
        }
    }
    let next = clamp01(pos + (move * k) / DRAG_FULL_PX);
    if (gate && ((pos < gate.pos && next > gate.pos) || (pos > gate.pos && next < gate.pos))) {
        const d = Math.sign(next - pos);
        const toGate = (Math.abs(gate.pos - pos) * DRAG_FULL_PX) / k; // 走到闸上用掉的视口 px
        const left = gate.px - (Math.abs(move) - toGate);
        if (left > 0) return { pos: gate.pos, hold: left, dir: d };
        next = clamp01(gate.pos + (d * -left * k) / DRAG_FULL_PX);
    }
    return { pos: next, hold: 0, dir: 0 };
}

/**
 * @param {HTMLElement} el 接收事件的元素（要可聚焦，role=slider 由调用方设置）
 * @param {object} o
 * @param {object} o.param param-echo 的参数对象
 * @param {(v:number)=>number} o.toPos 数值 -> 位置
 * @param {(p:number)=>number} o.fromPos 位置 -> 数值
 * @param {(v:number)=>number} [o.quantize] 写桥之前的取整
 * @param {"v"|"h"} [o.axis] 拖动轴：v 纵向（缺省），h 横向
 * @param {HTMLElement} [o.track] 绝对定位的轨道（推子）；给了就按指针位置跟手
 * @param {number} o.stepPos 一步的位置增量
 * @param {number} [o.wheelPxPerStep] 缺省 40
 * @param {number} o.def 默认值（双击复位）
 * @param {number} [o.cap] 滚轮、方向键、PgUp、End 向上不超过它
 * @param {{pos:number, px:number}} [o.gate] 拖动闸（位置空间里的位置，越过要多拖的视口 px）
 * @param {HTMLElement} [o.readout] 读数元素：单击它进入数值输入
 * @param {()=>void} [o.onEdit] 进入数值输入
 * @param {()=>boolean} [o.disabled]
 * @param {(on:boolean)=>void} [o.onDrag] 指针拖动开始或结束
 * @param {{setTimeout:Function, clearTimeout:Function}} [o.timers]
 * @returns {{destroy:()=>void, dragging:()=>boolean}}
 */
export function attachControl(el, o) {
    const timers = o.timers || globalThis;
    const axis = o.axis || "v";
    const pxPerStep = o.wheelPxPerStep || 40;
    const quantize = o.quantize || ((v) => v);
    const value = () => o.param.value();
    const disabled = () => Boolean(o.disabled && o.disabled());

    let drag = null;
    let wheelAcc = 0;
    let clickTimer = 0;

    const setValue = (v, mode) => {
        const q = quantize(v);
        if (q === value()) return;
        if (mode === "nudge") o.param.nudge(q);
        else o.param.set(q);
    };
    const stepBy = (n) => {
        const cur = value();
        let v = quantize(o.fromPos(clamp01(o.toPos(cur) + n * o.stepPos)));
        if (n > 0 && o.cap !== undefined) v = Math.min(v, Math.max(o.cap, cur));
        return v;
    };

    function followTrack(e) {
        const r = o.track.getBoundingClientRect();
        if (!(r.width > 0)) return;
        setValue(o.fromPos(clamp01((e.clientX - r.left) / r.width)));
    }

    function onPointerDown(e) {
        if (e.button !== 0 || disabled()) return;
        if (e.ctrlKey || e.metaKey) {
            // 立即进入数值输入，不开手势
            e.preventDefault();
            if (o.onEdit) o.onEdit();
            return;
        }
        if (clickTimer) {
            timers.clearTimeout(clickTimer);
            clickTimer = 0;
        }
        try {
            el.setPointerCapture(e.pointerId);
        } catch {
            // 没有这个指针（合成事件）：无妨
        }
        drag = {
            id: e.pointerId,
            x0: e.clientX,
            y0: e.clientY,
            lastX: e.clientX,
            lastY: e.clientY,
            moved: false,
            absolute: false,
            s: { pos: o.toPos(value()), hold: 0, dir: 0 },
            onReadout: Boolean(o.readout && o.readout.contains(e.target)),
        };
        if (o.track && !drag.onReadout) {
            // 推子：按下就跟手，卡箍落到指针处
            o.param.begin();
            drag.moved = true;
            drag.absolute = true;
            if (o.onDrag) o.onDrag(true);
            followTrack(e);
        }
    }

    function onPointerMove(e) {
        if (!drag || e.pointerId !== drag.id) return;
        const k = e.shiftKey ? 0.1 : 1;
        const along = axis === "v" ? drag.lastY - e.clientY : e.clientX - drag.lastX; // 向上或向右为正
        drag.lastX = e.clientX;
        drag.lastY = e.clientY;
        if (!drag.moved) {
            if (Math.hypot(e.clientX - drag.x0, e.clientY - drag.y0) < DRAG_SLOP_PX) return;
            drag.moved = true;
            o.param.begin();
            if (o.onDrag) o.onDrag(true);
            return; // 越过松动距离之前的位移不计，起点从这里算
        }
        if (drag.absolute && !e.shiftKey) {
            followTrack(e);
            drag.s = { pos: o.toPos(value()), hold: 0, dir: 0 };
            return;
        }
        drag.s = advanceDrag(drag.s, along, k, o.gate);
        setValue(o.fromPos(drag.s.pos));
    }

    function finish(e, cancelled) {
        if (!drag || e.pointerId !== drag.id) return;
        const d = drag;
        drag = null;
        try {
            el.releasePointerCapture(d.id);
        } catch {
            // 已经释放
        }
        if (cancelled) {
            if (d.moved) {
                o.param.cancel();
                if (o.onDrag) o.onDrag(false);
            }
            return;
        }
        if (d.moved) {
            o.param.end();
            if (o.onDrag) o.onDrag(false);
            return;
        }
        // 没动：单击。点在读数上，等双击判定窗口过去才进入数值输入
        if (d.onReadout && o.onEdit) {
            clickTimer = timers.setTimeout(() => {
                clickTimer = 0;
                if (!disabled()) o.onEdit();
            }, DBLCLICK_MS);
        }
    }

    function onDblClick() {
        if (disabled()) return;
        if (clickTimer) {
            timers.clearTimeout(clickTimer);
            clickTimer = 0;
        }
        o.param.commit(quantize(o.def));
    }

    function onWheel(e) {
        if (disabled()) return;
        e.preventDefault();
        const px = wheelPx(e);
        if (!px) return;
        wheelAcc += -px; // 向上滚（deltaY 为负）加大
        const steps = Math.trunc(wheelAcc / pxPerStep);
        if (steps === 0) return;
        wheelAcc -= steps * pxPerStep;
        const v = stepBy(steps);
        if (v === value()) {
            wheelAcc = 0; // 顶到头：不攒着等反向
            return;
        }
        setValue(v, "nudge");
    }

    function onKeyDown(e) {
        if (disabled() || e.ctrlKey || e.metaKey || e.altKey) return;
        let v;
        switch (e.key) {
            case "ArrowUp":
            case "ArrowRight":
                v = stepBy(1);
                break;
            case "ArrowDown":
            case "ArrowLeft":
                v = stepBy(-1);
                break;
            case "PageUp":
                v = stepBy(10);
                break;
            case "PageDown":
                v = stepBy(-10);
                break;
            case "Home":
                v = quantize(o.fromPos(0));
                break;
            case "End":
                v = o.cap !== undefined ? o.cap : quantize(o.fromPos(1));
                break;
            case "Enter":
            case "F2":
                if (o.onEdit) {
                    e.preventDefault();
                    o.onEdit();
                }
                return;
            default:
                return;
        }
        e.preventDefault();
        if (v !== value()) setValue(v, "nudge");
    }

    const on = (type, fn, opt) => el.addEventListener(type, fn, opt);
    on("pointerdown", onPointerDown);
    on("pointermove", onPointerMove);
    on("pointerup", (e) => finish(e, false));
    on("pointercancel", (e) => finish(e, true));
    on("dblclick", onDblClick);
    on("wheel", onWheel, { passive: false });
    on("keydown", onKeyDown);
    on("dragstart", (e) => e.preventDefault()); // 拖动时不触发原生拖拽

    return {
        destroy() {
            if (clickTimer) timers.clearTimeout(clickTimer);
            clickTimer = 0;
        },
        dragging: () => drag !== null && drag.moved,
    };
}

/**
 * 只有读数可点的件（Mix 的读数、延迟的读数、音色和上限的读数）：单击等双击判定窗口过去才进入数值输入，
 * 双击不进入，Ctrl（mac：Cmd）+ 单击立即进入。
 * @param {HTMLElement} node 读数元素
 * @param {{onEdit:()=>void, disabled?:()=>boolean, timers?:object}} o
 */
export function attachReadoutEdit(node, o) {
    const timers = o.timers || globalThis;
    let timer = 0;
    const off = () => Boolean(o.disabled && o.disabled());
    node.addEventListener("click", (e) => {
        if (off()) return;
        if (e.ctrlKey || e.metaKey) {
            if (timer) timers.clearTimeout(timer);
            timer = 0;
            o.onEdit();
            return;
        }
        if (timer) timers.clearTimeout(timer);
        timer = timers.setTimeout(() => {
            timer = 0;
            if (!off()) o.onEdit();
        }, DBLCLICK_MS);
    });
    node.addEventListener("dblclick", () => {
        if (timer) timers.clearTimeout(timer);
        timer = 0;
    });
}
