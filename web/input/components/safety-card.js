// SPDX-License-Identifier: GPL-3.0-or-later
//
// 安全卡（右栏上，248×290）：紧急切断的槽位（按钮本身在最上层，见 safety-layer.js）、切断原因、限制器开关与上限、
// 增益缩减表、三盏灯和失控保护开关。限制器是工程状态不是自动化参数：开关走 setLimiter，关闭前要先过确认卡。

import { el } from "../../shared/dom.js";
import { CEILING, ceilingFromPos, ceilingPos, fmtFixed, grView, lampsView, LAMP_META } from "../model.js";
import { createMiniDial } from "./mini-dial.js";
import { createToggle } from "./toggle.js";
import { attachControl, attachReadoutEdit } from "./gesture.js";
import { setAttr, setHidden, setText, setVar } from "./util.js";

/** 切断原因 -> 原因词条的后缀。 */
const CAUSE = Object.freeze({ panic: "manual", nan: "nan", huge: "huge", runaway: "runaway" });

const SCALE = [
    [0, "0"],
    [6, "6"],
    [12, "12"],
    [24, "24"],
];

/**
 * @param {object} ctx 页面上下文
 * @param {object} o
 * @param {()=>void} o.onLimiterOff 想关限制器：页面弹确认卡
 */
export function createSafetyCard(ctx, o) {
    let ceilingDrag = false;

    const limiterP = ctx.param("limiter", {
        kind: "bool",
        send: (v) => ctx.bridge.setLimiter(v),
        onDisplay: () => renderAll(),
    });
    const ceilingP = ctx.param("ceiling", {
        epsilon: 0.01,
        onDisplay: () => renderLimiter(),
        onAuto: () => renderLimiter(),
    });

    // ---- 结构
    const eyebrow = el("span", { class: "sc-eyebrow in-safety__eyebrow" });
    const panicSlot = el("div", { class: "in-safety__panic", "data-part": "panic-slot" });
    const cause = el("span", { class: "in-safety__cause", "data-part": "panic.cause" });
    const causeRow = el("div", { class: "in-safety__cause-row" }, cause);

    const limLabel = el("span", { class: "in-lim__label" });
    const limToggle = createToggle({
        part: "limiter.toggle",
        onToggle: (next) => {
            if (next) limiterP.commit(true);
            else o.onLimiterOff(limToggle.el);
        },
    });
    const offNote = el("div", { class: "in-lim__offnote" });
    const ceilText = el("span", { class: "in-lim__ceiling-text", "data-part": "ceiling.readout" });
    const ceilDial = createMiniDial({ size: 26, halo: "var(--halo-1)", part: "ceiling" });
    const ceilRow = el("div", { class: "in-lim__ceiling", "data-part": "ceiling" }, ceilText, ceilDial.el);
    const limiter = el(
        "div",
        { class: "in-lim", "data-gb": "input.limiter" },
        el("div", { class: "in-lim__row" }, limLabel, el("span", { class: "in-lim__toggle" }, limToggle.el)),
        offNote,
        ceilRow,
    );

    const grLabel = el("span", { class: "sc-eyebrow in-gr__label" });
    const grRead = el("span", { class: "in-gr__readout", "data-part": "gr.readout" });
    const grFill = el("div", { class: "in-gr__fill", "data-part": "gr.fill" });
    const grTube = el(
        "div",
        { class: "sc-tube in-tube in-gr__tube" },
        el("div", { class: "sc-tube__slot" }, grFill),
        el("div", { class: "sc-tube__gloss", "aria-hidden": "true" }),
    );
    const grScale = el(
        "div",
        { class: "in-gr__scale", "aria-hidden": "true" },
        ...SCALE.map(([db, text], i) =>
            el(
                "span",
                { class: "in-gr__tick", "data-i": String(i), style: { left: `${100 - (db / 24) * 100}%` } },
                db === 0 ? "0" : `−${text}`,
            ),
        ),
    );
    const gr = el(
        "div",
        { class: "in-gr", "data-gb": "input.gr" },
        el("div", { class: "in-gr__head" }, grLabel, grRead),
        grTube,
        grScale,
    );

    const lampRows = {};
    for (const id of ["dc", "nan", "run"]) {
        const dot = el("span", { class: "sc-dot in-lamp__dot" });
        const name = el("span", { class: "in-lamp__name" });
        const state = el("span", { class: "in-lamp__state" });
        const row = el("div", { class: "in-lamp", role: "img", "data-lamp": id }, dot, name, state);
        lampRows[id] = { row, dot, name, state };
    }
    const guardLabel = el("span", { class: "in-guard__label" });
    const guardToggle = createToggle({
        part: "guard.toggle",
        onToggle: (next) => {
            ctx.setGuardOn(next);
        },
    });
    const guardRow = el(
        "div",
        { class: "in-guard", "data-part": "guard" },
        guardLabel,
        el("span", { class: "in-guard__toggle" }, guardToggle.el),
    );
    const lamps = el(
        "div",
        { class: "in-lamps", "data-gb": "input.lamps" },
        el("div", { class: "in-lamps__rows" }, lampRows.dc.row, lampRows.nan.row, lampRows.run.row),
        guardRow,
    );

    const root = el(
        "section",
        { class: "sc-card in-card in-safety", "data-tour": "safety", "data-part": "safety-card" },
        el("div", { class: "in-safety__head" }, eyebrow),
        panicSlot,
        causeRow,
        limiter,
        gr,
        lamps,
    );

    // ---- 上限小表盘
    const editCeiling = () =>
        ctx.edit({
            cover: ceilText,
            initial: fmtFixed(ceilingP.value() ?? -6, 1),
            field: "ceiling.input",
            label: ctx.view.t("in.lim.ceilingTip"),
            fs: 10.5,
            h: 22,
            w: 64,
            parse: (text) => {
                const n = Number(
                    String(text)
                        .trim()
                        .replace(/−/g, "-")
                        .replace(/\s*dbfs$/i, ""),
                );
                return Number.isFinite(n) && String(text).trim() !== "" && n >= CEILING.min && n <= CEILING.max
                    ? { ok: true, value: Math.round(n * 10) / 10 }
                    : { ok: false, min: fmtFixed(CEILING.min, 0), max: fmtFixed(CEILING.max, 0) };
            },
            onCommit: (res) => ceilingP.commit(res.value),
            focusBack: ceilDial.el,
        });
    const ceilingOff = () => limiterP.value() === false;
    attachControl(ceilDial.el, {
        param: ceilingP,
        toPos: ceilingPos,
        fromPos: ceilingFromPos,
        quantize: (v) => Math.round(v * 10) / 10,
        stepPos: CEILING.step / (CEILING.max - CEILING.min),
        def: -6,
        onEdit: editCeiling,
        onDrag: (on) => {
            ceilingDrag = on;
            ctx.setDragging("ceiling", on);
            renderLimiter();
        },
        disabled: ceilingOff,
        timers: ctx.timers,
    });
    attachReadoutEdit(ceilText, { onEdit: editCeiling, timers: ctx.timers, disabled: ceilingOff });

    // ---- 渲染
    function renderLimiter() {
        if (!ctx.view) return;
        const t = ctx.view.t;
        const on = limiterP.value() !== false;
        const c = ceilingP.value();
        setText(limLabel, t("in.lim.label"));
        limToggle.update({ on, label: t("in.lim.label"), title: t("in.lim.tip") });
        setText(offNote, on ? "" : t("in.lim.offNote"));
        setHidden(offNote, on);
        setAttr(limiter, "data-state", on ? "on" : "off");
        if (c === null) return;
        setText(ceilText, t("in.lim.ceiling", { c: fmtFixed(c, 1) }));
        setAttr(ceilRow, "data-state", on ? "normal" : "disabled");
        setAttr(ceilRow, "title", t("in.lim.ceilingTip"));
        ceilDial.update({
            pos: ceilingPos(c),
            valueText: `${fmtFixed(c, 1)} dBFS`,
            label: t("in.lim.ceilingTip"),
            drag: ceilingDrag,
            auto: ceilingP.automating(),
            disabled: !on,
        });
    }

    function renderGr() {
        const view = ctx.view;
        const t = view.t;
        const on = limiterP.value() !== false;
        const g = grView((view.meters || {}).gr, on);
        setText(grLabel, t("in.gr.label"));
        setAttr(gr, "data-state", g.state);
        setAttr(gr, "title", g.state === "hidden" ? t("in.gr.hiddenTip") : null);
        if (g.state === "hidden") setText(grRead, "—");
        else setText(grRead, t("in.gr.readout", { v: g.readout }));
        setHidden(grFill, g.state === "hidden");
        setVar(grFill, "--w", `${g.fillPct}%`);
    }

    function renderLamps() {
        const view = ctx.view;
        const t = view.t;
        const lv = lampsView((view.meters || {}).lamps);
        for (const id of ["dc", "nan", "run"]) {
            const l = lv[id];
            const r = lampRows[id];
            const ok = l.state === "ok";
            setText(r.name, t(LAMP_META[id].label));
            setText(r.state, t(l.key, l.vals));
            setAttr(r.row, "data-state", l.state);
            setAttr(r.row, "data-tone", ok ? "gray" : l.tone);
            setAttr(r.dot, "data-tone", ok ? "gray" : l.tone);
            setAttr(r.dot, "data-hollow", ok ? "1" : null);
            setAttr(r.row, "title", t(LAMP_META[id].tip));
            setAttr(
                r.row,
                "aria-label",
                `${t(LAMP_META[id].label)}${view.lang === "zh" ? "：" : ": "}${t(l.key, l.vals)}`,
            );
        }
        const guardOn = view.guardOn;
        setText(guardLabel, t("in.guard.label"));
        guardToggle.update({ on: guardOn, label: t("in.guard.label"), title: t("in.guard.tip") });
        setAttr(guardRow, "data-state", guardOn ? "on" : "off");
        setAttr(
            lamps,
            "data-state",
            `dc:${lv.dc.state}|nan:${lv.nan.state}|run:${lv.run.state}|guard:${guardOn ? "on" : "off"}`,
        );
    }

    function renderCause() {
        const view = ctx.view;
        const c = (view.state.safety || {}).cut;
        setHidden(cause, !c);
        if (!c) {
            setText(cause, "");
            setAttr(cause, "data-state", null);
            return;
        }
        const k = CAUSE[c] || "manual";
        setAttr(cause, "data-state", k);
        setText(cause, view.t("in.panic.cause." + k));
    }

    function renderAll() {
        if (!ctx.view) return;
        setText(eyebrow, ctx.view.t("in.safety.eyebrow"));
        setAttr(root, "aria-label", ctx.view.t("in.safety.eyebrow"));
        renderLimiter();
        renderGr();
        renderLamps();
        renderCause();
    }

    return { el: root, render: renderAll, panicSlot, limiterToggle: limToggle.el };
}
