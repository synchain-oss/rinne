// SPDX-License-Identifier: GPL-3.0-or-later
//
// 安全层：紧急切断按钮与限制器关闭确认。
//   紧急切断放在外壳的最上层（z-index 400），盖在任何遮罩与面板之上：遮罩不压暗它，面板也不挡它。点它会先关掉
//   所有开着的面板再下发切断；已切断时再点不会恢复，恢复只有消息位里的「恢复回送」一个入口。
//   限制器关闭确认是一张浅色小卡，浮在安全栏上，默认焦点在取消；Esc 等于取消。

import { el } from "../../shared/dom.js";
import { icon } from "../../shared/icons.js";
import { setAttr, setHidden, setText } from "./util.js";

/**
 * @param {object} o
 * @param {HTMLElement} o.host 放按钮的层（外壳的 top 层）
 * @param {()=>void} o.onPress 按下：页面负责关面板并下发切断
 */
export function createPanic(o) {
    const label = el("span", { class: "in-panic__label" });
    let iconNode = icon("panic", 14, "var(--sem-red-icon)");
    const iconSlot = el("span", { class: "in-panic__icon" }, iconNode);
    const btn = el(
        "button",
        {
            class: "in-panic",
            type: "button",
            "data-gb": "input.panic",
            "data-safety": "panic",
            "data-tone": "red",
            "data-glow": "1",
        },
        iconSlot,
        label,
    );
    btn.addEventListener("click", () => o.onPress());
    const layer = el("div", { class: "in-panic-layer", "data-part": "safety-layer" }, btn);
    o.host.append(layer);

    let cutNow = null;
    return {
        el: btn,
        layer,
        /** @param {{cut:boolean, overScrim:boolean, t:(k:string)=>string}} p */
        update(p) {
            if (cutNow !== p.cut) {
                cutNow = p.cut;
                iconNode.remove();
                iconNode = icon("panic", 14, p.cut ? "var(--txt-1)" : "var(--sem-red-icon)");
                iconSlot.append(iconNode);
            }
            setText(label, p.t(p.cut ? "in.panic.cut" : "in.panic.label"));
            setAttr(btn, "aria-label", p.t("in.panic.aria"));
            setAttr(btn, "title", p.t(p.cut ? "in.panic.cutTip" : "in.panic.tip"));
            setAttr(btn, "data-state", (p.cut ? "cut" : "idle") + (p.overScrim ? "+overScrim" : ""));
        },
    };
}

/**
 * @param {object} o
 * @param {HTMLElement} o.host 放确认卡的层
 * @param {()=>void} o.onCancel
 * @param {()=>void} o.onConfirm
 */
export function createLimiterConfirm(o) {
    const text = el("div", { class: "in-limconfirm__text", id: "in-limconfirm-text" });
    const cancel = el("button", {
        class: "sc-btn sc-btn--cta in-limconfirm__cancel",
        type: "button",
        "data-glow": "1",
    });
    const confirm = el("button", {
        class: "sc-btn in-btn--darkred in-limconfirm__confirm",
        type: "button",
        "data-glow": "1",
    });
    const root = el(
        "div",
        {
            class: "in-limconfirm",
            role: "alertdialog",
            "data-part": "input.limiter.confirm",
            "data-state": "open",
            aria: { describedby: "in-limconfirm-text" },
        },
        text,
        el("div", { class: "in-limconfirm__buttons" }, cancel, confirm),
    );
    setHidden(root, true);
    o.host.append(root);
    let isOpen = false;
    let returnTo = null;

    const close = (restoreFocus) => {
        if (!isOpen) return;
        isOpen = false;
        setHidden(root, true);
        if (restoreFocus && returnTo && returnTo.isConnected) returnTo.focus({ preventScroll: true });
        returnTo = null;
    };
    cancel.addEventListener("click", () => {
        close(true);
        o.onCancel();
    });
    confirm.addEventListener("click", () => {
        close(false);
        o.onConfirm();
    });
    root.addEventListener("keydown", (e) => {
        if (e.key === "Escape") {
            e.preventDefault();
            e.stopPropagation();
            close(true);
            o.onCancel();
        } else if (e.key === "Tab") {
            // 两个按钮之间循环，不让焦点漏到后面的控件
            const next = e.shiftKey
                ? document.activeElement === cancel
                    ? confirm
                    : cancel
                : document.activeElement === confirm
                  ? cancel
                  : confirm;
            e.preventDefault();
            next.focus();
        }
    });

    return {
        el: root,
        isOpen: () => isOpen,
        /** @param {{t:(k:string)=>string, from?:HTMLElement}} p from：关闭后把焦点还给谁（限制器开关） */
        open(p) {
            setText(text, p.t("in.lim.confirm"));
            setText(cancel, p.t("common.cancel"));
            setText(confirm, p.t("in.lim.confirmOff"));
            setAttr(root, "aria-label", p.t("in.lim.label"));
            if (!isOpen) returnTo = p.from || document.activeElement;
            isOpen = true;
            setHidden(root, false);
            cancel.focus({ preventScroll: true });
        },
        /** 刷新文字（语言切换时）。 */
        retext(t) {
            if (!isOpen) return;
            setText(text, t("in.lim.confirm"));
            setText(cancel, t("common.cancel"));
            setText(confirm, t("in.lim.confirmOff"));
        },
        close: () => close(false),
    };
}
