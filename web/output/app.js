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

import common from "../i18n/common.js";
import output from "../i18n/output.js";
import { installDicts } from "../shared/i18n.js";
import { createBridge, BRIDGE_FUNCTIONS } from "../shared/bridge.js";
import { mountOutputPage } from "./page.js";

installDicts(common, output);

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

/** 预览专用：场景要求直接打开的覆盖层。真桥上没有 previewScene，返回 null。 */
function previewOverlay(bridge) {
    const mock = window.__RINNE_MOCK__;
    const st = bridge.isPreview && mock && mock.previewScene ? mock.previewScene.state : null;
    return st && typeof st === "object" ? { overlay: st.overlay, countdown: st.countdown } : null;
}

const { bridge, offline } = openBridge();
mountOutputPage({
    host: document.getElementById("app"),
    bridge,
    offline,
    preview: previewOverlay(bridge),
});

// 主模块初始化完成：boot 守卫与首帧信号都到位，才算页面起来了（见 index.html 头部）
if (typeof window.__rinneMarkReady === "function") window.__rinneMarkReady();
