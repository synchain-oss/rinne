// SPDX-License-Identifier: GPL-3.0-or-later
//
// 关于与许可面板：深色模态。Input 宽 480，放在左两栏（x 从 32 到 512，不盖右侧安全栏）；Output 宽 432，几乎占满窗口。
//
// 内容自上而下：
//   主标（Synchain 主标的形状，CSS mask 着色）+ 字标、标题、产品名、构建行；
//   正文：名字来历、许可短口径与「查看许可证」、字体与商标声明、四个链接、无 GitHub 账号时的邮箱、
//        诊断说明与诊断预览、高级设置；
//   底栏：复制诊断信息、查看使用规则、关闭。
//
// 诊断预览：打开面板时先调 copyDiagnostics(false) 取文本放进可滚动预览区，用户看过、点「复制诊断信息」
// 才调 copyDiagnostics(true) 写剪贴板。文本里有 8 位散列，它是指纹不是匿名化，所以必须让用户先看到再决定。
// 复制成功按钮变成「已复制」，失败在底栏左侧用红字写出原因；成败都回调给页面（页面决定要不要弹 toast）。
// 失败原因只显示登记过的几种说法：桥给的原因码查表换成本语言的短语，查不到（含异常消息、系统错误文字）一律写「原因不明」，
// 原始文字不上屏也不传给页面：它可能带路径、用户名或其他语言，面板里不该出现。
//
// 高级设置：起播清空环路与 tail 模式随工程走（桥函数 setResetOnPlay、setTailMode，只影响本实例），
// 诊断日志开关是本机设置（setDiagLog，写全局位，不进工程）。三项各自是否出现由 features 决定：
// 起播清空只有 Input 有；tail 行当前只有 Input 的桥提供函数。
// 行下面那句备注点名了 tail 模式，所以和 tail 行共用同一个开关：tail 行不渲染时备注也不渲染，免得指向看不见的控件。
//
// 链接只经桥函数 openExternal 交给系统浏览器：元素是 role=link 的按钮，不是带 href 的 <a>，
// 页面里不出现任何外部地址的赋值；地址表在 links.js。

import { el } from "./dom.js";
import { icon } from "./icons.js";
import { createModal } from "./modal.js";
import { BUTTON_KEYS } from "./common-keys.js";
import { SYNCHAIN_MARK_URL } from "./synchain-mark.js";
import { aboutLinks, isAllowedExternal } from "./links.js";

/** 「已复制」状态停留多久（毫秒）。 */
export const COPIED_MS = 2500;

/** 桥给的失败原因码 → 失败原因词条。 */
export const COPY_FAIL_KEYS = Object.freeze({
    clipboard: "about.copyFail.clipboard",
    "not-ready": "about.copyFail.notReady",
});

/** 原因码不认识时的词条。 */
export const COPY_FAIL_UNKNOWN = "about.copyFail.unknown";

/** 失败原因码换成词条键：只认登记过的码，其他一律退回「原因不明」。 */
export function copyFailKey(code) {
    return (
        (typeof code === "string" && Object.hasOwn(COPY_FAIL_KEYS, code) && COPY_FAIL_KEYS[code]) || COPY_FAIL_UNKNOWN
    );
}

const TAIL_MODES = Object.freeze([
    { mode: "auto", key: "about.adv.tail.auto" },
    { mode: "infinite", key: "about.adv.tail.infinite" },
    { mode: "30s", key: "about.adv.tail.s30" },
]);

const LINKS = Object.freeze([
    { name: "source", key: "about.link.source" },
    { name: "site", key: "about.link.site" },
    { name: "contact", key: "about.link.contact" },
    { name: "notices", key: "about.link.notices" },
]);

/** 面板几何：Input 放左两栏，Output 几乎占满窗口。 */
const GEOMETRY = Object.freeze({
    input: { x: 32, y: 16, width: 480, height: 552 },
    output: { x: 14, y: 12, width: 432, height: 340 },
});

/** 角色名（构建行里的 {role}）取品牌字的词条。 */
const ROLE_NAME_KEY = Object.freeze({ input: "in.brand.title", output: "out.brand.title" });

/**
 * @param {object} o
 * @param {"input"|"output"} o.role
 * @param {HTMLElement} o.host 外壳的 top 层
 * @param {HTMLElement[]} [o.inert] 打开时设为 inert 的节点（通常是外壳的 content 层）
 * @param {object} o.bridge 要有 copyDiagnostics、openExternal、showLicense、setDiagLog，按 features 还要 setResetOnPlay、setTailMode
 * @param {{resetOnPlay?:boolean, tail?:boolean}} [o.features]
 * @param {()=>void} [o.onViewRules] 点「查看使用规则」
 * @param {()=>void} [o.onCopied] 复制成功
 * @param {(reason:string)=>void} [o.onCopyFail] 复制失败；参数是已按当前语言取好的失败原因短语，不是原始原因码
 * @param {()=>void} [o.onClose]
 * @param {{setTimeout:Function, clearTimeout:Function}} [o.timers]
 */
export function createAbout(o) {
    const { role, bridge } = o;
    const geo = GEOMETRY[role];
    if (!geo) throw new RangeError(`createAbout：未知角色 ${role}`);
    const features = { resetOnPlay: role === "input", tail: role === "input", ...(o.features || {}) };
    const timers = o.timers || globalThis;
    const isIn = role === "input";

    let view = { t: (k) => k, lang: "zh", build: {}, cfg: {}, diagLog: false };
    let copyState = "open"; // open、copied、copyFail
    let copyFailCode = ""; // 桥给的原因码，上屏时按当前语言换成短语
    let copiedTimer = 0;

    const modal = createModal({
        host: o.host,
        gb: `${role}.about`,
        width: geo.width,
        height: geo.height,
        x: geo.x,
        y: geo.y,
        inert: o.inert,
        onEsc: () => close(),
        cls: "rn-about",
    });
    modal.panel.setAttribute("aria-labelledby", `rn-about-title-${role}`);

    // ---- 头部
    const mark = el("span", { class: "rn-about__mark", aria: { hidden: "true" }, data: { part: "synchain-mark" } });
    mark.style.setProperty("--rn-mark", SYNCHAIN_MARK_URL);
    const lockup = el("span", { class: "rn-about__lockup" });
    const title = el("div", { class: "rn-about__title", id: `rn-about-title-${role}` });
    const product = el("div", { class: "rn-about__product" });
    const build = el("div", { class: "rn-about__build" });
    const head = el(
        "div",
        { class: "rn-about__head" },
        el("div", { class: "rn-about__brandRow" }, mark, lockup),
        title,
        product,
        build,
    );

    // ---- 正文
    const origin = el("div", { class: "rn-about__text" });
    const license = el("div", { class: "rn-about__text" });
    const viewLicense = el("button", {
        type: "button",
        class: "sc-btn sc-btn--dark rn-btn",
        data: { glow: "1", part: "about.viewLicense" },
        on: { click: () => bridge.showLicense() },
    });
    const fonts = el("div", { class: "rn-about__small" });
    const tm = el("div", { class: "rn-about__small rn-about__small--gap" });
    const linkEls = LINKS.map((l) => {
        const node = el(
            "button",
            {
                type: "button",
                class: "rn-about__link",
                role: "link",
                data: { glow: "1", link: l.name },
                on: { click: () => openLink(l.name) },
            },
            icon("link", 11),
            el("span", { class: "rn-about__linkText" }),
        );
        return node;
    });
    const noGithub = el("div", { class: "rn-about__small" });
    const diagNote = el("div", { class: "rn-about__small" });
    const diagEyebrow = el("div", { class: "rn-about__eyebrow rn-about__eyebrow--gap" });
    const preview = el("pre", { class: "rn-about__preview", tabindex: "0", data: { part: "about.diagPreview" } });

    // ---- 高级设置
    const advEyebrow = el("div", { class: "rn-about__eyebrow" });
    const rows = [];

    function switchRow(id, onToggle) {
        const labelId = `rn-about-row-${role}-${id}`;
        const label = el("span", { class: "rn-about__rowLabel", id: labelId });
        const state = el("span", { class: "rn-about__state", aria: { hidden: "true" } });
        const track = el("span", { class: "sc-toggle", data: { on: "0" } });
        const btn = el(
            "button",
            {
                type: "button",
                class: "rn-switch",
                role: "switch",
                data: { glow: "1", adv: id },
                aria: { checked: "false", labelledby: labelId },
                on: { click: () => onToggle(btn.getAttribute("aria-checked") !== "true") },
            },
            track,
        );
        const root = el(
            "div",
            { class: "rn-about__row", data: { row: id } },
            label,
            el("span", { class: "rn-about__ctl" }, state, btn),
        );
        return {
            root,
            label,
            set(on, t) {
                btn.setAttribute("aria-checked", on ? "true" : "false");
                track.setAttribute("data-on", on ? "1" : "0");
                state.textContent = t(on ? BUTTON_KEYS.on : BUTTON_KEYS.off);
            },
            btn,
        };
    }

    const resetRow = features.resetOnPlay
        ? switchRow("resetOnPlay", async (on) => {
              const r = await bridge.setResetOnPlay(on);
              if (r && r.ok) view.cfg = { ...view.cfg, resetOnPlay: r.value };
              applyAdvanced();
          })
        : null;
    const diagRow = switchRow("diagLog", async (on) => {
        const r = await bridge.setDiagLog(on);
        if (r && r.ok) view.diagLog = r.value;
        applyAdvanced();
    });

    const tailLabel = el("span", { class: "rn-about__rowLabel" });
    const tailItems = TAIL_MODES.map((m) =>
        el("button", {
            type: "button",
            class: "rn-darkSeg__item",
            role: "radio",
            data: { glow: "1", tail: m.mode },
            aria: { checked: "false" },
            on: {
                click: async () => {
                    const r = await bridge.setTailMode(m.mode);
                    if (r && r.ok) view.cfg = { ...view.cfg, tailMode: r.value };
                    applyAdvanced();
                },
            },
        }),
    );
    const tailRow = features.tail
        ? el(
              "div",
              { class: "rn-about__row", data: { row: "tail" } },
              tailLabel,
              el(
                  "div",
                  { class: "rn-darkSeg", role: "radiogroup", aria: { labelledby: `rn-about-tail-${role}` } },
                  ...tailItems,
              ),
          )
        : null;
    if (tailRow) tailLabel.id = `rn-about-tail-${role}`;
    const advNote = el("div", { class: "rn-about__small rn-about__small--gap", data: { part: "about.advNote" } });

    const body = el(
        "div",
        { class: "rn-about__body", role: "region", tabindex: "0", data: { part: "about.body" } },
        el("section", { class: "rn-about__sec" }, origin),
        el("section", { class: "rn-about__sec" }, license, el("div", { class: "rn-about__actions" }, viewLicense)),
        el("section", { class: "rn-about__sec" }, fonts, tm),
        el(
            "section",
            { class: "rn-about__sec rn-about__sec--tight" },
            el("div", { class: "rn-about__links" }, ...linkEls),
            noGithub,
        ),
        el("section", { class: "rn-about__sec" }, diagNote, diagEyebrow, preview),
        el(
            "section",
            { class: "rn-about__sec rn-about__sec--wide" },
            advEyebrow,
            resetRow && resetRow.root,
            tailRow,
            diagRow.root,
            advNote,
        ),
    );

    // ---- 底栏
    const copyFail = el("span", { class: "rn-about__fail", role: "alert", hidden: true });
    const copyBtn = el("button", {
        type: "button",
        class: "sc-btn sc-btn--cta rn-btn rn-btn--lg",
        data: { glow: "1", part: "about.copy" },
        on: { click: () => copyDiagnostics() },
    });
    const rulesBtn = el("button", {
        type: "button",
        class: "sc-btn sc-btn--dark rn-btn rn-btn--lg",
        data: { glow: "1", part: "about.viewRules" },
        on: { click: () => o.onViewRules && o.onViewRules() },
    });
    const closeBtn = el("button", {
        type: "button",
        class: "sc-btn sc-btn--dark rn-btn rn-btn--lg",
        data: { glow: "1", part: "about.close" },
        on: { click: () => close() },
    });
    const foot = el("div", { class: "rn-about__foot" }, copyFail, copyBtn, rulesBtn, closeBtn);
    modal.panel.append(head, body, foot);

    // ---- 行为
    function openLink(name) {
        const url = aboutLinks(view.build.version)[name];
        if (!url || !isAllowedExternal(url)) return;
        Promise.resolve(bridge.openExternal(url)).catch(() => {});
    }

    async function loadPreview() {
        preview.textContent = "";
        try {
            const r = await bridge.copyDiagnostics(false);
            preview.textContent = r && r.ok && typeof r.text === "string" ? r.text : "";
        } catch {
            preview.textContent = "";
        }
    }

    async function copyDiagnostics() {
        if (copiedTimer) timers.clearTimeout(copiedTimer);
        copiedTimer = 0;
        let ok = false;
        let code = "";
        try {
            const r = await bridge.copyDiagnostics(true);
            ok = Boolean(r && r.ok && r.copied);
            if (!ok) code = r && typeof r.reason === "string" ? r.reason : "";
        } catch {
            code = ""; // 异常消息不上屏
        }
        if (ok) {
            copyState = "copied";
            copiedTimer = timers.setTimeout(() => {
                copiedTimer = 0;
                copyState = "open";
                applyCopy();
            }, COPIED_MS);
            if (o.onCopied) o.onCopied();
        } else {
            copyState = "copyFail";
            copyFailCode = code;
            if (o.onCopyFail) o.onCopyFail(view.t(copyFailKey(code)));
        }
        applyCopy();
    }

    function applyCopy() {
        const t = view.t;
        modal.panel.setAttribute("data-state", copyState);
        copyBtn.textContent = "";
        if (copyState === "copied") {
            copyBtn.append(icon("check", 12), el("span", { text: t("about.copied") }));
            copyBtn.setAttribute("data-state", "copied");
        } else {
            copyBtn.append(icon("copy", 12), el("span", { text: t("about.copy") }));
            copyBtn.removeAttribute("data-state");
        }
        copyFail.hidden = copyState !== "copyFail";
        copyFail.textContent =
            copyState === "copyFail" ? t("about.copyFail", { reason: t(copyFailKey(copyFailCode)) }) : "";
    }

    function applyAdvanced() {
        const t = view.t;
        advEyebrow.textContent = t("about.adv.title");
        if (resetRow) {
            resetRow.label.textContent = t("about.adv.resetOnPlay");
            resetRow.label.setAttribute("title", t("about.adv.resetOnPlayTip"));
            resetRow.set(view.cfg.resetOnPlay !== false, t); // 缺省开
        }
        if (tailRow) {
            tailLabel.textContent = t("about.adv.tail");
            tailLabel.setAttribute("title", t("about.adv.tailTip"));
            const cur = view.cfg.tailMode || "auto";
            tailItems.forEach((n, i) => {
                n.textContent = t(TAIL_MODES[i].key);
                n.setAttribute("aria-checked", TAIL_MODES[i].mode === cur ? "true" : "false");
            });
        }
        diagRow.label.textContent = t("about.adv.diagLog");
        diagRow.label.setAttribute("title", t("about.adv.diagLogTip"));
        diagRow.set(view.diagLog === true, t);
        // 备注按这个角色实际有哪些行选词条（Output 没有起播清空），并跟着 tail 行：没有 tail 行就不写这句
        advNote.hidden = !tailRow;
        advNote.textContent = tailRow ? t(isIn ? "about.adv.note" : "about.adv.noteOut") : "";
    }

    function applyText() {
        const t = view.t;
        const b = view.build || {};
        lockup.textContent = t("about.lockup");
        title.textContent = t(isIn ? "about.title.in" : "about.title.out");
        product.textContent = t("about.product");
        build.textContent = t("about.build", {
            role: t(ROLE_NAME_KEY[role]),
            v: b.version || "—",
            sha: b.sha || "—",
            fmt: b.fmt || "—",
            arch: b.arch || "—",
        });
        origin.textContent = t("about.origin");
        license.textContent = t("about.license");
        viewLicense.textContent = t("about.viewLicense");
        fonts.textContent = t("about.fonts");
        tm.textContent = t("about.tm");
        linkEls.forEach((n, i) => {
            n.querySelector(".rn-about__linkText").textContent = t(LINKS[i].key);
            n.setAttribute("title", t("about.linkTip"));
        });
        noGithub.textContent = t("about.noGithub");
        diagNote.textContent = t("about.diagNote");
        diagEyebrow.textContent = t("about.diagPreview");
        preview.setAttribute("aria-label", t("about.diagPreview"));
        body.setAttribute("aria-label", t(isIn ? "about.title.in" : "about.title.out"));
        rulesBtn.textContent = t("about.viewRules");
        closeBtn.textContent = t(BUTTON_KEYS.close);
        applyAdvanced();
        applyCopy();
    }

    function close() {
        if (copiedTimer) timers.clearTimeout(copiedTimer);
        copiedTimer = 0;
        copyState = "open";
        modal.hide();
        if (o.onClose) o.onClose();
    }

    return {
        el: modal.panel,
        /** @param {{t:(k:string,v?:object)=>string, lang:string, build?:object, cfg?:object, diagLog?:boolean}} p */
        update(p) {
            view = { ...view, ...p, build: p.build || view.build, cfg: p.cfg || view.cfg };
            applyText();
        },
        /** 打开：先按当前内容画出来，再异步取诊断预览。 */
        open(p) {
            if (p) view = { ...view, ...p, build: p.build || view.build, cfg: p.cfg || view.cfg };
            copyState = "open";
            applyText();
            modal.show();
            return loadPreview();
        },
        close,
        isOpen: modal.isShown,
    };
}
