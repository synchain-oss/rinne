// SPDX-License-Identifier: GPL-3.0-or-later
//
// Input 页面适配冒烟，预设面板的子态（无头浏览器）：中 / 英 / 法 × 八种子态（列表、长名当前、保存输入、名称为空、
// 非法字符、重名、保存失败、删除确认）。场景表里只有列表与重名两态，其余子态经真实交互摆出来（kit 的 openPresetState）。
// 每页量的与 smoke-input-fit-page 的 ① 到 ③ 相同：可见文字不越出所在的 data-gb 件与外壳、不被 overflow 裁掉、互相不压住
// （面板是独立一层，带完整 title 的预设名用省略号截短不算被裁）；页上确有足够多的可见文字；界面上没有裸词条键、占位符、null / undefined。
// 本文件由 smoke-input-fit-page 拆出：加了这 24 页之后那一个文件逼近 run-all 的单项 300 秒上限，拆开后各自留有余量；
// 判据与断言一条没动。浏览器缺失时整套跳过（退出码 2），不是通过。本机同时只能有一个浏览器冒烟。
//
// 用法：node web-preview/tests/smoke-input-fit-presets-page.mjs [仓库根]

import { createRun, repoRoot } from "./lib/harness.mjs";
import { findBrowser, launchBrowser } from "./lib/browser.mjs";
import { startStaticServer } from "./lib/static-server.mjs";
import {
    LANGS,
    PRESET_STATES,
    VIEW,
    findLeaks,
    fitProbe,
    inFrame,
    openInput,
    openPresetState,
} from "./smoke-input-kit-page.mjs";

const ROOT = repoRoot();
const run = createRun("Input 页面预设子态适配");
if (!findBrowser()) run.skip("没有 Chromium 系浏览器");

const server = await startStaticServer(ROOT);
const browser = await launchBrowser({ width: VIEW.w, height: VIEW.h });
try {
    const page = await browser.newPage();
    const requests = await page.recordRequests();

    // ---- ⑥ 预设面板的子态
    let presetPages = 0;
    for (const lang of LANGS) {
        for (const state of PRESET_STATES) {
            const tag = `预设面板 ${state}/${lang}`;
            await openInput(page, server, "in-playing", { lang });
            await openPresetState(page, state);
            const probe = await inFrame(page, fitProbe);
            presetPages++;
            run.check(probe.texts > 30, `${tag}：页上应有足够多的可见文字（防空转），实得 ${probe.texts}`);
            const bad = probe.problems.slice(0, 4).map((p) => `${p.kind}：${p.detail}`);
            run.check(probe.problems.length === 0, `${tag}：${bad.join("；")}${probe.problems.length > 4 ? "…" : ""}`);
            const leaks = findLeaks(`${probe.joined} ${probe.attrs.join(" ")}`);
            run.check(
                leaks.length === 0,
                `${tag}：界面上出现了裸键、占位符或 null：${JSON.stringify(leaks.slice(0, 5))}`,
            );
        }
    }
    run.log(`  预设面板子态共量 ${presetPages} 页`);

    run.check(page.errors.length === 0, `页面不应有错误：${page.errors.slice(0, 3).join("；")}`);
    const foreign = requests.filter((u) => !/^(data|blob|about):/.test(u) && new URL(u).origin !== server.url);
    run.check(foreign.length === 0, `页面的请求都必须同源，外部请求：${JSON.stringify(foreign)}`);
} finally {
    await browser.close();
    await server.close();
}
run.finish();
