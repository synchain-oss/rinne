// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/web.json
//
// bridge.js：前端桥，JS 与 C++ 之间唯一的传输层。
//
// 职责：把名表里登记的名字原样接到两种后端上，别的什么都不做。
//   - 上行（界面到 C++）：对 BRIDGE_FUNCTIONS[role] 的每个名字生成一个 async 方法，参数原样透传；
//   - 下行（C++ 到界面）：对 BRIDGE_EVENTS[role] 的每个名字挂监听，统一经 on(evt, cb) 分发。
// 页面只依赖本文件导出的 createBridge()，不直接碰 window.__JUCE__，也不自建事件通道。
//
// 后端切换语义：
//   - 检测到真 JUCE 宿主时用 JuceBackend：惰性 import("../js/juce/index.js") 后取 getNativeFunction(name)
//     调用；事件走 window.__JUCE__.backend.addEventListener。必须惰性，因为 JUCE 的辅助脚本在模块顶层
//     就会访问 window，静态 import 会让本文件在没有 window 的环境里直接报错。
//   - 否则用调用方注入的 opts.mockBackend（缺省取全局 window.__RINNE_MOCK__，由浏览器预览入口提供）。
//     本文件不含任何 mock 或假数据实现。两者都没有就抛错并给出指引。
//   - 判定为什么不是 `if (window.__JUCE__)`：JUCE 官方辅助脚本 check_native_interop.js 在没有 JUCE 宿主的
//     普通浏览器里会自己造一个占位 window.__JUCE__（带纯本地的 backend，以及空的
//     initialisationData.__juce__functions）。预览页只要在 createBridge 之前的任何位置 import 过
//     js/juce/index.js，「__JUCE__ 存在即 JUCE」的旧判定就会走 JuceBackend 分支并忽略注入的 mock，
//     所有 await 永久挂起，失败形态是白屏加零报错。所以判据收紧为 isJuceHost()：backend 存在，
//     并且 __juce__functions 是非空数组；同时让显式注入优先，占位对象加已注入 mock 就走 mock，并留一条警告。
//
// 只做传输，不复制契约语义：
//   - 只登记名字，不复制载荷字段，不校验参数，不做夹取：越界或类型不符一律由 C++ 侧夹取或拒绝。
//   - 不代调 requestInitialState()：何时调由页面掌握（首次装载，以及编辑器关闭重建后都要重新调用一次），
//     桥不猜时机，不缓存快照，不重放事件。
//   - 不做节流、diff 和首帧必发（都在 C++ 侧），也不按宿主回声过滤（防回环是界面的纪律：拖动期间
//     收到的回声只更新显示，绝不回写）。
//   - 枚举取值、载荷形状、单位换算一概不出现在本文件；想知道某个名字收什么、回什么，读 docs/UI_BRIDGE.md。
//
// 名表是 scripts/check-bridge-parity.mjs 的比对面（脚本按正则读数组字面量，不执行本文件）：
// 必须与 docs/UI_BRIDGE.md、两个 *BridgeApi.h 和预览 mock 逐字相符。数组行内不要写带方括号的注释，
// 脚本按首个右方括号截取数组。加桥函数时这几处同一个 PR 一起改。
//
// mock 后端形状：opts.mockBackend 必须是 window.__JUCE__.backend 的同形替身，
// 让「mock 与真后端导出名一致」由结构保证，而不靠人工核对：
//   - BRIDGE_FUNCTIONS[role] 的每个名字一个方法（可以是 async，返回 JSON 对象）；
//   - addEventListener(evtName, cb) 一个方法，用于推送 BRIDGE_EVENTS[role] 的事件。
// 缺名会在 createBridge() 处抛错并列出缺失项，这是运行期兜底，静态面由 parity 脚本把关。

/**
 * 桥函数名表。两个角色共用的名字（前九个）在两侧各写一遍，数组保持纯字面量，
 * 方便 parity 脚本不执行本文件就能读出。
 */
export const BRIDGE_FUNCTIONS = {
    output: [
        "requestInitialState",
        "setLang",
        "setUiScale",
        "commitUiScale",
        "setRulesAck",
        "setGuideSeen",
        "setGroup",
        "copyDiagnostics",
        "openExternal",
    ],
    input: [
        "requestInitialState",
        "setLang",
        "setUiScale",
        "commitUiScale",
        "setRulesAck",
        "setGuideSeen",
        "setGroup",
        "copyDiagnostics",
        "openExternal",
        "beginParamGesture",
        "setParam",
        "endParamGesture",
        "setLimiter",
        "panic",
        "resumeLoop",
        "dismissHint",
        "loadPreset",
        "savePreset",
        "deletePreset",
    ],
};

/** 桥事件名表。事件是纯下行：界面只订阅，绝不用这些名字发起上行调用。 */
export const BRIDGE_EVENTS = {
    output: ["rinne.state", "rinne.pair", "rinne.error"],
    input: ["rinne.state", "rinne.pair", "rinne.error", "rinne.params", "rinne.meters", "rinne.presets"],
};

// 名表冻结，防止调用方就地改写（mock 若靠 push 补名，parity 脚本静态扫不出来）。
// 必须写成「先声明字面量、再后置 freeze」：parity 脚本按 `<表名> = {` 的字面量抽取，
// 写成 Object.freeze({...}) 会让抽取落空。
for (const table of [BRIDGE_FUNCTIONS, BRIDGE_EVENTS]) {
    Object.freeze(table.output);
    Object.freeze(table.input);
    Object.freeze(table);
}

/** 合法角色。 */
const ROLES = ["output", "input"];

/**
 * 返回对象上的保留键。万一将来新增的函数名撞上这三个键，必须先改本文件，
 * 而不是让桥函数悄悄覆盖 on、role 或 isPreview。
 */
const RESERVED_KEYS = new Set(["role", "isPreview", "on"]);

/**
 * 极简事件分发器。同一事件的回调按注册顺序同步调用；派发前取快照，允许回调内退订。
 */
function makeEmitter() {
    const map = new Map();
    return {
        on(evt, cb) {
            if (typeof cb !== "function") {
                throw new TypeError("bridge.on(evt, cb)：cb 必须是函数");
            }
            let list = map.get(evt);
            if (!list) {
                list = [];
                map.set(evt, list);
            }
            list.push(cb);
            return function off() {
                const i = list.indexOf(cb);
                if (i >= 0) list.splice(i, 1);
            };
        },
        emit(evt, payload) {
            const list = map.get(evt);
            if (!list) return;
            for (const cb of list.slice()) cb(payload);
        },
    };
}

/**
 * 事件接线，JuceBackend 与 mock 后端共用：后端只需实现 addEventListener(name, cb)。
 * 只接本侧名表内的事件；C++ 若推了表外的名字，这里不会有监听者，parity 检查会在静态面先报出来。
 */
function wireEvents(role, backend, em) {
    for (const name of BRIDGE_EVENTS[role]) {
        backend.addEventListener(name, (payload) => em.emit(name, payload));
    }
}

/** 订阅口：事件名不在本侧名表内就抛错（拼错事件名比收不到事件更难查）。 */
function makeOn(role, em) {
    const allowed = new Set(BRIDGE_EVENTS[role]);
    return function on(evt, cb) {
        if (!allowed.has(evt)) {
            throw new RangeError(
                `bridge.on：事件名 ${JSON.stringify(evt)} 不在 ${role} 侧名表内；` +
                    `可用事件：${BRIDGE_EVENTS[role].join(", ")}`,
            );
        }
        return em.on(evt, cb);
    };
}

/**
 * 组装返回对象 { role, isPreview, on, ...每个函数一个方法 }。
 * call(name) 由各后端提供；方法一律 async，参数原样透传（校验归 C++）。
 * isRegistered(name) 判断后端是否真的实现了这个名字。
 */
function assemble(role, isPreview, em, call, isRegistered) {
    const api = { role, isPreview, on: makeOn(role, em) };
    const absent = [];
    for (const name of BRIDGE_FUNCTIONS[role]) {
        if (RESERVED_KEYS.has(name)) {
            throw new Error(`bridge：函数名 ${name} 与返回对象的保留键冲突，须先改 bridge.js 的 RESERVED_KEYS`);
        }
        // 名表里有而后端尚未实现的名字不挂：挂了会让页面的 `typeof bridge.X === "function"`
        // 特性探测判成「能用」，按钮点亮却什么也不做，那比「桥上没有这个名字」更坏。
        // 名表与实现之间的时间差由此如实暴露。
        if (!isRegistered(name)) {
            absent.push(name);
            continue;
        }
        api[name] = call(name);
    }
    if (absent.length > 0) {
        console.warn(
            `[bridge] ${role}：名表里有而宿主未登记的函数 ${absent.length} 个：` +
                `${absent.join(", ")}。这些名字不会挂到桥上（调用方读到 undefined），` +
                `属于「名表已加、原生侧待落地」的正常中间态；原生侧接线后这条警告自动消失。`,
        );
    }
    return api;
}

/**
 * 是否为真 JUCE 宿主（而不是 check_native_interop.js 造的占位对象，详见文件头）。
 *
 * 判据取 initialisationData.__juce__functions 为非空数组：
 *   - 占位对象把它初始化成空数组，恒为空；
 *   - 真宿主把每个已注册的 native function 名逐个登记进去，且该表为空时 JUCE 根本不开 native
 *     integration，所以真宿主里的空表等价于「桥不可能通」，不存在把真宿主误判成占位的空间。
 */
function isJuceHost(juce) {
    if (!juce || !juce.backend) return false;
    const data = juce.initialisationData;
    const fns = data ? data.__juce__functions : undefined;
    return Array.isArray(fns) && fns.length > 0;
}

/**
 * JuceBackend：插件内（检测到真 JUCE 宿主）。
 * 辅助脚本只 import 一次，每次调用时再取 getNativeFunction。
 */
function makeJuceBackend(role) {
    const em = makeEmitter();
    wireEvents(role, window.__JUCE__.backend, em);

    // 惰性加载 JUCE 官方前端辅助脚本（web/js/juce/）。
    const nativePromise = import("../js/juce/index.js");
    const call =
        (name) =>
        async (...args) => {
            const { getNativeFunction } = await nativePromise;
            return getNativeFunction(name)(...args);
        };

    // 真宿主把每个已注册的 native function 登记在 initialisationData.__juce__functions
    const registered = new Set((window.__JUCE__.initialisationData || {}).__juce__functions || []);
    return assemble(role, false, em, call, (name) => registered.has(name));
}

/**
 * mock 后端：浏览器预览（没有真 JUCE 宿主）。实现由 web-preview 注入，本文件只负责接线与校名。
 */
function makeMockBridge(role, mock) {
    if (typeof mock.addEventListener !== "function") {
        throw new TypeError(
            "createBridge：opts.mockBackend 缺少 addEventListener(evtName, cb)，" +
                "mock 后端须与 window.__JUCE__.backend 同形",
        );
    }
    const missing = BRIDGE_FUNCTIONS[role].filter((name) => typeof mock[name] !== "function");
    if (missing.length > 0) {
        throw new TypeError(
            `createBridge：opts.mockBackend 缺少 ${missing.length} 个 ${role} 侧函数` +
                `（mock 须逐一实现全集）：${missing.join(", ")}`,
        );
    }

    const em = makeEmitter();
    wireEvents(role, mock, em);
    const call =
        (name) =>
        async (...args) =>
            mock[name](...args);

    return assemble(role, true, em, call, (name) => typeof mock[name] === "function");
}

/**
 * 工厂，界面只调这一个。
 *
 * @param {{role: "output"|"input", mockBackend?: object}} opts
 *   role        必填；非法就抛错（每个页面各自传死值，不做自动嗅探）。
 *   mockBackend 仅在非真 JUCE 宿主时使用（含 __JUCE__ 只是辅助脚本占位对象的情形）；
 *               缺省取全局 window.__RINNE_MOCK__，由 web-preview 的入口注入。
 * @returns {{role: string, isPreview: boolean, on: Function}} 另含本侧全部桥函数（均为 async）。
 */
export function createBridge(opts = {}) {
    const { role } = opts;
    if (!ROLES.includes(role)) {
        throw new TypeError(
            `createBridge：opts.role 必须是 ${ROLES.map((r) => JSON.stringify(r)).join(" / ")} 之一，实得 ${JSON.stringify(role)}`,
        );
    }
    const win = typeof window !== "undefined" ? window : undefined;
    const juce = win ? win.__JUCE__ : undefined;
    const mockBackend = opts.mockBackend || (win && win.__RINNE_MOCK__);
    if (isJuceHost(juce)) {
        return makeJuceBackend(role);
    }
    if (mockBackend) {
        // 显式注入优先：__JUCE__ 存在但不是真宿主，说明是辅助脚本的占位对象，走 mock 并留一条痕迹，
        // 免得预览页「看起来接上了 JUCE」而实际在跑 mock（反之亦然）时无从判断。
        if (juce) {
            console.warn(
                "createBridge：检测到 JUCE 辅助脚本的占位对象（window.__JUCE__ 存在但未登记任何 native 函数），" +
                    "已按注入的 mockBackend 运行。",
            );
        }
        return makeMockBridge(role, mockBackend);
    }
    throw new Error(
        "createBridge：未检测到真 JUCE 宿主" +
            (juce
                ? "（window.__JUCE__ 只是 check_native_interop.js 造的占位对象，initialisationData." +
                  "__juce__functions 为空）"
                : "") +
            "，且未注入 mockBackend。浏览器预览请经 web-preview 的入口加载 mock 后端；" +
            "bridge.js 本身不含任何假数据实现。",
    );
}
