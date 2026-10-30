// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rinne Input 主面板的入口：接桥、建外壳与五张卡、把桥上的状态变成界面。
//
// 数据流：
//   rinne.state / rinne.pair / rinne.meters 是快照，页面只保留各自最近一帧，丢弃序号不大于已收到最大值的帧；
//   rinne.params 是参数回声，交给 param-echo：拖动期间丢、自己写入的旧回声丢、宿主自动化用 rAF 指数逼近；
//   消息位的内容由 model.js 的 deriveMessages 从这些快照与参数当前值现算，不另存。
// 渲染：快照到达只做「读 -> 比较 -> 写有变化的部分」，恒值段不产生 DOM 变更。电平帧走 meter.js 的弹道，
// 与其余渲染分开。
// 预览专用：浏览器预览里从 mock 读场景描述，用来直接摆出限制器确认、关于面板、缩放确认这几种覆盖层；
// 真宿主没有 previewScene，这些分支不会走到。

import { createBridge } from "../shared/bridge.js";
import { installDicts } from "../shared/i18n.js";
import { makeT } from "../shared/dom.js";
import { DESIGN } from "../shared/design-box.js";
import { installShellFit, zoomLocked, enforceZoomLock } from "../shared/shell-fit.js";
import { installGlow } from "../shared/glow.js";
import { createScaleUi } from "../shared/scale-ui.js";
import { createShell, createHeader, createLangCapsule } from "../shared/shell-frame.js";
import { createFooter } from "../shared/shell-footer.js";
import { createScaleConfirm } from "../shared/shell-scale-confirm.js";
import { createGroupPills } from "../shared/group-pills.js";
import { createPairPill } from "../shared/pair-status.js";
import { createMsgline, orderMessages } from "../shared/msgline.js";
import { createToast } from "../shared/toast.js";
import { createAbout } from "../shared/about.js";
import { docLink, docRef } from "../shared/links.js";
import common from "../i18n/common.js";
import input from "../i18n/input.js";
import { el } from "../shared/dom.js";
import {
    SMART_DEFAULT_MS,
    deriveMessages,
    delayUnitOf,
    exportHint,
    fbUnitOf,
    footerHint,
    groupsWithPeer,
    isDanger,
    isFlowing,
    mixFill,
} from "./model.js";
import { createParamEcho } from "./components/param-echo.js";
import { createNumEdit } from "./components/num-edit.js";
import { createMeterRenderer } from "./components/meter.js";
import { createFeedbackCard } from "./components/feedback-card.js";
import { createToneCard } from "./components/tone-card.js";
import { createDelayCard } from "./components/delay-card.js";
import { createSafetyCard } from "./components/safety-card.js";
import { createMeterCard } from "./components/meter-card.js";
import { createPeerRow } from "./components/peer-row.js";
import { createPanic, createLimiterConfirm } from "./components/safety-layer.js";
import { setHidden, setVar } from "./components/util.js";
import { createOfflineBridge, defaultFrames } from "./offline.js";

const ROLE = "input";
/** 桥一直不给快照时，等这么久就按默认快照揭幕。 */
const FALLBACK_MS = 400;
/** 参数的当前值影响消息位的那几个（回声显示出来时要重算消息）。 */
const MESSAGE_PARAMS = new Set(["feedback", "mix", "loop_on", "limiter", "ceiling"]);
/** 导出不一致的四条消息 id：它们共用一条「已关闭」记录。 */
const EXPORT_IDS = new Set(["exportViolation", "exportOverrun", "exportPeer", "exportResync"]);

/**
 * 桥有三种来路：真宿主（window.__JUCE__ 带后端）、预览（壳页注入 window.__RINNE_MOCK__）、都没有（纯浏览器直接打开，
 * 首帧冒烟与离线扫描就是这样）。只有第三种降级成无桥模式；真宿主里 createBridge 失败是真故障，让异常冒出去，
 * 由 index.html 的 boot 守卫上报原生侧，免得把它悄悄吞成一个空壳。
 */
function openBridge() {
    const hasMock = Boolean(globalThis.__RINNE_MOCK__);
    const hasHost = Boolean(globalThis.__JUCE__ && globalThis.__JUCE__.backend);
    if (!hasMock && !hasHost) {
        console.warn("Rinne Input：没有检测到桥（既不是插件宿主也不是预览），按无桥模式显示默认状态。");
        return { bridge: createOfflineBridge(), offline: true };
    }
    return { bridge: createBridge({ role: ROLE }), offline: false };
}

function boot() {
    installDicts(common, input);
    const { bridge, offline } = openBridge();
    installGlow(document);

    // ---- 页面状态
    const S = {
        state: null,
        pair: null,
        meters: null,
        seq: { state: -Infinity, pair: -Infinity, meters: -Infinity, params: -Infinity },
        dismissedLocal: new Set(),
        exportRec: null,
        exportSeeded: false,
        touchedFeedback: false,
        smartActive: false,
        smartGroup: null,
        smartTimer: 0,
        guardOverride: undefined,
        dragging: new Set(),
        msgSig: "",
        metersSig: "",
        frameSig: { state: "", pair: "" },
        renderQueued: false,
        scaleState: { confirming: false, left: 0 },
        sceneApplied: false,
    };
    const params = new Map();
    const mem = { fbUnit: "pct", delayUnit: "ms" };
    /**
     * 单位（反馈 %/dB、延迟 ms/样本）写进页面内存的唯一落点：用户点分段控件和首帧采纳宿主快照都走这里。
     * 目前选择只留在页面内存；原生桥补上 setFbUnit / setDelayUnit 之后，在这里把用户的选择转发给宿主即可。
     * @param {"fb"|"delay"} kind
     * @param {string} value 反馈 "pct" | "db"；延迟 "ms" | "samples"
     */
    const setUnit = (kind, value) => {
        if (kind === "fb") mem.fbUnit = value === "db" ? "db" : "pct";
        else if (kind === "delay") mem.delayUnit = value === "samples" ? "samples" : "ms";
    };
    const scene = readPreviewScene();

    // ---- 上下文：各张卡共用
    const echo = createParamEcho({ bridge });
    const ctx = {
        bridge,
        timers: globalThis,
        mem,
        setUnit,
        view: null,
        param(id, opt = {}) {
            const userDisplay = opt.onDisplay;
            const p = echo.param(id, {
                ...opt,
                onDisplay(v) {
                    if (userDisplay) userDisplay(v);
                    if (MESSAGE_PARAMS.has(id)) scheduleMessages();
                },
            });
            params.set(id, p);
            return p;
        },
        values() {
            const out = {};
            for (const [id, p] of params) out[id] = p.value();
            return out;
        },
        edit: (cfg) => numEdit.open(cfg),
        toast: (key, vals, tone) => {
            if (ctx.view) toastUi.show({ key, vals, tone, t: ctx.view.t });
        },
        setDragging(id, on) {
            if (on) S.dragging.add(id);
            else S.dragging.delete(id);
            renderFooter();
        },
        touchFeedback() {
            if (S.touchedFeedback) return;
            S.touchedFeedback = true;
            scheduleMessages();
        },
        meterState: (id) => meterRenderer.stateOf(id),
        async setGuardOn(next) {
            S.guardOverride = next;
            scheduleRender();
            const r = await bridge.setGuardOn(next).catch(() => null);
            S.guardOverride = r && r.ok ? r.value : undefined;
            scheduleRender();
        },
    };

    // ---- 外壳与共享件
    const box = DESIGN[ROLE];
    const shell = createShell({ role: ROLE });
    const header = createHeader({ role: ROLE });
    const langCap = createLangCapsule({
        role: ROLE,
        onPick: (code) => {
            bridge.setLang(code);
        },
    });
    const group = createGroupPills({
        role: ROLE,
        onSelect: async (g) => {
            const r = await bridge.setGroup(g).catch(() => null);
            if (r && r.ok) ctx.toast("toast.group", { g });
        },
    });
    const pairPill = createPairPill({ gb: "input.pair.pill" });
    const peerRow = createPeerRow();
    const msgline = createMsgline({ role: ROLE, onAction: onMessageAction, onDismiss: onMessageDismiss });
    const scaleUi = createScaleUi({
        role: ROLE,
        bridge,
        getScale: () => (S.state && S.state.uiScale) || 1,
        getScreen: () => ({ availWidth: globalThis.screen.availWidth, availHeight: globalThis.screen.availHeight }),
        onChange: (s) => {
            S.scaleState = s;
            scaleConfirm.update({ ...s, t: ctx.view ? ctx.view.t : (k) => k });
            renderShellState();
        },
    });
    const footer = createFooter({
        role: ROLE,
        onScale: (f) => scaleUi.preview(f),
        onAbout: openAbout,
        onHelp: openGuide,
    });
    const scaleConfirm = createScaleConfirm({
        role: ROLE,
        host: shell.top,
        inert: [shell.content],
        onKeep: () => scaleUi.keep(),
        onCancel: () => scaleUi.revert(),
    });
    const toastUi = createToast({ host: shell.layer, role: ROLE });
    const about = createAbout({
        role: ROLE,
        host: shell.top,
        inert: [shell.content],
        bridge,
        features: { resetOnPlay: true, tail: true },
        onViewRules: () => {},
        onCopied: () => ctx.toast("toast.diagCopied"),
        onCopyFail: (reason) => ctx.toast("toast.copyFail", { reason }),
        onClose: () => renderShellState(),
    });
    const numEdit = createNumEdit({ t: (k, v) => (ctx.view ? ctx.view.t(k, v) : k) });

    // ---- 五张卡
    const cards = {
        feedback: createFeedbackCard(ctx),
        tone: createToneCard(ctx),
        delay: createDelayCard(ctx),
        safety: createSafetyCard(ctx, {
            onLimiterOff: (from) => {
                if (ctx.view) limiterConfirm.open({ t: ctx.view.t, from });
            },
        }),
        meters: createMeterCard(ctx, { onClearPeak: (id) => meterRenderer.clearPeak(id) }),
    };
    const limiterConfirm = createLimiterConfirm({
        host: shell.layer,
        onCancel: () => {},
        onConfirm: () => params.get("limiter").commit(false),
    });
    const panic = createPanic({
        host: shell.top,
        onPress: () => {
            closeOverlays();
            bridge.panic();
        },
    });

    // ---- 电平弹道
    const mixLv = cards.feedback.levels;
    const meterRenderer = createMeterRenderer({
        ids: ["in", "ret", "out", "dry", "wet"],
        write(id, st) {
            if (id === "dry" || id === "wet") {
                const liquid = id === "dry" ? mixLv.dry : mixLv.wet;
                const peak = id === "dry" ? mixLv.dryPeak : mixLv.wetPeak;
                const pk = mixFill(st.peakDb);
                setVar(liquid, "--lv", `${mixFill(st.db).toFixed(1)}%`);
                setVar(peak, "--pk", `${pk.toFixed(1)}%`);
                setHidden(peak, pk <= 0);
            } else {
                cards.meters.write(id, st);
            }
        },
    });

    // ---- 布局（设计盒坐标）
    const at = (node, x, y, w, h) => {
        node.classList.add("in-abs");
        node.style.left = typeof x === "number" ? `${x}px` : "";
        node.style.top = `${y}px`;
        if (w) node.style.width = `${w}px`;
        if (h) node.style.height = `${h}px`;
        return node;
    };
    const pairRow = el("div", { class: "in-pairrow" }, group.el, pairPill.el);
    const presetSlot = el("div", { class: "in-preset-slot", "data-slot": "preset-trigger" });
    shell.content.append(
        at(header.el, 20, 17),
        at(presetSlot, 314, 21, 200, 30),
        at(langCap.el, "", 25),
        at(pairRow, 20, 64, 760, 28),
        at(peerRow.el, 20, 96, 300, 30),
        at(msgline.el, 328, 96),
        at(cards.feedback.el, 20, 134, 248, 400),
        at(cards.tone.el, 276, 134, 248, 149),
        at(cards.delay.el, 276, 291, 248, 243),
        at(cards.safety.el, 532, 134, 248, 290),
        at(cards.meters.el, 532, 432, 248, 102),
        at(footer.el, 20, 542, 760),
    );
    langCap.el.style.left = "";
    langCap.el.style.right = "20px";
    const host = document.getElementById("shell");
    host.replaceWith(shell.el);
    installShellFit({ el: shell.el, box });

    // ---- 渲染
    function buildView() {
        const state = S.state;
        const lang = ["zh", "en", "fr"].includes(state.lang) ? state.lang : "zh";
        const cfg = state.cfg || {};
        ctx.view = {
            lang,
            t: makeT(lang),
            state,
            pair: S.pair,
            meters: S.meters || {},
            cfg,
            guardOn: S.guardOverride !== undefined ? S.guardOverride : cfg.guardOn !== false,
            preview: scene,
        };
    }

    function scheduleRender() {
        if (S.renderQueued) return;
        S.renderQueued = true;
        queueMicrotask(() => {
            S.renderQueued = false;
            renderAll();
        });
    }

    function renderAll() {
        if (!S.state || !S.pair) return;
        buildView();
        const { lang, t, state, pair } = ctx.view;
        if (document.documentElement.lang !== lang) document.documentElement.lang = lang;
        const safety = state.safety || {};
        const values = ctx.values();
        header.update({ lang, t });
        langCap.update({ lang, t });
        group.update({
            group: state.group,
            auto: state.groupOrigin === "auto",
            peers: groupsWithPeer(pair.groupMask),
            unassigned: pair.state === "unassigned",
            autoGroup: state.group,
            t,
        });
        pairPill.update({
            state: pair.state,
            role: ROLE,
            flowing: isFlowing({
                pairState: pair.state,
                cut: safety.cut,
                loopOn: values.loop_on,
                silentCause: state.silentCause,
            }),
            t,
        });
        for (const card of Object.values(cards)) card.render();
        panic.update({ cut: Boolean(safety.cut), overScrim: about.isOpen() || S.scaleState.confirming, t });
        limiterConfirm.retext(t);
        renderMessages();
        renderFooter();
        renderShellState();
        scaleConfirm.update({ ...S.scaleState, t });
    }

    function renderShellState() {
        if (!ctx.view) return;
        const { state, pair } = ctx.view;
        const safety = state.safety || {};
        shell.setState({
            danger: isDanger({ cut: safety.cut, pairState: pair.state, l2: safety.l2 }),
            hostBypass: state.silentCause === "hostBypass",
            overlay: about.isOpen() || S.scaleState.confirming,
        });
    }

    function renderFooter() {
        if (!ctx.view) return;
        const { t, state, pair } = ctx.view;
        const safety = state.safety || {};
        const values = ctx.values();
        const h = footerHint({
            cut: safety.cut,
            pairState: pair.state,
            feedback: values.feedback,
            dragging: S.dragging.size > 0,
        });
        footer.update({
            hint: { state: h.state, text: t(h.key) },
            scaleOptions: scaleUi.options(),
            scale: state.uiScale || 1,
            locked: zoomLocked(),
            version: state.version,
            t,
        });
    }

    let messageQueued = false;
    function scheduleMessages() {
        if (messageQueued) return;
        messageQueued = true;
        queueMicrotask(() => {
            messageQueued = false;
            renderMessages();
            renderFooter();
        });
    }

    function renderMessages() {
        if (!ctx.view) return;
        const { t, lang, state, pair } = ctx.view;
        const merged = { ...state, dismissedHints: [...(state.dismissedHints || []), ...S.dismissedLocal] };
        const { messages, confirm } = deriveMessages({
            state: merged,
            pair,
            values: ctx.values(),
            meters: S.meters,
            exportRec: S.exportRec,
            touchedFeedback: S.touchedFeedback,
            smartActive: S.smartActive,
        });
        const sig = JSON.stringify([messages, confirm, lang]);
        const { list, shown } = orderMessages(messages);
        const top = shown >= 0 ? list[shown] : null;
        peerRow.update({ pair, showChain: Boolean(top && top.chain), lang, t });
        if (sig === S.msgSig) return;
        S.msgSig = sig;
        msgline.update({
            messages,
            confirm: confirm && {
                ...confirm,
                onConfirm: () => bridge.resolveTakeover(true),
                onCancel: () => bridge.resolveTakeover(false),
            },
            lang,
            t,
        });
    }

    // ---- 消息位的动作
    async function onMessageAction(name, msg) {
        if (!ctx.view) return;
        if (name === "restore") {
            const r = await bridge.resumeLoop().catch(() => null);
            if (r && r.ok) ctx.toast("toast.restored", undefined, "green");
        } else if (name === "moveFree") {
            const r = await bridge.moveToFreeGroup().catch(() => null);
            if (r && r.ok) ctx.toast("toast.moved", { g: r.group }, "green");
        } else if (name === "moveTo") {
            const g = msg && msg.action && msg.action.vals && msg.action.vals.y;
            if (!g) return;
            const r = await bridge.setGroup(g).catch(() => null);
            if (r && r.ok) ctx.toast("toast.group", { g });
        } else if (name === "limOn") {
            params.get("limiter").commit(true);
        } else if (name === "guide") {
            openGuide();
        }
    }

    function onMessageDismiss(id) {
        if (EXPORT_IDS.has(id)) {
            const cur = (S.state && S.state.exportCauses) || { bits: 0, count: 0 };
            S.exportRec = { bits: cur.bits >>> 0, count: cur.count >>> 0 };
            bridge.dismissHint("exportMismatch");
        } else {
            S.dismissedLocal.add(id);
            bridge.dismissHint(id);
        }
        scheduleMessages();
    }

    function openGuide() {
        const version = ctx.view && ctx.view.state.version;
        bridge.openExternal(docLink(docRef(version), "docs/USER_GUIDE.zh-CN.md"));
    }

    function openAbout() {
        if (!ctx.view) return;
        const { t, lang, state, cfg } = ctx.view;
        about.open({ t, lang, build: { version: state.version }, cfg, diagLog: state.diagLog === true });
        renderShellState();
        panic.update({ cut: Boolean((state.safety || {}).cut), overScrim: true, t });
    }

    function closeOverlays() {
        if (about.isOpen()) about.close();
        if (S.scaleState.confirming) scaleUi.revert();
        limiterConfirm.close();
        numEdit.close();
        renderShellState();
    }

    // ---- 事件
    /** 快照的内容签名：序号每帧都变，不算内容。 */
    const contentSig = (frame) => JSON.stringify(frame, (k, v) => (k === "seq" ? undefined : v));
    /** 快照内容与上一帧相同就不必重画：回声与周期快照每秒几十帧，重写同样的值会让浏览器重排、读屏重复播报。 */
    const changedSince = (kind, frame) => {
        const sig = contentSig(frame);
        const same = sig === S.frameSig[kind];
        S.frameSig[kind] = sig;
        return !same;
    };
    const fresh = (kind, frame) => {
        if (!frame || typeof frame !== "object") return false;
        if (Number.isFinite(frame.seq)) {
            if (frame.seq <= S.seq[kind]) return false;
            S.seq[kind] = frame.seq;
        }
        return true;
    };

    const onState = (f) => {
        if (!fresh("state", f)) return;
        const first = S.state === null;
        // 守卫开关的乐观覆盖要在下一帧回声里收回：即便内容没变也得重画一次，把界面拉回宿主的值
        const changed = changedSince("state", f) || S.guardOverride !== undefined;
        S.state = f;
        S.guardOverride = undefined;
        if (first) {
            setUnit("fb", fbUnitOf(f.cfg));
            setUnit("delay", delayUnitOf(f.cfg));
            // 本实例此前已经关掉过导出提示：以当前的锁存为基线，出现新原因或更多事件才重新提示
            if (Array.isArray(f.dismissedHints) && f.dismissedHints.includes("exportMismatch")) {
                const cur = f.exportCauses || { bits: 0, count: 0 };
                S.exportRec = { bits: cur.bits >>> 0, count: cur.count >>> 0 };
            }
        }
        if (S.exportRec && !exportHint(f.exportCauses, S.exportRec).rec) S.exportRec = null;
        trackSmartDefault(f);
        enforceZoomLock(f.uiScale, (s) => bridge.setUiScale(s));
        if (changed) scheduleRender();
        maybeApplyScene();
    };
    const onPair = (f) => {
        if (!fresh("pair", f)) return;
        const changed = changedSince("pair", f);
        S.pair = f;
        if (changed) scheduleRender();
        maybeApplyScene();
    };
    const onParams = (f) => {
        if (!fresh("params", f) || !f.values) return;
        for (const [id, p] of params)
            if (Object.prototype.hasOwnProperty.call(f.values, id)) p.receive(f.values[id], f.seq);
    };
    const onMeters = (f) => {
        if (!fresh("meters", f)) return;
        S.meters = f;
        meterRenderer.push({ in: f.in, ret: f.ret, out: f.out, dry: f.dryPart, wet: f.retPart });
        const sig = `${f.gr}|${JSON.stringify(f.lamps)}|${f.highCutLimited}`;
        if (sig !== S.metersSig) {
            // 视图还没建起来时也要记下签名：首次渲染的 buildView 会取最新的 S.meters，不必等下一帧再补画一次
            S.metersSig = sig;
            if (ctx.view) {
                ctx.view.meters = f;
                cards.safety.render();
                scheduleMessages();
            }
        }
    };
    bridge.on("rinne.state", onState);
    bridge.on("rinne.pair", onPair);
    bridge.on("rinne.params", onParams);
    bridge.on("rinne.meters", onMeters);
    bridge.on("rinne.presets", () => {});
    bridge.on("rinne.error", () => {});

    /** 智能默认刚认领了组：10 秒内给一条可关的提示。 */
    function trackSmartDefault(state) {
        const auto = state.groupOrigin === "auto" && Boolean(state.group);
        if (!auto) {
            S.smartGroup = null;
            S.smartActive = false;
            if (S.smartTimer) clearTimeout(S.smartTimer);
            S.smartTimer = 0;
            return;
        }
        if (S.smartGroup === state.group) return;
        S.smartGroup = state.group;
        S.smartActive = true;
        if (S.smartTimer) clearTimeout(S.smartTimer);
        S.smartTimer = setTimeout(() => {
            S.smartTimer = 0;
            S.smartActive = false;
            scheduleMessages();
        }, SMART_DEFAULT_MS);
    }

    /** 预览专用：按场景描述摆出覆盖层，只在首次拿到 state 与 pair 之后做一次。 */
    function maybeApplyScene() {
        if (S.sceneApplied || !scene || !S.state || !S.pair) return;
        S.sceneApplied = true;
        queueMicrotask(() => {
            renderAll();
            if (scene.limiter === "confirm") limiterConfirm.open({ t: ctx.view.t, from: cards.safety.limiterToggle });
            if (scene.overlay === "about") openAbout();
            if (scene.overlay === "scaleConfirm") scaleUi.preview(S.state.uiScale || 1);
        });
    }

    // ---- 启动
    // 桥一直不给快照（没有桥，或桥没接通）：按默认快照揭幕，真快照一到按序号盖过它。哪一路已经收到就不动哪一路。
    const applyDefaults = () => {
        const d = defaultFrames();
        if (!S.state) onState(d.state);
        if (!S.pair) onPair(d.pair);
        if (S.seq.params === -Infinity) onParams(d.params);
        if (!S.meters) onMeters(d.meters);
    };
    bridge.requestInitialState();
    if (offline) queueMicrotask(applyDefaults);
    else setTimeout(applyDefaults, FALLBACK_MS);
    return { bridge, ctx };
}

/** 预览里的场景描述（真宿主没有）。 */
function readPreviewScene() {
    const mock = globalThis.__RINNE_MOCK__;
    const s = mock && mock.previewScene && mock.previewScene.state;
    return s && typeof s === "object" ? { ...s } : null;
}

boot();
// 主模块初始化完成：index.html 的首帧信号与 boot 守卫都到位，才算页面起来了（见 index.html 头部）
if (typeof window.__rinneMarkReady === "function") window.__rinneMarkReady();
