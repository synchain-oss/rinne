// SPDX-License-Identifier: GPL-3.0-or-later
//
// Input 写入闸的纯逻辑自测（不开浏览器）：宿主的第一批快照到齐之前，名单里的桥函数回「没准备好」且不转给原桥，
// 其余成员原样透传；快照到齐之后全部放行。
//   ① 名单与桥名表对得上：名单 + 放行表 = Input 桥的全部函数，一项不多一项不少。以后桥上加了新函数，必须在这里
//      明确它属于「快照没到也能调」还是「要挡」，不会悄悄漏过去；
//   ② 闸关着：名单里每个函数都回 { ok: false, reason: "not-ready" } 且原桥一次没被调用；
//   ③ 放行表里的函数（含 panic）闸关着也照常转给原桥，参数与返回值原样；
//   ④ 闸开着：名单里的函数转给原桥；
//   ⑤ 原桥上没有的名字不补出来（特性探测的结果不变）；on、role 等成员原样保留。
// 用法：node web-preview/tests/smoke-input-bridge-gate.mjs [仓库根]

import { pathToFileURL } from "node:url";
import path from "node:path";
import { createRun, repoRoot } from "./lib/harness.mjs";

const ROOT = repoRoot();
const u = (rel) => pathToFileURL(path.join(ROOT, rel)).href;
const { GATED_WRITES, gateBridge } = await import(u("web/input/bridge-gate.js"));
const { BRIDGE_FUNCTIONS } = await import(u("web/shared/bridge.js"));

const run = createRun("Input 写入闸");

/** 快照没到也要能调的桥函数：取快照本身、紧急切断、窗口层面的语言与缩放、首启确认、只读或外跳。 */
const OPEN = [
    "requestInitialState",
    "panic",
    "setLang",
    "setUiScale",
    "commitUiScale",
    "setRulesAck",
    "setGuideSeen",
    "copyDiagnostics",
    "openExternal",
    "showLicense",
];

/** 记录调用的原桥替身：每个名字一个函数，回 { ok: true, name }。 */
function makeRaw(names) {
    const calls = [];
    const raw = { role: "input", isPreview: true, on: () => () => {} };
    for (const n of names) {
        raw[n] = async (...a) => {
            calls.push([n, ...a]);
            return { ok: true, name: n };
        };
    }
    return { raw, calls };
}

// ① 名单与名表
const all = [...BRIDGE_FUNCTIONS.input].sort();
run.eq(
    [...GATED_WRITES, ...OPEN].sort(),
    all,
    "写入闸名单 + 放行表应恰好等于 Input 桥的全部函数（新增的桥函数要在这里明确归类）",
);
run.check(new Set(GATED_WRITES).size === GATED_WRITES.length, "写入闸名单不应有重复项");
run.check(Object.isFrozen(GATED_WRITES), "写入闸名单应冻结");

// ② 闸关着
{
    let locked = true;
    const { raw, calls } = makeRaw(BRIDGE_FUNCTIONS.input);
    const gated = gateBridge(raw, () => locked);
    for (const n of GATED_WRITES) {
        const r = await gated[n]("x", 1);
        run.eq(r, { ok: false, reason: "not-ready" }, `闸关着：${n} 应回 not-ready`);
    }
    run.eq(calls.length, 0, "闸关着：名单里的函数不应转给原桥");

    // ③ 放行表
    for (const n of OPEN) {
        const r = await gated[n]("a", 2);
        run.eq(r, { ok: true, name: n }, `闸关着：${n} 应照常转给原桥并原样返回`);
    }
    run.eq(
        calls.map((c) => c[0]),
        OPEN,
        "闸关着：放行表里的函数各转给原桥一次，顺序不变",
    );
    run.eq(
        calls.every((c) => c[1] === "a" && c[2] === 2),
        true,
        "闸关着：放行表里的函数参数应原样透传",
    );

    // ④ 闸开着
    locked = false;
    calls.length = 0;
    for (const n of GATED_WRITES) {
        const r = await gated[n]("y", 3);
        run.eq(r, { ok: true, name: n }, `闸开着：${n} 应转给原桥并原样返回`);
    }
    run.eq(
        calls.map((c) => c[0]),
        [...GATED_WRITES],
        "闸开着：名单里的函数各转给原桥一次",
    );
    run.eq(
        calls.every((c) => c[1] === "y" && c[2] === 3),
        true,
        "闸开着：参数应原样透传",
    );
}

// ⑤ 原桥上没有的名字不补；其余成员保留
{
    const { raw } = makeRaw(BRIDGE_FUNCTIONS.input.filter((n) => n !== "setTailMode" && n !== "loadPreset"));
    const gated = gateBridge(raw, () => true);
    run.eq(typeof gated.setTailMode, "undefined", "原桥没有 setTailMode：闸不应补出来");
    run.eq(typeof gated.loadPreset, "undefined", "原桥没有 loadPreset：闸不应补出来");
    run.eq(gated.role, "input", "role 应保留");
    run.eq(gated.isPreview, true, "isPreview 应保留");
    run.check(gated.on === raw.on, "on 应原样保留");
}

// ⑥ 坏样例：一个不挡任何东西的「闸」过不了上面的判据（判据不是空转）
{
    const { raw, calls } = makeRaw(BRIDGE_FUNCTIONS.input);
    const leaky = gateBridge(raw, () => false);
    await leaky[GATED_WRITES[0]]("z");
    run.check(calls.length === 1, "坏样例：闸一直开着时，名单里的函数应被转给原桥（这正是闸关着时必须判红的行为）");
}

run.finish();
