// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/web.json
//
// 首启语言选择卡。
//
// 职责边界：
//   - 首启链的最前一环：引导页会显示时（工程里 ui.guide_seen 为 false）先显示本卡，用户选中语言后关闭本卡，
//     再露出各页自己的下一环。
//   - 两个插件共用一件，而不是各写一份：判据字段（工程里的 ui.guide_seen 与快照里的 guide_seen_global）
//     两侧逐字相同，视觉与文案（common.langStart.*）也是同一份；差异只在选完之后露出谁，
//     那一段在各页 app.js 的 onPick 里。
//   - 不属于引导步骤机：不写 state，不触引擎，不调任何桥函数；语言切换由 app.js 的 setLang 机制承担，
//     沿用既有的语言持久化，不新增 state 键。
//   - 两段导出：纯函数（没有 DOM，node 里可直接 import 断言）和 createLangStart(opts)（DOM 接线）。
//     模块顶层没有副作用，也不触碰 document。
//   - 文案走 data-t（applyI18n 统一刷新）：标题三语常显，三个按钮各用各自的语言（三语值相同）。
//   - 点击契约：只有三个语言按钮可点（点击即切换语言并关卡）；卡片其余区域与背景蒙版不挂任何点击处理器，
//     点击一律无动作、绝不推进。「点击任意处进入下一步」的规则只从引导第 1 步起生效，语言卡阶段不存在。

/** 可选语言（与 i18n.js 的 LANGS 同序；各按钮用各自的语言显示）。 */
export const LANG_PICK_CODES = Object.freeze(["zh", "en", "fr"]);

/**
 * 语言选择卡是否该显示：
 *   - 首帧未到（没有快照），或本会话已关过引导页、已选过语言，都不显示；
 *   - 工程 ui.guide_seen 为 false 且系统级 guide_seen_global 为 false 时显示（首启最前）。
 * 「选过就不再问」要有持久化判据：只靠会话内的标志，会随 WebView 一起销毁，于是每次开窗都重新问。
 * ui.lang_chosen 随工程走，lang_chosen_global 跨工程走，任一为真即不再出卡。
 * @param {object} state    工程 state 子树（含 ui.guide_seen）
 * @param {object} snapshot 状态快照（含 guide_seen_global）
 * @param {boolean} closedThisSession 本会话是否已关过引导页
 * @param {boolean} langChosen        本会话是否已选过语言
 */
export function shouldShowLangStart(state, snapshot, closedThisSession, langChosen) {
    if (closedThisSession || langChosen) return false;
    if (!snapshot) return false;
    const ui = (state && state.ui) || {};
    if (ui.lang_chosen === true || snapshot.lang_chosen_global === true) return false;
    return ui.guide_seen === false && snapshot.guide_seen_global === false;
}

/**
 * @param {{
 *   root: Document|Element,
 *   card: Element,             // 设计盒外壳（overlay 的挂载点，position:relative）
 *   onPick: (code:string) => void,  // 用户选中语言（页面切语言、关卡、落会话标记）
 * }} opts
 */
export function createLangStart(opts) {
    const root = opts.root;
    const card = opts.card;
    const onPick = opts.onPick || (() => {});

    let overlay = null;
    let panel = null;

    const STYLE_ID = "rinne-lang-start-style";
    function ensureStyle() {
        if (root.getElementById(STYLE_ID)) return;
        const style = document.createElement("style");
        style.id = STYLE_ID;
        // 居中大卡：蒙版复用深色玻璃令牌，面板复用模态玻璃样式，只补宽度与按钮排布。
        // z-index 200 压在引导页（100）之上、引导步骤层（1000）之下。
        style.textContent = [
            ".lang-start { position: absolute; inset: 0; z-index: 200; display: flex; align-items: center; justify-content: center; background: var(--dark-scrim); -webkit-backdrop-filter: var(--dark-scrim-blur); backdrop-filter: var(--dark-scrim-blur); }",
            ".lang-start__panel { width: 400px; max-width: calc(100% - 2 * var(--sp-24)); text-align: center; }",
            ".lang-start__title { margin: 0 0 var(--sp-16); }",
            ".lang-start__btns { display: flex; flex-direction: column; gap: var(--sp-10); }",
            ".lang-start__btn { width: 100%; padding: var(--sp-8) var(--sp-11); font-size: var(--fs-140); font-weight: 600; color: var(--txt-dark-1); }",
        ].join("\n");
        root.head.appendChild(style);
    }

    function el(tag, cls) {
        const n = document.createElement(tag);
        if (cls) n.className = cls;
        return n;
    }

    function buildDom() {
        overlay = el("div", "lang-start");
        overlay.setAttribute("data-gb", "lang-start");
        overlay.hidden = true;

        panel = el("div", "sc-modal lang-start__panel");
        panel.setAttribute("role", "dialog");
        panel.setAttribute("aria-modal", "true");
        panel.setAttribute("tabindex", "-1");
        const titleEl = el("p", "sc-modal__title lang-start__title");
        titleEl.id = "lang-start-title";
        titleEl.setAttribute("data-t", "common.langStart.title");
        panel.setAttribute("aria-labelledby", "lang-start-title");

        // 只有这三个按钮挂 click；蒙版和卡片都没有点击处理器，
        // 所以点卡外或卡内非按钮区一律无动作、绝不推进。
        const btns = el("div", "lang-start__btns");
        for (const code of LANG_PICK_CODES) {
            const b = el("button", "sc-btn lang-start__btn");
            b.type = "button";
            // 用 data-lang-pick 而不是 data-lang：避免命中页面里 [data-lang] 的全局切语言绑定，防止双触发
            b.setAttribute("data-lang-pick", code);
            b.setAttribute("data-t", "common.langStart." + code);
            b.addEventListener("click", () => onPick(code));
            btns.appendChild(b);
        }

        panel.append(titleEl, btns);
        overlay.appendChild(panel);
        card.appendChild(overlay);
    }

    function setShown(shown) {
        if (!overlay) return;
        const was = !overlay.hidden;
        overlay.hidden = !shown;
        if (shown && !was) {
            // 语言卡激活期间把背景其余内容设为 inert，避免 Tab 逃出卡片去触发真实的桥调用
            for (const child of card.children) {
                if (child !== overlay) child.setAttribute("inert", "");
            }
            if (panel) panel.focus({ preventScroll: true });
        } else if (!shown && was) {
            for (const child of card.children) {
                child.removeAttribute("inert");
            }
        }
    }

    function mount() {
        ensureStyle();
        buildDom();
    }

    return {
        mount,
        setShown,
        isShown: () => (overlay ? !overlay.hidden : false),
    };
}
