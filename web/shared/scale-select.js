// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/web.json
//
// scale-select.js：页脚缩放下拉的呈现与交互，Input、Output 共用。
//
// 为什么不用原生 <select>：它展开的列表由浏览器或系统自绘，页面样式碰不到。WebView2 下是一块白底系统列表，
// WKWebView 下是 macOS 菜单，和界面的玻璃质感格格不入。所以改成「触发钮 + 自绘浮层」，浮层的样式在 rinne.css 的 .rn-scale*。
//
// 分工：本模块只管呈现与交互，判据与状态机都留在 scale-ui.js 和各页：
//   · 档位表：调用方经 setOptions 传入 scale-ui.js 的 options() 结果（已过旧 WebKit 的锁，锁定时只剩 1 档；
//     放不下当前屏幕的档位带 disabled 与 titleKey）。本模块不读设计盒，也不算屏幕。
//   · 超屏档：灰显、不隐藏，点了不生效，注记用 titleKey 对应的词条（common.scale.overflow）。
//   · 选中之后：调 onPick(f)；10 秒防呆（预览、保存、取消、超时回退）仍是各页自己那一套，本模块不碰桥。
//   · 当前档：调用方在每次渲染时 setValue(f)；当前档在浮层里用勾号标出，同时写 aria-selected。
//
// 交互（ARIA listbox 语义；触发钮 aria-haspopup="listbox" 加 aria-expanded）：
//   · 点触发钮开或关，点外面收起；展开时焦点落在当前档。
//   · 触发钮上按 ↑ 或 ↓ 只展开，不直接换档：换档会立刻改窗口尺寸并起 10 秒倒计时，不该由一次方向键顺手触发。
//     Enter 与 Space 走按钮原生的 click，同样是展开。
//   · 浮层里 ↑ ↓ 逐项移焦点（超屏档也能停，读屏读得到它的注记），Home、End 到两端，
//     Enter 或 Space 选中，Esc 收起并把焦点还给触发钮（这一下 Esc 不再冒泡到页面级的 Esc 处理），Tab 离开即收起。
//   · 带 Ctrl、Cmd、Alt 的组合键一律放行，不吞：那是宿主与系统的地盘。
//   · 点的正是当前档就只收起，不发预览（与原生下拉同值不派发 change 一致）。
//   · 触发钮被置灰（首帧前、旧 WebKit 锁定）时打不开；已经开着时置灰会把它收起。
//
// 浮层定位：默认方向由 placement 给（页脚向上展开）。展开时量一次可用空间，量到最近的裁切祖先（overflow 不是 visible，
// 外壳 .sc-shell 就是一个）与视口为止：默认方向放不下而另一边放得下就翻过去，两边都放不下就压 max-height，改成内部滚动。
// 水平方向同理：按 align 贴齐触发钮的一侧，越界就换另一侧。
// 所有包围盒都在同一个坐标系里比较（rect 对 rect），换算成 CSS px 用的是量出来的倍率（触发钮 rect 高 ÷ offsetHeight）：
// 新语义（rect 含 zoom）下它就是外壳倍率；旧语义（rect 不含 zoom）下外壳锁在 1，量出来也是 1，两种语义都不需要分支。

import { el } from "./dom.js";
import { icon } from "./icons.js";

const PLACEMENTS = ["up", "down"];
const ALIGNS = ["start", "end"];
/** 浮层与触发钮之间的间距（CSS px），等于 rinne.css 里 .rn-scale__panel 的 calc(100% + var(--sp-9))。 */
const PANEL_GAP_PX = 9;
/** 浮层离裁切边缘至少留这么多（CSS px），免得描边贴死在外壳圆角上。 */
const EDGE_PX = 4;
/** 两边都放不下时 max-height 的下限（CSS px）：至少露出两三项，别压成一条缝。 */
const MIN_PANEL_PX = 72;
/** 没有可显示的档位时触发钮上写这个。 */
const NO_VALUE = "—";

const hasOwn = (o, k) => !!o && Object.prototype.hasOwnProperty.call(o, k);
const isModified = (e) => !!(e.ctrlKey || e.metaKey || e.altKey);

/**
 * 当前缩放倍率（元素所在坐标系到视口）= rect 高 ÷ offsetHeight。
 * 量不出来（未布局、0 高）就按 1 处理：失效方向是「按未缩放算空间」，最多让浮层多压一点。
 */
export function measuredZoom(node) {
    if (!node || typeof node.getBoundingClientRect !== "function") return 1;
    const h = node.offsetHeight;
    const r = node.getBoundingClientRect();
    if (!(h > 0) || !(r && r.height > 0)) return 1;
    const k = r.height / h;
    return Number.isFinite(k) && k > 0 ? k : 1;
}

/**
 * 纯几何（node 里可直接断言）：给出触发钮与裁切框（同一坐标系的 top、bottom）、浮层的自然高度与倍率，
 * 回答「往哪边开、要不要压高度」。
 *   · 默认方向放得下就用它；
 *   · 放不下而另一边放得下就翻过去；
 *   · 两边都放不下就取更宽的一边，maxHeight 等于那一边的空间（不低于 MIN_PANEL_PX）。
 * @returns {{ placement: "up"|"down", maxHeight: number|null }} maxHeight 为 CSS px；null 表示不压
 */
export function choosePlacement(prefer, trig, clip, naturalPx, zoom) {
    const k = Number.isFinite(zoom) && zoom > 0 ? zoom : 1;
    const first = PLACEMENTS.indexOf(prefer) >= 0 ? prefer : "up";
    const other = first === "up" ? "down" : "up";
    const room = {
        up: (trig.top - clip.top) / k - PANEL_GAP_PX - EDGE_PX,
        down: (clip.bottom - trig.bottom) / k - PANEL_GAP_PX - EDGE_PX,
    };
    const need = Number.isFinite(naturalPx) ? naturalPx : 0;
    if (need <= room[first]) return { placement: first, maxHeight: null };
    if (need <= room[other]) return { placement: other, maxHeight: null };
    const side = room[other] > room[first] ? other : first;
    return { placement: side, maxHeight: Math.max(MIN_PANEL_PX, Math.floor(room[side])) };
}

/** 从 node 往上找所有会裁切的祖先（overflow 不是 visible），与视口求交。坐标为视口坐标。 */
function clipBox(node) {
    const win = (node.ownerDocument && node.ownerDocument.defaultView) || window;
    let box = { top: 0, bottom: win.innerHeight, left: 0, right: win.innerWidth };
    for (let n = node.parentElement; n; n = n.parentElement) {
        const cs = win.getComputedStyle(n);
        const clips = /(hidden|auto|scroll|clip)/.test(cs.overflowY || "") || /(hidden|auto|scroll|clip)/.test(cs.overflowX || "");
        if (!clips) continue;
        const r = n.getBoundingClientRect();
        box = {
            top: Math.max(box.top, r.top),
            bottom: Math.min(box.bottom, r.bottom),
            left: Math.max(box.left, r.left),
            right: Math.min(box.right, r.right),
        };
    }
    return box;
}

/**
 * 装一个缩放下拉。调用方先备好三件 DOM（页脚组件里建）：
 *   wrap（根节点，data-open、data-placement、data-align 写在它身上）> trigger（button）和 panel（浮层）。
 *
 * @param {object} o
 * @param {HTMLElement} o.wrap
 * @param {HTMLButtonElement} o.trigger 触发钮，里面的文字节点交给 o.label
 * @param {HTMLElement} [o.label] 触发钮上显示当前档的那段文字
 * @param {HTMLElement} o.panel 浮层，档位项由本模块生成
 * @param {(f:number)=>void} o.onPick 用户选中了一档（不是超屏档，也不是当前档）
 * @param {(open:boolean)=>void} [o.onOpenChange] 开合变了就回调（页脚据此改 data-state）
 * @param {"up"|"down"} [o.placement] 默认展开方向，缺省向上
 * @param {"start"|"end"} [o.align] 默认水平贴齐，start 是左缘对齐触发钮，end 是右缘；缺省 end
 * @param {(key:string)=>string} [o.t] 取词条（超屏注记）
 */
export function createScaleSelect(o) {
    const opts = o || {};
    const { wrap, trigger, panel } = opts;
    if (!wrap || !trigger || !panel) return null;
    const label = opts.label || null;
    const doc = wrap.ownerDocument || document;
    const onPick = typeof opts.onPick === "function" ? opts.onPick : () => {};
    const onOpenChange = typeof opts.onOpenChange === "function" ? opts.onOpenChange : () => {};
    const prefer = PLACEMENTS.indexOf(opts.placement) >= 0 ? opts.placement : "up";
    const align = ALIGNS.indexOf(opts.align) >= 0 ? opts.align : "end";
    let t = typeof opts.t === "function" ? opts.t : (k) => k;
    let value = 0;
    let options = [];
    let items = [];
    let sig = "";

    wrap.setAttribute("data-open", "0");
    wrap.setAttribute("data-placement", prefer);
    wrap.setAttribute("data-align", align);
    trigger.setAttribute("aria-haspopup", "listbox");
    trigger.setAttribute("aria-expanded", "false");
    panel.setAttribute("role", "listbox");

    const isOpen = () => wrap.getAttribute("data-open") === "1";

    function valueLabel() {
        const hit = options.find((x) => x.factor === value);
        if (hit) return hit.label;
        return value > 0 ? Math.round(value * 100) + "%" : NO_VALUE;
    }

    function render() {
        if (label) label.textContent = valueLabel();
        for (const it of items) {
            const cur = it.opt.factor === value;
            it.el.setAttribute("data-current", cur ? "1" : "0");
            it.el.setAttribute("aria-selected", cur ? "true" : "false");
            const note = it.opt.disabled && it.opt.titleKey ? t(it.opt.titleKey) : "";
            it.note.textContent = note;
            it.note.hidden = note === "";
        }
    }

    function buildItems() {
        items = options.map((opt) => {
            const chk = el("span", { class: "rn-scale__chk", aria: { hidden: "true" } }, icon("check", 12, "currentColor"));
            const num = el("span", { class: "rn-scale__num", text: opt.label });
            const note = el("span", { class: "rn-scale__note" });
            const b = el(
                "button",
                {
                    type: "button",
                    class: "rn-scale__opt",
                    role: "option",
                    tabindex: "-1",
                    "aria-selected": "false",
                    "data-scale": String(opt.factor),
                    "data-current": "0",
                    "data-over": opt.disabled ? "1" : "0",
                },
                chk,
                num,
                note,
            );
            if (opt.disabled) b.setAttribute("aria-disabled", "true");
            return { opt, el: b, note };
        });
        panel.replaceChildren(...items.map((it) => it.el));
    }

    function place() {
        wrap.setAttribute("data-placement", prefer);
        wrap.setAttribute("data-align", align);
        panel.style.maxHeight = "";
        const k = measuredZoom(trigger);
        const clip = clipBox(wrap);
        const decision = choosePlacement(prefer, trigger.getBoundingClientRect(), clip, panel.scrollHeight, k);
        wrap.setAttribute("data-placement", decision.placement);
        if (decision.maxHeight !== null) panel.style.maxHeight = decision.maxHeight + "px";
        // 水平：贴齐的那一侧越出裁切框就换另一侧；两侧都越界时保持默认，不来回翻
        const pr = panel.getBoundingClientRect();
        if (align === "end" && pr.left < clip.left) wrap.setAttribute("data-align", "start");
        else if (align === "start" && pr.right > clip.right) wrap.setAttribute("data-align", "end");
    }

    function ensureVisible(node) {
        if (!node) return;
        // 两端的项连同浮层的内边距一起滚进来（offsetTop 里含着上内边距，只按它算会让第一项上方的内边距藏在滚动区之外）
        if (node === (items[0] && items[0].el)) {
            panel.scrollTop = 0;
            return;
        }
        if (node === (items[items.length - 1] && items[items.length - 1].el)) {
            panel.scrollTop = panel.scrollHeight;
            return;
        }
        const top = node.offsetTop;
        const bottom = top + node.offsetHeight;
        if (top < panel.scrollTop) panel.scrollTop = top;
        else if (bottom > panel.scrollTop + panel.clientHeight) panel.scrollTop = bottom - panel.clientHeight;
    }

    function focusItem(i) {
        const it = items[i];
        if (!it) return;
        it.el.focus({ preventScroll: true });
        ensureVisible(it.el);
    }

    function currentIndex() {
        const i = items.findIndex((x) => x.opt.factor === value);
        return i < 0 ? 0 : i;
    }

    /** focusTarget 为 "trigger" 时，收起后把焦点还给触发钮（展开时焦点在浮层里，浮层一收它就无处可落）。 */
    function setOpen(open, focusTarget) {
        const was = isOpen();
        if (open && (trigger.disabled || items.length === 0)) return;
        if (open === was) return;
        wrap.setAttribute("data-open", open ? "1" : "0");
        trigger.setAttribute("aria-expanded", open ? "true" : "false");
        if (open) {
            place();
            focusItem(currentIndex());
        } else if (focusTarget === "trigger") {
            trigger.focus({ preventScroll: true });
        }
        onOpenChange(open);
    }

    function pick(it) {
        setOpen(false, "trigger");
        // 超屏档灰显但不隐藏，选它不生效
        if (it.opt.disabled) return;
        // 选的就是当前档：只收起，不发预览
        if (it.opt.factor === value) return;
        onPick(it.opt.factor);
    }

    trigger.addEventListener("click", () => setOpen(!isOpen(), "trigger"));
    trigger.addEventListener("keydown", (e) => {
        if (isModified(e)) return;
        if (e.key !== "ArrowDown" && e.key !== "ArrowUp") return;
        e.preventDefault();
        if (!isOpen()) setOpen(true);
        else focusItem(currentIndex());
    });

    panel.addEventListener("click", (e) => {
        const btn = e.target instanceof Element ? e.target.closest("[data-scale]") : null;
        if (!btn) return;
        const it = items.find((x) => x.el === btn);
        if (it) pick(it);
    });

    panel.addEventListener("keydown", (e) => {
        if (isModified(e)) return;
        const at = items.findIndex((x) => x.el === doc.activeElement);
        const last = items.length - 1;
        let next = -1;
        if (e.key === "ArrowDown") next = at < 0 ? currentIndex() : Math.min(last, at + 1);
        else if (e.key === "ArrowUp") next = at < 0 ? currentIndex() : Math.max(0, at - 1);
        else if (e.key === "Home") next = 0;
        else if (e.key === "End") next = last;
        if (next >= 0) {
            e.preventDefault();
            focusItem(next);
            return;
        }
        if (e.key === "Enter" || e.key === " ") {
            // 自己接，不等按钮原生的 click：空格的原生激活落在 keyup 上，那时焦点已经回到触发钮，会把刚收起的浮层又点开
            e.preventDefault();
            if (at >= 0) pick(items[at]);
            return;
        }
        if (e.key === "Escape") {
            // 这一下 Esc 只归浮层，不冒泡到页面级的 Esc 处理（那里会顺带关别的叠层）
            e.preventDefault();
            e.stopPropagation();
            setOpen(false, "trigger");
        }
        // Tab 不拦，焦点自然离开，下面的 focusout 收起浮层
    });

    // 在下拉里按下鼠标不挪焦点（菜单类控件的通行做法）。不拦的话 WKWebView 上会出事：WebKit 的按钮不接受鼠标焦点，
    // 按下档位项时焦点会从当前档掉到 body，下面的 focusout 判「焦点离开了」先把浮层收起，随后那一下 click 就落空；
    // 按触发钮去收起时同理，会先收起再被 click 重新打开。click 本身不受影响，只拦 mousedown 的默认动作。
    wrap.addEventListener("mousedown", (e) => e.preventDefault());

    // 焦点离开下拉（Tab 走了、别处抢了焦点）就收起。判据看落定之后的 activeElement，不看 relatedTarget：
    // Tab 出了 WebView 的最后一个控件时 relatedTarget 是 null，而那正是「离开」。窗口整体失焦时文档里的
    // activeElement 不变，所以不收（切回来浮层还在）。
    wrap.addEventListener("focusout", () => {
        setTimeout(() => {
            if (isOpen() && !wrap.contains(doc.activeElement)) setOpen(false);
        }, 0);
    });
    const onDocPointerDown = (e) => {
        if (!isOpen()) return;
        if (e.target instanceof Node && wrap.contains(e.target)) return;
        setOpen(false);
    };
    doc.addEventListener("pointerdown", onDocPointerDown);

    // 触发钮被置灰（页面在首帧前、旧 WebKit 锁定时直接写 disabled）时，已经开着的浮层要跟着收起
    let watcher = null;
    if (typeof MutationObserver === "function") {
        watcher = new MutationObserver(() => {
            if (trigger.disabled && isOpen()) setOpen(false);
        });
        watcher.observe(trigger, { attributes: true, attributeFilter: ["disabled"] });
    }

    return {
        /**
         * 换档位表（scale-ui.js 的 options() 结果）。签名没变就不重建：页面每秒会重发好几次同样的状态，
         * 用户正开着浮层、用键盘选档时重建会让焦点丢掉。
         * @param {Array<{factor:number,label:string,disabled:boolean,titleKey:string|null}>} list
         */
        setOptions(list) {
            const next = (Array.isArray(list) ? list : []).map((x) => ({
                factor: Number(x.factor),
                label: String(x.label),
                disabled: x.disabled === true,
                titleKey: x.titleKey || null,
            }));
            const nextSig = JSON.stringify(next);
            if (nextSig === sig) return;
            sig = nextSig;
            const open = isOpen();
            const focusedFactor = open ? (items.find((x) => x.el === doc.activeElement) || { opt: {} }).opt.factor : undefined;
            options = next;
            buildItems();
            render();
            if (open) {
                if (items.length === 0) {
                    setOpen(false);
                    return;
                }
                place();
                const i = items.findIndex((x) => x.opt.factor === focusedFactor);
                if (i >= 0) focusItem(i);
            }
        },
        /** 状态回推：当前档。 */
        setValue(f) {
            const v = Number(f);
            value = Number.isFinite(v) && v > 0 ? v : 0;
            render();
        },
        value: () => value,
        /** 换取词条函数（切语言时），重写超屏注记。 */
        setT(fn) {
            if (typeof fn === "function") t = fn;
            render();
        },
        /** 触发钮上显示的当前档文字。 */
        labelText: valueLabel,
        setOpen: (open) => setOpen(!!open),
        isOpen,
        /** 摘掉文档级监听（页面卸载时）。 */
        destroy() {
            doc.removeEventListener("pointerdown", onDocPointerDown);
            if (watcher) watcher.disconnect();
        },
    };
}
