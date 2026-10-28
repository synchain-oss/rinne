// SPDX-License-Identifier: GPL-3.0-or-later
//
// mock 后端的行为冒烟（Node，无 DOM）：和 smoke-ready-race 同一套装置，守的是页面开发时会依赖的几条桥语义。
//   全局位    语言、规则确认、引导已看、缩放档位存同一份存储，Input 与 Output 两个会话共享；?fresh=1 先清空；
//             存储里被写坏的值（类型不对、未知键）读出来时被丢掉，不会带进状态；每实例的已关闭提示不进全局位；
//   外链      openExternal 只放行 https 的 synchain.ca 与本项目的 github.com 路径；
//   诊断      copyDiagnostics(false) 只给预览不写剪贴板；文本是 rinne-diag v1 的分节形态（第一行格式标识、每节一行、
//             最近事件段、末行 ui 段），用到的名字都在白名单里，Output 只有字段子集，不含路径、邮箱和主机名；
//   参数      setParam 夹取与校验（越界夹到边界、类型不符拒绝、未知 ID 拒绝，步进量化），setLimiter 等价于 limiter 参数；
//   预设      工厂预设 8 个，载入不改 loop_on、limiter、ceiling；保存名去控制字符、限 24 个字符、重名要 overwrite；
//   场景      ?scenario= 不走原型链，未知场景、窗口不符、目录格都只告警，按默认态打开；
//   档位      setUiScale 只收该角色设计盒的档位，commitUiScale 才落全局位。
//
// 用法：node web-preview/tests/smoke-ready-race-globals.mjs [仓库根]

import { pathToFileURL } from "node:url";
import path from "node:path";
import { createRun, repoRoot } from "./lib/harness.mjs";

const ROOT = repoRoot();
const run = createRun("mock 行为");
const u = (p) => pathToFileURL(path.join(ROOT, p)).href;

const { createPreviewSession } = await import(u("web-preview/mock/state-driver.js"));
const { buildDiagnostics, diagNamesOf, memoryStorage, isAllowedExternal, previewDiagValues, UI_DEFAULTS_KEY } =
    await import(u("web-preview/mock/bridge-common.js"));
const { cleanPresetName, coerceParam } = await import(u("web-preview/mock/bridge-input.js"));
const { DIAG_INPUT_ONLY_SECTIONS, DIAG_KEYS, FACTORY_PRESETS, PARAM_DEFS, PRESET_NAME_MAX } = await import(
    u("web-preview/mock/contract.js")
);
const { DESIGN } = await import(u("web/shared/design-box.js"));

const stores = () => ({ local: memoryStorage(), session: memoryStorage() });
const open = (role, params, storage) => {
    const s = createPreviewSession({ role, params, storage });
    s.mock.requestInitialState();
    return s;
};
const lastState = (session) => {
    let last = null;
    session.mock.addEventListener("rinne.state", (p) => (last = p));
    session.core.push("rinne.state");
    return last;
};

// ---- 全局位 ----
{
    const st = stores();
    const a = open("input", "auto=0", st);
    const b = open("output", "auto=0", st);
    a.mock.setLang("fr");
    a.mock.setRulesAck(3);
    a.mock.setGuideSeen(true);
    run.eq(lastState(b).lang, "fr", "全局位：Input 改的语言，Output 会话读到");
    run.eq(lastState(b).rulesAck, 3, "全局位：规则确认版本共享");
    run.eq(lastState(b).inputGuideSeen, true, "全局位：Output 能知道 Input 的引导已看");
    run.eq(lastState(b).guideSeen, false, "全局位：两个角色的引导已看互不相干");
    run.check(
        st.session.getItem("rinne.dismissedHints.input") === null &&
            !(st.local.getItem(UI_DEFAULTS_KEY) || "").includes("dismissed"),
        "全局位：已关闭提示不进全局位",
    );
    a.mock.dismissHint("in.hint.unpaired");
    run.eq(
        JSON.parse(st.session.getItem("rinne.dismissedHints.input")),
        ["in.hint.unpaired"],
        "已关闭提示存 sessionStorage，每实例各自",
    );
    const c = open("input", "auto=0&fresh=1", st);
    run.eq(lastState(c).lang, "zh", "?fresh=1：清空全局位后回到默认语言");
    run.eq(lastState(c).dismissedHints, [], "?fresh=1：同时清空本实例的已关闭提示");
    const dirty = stores();
    dirty.local.setItem(
        UI_DEFAULTS_KEY,
        JSON.stringify({ lang: 42, rulesAckVersion: "x", extra: 1, guideSeenInput: true }),
    );
    const d = open("input", "auto=0", dirty);
    run.eq(
        [lastState(d).lang, lastState(d).rulesAck, lastState(d).guideSeen],
        ["zh", 0, true],
        "全局位：类型不对的值丢掉，类型对的保留，未知键忽略",
    );
    dirty.local.setItem(UI_DEFAULTS_KEY, "{不是 JSON");
    run.eq(lastState(open("input", "auto=0", dirty)).lang, "zh", "全局位：存储里不是 JSON 时回到默认");
    for (const s of [a, b, c, d]) s.stop();
}

// ---- 外链 ----
for (const [url, want] of [
    ["https://synchain.ca", true],
    ["https://synchain.ca/docs?x=1", true],
    ["https://github.com/synchain-oss/rinne", true],
    ["https://github.com/synchain-oss/rinne/issues/1", true],
    ["http://synchain.ca", false],
    ["https://github.com/synchain-oss/rinne-evil", false],
    ["https://github.com/synchain-oss", false],
    ["https://github.com/other/rinne", false],
    ["https://synchain.ca.evil.example/", false],
    ["https://evil.example/synchain.ca", false],
    ["https://user:pw@synchain.ca/", false],
    ["https://synchain.ca:8443/", false],
    ["javascript:alert(1)", false],
    ["file:///etc/passwd", false],
    ["", false],
    [null, false],
]) {
    run.eq(isAllowedExternal(url), want, `openExternal 白名单：${String(url)}`);
}

// ---- 诊断 ----
const sectionOf = (name) => name.slice(0, name.indexOf("."));
for (const role of ["input", "output"]) {
    const s = open(role, "auto=0", stores());
    s.model.pair.state = "connected"; // 有对端时 peer 一节才有字段
    const peek = s.mock.copyDiagnostics(false);
    run.check(peek.ok && peek.copied === false, `诊断（${role}）：预览不写剪贴板`);
    const names = diagNamesOf(peek.text);
    const stray = names.filter((k) => !DIAG_KEYS.includes(k));
    run.check(names.length > 0 && stray.length === 0, `诊断（${role}）：只含白名单字段，越界的有 ${stray.join("、")}`);
    run.eq(new Set(names).size, names.length, `诊断（${role}）：名字不重复`);
    run.check(!/[A-Za-z]:[\/]|@|\bUsers\b/.test(peek.text), `诊断（${role}）：不含路径、邮箱和用户目录`);
    run.check(peek.text.startsWith("rinne-diag v1\n"), `诊断（${role}）：第一行是格式标识`);
    const lines = peek.text.trimEnd().split("\n");
    run.check(/^ui: webview=\S+ scale=\S+ lang=\S+$/.test(lines[lines.length - 1]), `诊断（${role}）：末行是 ui 段`);
    const head = lines.findIndex((l) => /^events\(last \d+ of \d+\):$/.test(l));
    const shown = head < 0 ? -1 : Number(/last (\d+)/.exec(lines[head])[1]);
    run.check(
        head >= 0 &&
            shown <= 64 &&
            lines.slice(head + 1, -1).every((l) => l.startsWith("  t+")) &&
            lines.length === head + shown + 2,
        `诊断（${role}）：事件段的条数与标题一致且不超过 64 条`,
    );
    const allInputOnly = DIAG_KEYS.filter((k) => DIAG_INPUT_ONLY_SECTIONS.includes(sectionOf(k)));
    const inputOnly = names.filter((k) => DIAG_INPUT_ONLY_SECTIONS.includes(sectionOf(k)));
    if (role === "input") run.eq(inputOnly, allInputOnly, "诊断（input）：loop、sync、order、safety 四节齐全");
    else run.eq(inputOnly, [], "诊断（output）：只有字段子集，没有 loop、sync、order、safety");
    const expected = DIAG_KEYS.filter((k) => role === "input" || !allInputOnly.includes(k));
    run.eq(
        names,
        expected,
        `诊断（${role}）：有对端时名字与白名单（${role === "input" ? "全集" : "子集"}）逐项一致、顺序相同`,
    );
    run.check(s.mock.copyDiagnostics(true).copied === true, `诊断（${role}）：确认后才写剪贴板`);
    s.model.pair.state = "searching";
    run.check(
        s.mock.copyDiagnostics(false).text.includes("\npeer: none\n"),
        `诊断（${role}）：没有对端时 peer 一行写 none`,
    );
    s.stop();
}

// 诊断值里的换行、等号和路径字符会被清洗：不能另起一行、冒充键，也不会带出路径或邮箱
{
    const o = open("output", "auto=0", stores());
    const dirty = buildDiagnostics("output", {
        ...previewDiagValues("output", o.model, o.core),
        "product.host": "D:\\work\\x user=evil",
        "backend.group": "A\nfake: x=1",
        "track.relation": "me@example.invalid",
    });
    o.stop();
    const stray = diagNamesOf(dirty).filter((k) => !DIAG_KEYS.includes(k));
    run.eq(stray, [], "诊断：值里的换行与等号不能另起一行或冒充键");
    run.check(!/[A-Za-z]:[\\/]|@/.test(dirty), "诊断：值里的路径和邮箱字符被清洗");
}

// ---- 参数 ----
{
    const s = open("input", "auto=0", stores());
    const m = s.mock;
    run.eq(m.setParam("feedback", 999).value, 150, "setParam：越界夹到上界");
    run.eq(m.setParam("feedback", -5).value, 0, "setParam：越界夹到下界");
    run.eq(m.setParam("ceiling", -6.04).value, -6, "setParam：ceiling 按 0.1 步进量化");
    run.check(m.setParam("loop_on", 1).ok === false, "setParam：布尔参数拒绝数字");
    run.check(m.setParam("feedback", "50").ok === false, "setParam：浮点参数拒绝字符串");
    run.check(m.setParam("feedback", NaN).ok === false, "setParam：拒绝 NaN");
    run.check(m.setParam("delay_note", "n7").ok === false, "setParam：枚举拒绝表外取值");
    run.check(m.setParam("delay_note", "n8d").ok === true, "setParam：枚举接受表内取值");
    for (const bad of ["constructor", "__proto__", "toString", "mix_mode", "dry", ""]) {
        run.check(
            m.setParam(bad, 1).ok === false && m.beginParamGesture(bad).ok === false,
            `setParam：未知 ID ${JSON.stringify(bad)} 被拒（含已删除的 mix_mode、dry）`,
        );
    }
    run.eq(m.setLimiter(false).value, false, "setLimiter(false) 等价于改 limiter 参数");
    run.check(m.setLimiter("no").ok === false, "setLimiter：拒绝非布尔");
    run.eq(Object.keys(PARAM_DEFS).length, 11, "参数表共 11 个");
    run.eq(coerceParam("mix", 25), 25, "mix 默认值过自己的夹取");
    s.stop();
}

// ---- 预设 ----
{
    const s = open("input", "auto=0", stores());
    const m = s.mock;
    run.eq(FACTORY_PRESETS.length, 8, "工厂预设共 8 个");
    m.setParam("loop_on", false);
    m.setParam("limiter", false);
    m.setParam("ceiling", -12);
    m.setParam("feedback", 30);
    run.check(m.loadPreset("longHold").ok, "载入工厂预设");
    let params = null;
    m.addEventListener("rinne.params", (p) => (params = p));
    s.core.push("rinne.params");
    run.eq([params.values.feedback, params.values.mix], [92, 50], "载入预设：feedback 与 mix 来自预设");
    run.eq(
        [params.values.loop_on, params.values.limiter, params.values.ceiling],
        [false, false, -12],
        "载入预设：不改 loop_on、limiter、ceiling",
    );
    run.check(m.loadPreset("nope").ok === false && m.loadPreset("__proto__").ok === false, "载入预设：未知 ID 被拒");
    for (const p of FACTORY_PRESETS) {
        run.check(
            Object.values(p.name).every(Boolean) && (p.values.feedback ?? 0) <= 100,
            `工厂预设 ${p.id}：三语名齐全，feedback 不超过 100%`,
        );
        run.check(
            !("loop_on" in p.values) && !("limiter" in p.values) && !("ceiling" in p.values),
            `工厂预设 ${p.id}：不含 loop_on、limiter、ceiling`,
        );
    }
    run.eq(cleanPresetName("  a\u0000b\u001fc\u007f  "), "abc", "预设名：去控制字符与首尾空白");
    run.eq(Array.from(cleanPresetName("字".repeat(40))).length, PRESET_NAME_MAX, "预设名：限 24 个字符");
    run.eq(Array.from(cleanPresetName("😀".repeat(30))).length, PRESET_NAME_MAX, "预设名：按字符而不是码元计数");
    const saved = m.savePreset("我的预设", false);
    run.check(saved.ok, "保存预设");
    run.eq(m.savePreset("我的预设", false).reason, "exists", "保存预设：重名且未允许覆盖被拒");
    run.check(m.savePreset("我的预设", true).ok, "保存预设：允许覆盖时成功");
    run.eq(m.savePreset("   ", false).reason, "bad-name", "保存预设：空名被拒");
    run.check(m.deletePreset(saved.id).ok && m.deletePreset(saved.id).ok === false, "删除预设：只能删一次");
    run.check(m.deletePreset("longHold").ok === false, "删除预设：工厂预设删不掉");
    s.stop();
}

// ---- 场景与档位 ----
{
    for (const bad of ["constructor", "__proto__", "toString", "hasOwnProperty", "nope"]) {
        const s = createPreviewSession({ role: "input", params: `scenario=${bad}` });
        run.check(s.info.scenario === null && s.info.warnings.length === 1, `?scenario=${bad}：不命中任何场景并告警`);
    }
    const wrongWindow = createPreviewSession({ role: "input", params: "scenario=out-connected" });
    run.check(
        wrongWindow.info.scenario === null && wrongWindow.info.warnings.length === 1,
        "场景窗口不符：告警并按默认态",
    );
    const catalog = createPreviewSession({ role: "input", params: "scenario=cat-pair" });
    run.check(
        catalog.info.scenario === null && catalog.info.warnings.length === 1,
        "目录格没有对应页面：告警并按默认态",
    );
    const ok = createPreviewSession({ role: "input", params: "scenario=in-fr-worst" });
    run.eq([ok.info.scenario, ok.info.lang], ["in-fr-worst", "fr"], "场景语言：没给 ?lang= 时取场景自己的");
    run.eq(
        createPreviewSession({ role: "input", params: "scenario=in-fr-worst&lang=en" }).info.lang,
        "en",
        "?lang= 优先于场景语言",
    );
    run.eq(createPreviewSession({ role: "input", params: "lang=de" }).info.warnings.length, 1, "未知语言告警");

    const s = open("input", "auto=0", stores());
    const presets = DESIGN.input.presets;
    run.check(
        s.mock.setUiScale(presets[0]).ok && s.mock.setUiScale(0.77).ok === false && s.mock.setUiScale("1").ok === false,
        "setUiScale：只收设计盒的档位",
    );
    run.check(open("output", "auto=0", stores()).mock.setUiScale(0.33).ok, "setUiScale：Output 有 0.33 档");
    run.check(s.mock.setUiScale(0.33).ok === false, "setUiScale：Input 没有 0.33 档");
    const st = stores();
    const t = open("input", "auto=0", st);
    t.mock.setUiScale(1.5);
    run.check(!(st.local.getItem(UI_DEFAULTS_KEY) || "").includes("1.5"), "setUiScale：确认之前不落全局位");
    t.mock.commitUiScale();
    run.eq(JSON.parse(st.local.getItem(UI_DEFAULTS_KEY)).uiScaleInput, 1.5, "commitUiScale：落全局位");
    run.eq(JSON.parse(st.local.getItem(UI_DEFAULTS_KEY)).uiScaleOutput, 1, "commitUiScale：只改本角色的档位");
    run.check(t.mock.commitUiScale().ok === false, "commitUiScale：没有待确认的档位时被拒");
    s.stop();
    t.stop();
}

// ---- 新增动作 ----
{
    const d = open("input", "auto=0", stores());
    const dirtyOf = () => lastState(d).preset.dirty;
    d.mock.setLimiter(false);
    d.mock.setParam("loop_on", false);
    d.mock.setParam("ceiling", -10);
    run.eq(dirtyOf(), false, "limiter、loop_on、ceiling 不进预设，改它们不弄脏预设");
    d.mock.setParam("feedback", 10);
    run.eq(dirtyOf(), true, "改 feedback 弄脏预设");
    d.stop();
    const s = open("input", "scenario=in-conflict&auto=0", stores());
    const m = s.mock;
    run.check(m.setGuardOn(false).ok && m.setGuardOn("x").ok === false, "setGuardOn：只收布尔");
    run.check(m.setTailMode("30s").ok && m.setTailMode("nope").ok === false, "setTailMode：只收表内取值");
    run.check(m.setResetOnPlay(false).ok, "setResetOnPlay");
    run.eq(m.moveToFreeGroup().ok, true, "moveToFreeGroup：冲突态可以移走");
    run.eq(m.moveToFreeGroup().reason, "not-conflict", "moveToFreeGroup：不在冲突态被拒");
    run.eq(m.resolveTakeover(true).reason, "no-pending", "resolveTakeover：没有待确认时被拒");
    run.check(m.setDiagLog(true).ok && m.setDiagLog(1).ok === false, "setDiagLog：只收布尔");
    run.check(m.showLicense().ok, "showLicense");
    s.stop();
    const t = open("output", "scenario=out-takeover-confirm&auto=0", stores());
    run.eq(lastState(t).takeover, true, "接管场景：state 带 takeover");
    run.check(
        t.mock.resolveTakeover(false).ok && lastState(t).takeover === false,
        "resolveTakeover(false)：取消后确认条消失",
    );
    run.eq(t.mock.repairPair().reason, "not-conflict", "repairPair：不在冲突态被拒");
    t.stop();
    const c = open("output", "scenario=out-conflict&auto=0", stores());
    run.check(c.mock.repairPair().ok && lastState(c).group !== "A", "repairPair：冲突态重选组");
    c.stop();
}

run.finish();
