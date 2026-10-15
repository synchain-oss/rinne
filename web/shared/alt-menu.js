// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/web.json
//
// 裸 Alt 的「菜单模式」抑制。
//
// 症状：Alt+滚轮用过之后，Ctrl+滚轮概率性失效。机理是 Windows 的老规矩：单独按下再松开 Alt 会激活窗口的
// 菜单栏，进入菜单模式。承载界面的 WebView2 在菜单模式下失去焦点，之后的滚轮消息不再路由给页面。
// Alt+滚轮这类手势天然以裸 Alt 的 keyup 收尾，所以每次都会踩到；「概率性」取决于松开时有没有别的键还按着。
//
// 做法：在文档级拦掉裸 Alt 的默认动作（keydown 与 keyup 都拦；菜单模式由 keyup 触发，但有些宿主在
// keydown 时就先高亮菜单）。只拦裸 Alt，判据见 shouldSuppressAltDefault：
//   - e.key === "Alt"：组合键里的 Alt（Alt+F 之类）key 是那个字母，不受影响；
//   - 带 ctrl / shift / meta 的组合一律放行，那是宿主和系统的地盘；
//   - Alt+F4 的 key 是 "F4"，Alt+Tab 根本到不了页面。
// 插件窗里没有可供 Alt 访问的菜单栏，拦掉它不损失用户能做的事。
//
// 效果只能在 Windows + WebView2 真机上验证：无头浏览器没有菜单栏、页面恒有焦点。
// 无头环境能验的只有「拦谁、不拦谁」这一层，即 shouldSuppressAltDefault。

/**
 * 这一记按键要不要拦掉默认动作（纯函数，node 里可直接断言）。
 *
 * @param {KeyboardEvent|object} e 只读 key / ctrlKey / shiftKey / metaKey
 * @returns {boolean} true 为裸 Alt，应当拦；其余一律放行
 */
export function shouldSuppressAltDefault(e) {
    const ev = e || {};
    if (ev.key !== "Alt") return false;
    return !ev.ctrlKey && !ev.shiftKey && !ev.metaKey;
}

/**
 * 抑制裸 Alt 的菜单模式。
 *
 * 挂在捕获阶段：菜单模式在默认动作阶段由宿主或浏览器发起，越早拦越稳，
 * 也不会被页面里某个调用了 stopPropagation 的控件挡住。
 *
 * @param {Document} doc 目标文档，缺省取全局 document
 * @returns {() => void} 解绑函数（测试用；生产环境挂上就不摘）
 */
export function suppressBareAltMenu(doc) {
    const d = doc || (typeof document !== "undefined" ? document : null);
    if (!d || typeof d.addEventListener !== "function") return () => {};
    const onKey = (e) => {
        if (shouldSuppressAltDefault(e)) e.preventDefault();
    };
    d.addEventListener("keydown", onKey, true);
    d.addEventListener("keyup", onKey, true);
    return () => {
        d.removeEventListener("keydown", onKey, true);
        d.removeEventListener("keyup", onKey, true);
    };
}
