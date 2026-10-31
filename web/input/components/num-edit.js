// SPDX-License-Identifier: GPL-3.0-or-later
//
// 数值输入：点读数（或 Ctrl / Cmd + 单击）把读数换成一个输入框。
//   Enter  按 parse 解析；合法就提交并关闭，超出范围则停在原处、边框变红、框下写「超出范围：最小–最大」；
//   Esc    取消；失去焦点也取消（未提交的文字不落地）。
// 同一时刻只开一个输入框（单例），输入框里的指针、滚轮、键盘事件都不往外冒，免得被外层的表盘手势吃掉。

import { el } from "../../shared/dom.js";
import { setAttr, setHidden, setText } from "./util.js";

/**
 * @param {object} o
 * @param {(k:string, v?:object)=>string} o.t 取词条（随语言切换，用函数避免缓存旧语言）
 */
export function createNumEdit(o) {
    let open = null;

    function close(returnFocus = true) {
        if (!open) return;
        const s = open;
        open = null;
        s.root.remove();
        setHidden(s.cfg.cover, false);
        s.host.removeAttribute("data-editing");
        if (s.cfg.onClose) s.cfg.onClose();
        if (returnFocus && s.cfg.focusBack) s.cfg.focusBack.focus({ preventScroll: true });
    }

    /**
     * @param {object} cfg
     * @param {HTMLElement} cfg.cover 被换掉的读数元素（编辑期间隐藏）
     * @param {string} cfg.initial 初始文字
     * @param {string} cfg.field data-part
     * @param {string} cfg.label aria-label
     * @param {number} cfg.fs 字号 px
     * @param {number} cfg.h 高度 px
     * @param {number} cfg.w 宽度 px
     * @param {(text:string)=>{ok:boolean, min?:string, max?:string}} cfg.parse
     * @param {(parsed:object)=>void} cfg.onCommit
     * @param {()=>void} [cfg.onClose]
     * @param {HTMLElement} [cfg.focusBack] 关闭后把焦点还给谁
     */
    function openEdit(cfg) {
        close(false);
        const input = el("input", {
            class: "in-numedit__input",
            type: "text",
            inputmode: "decimal",
            autocomplete: "off",
            spellcheck: "false",
            "data-part": cfg.field,
            "aria-label": cfg.label,
            value: cfg.initial,
        });
        const message = el("span", { class: "in-numedit__range", role: "alert" });
        setHidden(message, true);
        const box = el(
            "span",
            {
                class: "in-numedit__box",
                style: { height: `${cfg.h}px`, minWidth: `${cfg.w}px`, fontSize: `${cfg.fs}px` },
            },
            input,
        );
        const root = el(
            "span",
            { class: "in-numedit", "data-gb": "input.numEdit", "data-state": "editing" },
            box,
            message,
        );
        const stop = (e) => e.stopPropagation();
        for (const type of ["pointerdown", "pointerup", "dblclick", "wheel", "click", "contextmenu"]) {
            root.addEventListener(type, stop);
        }
        const commit = () => {
            const parsed = cfg.parse(input.value);
            if (parsed.ok) {
                const commitFn = cfg.onCommit;
                close();
                commitFn(parsed);
                return;
            }
            setAttr(root, "data-state", "range");
            setText(message, o.t("in.num.range", { min: parsed.min, max: parsed.max }));
            setHidden(message, false);
            input.select();
        };
        input.addEventListener("keydown", (e) => {
            e.stopPropagation();
            if (e.key === "Enter") {
                e.preventDefault();
                commit();
            } else if (e.key === "Escape") {
                e.preventDefault();
                close();
            }
        });
        input.addEventListener("input", () => {
            if (root.getAttribute("data-state") === "range") {
                setAttr(root, "data-state", "editing");
                setHidden(message, true);
            }
        });
        input.addEventListener("blur", () => {
            if (open && open.root === root) close(false);
        });
        const host = cfg.cover.parentElement;
        open = { root, cfg, host };
        setHidden(cfg.cover, true);
        host.setAttribute("data-editing", "1"); // 读数旁的副行在编辑期间收起
        cfg.cover.after(root);
        input.focus({ preventScroll: true });
        input.select();
    }

    return {
        open: openEdit,
        close,
        isOpen: () => open !== null,
    };
}
