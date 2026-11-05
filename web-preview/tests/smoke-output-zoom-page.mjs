// SPDX-License-Identifier: GPL-3.0-or-later
//
// Output 全部缩放档位的页面冒烟（无头浏览器）：真页面（夹具 web-preview/fixtures/output）× 中 / 英 / 法 × 设计盒的全部缩放预设。
// 页面自己的 shell-fit 跟着视口把外壳 zoom 到档位，所以这里只改视口，不手写 zoom。每个档位摆最坏组合：
//   · 已连接、超长轨道名（单声道 + 采样率最宽的写法）、14 条消息逐条、接管确认条、位置提示关着；
// 每个画面量三件事：
//   ① 外壳确实缩放到这一档；
//   ② 适配量尺（共用的 OUT_FIT_PROBE）没有越界、被裁、互相压住的文字；超长轨道名按设计被省略号截断，量尺会报成 clipped，按名字放行；
//   ③ 关键文字（位置提示、消息位、副行、轨道名、发送行、状态胶囊）的行数与 1 档一致——文字在小数缩放下换了行就是版式不稳。
// 末尾有坏样例：把副行撑大，量尺必须看得出。
// 浏览器缺失时整套跳过（退出码 2）。单个冒烟有 300 秒上限，这一套只覆盖缩放，其余版式断言在 smoke-output-chain-page 与 smoke-output-fit-page。
//
// 用法：node web-preview/tests/smoke-output-zoom-page.mjs [仓库根]

import path from "node:path";
import { pathToFileURL } from "node:url";
import { createRun, repoRoot } from "./lib/harness.mjs";
import { findBrowser, launchBrowser } from "./lib/browser.mjs";
import { startStaticServer } from "./lib/static-server.mjs";
import { LANGS, MESSAGE_CASES, OUT_FIT_PROBE, loadDesign, openOutput, setModel } from "../fixtures/output/lib.mjs";

const ROOT = repoRoot();
const run = createRun("Output 缩放档位");
const load = (rel) => import(pathToFileURL(path.join(ROOT, rel)).href);
const { DESIGN } = await load("web/shared/design-box.js");
if (!findBrowser()) run.skip("没有 Chromium 系浏览器");

const box = await loadDesign(ROOT);
const server = await startStaticServer(ROOT);
const browser = await launchBrowser();

const LONG = "Chœurs doublés – refrain 2 (bus stéréo) – prise alternative très longue 浓缩人声总线 final mix";
/** 超长轨道名按设计被省略号截断，量尺会把它们报成 clipped / outside：按名字放行，其余照报。 */
const ELLIPSIS_OK = /track.(this|input).name|Chœurs doublés/;

/** 一组画面共用一张页（开页参数相同）：[开页场景, 开页附加参数, 该组的 [名字, 模型] 表]。 */
const GROUPS = [
    [
        "out-connected",
        "",
        [
            ["已连接", {}],
            [
                "超长轨道名",
                {
                    state: "crossTrack",
                    verify: "diff",
                    peer: { name: LONG, channels: 1 },
                    self: { name: LONG },
                    send: { channels: "MONO", sampleRate: 44100 },
                },
            ],
            ...Object.entries(MESSAGE_CASES).map(([id, [notices, state, peer]]) => [
                "消息 " + id,
                {
                    notices,
                    state,
                    peer: peer.version || peer.sampleRate ? peer : {},
                    group: state === "unassigned" ? null : "A",
                },
            ]),
        ],
    ],
    ["out-takeover-confirm", "", [["接管确认条", null]]],
    ["out-connected", "&placeHidden=1", [["位置提示关着", { state: "conflict" }]]],
];
const CASES = GROUPS.reduce((n, g) => n + g[2].length, 0);

/** 各关键文字的行数（只数框内露出的行，被 line-clamp 截掉的不算；顶边按当前档位归一）。 */
const SIG = `(() => {
    const lines = (sel) => [...document.querySelectorAll(sel)].filter((e) => !e.hidden && e.getBoundingClientRect().width > 0).map((e) => {
        const tops = new Set();
        const bottom = e.getBoundingClientRect().bottom;
        const w = document.createTreeWalker(e, NodeFilter.SHOW_TEXT);
        for (let n = w.nextNode(); n; n = w.nextNode()) {
            if (!n.nodeValue.trim()) continue;
            const r = document.createRange();
            r.selectNodeContents(n);
            for (const x of r.getClientRects()) if (x.width > 0 && x.top < bottom - 1) tops.add(Math.round(x.top / (3 * window.__z)));
        }
        return tops.size;
    });
    return JSON.stringify({
        place: lines(".rn-place__text"),
        band: lines('[data-gb="output.band"] .rn-banner__text'),
        sub: lines(".rn-paircard__sub"),
        name: lines(".rn-track__name"),
        send: lines(".rn-send"),
        pill: lines(".rn-pair"),
    });
})()`;

const fitTo = async (page, z) => {
    await page.setViewport(Math.round(box.w * z), Math.round(box.h * z));
    await page.eval(`window.__z = ${z}`);
};
const settle = (page) => page.eval("new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)))");

try {
    const page = await browser.newPage();
    const zooms = [1, ...DESIGN.output.presets.filter((x) => x !== 1)];
    const baseByLang = new Map();
    for (const lang of LANGS) {
        let n = 0;
        const base = new Map();
        baseByLang.set(lang, base);
        for (const [scenario, extra, cases] of GROUPS) {
            await openOutput(page, server, box, lang, scenario, extra);
            for (const z of zooms) {
                await fitTo(page, z);
                for (const [name, model] of cases) {
                    if (model) await setModel(page, model);
                    await settle(page);
                    const where = `${lang}/${z} 倍/${name}`;
                    const zoom = Number(await page.eval(`document.querySelector(".rn-shell").style.zoom || "1"`));
                    run.check(Math.abs(zoom - z) < 0.01, `${where}：外壳应缩放到这一档，实得 ${zoom}`);
                    const probe = await page.eval(OUT_FIT_PROBE);
                    const kept = probe.problems.filter(
                        (q) => !(name === "超长轨道名" && ELLIPSIS_OK.test(q.detail || "")),
                    );
                    run.eq(kept, [], `${where}：不应有越界、裁切或互相压住`);
                    const sig = await page.eval(SIG);
                    if (z === 1) base.set(name, sig);
                    run.eq(sig, base.get(name), `${where}：关键文字的行数应与 1 档一致`);
                    n++;
                }
            }
        }
        run.eq(n, zooms.length * CASES, `${lang}：每个档位 × 每种组合都量到`);
    }

    // ---- 坏样例：量尺必须抓得出 ----
    await openOutput(page, server, box, "zh", "out-connected");
    await fitTo(page, 2);
    await page.eval(
        `(() => { const s = document.createElement("style"); s.textContent = '.rn-paircard__sub{font-size:30px !important}'; document.head.append(s); })()`,
    );
    await settle(page);
    {
        const probe = await page.eval(OUT_FIT_PROBE);
        run.check(probe.problems.length > 0, "坏样例：副行字号撑到 30 px，缩放档位下量尺也应看得出越界或裁切");
        const sig = await page.eval(SIG);
        run.check(sig !== baseByLang.get("zh").get("已连接"), "坏样例：副行字号撑大后行数签名应与 1 档不同");
    }

    run.check(page.errors.length === 0, `页面不应有错误：${page.errors.slice(0, 3).join("；")}`);
} finally {
    await browser.close();
    await server.close();
}
run.finish();
