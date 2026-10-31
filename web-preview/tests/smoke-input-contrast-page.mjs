// SPDX-License-Identifier: GPL-3.0-or-later
//
// Input 页面文字对比度冒烟（无头浏览器）：范围内的 32 个 in-* 场景 × 中 / 英 / 法。
// 量法与共享件的对比度冒烟相同：页内采样每段可见文字的矩形、计算色与各层不透明度，把页面文字改成透明后截图得到合成底，
// 逐像素把文字色合成上去，取矩形内最差的一格。分档：
//   A（≥ 4.5）：危险与确认横幅、模态里的文字（含限制器关闭确认框）、横幅与消息位里的按钮、紧急切断；
//   B（≥ 3.0）：琥珀 / 灰 / 紫横幅、配对状态灯；
//   C：其余只记录不判（mono 小标等按规范不设门禁）。
// 预设面板：场景表只有列表与重名两态，其余子态逐语言逐子态摆出来量，那一段在 smoke-input-contrast-presets-page.mjs（拆出去是为了让两个文件
//   各自跑在 run-all 单项 300 秒的上限之内，判据一条没动）。这里留下场景表里两个 in-preset-* 场景与一个预设红字提示的坏样例。
//   分档：条目、动作与按钮名按 A，红字提示按 A，琥珀提示按 B，预设按钮在内容层里（旁路时随内容层降级）也按 B，标签与两处说明文字（设计稿给的 --txt-3）按 C 只记录，置灰的不判。
// 宿主旁路：设计稿把内容层降到 0.55（等同停用的界面），这一层的文字按 C 档只记录不判；
//   但紧急切断、消息位（红字、恢复按钮）、危险描边不在降级层里，照 A / B 档判，并用有效不透明度断言它们没被降级。
//   场景表里的旁路场景之外，另有「旁路 + 紧急切断」「旁路 + 沙盒拒绝共享内存」两个红字用例，要求量到 A 档红字并过线。
// 采样两遍、截图前后核对矩形，页面没静止就不量（不拿一张拼凑的画面去量）。
// 坏样例：把一段 A 档文字改成与底近似的颜色，量尺必须量出低于门槛；采样为空必须报错（防空转）；
//   把消息位或紧急切断挪进降级层、或把内容层整体降回 0.55，旁路断言必须报出来；
//   强加等待态把配对 pill 压暗到 0.6，量尺必须量出低于门槛，失败信息必须带出有效不透明度、旁路、等待态、inert 与进行中的动画。
// 量到低于门槛时，失败信息后面附页面现场（见 describeFailure），下次再红时分得清是量尺的问题还是页面状态。
// 浏览器缺失时整套跳过（退出码 2），不是通过。本机同时只能有一个浏览器冒烟。
//
// 用法：node web-preview/tests/smoke-input-contrast-page.mjs [仓库根]

import { createRun, repoRoot } from "./lib/harness.mjs";
import { findBrowser, launchBrowser } from "./lib/browser.mjs";
import { startStaticServer } from "./lib/static-server.mjs";
import { TIER_MIN, worstContrast } from "./shared-measure.mjs";
import {
    LANGS,
    SCENES,
    VIEW,
    describeFailure,
    inFrame,
    measure,
    openInput,
    openPresetState,
    settle,
} from "./smoke-input-kit-page.mjs";

const ROOT = repoRoot();
const run = createRun("Input 页面对比度");
if (!findBrowser()) run.skip("没有 Chromium 系浏览器");

/**
 * 宿主旁路时被设计稿降到 0.55 的内容层文字：不是消息位里的字、不是 A 档，且有效不透明度明显低于 1。
 * 这一层按 C 档只记录不判；安全层（紧急切断、恢复按钮）、消息位与 A 档文字不在其内，降了也照门槛判。
 */
const isDegraded = (it) => !it.inLine && it.tier !== "A" && it.op < 0.99;

/**
 * 旁路下安全相关的几处必须不降级，页内取各自的有效不透明度（沿祖先连乘）。返回问题清单，空表示都对：
 *   消息位（非空闲时）、紧急切断、危险描边（外壳本身与 ::after）有效不透明度 1；
 *   内容层的件仍按设计稿降到 0.55（页眉为例），否则旁路降级悄悄失效。
 */
const safetyNotDegraded = (page) =>
    inFrame(page, (doc, win) => {
        const eff = (e) => {
            let o = 1;
            for (let a = e; a && a !== doc.documentElement; a = a.parentElement)
                o *= Number(win.getComputedStyle(a).opacity);
            return o;
        };
        const problems = [];
        const shell = doc.querySelector('[data-gb="input.shell"]');
        if (!shell || !shell.hasAttribute("data-bypass")) return ["外壳不在旁路状态"];
        const line = doc.querySelector(".rn-msgline");
        if (line && line.getAttribute("data-state") !== "idle" && eff(line) < 0.99)
            problems.push(`消息位有效不透明度 ${eff(line).toFixed(2)}`);
        for (const b of doc.querySelectorAll(".rn-msgline .rn-btn"))
            if (eff(b) < 0.99) problems.push(`消息位按钮「${b.textContent.trim()}」有效不透明度 ${eff(b).toFixed(2)}`);
        const panic = doc.querySelector('[data-gb="input.panic"]');
        if (panic && eff(panic) < 0.99) problems.push(`紧急切断有效不透明度 ${eff(panic).toFixed(2)}`);
        if (shell.getAttribute("data-danger") === "1") {
            if (eff(shell) < 0.99) problems.push(`危险描边所在的外壳有效不透明度 ${eff(shell).toFixed(2)}`);
            const ring = Number(win.getComputedStyle(shell, "::after").opacity);
            if (ring < 0.99) problems.push(`危险描边不透明度 ${ring.toFixed(2)}`);
        }
        const header = doc.querySelector('[data-gb="input.header"]');
        if (header && Math.abs(eff(header) - 0.55) > 0.02)
            problems.push(`旁路下页眉应仍按设计稿降到 0.55，实得 ${eff(header).toFixed(2)}`);
        return problems;
    });

const server = await startStaticServer(ROOT);
const browser = await launchBrowser({ width: VIEW.w, height: VIEW.h });
try {
    const page = await browser.newPage();
    let pages = 0;
    const tally = { A: 0, B: 0, C: 0 };
    const bypassScenes = [];
    let degraded = 0;
    for (const lang of LANGS) {
        for (const scene of SCENES) {
            const tag = `${scene.id}/${lang}`;
            await openInput(page, server, scene.id, { lang });
            // 缩放确认的 10 秒倒计时每秒重写一次文本，量尺的取样窗口会和它共振；量的是画面不是倒计时，冻结页内定时器
            if (scene.id === "in-scale-confirm")
                await inFrame(page, (doc, win) => {
                    for (let i = 1; i < 4096; i++) win.clearInterval(i);
                });
            const items = await measure(page);
            pages++;
            // 模态与确认框打开时只量它们自己里面的字，段数天然少
            const overlay = scene.id === "in-scale-confirm" || scene.id === "in-about";
            run.check(
                items.length > (overlay ? 2 : 10),
                `${tag}：应采到足够多的文字段（防空转），实得 ${items.length}`,
            );
            // 宿主旁路时设计稿把内容层降到 0.55（等同停用的界面），这一层按 C 档只记录不判；
            // 安全层（紧急切断、恢复按钮）与消息位不在降级层里，照自己的档判（见 safetyNotDegraded）
            const bypass = await inFrame(page, (doc) =>
                doc.querySelector('[data-gb="input.shell"]').hasAttribute("data-bypass"),
            );
            if (bypass) bypassScenes.push(scene.id);
            for (const it of items) {
                tally[it.tier]++;
                const min = TIER_MIN[it.tier];
                if (!min) continue;
                if (bypass && isDegraded(it)) {
                    degraded++;
                    continue;
                }
                const ok = it.worst.min >= min;
                run.check(
                    ok,
                    `${tag}：${it.tier} 档文字 ${JSON.stringify(it.text)}（${it.sel}）对比度 ${it.worst.min.toFixed(2)} 低于 ${min}；` +
                        (ok ? "" : await describeFailure(page, it)),
                );
            }
            if (bypass) run.eq(await safetyNotDegraded(page), [], `${tag}：旁路下安全层与消息位不得被降级`);
        }
    }
    run.log(
        `  共量 ${pages} 页；A 档 ${tally.A} 段、B 档 ${tally.B} 段、C 档 ${tally.C} 段（只记录）；旁路降级层 ${degraded} 段按 C 档只记录`,
    );
    run.check(tally.A > 0 && tally.B > 0, "A 档与 B 档都应采到文字（防空转）");

    run.eq([...new Set(bypassScenes)], ["in-bypassed"], "场景表里只有 in-bypassed 带宿主旁路");
    run.check(degraded > 0, "旁路场景里应有被降级的内容层文字（防止旁路降级悄悄失效）");

    // ---- 旁路 + 红字：消息位里的红字与按钮必须量到 A 档并过线；危险描边与紧急切断同样不降级
    const redCases = [
        {
            name: "旁路 + 紧急切断",
            scene: "in-panic",
            setup: `(() => { const s = window.__RINNE_PREVIEW__; s.model.silentCause = "hostBypass"; s.core.push("rinne.state"); })()`,
            wantButton: true,
        },
        {
            name: "旁路 + 沙盒拒绝共享内存",
            scene: "in-bypassed",
            setup: `(() => { const s = window.__RINNE_PREVIEW__; s.model.notices = [{ id: "ipcSandbox" }]; s.core.push("rinne.state"); })()`,
            wantButton: true,
        },
    ];
    for (const lang of LANGS) {
        for (const c of redCases) {
            const tag = `${c.name}/${lang}`;
            await openInput(page, server, c.scene, { lang });
            await page.eval(c.setup);
            await page.waitFor(
                `(() => { const d = document.querySelector("iframe").contentDocument; const b = d.querySelector('[data-gb="input.band"]'); return !!b && b.getAttribute("data-tone") === "red" && !!d.querySelector('[data-gb="input.shell"][data-bypass]'); })()`,
                { label: `${tag}：应出现红色消息且外壳处于旁路` },
            );
            const items = await measure(page);
            const red = items.filter((i) => i.inLine && i.tone === "red" && i.tier === "A");
            run.check(red.length > 0, `${tag}：应量到消息位里 A 档的红字（防空转）`);
            for (const it of red) {
                run.check(
                    it.op >= 0.99,
                    `${tag}：红字 ${JSON.stringify(it.text)} 的有效不透明度 ${it.op.toFixed(2)} 应为 1`,
                );
                const ok = it.worst.min >= TIER_MIN.A;
                run.check(
                    ok,
                    `${tag}：红字 ${JSON.stringify(it.text)} 对比度 ${it.worst.min.toFixed(2)} 低于 ${TIER_MIN.A}；` +
                        (ok ? "" : await describeFailure(page, it)),
                );
            }
            if (c.wantButton) {
                const btn = items.filter((i) => i.inLine && i.inBtn && i.tier === "A");
                run.check(btn.length > 0, `${tag}：消息位里应量到按钮文字（恢复 / 查看说明）`);
                for (const it of btn)
                    run.check(
                        it.op >= 0.99 && it.worst.min >= TIER_MIN.A,
                        `${tag}：按钮 ${JSON.stringify(it.text)} 不透明度 ${it.op.toFixed(2)}、对比度 ${it.worst.min.toFixed(2)} 应不降级并过 ${TIER_MIN.A}`,
                    );
            }
            if (c.scene === "in-panic") {
                const cut = items.filter(
                    (i) => i.tier === "A" && /切断|cut|coupure|紧急|panic|urgence/i.test(i.text) && !i.inLine,
                );
                for (const it of cut)
                    run.check(
                        it.op >= 0.99,
                        `${tag}：紧急切断文字 ${JSON.stringify(it.text)} 的有效不透明度 ${it.op.toFixed(2)} 应为 1`,
                    );
            }
            run.eq(await safetyNotDegraded(page), [], `${tag}：安全层与消息位不得被降级`);
        }
    }

    // ---- 坏样例：把安全层挪进降级层，safetyNotDegraded 必须报出来
    for (const [label, mutate] of [
        [
            "消息位落进降级层",
            (doc) => {
                doc.querySelector(".rn-msgline").style.setProperty("opacity", "0.55", "important");
            },
        ],
        [
            "紧急切断落进降级层",
            (doc) => {
                const p = doc.querySelector('[data-gb="input.panic"]');
                const wrap = doc.createElement("div");
                wrap.className = "in-abs";
                wrap.append(p);
                doc.querySelector(".rn-shell__content").append(wrap);
            },
        ],
        [
            "内容层整体降 0.55（旁路规则退回设计稿原样）",
            (doc) => {
                doc.querySelector(".rn-shell__content").style.setProperty("opacity", "0.55", "important");
            },
        ],
    ]) {
        await openInput(page, server, "in-panic", { lang: "zh" });
        await page.eval(redCases[0].setup);
        await page.waitFor(
            `(() => !!document.querySelector("iframe").contentDocument.querySelector('[data-gb="input.shell"][data-bypass]'))()`,
            { label: "坏样例：外壳应处于旁路" },
        );
        run.eq(await safetyNotDegraded(page), [], `坏样例前提（${label}）：改坏之前应没有问题`);
        await inFrame(page, mutate);
        run.check((await safetyNotDegraded(page)).length > 0, `坏样例：${label}，应被 safetyNotDegraded 报出来`);
    }
    // 坏样例：旁路下内容层一旦不降级，isDegraded 不得凭空放行（按 op 判，不按场景名）
    run.check(
        isDegraded({ op: 1, inLine: false, tier: "B" }) === false &&
            isDegraded({ op: 0.55, inLine: false, tier: "B" }) === true,
        "isDegraded 只按有效不透明度判",
    );
    run.check(
        isDegraded({ op: 0.55, inLine: true, tier: "B" }) === false &&
            isDegraded({ op: 0.55, inLine: false, tier: "A" }) === false,
        "消息位与 A 档文字永远不算降级层：降了也照门槛判",
    );

    // ---- 坏样例：把一段 A 档文字（紧急切断）的颜色改成与底近似，量尺必须量出低于门槛
    await openInput(page, server, "in-playing", { lang: "zh" });
    await inFrame(page, (doc) => {
        const e = doc.querySelector(".in-panic");
        e.style.setProperty("color", "rgba(255, 190, 190, 0.55)", "important");
        for (const c of e.querySelectorAll("*")) c.style.setProperty("color", "rgba(255, 190, 190, 0.55)", "important");
    });
    {
        const items = await measure(page);
        const panic = items.filter((i) => i.tier === "A" && /切断/.test(i.text));
        run.check(panic.length > 0, "坏样例：应采到紧急切断的文字");
        run.check(
            panic.some((i) => i.worst.min < TIER_MIN.A),
            `坏样例：把紧急切断改成浅红后对比度应低于 ${TIER_MIN.A}，实得 ${JSON.stringify(panic.map((i) => +i.worst.min.toFixed(2)))}`,
        );
    }
    // ---- 坏样例：预设面板里的红字提示（A 档）改成与底近似的颜色，量尺必须量出低于门槛
    await openInput(page, server, "in-playing", { lang: "zh" });
    await openPresetState(page, "saveFail");
    await inFrame(page, (doc) => {
        doc.querySelector(".in-pm__msg").style.setProperty("color", "rgba(255, 200, 200, 0.6)", "important");
    });
    {
        const items = await measure(page);
        const msg = items.filter((i) => i.tier === "A" && /保存失败/.test(i.text));
        run.check(msg.length > 0, "坏样例：应采到预设面板的保存失败提示");
        run.check(
            msg.some((i) => i.worst.min < TIER_MIN.A),
            `坏样例：把保存失败提示改成浅红后对比度应低于 ${TIER_MIN.A}，实得 ${JSON.stringify(msg.map((i) => +i.worst.min.toFixed(2)))}`,
        );
    }
    // ---- 坏样例：强加等待态（内容层 data-pending、配对行 inert），配对 pill 的文字被压暗到 0.6，量尺必须量出低于 B 档门槛；
    //      失败信息要带得出现场：有效不透明度 0.60、data-pending=1、inert 里有配对行、进行中的动画（这一页的脉冲带 ∞）
    await openInput(page, server, "in-playing", { lang: "zh" });
    await inFrame(page, (doc) => {
        doc.querySelector('[data-part="content"]').setAttribute("data-pending", "1");
        doc.querySelector(".in-pairrow").setAttribute("inert", "");
    });
    await settle(page);
    {
        const items = await measure(page);
        const pill = items.filter((i) => i.tier === "B" && /rn-pair__text/.test(i.sel));
        run.check(pill.length > 0, "坏样例：应采到配对 pill 的文字");
        run.check(
            pill.length > 0 && pill.every((i) => i.worst.min < TIER_MIN.B && Math.abs(i.op - 0.6) < 0.01),
            `坏样例：强加等待态后配对 pill 的对比度应低于 ${TIER_MIN.B}、有效不透明度应是 0.60，实得 ${JSON.stringify(pill.map((i) => [+i.worst.min.toFixed(2), +i.op.toFixed(2)]))}`,
        );
        const msg = pill.length ? await describeFailure(page, pill[0]) : "";
        run.check(
            /有效不透明度 0\.60/.test(msg) &&
                /data-pending=1/.test(msg) &&
                /data-bypass=0/.test(msg) &&
                /inert 1 处\[in-pairrow\]/.test(msg) &&
                /进行中的动画 [1-9]\d* 条\[[^\]]*∞/.test(msg),
            `坏样例：失败信息应带出有效不透明度、等待态、旁路、inert 与进行中的动画，实得 ${JSON.stringify(msg)}`,
        );
    }
    // 坏样例：矩形在截图之外时一个像素都没采到，必须抛错而不是悄悄通过
    {
        let threw = false;
        try {
            worstContrast(
                { w: 10, h: 10, ch: 4, data: Buffer.alloc(400) },
                { c: { r: 0, g: 0, b: 0, a: 1 }, op: 1, r: { x: 50, y: 50, w: 5, h: 5 }, text: "x", sel: "x" },
            );
        } catch {
            threw = true;
        }
        run.check(threw, "坏样例：矩形与截图没有交集时应抛错");
    }

    run.check(page.errors.length === 0, `页面不应有错误：${page.errors.slice(0, 3).join("；")}`);
} finally {
    await browser.close();
    await server.close();
}
run.finish();
