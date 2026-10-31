// SPDX-License-Identifier: GPL-3.0-or-later
//
// 共享件夹具：按参数摆出外壳整窗或一张状态目录，并把观测接口挂在 window.__fx 上给冒烟脚本。
// 摆放坐标沿用设计稿（Input 800×580、Output 460×364）。桥是页内替身，调用记在 __fx.calls 里。
// defect 参数只给冒烟的坏样例用：故意造出一种问题，检查脚本必须把它抓出来。

import { DESIGN } from "../../../web/shared/design-box.js";
import { installDicts } from "../../../web/shared/i18n.js";
import { installShellFit } from "../../../web/shared/shell-fit.js";
import { installGlow } from "../../../web/shared/glow.js";
import { makeT, el } from "../../../web/shared/dom.js";
import { createScaleUi } from "../../../web/shared/scale-ui.js";
import { createShell, createHeader, createLangCapsule } from "../../../web/shared/shell-frame.js";
import { createFooter } from "../../../web/shared/shell-footer.js";
import { createScaleConfirm } from "../../../web/shared/shell-scale-confirm.js";
import { createGroupPills, GROUP_LETTERS } from "../../../web/shared/group-pills.js";
import {
    createPairPill,
    createVerifyBadge,
    createChannelBadge,
    statesFor,
    pairLabelKey,
} from "../../../web/shared/pair-status.js";
import { createChainDiagram } from "../../../web/shared/chain-diagram.js";
import { createMsgline, ACTIONS } from "../../../web/shared/msgline.js";
import { createToast } from "../../../web/shared/toast.js";
import { createAbout, COPY_FAIL_KEYS } from "../../../web/shared/about.js";
import common from "../../../web/i18n/common.js";
import { PAGE_DICT, BAND_META } from "./fixture-dict.js";

const q = new URLSearchParams(location.search);
const role = q.get("role") === "output" ? "output" : "input";
const lang = ["zh", "en", "fr"].includes(q.get("lang")) ? q.get("lang") : "zh";
const scene = q.get("scene") || "window";
const overlay = q.get("overlay") || "";
const st = q.get("st") || "";
const defect = q.get("defect") || "";
// 轻提示的计时器。量文字、量对比度、查读屏角色的冒烟都要等字体、入场动画和浏览器往返，机器慢的时候从弹出到量的那一刻
// 可以超过 TOAST_MS，提示自己收了就量不到。所以缺省给 createToast 一对不触发的计时器，提示一直留着；
// 只有 ?toast=live 才用真计时器，专给「4 秒后自己收」那一组。
const toastLive = q.get("toast") === "live";
const HELD_TIMERS = { setTimeout: () => 0, clearTimeout: () => {} };
const box = DESIGN[role];

installDicts(common, PAGE_DICT);
document.documentElement.lang = lang;
const t = makeT(lang);

/** 复制失败的原因短语里本语言最长的一条（页面真正会传给轻提示的就是这种已本地化的短语）及产生它的原因码。 */
const WORST_CODE = Object.keys(COPY_FAIL_KEYS).reduce((a, b) =>
    t(COPY_FAIL_KEYS[b]).length > t(COPY_FAIL_KEYS[a]).length ? b : a,
);
const WORST_REASON = t(COPY_FAIL_KEYS[WORST_CODE]);

/** 每个占位符的样例值：取尽量长的合理值，让最坏串真的上屏。 */
const SAMPLE_VALS = {
    a: "0.10.0",
    b: "0.10.0",
    g: "H",
    y: "C",
    n: "32768",
    x: "12.5",
    d: "21.3",
    m: "10.7",
    f: "14.4",
    s: "130",
    c: "−6.0",
    reason: WORST_REASON,
    peer: "Gtr DI",
    self: "Vox Lead",
};
const valsFor = (key) => {
    const found = {};
    for (const m of makeT("zh")(key).matchAll(/\{(\w+)\}/g)) found[m[1]] = SAMPLE_VALS[m[1]] ?? "1";
    return Object.keys(found).length ? found : undefined;
};
/** 动作名对应的词条键。 */
const ACTION_KEY = Object.fromEntries(Object.entries(ACTIONS).map(([name, def]) => [name, def.key]));

const calls = [];
const log = (name, ...args) => calls.push({ name, args });
const expectedTexts = [];
/** 登记「这一格应当有这段文字上屏」。 */
const expectText = (text, what) => expectedTexts.push({ text, what });

const DIAG =
    "rinne-diag v1\nproduct=Rinne Input version=0.9.0 build=a1b2c3d format=VST3 os=win64\nhost=Cubase sr=48000 maxBlockDeclared=512 maxBlockSeen=512\n" +
    "pair.group=A pair.origin=smart pair.state=connected peer.track=same:3f9a12c4 crossProcess=no\nver=0.9.0/0.9.0 abi=1/1 ch=2/2\n" +
    "events: 12 (last 64 kept)\nui: webview=WebView2 scale=1.00 lang=zh";

let diagFail = st === "copyFail";
// reason= 指定桥返回的失败原因：缺省取会上屏最长短语的原因码；throw 表示桥抛异常；其他值原样当作桥给的原因码
const failReason = q.get("reason") || WORST_CODE;
let diagLog = false;
const cfg = { resetOnPlay: true, tailMode: "auto" };
const bridge = {
    async setUiScale(f) {
        log("setUiScale", f);
        S.scaleApplied = f;
        return { ok: true, scale: f };
    },
    async commitUiScale() {
        log("commitUiScale");
        S.scale = S.scaleApplied || S.scale;
        render();
        return { ok: true, scale: S.scale };
    },
    async copyDiagnostics(commit) {
        log("copyDiagnostics", commit);
        if (commit && diagFail) {
            if (failReason === "throw") throw new Error("TypeError: boom at secret-host/a.js:12");
            return { ok: false, copied: false, reason: failReason };
        }
        return { ok: true, copied: commit, text: DIAG };
    },
    async openExternal(url) {
        log("openExternal", url);
        return { ok: true };
    },
    async showLicense() {
        log("showLicense");
        return { ok: true };
    },
    async setDiagLog(on) {
        log("setDiagLog", on);
        diagLog = on;
        return { ok: true, value: on };
    },
    async setResetOnPlay(on) {
        log("setResetOnPlay", on);
        cfg.resetOnPlay = on;
        return { ok: true, value: on };
    },
    async setTailMode(mode) {
        log("setTailMode", mode);
        cfg.tailMode = mode;
        return { ok: true, value: mode };
    },
};

// ------------------------------------------------------------------ 整窗

const S = {
    group: "A",
    auto: false,
    peers: [],
    pairState: "connected",
    flowing: true,
    scale: 1,
    scaleApplied: 1,
    version: "0.9.0",
    messages: [],
    confirm: null,
    unassigned: false,
    danger: q.get("danger") === "1",
    hintKey: role === "input" ? "in.hint.ready" : "out.hint.connected",
    hintState: "ready",
};

const stage = document.getElementById("stage");
const abs = (node, x, y, extra) => {
    node.classList.add("fx-abs");
    node.style.left = typeof x === "number" ? `${x}px` : "";
    node.style.top = `${y}px`;
    if (typeof x === "string") node.style[x] = `${extra}px`;
    return node;
};

let shell, header, langCap, group, pair, msgline, footer, scaleUi, scaleConfirm, toast, about;

function buildWindow() {
    shell = createShell({ role });
    header = createHeader({ role });
    langCap = createLangCapsule({
        role,
        onPick: (code) => {
            log("setLang", code);
            const u = new URL(location.href);
            u.searchParams.set("lang", code);
            location.href = u.toString();
        },
    });
    group = createGroupPills({
        role,
        onSelect: (g) => {
            log("setGroup", g);
            S.group = g;
            render();
        },
    });
    pair = createPairPill({ gb: role === "input" ? "input.pair.pill" : "output.pair.pill" });
    msgline = createMsgline({
        role,
        onAction: (name, msg) => log("action", name, msg && msg.tag),
        onDismiss: (id) => {
            log("dismiss", id);
            S.messages = S.messages.filter((m) => m.id !== id);
            render();
        },
    });
    scaleUi = createScaleUi({
        role,
        bridge,
        getScale: () => S.scale,
        getScreen: () => {
            const m = /^(\d+)x(\d+)$/.exec(q.get("screen") || "");
            return m
                ? { availWidth: Number(m[1]), availHeight: Number(m[2]) }
                : { availWidth: 3840, availHeight: 2160 };
        },
        onChange: (s) => scaleConfirm && scaleConfirm.update({ ...s, t }),
    });
    footer = createFooter({
        role,
        onScale: (f) => {
            scaleUi.preview(f);
        },
        onAbout: () => about.open({ t, lang, build: BUILD, cfg, diagLog }),
        onHelp: () => log("help"),
    });
    scaleConfirm = createScaleConfirm({
        role,
        host: shell.top,
        inert: [shell.content],
        onKeep: () => {
            scaleUi.keep();
        },
        onCancel: () => {
            scaleUi.revert();
        },
    });
    toast = createToast({ host: shell.layer, role, timers: toastLive ? undefined : HELD_TIMERS });
    about = createAbout({
        role,
        host: shell.top,
        inert: [shell.content],
        bridge,
        // tail=1：让 Output 也带上 tail 行，测备注是否跟着 tail 行走
        features: { resetOnPlay: role === "input", tail: role === "input" || q.get("tail") === "1" },
        onViewRules: () => log("viewRules"),
        onCopied: () => toast.show({ key: "toast.diagCopied", t }),
        onCopyFail: (reason) => toast.show({ key: "toast.copyFail", vals: { reason }, t }),
    });

    const c = shell.content;
    if (role === "input") {
        c.append(
            abs(header.el, 20, 17),
            abs(langCap.el, "right", 25, 20),
            abs(
                el(
                    "div",
                    {
                        class: "fx-row",
                        style: {
                            display: "flex",
                            justifyContent: "space-between",
                            alignItems: "center",
                            width: "760px",
                            height: "28px",
                        },
                    },
                    group.el,
                    pair.el,
                ),
                20,
                64,
            ),
            abs(msgline.el, 328, 96),
            abs(footer.el, 20, 542),
        );
        footer.el.style.width = "760px";
    } else {
        c.append(
            abs(header.el, 18, 14),
            abs(langCap.el, "right", 22, 18),
            abs(group.el, 18, 62),
            abs(pair.el, 18, 100),
            abs(msgline.el, 18, 262),
            abs(footer.el, 18, 328),
        );
        footer.el.style.width = "424px";
    }
    stage.append(shell.el);
}

const BUILD = { version: "0.9.0", sha: "a1b2c3d", fmt: "VST3", arch: "Windows x64" };

function msgFor(meta, extra) {
    return {
        id: meta.id,
        key: meta.key,
        vals: valsFor(meta.key),
        tone: meta.tone,
        action: meta.action ? { name: meta.action, vals: valsFor(ACTION_KEY[meta.action]) } : undefined,
        ...extra,
    };
}

function footerArgs(over = {}) {
    return {
        hint: { state: S.hintState, text: t(S.hintKey) },
        scaleOptions: scaleUi.options(),
        scale: S.scale,
        locked: q.get("locked") === "1",
        version: S.version,
        t,
        ...over,
    };
}

function render() {
    header.update({ lang, t });
    langCap.update({ lang, t });
    group.update({
        group: S.group,
        auto: S.auto,
        peers: S.peers,
        unassigned: S.unassigned,
        autoGroup: S.group,
        t,
    });
    pair.update({ state: S.pairState, role, flowing: S.flowing, t });
    msgline.update({
        messages: S.messages,
        confirm: S.confirm,
        idleKey: role === "output" ? "out.hint.idle" : undefined,
        lang,
        t,
    });
    footer.update(footerArgs());
    shell.setState({ danger: S.danger, overlay: Boolean(overlay) });
}

function setupWindow() {
    buildWindow();
    // 子状态：st 决定窗里摆什么
    const metaOf = (id) => BAND_META.find((m) => m.role === role && m.id === id);
    if (scene === "window" && !overlay) {
        const pick = st.split(",").filter(Boolean);
        S.messages = pick
            .map((id) => metaOf(id))
            .filter(Boolean)
            .map((m) => msgFor(m));
    }
    if (q.get("pair")) S.pairState = q.get("pair");
    if (q.get("auto")) S.auto = q.get("auto") === "fallback" ? "fallback" : true;
    if (q.get("peers")) S.peers = q.get("peers").split(",");
    if (q.get("unassigned") === "1") {
        S.unassigned = true;
        S.group = null;
    }
    render();
    expectText(t(role === "input" ? "in.brand.title" : "out.brand.title"), "text");
    expectText(t(role === "input" ? "in.brand.eyebrow" : "out.brand.eyebrow"), "text");
    expectText(t("common.footer.version", { v: S.version }), "text");
    expectText(t(S.hintKey), "text");
}

async function setupOverlay() {
    if (overlay === "about") {
        await about.open({ t, lang, build: BUILD, cfg, diagLog });
        if (st === "copied" || st === "copyFail") {
            const btn = about.el.querySelector('[data-part="about.copy"]');
            btn.click();
            await new Promise((r) => setTimeout(r, 120));
            if (st === "copyFail" && failReason === WORST_CODE) {
                expectText(t("about.copyFail", { reason: WORST_REASON }), "text");
            }
        }
        expectText(t(role === "input" ? "about.title.in" : "about.title.out"), "text");
        expectText(t("about.license"), "text");
        expectText(t("about.fonts"), "text");
        expectText(t("about.tm"), "text");
        expectText(t("about.noGithub"), "text");
        expectText(t("about.diagNote"), "text");
    } else if (overlay === "scale") {
        scaleConfirm.update({ confirming: true, left: Number(st) || 7, t });
        expectText(t("common.scale.confirmBody", { s: Number(st) || 7 }), "text");
    } else if (overlay === "toast") {
        const key = `toast.${st || "group"}`;
        window.__fxToastAt = performance.now();
        toast.show({
            key,
            vals: key === "toast.copyFail" ? { reason: WORST_REASON } : key === "toast.group" ? { g: "H" } : undefined,
            t,
        });
        expectText(
            t(
                key,
                key === "toast.copyFail" ? { reason: WORST_REASON } : key === "toast.group" ? { g: "H" } : undefined,
            ),
            "text",
        );
    }
}

// ------------------------------------------------------------------ 目录场景

function plate(node, caption, dark) {
    return el(
        "div",
        { class: "fx-cell" },
        el("div", { class: `fx-plate${dark ? " fx-plate--dark" : ""}` }, node),
        el("div", { class: "fx-cap", text: caption }),
    );
}

function catalog(...cells) {
    stage.append(el("div", { class: "fx-catalog" }, ...cells));
}

function setupPair() {
    const cells = [];
    for (const state of statesFor(role)) {
        for (const flowing of state === "connected" || state === "connectedUnverified" ? [true, false] : [true]) {
            const p = createPairPill({ gb: role === "input" ? "input.pair.pill" : "output.pair.pill" });
            p.update({ state, role, flowing, t });
            cells.push(plate(p.el, `${state}${flowing ? "" : " (still)"}`));
            expectText(t(pairLabelKey(state, role)), "text");
        }
    }
    catalog(...cells);
}

function setupBadges() {
    const cells = [];
    for (const kind of ["match", "diff", "unknown"]) {
        const v = createVerifyBadge();
        v.update({ kind, t });
        cells.push(plate(v.el, `verify ${kind}`));
        expectText(t(`common.verify.${kind}`), "text");
    }
    for (const [i, o] of [
        ["st", null],
        ["mono", null],
        ["st", "mono"],
        ["mono", "st"],
    ]) {
        const c = createChannelBadge();
        c.update({ input: i, output: o, t });
        cells.push(plate(c.el, `ch ${i}${o ? ">" + o : ""}`));
    }
    catalog(...cells);
}

function setupGroup() {
    const variants = [
        ["manual A", { group: "A", auto: false, peers: [] }],
        ["auto + peers", { group: "B", auto: true, peers: GROUP_LETTERS.slice() }],
        ["fallback", { group: "H", auto: "fallback", peers: ["C"] }],
        ["unassigned", { group: null, unassigned: true, peers: [] }],
        ["disabled", { group: "C", auto: false, peers: ["D"], disabled: true }],
    ];
    const cells = variants.map(([cap, p]) => {
        const g = createGroupPills({ role });
        g.update({ ...p, autoGroup: p.group || "A", t });
        return plate(g.el, cap);
    });
    expectText(t("common.group.label"), "text");
    expectText(t("common.group.auto"), "text");
    catalog(...cells);
}

function setupChain() {
    const cells = [];
    for (const [cap, o, p] of [
        ["out", {}, { highlight: "out" }],
        ["in", {}, { highlight: "in" }],
        ["static", {}, { highlight: null, still: true }],
        ["compact", { compact: true }, { highlight: "out" }],
        ["dark", { dark: true }, { highlight: "out" }],
    ]) {
        const c = createChainDiagram(o);
        c.update({ ...p, t });
        cells.push(plate(c.el, `chain ${cap}`, Boolean(o.dark)));
    }
    for (const k of ["common.chain.in", "common.chain.out", "common.chain.fader"]) expectText(t(k), "text");
    catalog(...cells);
}

function setupBands() {
    const cells = [];
    const mine = BAND_META.filter((m) => m.role === role);
    for (const meta of mine) {
        const line = createMsgline({ role });
        line.update({ messages: [msgFor(meta, meta.id === "exportMismatch" ? { dismiss: true } : {})], lang, t });
        cells.push(plate(line.el, `${meta.id} · ${meta.tone}`));
        expectText(t(meta.key, valsFor(meta.key)), "text");
        if (meta.action) expectText(t(ACTION_KEY[meta.action], valsFor(ACTION_KEY[meta.action])), "text");
    }
    // 冷却态：恢复回送
    const cutMeta = mine.find((m) => m.id === "cutPanic");
    if (cutMeta) {
        const line = createMsgline({ role });
        line.update({ messages: [msgFor(cutMeta, { action: { name: "restore", cooldown: true } })], lang, t });
        cells.push(plate(line.el, "cutPanic · cooldown"));
    }
    // 多条：chip
    if (mine.length > 2) {
        const line = createMsgline({ role });
        line.update({ messages: mine.slice(0, 3).map((m) => msgFor(m)), lang, t });
        cells.push(plate(line.el, "multi · +2"));
    }
    // 确认条
    const confirmLine = createMsgline({ role });
    confirmLine.update({
        confirm: { key: `band.${role === "input" ? "in" : "out"}.takeover`, vals: { g: "H" } },
        lang,
        t,
    });
    cells.push(plate(confirmLine.el, "confirm"));
    expectText(t(`band.${role === "input" ? "in" : "out"}.takeover`, { g: "H" }), "text");
    // 空闲
    const idle = createMsgline({ role });
    idle.update({ messages: [], idleKey: role === "output" ? "out.hint.idle" : undefined, lang, t });
    cells.push(plate(idle.el, "idle"));
    catalog(...cells);
}

// ------------------------------------------------------------------ 坏样例

function applyDefect() {
    if (!defect) return;
    const style = document.createElement("style");
    const rules = {
        // 让配对 pill 的文字撑出 240 的上限
        overflow: ".rn-pair__text{letter-spacing:.9em !important}",
        // 消息文字被硬截断
        clip: ".rn-banner__text{overflow:hidden !important;max-width:40px !important;white-space:nowrap !important}",
        // 琥珀条的字色换成与底几乎一样的色
        lowcontrast: ".rn-banner--amber .rn-banner__text,.rn-banner--gray .rn-banner__text{color:#c9b8a0 !important}",
        // 键盘焦点环被去掉
        noring: ".rn-shell :focus-visible{outline:none !important}",
    };
    if (rules[defect]) {
        style.textContent = rules[defect];
        document.head.append(style);
    }
    if (defect === "noname") {
        document.querySelectorAll(".rn-footer__help, .rn-footer__scale, .rn-lang").forEach((n) => {
            n.removeAttribute("aria-label");
            n.removeAttribute("title");
        });
    }
    if (defect === "longmsg") {
        // Input 一行槽里塞进远超一行的文字：不折行的话它必须越出槽
        const t = document.querySelector('[data-gb="input.band"] .rn-banner__text');
        if (t) t.textContent = `${t.textContent} ${t.textContent} ${t.textContent}`;
    }
    if (defect === "rawkey") stage.append(el("div", { class: "fx-bad", text: "band.in.neverDefined" }));
    if (defect === "placeholder") stage.append(el("div", { class: "fx-bad", text: "组 {g} 已被占用" }));
}

// ------------------------------------------------------------------ 启动

async function main() {
    installGlow(document);
    const dsh = scene === "window" || overlay;
    if (dsh) {
        setupWindow();
        await setupOverlay();
        installShellFit({ el: shell.el, box });
    } else {
        const setups = {
            pair: setupPair,
            badges: setupBadges,
            group: setupGroup,
            chain: setupChain,
            bands: setupBands,
        };
        (setups[scene] || setupPair)();
    }
    applyDefect();
    await document.fonts.ready;
    window.__fx = {
        ready: true,
        role,
        lang,
        scene,
        calls,
        expectedTexts,
        state: S,
        // 页面级测试要用的内部入口
        api: {
            render: () => render(),
            scaleUi: () => scaleUi,
            msgline: () => msgline,
            about: () => about,
            toast: () => toast,
            footer: () => footer,
            // 用当前状态加覆盖项直接调一次页脚的 update（测下拉选项的重建判据用）
            footerUpdate: (over) => footer.update(footerArgs(over)),
            makeT: (code) => makeT(code),
            shell: () => shell,
            group: () => group,
            setMessages: (ids, extra) => {
                const metaOf = (id) => BAND_META.find((m) => m.role === role && m.id === id);
                S.messages = ids
                    .map((id) => metaOf(id))
                    .filter(Boolean)
                    .map((m) => msgFor(m, extra));
                render();
            },
            setPair: (s, flowing = true) => {
                S.pairState = s;
                S.flowing = flowing;
                render();
            },
            setConfirm: (c) => {
                S.confirm = c;
                render();
            },
            setDanger: (d) => {
                S.danger = d;
                render();
            },
        },
    };
}

main().catch((e) => {
    window.__fxError = String((e && e.stack) || e);
    console.error(window.__fxError);
});
