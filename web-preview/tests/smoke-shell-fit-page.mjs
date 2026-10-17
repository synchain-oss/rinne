// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/web-preview.json
//
// 外壳自适配，页面级冒烟（无头浏览器）。Node 侧那套（smoke-shell-fit.mjs）钉算术与唯一写方，这里钉真实布局：
//   ① 夹具页（设计盒大小的空外壳 + installShellFit）：视口恰为设计盒 × 档位时，外壳铺满视口、倍率等于档位、
//      页面不出滚动条；视口比例不对时短边贴边、长边留黑边、外壳居中、不超框；
//   ② 壳页扮演宿主：iframe 的尺寸是设计盒 × 档位，开窗档位（?scale=）生效，setUiScale 被接受后 iframe 跟着变，
//      被拒绝（不在档位表）时不变；同时端到端验证 mock 注入、首帧与事件分发在浏览器里通。
// 浏览器缺失时整套跳过（退出码 2），不是通过。本机同时只能有一个浏览器冒烟，gates 里持 gui 锁。
//
// 用法：node web-preview/tests/smoke-shell-fit-page.mjs [仓库根]

import { pathToFileURL } from "node:url";
import path from "node:path";
import { createRun, repoRoot } from "./lib/harness.mjs";
import { findBrowser, launchBrowser } from "./lib/browser.mjs";
import { startStaticServer } from "./lib/static-server.mjs";

const ROOT = repoRoot();
const run = createRun("外壳自适配（页面）");
if (!findBrowser()) run.skip("没有 Chromium 系浏览器");

const { DESIGN } = await import(pathToFileURL(path.join(ROOT, "web/shared/design-box.js")).href);
const server = await startStaticServer(ROOT);
const browser = await launchBrowser();

try {
    const page = await browser.newPage();

    // ---- ① 夹具页 ----
    const measure = () =>
        page.eval(`(() => {
            const r = document.getElementById("shell").getBoundingClientRect();
            const de = document.documentElement;
            return { x: r.x, y: r.y, w: r.width, h: r.height, zoom: document.getElementById("shell").style.zoom,
                     sw: de.scrollWidth, cw: de.clientWidth, sh: de.scrollHeight, ch: de.clientHeight };
        })()`);
    for (const role of ["input", "output"]) {
        const { w, h, presets } = DESIGN[role];
        for (const f of presets) {
            await page.setViewport(Math.round(w * f), Math.round(h * f));
            await page.goto(`${server.url}/web-preview/fixtures/common/shell-fit.html?role=${role}`);
            await page.waitFor("window.__fitReady === true");
            const m = await measure();
            run.check(
                Math.abs(m.w - w * f) <= 1.5 && Math.abs(m.h - h * f) <= 1.5,
                `${role} × ${f}：外壳应铺满视口，实得 ${m.w.toFixed(1)}×${m.h.toFixed(1)}`,
            );
            run.check(Math.abs(Number(m.zoom) - f) < 0.01, `${role} × ${f}：倍率应为档位，实得 ${m.zoom}`);
            run.check(
                m.sw <= m.cw && m.sh <= m.ch,
                `${role} × ${f}：页面不应出滚动条（${m.sw}/${m.cw}，${m.sh}/${m.ch}）`,
            );
        }
        // 比例不对：宽视口，短边（高）贴边，左右留黑边且居中
        await page.setViewport(Math.round(w * 2.5), Math.round(h * 1.25));
        await page.goto(`${server.url}/web-preview/fixtures/common/shell-fit.html?role=${role}`);
        await page.waitFor("window.__fitReady === true");
        const m = await measure();
        const vw = Math.round(w * 2.5);
        run.check(Math.abs(m.h - h * 1.25) <= 1.5, `${role}：宽视口时高度贴边，实得 ${m.h.toFixed(1)}`);
        run.check(m.w < vw && Math.abs(m.x * 2 + m.w - vw) <= 2, `${role}：宽视口时外壳居中、左右留黑边`);
        // 运行中改视口：倍率跟着变
        await page.setViewport(Math.round(w * 0.5), Math.round(h * 0.5));
        await page.waitFor(`Math.abs(Number(document.getElementById("shell").style.zoom) - 0.5) < 0.01`, {
            label: `${role}：缩小视口后倍率应变为 0.5`,
        });
        run.check(true, `${role}：运行中缩小视口，倍率跟随`);
    }

    // ---- ② 壳页 = 宿主 ----
    await page.setViewport(1500, 1100);
    await page.goto(`${server.url}/web-preview/input.html?page=probe&scenario=in-playing&scale=1.25&fresh=1`);
    await page.waitFor(`document.querySelector("iframe")?.contentWindow?.__bridgeProbe?.ready === true`, {
        label: "夹具页建桥并请求初始状态",
    });
    const size = () =>
        page.eval(`(() => { const f = document.querySelector("iframe"); return [f.style.width, f.style.height]; })()`);
    run.eq(
        await size(),
        [`${DESIGN.input.w * 1.25}px`, `${Math.round(DESIGN.input.h * 1.25)}px`],
        "开窗档位 ?scale=1.25：iframe 为设计盒 × 1.25",
    );
    const probe = (expr) => page.eval(`document.querySelector("iframe").contentWindow.__bridgeProbe.${expr}`);
    run.check(
        (await probe("counts['rinne.state']")) >= 1 && (await probe("counts['rinne.pair']")) >= 1,
        "夹具页：首帧的 rinne.state 与 rinne.pair 已到",
    );
    run.eq(
        await page.eval(`document.querySelector(".pv-status").dataset.ok`),
        "1",
        "工具条：mock 已注入且页面接了事件",
    );
    run.eq(await probe("last['rinne.state'].uiScale"), 1.25, "rinne.state 带开窗档位");

    const r1 = await probe("bridge.setUiScale(0.5)");
    run.check(r1.ok === true, "setUiScale(0.5) 在档位表内应被接受");
    await page.waitFor(`document.querySelector("iframe").style.width === "400px"`, {
        label: "档位被接受后 iframe 跟着变",
    });
    run.eq(await size(), ["400px", "290px"], "setUiScale(0.5)：iframe 变为设计盒 × 0.5");
    const r2 = await probe("bridge.setUiScale(0.77)");
    run.check(r2.ok === false, "setUiScale(0.77) 不在档位表内应被拒");
    run.eq(await size(), ["400px", "290px"], "被拒的档位不改 iframe 尺寸");

    // Output 角色走同一条路
    await page.goto(`${server.url}/web-preview/output.html?page=probe&scenario=out-connected&fresh=1`);
    await page.waitFor(`document.querySelector("iframe")?.contentWindow?.__bridgeProbe?.ready === true`, {
        label: "Output 夹具页就绪",
    });
    run.eq(await size(), [`${DESIGN.output.w}px`, `${DESIGN.output.h}px`], "Output：iframe 为 Output 设计盒");
    run.eq(await probe("last['rinne.pair'].state"), "connected", "场景 out-connected 的配对状态");

    // 占位页（还没接桥）：工具条如实说明，不当作失败
    await page.goto(`${server.url}/web-preview/input.html`);
    await page.waitFor(
        `document.querySelector(".pv-status")?.dataset.ok === "wait" && document.querySelector(".pv-status").textContent.includes("尚未接桥")`,
        {
            label: "占位页：工具条应显示「页面尚未接桥」",
        },
    );
    run.check(true, "占位页：工具条显示「页面尚未接桥」，不判失败");

    run.check(page.errors.length === 0, `页面不应有错误：${page.errors.join("；")}`);
} finally {
    await browser.close();
    await server.close();
}
run.finish();
