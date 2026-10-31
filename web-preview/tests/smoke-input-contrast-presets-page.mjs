// SPDX-License-Identifier: GPL-3.0-or-later
//
// Input 页面文字对比度冒烟，预设面板的子态（无头浏览器）：中 / 英 / 法 × 八种子态（列表、长名当前、保存输入、名称为空、
// 非法字符、重名、保存失败、删除确认）。场景表里只有列表与重名两态，其余子态经真实交互摆出来（kit 的 openPresetState）。
// 量法与 smoke-input-contrast-page 完全相同（页内采样、截图合成底、逐像素取矩形内最差的一格，量尺在 kit 里），分档也相同：
//   条目、动作与按钮名按 A，红字提示按 A，琥珀提示按 B，预设按钮在内容层里也按 B，标签与两处说明文字（设计稿给的 --txt-3）按 C 只记录，置灰的不判。
// 本文件由 smoke-input-contrast-page 拆出：加了这 24 页之后那一个文件逼近 run-all 的单项 300 秒上限，拆开后各自留有余量；
// 判据与断言一条没动。浏览器缺失时整套跳过（退出码 2），不是通过。本机同时只能有一个浏览器冒烟。
//
// 用法：node web-preview/tests/smoke-input-contrast-presets-page.mjs [仓库根]

import { createRun, repoRoot } from "./lib/harness.mjs";
import { findBrowser, launchBrowser } from "./lib/browser.mjs";
import { startStaticServer } from "./lib/static-server.mjs";
import { TIER_MIN } from "./shared-measure.mjs";
import {
    LANGS,
    PRESET_STATES,
    VIEW,
    describeFailure,
    inFrame,
    measure,
    openInput,
    openPresetState,
} from "./smoke-input-kit-page.mjs";

const ROOT = repoRoot();
const run = createRun("Input 页面预设子态对比度");
if (!findBrowser()) run.skip("没有 Chromium 系浏览器");

const server = await startStaticServer(ROOT);
const browser = await launchBrowser({ width: VIEW.w, height: VIEW.h });
try {
    const page = await browser.newPage();

    // ---- 预设面板的子态
    let presetPages = 0;
    for (const lang of LANGS) {
        for (const state of PRESET_STATES) {
            const tag = `预设面板 ${state}/${lang}`;
            await openInput(page, server, "in-playing", { lang });
            await openPresetState(page, state);
            const items = await measure(page);
            presetPages++;
            const judged = items.filter((i) => /in-pm|in-pt/.test(i.sel) && TIER_MIN[i.tier]);
            run.check(judged.length > 0, `${tag}：应采到面板里 A 或 B 档的文字（防空转），实得 ${judged.length}`);
            for (const it of items) {
                const min = TIER_MIN[it.tier];
                if (!min) continue;
                const ok = it.worst.min >= min;
                run.check(
                    ok,
                    `${tag}：${it.tier} 档文字 ${JSON.stringify(it.text)}（${it.sel}）对比度 ${it.worst.min.toFixed(2)} 低于 ${min}；` +
                        (ok ? "" : await describeFailure(page, it)),
                );
            }
        }
    }
    run.log(`  预设面板子态共量 ${presetPages} 页`);

    run.check(page.errors.length === 0, `页面不应有错误：${page.errors.slice(0, 3).join("；")}`);
} finally {
    await browser.close();
    await server.close();
}
run.finish();
