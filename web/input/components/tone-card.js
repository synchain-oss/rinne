// SPDX-License-Identifier: GPL-3.0-or-later
//
// 回送音色卡（中栏上，248×149）：低切、高切两只小表盘和极性反转开关。只作用于回送，干声不受影响。
// 低切的弧从起点画（20 Hz 即「关」），高切的弧从末端往回画（20000 Hz 即「关」）。

import { el } from "../../shared/dom.js";
import {
    HIGH_CUT,
    LOW_CUT,
    highCutFromPos,
    highCutPos,
    isHighCutOff,
    isLowCutOff,
    lowCutFromPos,
    lowCutPos,
    parseToneInput,
    toneValueText,
    fmtHz,
} from "../model.js";
import { createMiniDial } from "./mini-dial.js";
import { createToggle } from "./toggle.js";
import { attachControl, attachReadoutEdit } from "./gesture.js";
import { setAttr, setHidden, setText } from "./util.js";

/** @param {object} ctx 页面上下文 */
export function createToneCard(ctx) {
    const drags = { low: false, high: false };

    const kinds = {
        low: {
            id: "low_cut",
            range: LOW_CUT,
            pos: lowCutPos,
            fromPos: lowCutFromPos,
            off: isLowCutOff,
            labelKey: "in.tone.lowcut",
            halo: "var(--halo-1)",
            fromEnd: false,
        },
        high: {
            id: "high_cut",
            range: HIGH_CUT,
            pos: highCutPos,
            fromPos: highCutFromPos,
            off: isHighCutOff,
            labelKey: "in.tone.highcut",
            halo: "var(--halo-3)",
            fromEnd: true,
        },
    };

    const cols = {};
    for (const [kind, k] of Object.entries(kinds)) {
        const param = ctx.param(k.id, {
            epsilon: 0.2,
            onDisplay: () => renderKind(kind),
            onAuto: () => renderKind(kind),
        });
        const dial = createMiniDial({ size: 36, halo: k.halo, fromEnd: k.fromEnd, glow: true, part: `tone.${kind}` });
        const label = el("span", { class: "in-tone__label" });
        const readout = el("span", { class: "in-tone__readout", "data-part": "tone.readout" });
        const off = el("span", { class: "in-tone__off" });
        const slot = el("span", { class: "in-tone__slot" }, readout, off);
        const col = el("div", { class: "in-tone__col", "data-kind": kind }, dial.el, label, slot);
        const edit = () =>
            ctx.edit({
                cover: readout,
                initial: String(Math.round(param.value() ?? k.range.min)),
                field: "tone.input",
                label: ctx.view.t(k.labelKey),
                fs: 10,
                h: 18,
                w: 60,
                parse: (text) => parseToneInput(text, kind),
                onCommit: (res) => param.commit(res.value),
                focusBack: dial.el,
            });
        attachControl(dial.el, {
            param,
            toPos: k.pos,
            fromPos: k.fromPos,
            quantize: Math.round,
            stepPos: 0.01,
            def: kind === "low" ? LOW_CUT.min : HIGH_CUT.max,
            onEdit: edit,
            onDrag: (on) => {
                drags[kind] = on;
                ctx.setDragging(`tone.${kind}`, on);
                renderKind(kind);
            },
            timers: ctx.timers,
        });
        attachReadoutEdit(readout, {
            onEdit: edit,
            timers: ctx.timers,
            param,
            disabled: () => k.off(param.value() ?? 0),
        });
        cols[kind] = { param, dial, label, readout, off, col, k };
    }

    const polP = ctx.param("polarity", { kind: "bool", onDisplay: () => renderPolarity() });
    const polLabel = el("span", { class: "in-tone__pol-label" });
    const polToggle = createToggle({
        part: "tone.polarity.toggle",
        green: true,
        onToggle: (next) => polP.commit(next),
    });
    const polRow = el(
        "div",
        { class: "in-tone__pol", "data-part": "tone.polarity" },
        polLabel,
        el("span", { class: "in-tone__pol-toggle" }, polToggle.el),
    );
    const eyebrow = el("span", { class: "sc-eyebrow in-tone__eyebrow" });
    const root = el(
        "section",
        { class: "sc-card in-card in-tone", "data-gb": "input.tone" },
        el("div", { class: "in-tone__head" }, eyebrow),
        el("div", { class: "in-tone__dials" }, cols.low.col, cols.high.col),
        polRow,
    );

    function renderKind(kind) {
        const c = cols[kind];
        if (!ctx.view) return;
        const hz = c.param.value();
        if (hz === null) return;
        const t = ctx.view.t;
        const isOff = c.k.off(hz);
        const offText = t("in.tone.off");
        setText(c.label, t(c.k.labelKey));
        setText(c.readout, fmtHz(hz));
        setText(c.off, offText);
        setHidden(c.readout, isOff);
        setHidden(c.off, !isOff);
        c.dial.update({
            pos: c.k.pos(hz),
            valueText: toneValueText(kind, hz, offText),
            label: t(c.k.labelKey),
            title: undefined,
            drag: drags[kind],
            auto: c.param.automating(),
            disabled: false,
        });
        setAttr(c.col, "data-state", isOff ? "off" : "on");
        renderState();
    }

    function renderPolarity() {
        if (!ctx.view) return;
        const t = ctx.view.t;
        const on = polP.value() === true;
        setText(polLabel, t("in.tone.polarity"));
        polToggle.update({ on, label: t("in.tone.polarity") });
        setAttr(polRow, "data-state", on ? "on" : "off");
        renderState();
    }

    function renderState() {
        const lo = cols.low.param.value();
        const hi = cols.high.param.value();
        const part = (v, k, name) => (v !== null && !k.off(v) ? `${name}:${Math.round(v)}` : `${name}:off`);
        setAttr(
            root,
            "data-state",
            `${part(lo, kinds.low, "low")}|${part(hi, kinds.high, "high")}|pol:${polP.value() ? "on" : "off"}`,
        );
    }

    return {
        el: root,
        /** 宿主的快照还没到：标签照写，读数写「—」，两只小表盘与极性开关关着。 */
        renderPending(t) {
            setText(eyebrow, t("in.tone.eyebrow"));
            setAttr(eyebrow, "title", t("in.tone.eyebrowTip"));
            setAttr(root, "aria-label", t("in.tone.eyebrow"));
            for (const c of Object.values(cols)) {
                setText(c.label, t(c.k.labelKey));
                setText(c.readout, "—");
                setHidden(c.readout, false);
                setHidden(c.off, true);
                setAttr(c.dial.el, "aria-label", t(c.k.labelKey));
                setAttr(c.dial.el, "aria-valuenow", null);
                setAttr(c.dial.el, "aria-valuetext", "—");
                setAttr(c.dial.el, "aria-disabled", "true");
                setAttr(c.dial.el, "data-state", "disabled");
                setAttr(c.col, "data-state", "pending");
            }
            setText(polLabel, t("in.tone.polarity"));
            polToggle.update({ on: false, label: t("in.tone.polarity"), disabled: true });
            setAttr(polRow, "data-state", "pending");
            setAttr(root, "data-state", "pending");
        },
        render() {
            if (!ctx.view) return;
            setText(eyebrow, ctx.view.t("in.tone.eyebrow"));
            setAttr(eyebrow, "title", ctx.view.t("in.tone.eyebrowTip"));
            setAttr(root, "aria-label", ctx.view.t("in.tone.eyebrow"));
            renderKind("low");
            renderKind("high");
            renderPolarity();
        },
    };
}
