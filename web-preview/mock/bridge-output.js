// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rinne Output 的 mock 后端：九个共用函数，三个事件。Output 只做分组与配对，没有参数。
// 名表与 docs/UI_BRIDGE.md、src/output/OutputBridgeApi.h、web/shared/bridge.js 逐名相等，由 check-bridge-parity 对拍。
// 载荷形状见 docs/UI_BRIDGE.md；这里只造够页面开发用的数据。

import { buildDiagnostics, commonFunctions, createCore, memoryStorage } from "./bridge-common.js";

export const FUNCTIONS = [
    "requestInitialState",
    "setLang",
    "setUiScale",
    "commitUiScale",
    "setRulesAck",
    "setGuideSeen",
    "setGroup",
    "copyDiagnostics",
    "openExternal",
];

export const EVENTS = ["rinne.state", "rinne.pair", "rinne.error"];

/**
 * 创建 Output 后端。
 * @param {object} o
 * @param {{local:object, session:object}} [o.storage]
 * @param {object} [o.timers]
 * @param {object} [o.opts] 见 createCore
 */
export function createOutputBackend({ storage, timers, opts } = {}) {
    const store = storage || { local: memoryStorage(), session: memoryStorage() };
    const core = createCore({ role: "output", events: EVENTS, storage: store, timers, opts });

    const model = {
        version: "0.9.0",
        host: "preview",
        sampleRate: 48000,
        blockSize: 512,
        scale: core.ui.uiScaleOutput,
        lang: null, // 非空时覆盖全局语言（?lang=），不落盘
        group: "A",
        groupOrigin: "auto",
        restored: false,
        send: { channels: "ST", sampleRate: 48000, bypassed: false, hostPaused: false },
        pair: {
            state: "searching",
            groupMask: 0,
            peer: { name: null, color: null, channels: null, version: null, holder: null },
            self: { name: null },
            verify: "unknown",
        },
    };

    const env = {
        get scale() {
            return model.scale;
        },
        set scale(v) {
            model.scale = v;
        },
        get group() {
            return model.group;
        },
        set group(v) {
            model.group = v;
        },
        get groupOrigin() {
            return model.groupOrigin;
        },
        set groupOrigin(v) {
            model.groupOrigin = v;
        },
        clipboard: null,
        opened: null,
        diagnostics: () =>
            buildDiagnostics({
                "diag.format": "rinne-diag v1",
                "diag.version": model.version,
                "diag.role": "output",
                "diag.os": "preview",
                "diag.host": model.host,
                "diag.sample_rate": model.sampleRate,
                "diag.block_size": model.blockSize,
                "diag.group": model.group,
                "diag.pair_state": model.pair.state,
                "diag.track_relation": model.pair.verify,
                "diag.lang": model.lang ?? core.ui.lang,
                "diag.ui_scale": model.scale,
                "diag.webview": "preview",
            }),
    };
    const common = commonFunctions(core, env);

    core.register("rinne.state", () => ({
        seq: core.nextSeq(),
        role: "output",
        version: model.version,
        lang: model.lang ?? core.ui.lang,
        langChosen: core.ui.langChosen,
        rulesAck: core.ui.rulesAckVersion,
        guideSeen: core.ui.guideSeenOutput,
        inputGuideSeen: core.ui.guideSeenInput,
        uiScale: model.scale,
        restored: model.restored,
        group: model.group,
        groupOrigin: model.groupOrigin,
        diagLog: core.ui.diagLog,
        sampleRate: model.sampleRate,
        send: { ...model.send },
    }));
    core.register("rinne.pair", () => ({
        seq: core.nextSeq(),
        state: model.pair.state,
        groupMask: model.pair.groupMask,
        peer: { ...model.pair.peer },
        self: { ...model.pair.self },
        verify: model.pair.verify,
    }));

    const mock = { addEventListener: core.addEventListener };
    for (const name of FUNCTIONS) mock[name] = common[name];
    return { role: "output", core, model, mock };
}
