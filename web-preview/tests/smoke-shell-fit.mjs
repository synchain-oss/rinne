// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/web-preview.json
//
// 外壳自适配，Node 侧冒烟（纯函数加接线）。页面级那套（smoke-shell-fit-page.mjs）要无头浏览器，缺了会跳过；
// 本套不需要浏览器，恒执行，守两件页面级守不到或不该由它守的事：
//   ① fitFactor 的算术面，含「量化方向必须向下」：向上取整会让「设计宽 × f」比视口大出零点几个像素，
//      那点溢出在整数比较的 scrollWidth ≤ clientWidth 上量不出来，页面级永远抓不到，所以只能在这里钉；
//   ② 唯一写方：整个 web/ 里给外壳写 CSS zoom 的地方只允许一处（shell-fit.js）。病根是「档位数字」与「实际倍率」
//      两个真源分家；判据钉的是「只有一个写方」这个状态，谁再加第二处写方，这里立刻变红。
//
// 用法：node web-preview/tests/smoke-shell-fit.mjs [仓库根]

import fs from "node:fs";
import path from "node:path";
import { pathToFileURL } from "node:url";
import { createRun, repoRoot } from "./lib/harness.mjs";

const ROOT = repoRoot();
const run = createRun("外壳自适配（Node）");
const url = (p) => pathToFileURL(path.join(ROOT, p)).href;

const { fitFactor, backingFitFactor, installShellFit, shellFitFactor } = await import(url("web/shared/shell-fit.js"));
const { DESIGN } = await import(url("web/shared/design-box.js"));

// ---- ① 算术面 ----
const near = (a, b) => Math.abs(a - b) < 1e-9;
for (const role of ["input", "output"]) {
    const { w, h, presets } = DESIGN[role];
    for (const f of presets) {
        const got = fitFactor(w * f, h * f, w, h);
        run.check(
            got <= f && f - got < 1e-3,
            `${role}：视口恰为设计盒 × ${f} 时倍率应当是 ${f}（向下量化），实得 ${got}`,
        );
    }
    run.check(near(fitFactor(w * 3, h * 1, w, h), 1), `${role}：宽裕而矮的视口按短边取 min`);
    run.check(near(fitFactor(w * 1, h * 3, w, h), 1), `${role}：窄而高的视口按短边取 min`);
}
run.check(fitFactor(801.9, 581.9, 800, 580) * 800 <= 801.9, "量化方向：设计宽 × f 不得超出视口");
run.check(fitFactor(1000.00001, 725, 800, 580) * 580 <= 725, "量化方向：设计高 × f 不得超出视口（边界）");
for (const bad of [0, -1, NaN, Infinity, undefined, null, "800"]) {
    run.eq(fitFactor(bad, 580, 800, 580), 1, `视口宽 ${String(bad)}：回落到 1`);
    run.eq(fitFactor(800, 580, bad, 580), 1, `设计宽 ${String(bad)}：回落到 1`);
}

// ---- 接线：假 window 驱动 installShellFit ----
function fakeEnv(vw, vh) {
    const listeners = new Map();
    const timers = [];
    const de = { clientWidth: vw, clientHeight: vh };
    const win = {
        document: { documentElement: de },
        addEventListener: (n, cb) => listeners.set(n, cb),
        removeEventListener: (n) => listeners.delete(n),
        setTimeout: (fn) => (timers.push(fn), timers.length),
        clearTimeout: () => {},
    };
    return { win, de, listeners, flush: () => timers.splice(0).forEach((fn) => fn()) };
}
{
    const env = fakeEnv(800, 580);
    const el = { style: {} };
    const changes = [];
    const fit = installShellFit({ el, box: DESIGN.input, win: env.win, onChange: (f) => changes.push(f) });
    run.eq(el.style.zoom, "1", '安装：1 档时读回裸 "1"');
    run.eq(changes, [], "安装：首帧不回调 onChange");
    env.de.clientWidth = 400;
    env.de.clientHeight = 290;
    env.listeners.get("resize")();
    env.flush();
    run.eq(el.style.zoom, "0.5", "resize 之后倍率跟着视口走");
    run.eq(changes, [0.5], "倍率变了才回调一次");
    run.check(near(shellFitFactor(), 0.5) && near(backingFitFactor(), 0.5), "模块级倍率与后备存储倍率同步");
    env.listeners.get("resize")();
    env.flush();
    run.eq(changes, [0.5], "倍率没变不重复回调");
    fit.destroy();
    run.eq(shellFitFactor(), 1, "destroy 之后模块级倍率复位为 1");
    run.eq(env.listeners.size, 0, "destroy 之后监听全部摘掉");
}

// ---- ② 唯一写方 ----
function walk(dir, out = []) {
    for (const name of fs.readdirSync(path.join(ROOT, dir))) {
        const rel = `${dir}/${name}`;
        if (fs.statSync(path.join(ROOT, rel)).isDirectory()) walk(rel, out);
        else if (/\.(js|html|css)$/.test(name)) out.push(rel);
    }
    return out;
}
// hit.js 的 zoom 是语义探针（一个不可见的、zoom: 2 的临时元素），不是外壳，单列放行
const PROBE_ONLY = ["web/shared/hit.js"];
const writers = [];
for (const f of walk("web").filter((p) => !p.startsWith("web/js/juce/") && !PROBE_ONLY.includes(p))) {
    const text = fs
        .readFileSync(path.join(ROOT, f), "utf8")
        .replace(/\/\*[\s\S]*?\*\//g, "")
        .replace(/^\s*\/\/.*$/gm, "");
    if (/\.style\.zoom\s*=|style\.setProperty\(\s*["']zoom["']|(?:^|[;{\s"'])zoom\s*:/m.test(text)) writers.push(f);
}
run.eq(writers, ["web/shared/shell-fit.js"], "唯一写方：web/ 里写 CSS zoom 的文件只能是 shell-fit.js");

run.finish();
