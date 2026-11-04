// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rinne Output 页面入口：装词条、接桥、挂页面，最后告诉 index.html 的启动守卫「主模块已起来」。
//
// 桥有三种来路：
//   真宿主        window.__JUCE__ 带着已登记的原生函数；
//   预览          壳页往 window.__RINNE_MOCK__ 注入 mock 后端（web-preview/shell.js）；
//   都没有        纯浏览器直接打开本页（首帧冒烟、离线扫描）：没有桥可接，页面以默认状态显示，不报错。
// 真宿主里 createBridge 失败是真故障，让异常冒出去，由 index.html 的 boot 守卫上报原生侧；
// 只有「既没有 mock、也没有 JUCE 后端」这一种情形才降级成无桥模式，免得把真故障悄悄吞成一个空壳。
//
// 首启链（语言卡 → 使用规则页 → 导览）挂在这里而不进 page.js：页面经 hooks 交出「?」与关于面板的「查看使用规则」，
// 页面的视图模型不收全局位，所以这里自己订一份 rinne.state / rinne.pair 原帧（同样丢弃序号不增的帧），
// 摘出 langChosen、rulesAck、guideSeen、inputGuideSeen、restored，危险态读页面的视图模型。
// 预览里给了场景就不自动弹（场景描述的是一种既定画面），首启的三个场景改为静态摆出；无桥模式也不弹。

import common from "../i18n/common.js";
import output from "../i18n/output.js";
import guide from "../i18n/guide.js";
import { installDicts } from "../shared/i18n.js";
import { createBridge, BRIDGE_FUNCTIONS } from "../shared/bridge.js";
import { createFirstRun } from "../shared/first-run.js";
import { mountOutputPage } from "./page.js";
import { TOUR_OUT_STEPS } from "./tour-out.js";

installDicts(common, output, guide);

/** 无桥时的替身：每个函数都回「没准备好」，事件订阅是空操作。 */
function createOfflineBridge() {
    const bridge = { role: "output", isPreview: false, on: () => () => {} };
    for (const name of BRIDGE_FUNCTIONS.output) bridge[name] = async () => ({ ok: false, reason: "not-ready" });
    return bridge;
}

function openBridge() {
    const hasMock = Boolean(window.__RINNE_MOCK__);
    const hasHost = Boolean(window.__JUCE__ && window.__JUCE__.backend);
    if (!hasMock && !hasHost) {
        console.warn("Rinne Output：没有检测到桥（既不是插件宿主也不是预览），按无桥模式显示默认状态。");
        return { bridge: createOfflineBridge(), offline: true };
    }
    return { bridge: createBridge({ role: "output" }), offline: false };
}

/** 预览专用：场景描述。真桥上没有 previewScene，返回 null。 */
function previewScene(bridge) {
    const mock = window.__RINNE_MOCK__;
    const st = bridge.isPreview && mock && mock.previewScene ? mock.previewScene.state : null;
    return st && typeof st === "object" ? st : null;
}

/** 「环路这头正在回送」：已连接，且宿主没有旁路或暂停发送端。规则页据此放行 Esc。 */
const FLOWING = new Set(["connected", "connectedUnverified"]);
function outputFlowing(state, pair) {
    const send = (state && state.send) || {};
    return FLOWING.has(pair && pair.state) && send.bypassed !== true && send.hostPaused !== true;
}

const { bridge, offline } = openBridge();
const scene = previewScene(bridge);
// 原帧：只认序号递增的帧；两路都到过一帧才喂首启链。
// 要在挂页面之前订：页面一挂上就请求首帧，后端可能同步回帧（预览就是），晚订会漏掉首批，
// 恢复实例的页脚提示要等下一次回声才换。喂放进微任务：同一批帧页面先处理完（危险态读页面的视图模型），首启链再吃。
const raw = { state: null, pair: null, seq: { state: -Infinity, pair: -Infinity } };
let restoredShown = false;
let sceneShown = false;
let feedQueued = false;
function feed() {
    if (!raw.state || !raw.pair) return;
    const st = raw.state;
    firstRun.update(
        {
            lang: st.lang,
            langChosen: st.langChosen,
            rulesAck: st.rulesAck,
            guideSeen: st.guideSeen,
            inputGuideSeen: st.inputGuideSeen,
            restored: st.restored,
        },
        { l2: page.snapshot().view.danger === true, guardLit: false, loopActive: outputFlowing(st, raw.pair) },
    );
    // 恢复的实例而本机没确认过当前版本：页脚提示换成「点版本号查看」，冲突的提示照旧优先
    if (firstRun.restoredHint() !== restoredShown) {
        restoredShown = firstRun.restoredHint();
        footer.setHintOverride(
            restoredShown
                ? (h, t) => (h && h.state === "conflict" ? h : { state: "restored", text: t("hint.firstUseRestored") })
                : null,
        );
    }
    if (scene && !sceneShown) {
        sceneShown = true;
        firstRun.showScene(scene);
    }
}
const take = (kind) => (f) => {
    if (!f || typeof f !== "object") return;
    if (Number.isFinite(f.seq)) {
        if (f.seq <= raw.seq[kind]) return;
        raw.seq[kind] = f.seq;
    }
    raw[kind] = f;
    if (feedQueued) return;
    feedQueued = true;
    queueMicrotask(() => {
        feedQueued = false;
        feed();
    });
};
if (!offline) {
    bridge.on("rinne.state", take("state"));
    bridge.on("rinne.pair", take("pair"));
}

let firstRun = null;
const page = mountOutputPage({
    host: document.getElementById("app"),
    bridge,
    offline,
    preview: scene ? { overlay: scene.overlay, countdown: scene.countdown } : null,
    hooks: {
        onHelp: () => firstRun.openRules(),
        onViewRules: () => {
            page.parts.about.close();
            firstRun.openRules();
        },
    },
});
const { shell, footer } = page.parts;
firstRun = createFirstRun({
    role: "output",
    bridge,
    host: shell.top,
    shell: shell.el,
    // 常驻提示在 layer 层，不在 content 层里：它的「知道了」按钮也要一起 inert，否则 Tab 能走到叠层之下
    inert: [shell.content, page.parts.place.el],
    steps: TOUR_OUT_STEPS,
    onChange: () => shell.refresh(),
});
// 首启链开着时外壳同样进遮罩态（与关于面板、缩放确认一样），页面的遮罩判据之外再并上它
shell.setOverlaySource(() => firstRun.isOpen());
if (offline || scene) firstRun.disableAuto();

// 主模块初始化完成：boot 守卫与首帧信号都到位，才算页面起来了（见 index.html 头部）
if (typeof window.__rinneMarkReady === "function") window.__rinneMarkReady();
