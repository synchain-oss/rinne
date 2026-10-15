// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/web.json
//
// shell-fit.js：设计盒「装进窗口」的唯一缩放来源。
//
// 缩放机制由两半组成：页面里的固定设计盒加 CSS zoom，和宿主侧 setSize(设计盒 × 档位)。
// 本模块把「页面缩放多少」从档位数字改成实际视口：
//
//     zoom = min(视口宽 / 设计宽, 视口高 / 设计高)
//
// 档位这条路不变（setUiScale 通知原生 setSize(W×F, H×F)，外加 10 秒确认回退），页面不再自己去读档位
// 数字：窗口变成 W×F 之后，上式自然算出 F。这样档位生效，宿主自作主张改窗口尺寸（部分 DAW 会绕过
// 不可缩放的设置）时也不会超框；两件事共用同一条公式，不需要第二个开关。
//
// 取 min 而不是各轴独立拉伸：设计盒的宽高比是定死的，分轴拉伸会变形。短边贴边、长边留黑边
// （body 已经 flex 居中），是「刚好装进去」的唯一解。
//
// 只写 el.style.zoom，不碰 transform：命中测试（hit.js）与画布后备存储都按 CSS zoom 的语义写，
// 换成 transform 会让 getBoundingClientRect 与布局坐标脱钩。
//
// 旧 rect 语义下缩放锁在 1（WebKit 26.4 之前的系统 WKWebView；判据与理由见 hit.js 的 zoomRectMode）。
// 安装时量一次语义，判出旧语义就：
//   1. 画面倍率恒为 1，不再从视口反算（zoom 为 1 时两种语义相同，命中按构造正确）；
//   2. 档位只留 1 档（scalePresets）；宿主带着别的档位开窗时请回 1（enforceZoomLock），走现有的
//      setUiScale，不新增桥函数，也不落盘：用户存过的档位原样留着，换到新 WebKit 后照旧生效；
//   3. 页面显示一行提示（词条 common.scale.lockedLegacy，各页自己摘 hidden）。
// 新语义以及量不出来的 unknown 下，以上三条一条都不触发。

import { ZOOM_RECT_LEGACY, zoomRectMode } from "./hit.js";

/** 有限数才用，否则回落 d（视口在首帧或隐藏时会读到 0 或 NaN）。 */
function num(v, d) {
    return typeof v === "number" && Number.isFinite(v) ? v : d;
}

/**
 * 「装进窗口」的缩放系数，纯函数，node 里可直接断言。
 *
 * 量化方向是向下取到 1e-4：四舍五入会让「设计宽 × f」比视口大出半个像素，而这半个像素的溢出
 * 正是本模块要根治的。向下取最多少半个像素，不会溢出。
 *
 * 视口或设计盒任一维不是正有限数就回落到 1（首帧或页面隐藏时不能把画面缩成 0）。
 *
 * @param {number} vw 视口宽（CSS px）
 * @param {number} vh 视口高（CSS px）
 * @param {number} boxW 设计盒宽
 * @param {number} boxH 设计盒高
 * @returns {number} 缩放系数，恒大于 0
 */
export function fitFactor(vw, vh, boxW, boxH) {
    const w = num(vw, 0);
    const h = num(vh, 0);
    const bw = num(boxW, 0);
    const bh = num(boxH, 0);
    if (!(w > 0) || !(h > 0) || !(bw > 0) || !(bh > 0)) return 1;
    const f = Math.min(w / bw, h / bh);
    if (!(f > 0) || !Number.isFinite(f)) return 1;
    return Math.floor(f * 1e4) / 1e4;
}

/**
 * 当前页面实际生效的外壳缩放。
 *
 * 做成模块级单例，因为一个文档里只有一个设计盒外壳，这是页面结构本身的不变量。
 * 画布后备存储的倍率（k = 外壳缩放 × dpr）要的是「画面实际被放大了多少」，而不是档位数字：
 * 宿主改窗口时两者会分家，所以读这里。
 *
 * 读方清单不写在注释里：手写的清单会漏，而漏掉的读方没有任何东西会变红。
 * 清单由预览测试里的扫描判据现算。
 */
let current = 1;

/** 画面缩放用这个精值（量化到 1e-4）。 */
export function shellFitFactor() {
    return current;
}

/**
 * 画布后备存储专用的粗量化倍率。
 *
 * 画面倍率必须精，否则就是要修的那半个像素溢出。但后备存储的 k 不必跟着一像素一跳：宿主拖着窗口
 * 边框走时，精值几乎每个像素都是新数，拿它当「后备存储要不要重建」的判据，等于每帧把所有泳道的
 * canvas 重新分配一次。量化到 0.01：1% 的误差在屏幕上看不出来，拖拽整程只会跨过有限几档。
 */
export function backingFitFactor() {
    return Math.round(current * 100) / 100;
}

/**
 * 本文档是否因旧 rect 语义把缩放锁在 1（安装时量定）。
 * 与 current 同为模块级单例，理由相同：一个文档只有一个外壳。
 */
let locked = false;
/** enforceZoomLock 这一趟离开 1 档时，已经为哪个档位值请求过回 1（回到 1 时清空）。 */
let lockRequestedFrom = null;

/** 旧 rect 语义时为 true：缩放锁在 1，档位只剩 1 档，页面该显示那行提示。 */
export function zoomLocked() {
    return locked;
}

/**
 * 档位表过一道锁：锁定时只留 1 档；没锁时原样返回同一个数组，调用方生成的选项与不加锁时逐字相同。
 * @param {number[]} presets design-box.js 里的档位表
 */
export function scalePresets(presets) {
    if (!locked || !Array.isArray(presets)) return presets;
    return presets.filter((f) => f === 1);
}

/**
 * 锁定时，如果 state 回推的档位不是 1（宿主按用户存过的档位开了窗），经 request(1) 请回 1。
 *
 * request 由页面传入（桥在页面手里），约定就是调现有的 setUiScale(1)：只改窗口，不调
 * commitUiScale，所以用户存过的档位不会被改写。每离开 1 档一次只请求一次：渲染每帧都会调到这里，
 * 而回声要晚一拍才到；回推回到 1 时清掉记账，之后再被推到别的档位（比如宿主重新按存档开窗）会再请求
 * 一次。请求本身失败不重试（setUiScale(1) 在档位表内，不会被拒，真失败多半是桥没接上，重试只会刷屏）。
 * 没锁时直接返回 false，不碰任何东西。
 *
 * @param {number} scale 页面此刻手上的档位（state 回推值）
 * @param {(f:number)=>unknown} request 发 setUiScale 的那一下
 * @returns {boolean} 这一次是否发了请求
 */
export function enforceZoomLock(scale, request) {
    if (!locked) return false;
    if (typeof scale !== "number" || !Number.isFinite(scale)) return false;
    if (scale === 1) {
        lockRequestedFrom = null;
        return false;
    }
    if (scale === lockRequestedFrom || typeof request !== "function") {
        return false;
    }
    lockRequestedFrom = scale;
    request(1);
    return true;
}

/**
 * 把 el 钉成「永远刚好装进视口」。
 *
 * @param {object} o
 * @param {HTMLElement} o.el 设计盒外壳元素
 * @param {{w:number,h:number}} o.box 设计盒尺寸（design-box.js）
 * @param {(f:number)=>void} [o.onChange] 倍率变了才回调。安装时那一次不回调：页面通常在文档顶部就装上
 *   本模块，而回调里要访问的对象往往要几十行之后才声明，安装时回调会撞上暂时性死区。首帧的后备存储
 *   由各画布挂载时自己读 backingFitFactor() 得到。
 *   这个钩子不能省：倍率变了之后，没有任何自发路径会去重算后备存储的 k。CSS zoom 改的是计算值，
 *   不改后代元素自己的 clientWidth，所以画布的尺寸测量判不出变化，ResizeObserver 也不会响。
 * @param {Window} [o.win] 注入用，缺省取 globalThis
 * @returns {{factor:()=>number, refresh:()=>void, destroy:()=>void}}
 */
export function installShellFit(o) {
    const el = o && o.el;
    const box = (o && o.box) || {};
    const onChange = typeof (o && o.onChange) === "function" ? o.onChange : null;
    const win = (o && o.win) || globalThis;
    if (!el || !win) {
        return { factor: () => 1, refresh: () => {}, destroy: () => {} };
    }

    // 语义只量这一次（hit.js 按文档缓存）。unknown 按新语义处理：不锁。
    locked = zoomRectMode(win.document) === ZOOM_RECT_LEGACY;
    lockRequestedFrom = null;

    let last = 0;
    let raf = 0;
    let rafIsTimeout = false; // raf 里存的是 timeout 句柄还是 rAF 句柄，destroy 按类型取消

    function viewport() {
        const de = win.document && win.document.documentElement;
        // clientWidth 排除滚动条，是布局视口的准值；拿不到再退到 innerWidth
        return {
            w: num(de && de.clientWidth, 0) || num(win.innerWidth, 0),
            h: num(de && de.clientHeight, 0) || num(win.innerHeight, 0),
        };
    }

    // silent 为 true 表示安装时那一次：只落样式，不回调。
    function apply(silent) {
        raf = 0;
        const v = viewport();
        // 旧 rect 语义下降级而不是换算：画面倍率恒为 1，不随视口变
        const f = locked ? 1 : fitFactor(v.w, v.h, box.w, box.h);
        if (f === last) return;
        last = f;
        current = f;
        // String(1) 是 "1"：档位 1x 时读回的仍是裸 "1"，而不是 "1.0000"
        el.style.zoom = String(f);
        if (onChange && silent !== true) onChange(f);
    }

    function schedule() {
        if (raf) return;
        // 包一层箭头而不是直接交出 apply：rAF 会把时间戳当第一个实参传进来，
        // 而 apply 的第一个形参是 silent，直接传就等于拿时间戳当布尔用
        if (typeof win.requestAnimationFrame === "function") {
            rafIsTimeout = false;
            raf = win.requestAnimationFrame(() => apply(false));
        } else {
            // 回退到 setTimeout 时句柄类型也变了：destroy 必须按类型取消，
            // 否则待跑的 apply 会在 destroy 之后再写一次 style.zoom，并把模块级 current 改回去
            rafIsTimeout = true;
            raf = win.setTimeout(() => apply(false), 0);
        }
    }

    apply(true); // 首帧同步落一次：等 rAF 的话会先闪一帧未缩放的画面

    win.addEventListener("resize", schedule);
    const vv = win.visualViewport || null;
    if (vv) vv.addEventListener("resize", schedule);

    // WebView2 里 resize 事件并非每次窗口尺寸变化都会到（宿主 setBounds 走原生路径），
    // ResizeObserver 观察 documentElement 是兜底。两条都挂只多算一次 apply，
    // 倍率没变时直接返回，不会来回写样式。
    let ro = null;
    if (typeof win.ResizeObserver === "function" && win.document) {
        ro = new win.ResizeObserver(schedule);
        ro.observe(win.document.documentElement);
    }

    return {
        factor: () => last,
        refresh: () => apply(false),
        destroy() {
            win.removeEventListener("resize", schedule);
            if (vv) vv.removeEventListener("resize", schedule);
            if (ro) ro.disconnect();
            if (raf) {
                if (rafIsTimeout) win.clearTimeout(raf);
                else if (typeof win.cancelAnimationFrame === "function") win.cancelAnimationFrame(raf);
            }
            raf = 0;
            // 拆完等于没装过：模块级 current 是给后备存储读的，
            // 留着上一个文档的倍率会让热重挂后的第一批画布按陈旧值分配
            current = 1;
            last = 0;
            locked = false;
            lockRequestedFrom = null;
        },
    };
}
