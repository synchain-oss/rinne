// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rinne Output 的 mock 后端：十二个共用函数加两个 Output 专用函数（repairPair、setTailMode），三个事件。
// Output 只做分组与配对，没有参数；随工程走的非参数设置只有 tail 模式（cfg.tailMode）。
// 名表与 docs/UI_BRIDGE.md、src/output/OutputBridgeApi.h、web/shared/bridge.js 逐名相等，由 check-bridge-parity 对拍。
// 载荷形状见 docs/UI_BRIDGE.md；这里只造够页面开发用的数据。
//
// 场景（?scenario=）里 state-driver 管不到的几项，在首次造帧时由本文件补上，只做一次、之后交给用户操作：
//   groupAuto 缺省 = 手动选的组（模型默认是「自动」）；scale: "150%" 这种档位；msg 里的 crossTrack 在配对状态不是
//   crossTrack 时靠核验结果「不同」表达；msg 里属于 notices 的四条（ipcSandbox 等）进 rinne.state 的 notices
//   （newerProject 的 a / b 照原生写成 "ABI N"：a 是工程 state 的 abi，b 是本构建的 abi）；
//   有名字的轨道配上预览用的颜色。真桥上没有 previewScene，这些补丁在真宿主里不存在。

import { GROUPS, PAIR_STATES } from "./contract.js";
import { sceneById } from "./scenarios.js";
import {
    buildDiagnostics,
    commonFunctions,
    createCore,
    memoryStorage,
    previewDiagEvents,
    previewDiagValues,
} from "./bridge-common.js";

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
    "setDiagLog",
    "resolveTakeover",
    "showLicense",
    "repairPair",
    "setTailMode",
];

export const EVENTS = ["rinne.state", "rinne.pair", "rinne.error"];

/** 契约里没有独立状态的四条消息，经 rinne.state 的 notices 字段带给页面。 */
const NOTICE_IDS = ["ipcSandbox", "newerProject", "stateDecodeFailed", "outOfOrder"];
/** setTailMode 收的三个值，与 Input 同一张表。 */
const TAIL_MODES = ["auto", "infinite", "30s"];
/** newerProject 的 a / b：预览里假定工程比本构建新一版 abi。 */
const PREVIEW_NEWER_ABI = { a: "ABI 2", b: "ABI 1" };
/** 预览里有名字的轨道用的颜色（设计稿的示例蓝）。 */
const PREVIEW_TRACK_COLOR = "rgb(45,121,198)";
const PREVIEW_TRACK_NAME = "Vox Lead";
/** 这几个态里没有对端，Input 所在轨不显示名字（searching 显示插入链，其余显示「未提供轨道名」）。 */
const NO_PEER_STATES = ["searching", "conflict", "waitingAudio", "unassigned", "ipcUnavailable"];

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
        version: "0.0.0-preview",
        host: "preview",
        sampleRate: 48000,
        blockSize: 512,
        scale: core.ui.uiScaleOutput,
        lang: null, // 非空时覆盖全局语言（?lang=），不落盘
        group: "A",
        groupOrigin: "auto",
        restored: false,
        takeover: false, // 休眠实例占着本组，等用户确认接管
        send: { channels: "ST", sampleRate: 48000, bypassed: false, hostPaused: false },
        notices: [], // [{ id, a?, b? }]，id 取 NOTICE_IDS
        build: { sha: "a1b2c3d", fmt: "VST3", arch: "Windows x64" },
        cfg: { tailMode: "auto" },
        pair: {
            state: "searching",
            groupMask: 0,
            peer: { name: null, color: null, channels: null, version: null, holder: null, sampleRate: null },
            self: { name: null, color: null },
            verify: "unknown",
        },
    };

    let sceneApplied = false;
    const sceneState = () => (mock.previewScene && mock.previewScene.state) || null;
    /**
     * 场景表把「设计稿里的默认轨道」省掉了：设计稿每个 Output 场景都从「本轨 Vox Lead、Input 所在轨 Vox Lead」起算，
     * 只有没有对端的几个态才覆盖成无名或插入链。state-driver 只认写了 tracks 的场景，这里把缺省补成设计稿的样子；
     * 场景里写成 "chain" 的是「没有对端」的标记，不是轨道名。
     */
    function fillDesignTracks(sc) {
        const has = (o, k) => Object.prototype.hasOwnProperty.call(o, k);
        const tr = sc.tracks && typeof sc.tracks === "object" ? sc.tracks : null;
        if (!tr || !has(tr, "this")) model.pair.self.name = PREVIEW_TRACK_NAME;
        if (model.pair.peer.name === "chain") model.pair.peer.name = null;
        if (!tr) {
            model.pair.peer.name = NO_PEER_STATES.includes(model.pair.state) ? null : PREVIEW_TRACK_NAME;
            model.pair.verify = model.pair.peer.name ? "match" : "unknown";
        }
    }

    /**
     * 场景写成 { as: "<另一个场景>" }（out-en）时 state-driver 不展开，页面会落在默认态；这里把被引用场景里
     * Output 关心的几项按 state-driver 同样的口径套上，再按被引用场景的 state 往下补。
     */
    function resolveAs(sc) {
        const base = typeof sc.as === "string" ? sceneById(sc.as) : null;
        if (!base || base.role !== "output") return sc;
        const st = base.state;
        if (typeof st.pair === "string" && PAIR_STATES.includes(st.pair)) model.pair.state = st.pair;
        if (Object.prototype.hasOwnProperty.call(st, "group")) model.group = st.group;
        if (st.groupAuto) model.groupOrigin = "auto";
        if (st.tracks && typeof st.tracks === "object") {
            model.pair.self.name = st.tracks.this ?? null;
            model.pair.peer.name = st.tracks.input ?? null;
            model.pair.verify = { match: "match", diff: "diff" }[st.tracks.verify] || "unknown";
        }
        if (typeof st.send === "string" && st.send.startsWith("ST")) {
            model.send.channels = "ST";
            model.send.sampleRate = (Number.parseFloat(st.send.split("·")[1]) || 48) * 1000;
        }
        return st;
    }

    /** 首次造帧时把场景里 state-driver 管不到的几项补上；只做一次。 */
    function applySceneOnce() {
        if (sceneApplied) return;
        sceneApplied = true;
        const own = sceneState();
        if (!own) return;
        const sc = resolveAs(own);
        if (!sc.groupAuto && model.group) model.groupOrigin = "manual";
        const pct = typeof sc.scale === "string" ? Number.parseFloat(sc.scale) / 100 : NaN;
        if (core.box.presets.includes(pct)) model.scale = pct;
        const msgs = Array.isArray(sc.msg) ? sc.msg : [];
        if (msgs.includes("crossTrack") && model.pair.state !== "crossTrack") model.pair.verify = "diff";
        for (const id of msgs) {
            if (NOTICE_IDS.includes(id))
                model.notices.push(id === "newerProject" ? { id, ...PREVIEW_NEWER_ABI } : { id });
        }
        fillDesignTracks(sc);
        if (model.pair.self.name) model.pair.self.color = PREVIEW_TRACK_COLOR;
        if (model.pair.peer.name) model.pair.peer.color = PREVIEW_TRACK_COLOR;
        if (model.pair.peer.channels === null && !NO_PEER_STATES.includes(model.pair.state)) {
            model.pair.peer.channels = 2;
        }
    }

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
        get takeover() {
            return model.takeover;
        },
        set takeover(v) {
            model.takeover = v;
        },
        set pairState(v) {
            model.pair.state = v;
        },
        licenseShown: false,
        clipboard: null,
        opened: null,
        // Output 只输出字段子集：没有 loop、sync、order、safety 四节
        diagnostics: () =>
            buildDiagnostics("output", previewDiagValues("output", model, core), previewDiagEvents(model)),
    };
    const common = commonFunctions(core, env);

    core.register("rinne.state", () => {
        applySceneOnce();
        return {
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
            takeover: model.takeover,
            // 待接管的组：预览里休眠实例占着的就是本组；没有待接管时不发
            ...(model.takeover && model.group ? { takeoverGroup: model.group } : {}),
            group: model.group,
            groupOrigin: model.groupOrigin,
            diagLog: core.ui.diagLog,
            sampleRate: model.sampleRate,
            send: { ...model.send },
            notices: model.notices.map((n) => ({ ...n })),
            build: { ...model.build },
            cfg: { ...model.cfg },
        };
    });
    core.register("rinne.pair", () => {
        applySceneOnce();
        return {
            seq: core.nextSeq(),
            state: model.pair.state,
            groupMask: model.pair.groupMask,
            peer: { ...model.pair.peer },
            self: { ...model.pair.self },
            verify: model.pair.verify,
        };
    });

    const mock = { addEventListener: core.addEventListener };
    const fns = {
        ...common,
        repairPair() {
            if (model.pair.state !== "conflict") return { ok: false, reason: "not-conflict" };
            // 按智能默认重选组：这里取第一个与当前不同、没有未配对对端在等的组
            const free = GROUPS.find((g) => g !== model.group && !(model.pair.groupMask & (1 << GROUPS.indexOf(g))));
            if (!free) return { ok: false, reason: "no-free-group" };
            model.group = free;
            model.groupOrigin = "auto"; // 重新配对是按智能默认重选，所以仍标「自动」
            model.pair.state = "searching";
            core.scheduleEcho();
            return { ok: true, group: free };
        },
        setTailMode(mode) {
            if (!TAIL_MODES.includes(mode)) return { ok: false, reason: "bad-value" };
            model.cfg.tailMode = mode;
            core.scheduleEcho();
            return { ok: true, value: mode };
        },
    };
    for (const name of FUNCTIONS) mock[name] = fns[name];
    return { role: "output", core, model, mock };
}
