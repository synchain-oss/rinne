// SPDX-License-Identifier: GPL-3.0-or-later
//
// 反馈卡（左栏，248×400）：环路开关、反馈大表盘、状态芯片、百分比 / dB 单位、Mix 推子。
// 反馈与 Mix 的显示值来自参数回声（param-echo）：拖动期间显示跟手，宿主自动化时按指数逼近。

import { el } from "../../shared/dom.js";
import { FB_CAP, FB_MAX, fbDbText, feedbackChip, parseFeedbackInput, WHEEL_STEP_PX } from "../model.js";
import { createDial } from "./dial.js";
import { createMixFader } from "./mix-fader.js";
import { createToggle } from "./toggle.js";
import { createSeg } from "./seg.js";
import { attachControl, attachReadoutEdit } from "./gesture.js";
import { setAttr, setHidden, setText } from "./util.js";

const GATE_PX = 12;

/** @param {object} ctx 页面上下文（见 app.js） */
export function createFeedbackCard(ctx) {
    const dial = createDial();
    const mix = createMixFader();
    let dragging = false;
    let mixDragging = false;
    let over100Toasted = false;

    const fb = ctx.param("feedback", {
        epsilon: 0.05,
        onDisplay: (v) => {
            renderDial();
            if (v > FB_CAP && fb.active() && !over100Toasted) {
                over100Toasted = true;
                ctx.toast("toast.over100", undefined, "amber");
            }
        },
        onAuto: renderDial,
        onGesture: (on) => {
            if (on) ctx.touchFeedback();
            renderChip();
        },
    });
    const mixP = ctx.param("mix", { epsilon: 0.05, onDisplay: renderMix, onAuto: renderMix });
    const loopP = ctx.param("loop_on", { kind: "bool", onDisplay: () => renderAll() });

    // ---- 静态结构
    const eyebrow = el("span", { class: "sc-eyebrow in-fb__eyebrow" });
    const loopLabel = el("span", { class: "in-fb__loop-label" });
    const loopToggle = createToggle({ part: "loop.toggle", onToggle: (next) => loopP.commit(next) });
    const loopWrap = el("span", { class: "in-fb__loop", "data-gb": "input.loop" }, loopToggle.el);
    const head = el(
        "div",
        { class: "in-fb__head" },
        eyebrow,
        el("span", { class: "in-fb__loop-row" }, loopLabel, loopWrap),
    );
    const dialHost = el("div", { class: "in-fb__dial" }, dial.el);
    const tour = el("div", { class: "in-fb__tour", "data-tour": "feedback" }, head, dialHost);
    const label = el("div", { class: "in-fb__label" });
    const chip = el("span", { class: "in-chip", "data-part": "fb.chip" });
    const chipRow = el("div", { class: "in-fb__chip" }, chip);
    const seg = createSeg({
        part: "fb.unit",
        ids: ["pct", "db"],
        onPick: (id) => {
            ctx.mem.fbUnit = id;
            renderDial();
            renderSeg();
        },
    });
    const segRow = el("div", { class: "in-fb__seg" }, seg.el);
    const divider = el("div", { class: "in-fb__divider", "aria-hidden": "true" });
    const mixHost = el("div", { class: "in-fb__mix" }, mix.el);
    const root = el(
        "section",
        { class: "sc-card in-card in-fb", "aria-label": "" },
        tour,
        label,
        chipRow,
        segRow,
        divider,
        mixHost,
    );

    // ---- 手势
    attachControl(dial.disc, {
        param: fb,
        toPos: (v) => v / FB_MAX,
        fromPos: (p) => p * FB_MAX,
        quantize: Math.round,
        stepPos: 1 / FB_MAX,
        wheelPxPerStep: WHEEL_STEP_PX,
        def: 0,
        cap: FB_CAP,
        gate: { pos: FB_CAP / FB_MAX, px: GATE_PX },
        readout: dial.readout,
        onEdit: editFeedback,
        onDrag: (on) => {
            dragging = on;
            ctx.setDragging("feedback", on);
            renderDial();
        },
        timers: ctx.timers,
    });
    attachControl(mix.fader, {
        param: mixP,
        toPos: (v) => v / 100,
        fromPos: (p) => p * 100,
        quantize: Math.round,
        axis: "h",
        track: mix.fader,
        stepPos: 0.01,
        def: 25,
        onDrag: (on) => {
            mixDragging = on;
            ctx.setDragging("mix", on);
            renderMix();
        },
        timers: ctx.timers,
    });
    attachReadoutEdit(mix.readout, { onEdit: editMix, timers: ctx.timers });

    // ---- 数值输入
    function editFeedback() {
        const unit = ctx.mem.fbUnit;
        const v = fb.value() ?? 0;
        ctx.edit({
            cover: dial.main,
            initial: unit === "db" ? fbDbText(v) : String(Math.round(v)),
            field: "fb.input",
            label: ctx.view.t("in.fb.label"),
            fs: 18,
            h: 30,
            w: 70,
            parse: (text) => parseFeedbackInput(text, unit),
            onCommit: (res) => fb.commit(Math.round(Math.min(FB_MAX, Math.max(0, res.value)))),
            focusBack: dial.disc,
        });
    }
    function editMix() {
        ctx.edit({
            cover: mix.readout,
            initial: String(Math.round(mixP.value() ?? 0)),
            field: "mix.input",
            label: ctx.view.t("in.mix.label"),
            fs: 12,
            h: 22,
            w: 54,
            parse: (text) => {
                const n = Number(String(text).trim().replace(/%$/, ""));
                return Number.isFinite(n) && String(text).trim() !== "" && n >= 0 && n <= 100
                    ? { ok: true, value: n }
                    : { ok: false, min: "0", max: "100" };
            },
            onCommit: (res) => mixP.commit(Math.round(res.value)),
            focusBack: mix.fader,
        });
    }

    // ---- 渲染
    function renderDial() {
        const v = fb.value();
        if (v === null || !ctx.view) return;
        const view = ctx.view;
        const values = ctx.values();
        dial.update({
            value: v,
            unit: ctx.mem.fbUnit,
            limOff: values.limiter === false,
            drag: dragging,
            auto: fb.automating(),
            cut: Boolean(view.state.safety && view.state.safety.cut),
            loopOff: values.loop_on === false,
            lang: view.lang,
            t: view.t,
        });
        renderChip();
    }

    function renderChip() {
        if (!ctx.view || fb.value() === null) return;
        const view = ctx.view;
        const values = ctx.values();
        const safety = view.state.safety || {};
        const c = feedbackChip({
            cut: Boolean(safety.cut),
            fadeIn: safety.fadeIn === true,
            limiterOn: values.limiter !== false,
            feedback: values.feedback,
            mix: values.mix,
        });
        setHidden(chip, !c);
        if (!c) {
            setAttr(chip, "data-state", null);
            setText(chip, "");
            return;
        }
        setAttr(chip, "data-state", c.kind);
        setAttr(chip, "data-tone", c.tone);
        setText(chip, view.t(c.key, c.vals));
    }

    function renderMix() {
        const v = mixP.value();
        if (v === null || !ctx.view) return;
        mix.update({ value: v, drag: mixDragging, auto: mixP.automating(), t: ctx.view.t });
    }

    function renderSeg() {
        const t = ctx.view.t;
        seg.update({
            value: ctx.mem.fbUnit,
            labels: [t("in.fb.unit.pct"), t("in.fb.unit.db")],
            label: t("in.fb.label"),
        });
    }

    function renderAll() {
        if (!ctx.view) return;
        const t = ctx.view.t;
        setText(eyebrow, t("in.fb.eyebrow"));
        setText(loopLabel, t("in.loop.label"));
        setText(label, t("in.fb.label"));
        setAttr(root, "aria-label", t("in.fb.eyebrow"));
        const loopOn = loopP.value() !== false;
        loopToggle.update({ on: loopOn, label: t("in.loop.label"), title: t("in.loop.tip") });
        setAttr(loopWrap, "data-state", loopOn ? "on" : "off");
        renderDial();
        renderMix();
        renderSeg();
    }

    return {
        el: root,
        render: renderAll,
        /** 电平弹道要写液柱的节点。 */
        levels: mix.levels,
    };
}
