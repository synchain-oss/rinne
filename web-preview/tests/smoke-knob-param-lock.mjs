// SPDX-License-Identifier: GPL-3.0-or-later
//
// 参数锁的单测（Node，无 DOM）：web/input/components/param-echo.js 的 locked 选项，以及 gesture.js 随它禁用。
// 宿主的第一批快照没到之前，参数不接受手势与写入，否则用户在这段空档里的一次拖动会把显示值设成凭空的数，
// 第一帧真回声到了反而被当成宿主自动化去逼近它。
//   ① 整体上锁（locked() 为真）：begin、set、nudge、commit 都不改显示值、不调桥、不触发显示回调；
//   ② 还没有宿主的值（显示值是 null）：即使没有整体上锁，同样不接受写入；
//   ③ 第一帧回声直接落位：显示回调只一次、不进「自动」态、不开 rAF 逼近；有了值之后才接受写入；
//   ④ 第二帧不同的回声才进「自动」并逼近（对照：③ 里「不逼近」不是因为逼近根本不工作）；
//   ⑤ 手势层：参数锁着时，指针按下、滚轮、方向键、Enter、双击、读数点击一概不响应；解锁后同一套输入有响应；
//   ⑥ 坏样例：不认锁的参数替身必须被同一组断言抓出来；
//   ⑦ 手势收尾的空档：松手后、结束命令的回执到达前立刻来一个滚轮，桥上的序列必须是 begin、set、end、begin、set、end，
//      不能有落在手势之外的 set（拖动、滚轮、方向键走的是同一个 set 入口）；判据自带坏样例。
// 用法：node web-preview/tests/smoke-knob-param-lock.mjs [仓库根]

import { pathToFileURL } from "node:url";
import path from "node:path";
import { createRun, repoRoot } from "./lib/harness.mjs";

const ROOT = repoRoot();
const run = createRun("参数锁");
const u = (rel) => pathToFileURL(path.join(ROOT, rel)).href;
const { createParamEcho, IDLE_END_MS } = await import(u("web/input/components/param-echo.js"));
const { attachControl, attachReadoutEdit, DBLCLICK_MS } = await import(u("web/input/components/gesture.js"));

/** 假桥：记录所有调用。 */
function fakeBridge() {
    const calls = [];
    const rec =
        (name) =>
        async (...a) => {
            calls.push([name, ...a]);
            return { ok: true, seq: 1 };
        };
    return {
        calls,
        beginParamGesture: rec("beginParamGesture"),
        setParam: rec("setParam"),
        endParamGesture: rec("endParamGesture"),
    };
}

/** 假定时器与假 rAF：手动推进。 */
function fakeClock() {
    let t = 0;
    let id = 0;
    const q = new Map();
    const raf = [];
    return {
        timers: {
            setTimeout(fn, ms) {
                q.set(++id, { at: t + ms, fn });
                return id;
            },
            clearTimeout: (i) => q.delete(i),
        },
        raf: (cb) => raf.push(cb),
        rafCount: () => raf.length,
        now: () => t,
        tick(ms) {
            const end = t + ms;
            for (;;) {
                const next = [...q.entries()].filter(([, e]) => e.at <= end).sort((a, b) => a[1].at - b[1].at)[0];
                if (!next) break;
                q.delete(next[0]);
                t = next[1].at;
                next[1].fn();
            }
            t = end;
        },
    };
}

function rig(locked) {
    const bridge = fakeBridge();
    const clock = fakeClock();
    const events = { display: [], auto: [] };
    const echo = createParamEcho({
        bridge,
        timers: clock.timers,
        raf: clock.raf,
        caf: () => {},
        now: clock.now,
        locked: () => locked.v,
    });
    const p = echo.param("feedback", {
        epsilon: 0.05,
        onDisplay: (v) => events.display.push(v),
        onAuto: (on) => events.auto.push(on),
    });
    return { bridge, clock, events, p };
}

/** 对一个参数做各种写入的尝试。 */
const attempt = (p) => {
    p.begin();
    p.set(33);
    p.nudge(44);
    p.commit(55);
};

/** ① 的判据：整体上锁（意图）的参数，写入尝试不应有任何效果。返回失败描述。 */
async function lockedFailures(locked) {
    const f = [];
    const { bridge, events, p } = rig(locked);
    p.seed(10); // 已有值，只靠整体上锁挡住
    events.display.length = 0;
    attempt(p);
    await Promise.resolve();
    if (p.value() !== 10) f.push("写入尝试改动了显示值");
    if (bridge.calls.length !== 0) f.push("写入尝试调到了桥");
    if (events.display.length !== 0) f.push("写入尝试触发了显示回调");
    if (p.active()) f.push("开出了手势");
    return f;
}

// ① 整体上锁
{
    const locked = { v: true };
    run.eq(
        await lockedFailures(locked),
        [],
        "① 整体上锁：写入尝试不应改动显示值、不应调桥、不应触发显示回调、不应开出手势",
    );
    const { bridge, p } = rig(locked);
    p.seed(10);
    run.eq(p.locked(), true, "① 整体上锁：locked() 应为真");

    // 解锁之后同一套尝试有效（防空转）
    locked.v = false;
    run.eq(p.locked(), false, "① 解锁后 locked() 应为假");
    attempt(p);
    await Promise.resolve();
    run.check(bridge.calls.length > 0, "① 解锁后同一套写入尝试应能调到桥（防空转）");
    run.eq(p.value(), 55, "① 解锁后写入应改动显示值");
}

// ② 还没有宿主的值
{
    const locked = { v: false };
    const { bridge, events, p } = rig(locked);
    run.eq(p.value(), null, "② 起始显示值应是 null");
    run.eq(p.locked(), true, "② 没有宿主的值时 locked() 应为真（即使没有整体上锁）");
    attempt(p);
    run.eq(p.value(), null, "② 没有宿主的值：写入尝试不应凭空设出显示值");
    run.eq(bridge.calls, [], "② 没有宿主的值：写入尝试不应调桥");
    run.eq(events.display, [], "② 没有宿主的值：写入尝试不应触发显示回调");
}

// ③ 第一帧回声直接落位，之后才接受写入；④ 第二帧不同的回声才逼近
{
    const locked = { v: false };
    const { bridge, clock, events, p } = rig(locked);
    p.receive(60, 5);
    run.eq(p.value(), 60, "③ 第一帧回声应直接落位");
    run.eq(events.display, [60], "③ 第一帧回声：显示回调恰好一次、直接是目标值");
    run.eq(events.auto, [], "③ 第一帧回声不应进「自动」态");
    run.eq(clock.rafCount(), 0, "③ 第一帧回声不应开 rAF 逼近");
    run.eq(p.locked(), false, "③ 有了宿主的值之后 locked() 应为假");
    p.commit(70);
    await Promise.resolve();
    run.check(bridge.calls.length > 0, "③ 有了值之后应接受写入");

    // 另一个参数：第二帧不同的回声才逼近
    const r2 = rig({ v: false });
    r2.p.receive(60, 5);
    r2.p.receive(90, 6);
    run.eq(r2.events.auto, [true], "④ 第二帧不同的回声应进「自动」态");
    run.check(r2.clock.rafCount() > 0, "④ 第二帧不同的回声应开 rAF 逼近");
}

// ⑤ 手势层随参数锁禁用
{
    const mkEl = () => {
        const map = new Map();
        return {
            addEventListener: (t, fn) => map.set(t, fn),
            setPointerCapture() {},
            releasePointerCapture() {},
            contains: () => false,
            fire(type, e = {}) {
                const fn = map.get(type);
                if (fn) fn({ type, button: 0, pointerId: 1, preventDefault() {}, target: {}, ...e });
            },
        };
    };
    const drive = (locked) => {
        const bridge = fakeBridge();
        const clock = fakeClock();
        const echo = createParamEcho({
            bridge,
            timers: clock.timers,
            raf: clock.raf,
            caf: () => {},
            now: clock.now,
            locked: () => locked.v,
        });
        const p = echo.param("feedback", {});
        p.seed(50);
        const el = mkEl();
        const readout = (() => {
            const map = new Map();
            return { addEventListener: (t, fn) => map.set(t, fn), fire: (t, e = {}) => map.get(t) && map.get(t)(e) };
        })();
        let edits = 0;
        attachControl(el, {
            param: p,
            toPos: (v) => v / 150,
            fromPos: (x) => x * 150,
            quantize: Math.round,
            stepPos: 1 / 150,
            def: 0,
            onEdit: () => edits++,
            timers: clock.timers,
        });
        attachReadoutEdit(readout, { onEdit: () => edits++, timers: clock.timers, param: p });
        const stimulate = () => {
            el.fire("pointerdown", { clientX: 0, clientY: 100 });
            el.fire("pointermove", { clientX: 0, clientY: 60 });
            el.fire("pointermove", { clientX: 0, clientY: 20 });
            el.fire("pointerup", { clientX: 0, clientY: 20 });
            el.fire("wheel", { deltaY: -400, deltaMode: 0 });
            el.fire("keydown", { key: "ArrowUp" });
            el.fire("keydown", { key: "Enter" });
            el.fire("dblclick", {});
            readout.fire("click", {});
            readout.fire("click", { ctrlKey: true });
            clock.tick(DBLCLICK_MS + 50);
        };
        return { p, bridge, stimulate, edits: () => edits };
    };

    const lockedRig = drive({ v: true });
    lockedRig.stimulate();
    await Promise.resolve();
    run.eq(lockedRig.p.value(), 50, "⑤ 参数锁着：指针、滚轮、方向键、双击不应改动显示值");
    run.eq(lockedRig.bridge.calls, [], "⑤ 参数锁着：不应调桥");
    run.eq(lockedRig.edits(), 0, "⑤ 参数锁着：Enter、读数点击、Ctrl+单击都不应进数值输入");

    const freeRig = drive({ v: false });
    freeRig.stimulate();
    await Promise.resolve();
    run.check(freeRig.p.value() !== 50, "⑤ 解锁后同一套输入应改动显示值（防空转）");
    run.check(freeRig.bridge.calls.length > 0, "⑤ 解锁后同一套输入应调到桥（防空转）");
    run.check(freeRig.edits() > 0, "⑤ 解锁后 Enter 或读数点击应进数值输入（防空转）");
}

// ⑥ 坏样例：不认锁的实现（locked 恒为假）被 ① 的判据抓出来
run.check((await lockedFailures({ v: false })).length > 0, "⑥ 坏样例：不认锁的参数应被 ① 的判据抓出来（判据不是空转）");

// ⑦ 手势收尾的空档
{
    /** 假桥的变体：endParamGesture 的回执扣着，手动放行（模拟原生侧还没回执的那段空档）。 */
    const deferredEnd = () => {
        const b = fakeBridge();
        const waiters = [];
        b.endParamGesture = (...a) => {
            b.calls.push(["endParamGesture", ...a]);
            return new Promise((r) => waiters.push(() => r({ ok: true, seq: 1 })));
        };
        b.releaseEnds = () => {
            while (waiters.length) waiters.shift()();
        };
        return b;
    };
    const flushMicro = async () => {
        for (let i = 0; i < 30; i++) await Promise.resolve();
    };
    const names = (calls) => calls.map((c) => c[0]);
    /** 不在 beginParamGesture 与 endParamGesture 之间的 setParam。 */
    const strayWrites = (calls) => {
        let open = 0;
        const stray = [];
        for (const c of calls) {
            if (c[0] === "beginParamGesture") open++;
            else if (c[0] === "endParamGesture") open--;
            else if (c[0] === "setParam" && open <= 0) stray.push(c);
        }
        return stray;
    };

    const bridge = deferredEnd();
    const clock = fakeClock();
    const echo = createParamEcho({ bridge, timers: clock.timers, raf: clock.raf, caf: () => {}, now: clock.now });
    const p = echo.param("feedback", {});
    p.seed(10);
    p.begin();
    p.set(20);
    p.end(); // 松手：结束命令已发出，回执扣着
    p.nudge(30); // 回执到达前来一个滚轮
    await flushMicro();
    run.eq(
        names(bridge.calls),
        ["beginParamGesture", "setParam", "endParamGesture"],
        "⑦ 回执没到时：桥上只有第一个手势，新手势的 begin 与 set 排在回执之后",
    );
    run.eq(p.value(), 30, "⑦ 显示立即跟手：滚轮的值已经显示");
    bridge.releaseEnds();
    await flushMicro();
    clock.tick(IDLE_END_MS + 50); // 停手：第二个手势收尾
    await flushMicro();
    bridge.releaseEnds();
    await flushMicro();
    run.eq(
        names(bridge.calls),
        ["beginParamGesture", "setParam", "endParamGesture", "beginParamGesture", "setParam", "endParamGesture"],
        "⑦ 序列应是 begin、set、end、begin、set、end",
    );
    run.eq(
        bridge.calls.filter((c) => c[0] === "setParam").map((c) => c[2]),
        [20, 30],
        "⑦ 两次 setParam 的值应依次是 20 与 30",
    );
    run.eq(strayWrites(bridge.calls), [], "⑦ 不应有落在手势之外的 setParam");

    // 判据自检（坏样例）：旧行为的序列（第二个值落在 end 之后）必须被 strayWrites 抓出来；正常序列不被误报
    const old = [
        ["beginParamGesture", "feedback"],
        ["setParam", "feedback", 20],
        ["endParamGesture", "feedback"],
        ["setParam", "feedback", 30],
    ];
    run.eq(strayWrites(old).length, 1, "⑦ 坏样例：set 落在 end 之后的序列应被判据抓出来");
    run.eq(strayWrites(bridge.calls).length, 0, "⑦ 正例：正常序列不应被判据误报");
}

run.finish();
