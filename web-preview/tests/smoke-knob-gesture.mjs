// SPDX-License-Identifier: GPL-3.0-or-later
//
// 旋钮与推子的手势单测（Node，无 DOM）：web/input/components/gesture.js 的 advanceDrag 与 attachControl。
// attachControl 用最小的假元素、假参数与假定时器驱动，逐条钉住交互约定：
//   拖动：松动距离内不开手势；越过后 begin 一次、按 150 px 走完全程、Shift 为 0.1 倍、抬起 end 一次；
//   闸：位置空间里的 100%，越过要多拖一段，停得住，反向立即离开；
//   滚轮：满一步才走，到上限后累计清零（60 个 3 px 的滚轮不会越过 100%，也不会攒着等反向）；
//   键盘：方向键 1 步、PgUp 与 PgDn 10 步、Home 到 0、End 到上限、Enter 进入数值输入；
//   双击复位且不进入数值输入；单击读数等双击窗口过去才进入；Ctrl 加单击立即进入；
//   pointercancel：参数回滚（cancel）、不走 end，拖动状态回报结束；
//   禁用时一概不响应。
// 每组都带坏样例：把某个选项写错的替身（没有上限、没有闸、不认 Shift）必须被同一组断言抓出来。
//
// 用法：node web-preview/tests/smoke-knob-gesture.mjs [仓库根]

import { pathToFileURL } from "node:url";
import path from "node:path";
import { createRun, repoRoot } from "./lib/harness.mjs";

const ROOT = repoRoot();
const run = createRun("旋钮手势");
const G = await import(pathToFileURL(path.join(ROOT, "web/input/components/gesture.js")).href);
const M = await import(pathToFileURL(path.join(ROOT, "web/input/model.js")).href);
const { advanceDrag, attachControl, attachReadoutEdit, DRAG_SLOP_PX, DBLCLICK_MS } = G;

/** 一组断言：返回失败描述。 */
function suite() {
    const f = [];
    return {
        f,
        ok: (c, m) => {
            if (!c) f.push(m);
        },
        eq: (a, b, m) => {
            if (JSON.stringify(a) !== JSON.stringify(b))
                f.push(`${m}：实得 ${JSON.stringify(a)}，应为 ${JSON.stringify(b)}`);
        },
    };
}
const group = (name, fn, ...rest) => {
    const s = suite();
    fn(s, ...rest);
    run.eq(s.f, [], `${name}：全部断言通过`);
};
const caught = (name, fn, label, ...rest) => {
    const s = suite();
    try {
        fn(s, ...rest);
    } catch (e) {
        s.f.push(`抛出异常：${e.message}`);
    }
    run.check(s.f.length > 0, `${name}：坏样例被抓出（${label}）`);
};

// ---------------------------------------------------------------- 假件

/** 假定时器：手动推进。 */
function fakeTimers() {
    let now = 0;
    let id = 0;
    const q = new Map();
    return {
        setTimeout(fn, ms) {
            q.set(++id, { at: now + ms, fn });
            return id;
        },
        clearTimeout(i) {
            q.delete(i);
        },
        tick(ms) {
            const end = now + ms;
            for (;;) {
                const next = [...q.entries()].filter(([, t]) => t.at <= end).sort((a, b) => a[1].at - b[1].at)[0];
                if (!next) break;
                q.delete(next[0]);
                now = next[1].at;
                next[1].fn();
            }
            now = end;
        },
        pending: () => q.size,
    };
}

/** 假元素：只实现 addEventListener 与指针捕获。 */
function fakeEl() {
    const map = new Map();
    return {
        addEventListener: (t, fn) => map.set(t, fn),
        setPointerCapture() {},
        releasePointerCapture() {},
        contains: () => false,
        fire(type, e = {}) {
            const ev = { type, button: 0, pointerId: 1, preventDefault() {}, target: {}, ...e };
            const fn = map.get(type);
            if (fn) fn(ev);
            return ev;
        },
    };
}

/** 假参数：记录调用，值落在 v。 */
function fakeParam(v0) {
    const p = { v: v0, calls: [] };
    p.value = () => p.v;
    p.set = (x) => (p.calls.push(["set", x]), (p.v = x));
    p.nudge = (x) => (p.calls.push(["nudge", x]), (p.v = x));
    p.begin = () => p.calls.push(["begin"]);
    p.end = () => p.calls.push(["end"]);
    p.cancel = () => (p.calls.push(["cancel"]), (p.v = p.v0));
    p.commit = (x) => (p.calls.push(["commit", x]), (p.v = x));
    p.v0 = v0;
    return p;
}
const count = (p, name) => p.calls.filter((c) => c[0] === name).length;

/** 反馈旋钮的取值空间：0..150，位置 = 值 / 150，上限 100，闸在 100，闸宽取 GATE_PX 量级。 */
function rig(over = {}, v0 = 72) {
    const el = fakeEl();
    const param = fakeParam(v0);
    const timers = fakeTimers();
    const calls = { edit: 0, drag: [] };
    const o = {
        param,
        toPos: (v) => v / 150,
        fromPos: (p) => p * 150,
        quantize: (v) => Math.round(v),
        stepPos: 1 / 150,
        wheelPxPerStep: M.WHEEL_STEP_PX,
        def: 0,
        cap: M.FB_CAP,
        gate: { pos: M.FB_CAP / M.FB_MAX, px: 12 },
        onEdit: () => calls.edit++,
        onDrag: (on) => calls.drag.push(on),
        timers,
        ...over,
    };
    const ctl = attachControl(el, o);
    return { el, param, timers, calls, ctl };
}

// ---------------------------------------------------------------- advanceDrag

const drag = (s, adv) => {
    const S = (pos, hold = 0, dir = 0) => ({ pos, hold, dir });
    const near = (a, b) => Math.abs(a - b) < 1e-9;
    s.ok(near(adv(S(0.2), 75, 1).pos, 0.7), "75 px 走半程");
    s.ok(near(adv(S(0.2), 75, 0.1).pos, 0.25), "Shift 为 0.1 倍");
    s.ok(near(adv(S(0.95), 100, 1).pos, 1), "越过上端夹在 1");
    s.ok(near(adv(S(0.05), -100, 1).pos, 0), "越过下端夹在 0");
    const same = S(0.4);
    s.ok(adv(same, 0, 1) === same, "零位移原样返回");
    const gate = { pos: 2 / 3, px: 20 };
    // 从 0.6 往上拖 20 px：先用 10 px 走到闸上，剩 10 px 的停留
    let r = adv(S(0.6), 20, 1, gate);
    s.ok(near(r.pos, 2 / 3) && near(r.hold, 10) && r.dir === 1, `闸前停住并记停留量，实得 ${JSON.stringify(r)}`);
    // 再往上 6 px：吃掉停留，仍在闸上
    r = adv(r, 6, 1, gate);
    s.ok(near(r.pos, 2 / 3) && near(r.hold, 4), `继续顶闸只消耗停留量，实得 ${JSON.stringify(r)}`);
    // 再往上 10 px：停留用完，多出的 6 px 才继续走
    r = adv(r, 10, 1, gate);
    s.ok(near(r.pos, 2 / 3 + 6 / 150) && r.hold === 0, `停留用完后继续走，实得 ${JSON.stringify(r)}`);
    // 在闸上停留时反向：立即离开
    const held = adv(S(0.6), 20, 1, gate);
    r = adv(held, -3, 1, gate);
    s.ok(near(r.pos, 2 / 3 - 3 / 150) && r.hold === 0, `反向立即离开闸，实得 ${JSON.stringify(r)}`);
    // 从上面往下穿闸
    r = adv(S(0.74), -20, 1, gate);
    s.ok(near(r.pos, 2 / 3) && r.hold > 0 && r.dir === -1, `向下穿闸同样停住，实得 ${JSON.stringify(r)}`);
    // Shift 的精细拖动也过闸要多拖：同样的位移在 0.1 倍下走不到闸
    r = adv(S(0.6), 20, 0.1, gate);
    s.ok(near(r.pos, 0.6 + 2 / 150), `精细拖动 20 px 只走 2 px 的位置，实得 ${JSON.stringify(r)}`);
};
group("拖动推进", (s) => drag(s, advanceDrag));
caught("拖动推进", (s) => drag(s, (st, d, k, g) => advanceDrag(st, d, 1, g)), "不认 Shift 精细系数");
caught("拖动推进", (s) => drag(s, (st, d, k) => advanceDrag(st, d, k)), "没有闸");
caught("拖动推进", (s) => drag(s, (st, d, k, g) => advanceDrag({ ...st, hold: 0 }, d, k, g)), "闸的停留量不跨次保留");

// ---------------------------------------------------------------- 拖动、滚轮、键盘

const gestures = (s, make = rig) => {
    // ---- 拖动
    let r = make();
    r.el.fire("pointerdown", { clientX: 100, clientY: 200 });
    r.el.fire("pointermove", { clientX: 100, clientY: 200 - (DRAG_SLOP_PX - 1) });
    s.eq(count(r.param, "begin"), 0, "松动距离内不开手势");
    r.el.fire("pointermove", { clientX: 100, clientY: 150 });
    s.eq(count(r.param, "begin"), 1, "越过松动距离开手势一次");
    s.ok(r.param.v === 72, `越过松动距离的那一下不计位移（起点从这里算），实得 ${r.param.v}`);
    r.el.fire("pointermove", { clientX: 100, clientY: 130 });
    s.eq(r.param.v, 72 + 20, "再向上 20 px，值加 20（150 px 走满 150）");
    r.el.fire("pointerup", {});
    s.eq([count(r.param, "end"), count(r.param, "cancel")], [1, 0], "抬起走 end，不走 cancel");
    s.eq(r.calls.drag, [true, false], "拖动状态回报开始与结束");
    // Shift 精细
    r = make();
    r.el.fire("pointerdown", { clientX: 0, clientY: 200 });
    r.el.fire("pointermove", { clientX: 0, clientY: 150 });
    r.el.fire("pointermove", { clientX: 0, clientY: 100, shiftKey: true });
    s.eq(r.param.v, 72 + 5, "Shift 拖 50 px 只走 5");
    // 闸：72 往上拖 30 px 落在 100 并停住
    r = make();
    r.el.fire("pointerdown", { clientX: 0, clientY: 200 });
    r.el.fire("pointermove", { clientX: 0, clientY: 150 });
    r.el.fire("pointermove", { clientX: 0, clientY: 150 - 30 });
    s.eq(r.param.v, 100, "拖过 100% 处的闸，停在 100（走到闸要 28 px，剩 10 px 的停留）");
    r.el.fire("pointermove", { clientX: 0, clientY: 150 - 30 - 6 });
    s.eq(r.param.v, 100, "再多拖 6 px 仍停在 100（闸的停留）");
    r.el.fire("pointermove", { clientX: 0, clientY: 150 - 30 - 6 - 15 });
    s.ok(r.param.v > 100, `拖过闸的停留量后才越过 100，实得 ${r.param.v}`);
    // pointercancel 回滚
    r = make();
    r.el.fire("pointerdown", { clientX: 0, clientY: 200 });
    r.el.fire("pointermove", { clientX: 0, clientY: 150 });
    r.el.fire("pointermove", { clientX: 0, clientY: 100 });
    s.ok(r.param.v !== 72, "拖动中值已变");
    r.el.fire("pointercancel", {});
    s.eq([count(r.param, "cancel"), count(r.param, "end")], [1, 0], "pointercancel 走 cancel，不走 end");
    s.eq(r.param.v, 72, "回滚到起点值");
    s.eq(r.calls.drag, [true, false], "取消后拖动状态回报结束");
    // 没动就取消：不开手势也不回滚
    r = make();
    r.el.fire("pointerdown", { clientX: 0, clientY: 200 });
    r.el.fire("pointercancel", {});
    s.eq(r.param.calls, [], "没动的取消不碰参数");
    // Ctrl 加单击：立即进入数值输入，不开手势
    r = make();
    r.el.fire("pointerdown", { clientX: 0, clientY: 200, ctrlKey: true });
    s.eq([r.calls.edit, count(r.param, "begin")], [1, 0], "Ctrl 加单击立即进入数值输入，不开手势");
    // 禁用
    r = make({ disabled: () => true });
    r.el.fire("pointerdown", { clientX: 0, clientY: 200 });
    r.el.fire("pointermove", { clientX: 0, clientY: 100 });
    r.el.fire("wheel", { deltaY: -500, deltaMode: 0 });
    r.el.fire("keydown", { key: "ArrowUp" });
    r.el.fire("dblclick", {});
    s.eq(r.param.calls, [], "禁用时拖动、滚轮、键盘、双击都不响应");

    // ---- 双击与单击读数
    r = make({ readout: { contains: () => true } });
    r.el.fire("pointerdown", { clientX: 0, clientY: 0, target: {} });
    r.el.fire("pointerup", {});
    s.eq(r.calls.edit, 0, "单击读数后窗口内不进入数值输入");
    r.timers.tick(DBLCLICK_MS + 1);
    s.eq(r.calls.edit, 1, "窗口过去才进入数值输入");
    r = make({ readout: { contains: () => true } });
    // 浏览器的真实顺序：按下、抬起、按下、抬起、dblclick
    r.el.fire("pointerdown", { clientX: 0, clientY: 0 });
    r.el.fire("pointerup", {});
    r.el.fire("pointerdown", { clientX: 0, clientY: 0 });
    r.el.fire("pointerup", {});
    r.el.fire("dblclick", {});
    r.timers.tick(1000);
    s.eq(r.calls.edit, 0, "双击不进入数值输入");
    s.eq(
        r.param.calls.filter((c) => c[0] === "commit"),
        [["commit", 0]],
        "双击复位到默认值",
    );

    // ---- 滚轮：60 个 3 px 不越过 100
    r = make({}, 99);
    for (let i = 0; i < 60; i++) r.el.fire("wheel", { deltaY: -3, deltaMode: 0 });
    s.ok(r.param.v <= 100, `60 个 3 px 的滚轮不越过 100，实得 ${r.param.v}`);
    s.eq(r.param.v, 100, "滚满一步走到 100");
    for (let i = 0; i < 60; i++) r.el.fire("wheel", { deltaY: -3, deltaMode: 0 });
    s.eq(r.param.v, 100, "到顶之后继续滚还是 100");
    r.el.fire("wheel", { deltaY: 3, deltaMode: 0 });
    s.eq(r.param.v, 100, "到顶后累计已清零：反向一个小滚轮不会立刻往下走");
    for (let i = 0; i < 40; i++) r.el.fire("wheel", { deltaY: 3, deltaMode: 0 });
    s.ok(r.param.v < 100, `反向滚满一步才往下走，实得 ${r.param.v}`);
    // 100 px 才走一步
    r = make({}, 50);
    for (let i = 0; i < 33; i++) r.el.fire("wheel", { deltaY: -3, deltaMode: 0 });
    s.eq(r.param.v, 50, "99 px 还不满一步");
    r.el.fire("wheel", { deltaY: -3, deltaMode: 0 });
    s.eq(r.param.v, 51, "满 100 px 走一步");
    s.eq(count(r.param, "nudge"), 1, "滚轮走 nudge 手势");

    // ---- 键盘
    r = make();
    r.el.fire("keydown", { key: "ArrowUp" });
    s.eq(r.param.v, 73, "方向键上加 1");
    r.el.fire("keydown", { key: "ArrowDown" });
    r.el.fire("keydown", { key: "ArrowDown" });
    s.eq(r.param.v, 71, "方向键下减 1");
    r.el.fire("keydown", { key: "PageUp" });
    s.eq(r.param.v, 81, "PgUp 加 10");
    r.el.fire("keydown", { key: "PageDown" });
    s.eq(r.param.v, 71, "PgDn 减 10");
    r.el.fire("keydown", { key: "End" });
    s.eq(r.param.v, 100, "End 到上限 100");
    r.el.fire("keydown", { key: "ArrowUp" });
    s.eq(r.param.v, 100, "方向键向上过不了上限");
    r.el.fire("keydown", { key: "PageUp" });
    s.eq(r.param.v, 100, "PgUp 也过不了上限");
    r.el.fire("keydown", { key: "Home" });
    s.eq(r.param.v, 0, "Home 到 0");
    r.el.fire("keydown", { key: "Enter" });
    s.eq(r.calls.edit, 1, "Enter 进入数值输入");
    r.el.fire("keydown", { key: "ArrowUp", ctrlKey: true });
    s.eq(r.param.v, 0, "带 Ctrl 的方向键不处理");
};
group("拖动、滚轮与键盘", gestures);
caught(
    "拖动、滚轮与键盘",
    (s) => gestures(s, (o, v) => rig({ cap: undefined, ...o }, v)),
    "没有上限（滚轮与键盘能越过 100）",
);
caught("拖动、滚轮与键盘", (s) => gestures(s, (o, v) => rig({ gate: undefined, ...o }, v)), "没有闸（拖动不停在 100）");
caught("拖动、滚轮与键盘", (s) => gestures(s, (o, v) => rig({ wheelPxPerStep: 3, ...o }, v)), "滚轮每 3 px 一步");
caught("拖动、滚轮与键盘", (s) => gestures(s, (o, v) => rig({ stepPos: 10 / 150, ...o }, v)), "方向键一步走 10");

// ---------------------------------------------------------------- 只有读数可点的件

const readoutEdit = (s, mk) => {
    const timers = fakeTimers();
    let edits = 0;
    const handlers = new Map();
    const node = { addEventListener: (t, fn) => handlers.set(t, fn) };
    mk(node, { onEdit: () => edits++, timers });
    handlers.get("click")({});
    timers.tick(DBLCLICK_MS - 10);
    s.eq(edits, 0, "单击后窗口内不进入");
    timers.tick(20);
    s.eq(edits, 1, "窗口过去进入数值输入");
    handlers.get("click")({});
    handlers.get("dblclick")({});
    timers.tick(1000);
    s.eq(edits, 1, "双击取消待进入的单击");
    handlers.get("click")({ ctrlKey: true });
    s.eq(edits, 2, "Ctrl 加单击立即进入");
};
group("读数单击", readoutEdit, (node, o) => attachReadoutEdit(node, o));
caught(
    "读数单击",
    (s) => readoutEdit(s, (node, o) => node.addEventListener("click", () => o.onEdit())),
    "单击立即进入、不等双击窗口",
);

run.finish();
