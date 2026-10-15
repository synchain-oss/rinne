// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/web.json
//
// 设计盒门禁：
//   1. web/shared/design-box.js 的 DESIGN 与定稿值逐项相等（Input 800×580、Output 460×364、缩放档位、hostFrame），
//      档位严格递增且含 1。改尺寸是设计事件：先改这里的 EXPECT，再改 design-box.js，再重跑生成器。
//   2. src/core/util/DesignBox.h 与 design-box.js 一致（头文件由 gen-design-box.py 生成，这里直接解析头文件对拍，
//      不依赖 Python）。
//   3. web/shared/scale-ui.js 的状态机行为：10 秒倒计时回退、保存、取消、pagehide 只停计时、
//      桥调用被拒不进确认态、确认态里再选档位回退目标不变、放不下的档位置灰。
//   node scripts/check-design-box.mjs [--self-test]

import fs from "node:fs";
import path from "node:path";
import { pathToFileURL } from "node:url";
import { ROOT, assertNodeVersion, caseResult, isMain } from "./lib/check-kit.mjs";

export const EXPECT = {
    input: { w: 800, h: 580, presets: [0.5, 0.65, 0.8, 1, 1.25, 1.5, 2] },
    output: { w: 460, h: 364, presets: [0.33, 0.5, 0.75, 1, 1.25, 1.5, 1.75, 2, 2.5, 3] },
    hostFrame: { w: 16, h: 64 },
};

const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);

/** DESIGN 对象与定稿值的差异，返回问题数组。 */
export function designProblems(design, expect = EXPECT) {
    const out = [];
    for (const role of ["input", "output"]) {
        const got = design?.[role];
        if (!got) {
            out.push(`DESIGN.${role} 缺失`);
            continue;
        }
        for (const dim of ["w", "h"]) {
            if (got[dim] !== expect[role][dim])
                out.push(`DESIGN.${role}.${dim} 是 ${got[dim]}，定稿为 ${expect[role][dim]}`);
        }
        const p = got.presets;
        if (!Array.isArray(p) || !same(p, expect[role].presets)) {
            out.push(`DESIGN.${role}.presets 是 [${p}]，定稿为 [${expect[role].presets}]`);
        }
        if (Array.isArray(p) && p.some((v, i) => i > 0 && !(v > p[i - 1])))
            out.push(`DESIGN.${role}.presets 必须严格递增`);
        if (Array.isArray(p) && !p.includes(1)) out.push(`DESIGN.${role}.presets 必须含 1`);
    }
    if (!same(design?.hostFrame, expect.hostFrame)) {
        out.push(
            `DESIGN.hostFrame 是 ${JSON.stringify(design?.hostFrame)}，定稿为 ${JSON.stringify(expect.hostFrame)}`,
        );
    }
    return out;
}

/** 从生成的头文件里解析出与 DESIGN 同形的对象。 */
export function parseHeader(text) {
    const num = (name) => {
        const m = new RegExp(`\\b${name}\\s*=\\s*(\\d+)\\s*;`).exec(text);
        return m ? Number(m[1]) : undefined;
    };
    const arr = (name) => {
        const m = new RegExp(`\\b${name}\\s*=\\s*\\{([^}]*)\\}`).exec(text);
        return m ? m[1].split(",").map((v) => Number(v.trim())) : undefined;
    };
    const role = (R) => ({ w: num(`k${R}DesignW`), h: num(`k${R}DesignH`), presets: arr(`k${R}Presets`) });
    return {
        input: role("Input"),
        output: role("Output"),
        hostFrame: { w: num("kHostFrameW"), h: num("kHostFrameH") },
    };
}

export function headerProblems(design, headerText) {
    if (headerText === null) return ["src/core/util/DesignBox.h 不存在，先运行 scripts/gen-design-box.py"];
    const h = parseHeader(headerText);
    const out = [];
    for (const key of ["input", "output", "hostFrame"]) {
        if (!same(h[key], design[key]))
            out.push(`DesignBox.h 的 ${key} 与 design-box.js 不一致，重跑 scripts/gen-design-box.py`);
    }
    return out;
}

async function loadDesign(root) {
    const mod = await import(pathToFileURL(path.join(root, "web", "shared", "design-box.js")).href);
    return mod.DESIGN;
}

/** scale-ui.js 的行为断言，返回 caseResult 用的用例数组。 */
export async function scaleUiCases(root = ROOT) {
    const { createScaleUi, scaleOverflows, CONFIRM_SECONDS } = await import(
        pathToFileURL(path.join(root, "web", "shared", "scale-ui.js")).href
    );
    const cases = [];
    const add = (label, bad, passed) => cases.push({ label, bad, passed });

    // 可手动推进的假计时器
    const fakeTimers = () => {
        let next = 1;
        const live = new Map();
        return {
            setInterval(fn) {
                live.set(next, fn);
                return next++;
            },
            clearInterval(id) {
                live.delete(id);
            },
            tick(n = 1) {
                for (let i = 0; i < n; i++) for (const fn of [...live.values()]) fn();
            },
            count: () => live.size,
        };
    };
    const rig = (role = "input", { reject = false, scale = 1 } = {}) => {
        const calls = [];
        const timers = fakeTimers();
        const bridge = {
            async setUiScale(f) {
                calls.push(["set", f]);
                return reject ? { ok: false } : { ok: true };
            },
            commitUiScale() {
                calls.push(["commit"]);
            },
        };
        const ui = createScaleUi({ role, bridge, timers, getScale: () => scale });
        return { ui, calls, timers };
    };
    const flush = () => new Promise((r) => setImmediate(r));

    {
        const { ui, calls, timers } = rig();
        await ui.preview(1.5);
        timers.tick(CONFIRM_SECONDS - 1);
        const midway = ui.state().confirming && calls.length === 1;
        timers.tick(1);
        await flush();
        add("10 秒到点回退到选档前的档位", false, midway && !ui.state().confirming && calls.at(-1)?.join() === "set,1");
    }
    {
        const { ui, calls, timers } = rig();
        await ui.preview(1.5);
        timers.tick(3);
        await ui.keep();
        timers.tick(20);
        await flush();
        add(
            "保存后停计时并提交，不再回退",
            false,
            calls.map((c) => c[0]).join() === "set,commit" && timers.count() === 0,
        );
    }
    {
        const { ui, calls, timers } = rig("output", { scale: 2 });
        await ui.preview(0.5);
        await ui.revert();
        add("取消回到选档前的档位", false, calls.at(-1)?.join() === "set,2" && timers.count() === 0);
    }
    {
        const { ui, calls, timers } = rig();
        await ui.preview(1.5);
        ui.pagehide();
        timers.tick(20);
        await flush();
        add(
            "pagehide 只停计时，不发桥调用",
            false,
            calls.length === 1 && timers.count() === 0 && !ui.state().confirming,
        );
    }
    {
        const { ui, calls, timers } = rig("input", { reject: true });
        const ok = await ui.preview(1.5);
        add(
            "桥调用被拒不进入确认态",
            false,
            ok === false && !ui.state().confirming && timers.count() === 0 && calls.length === 1,
        );
    }
    {
        const { ui, calls, timers } = rig("input", { scale: 1 });
        await ui.preview(1.5);
        timers.tick(4);
        await ui.preview(2);
        const left = ui.state().left;
        await ui.revert();
        add(
            "确认态里再选档位：倒计时重置，回退目标仍是最初的档位",
            false,
            left === CONFIRM_SECONDS && calls.at(-1)?.join() === "set,1",
        );
    }
    // 桥调用的返回时刻由测试控制：覆盖「预览还没返回时又选档位、保存、取消」的交错
    const deferredRig = ({ scale = 1 } = {}) => {
        const calls = [];
        const pending = [];
        const timers = fakeTimers();
        const errors = [];
        const bridge = {
            setUiScale(f) {
                calls.push(["set", f]);
                return new Promise((resolve, reject) => pending.push({ resolve, reject }));
            },
            commitUiScale() {
                calls.push(["commit"]);
            },
        };
        const ui = createScaleUi({
            role: "input",
            bridge,
            timers,
            getScale: () => scale,
            onError: (e) => errors.push(e),
        });
        return { ui, calls, pending, timers, errors };
    };
    {
        const { ui, calls, pending, timers } = deferredRig({ scale: 1 });
        const a = ui.preview(1.5);
        const b = ui.preview(2);
        pending[0].resolve({ ok: true });
        pending[1].resolve({ ok: true });
        const results = [await a, await b];
        timers.tick(CONFIRM_SECONDS);
        await flush();
        add(
            "两次预览交叠：只有最后一次起倒计时，回退目标仍是最初的档位",
            false,
            results.join() === "false,true" &&
                calls.at(-1)?.join() === "set,1" &&
                ui.state().prev === 1 &&
                timers.count() === 0,
        );
    }
    {
        const { ui, calls, pending, timers } = deferredRig();
        const p = ui.preview(1.5);
        await ui.keep();
        pending[0].resolve({ ok: true });
        const started = await p;
        timers.tick(CONFIRM_SECONDS * 2);
        await flush();
        add(
            "预览未返回时保存：之后不起倒计时，也不会把保存的档位撤掉",
            false,
            started === false && timers.count() === 0 && calls.map((c) => c[0]).join() === "set,commit",
        );
    }
    {
        const { ui, calls, pending, timers } = deferredRig({ scale: 2 });
        const p = ui.preview(0.5);
        const r = ui.revert();
        pending[0].resolve({ ok: true });
        pending[1].resolve({ ok: true });
        await Promise.all([p, r]);
        timers.tick(CONFIRM_SECONDS);
        await flush();
        add(
            "预览未返回时取消：回到选档前的档位，不起倒计时",
            false,
            calls.at(-1)?.join() === "set,2" && timers.count() === 0 && !ui.state().confirming,
        );
    }
    for (const order of ["过期的先返回", "最新的先被拒"]) {
        const { ui, calls, pending, timers } = deferredRig({ scale: 1 });
        const a = ui.preview(1.5);
        const b = ui.preview(2);
        if (order === "过期的先返回") {
            pending[0].resolve({ ok: true });
            pending[1].resolve({ ok: false });
        } else {
            pending[1].resolve({ ok: false });
            pending[0].resolve({ ok: true });
        }
        await Promise.all([a, b]);
        const counting = ui.state().confirming && timers.count() === 1;
        timers.tick(CONFIRM_SECONDS);
        await flush();
        add(
            `交叠预览，前一次生效、后一次被拒（${order}）：仍有倒计时，回到最初的档位`,
            false,
            counting && calls.at(-1)?.join() === "set,1" && !ui.state().confirming,
        );
    }
    {
        // 桥调用以 undefined 为原因 reject 时，onError 也必须被调用
        const { ui, pending, errors } = deferredRig();
        const p = ui.preview(1.5);
        pending[0].reject(undefined);
        await p;
        add("桥调用无原因地 reject：仍交给 onError", false, errors.length === 1 && !ui.state().confirming);
    }
    {
        const { ui, pending, timers } = deferredRig();
        const a = ui.preview(1.5);
        const b = ui.preview(2);
        pending[0].resolve({ ok: false });
        pending[1].resolve({ ok: false });
        await Promise.all([a, b]);
        add("交叠预览全部被拒：退出确认态，不起倒计时", false, !ui.state().confirming && timers.count() === 0);
    }
    {
        const { ui, calls, pending, timers } = deferredRig();
        const first = ui.preview(1.5);
        pending[0].resolve({ ok: true });
        await first;
        timers.tick(3);
        const second = ui.preview(2);
        pending[1].resolve({ ok: false });
        await second;
        add(
            "后一次预览被拒：前一次的倒计时继续，仍会回退",
            false,
            ui.state().confirming && timers.count() === 1 && calls.length === 2,
        );
    }
    {
        const { ui, pending, timers, errors } = deferredRig();
        const p = ui.preview(1.5);
        pending[0].resolve({ ok: true });
        await p;
        timers.tick(CONFIRM_SECONDS);
        pending[1].reject(new TypeError("setUiScale 未登记"));
        await flush();
        add(
            "倒计时回退时桥调用失败：交给 onError，不产生未处理的 rejection",
            false,
            errors.length === 1 && !ui.state().confirming,
        );
    }
    {
        const o = rig("input").ui.options();
        const p = rig("output").ui.options();
        add("Input 7 档、Output 10 档", false, o.length === 7 && p.length === 10);
    }
    {
        const screen = { availWidth: 1280, availHeight: 672 };
        // Input 580×F + 64 <= 672 时 F <= 1.048：1 档可用，1.25 档放不下
        const okRole = scaleOverflows("input", 1, screen) === false && scaleOverflows("input", 1.25, screen) === true;
        add("按设计盒加宿主框判断放不放得下", false, okRole);
        add("屏幕尺寸未知时不判溢出", false, scaleOverflows("input", 2, null) === false);
    }
    return cases;
}

export async function checkTree(root = ROOT) {
    const design = await loadDesign(root);
    const hp = path.join(root, "src", "core", "util", "DesignBox.h");
    const header = fs.existsSync(hp) ? fs.readFileSync(hp, "utf8") : null;
    const problems = [...designProblems(design), ...headerProblems(design, header)];
    const failed = (await scaleUiCases(root)).filter((c) => !c.bad && !c.passed);
    for (const c of failed) problems.push(`scale-ui：${c.label}`);
    return problems;
}

export async function selfTest() {
    const live = await loadDesign(ROOT);
    const mutate = (fn) => {
        const d = JSON.parse(JSON.stringify(live));
        fn(d);
        return d;
    };
    const hp = fs.readFileSync(path.join(ROOT, "src", "core", "util", "DesignBox.h"), "utf8");
    const cases = [
        ["仓内设计盒干净", false, designProblems(live)],
        ["Input 宽被改回旧值", true, designProblems(mutate((d) => (d.input.w = 460)))],
        ["Output 高被改回旧值", true, designProblems(mutate((d) => (d.output.h = 360)))],
        ["增删一个档位", true, designProblems(mutate((d) => d.input.presets.pop()))],
        ["档位乱序", true, designProblems(mutate((d) => d.output.presets.reverse()))],
        ["hostFrame 被改", true, designProblems(mutate((d) => (d.hostFrame.h = 80)))],
        ["仓内头文件与 design-box.js 一致", false, headerProblems(live, hp)],
        ["头文件宽度被改", true, headerProblems(live, hp.replace("kInputDesignW = 800", "kInputDesignW = 801"))],
        ["头文件缺失", true, headerProblems(live, null)],
    ].map(([label, bad, problems]) => ({ label, bad, passed: bad ? problems.length > 0 : problems.length === 0 }));
    return caseResult([...cases, ...(await scaleUiCases(ROOT))]);
}

async function main(argv) {
    assertNodeVersion();
    if (argv.includes("--self-test")) {
        const r = await selfTest();
        for (const l of r.lines) console.error(l);
        console.log(`check-design-box --self-test：${r.ok ? "通过" : "失败"}（坏样例 ${r.bad}、正例 ${r.good}）`);
        return r.ok ? 0 : 1;
    }
    const problems = await checkTree();
    if (!problems.length) {
        console.log("check-design-box 通过");
        return 0;
    }
    console.error(`check-design-box 失败：${problems.length} 项`);
    for (const p of problems) console.error(`  ${p}`);
    return 1;
}

if (isMain(import.meta.url)) process.exitCode = await main(process.argv.slice(2));
