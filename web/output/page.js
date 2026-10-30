// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rinne Output 页面：把桥的状态翻译成共享件的入参并接上交互。
//
// 分工：view-model.js 把 rinne.state / rinne.pair 整理成视图模型（纯函数）；pair-card.js、place-send.js 是 Output 自己的三个小件；
// 其余（外壳、页眉、组选择、消息位、页脚、缩放、关于、轻提示）全部是 web/shared 的共享件，这里只创建一份并按视图模型喂数据。
// 每个页面只建一个消息位、一个轻提示宿主（外壳的 layer 层）。
//
// 交互一览：
//   选组          setGroup(字母)；成功后等下一帧确认已切过去、且没有等接管，才弹「已切到组 X」，免得接管确认条和成功提示同时出现；
//   重新配对       消息位的动作按钮 → repairPair()（冲突态的一键修复），成功后同样弹「已切到组 X」；
//   接管确认条     resolveTakeover(true / false)，展开期间其余组胶囊禁用；
//   查看说明       消息位的动作按钮 → openExternal(用户指南)，链接钉当前版本的 tag；
//   语言           setLang；成功后立即换语言，以后续帧里的 lang 为准；
//   缩放、关于     共享的状态机与面板。
// 页面不依赖具体桥：只用 createBridge 返回的对象，所以预览（mock）、真宿主与「没有桥」的纯浏览器三种情形走同一份代码。
//
// 防回环纪律：这里没有任何参数拖动；setGroup / setLang 的回执只用来触发提示，显示一律以快照为准。
// 重绘靠视图模型的 JSON 签名去重：回声每秒来好几次同样的状态，签名不变就什么都不写，hover 的提示与键盘焦点不会被打断。

import { DESIGN } from "../shared/design-box.js";
import { makeT } from "../shared/dom.js";
import { installGlow } from "../shared/glow.js";
import { installShellFit, enforceZoomLock, zoomLocked } from "../shared/shell-fit.js";
import { createShell, createHeader, createLangCapsule } from "../shared/shell-frame.js";
import { createGroupPills } from "../shared/group-pills.js";
import { createMsgline } from "../shared/msgline.js";
import { createFooter } from "../shared/shell-footer.js";
import { createScaleUi } from "../shared/scale-ui.js";
import { createScaleConfirm } from "../shared/shell-scale-confirm.js";
import { createToast } from "../shared/toast.js";
import { createAbout } from "../shared/about.js";
import { docLink, docRef, isAllowedExternal } from "../shared/links.js";
import { createPairCard } from "./pair-card.js";
import { createPlaceBlock, createSendLine } from "./place-send.js";
import {
    defaultPair,
    defaultState,
    deriveView,
    readPair,
    readState,
    HINT_KEYS,
    IDLE_KEY,
    TAKEOVER_KEY,
} from "./view-model.js";

/** 用户指南：仓内稳定路径（docs/stable-links.txt 登记过）。不带锚点：前端白名单不放行 # 片段。 */
export const GUIDE_PATH = "docs/USER_GUIDE.zh-CN.md";

/** 选组成功后等确认帧的最长时间（毫秒）。 */
const GROUP_TOAST_WAIT_MS = 2000;
/** 首个快照到齐前最多藏多久内容层（毫秒）：另一路快照迟迟不到时不能一直空着。 */
const REVEAL_WAIT_MS = 400;
/** requestInitialState 失败后的重试间隔与次数。 */
const INITIAL_RETRY_MS = 400;
const INITIAL_RETRIES = 3;

const noop = () => {};

/**
 * @param {object} o
 * @param {HTMLElement} o.host 挂外壳的容器
 * @param {object} o.bridge createBridge 的返回值（或同形替身）
 * @param {Window} [o.win]
 * @param {{setTimeout:Function, clearTimeout:Function}} [o.timers]
 * @param {{onHelp?:()=>void, onViewRules?:()=>void}} [o.hooks] 首启链接入点：页脚「?」与关于面板的「查看使用规则」
 * @param {boolean} [o.offline] 没有桥（纯浏览器直接打开页面）：不等快照，直接按默认状态显示
 * @param {{overlay?:string, countdown?:number}|null} [o.preview] 预览专用：场景要求直接打开的覆盖层（真桥上没有）
 */
export function mountOutputPage(o) {
    const { host, bridge } = o;
    const win = o.win || window;
    const doc = win.document;
    const timers = o.timers || globalThis;
    const hooks = o.hooks || {};
    const role = "output";

    let state = defaultState();
    let pair = defaultPair();
    let gotState = false;
    let gotPair = false;
    let revealed = false;
    let revealTimer = 0;
    let lastSig = "";
    let lang = "zh";
    let t = makeT(lang);
    let view = deriveView(state, pair);
    /** 选组成功后等待确认的那个组与截止时刻。 */
    let groupToast = null;
    let staticConfirm = false;

    // ---------------------------------------------------------------- 创建共享件

    const shell = createShell({ role });
    const header = createHeader({ role });
    const langCap = createLangCapsule({ role, onPick: (code) => void pickLang(code) });
    const group = createGroupPills({ role, onSelect: (letter) => void pickGroup(letter) });
    const card = createPairCard();
    const send = createSendLine();
    const place = createPlaceBlock();
    const msgline = createMsgline({ role, onAction: (name) => void runAction(name) });
    const toast = createToast({ host: shell.layer, role });
    const scaleUi = createScaleUi({
        role,
        bridge,
        getScale: () => state.uiScale,
        getScreen: () => ({
            availWidth: win.screen && win.screen.availWidth,
            availHeight: win.screen && win.screen.availHeight,
        }),
        onChange: (s) => {
            staticConfirm = false;
            scaleConfirm.update({ ...s, t });
            applyShellState();
        },
        onError: noop,
    });
    const footer = createFooter({
        role,
        onScale: (f) => void scaleUi.preview(f),
        onAbout: () => openAbout(),
        onHelp: () => (typeof hooks.onHelp === "function" ? hooks.onHelp() : undefined),
    });
    const scaleConfirm = createScaleConfirm({
        role,
        host: shell.top,
        inert: [shell.content],
        onKeep: () => {
            void scaleUi.keep();
            closeStaticConfirm();
        },
        onCancel: () => {
            void scaleUi.revert();
            closeStaticConfirm();
        },
    });
    const about = createAbout({
        role,
        host: shell.top,
        inert: [shell.content],
        bridge,
        // Output 没有起播清空，tail 行也还没有桥函数（归 Output 原生桥那张卡）：features 都关，备注 about.adv.noteOut 跟着不出现
        features: { resetOnPlay: false, tail: false },
        onViewRules: () => (typeof hooks.onViewRules === "function" ? hooks.onViewRules() : undefined),
        onCopied: () => toast.show({ key: "toast.diagCopied", t }),
        onCopyFail: (reason) => toast.show({ key: "toast.copyFail", vals: { reason }, t }),
        onClose: () => applyShellState(),
    });

    // 内容层各件的版位：坐标都在 output.css 里（设计稿 v2.5，460×364）
    const slots = [
        ["header", header.el],
        ["lang", langCap.el],
        ["group", group.el],
        ["pair", card.el],
        ["send", send.el],
        ["band", msgline.el],
        ["foot", footer.el],
    ];
    for (const [name, node] of slots) node.classList.add("rn-out", `rn-out--${name}`);
    shell.content.append(...slots.map(([, node]) => node));
    place.el.classList.add("rn-out", "rn-out--place");
    shell.layer.append(place.el);
    host.append(shell.el);
    shell.content.setAttribute("data-booting", "1");

    installGlow(doc);
    const fit = installShellFit({ el: shell.el, box: DESIGN[role], win });

    // ---------------------------------------------------------------- 渲染

    function overlayOn() {
        return about.isOpen() || scaleConfirm.isShown();
    }

    function applyShellState() {
        shell.setState({ danger: view.danger, hostBypass: view.hostBypass, overlay: overlayOn() });
    }

    function buildInfo() {
        return { version: state.version, ...(state.build || {}) };
    }

    function render(force) {
        view = deriveView(state, pair);
        const options = scaleUi.options();
        const locked = zoomLocked();
        const sig = JSON.stringify([view, state.uiScale, state.diagLog, state.build, options, locked]);
        if (!force && sig === lastSig) return;
        lastSig = sig;
        lang = view.lang;
        t = makeT(lang);
        doc.documentElement.lang = lang;

        header.update({ lang, t });
        langCap.update({ lang, t });
        group.update({
            group: view.group.group,
            auto: view.group.auto,
            peers: view.group.peers,
            disabled: view.group.disabled,
            unassigned: view.group.unassigned,
            autoGroup: view.group.group || "",
            t,
        });
        card.update({ view, t });
        send.update({ send: view.send, t });
        place.update({ t });
        msgline.update({
            messages: view.messages,
            confirm: view.confirm
                ? {
                      key: TAKEOVER_KEY,
                      vals: { g: view.confirm.g },
                      onConfirm: () => void resolveTakeover(true),
                      onCancel: () => void resolveTakeover(false),
                  }
                : null,
            idleKey: IDLE_KEY,
            lang,
            t,
        });
        footer.update({
            hint: { state: view.hint, text: t(HINT_KEYS[view.hint]) },
            scaleOptions: options,
            scale: state.uiScale,
            locked,
            version: state.version || "—",
            t,
        });
        if (about.isOpen()) about.update({ t, lang, build: buildInfo(), diagLog: state.diagLog });
        if (scaleUi.state().confirming) scaleConfirm.update({ ...scaleUi.state(), t });
        applyShellState();
    }

    function reveal() {
        if (revealed) return;
        revealed = true;
        if (revealTimer) timers.clearTimeout(revealTimer);
        revealTimer = 0;
        shell.content.removeAttribute("data-booting");
    }

    // ---------------------------------------------------------------- 快照

    function checkGroupToast() {
        if (!groupToast) return;
        if (Date.now() > groupToast.until || state.takeover) {
            groupToast = null;
            return;
        }
        if (state.group === groupToast.g) {
            toast.show({ key: "toast.group", vals: { g: state.group }, t });
            groupToast = null;
        }
    }

    function onState(raw) {
        const next = readState(raw, gotState ? state : null);
        if (!next) return;
        state = next;
        gotState = true;
        // 旧 WebKit 把缩放锁在 100%：宿主按存档开了别的档位时请回 1（走现有 setUiScale，不新增桥函数）
        enforceZoomLock(state.uiScale, (f) => Promise.resolve(bridge.setUiScale(f)).catch(noop));
        afterFrame();
    }

    function onPair(raw) {
        const next = readPair(raw, gotPair ? pair : null);
        if (!next) return;
        pair = next;
        gotPair = true;
        afterFrame();
    }

    function afterFrame() {
        render(false);
        checkGroupToast();
        if (gotState && gotPair) reveal();
        else if (!revealTimer && !revealed) revealTimer = timers.setTimeout(reveal, REVEAL_WAIT_MS);
    }

    // ---------------------------------------------------------------- 交互

    async function pickGroup(letter) {
        if (letter === state.group && !state.takeover) return;
        try {
            const res = await bridge.setGroup(letter);
            if (res && res.ok === true) {
                groupToast = { g: letter, until: Date.now() + GROUP_TOAST_WAIT_MS };
                checkGroupToast();
            }
        } catch {
            // 桥没接上：显示以快照为准，没有快照变化就什么都不发生
        }
    }

    async function pickLang(code) {
        try {
            const res = await bridge.setLang(code);
            if (res && res.ok === true) {
                state = { ...state, lang: code };
                render(false);
            }
        } catch {
            // 同上
        }
    }

    async function resolveTakeover(accept) {
        try {
            await bridge.resolveTakeover(accept);
        } catch {
            // 同上
        }
    }

    async function runAction(name) {
        if (name === "repair") {
            try {
                const res = await bridge.repairPair();
                if (res && res.ok === true && typeof res.group === "string") {
                    groupToast = { g: res.group, until: Date.now() + GROUP_TOAST_WAIT_MS };
                    checkGroupToast();
                }
            } catch {
                // 同上
            }
        } else if (name === "guide") {
            const url = docLink(docRef(state.version), GUIDE_PATH);
            if (!isAllowedExternal(url)) return;
            try {
                await bridge.openExternal(url);
            } catch {
                // 同上
            }
        }
    }

    function openAbout() {
        void about.open({ t, lang, build: buildInfo(), cfg: {}, diagLog: state.diagLog });
        applyShellState();
    }

    function closeStaticConfirm() {
        if (!staticConfirm || scaleUi.state().confirming) return;
        staticConfirm = false;
        scaleConfirm.update({ confirming: false, left: 0, t });
        applyShellState();
    }

    // ---------------------------------------------------------------- 预览专用覆盖层

    function applyPreviewOverlay() {
        const p = o.preview;
        if (!p || !p.overlay) return;
        if (p.overlay === "about") openAbout();
        else if (p.overlay === "scaleConfirm") {
            // 静态展示：不起状态机的倒计时，截图与评审时数字固定
            staticConfirm = true;
            scaleConfirm.update({ confirming: true, left: Number.isFinite(p.countdown) ? p.countdown : 10, t });
            applyShellState();
        }
    }

    // ---------------------------------------------------------------- 启动

    const offs = [];
    if (!o.offline) {
        offs.push(bridge.on("rinne.state", onState));
        offs.push(bridge.on("rinne.pair", onPair));
        offs.push(bridge.on("rinne.error", noop));
    }
    win.addEventListener("pagehide", () => scaleUi.pagehide());

    async function requestInitial(tries) {
        let ok = false;
        try {
            const res = await bridge.requestInitialState();
            ok = !(res && res.ok === false);
        } catch {
            ok = false;
        }
        if (!ok && tries > 0) timers.setTimeout(() => void requestInitial(tries - 1), INITIAL_RETRY_MS);
    }

    render(true);
    if (o.offline) {
        reveal();
    } else {
        // 桥一直不给快照也不能让窗口空着：到点按默认状态揭幕，快照随后到了照常覆盖
        revealTimer = timers.setTimeout(reveal, REVEAL_WAIT_MS);
        void requestInitial(INITIAL_RETRIES);
    }
    applyPreviewOverlay();

    return {
        el: shell.el,
        /** 测试与预览用的观测口：当前快照与视图模型的只读副本。 */
        snapshot: () => ({ state, pair, view }),
        parts: {
            shell,
            header,
            langCap,
            group,
            card,
            send,
            place,
            msgline,
            footer,
            toast,
            about,
            scaleUi,
            scaleConfirm,
        },
        fit,
        isRevealed: () => revealed,
        destroy() {
            for (const off of offs) if (typeof off === "function") off();
            if (revealTimer) timers.clearTimeout(revealTimer);
            msgline.destroy();
            fit.destroy();
        },
    };
}
