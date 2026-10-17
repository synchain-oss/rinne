// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/web-preview.json
//
// 首帧竞态回归（Node，无 DOM）。守的是：mock 的周期定时器在页面调 requestInitialState() 之前就已经在跑，
// 那几拍必须被门控丢掉；如果门控期照样记账，门控解除后的「首帧必发」就会被自己那几拍悄悄抑制，
// 实测症状是页面只收到 rinne.state，配对状态永远不亮。
// 同时钉住 mock 与真桥同形的几条时序：回声异步、过期帧按 seq 可识别、页面一直不请求时 mock 代请求（可关）。
//
// 用法：node web-preview/tests/smoke-ready-race.mjs [仓库根]

import { pathToFileURL } from "node:url";
import path from "node:path";
import { createRun, repoRoot } from "./lib/harness.mjs";
import { awaitState } from "./lib/await-state.mjs";

const ROOT = repoRoot();
const run = createRun("首帧竞态");
const u = (p) => pathToFileURL(path.join(ROOT, p)).href;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const { createBridge, BRIDGE_EVENTS } = await import(u("web/shared/bridge.js"));
const { createPreviewSession } = await import(u("web-preview/mock/state-driver.js"));
const { ECHO_MS, AUTO_INITIAL_MS } = await import(u("web-preview/mock/bridge-common.js"));

/** 首帧必发的状态类事件，逐角色。rinne.error 是按需事件，不在其内。 */
const REQUIRED = {
    input: ["rinne.state", "rinne.pair", "rinne.params", "rinne.meters", "rinne.presets"],
    output: ["rinne.state", "rinne.pair"],
};

function attach(role, params) {
    const session = createPreviewSession({ role, params });
    const bridge = createBridge({ role, mockBackend: session.mock });
    const seen = new Map();
    const order = [];
    for (const ev of BRIDGE_EVENTS[role]) {
        bridge.on(ev, (p) => {
            seen.set(ev, (seen.get(ev) || 0) + 1);
            seen.set(`${ev}:last`, p);
            if (ev === "rinne.state") order.push(p.seq);
        });
    }
    return { session, bridge, seen, order };
}

const count = (seen, ev) => seen.get(ev) || 0;

for (const role of ["input", "output"]) {
    // ① 页面晚调 requestInitialState：门控期一帧不发，请求之后首帧一次到齐
    {
        const { session, bridge, seen } = attach(role, "auto=0&stale=0");
        session.start();
        await sleep(ECHO_MS * 2 + 80);
        // 请求之前的写操作也会排一次回声，同样必须被门控丢掉
        await bridge.setLang("en");
        await sleep(40);
        run.eq(
            [...seen.keys()].filter((k) => !k.endsWith(":last")),
            [],
            `${role}（晚调）：请求之前不应收到任何事件`,
        );
        await bridge.requestInitialState();
        for (const need of REQUIRED[role]) {
            run.check(count(seen, need) === 1, `${role}（晚调）：首帧缺 ${need} 或重复（${count(seen, need)} 次）`);
        }
        session.stop();
    }

    // ② 页面根本不调：mock 在 AUTO_INITIAL_MS 之后代请求
    {
        const { session, seen } = attach(role, "stale=0");
        session.start();
        await sleep(AUTO_INITIAL_MS + 200);
        run.check(session.core.autoCalled, `${role}（代请求）：页面不请求时 mock 应当代请求`);
        for (const need of REQUIRED[role]) run.check(count(seen, need) >= 1, `${role}（代请求）：首帧缺 ${need}`);
        session.stop();
    }

    // ③ ?auto=0：关掉代请求后一直静默，排查时序时不会被 mock 偷偷补上
    {
        const { session, seen } = attach(role, "auto=0");
        session.start();
        await sleep(AUTO_INITIAL_MS + 200);
        run.check(!session.core.autoCalled && count(seen, "rinne.state") === 0, `${role}（auto=0）：不应代请求`);
        session.stop();
    }
}

// ④ 回声异步：写函数的回执先到，rinne.state 晚一拍才反映新值
{
    const { session, bridge, seen } = attach("input", "auto=0&stale=0");
    session.start();
    await bridge.requestInitialState();
    const before = seen.get("rinne.state:last");
    run.eq(before.lang, "zh", "回声：初始语言应为 zh");
    const receipt = await bridge.setLang("fr");
    run.check(receipt.ok && receipt.lang === "fr", "回声：setLang 的回执应当立刻返回新语言");
    run.eq(seen.get("rinne.state:last").lang, "zh", "回声：回执到了而 rinne.state 不应同步更新");
    await awaitState(
        () => seen.get("rinne.state:last").lang,
        (v) => v === "fr",
        "语言回声",
    );
    run.check(true, "回声：rinne.state 随后到达");
    session.stop();
}

// ⑤ 过期帧：周期回声里隔几拍补发一帧序号更小的旧快照，页面必须能按 seq 识别
{
    const { session, bridge, order } = attach("input", "auto=0");
    session.start();
    await bridge.requestInitialState();
    await sleep(ECHO_MS * 9 + 100);
    let regress = 0;
    for (let i = 1; i < order.length; i++) if (order[i] < order[i - 1]) regress++;
    run.check(
        session.core.sent["rinne.state.stale"] >= 1 && regress >= 1,
        `过期帧：应当观察到 seq 倒退（倒退 ${regress} 次）`,
    );
    // 页面纪律：按最大 seq 过滤之后，序列单调
    let max = -1;
    const kept = order.filter((s) => (s > max ? ((max = s), true) : false));
    run.check(
        kept.every((s, i) => i === 0 || s > kept[i - 1]),
        "过期帧：按 seq 过滤后应当单调",
    );
    session.stop();
}

// ⑥ 冷却：恢复回送只有一个入口，切断后 1.5 s 内被拒
{
    const { session, bridge } = attach("input", "auto=0&stale=0");
    session.start();
    await bridge.requestInitialState();
    await bridge.panic();
    const early = await bridge.resumeLoop();
    run.check(early.ok === false && early.reason === "cooldown", "冷却：切断后立刻恢复应当被拒");
    await bridge.panic();
    const again = await bridge.resumeLoop();
    run.check(again.ok === false, "冷却：连点 Panic 不会恢复");
    session.stop();
}

run.finish();
