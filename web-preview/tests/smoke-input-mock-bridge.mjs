// SPDX-License-Identifier: GPL-3.0-or-later
//
// Input mock 跟着原生桥补的几条语义（Node，无 DOM）。页面开发和页面冒烟都站在 mock 上，mock 说错了页面就跟着错：
//   单位      setFbUnit、setDelayUnit 只收契约里的取值，收下才进 cfg，非法值回 bad-value、不动原值；
//   导出确认  dismissHint("exportMismatch") 是确认：原因位与计数清零，不进 dismissedHints；别的提示 id 才进；
//   诊断失败  两个失败开关：not-ready 预览与写剪贴板都失败；clipboard 只有真写时失败，预览照常、剪贴板没写；
//   外链      带查询串或片段的一律拒绝，包括解析后看不出来的单独一个 ? 或 #。
//
// 用法：node web-preview/tests/smoke-input-mock-bridge.mjs [仓库根]

import { pathToFileURL } from "node:url";
import path from "node:path";
import { createRun, repoRoot } from "./lib/harness.mjs";

const ROOT = repoRoot();
const run = createRun("Input mock 桥语义");
const u = (p) => pathToFileURL(path.join(ROOT, p)).href;

const { createPreviewSession } = await import(u("web-preview/mock/state-driver.js"));
const { memoryStorage, isAllowedExternal } = await import(u("web-preview/mock/bridge-common.js"));

const open = () => {
    const s = createPreviewSession({
        role: "input",
        params: "auto=0",
        storage: { local: memoryStorage(), session: memoryStorage() },
    });
    s.mock.requestInitialState();
    return s;
};
const lastState = (session) => {
    let last = null;
    const off = session.mock.addEventListener("rinne.state", (p) => (last = p));
    session.core.push("rinne.state");
    if (typeof off === "function") off();
    return last;
};

// ---- 单位 ----
{
    const s = open();
    const cases = [
        ["setFbUnit", "fbUnit", ["db", "pct"], "pct"],
        ["setDelayUnit", "delayUnit", ["samples", "ms"], "ms"],
    ];
    for (const [fn, key, values, initial] of cases) {
        run.eq(lastState(s).cfg[key], initial, `${fn}：默认单位`);
        for (const value of values) {
            run.eq(s.mock[fn](value), { ok: true, value }, `${fn}：${value} 收下，回执带值`);
            run.eq(lastState(s).cfg[key], value, `${fn}：${value} 进 cfg`);
        }
        s.mock[fn](values[0]);
        for (const bad of ["", "PCT", "%", "beats", 1, null, undefined, {}]) {
            run.eq(s.mock[fn](bad), { ok: false, reason: "bad-value" }, `${fn}：非法值 ${String(bad)} 回 bad-value`);
        }
        run.eq(lastState(s).cfg[key], values[0], `${fn}：非法值不动原值`);
    }
    // 两个单位各管各的
    s.mock.setFbUnit("db");
    s.mock.setDelayUnit("ms");
    run.eq(lastState(s).cfg.fbUnit, "db", "改延迟单位不动反馈单位");
    s.stop();
}

// ---- 导出确认 ----
{
    const s = open();
    s.model.exportCauses = { bits: 5, count: 3 };
    run.eq(lastState(s).exportCauses, { bits: 5, count: 3 }, "导出：状态里带原因位与计数");
    run.eq(s.mock.dismissHint("exportMismatch"), { ok: true }, "导出确认：回执 ok");
    const after = lastState(s);
    run.eq(after.exportCauses, { bits: 0, count: 0 }, "导出确认：原因位与计数清零");
    run.eq(after.dismissedHints, [], "导出确认：不进 dismissedHints");
    run.eq(s.mock.dismissHint("someHint"), { ok: true }, "别的提示：回执 ok");
    run.eq(lastState(s).dismissedHints, ["someHint"], "别的提示：进 dismissedHints");
    run.eq(lastState(s).exportCauses, { bits: 0, count: 0 }, "别的提示：不动导出计数");
    run.eq(s.mock.dismissHint(""), { ok: false, reason: "bad-id" }, "空 id 回 bad-id");
    s.stop();
}

// ---- 诊断失败开关 ----
{
    const s = open();
    const good = s.mock.copyDiagnostics(false);
    run.check(good.ok && good.copied === false && good.text.startsWith("rinne-diag v1\n"), "诊断：不设开关时预览正常");

    s.model.diagFail = "not-ready";
    run.eq(s.mock.copyDiagnostics(false), { ok: false, copied: false, reason: "not-ready" }, "not-ready：预览也失败");
    run.eq(
        s.mock.copyDiagnostics(true),
        { ok: false, copied: false, reason: "not-ready" },
        "not-ready：写剪贴板也失败",
    );

    s.model.diagFail = "clipboard";
    const peek = s.mock.copyDiagnostics(false);
    run.check(peek.ok && peek.copied === false && peek.text.length > 0, "clipboard：预览不受影响");
    run.eq(s.mock.copyDiagnostics(true), { ok: false, copied: false, reason: "clipboard" }, "clipboard：真写时失败");

    s.model.diagFail = undefined;
    const done = s.mock.copyDiagnostics(true);
    run.check(done.ok && done.copied === true && done.text.length > 0, "撤掉开关后真写成功");
    s.stop();
}

// ---- 外链：查询串与片段 ----
for (const [url, want] of [
    ["https://synchain.ca/docs", true],
    ["https://github.com/synchain-oss/rinne/blob/dev/README.md", true],
    ["https://synchain.ca/docs?x=1", false],
    ["https://synchain.ca/docs#top", false],
    ["https://synchain.ca/?", false],
    ["https://synchain.ca/#", false],
    ["https://github.com/synchain-oss/rinne?tab=readme", false],
    ["https://github.com/synchain-oss/rinne#readme", false],
]) {
    run.eq(isAllowedExternal(url), want, `外链：${url}`);
}

run.finish();
