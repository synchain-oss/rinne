// SPDX-License-Identifier: GPL-3.0-or-later
//
// 外部链接的唯一出处：关于面板里的源代码、官网、联系、第三方许可，以及消息位「查看说明」指向的文档。
//
// 文档链接钉到当前版本的 tag（v<版本>），不指向会变的分支：用户点开看到的就是他手里这一版对应的文字。
// 版本串不合规（开发构建、占位符、空串）时回退到 prod，那是稳定分支，链接不会 404。
// links 检查对「仓内其余位置的 blob 链接必须写 blob/prod/」设了规则，本文件是拼接 tag 链接的唯一例外。
//
// 链接只经桥函数 openExternal 交给系统浏览器，页面本身从不把外部地址写进 href 或 location。
// isAllowedExternal 是原生白名单的前端镜像：先在页面里挡掉明显不会放行的地址，原生侧仍会再判一次。

// 这两个地址是字面量：离线扫描（web-preview/build-static.mjs）的白名单登记了同样的两个源，口径与原生 openExternal 一致。
// 它们只经桥交给原生端用系统浏览器打开，页面从不加载，不是页面请求。
export const REPO_URL = "https://github.com/synchain-oss/rinne";
export const SITE_URL = "https://synchain.ca/";
export const ISSUES_URL = `${REPO_URL}/issues`;
export const CONTACT_MAIL = "contact@synchain.ca";

/** 版本串是否是正式版本号（x.y.z，可带 -rc.1 一类预发布后缀）。 */
const VERSION_RE = /^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$/;

/** 文档链接的 ref：合规版本串给 v<版本>，否则 prod。 */
export function docRef(version) {
    return typeof version === "string" && VERSION_RE.test(version) ? `v${version}` : "prod";
}

/** 仓内文件的 blob 链接。path 不带前导斜杠；带了就去掉，防止拼出双斜杠。 */
export function docLink(ref, path) {
    return `${REPO_URL}/blob/${ref}/${String(path).replace(/^\/+/, "")}`;
}

/** 关于面板的四个链接。 */
export function aboutLinks(version) {
    const ref = docRef(version);
    return {
        source: REPO_URL,
        site: SITE_URL,
        contact: ISSUES_URL,
        notices: docLink(ref, "THIRD-PARTY-NOTICES.md"),
    };
}

/**
 * 前端镜像的白名单：scheme 为 https，host 为 synchain.ca，
 * 或 host 为 github.com 且路径等于 /synchain-oss/rinne 或以 /synchain-oss/rinne/ 开头。
 * 原生侧用同一口径再判一次，这里只是少发一次必被拒的调用。
 */
export function isAllowedExternal(url) {
    let u;
    try {
        u = new URL(String(url));
    } catch {
        return false;
    }
    if (u.protocol !== "https:" || u.username || u.password || u.port) return false;
    if (u.hostname === "synchain.ca") return true;
    return (
        u.hostname === "github.com" &&
        (u.pathname === "/synchain-oss/rinne" || u.pathname.startsWith("/synchain-oss/rinne/"))
    );
}
