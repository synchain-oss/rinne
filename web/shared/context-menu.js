// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/web.json
//
// 原生右键菜单抑制。
//
// 插件窗里冒出 WebView 的浏览器菜单（重新加载、另存为、查看网页源代码）是穿帮：用户看到的应该是一个
// 插件，不是一个网页。
//
// 为什么落在 JS 这一层：WebView2 有 ICoreWebView2Settings::put_AreDefaultContextMenusEnabled，但 JUCE 8
// 没有把它暴露出来（它只设了开发者工具、状态栏、内置错误页和 User-Agent 四项，后端选项里也没有对应开关）。
// 走 C++ 就得改 JUCE 源码，那是不接受的依赖分叉。JS 这一层反而更干净：一个监听器，两个插件通用，
// 预览页里也能照样验证。
//
// 可编辑控件例外：输入框上的右键菜单是剪切、复制、粘贴的唯一入口，一刀切会把粘贴堵死，
// 所以只在非可编辑目标上拦。

/**
 * 文本族放行选择器，只挑真的能编辑文本的控件。
 *
 * 裸 input 不分 type 是错的：type="range" 是滑杆，checkbox、radio、button、color 同理，
 * 这些控件上的右键菜单没有「粘贴」可言，放行它们等于在插件里露出浏览器菜单。
 * 所以用白名单：只放文本族的 input，加 textarea 与 contenteditable。
 *
 * select 不在这一份里：下拉没有文本撤销语义。右键这一侧仍放行它，理由与文本无关，见 EDITABLE_SELECTOR。
 * 文本族的单一真源就是这一份常量，两个消费面都从它派生，不要再抄第二份。
 */
export const EDITABLE_TEXT_SELECTOR = [
    'input[type="text"]',
    'input[type="search"]',
    'input[type="url"]',
    'input[type="tel"]',
    'input[type="email"]',
    'input[type="password"]',
    'input[type="number"]',
    "input:not([type])", // 缺省 type 即 text
    "textarea",
    '[contenteditable=""]',
    '[contenteditable="true"]',
    '[contenteditable="plaintext-only"]',
].join(", ");

/**
 * 右键菜单放行选择器 = 文本族 + select。
 *
 * 多出来的 select 是右键这一侧独有的理由：下拉的右键菜单归宿主系统，拦了反而奇怪。
 * 它与「这里能不能撤销文本」无关，所以 Ctrl+Z 那一侧不用本常量，而是用 EDITABLE_TEXT_SELECTOR。
 * 两处用途正当不同，不是其中一处写错。
 */
export const EDITABLE_SELECTOR = EDITABLE_TEXT_SELECTOR + ", select";

/**
 * 目标是不是可编辑文本控件（Ctrl+Z 要放行的那一类）。
 *
 * 用 closest() 而不是只看 target 自己：contenteditable 容器里的行内元素才是真正的事件目标，
 * 只比对 tagName 会漏。而且只比 tagName 会把 type="range" 的滑杆和勾选框当成文本框，
 * 焦点停在滑杆上按 Ctrl+Z 时事件不被拦截，直接漏给宿主的撤销栈。
 * contenteditable="false" 不放行：那是显式声明「这块不可编辑」，而裸 [contenteditable] 属性选择器
 * 会把它一起放过去。
 *
 * 与按 tagName 判断相比有两处极窄的语义差，只记录不处理：
 *   1. 属性选择器的值匹配区分大小写，而 isContentEditable 是浏览器算出的布尔，写成
 *      contenteditable="TRUE" 时旧写法放行、本写法不放行。规范写法为小写，仓内也没有这个属性的实例。
 *   2. isContentEditable 会沿继承算出「在可编辑容器内」，本函数靠 closest() 往上找，
 *      两者在本仓的 DOM 形态下结论相同。
 * 真出现了大写写法，该修的是那份 HTML，而不是把判据放宽回去。
 */
export function isEditableTextTarget(el) {
    if (!el || typeof el.closest !== "function") return false;
    return !!el.closest(EDITABLE_TEXT_SELECTOR);
}

/**
 * 目标是不是可编辑控件（右键要放行的那一类：文本族加 select）。
 */
export function isEditableTarget(el) {
    if (!el || typeof el.closest !== "function") return false;
    return !!el.closest(EDITABLE_SELECTOR);
}

/**
 * 抑制原生右键菜单（可编辑控件除外）。
 *
 * @param {Document} doc 目标文档，缺省取全局 document
 * @returns {() => void} 解绑函数（测试用；生产环境挂上就不摘）
 */
export function disableNativeContextMenu(doc) {
    const d = doc || (typeof document !== "undefined" ? document : null);
    if (!d || typeof d.addEventListener !== "function") return () => {};
    const onMenu = (e) => {
        if (isEditableTarget(e.target)) return;
        e.preventDefault();
    };
    d.addEventListener("contextmenu", onMenu);
    return () => d.removeEventListener("contextmenu", onMenu);
}
