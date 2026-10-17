// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/web-preview.json
//
// web-preview 壳页：把 mock 后端送进真源页面的 window，并扮演宿主（按设计盒和档位给 iframe 定尺寸）。
//
// 为什么是注入：web/ 是页面代码的唯一真源，web-preview 里不复制任何界面代码，真源页面也不认识预览器。
// 所以壳页用同源 iframe 引用 ../web/<角色>/index.html，再把 mock 挂到 iframe 的 window.__RINNE_MOCK__，
// 真源页面里 createBridge({ role }) 会自己取到它。壳页与 iframe 同源，对象可以跨 realm 直接共享：
// 定时器和事件回调留在壳页，bridge.js 在 iframe 里调用同一个对象的方法。
//
// 时序：真源页面的入口是 module script，恒为 deferred，解析完成之后才求值。注入窗口就是新文档提交到入口模块
// 求值之间的那几毫秒，两手准备：
//   - 导航前先写一次 about:blank 的 window。多数浏览器导航后会换新的全局对象，这一手不作依赖，只当零成本先手；
//   - 重注入泵：赋 src 之后立刻开一条 MessageChannel 任务链（比 setTimeout(0) 快得多），每个 tick 检查
//     iframe 的 window 上有没有 mock，缺了就补，新文档一提交下一个 tick 就补上，此时模块图还在抓取。
//     注入落定、出错或超时就停泵。
// 必须经 serve.ps1 的 http://127.0.0.1 打开；file:// 下 iframe 变跨源，注入不成立。
//
// 页面还没有接桥时（占位页），没有任何事件监听者，工具条显示「页面尚未接桥」，不当作失败，也不重试。

import { DESIGN } from "../web/shared/design-box.js";
import { memoryStorage } from "./mock/bridge-common.js";
import { createPreviewSession } from "./mock/state-driver.js";
import { SCENES } from "./mock/scenarios.js";

/** 真源页面路径：web-preview 对 web/ 的唯一依赖形式是引用，不复制。 */
const TARGET_PAGE = {
    input: "../web/input/index.html",
    output: "../web/output/index.html",
};

/** 夹具页：只给自动化冒烟用，页面自己 createBridge 并把观察到的事件写到 window.__bridgeProbe。 */
const PROBE_PAGE = "./fixtures/common/bridge-probe.html";

/** 泵的硬上限：超时即判失败，免得某天真源改成同步脚本时壳页无声空转。 */
const PUMP_DEADLINE_MS = 15000;

/** 工具条上显示的元信息只收白名单里的值，表外一律显示 unknown，既挡注入面，也让拼错的参数一眼可见。 */
const SCENE_IDS = SCENES.map((s) => s.id);
const LANGS = ["zh", "en", "fr"];

function allowedOr(value, allowed) {
    if (value === null || value === undefined || value === "") return null;
    return allowed.includes(value) ? value : "unknown";
}

/** 往工具条里追加一格 key=value，全程 textContent，不拼 HTML。 */
function appendMeta(parent, key, value) {
    if (value === null) return;
    if (parent.childNodes.length > 0) parent.appendChild(document.createTextNode(" · "));
    parent.appendChild(document.createTextNode(`${key}=`));
    const code = document.createElement("code");
    code.textContent = value;
    parent.appendChild(code);
}

/** 壳页工具条样式：预览家具，不是产品界面，故意不引 tokens.css，一眼能看出它不属于插件画面。 */
const SHELL_CSS = `
html, body { margin: 0; height: 100%; }
body { display: flex; flex-direction: column; background: rgb(18 18 20); color: rgb(238 238 242);
    font: 13px/1.5 system-ui, "Segoe UI", sans-serif; }
.pv-bar { display: flex; flex-wrap: wrap; align-items: center; gap: 12px; padding: 6px 12px;
    background: rgb(32 32 38); border-bottom: 1px solid rgb(72 72 82); }
.pv-bar a { color: rgb(150 202 255); }
.pv-bar button { font: inherit; color: rgb(238 238 242); background: rgb(58 58 68);
    border: 1px solid rgb(104 104 118); border-radius: 4px; padding: 2px 10px; cursor: pointer; }
.pv-bar code { font-family: ui-monospace, Consolas, monospace; }
.pv-status[data-ok="1"] { color: rgb(126 226 150); }
.pv-status[data-ok="0"] { color: rgb(255 146 146); }
.pv-status[data-ok="wait"] { color: rgb(240 214 128); }
.pv-warn { color: rgb(240 214 128); }
.pv-warn:empty { display: none; }
.pv-stage { flex: 1 1 auto; min-height: 0; overflow: auto; }
.pv-stage iframe { display: block; border: 0; background: rgb(18 18 20); }
`;

/**
 * 注入引擎，可独立测试：session 由调用方给。
 * @param {object} o
 * @param {"input"|"output"} o.role
 * @param {HTMLIFrameElement} o.frame 不带 src 的 iframe
 * @param {string} o.targetUrl 真源页面 URL
 * @param {object} o.session { mock, start(), stop(), info }
 * @param {(s:{ok:boolean|null,text:string})=>void} [o.onStatus]
 * @param {(f:number)=>void} [o.hostResize] 档位被接受之后按设计盒 × 档位改 iframe 尺寸，对应原生的 resizeToDesignBox
 * @returns {Promise<object>} 诊断对象
 */
export function injectAndMount({ role, frame, targetUrl, session, onStatus = () => {}, hostResize = null }) {
    // 探针：包一层 addEventListener，数一数页面接了多少事件，判断桥是不是真的接上了。
    let wired = 0;
    const probeMock = Object.create(session.mock);
    probeMock.addEventListener = (name, cb) => {
        wired += 1;
        return session.mock.addEventListener(name, cb);
    };
    // 壳页扮演宿主：档位两件事各有其主，mock 记档位，窗口尺寸由宿主改。拒绝态不改尺寸。
    if (typeof session.mock.setUiScale === "function" && hostResize) {
        probeMock.setUiScale = (f) => {
            const r = session.mock.setUiScale(f);
            Promise.resolve(r)
                .then((res) => {
                    if (!res || res.ok !== false) hostResize(f);
                })
                .catch(() => {});
            return r;
        };
    }

    const diag = {
        role,
        targetUrl,
        wired: 0,
        injectCount: 0,
        committedInject: false,
        pumpTicks: 0,
        ok: false,
        error: null,
    };
    let stopPump = () => {};
    let settle;
    const done = new Promise((r) => {
        settle = r;
    });

    /** 单次注入：窗口上缺 mock 就补；补在目标文档上（不是 about:blank）才算落定。 */
    function tryInject() {
        let w;
        try {
            w = frame.contentWindow;
        } catch (e) {
            diag.error = String(e);
            return true;
        }
        if (!w) return false;
        if (w.__RINNE_MOCK__ === probeMock) return diag.committedInject;
        try {
            w.__RINNE_MOCK__ = probeMock;
            diag.injectCount += 1;
            const url = frame.contentDocument ? frame.contentDocument.URL : "";
            if (url && url !== "about:blank") {
                diag.committedInject = true;
                return true;
            }
        } catch (e) {
            diag.error = String(e);
            return true;
        }
        return false;
    }

    function startPump() {
        const chan = new MessageChannel();
        const deadline = Date.now() + PUMP_DEADLINE_MS;
        let stopped = false;
        chan.port1.onmessage = () => {
            if (stopped) return;
            diag.pumpTicks += 1;
            if (tryInject() || Date.now() > deadline) {
                stopped = true;
                chan.port1.close();
                chan.port2.close();
                return;
            }
            chan.port2.postMessage(0);
        };
        chan.port2.postMessage(0);
        return () => {
            stopped = true;
            try {
                chan.port1.close();
                chan.port2.close();
            } catch {
                // 已经关了
            }
        };
    }

    frame.addEventListener("load", () => {
        let href = "";
        try {
            href = frame.contentDocument ? frame.contentDocument.URL : "";
        } catch (e) {
            diag.error = String(e);
        }
        if (!href || href === "about:blank") return;
        stopPump();
        diag.wired = wired;
        diag.ok = wired > 0;
        if (wired > 0) onStatus({ ok: true, text: `mock 已注入 · 页面接了 ${wired} 个事件` });
        else if (diag.error) onStatus({ ok: false, text: `注入失败：${diag.error}` });
        else onStatus({ ok: null, text: "页面尚未接桥（占位页），mock 已就位" });
        try {
            session.start();
        } catch (e) {
            diag.error = String(e);
            onStatus({ ok: false, text: `启动 mock 抛错：${e}` });
        }
        settle(diag);
    });

    onStatus({ ok: null, text: "正在注入 mock…" });
    try {
        const w0 = frame.contentWindow;
        if (w0) w0.__RINNE_MOCK__ = probeMock;
    } catch (e) {
        diag.error = String(e);
    }
    stopPump = startPump(); // 先开泵再赋 src：导航是异步的，泵必须已经在跑
    frame.src = targetUrl;

    addEventListener("pagehide", () => {
        stopPump();
        try {
            session.stop();
        } catch {
            // 没起过就算了
        }
    });
    return done;
}

/** 浏览器存储，不可用（隐私模式、被禁用）时退回内存存储。 */
function browserStorage() {
    const pick = (name) => {
        try {
            const s = window[name];
            s.setItem("rinne.probe", "1");
            s.removeItem("rinne.probe");
            return s;
        } catch {
            return memoryStorage();
        }
    };
    return { local: pick("localStorage"), session: pick("sessionStorage") };
}

/** 壳页入口：input.html 与 output.html 各自只写 mountPreview({ role })。 */
export async function mountPreview({ role: pageRole }) {
    const style = document.createElement("style");
    style.textContent = SHELL_CSS;
    document.head.appendChild(style);

    const params = new URLSearchParams(location.search);
    const asked = params.get("role");
    const role = asked === "input" || asked === "output" ? asked : pageRole;

    const statusEl = document.querySelector(".pv-status");
    const warnEl = document.querySelector(".pv-warn");
    const metaEl = document.querySelector(".pv-meta");
    const stage = document.querySelector(".pv-stage");
    const setStatus = ({ ok, text }) => {
        statusEl.textContent = text;
        statusEl.setAttribute("data-ok", ok === null ? "wait" : ok ? "1" : "0");
    };

    let session;
    try {
        session = createPreviewSession({ role, params, storage: browserStorage() });
        // 自动化冒烟的走带开关：挂在壳页上，真源页面拿不到，也不知道它存在。
        window.__RINNE_PREVIEW__ = session;
    } catch (e) {
        setStatus({ ok: false, text: `mock 后端不可用：${e.message}` });
        console.error("[web-preview] 建立 mock 会话失败：", e);
        return null;
    }

    const info = session.info;
    metaEl.textContent = "";
    appendMeta(metaEl, "role", role);
    appendMeta(metaEl, "scenario", allowedOr(params.get("scenario"), SCENE_IDS));
    appendMeta(metaEl, "lang", allowedOr(params.get("lang"), LANGS));
    warnEl.textContent = info.warnings.join(" / ");

    // iframe 就是宿主窗口：尺寸是设计盒 × 档位，页面的倍率从视口反算，视口对不上预览就和插件对不上。
    const box = DESIGN[role];
    const frame = document.createElement("iframe");
    frame.title = `Rinne ${role} 预览（真源 web/${role}/index.html）`;
    const sizeFrame = (f) => {
        const s = typeof f === "number" && Number.isFinite(f) && f > 0 ? f : 1;
        frame.style.width = `${Math.round(box.w * s)}px`;
        frame.style.height = `${Math.round(box.h * s)}px`;
    };
    sizeFrame(info.scale ?? session.model.scale);
    stage.appendChild(frame);
    document.querySelector(".pv-reload").addEventListener("click", () => location.reload());

    const target = params.get("page") === "probe" ? `${PROBE_PAGE}?role=${role}` : TARGET_PAGE[role];
    return injectAndMount({ role, frame, targetUrl: target, session, onStatus: setStatus, hostResize: sizeFrame });
}
