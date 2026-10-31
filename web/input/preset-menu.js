// SPDX-License-Identifier: GPL-3.0-or-later
//
// 预设按钮（页眉右侧，200×30）与预设面板（浮在页眉下方，260 宽）。
//   列表来自 rinne.presets（工厂 8 个带三语名，用户预设是字符串），当前预设与「 *」读 state.preset；页面不替宿主
//   猜这些：rinne.presets 没到时按钮置灰、面板不开，快照没到时整块 inert（写入闸另在 bridge-gate.js 里挡着）。
//   面板有七种子态：列表、保存输入、名称为空、重名、非法字符、保存失败、删除确认（data-state 的取值同设计稿）。
//   名称清洗（去控制字符、首尾空白、限 24 个码点）与原生预设库同一规则，在前端也做一遍，重名提示和保存按钮
//   的可用性都按清洗后的名字判；原生侧才是最终裁决，回 exists 时按重名处理，其他失败原样显示原因码。
//   交互：↑↓/Home/End 在条目间移动，Enter 或空格载入，Esc 关整个面板，Tab 在面板内循环；点面板外关闭（不吞这一下点击）。

import { el } from "../shared/dom.js";
import { icon } from "../shared/icons.js";
import { nextId, setAttr, setHidden, setText } from "./components/util.js";

/** 名称上限（Unicode 码点）。 */
export const NAME_MAX = 24;
/** 规格要求页面拦的九个字符（原生预设库不拦，页面是第一道）。 */
export const BAD_CHARS = Object.freeze(["\\", "/", ":", "*", "?", '"', "<", ">", "|"]);
/** 没有当前预设时按钮上写这个。 */
const NO_NAME = "—";
/** 有人话词条的失败原因码（表里没有的码原样显示）。 */
const REASON_KEYS = Object.freeze({
    "write-failed": "in.preset.reason.writeFailed",
    "not-found": "in.preset.reason.notFound",
    "not-ready": "in.preset.reason.notReady",
});

/** C0（含制表、换行）、DEL、C1。码位判，不写 \u 区间：字体检查会把源码里的转义当必需字符。 */
const isControl = (cp) => cp < 0x20 || (cp >= 0x7f && cp <= 0x9f);

/** 去控制字符。 */
export function stripControl(raw) {
    let out = "";
    for (const ch of String(raw === undefined || raw === null ? "" : raw)) {
        if (!isControl(ch.codePointAt(0))) out += ch;
    }
    return out;
}

/** 名称清洗：去控制字符，去首尾空白（String.trim 含 NBSP 与全角空格），按码点截到上限，截后再去尾部空白。 */
export function cleanName(raw) {
    return Array.from(stripControl(raw).trim()).slice(0, NAME_MAX).join("").trim();
}

/** 名字里含的非法字符（按 BAD_CHARS 的顺序、去重）。 */
export function badCharsIn(name) {
    return BAD_CHARS.filter((c) => String(name).includes(c));
}

/** 清洗后的名字有什么问题：空、含非法字符，或没问题（null）。 */
export function nameProblem(name) {
    if (name === "") return "empty";
    if (badCharsIn(name).length > 0) return "badChars";
    return null;
}

/** 重名按去空白后的显示名不分大小写比较。 */
export function sameName(a, b) {
    return String(a).trim().toLowerCase() === String(b).trim().toLowerCase();
}

/** 预设名在当前语言下的显示：工厂预设是 { zh, en, fr }，用户预设是字符串。 */
export function displayName(name, lang) {
    if (typeof name === "string") return name;
    if (name && typeof name === "object") {
        for (const k of [lang, "en", "zh"]) if (typeof name[k] === "string" && name[k] !== "") return name[k];
    }
    return "";
}

/** 保存输入框的初始名字：从「用户预设数 + 1」起找第一个没被占的序号。make(n) 给出带序号的名字。 */
export function pickDefaultName(userNames, make) {
    let n = userNames.length + 1;
    while (userNames.some((u) => sameName(u, make(n)))) n += 1;
    return make(n);
}

const validItem = (p) =>
    Boolean(p) &&
    typeof p === "object" &&
    typeof p.id === "string" &&
    p.id !== "" &&
    (typeof p.name === "string" || (Boolean(p.name) && typeof p.name === "object"));

/**
 * @param {object} ctx 页面上下文（bridge、view、locked、toast、timers）
 * @param {object} o
 * @param {HTMLElement} o.slot 放按钮的槽位（页眉右侧，设计盒坐标 314,21，200×30）
 * @param {HTMLElement} o.host 放面板的层（外壳的 layer，z 110）
 */
export function createPresetMenu(ctx, o) {
    const bridge = ctx.bridge;
    const can = (name) => typeof bridge[name] === "function";
    const idPrefix = nextId("in-pm-");

    // ---- 状态
    /** 最近一帧 rinne.presets（null：还没到，按钮置灰、面板不开）。 */
    let presets = null;
    let isOpen = false;
    /** 有请求在途：其间不再接受新的动作。 */
    let busy = false;
    /** list | save | delete */
    let view = "list";
    /** null | dupe | badChars | saveFail */
    let msg = null;
    let failReason = "";
    /** 删除失败的原因码（null：没有失败提示）；只在删除确认态下显示。 */
    let delFail = null;
    /** 宿主回过 exists 的名字（页面手里的列表已经旧了）：下次保存按覆盖发。 */
    const staleDupes = new Set();
    let rowSig = "";
    let rows = [];
    let rovingRow = null;

    // ---- 触发按钮
    const trLabel = el("span", { class: "in-pt__label" });
    const trText = el("span", { class: "in-pt__text", "data-part": "preset.name" });
    const trDirty = el("span", { class: "in-pt__dirty", "data-part": "preset.dirty" });
    const trigger = el(
        "button",
        {
            class: "in-pt",
            type: "button",
            "data-gb": "input.preset.trigger",
            "data-glow": "1",
            aria: { haspopup: "dialog", expanded: "false" },
        },
        trLabel,
        el("span", { class: "in-pt__nm" }, trText, trDirty),
        el("span", { class: "in-pt__chev" }, icon("chev", 12, "currentColor")),
    );
    o.slot.append(trigger);

    // ---- 面板：列表
    const idFac = `${idPrefix}fac`;
    const idUser = `${idPrefix}user`;
    const facLabel = el("div", { class: "in-pm__label", id: idFac });
    const facRows = el("div", { class: "in-pm__rows", "data-part": "preset.factory.list" });
    const userLabel = el("div", { class: "in-pm__label", id: idUser });
    const userRows = el("div", { class: "in-pm__rows in-pm__rows--user", "data-part": "preset.user.list" });
    const userEmpty = el("div", { class: "in-pm__empty", "data-part": "preset.user.empty" });
    const sep = () => el("div", { class: "in-pm__sep", aria: { hidden: "true" } });
    const list = el(
        "div",
        { class: "in-pm__list", role: "listbox", "data-part": "preset.list" },
        el("div", { class: "in-pm__group", role: "group", aria: { labelledby: idFac } }, facLabel, facRows),
        el(
            "div",
            { class: "in-pm__group", role: "group", aria: { labelledby: idUser } },
            sep(),
            userLabel,
            userRows,
            userEmpty,
        ),
    );

    // ---- 面板：动作区（三套子树，按子态显示一套）
    const act = (part) => el("button", { class: "in-pm__act", type: "button", "data-glow": "1", "data-part": part });
    const saveAsBtn = act("preset.saveAs");
    const overwriteBtn = act("preset.overwrite");
    const deleteBtn = act("preset.delete");
    const actList = el(
        "div",
        { class: "in-pm__acts", "data-part": "preset.actions" },
        saveAsBtn,
        el("div", { class: "in-pm__acts2" }, overwriteBtn, deleteBtn),
    );

    const delText = el("div", { class: "in-pm__delmsg", id: `${idPrefix}delmsg` });
    const delCancel = el("button", { class: "sc-btn in-pm__btn", type: "button", "data-glow": "1" });
    const delGo = el("button", { class: "sc-btn in-btn--darkred in-pm__btn", type: "button", "data-glow": "1" });
    const actDelete = el(
        "div",
        { class: "in-pm__confirm", role: "group", "data-tone": "amber", "data-part": "preset.delete.confirm" },
        delText,
        el("div", { class: "in-pm__btns" }, delCancel, delGo),
    );

    const delMsgEl = el("div", {
        class: "in-pm__delfail",
        role: "status",
        "data-tone": "red",
        "data-part": "preset.delete.msg",
    });

    const input = el("input", {
        class: "in-pm__input",
        type: "text",
        autocomplete: "off",
        spellcheck: "false",
        "data-part": "preset.name.input",
    });
    const msgEl = el("div", { class: "in-pm__msg", role: "status", "data-part": "preset.msg" });
    const saveCancel = el("button", { class: "sc-btn in-pm__btn", type: "button", "data-glow": "1" });
    const saveGo = el("button", { class: "sc-btn sc-btn--cta in-pm__btn", type: "button", "data-glow": "1" });
    const actSave = el(
        "div",
        { class: "in-pm__save" },
        el("div", { class: "in-pm__field", "data-part": "preset.name.field" }, input),
        msgEl,
        el("div", { class: "in-pm__btns" }, saveCancel, saveGo),
    );

    const note = el("div", { class: "in-pm__note" });
    const root = el(
        "div",
        {
            class: "in-pm",
            role: "dialog",
            tabindex: "-1",
            "data-gb": "input.preset.menu",
            "data-state": "list",
        },
        list,
        sep(),
        actList,
        actDelete,
        delMsgEl,
        actSave,
        note,
    );
    setHidden(root, true);
    o.host.append(root);

    // ---- 取值
    const tr = () => (ctx.view ? ctx.view.t : null);
    const lang = () => (ctx.view ? ctx.view.lang : "zh");
    const curPreset = () => (ctx.view && ctx.view.state && ctx.view.state.preset) || null;
    const curId = () => {
        const p = curPreset();
        return p && typeof p.id === "string" ? p.id : null;
    };
    const userNames = () => (presets ? presets.user.map((p) => displayName(p.name, lang())) : []);
    /** 当前预设若是用户预设，返回它在列表里的条目；否则 null（工厂预设与没有当前预设都不能覆盖或删除）。 */
    const curUser = () => {
        const id = curId();
        return presets && id ? presets.user.find((p) => p.id === id) || null : null;
    };
    const isDupe = (name) => userNames().some((u) => sameName(u, name)) || staleDupes.has(String(name).toLowerCase());
    const usable = () => presets !== null && !ctx.locked();
    /** 失败原因码换成人话；表里没有的码原样显示，没有原因码时写「原因未知」。 */
    const reasonText = (code) => {
        if (typeof code !== "string" || code === "") return tr()("in.preset.reason.unknown");
        return Object.prototype.hasOwnProperty.call(REASON_KEYS, code) ? tr()(REASON_KEYS[code]) : code;
    };

    // ---- 渲染
    function stateName() {
        if (view === "delete") return "deleteConfirm";
        if (view === "save") {
            if (msg) return msg;
            return cleanName(input.value) === "" ? "empty" : "saveInput";
        }
        return "list";
    }

    function renderTrigger() {
        const t = tr();
        if (!t) return;
        const p = curPreset();
        const name = p ? displayName(p.name, lang()) : "";
        const shown = name === "" ? NO_NAME : name;
        const dirty = Boolean(p && p.dirty === true) && name !== "";
        setText(trLabel, t("in.preset.label"));
        setText(trText, shown);
        setAttr(trText, "title", name === "" ? null : name);
        setText(trDirty, dirty ? " *" : "");
        setHidden(trDirty, !dirty);
        setAttr(trDirty, "title", dirty ? t("in.preset.dirtyTip") : null);
        const off = presets === null;
        if (trigger.disabled !== off) trigger.disabled = off;
        setAttr(trigger, "aria-expanded", isOpen ? "true" : "false");
        setAttr(trigger, "data-state", isOpen ? "open" : off ? "disabled" : dirty ? "dirty" : "normal");
    }

    /** 快照没到：按钮写「—」且置灰，面板收起。 */
    function renderPending(t) {
        if (isOpen) close(false);
        setText(trLabel, t("in.preset.label"));
        setText(trText, NO_NAME);
        setAttr(trText, "title", null);
        setText(trDirty, "");
        setHidden(trDirty, true);
        if (!trigger.disabled) trigger.disabled = true;
        setAttr(trigger, "aria-expanded", "false");
        setAttr(trigger, "data-state", "disabled");
    }

    function buildRow(item) {
        const chk = el("span", { class: "in-pm__chk" }, icon("check", 12, "currentColor"));
        const name = el("span", {
            class: "in-pm__name",
            "data-part": item.kind === "user" ? "preset.user.name" : "preset.factory.name",
        });
        setText(name, item.label);
        if (item.kind === "user") setAttr(name, "title", item.label);
        const row = el(
            "div",
            {
                class: "in-pm__opt",
                role: "option",
                tabindex: "-1",
                "data-glow": "1",
                "data-id": item.id,
                "data-kind": item.kind,
                aria: { selected: "false" },
            },
            chk,
            name,
        );
        row.addEventListener("click", () => void doLoad(item.id, item.label));
        return row;
    }

    const fill = (node, kids) => {
        node.textContent = "";
        node.append(...kids);
    };

    function focusedRowId() {
        const a = document.activeElement;
        return a && a.getAttribute && rows.includes(a) ? a.getAttribute("data-id") : null;
    }

    function syncRows() {
        const lg = lang();
        const f = presets.factory.map((p) => ({ id: p.id, kind: "factory", label: displayName(p.name, lg) }));
        const u = presets.user.map((p) => ({ id: p.id, kind: "user", label: displayName(p.name, lg) }));
        const sig = JSON.stringify([f, u]);
        if (sig !== rowSig) {
            rowSig = sig;
            const had = focusedRowId();
            fill(facRows, f.map(buildRow));
            fill(userRows, u.map(buildRow));
            rows = [...facRows.children, ...userRows.children];
            rovingRow = null;
            if (had) {
                const back = rows.find((r) => r.getAttribute("data-id") === had);
                if (back) setRoving(back, true);
            }
        }
        setHidden(userRows, u.length === 0);
        setHidden(userEmpty, u.length > 0);
        const id = curId();
        for (const r of rows) {
            const cur = id !== null && r.getAttribute("data-id") === id;
            setAttr(r, "aria-selected", cur ? "true" : "false");
            setAttr(r, "data-state", cur ? "current" : null);
            setHidden(r.firstChild, !cur);
            setAttr(r, "aria-disabled", can("loadPreset") ? null : "true");
        }
        if (!rovingRow || !rows.includes(rovingRow)) {
            setRoving(rows.find((r) => r.getAttribute("data-state") === "current") || rows[0] || null, false);
        }
    }

    function setRoving(row, focus) {
        rovingRow = row;
        for (const r of rows) {
            const want = r === row ? "0" : "-1";
            if (r.getAttribute("tabindex") !== want) r.setAttribute("tabindex", want);
        }
        if (focus && row) {
            row.focus({ preventScroll: true });
            row.scrollIntoView({ block: "nearest" });
        }
    }

    function renderPanel() {
        const t = tr();
        if (!t || presets === null) return;
        syncRows();
        setAttr(root, "aria-label", t("in.preset.label"));
        setAttr(list, "aria-label", t("in.preset.label"));
        setText(facLabel, t("in.preset.factory"));
        setText(userLabel, t("in.preset.user"));
        setText(userEmpty, t("in.preset.empty"));
        setText(note, t("in.preset.note"));

        const cu = curUser();
        const curName = cu ? displayName(cu.name, lang()) : "";
        setText(saveAsBtn, t("in.preset.saveAs"));
        setText(overwriteBtn, t("in.preset.overwrite", { name: curName || NO_NAME }));
        setText(deleteBtn, t("in.preset.delete", { name: curName || NO_NAME }));
        saveAsBtn.disabled = !can("savePreset");
        overwriteBtn.disabled = !cu || !can("savePreset");
        deleteBtn.disabled = !cu || !can("deletePreset");
        setAttr(overwriteBtn, "title", curName === "" ? null : overwriteBtn.textContent);
        setAttr(deleteBtn, "title", curName === "" ? null : deleteBtn.textContent);

        setText(delText, t("in.preset.deleteConfirm"));
        setAttr(actDelete, "aria-label", t("in.preset.delete", { name: curName || NO_NAME }));
        setText(delCancel, t("common.cancel"));
        setText(delGo, t("common.delete"));
        delGo.disabled = !cu || !can("deletePreset");

        setAttr(input, "aria-label", t("in.preset.namePlaceholder"));
        setAttr(input, "placeholder", t("in.preset.namePlaceholder"));
        setText(saveCancel, t("common.cancel"));
        setText(saveGo, t("common.save"));
        const name = cleanName(input.value);
        const blocked = nameProblem(name) !== null || !can("savePreset");
        saveGo.disabled = blocked;
        setAttr(saveGo, "data-disabled", blocked ? "1" : null);
        if (msg === "dupe") setText(msgEl, t("in.preset.dupe"));
        else if (msg === "badChars") setText(msgEl, t("in.preset.badChars"));
        else if (msg === "saveFail") setText(msgEl, t("in.preset.saveFail", { reason: reasonText(failReason) }));
        else setText(msgEl, "");
        setHidden(msgEl, msg === null);
        setAttr(msgEl, "data-tone", msg === "saveFail" ? "red" : msg ? "amber" : null);

        setText(delMsgEl, delFail === null ? "" : t("in.preset.deleteFail", { reason: reasonText(delFail) }));
        setHidden(delMsgEl, view !== "delete" || delFail === null);

        setHidden(actList, view !== "list");
        setHidden(actDelete, view !== "delete");
        setHidden(actSave, view !== "save");
        setAttr(root, "data-state", stateName());
    }

    function render() {
        renderTrigger();
        if (isOpen) renderPanel();
    }

    // ---- 开合
    function open() {
        if (isOpen || !usable() || !tr()) return false;
        isOpen = true;
        view = "list";
        msg = null;
        failReason = "";
        delFail = null;
        staleDupes.clear();
        // 每次打开都从当前预设起步（上次停在哪个条目不算）
        rovingRow = null;
        setHidden(root, false);
        renderTrigger();
        renderPanel();
        document.addEventListener("pointerdown", onOutside, true);
        setRoving(rovingRow, true);
        return true;
    }

    function close(restoreFocus) {
        if (!isOpen) return;
        isOpen = false;
        view = "list";
        msg = null;
        delFail = null;
        setHidden(root, true);
        document.removeEventListener("pointerdown", onOutside, true);
        renderTrigger();
        if (restoreFocus && trigger.isConnected && !trigger.disabled) trigger.focus({ preventScroll: true });
    }

    /** 点在面板和按钮之外就收起；不拦这一下点击，被点的控件照常响应。 */
    function onOutside(e) {
        const n = e.target;
        if (root.contains(n) || trigger.contains(n)) return;
        close(false);
    }

    function toList() {
        view = "list";
        msg = null;
        delFail = null;
        renderPanel();
        setRoving(rovingRow, true);
    }

    function toSave(initial, forced) {
        view = "save";
        msg = null;
        failReason = "";
        const t = tr();
        input.value =
            typeof initial === "string"
                ? initial
                : pickDefaultName(userNames(), (n) => t("in.preset.defaultName", { n }));
        evalName();
        if (forced) {
            msg = forced.msg;
            failReason = forced.reason || "";
        }
        renderPanel();
        input.focus({ preventScroll: true });
        input.select();
    }

    function toDelete() {
        if (!curUser() || !can("deletePreset")) return;
        view = "delete";
        delFail = null;
        renderPanel();
        delCancel.focus({ preventScroll: true });
    }

    /** 按清洗后的名字重算提示：非法字符、重名；名字空了什么也不提示（保存键自己置灰）。 */
    function evalName() {
        const name = cleanName(input.value);
        if (name === "") msg = null;
        else if (badCharsIn(name).length > 0) msg = "badChars";
        else if (isDupe(name)) msg = "dupe";
        else msg = null;
    }

    // ---- 动作
    async function call(fn, ...args) {
        try {
            return await fn(...args);
        } catch {
            return null;
        }
    }

    async function doLoad(id, label) {
        if (busy || !usable() || !can("loadPreset")) return;
        busy = true;
        const r = await call(bridge.loadPreset, id);
        busy = false;
        if (r && r.ok) {
            ctx.toast("toast.presetLoaded", { name: label }, "green");
            close(true);
        } else if (r && r.reason === "bad-file") {
            ctx.toast("toast.presetBroken", undefined, "amber");
        } else {
            ctx.toast("toast.presetLoadFail", { reason: reasonText(r && r.reason) }, "amber");
        }
    }

    async function doSave(name, overwrite) {
        busy = true;
        const r = await call(bridge.savePreset, name, overwrite);
        busy = false;
        return r;
    }

    async function trySave() {
        if (busy || !usable() || view !== "save" || !can("savePreset")) return;
        const name = cleanName(input.value);
        if (nameProblem(name) !== null) return;
        const r = await doSave(name, isDupe(name));
        if (r && r.ok) {
            ctx.toast("toast.presetSaved", { name }, "green");
            close(true);
            return;
        }
        if (!isOpen || view !== "save") return;
        if (r && r.reason === "exists") {
            staleDupes.add(name.toLowerCase());
            msg = "dupe";
        } else {
            msg = "saveFail";
            failReason = r && typeof r.reason === "string" && r.reason !== "" ? r.reason : NO_NAME;
        }
        renderPanel();
        input.focus({ preventScroll: true });
    }

    async function tryOverwrite() {
        const cu = curUser();
        if (busy || !usable() || !cu || view !== "list" || !can("savePreset")) return;
        const name = displayName(cu.name, lang());
        const r = await doSave(name, true);
        if (r && r.ok) {
            ctx.toast("toast.presetSaved", { name }, "green");
            close(true);
            return;
        }
        if (!isOpen) return;
        toSave(name, {
            msg: "saveFail",
            reason: r && typeof r.reason === "string" && r.reason !== "" ? r.reason : NO_NAME,
        });
    }

    async function tryDelete() {
        const cu = curUser();
        if (busy || !usable() || !cu || view !== "delete" || !can("deletePreset")) return;
        const name = displayName(cu.name, lang());
        busy = true;
        const r = await call(bridge.deletePreset, cu.id);
        busy = false;
        if (r && r.ok) {
            ctx.toast("toast.presetDeleted", { name }, "green");
            if (isOpen) toList();
            return;
        }
        // 失败时留在确认态，红字写原因（表里没有的码原样显示），可以再点删除重试
        if (!isOpen || view !== "delete") return;
        delFail = r && typeof r.reason === "string" && r.reason !== "" ? r.reason : NO_NAME;
        renderPanel();
    }

    saveAsBtn.addEventListener("click", () => toSave());
    overwriteBtn.addEventListener("click", () => void tryOverwrite());
    deleteBtn.addEventListener("click", () => toDelete());
    delCancel.addEventListener("click", () => {
        toList();
    });
    delGo.addEventListener("click", () => void tryDelete());
    saveCancel.addEventListener("click", () => toList());
    saveGo.addEventListener("click", () => void trySave());

    input.addEventListener("input", () => {
        const clean = stripControl(input.value).trim();
        const cps = Array.from(clean);
        if (cps.length > NAME_MAX) input.value = cps.slice(0, NAME_MAX).join("");
        failReason = "";
        evalName();
        renderPanel();
    });

    trigger.addEventListener("click", () => {
        if (isOpen) close(true);
        else open();
    });
    trigger.addEventListener("keydown", (e) => {
        if (e.key === "ArrowDown" && !isOpen) {
            e.preventDefault();
            open();
        } else if (e.key === "Escape" && isOpen) {
            e.preventDefault();
            close(true);
        }
    });

    /** 面板里当前能用 Tab 到的节点（按文档顺序）。 */
    function tabStops() {
        return [...root.querySelectorAll('[tabindex="0"], button, input')].filter(
            (n) => !n.disabled && !n.closest("[hidden]"),
        );
    }

    root.addEventListener("keydown", (e) => {
        if (e.key === "Escape") {
            e.preventDefault();
            e.stopPropagation();
            close(true);
            return;
        }
        if (e.key === "Tab") {
            const stops = tabStops();
            if (stops.length === 0) return;
            const first = stops[0];
            const last = stops[stops.length - 1];
            const a = document.activeElement;
            if (e.shiftKey && (a === first || a === root || !root.contains(a))) {
                e.preventDefault();
                last.focus();
            } else if (!e.shiftKey && (a === last || a === root || !root.contains(a))) {
                e.preventDefault();
                first.focus();
            }
            return;
        }
        if (e.target === input) {
            if (e.key === "Enter") {
                e.preventDefault();
                void trySave();
            }
            return;
        }
        const i = rows.indexOf(e.target);
        if (i < 0) return;
        let to = -1;
        if (e.key === "ArrowDown") to = Math.min(rows.length - 1, i + 1);
        else if (e.key === "ArrowUp") to = Math.max(0, i - 1);
        else if (e.key === "Home") to = 0;
        else if (e.key === "End") to = rows.length - 1;
        if (to >= 0) {
            e.preventDefault();
            setRoving(rows[to], true);
        } else if (e.key === "Enter" || e.key === " ") {
            e.preventDefault();
            void doLoad(e.target.getAttribute("data-id"), e.target.querySelector(".in-pm__name").textContent);
        }
    });

    return {
        trigger,
        el: root,
        isOpen: () => isOpen,
        hasPresets: () => presets !== null,
        /** rinne.presets 的新一帧。 */
        setPresets(frame) {
            if (!frame || !Array.isArray(frame.factory) || !Array.isArray(frame.user)) return;
            presets = { factory: frame.factory.filter(validItem), user: frame.user.filter(validItem) };
            render();
        },
        /** 快照到了或变了：按钮与（开着时的）面板重画。 */
        render,
        renderPending,
        open,
        close: () => close(false),
        /**
         * 预览专用：摆出某个子态。state 取 list、saveInput、empty、dupe、badChars、saveFail、deleteConfirm，
         * 可用「+」连接（设计稿的场景写作 saveInput+dupe）；name 是输入框里的名字。
         */
        showState(state, name) {
            if (!open() && !isOpen) return;
            const tokens = String(state || "list").split("+");
            const forced = tokens.find((k) => k === "dupe" || k === "badChars" || k === "saveFail");
            if (tokens.includes("deleteConfirm")) toDelete();
            else if (forced || tokens.includes("saveInput") || tokens.includes("empty")) {
                const initial = tokens.includes("empty") ? "" : typeof name === "string" ? name : undefined;
                toSave(initial, forced ? { msg: forced, reason: forced === "saveFail" ? "write-failed" : "" } : null);
            }
        },
    };
}
