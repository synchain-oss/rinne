// SPDX-License-Identifier: GPL-3.0-or-later
//
// Input 预设与时值下拉的逻辑冒烟（Node，无 DOM、无浏览器）：
//   名称      清洗（去控制字符、首尾空白含 NBSP 与全角空格、24 个码点）、九个非法字符、重名不分大小写、默认名序号；
//             词条 in.preset.badChars 里列的字符与页面真拦的 BAD_CHARS 逐个一致；前端清洗与 mock 清洗对同一批输入结果相同；
//   契约      UI_BRIDGE input 节三条预设函数登记的失败原因码是确切的一组；mock 能触发的原因码都在登记里，
//             登记里 mock 触发不了的只有原生才有的几个（写文件失败、预设文件损坏、参数树缺参数）；
//   语义      rinne.presets 的形状与排序，保存成为当前预设且 dirty 为假，覆盖沿用原 ID，删除当前预设后 state.preset 变空，
//             载入不改环路开关、限制器与上限；
//   时值      delay_note 十六档的顺序在 UI_BRIDGE、参数表头文件、mock 契约、页面（model.js）四处逐档一致。
//
// 用法：node web-preview/tests/smoke-input-presets-model.mjs [仓库根]

import fs from "node:fs";
import path from "node:path";
import { pathToFileURL } from "node:url";
import { createRun, repoRoot } from "./lib/harness.mjs";

const ROOT = repoRoot();
const run = createRun("Input 预设与时值下拉的逻辑");
const LANGS = ["zh", "en", "fr"];

const u = (rel) => pathToFileURL(path.join(ROOT, rel)).href;
const {
    cleanName,
    nameProblem,
    badCharsIn,
    sameName,
    displayName,
    pickDefaultName,
    BAD_CHARS,
    NAME_MAX,
    stripControl,
} = await import(u("web/input/preset-menu.js"));
const { NOTE_IDS } = await import(u("web/input/model.js"));
const common = (await import(u("web/i18n/common.js"))).default;
const inputDict = (await import(u("web/i18n/input.js"))).default;
const { FACTORY_PRESETS, PARAM_DEFS } = await import(u("web-preview/mock/contract.js"));
const { cleanPresetName } = await import(u("web-preview/mock/bridge-input.js"));
const { createPreviewSession } = await import(u("web-preview/mock/state-driver.js"));
const { memoryStorage } = await import(u("web-preview/mock/bridge-common.js"));

const T =
    (lang) =>
    (key, vals = {}) => {
        const table = { ...common[lang], ...inputDict[lang] };
        const raw = Object.prototype.hasOwnProperty.call(table, key) ? table[key] : `!!${key}`;
        return raw.replace(/\{(\w+)\}/g, (m, k) => (k in vals ? String(vals[k]) : m));
    };
const stores = () => ({ local: memoryStorage(), session: memoryStorage() });

// ---------------------------------------------------------------- 名称
{
    const ctl = String.fromCharCode(1, 7, 31, 127, 128, 150, 159);
    run.eq(stripControl(`a${ctl}b`), "ab", "stripControl：C0、DEL、C1 都应去掉");
    run.eq(stripControl("a\tb\nc"), "abc", "stripControl：制表与换行也是控制字符");
    run.eq(cleanName(`  Vox${ctl} 回旋  `), "Vox 回旋", "cleanName：去控制字符并去首尾空白");
    const nbsp = String.fromCharCode(0xa0);
    const ideo = String.fromCharCode(0x3000);
    run.eq(cleanName(`${nbsp}${ideo}x${ideo}${nbsp}`), "x", "cleanName：首尾的 NBSP 与全角空格都要去掉");
    run.eq(cleanName("a".repeat(40)), "a".repeat(NAME_MAX), "cleanName：超过上限按码点截断");
    const astral = String.fromCodePoint(0x1f600);
    run.eq(
        Array.from(cleanName(astral.repeat(40))).length,
        NAME_MAX,
        "cleanName：补充平面字符按码点而不是 UTF-16 单元数",
    );
    run.eq(
        cleanName(`${"a".repeat(NAME_MAX - 1)} bcd`),
        "a".repeat(NAME_MAX - 1),
        "cleanName：截断后新出现的尾部空白要再去掉",
    );
    run.eq(cleanName(null), "", "cleanName：null 当空");
    run.eq(cleanName(`${ctl}  `), "", "cleanName：只剩控制字符与空白就是空");
    run.eq(nameProblem(""), "empty", "nameProblem：空名");
    run.eq(nameProblem("ok"), null, "nameProblem：正常的名字没有问题");
    run.eq(BAD_CHARS.length, 9, "BAD_CHARS：规格列了九个字符");
    for (const c of BAD_CHARS) {
        run.eq(nameProblem(`a${c}b`), "badChars", `nameProblem：含 ${c} 应判非法字符`);
        run.eq(badCharsIn(`a${c}b`), [c], `badCharsIn：只含 ${c}`);
    }
    run.eq(nameProblem("a-b_c (1) 回旋 é"), null, "nameProblem：其余可见字符都允许");

    // 词条里列出的非法字符与页面实际拦的 BAD_CHARS 一致：zh 逐字照规格，en 与 fr 的直双引号写成弯双引号
    const BS = String.fromCharCode(92);
    const listed = (lang) => {
        const msg = T(lang)("in.preset.badChars");
        return msg.slice(msg.indexOf(BS)).replace("“ ”", '"').split(/\s+/).filter(Boolean);
    };
    for (const lang of LANGS) {
        run.eq(listed(lang), [...BAD_CHARS], `词条 in.preset.badChars（${lang}）列出的字符应与 BAD_CHARS 逐个一致`);
    }

    run.check(sameName("Vox 回旋", " vox 回旋 "), "sameName：不分大小写、不计首尾空白");
    run.check(!sameName("Vox", "Vox2"), "sameName：不同名字不算重名");
    run.eq(displayName({ zh: "甲", en: "A", fr: "Af" }, "fr"), "Af", "displayName：工厂预设取当前语言");
    run.eq(displayName({ zh: "甲", en: "A" }, "fr"), "A", "displayName：缺当前语言时退到 en");
    run.eq(displayName("用户", "en"), "用户", "displayName：用户预设是字符串，原样");
    run.eq(displayName(null, "zh"), "", "displayName：没有名字给空串");
    const make = (n) => `My ${n}`;
    run.eq(pickDefaultName([], make), "My 1", "pickDefaultName：空列表从 1 起");
    run.eq(pickDefaultName(["a", "b"], make), "My 3", "pickDefaultName：从用户预设数加 1 起");
    // 起点（用户预设数加 1）被占才会跳：两条用户预设时起点是 3，所以占的得是 3，不是 2
    run.eq(pickDefaultName(["a", "my 3"], make), "My 4", "pickDefaultName：起点被占（不分大小写）就往后找");
    run.eq(pickDefaultName(["My 3", "My 4", "z"], make), "My 5", "pickDefaultName：连着被占就连着跳过");

    // 前端清洗与 mock 清洗（对应原生预设库的规则）对同一批输入结果相同
    const battery = [
        "",
        "   ",
        "plain",
        `  pad ${ctl} ed  `,
        `${nbsp}${ideo}lead`,
        `trail${ideo}${nbsp}`,
        "字".repeat(30),
        astral.repeat(30),
        `${"a".repeat(NAME_MAX - 1)} tail`,
        `${"a".repeat(NAME_MAX)} tail`,
        `${ctl}${ctl}`,
        "a b",
        "Tab\there",
    ];
    for (const s of battery) {
        run.eq(cleanName(s), cleanPresetName(s), `前端清洗与 mock 清洗应一致：${JSON.stringify(s)}`);
    }
}

// ---------------------------------------------------------------- 契约：失败原因码
{
    const doc = fs.readFileSync(path.join(ROOT, "docs/UI_BRIDGE.md"), "utf8");
    const a = doc.indexOf("<!-- section:input -->");
    const b = doc.indexOf("<!-- section:output -->");
    const input = doc.slice(a, b);
    const cell = (fn) => {
        const row = input.split("\n").find((l) => l.startsWith(`| \`${fn}\` |`));
        return row ? row.split("|").slice(4).join("|") : "";
    };
    /** 一行里「失败回 `{ ok: false, reason }`」之后，每个带括号说明的 `原因码`，按出现顺序。 */
    const codes = (fn) => {
        const text = cell(fn);
        const from = text.indexOf("reason");
        return from < 0 ? [] : [...text.slice(from).matchAll(/`([a-z]+(?:-[a-z]+)*)`（/g)].map((m) => m[1]);
    };
    const want = {
        loadPreset: ["not-found", "bad-file", "not-ready"],
        savePreset: ["exists", "bad-name", "write-failed"],
        deletePreset: ["not-found", "factory", "write-failed"],
    };
    for (const fn of Object.keys(want)) run.eq(codes(fn), want[fn], `UI_BRIDGE：${fn} 登记的失败原因码`);

    // mock 能触发的原因码都在登记里；登记里 mock 触发不了的只有原生才有的三个
    const NATIVE_ONLY = new Set(["bad-file", "not-ready", "write-failed"]);
    const s = createPreviewSession({ role: "input", params: "auto=0", storage: stores() });
    s.mock.requestInitialState();
    const m = s.mock;
    const seen = { loadPreset: new Set(), savePreset: new Set(), deletePreset: new Set() };
    const note = (fn, r) => {
        if (r && r.ok === false) seen[fn].add(r.reason);
    };
    note("loadPreset", m.loadPreset("nope"));
    note("loadPreset", m.loadPreset("__proto__"));
    const saved = m.savePreset("Probe", false);
    note("savePreset", m.savePreset("Probe", false));
    note("savePreset", m.savePreset(" \u0001 ", false));
    note("deletePreset", m.deletePreset("init"));
    note("deletePreset", m.deletePreset("nope"));
    run.check(saved.ok, "对拍准备：保存成功");
    for (const fn of Object.keys(want)) {
        const got = [...seen[fn]].sort();
        const documented = want[fn];
        run.check(
            got.every((c) => documented.includes(c)),
            `${fn}：mock 触发的原因码 ${JSON.stringify(got)} 都应在 UI_BRIDGE 登记里 ${JSON.stringify(documented)}`,
        );
        run.eq(
            documented.filter((c) => !seen[fn].has(c)).sort(),
            documented.filter((c) => NATIVE_ONLY.has(c)).sort(),
            `${fn}：登记里 mock 没触发到的，应恰好是原生才有的原因码`,
        );
    }
    s.stop();
}

// ---------------------------------------------------------------- 语义：rinne.presets、保存、覆盖、删除
{
    const s = createPreviewSession({ role: "input", params: "auto=0", storage: stores() });
    const m = s.mock;
    m.requestInitialState();
    let presets = null;
    let state = null;
    m.addEventListener("rinne.presets", (p) => (presets = p));
    m.addEventListener("rinne.state", (p) => (state = p));
    const pull = () => {
        s.core.push("rinne.presets");
        s.core.push("rinne.state");
    };
    pull();
    run.eq(
        presets.factory.map((p) => p.id),
        FACTORY_PRESETS.map((p) => p.id),
        "rinne.presets：工厂预设八个，ID 与顺序同契约",
    );
    run.check(
        presets.factory.every((p) => LANGS.every((l) => typeof p.name[l] === "string" && p.name[l] !== "")),
        "rinne.presets：工厂预设三语名齐全",
    );
    run.eq(presets.user, [], "rinne.presets：起始没有用户预设");
    run.eq(presets.current, state.preset.id, "rinne.presets.current 与 state.preset.id 同值");

    const ids = {};
    for (const n of ["b second", "A first", "c third"]) ids[n] = m.savePreset(n, false).id;
    pull();
    run.eq(
        presets.user.map((p) => p.name),
        ["A first", "b second", "c third"],
        "rinne.presets：用户预设按名字不分大小写排序",
    );
    run.check(
        presets.user.every((p) => /^u-[0-9a-f]{12}$/.test(p.id)),
        "rinne.presets：用户预设 ID 形如 u-<12 位十六进制>",
    );
    run.eq(
        [state.preset.id, state.preset.dirty, state.preset.name],
        [ids["c third"], false, "c third"],
        "保存后当前预设是刚存的，dirty 为假",
    );

    m.setParam("feedback", 33);
    pull();
    run.eq(state.preset.dirty, true, "改了参数后 state.preset.dirty 为真");
    const ow = m.savePreset("C THIRD", true);
    pull();
    run.eq(ow.id, ids["c third"], "覆盖保存沿用原 ID");
    run.eq(presets.user.length, 3, "覆盖保存不新增条目");
    run.eq(state.preset.dirty, false, "覆盖保存后 dirty 为假");

    m.loadPreset(ids["A first"]);
    pull();
    run.eq([state.preset.id, state.preset.dirty], [ids["A first"], false], "载入后当前预设换成所载的，dirty 为假");
    run.check(m.deletePreset(ids["c third"]).ok, "删除非当前的用户预设");
    pull();
    run.eq(state.preset.id, ids["A first"], "删除别的预设不改当前预设");
    run.check(m.deletePreset(ids["A first"]).ok, "删除当前预设");
    pull();
    run.eq(
        state.preset,
        { id: null, dirty: false, name: null },
        "删掉当前预设后 state.preset 变空（id 与 name 为 null）",
    );
    run.eq(presets.current, null, "删掉当前预设后 rinne.presets.current 为 null");
    run.eq(
        presets.user.map((p) => p.name),
        ["b second"],
        "删除后列表只剩没删的",
    );
    s.stop();
}

// ---------------------------------------------------------------- 预览场景：用户预设段
{
    const open = (scenario) => {
        const s = createPreviewSession({ role: "input", params: `auto=0&scenario=${scenario}`, storage: stores() });
        s.mock.requestInitialState();
        const got = { presets: null, state: null };
        s.mock.addEventListener("rinne.presets", (p) => (got.presets = p));
        s.mock.addEventListener("rinne.state", (p) => (got.state = p));
        s.core.push("rinne.presets");
        s.core.push("rinne.state");
        return { s, ...got };
    };
    const NAMES = ["Vox 回旋", "Long tail 2", "我的超长预设名称用于测试截断效果甲乙"];
    const menu = open("in-preset-menu");
    run.eq(
        menu.presets.user.map((p) => p.name).sort(),
        [...NAMES].sort(),
        "场景 in-preset-menu：场景里写的三个用户预设都进了 rinne.presets.user",
    );
    run.check(
        menu.presets.user.every((p) => /^u-[0-9a-f]{12}$/.test(p.id)) &&
            new Set(menu.presets.user.map((p) => p.id)).size === NAMES.length,
        "场景 in-preset-menu：用户预设 ID 形如 u-<12 位十六进制>且互不相同",
    );
    run.eq(menu.presets.current, "slapback", "场景 in-preset-menu：当前预设是工厂的 slapback");
    const exempt = ["loop_on", "limiter", "ceiling"];
    const entries = menu.s.model.userPresets;
    run.check(
        entries.every(
            (e) =>
                exempt.every((k) => !(k in e.values)) &&
                e.values.feedback === menu.s.model.values.feedback &&
                e.values.mix === menu.s.model.values.mix,
        ),
        "场景 in-preset-menu：用户预设的参数取场景套完底座后的模型值，且不含环路开关、限制器与上限",
    );
    run.eq(
        menu.s.mock.savePreset("Fresh take", false).id,
        "u-000000000004",
        "场景造的三个预设占了前三个编号，之后保存的接着往下排",
    );
    menu.s.stop();
    const plain = open("in-playing");
    run.eq(plain.presets.user, [], "场景 in-playing 没写 user：用户预设段是空的");
    plain.s.stop();
}

// ---------------------------------------------------------------- mock：载入后「淡入中」标签的计时
{
    // 假定时器，手动推进；setInterval 只占个号（回声不在这里测）
    let now = 0;
    let id = 0;
    const q = new Map();
    const timers = {
        setTimeout(fn, ms) {
            q.set(++id, { at: now + ms, fn });
            return id;
        },
        clearTimeout(i) {
            q.delete(i);
        },
        setInterval: () => ++id,
        clearInterval() {},
        tick(ms) {
            const end = now + ms;
            for (;;) {
                const next = [...q.entries()].filter(([, t]) => t.at <= end).sort((a, b) => a[1].at - b[1].at)[0];
                if (!next) break;
                q.delete(next[0]);
                now = next[1].at;
                next[1].fn();
            }
            now = end;
        },
    };
    const s = createPreviewSession({ role: "input", params: "auto=0", storage: stores(), timers });
    const m = s.mock;
    m.requestInitialState();
    let state = null;
    m.addEventListener("rinne.state", (p) => (state = p));
    const fade = () => {
        s.core.push("rinne.state");
        return state.safety.fadeIn;
    };
    run.eq(fade(), false, "淡入标签：没载入过时没有");
    m.loadPreset("slapback");
    run.eq(fade(), true, "淡入标签：载入后出现");
    timers.tick(509);
    run.eq(fade(), true, "淡入标签：只载入一次时，509 ms 还在");
    timers.tick(1);
    run.eq(fade(), false, "淡入标签：只载入一次时，510 ms 撤掉");

    // 相隔 300 ms 载入两次：第一次的计时器不能抢先撤掉标签，要从最后一次载入算起满 510 ms
    m.loadPreset("slapback");
    timers.tick(300);
    m.loadPreset("pingPong");
    timers.tick(300);
    run.eq(fade(), true, "淡入标签：相隔 300 ms 载入两次，第一次载入后 600 ms 时标签还在");
    timers.tick(209);
    run.eq(fade(), true, "淡入标签：第二次载入后 509 ms 标签还在");
    timers.tick(1);
    run.eq(fade(), false, "淡入标签：第二次载入后满 510 ms 才撤掉");
    s.stop();
}

// ---------------------------------------------------------------- 时值：delay_note 十六档的顺序
{
    const doc = fs.readFileSync(path.join(ROOT, "docs/UI_BRIDGE.md"), "utf8");
    const row = doc.split("\n").find((l) => l.startsWith("| `delay_note` |"));
    const bridgeOrder = [...row.split("|")[2].matchAll(/`(n\d+[td]?)`/g)].map((x) => x[1]);
    const table = fs.readFileSync(path.join(ROOT, "src/input/ParamTable.h"), "utf8");
    const tableOrder = [...table.matchAll(/RINNE_CHOICE\(delay_note,\s*\d+,\s*(\w+),/g)].map((x) => x[1]);
    run.eq(bridgeOrder.length, 16, "UI_BRIDGE 的 delay_note 取值表应列 16 档");
    run.eq(tableOrder, bridgeOrder, "参数表头文件与 UI_BRIDGE 的档位顺序应逐档一致");
    run.eq(PARAM_DEFS.delay_note.values, bridgeOrder, "mock 契约与 UI_BRIDGE 的档位顺序应逐档一致");
    run.eq([...NOTE_IDS], bridgeOrder, "页面（model.js 的 NOTE_IDS）与 UI_BRIDGE 的档位顺序应逐档一致");
    for (const lang of LANGS) {
        for (const id of bridgeOrder) {
            run.check(!T(lang)(`in.dly.note.${id}`).startsWith("!!"), `词条 in.dly.note.${id}（${lang}）应存在`);
        }
    }
}

run.finish();
