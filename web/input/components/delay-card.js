// SPDX-License-Identifier: GPL-3.0-or-later
//
// 环路延迟卡（中栏下，248×243）：毫秒 / 采样 / 节拍三档、读数、对数滑杆（1–4000 ms）和四行说明（实际值、最小值、
// 本会话最大块长、总延迟）加原因徽标。
//   毫秒与采样是同一个参数（delay_ms）的两种读法，选哪种只在页面内存里（cfg 里目前没有写入它的桥函数），
//   节拍档把 delay_mode 切到 sync，时值由 delay_note 决定，由滑杆位置上的十六档下拉选（选项顺序即取值表的顺序，
//   选中走 setParam 一套；三连音与附点档的悬停说明挂在下拉上）。
//   D 低于最小值时滑杆左段画斜纹、把手变琥珀；实际值高于设定值时第一行变琥珀并带向上箭头。

import { el, svg } from "../../shared/dom.js";
import { icon } from "../../shared/icons.js";
import {
    DELAY_MAX_MS,
    DELAY_MIN_MS,
    NOTE_IDS,
    delayFromPos,
    delayPos,
    delayView,
    fmtFixed,
    msToSamples,
    parseDelayInput,
    quantizeDelayMs,
} from "../model.js";
import { createSeg } from "./seg.js";
import { attachControl, attachReadoutEdit } from "./gesture.js";
import { setAttr, setHidden, setLine, setText, setVar } from "./util.js";

/** @param {object} ctx 页面上下文 */
export function createDelayCard(ctx) {
    let dragging = false;

    const ms = ctx.param("delay_ms", { epsilon: 0.01, onDisplay: () => renderAll(), onAuto: () => renderAll() });
    const mode = ctx.param("delay_mode", { kind: "enum", onDisplay: () => renderAll() });
    const note = ctx.param("delay_note", { kind: "enum", onDisplay: () => renderAll() });

    const sampleRate = () => (ctx.view && ctx.view.state.sampleRate) || 48000;
    const unit = () => ctx.mem.delayUnit;

    // ---- 静态结构
    const eyebrow = el("span", { class: "sc-eyebrow in-dly__eyebrow" });
    const seg = createSeg({
        part: "dly.unit",
        ids: ["ms", "samples", "sync"],
        onPick: (id) => {
            if (ctx.locked()) return;
            if (id === "sync") {
                if (mode.value() !== "sync") mode.commit("sync");
                return;
            }
            ctx.setUnit("delay", id);
            if (mode.value() !== "time") mode.commit("time");
            renderAll();
        },
    });
    const big = el("span", { class: "in-dly__big", "data-part": "dly.readout" });
    const bigUnit = el("span", { class: "in-dly__unit" });
    const readoutSlot = el("span", { class: "in-dly__readout" }, big, bigUnit);
    const sub = el("div", { class: "in-dly__sub" });

    const hatch = el("div", { class: "in-dly__hatch", "data-part": "hatch" });
    const fill = el("div", { class: "in-dly__fill" });
    const tick = el("div", { class: "in-dly__tick", "data-part": "min-tick" });
    const tickLabel = el("span", { class: "in-dly__tick-label" });
    const knob = el("div", { class: "in-dly__knob" });
    const slider = el(
        "div",
        {
            class: "in-dly__slider",
            role: "slider",
            tabindex: "0",
            "data-glow": "1",
            "data-part": "dly.slider",
            aria: { valuemin: DELAY_MIN_MS, valuemax: DELAY_MAX_MS },
        },
        hatch,
        el("div", { class: "in-dly__rail" }),
        fill,
        tick,
        tickLabel,
        knob,
    );
    const noteSelect = el(
        "select",
        { class: "in-dly__select", "data-part": "dly.note" },
        ...NOTE_IDS.map((id) => el("option", { value: id })),
    );
    noteSelect.addEventListener("change", () => {
        if (!ctx.locked() && note.value() !== noteSelect.value) note.commit(noteSelect.value);
        // 没写成（快照没到、宿主不认）就把下拉拉回当前值，选项不留在没生效的档上
        renderAll();
    });
    const noteSlot = el(
        "div",
        { class: "in-dly__note", "data-slot": "note-select" },
        noteSelect,
        el("span", { class: "in-dly__selchev" }, icon("chev", 10, "currentColor")),
    );
    const control = el("div", { class: "in-dly__control" }, slider, noteSlot);

    const up = svg(
        "svg",
        {
            width: 10,
            height: 10,
            viewBox: "0 0 16 16",
            fill: "none",
            stroke: "var(--sem-amber-icon)",
            "stroke-width": 1.8,
            "stroke-linecap": "round",
            "stroke-linejoin": "round",
            "aria-hidden": "true",
            class: "in-dly__up",
        },
        svg("path", { d: "M8 13V3" }),
        svg("path", { d: "M4 7l4-4 4 4" }),
    );
    const actual = el("span", { class: "in-dly__line-text", "data-part": "dly.actual" });
    const actualRow = el("div", { class: "in-dly__row in-dly__row--actual" }, up, actual);
    const minText = el("span", { class: "in-dly__line-text" });
    const minRow = el("div", { class: "in-dly__row in-dly__row--min" }, minText);
    const maxText = el("span", { class: "in-dly__line-text" });
    const maxRow = el("div", { class: "in-dly__row in-dly__row--max" }, maxText);
    const totalText = el("span", { class: "in-dly__total" });
    const totalRow = el("div", { class: "in-dly__row in-dly__row--total" }, totalText);
    const badge = el("span", { class: "sc-badge sc-badge--amber in-dly__badge", "data-part": "dly.badge" });
    const badgeRow = el("div", { class: "in-dly__row in-dly__row--badge" }, badge);

    const root = el(
        "section",
        { class: "sc-card in-card in-dly", "data-gb": "input.dly", "data-tour": "delay" },
        el("div", { class: "in-dly__head" }, eyebrow),
        el("div", { class: "in-dly__seg" }, seg.el),
        el("div", { class: "in-dly__big-row" }, readoutSlot),
        sub,
        control,
        actualRow,
        minRow,
        maxRow,
        totalRow,
        badgeRow,
    );

    // ---- 手势：滑杆在对数轴上，毫秒档取 0.1 ms、采样档取整采样
    const quantize = (v) => quantizeDelayMs(v, unit(), sampleRate());
    attachControl(slider, {
        param: ms,
        toPos: (v) => delayPos(v) / 100,
        fromPos: (p) => delayFromPos(p * 100),
        quantize,
        axis: "h",
        track: slider,
        stepPos: 0.005,
        def: 100,
        onEdit: edit,
        onDrag: (on) => {
            dragging = on;
            ctx.setDragging("delay", on);
            renderAll();
        },
        disabled: () => mode.value() === "sync",
        timers: ctx.timers,
    });
    attachReadoutEdit(big, {
        onEdit: edit,
        timers: ctx.timers,
        param: ms,
        disabled: () => mode.value() === "sync",
    });

    function edit() {
        const sr = sampleRate();
        const u = unit();
        const cur = ms.value() ?? 100;
        ctx.edit({
            cover: big,
            initial: u === "samples" ? String(msToSamples(cur, sr)) : fmtFixed(cur, 1),
            field: "dly.input",
            label: ctx.view.t("in.dly.eyebrow"),
            fs: 16,
            h: 28,
            w: 92,
            parse: (text) => parseDelayInput(text, u, sr),
            onCommit: (res) => ms.commit(quantize(res.ms)),
            focusBack: slider,
        });
    }

    // ---- 渲染
    function view() {
        const v = ctx.view;
        return delayView({
            mode: mode.value() === "sync" ? "sync" : "time",
            unit: unit(),
            delayMs: ms.value() ?? 100,
            note: note.value() || "n4",
            delay: v.state.delay,
            sampleRate: sampleRate(),
        });
    }

    function renderAll() {
        if (!ctx.view || ms.value() === null) return;
        const t = ctx.view.t;
        const m = view();
        setText(eyebrow, t("in.dly.eyebrow"));
        setAttr(eyebrow, "title", t("in.dly.eyebrowTip"));
        seg.update({
            value: m.segment,
            labels: [t("in.dly.unit.ms"), t("in.dly.unit.samples"), t("in.dly.unit.sync")],
            label: t("in.dly.eyebrow"),
        });
        if (m.readout.kind === "note") {
            setText(big, t(m.readout.noteKey));
            setHidden(bigUnit, true);
        } else {
            setText(big, m.readout.text);
            setText(bigUnit, t(m.readout.unitKey));
            setHidden(bigUnit, false);
        }
        setLine(sub, t(m.sub.key, m.sub.vals));

        // 滑杆（节拍档换成时值下拉的槽位）
        setHidden(slider, m.sync);
        setHidden(noteSlot, !m.sync);
        renderNoteSelect(t, note.value() || "n4");
        setVar(slider, "--v", `${m.sliderPct}%`);
        setAttr(slider, "aria-valuenow", fmtFixed(m.setMs, 1));
        setAttr(slider, "aria-valuetext", `${fmtFixed(m.setMs, 1)} ms`);
        setAttr(slider, "aria-label", t("in.dly.eyebrow"));
        setAttr(slider, "data-state", m.inHatch ? "inHatch" : dragging ? "drag" : ms.automating() ? "auto" : "normal");
        const hasMin = m.minPct !== null;
        setHidden(hatch, !hasMin);
        setHidden(tick, !hasMin);
        setHidden(tickLabel, !hasMin);
        if (hasMin) {
            setVar(slider, "--min", `${m.minPct}%`);
            setText(tickLabel, t("in.dly.minMark"));
            setAttr(hatch, "title", t("in.dly.hatchTip"));
        }

        // 四行说明
        setLine(actual, t("in.dly.actual", m.actualVals));
        setHidden(up, !m.raised);
        setAttr(actualRow, "data-tone", m.raised ? "amber" : null);
        if (m.minKnown) setLine(minText, t("in.dly.min", m.minVals));
        else setLine(minText, t("in.dly.minWait"));
        const maxBlock = ctx.view.state.delay && ctx.view.state.delay.maxBlock;
        setAttr(minRow, "title", t("in.dly.minTip", { n: String(Number.isFinite(maxBlock) ? maxBlock : 0) }));
        setLine(maxText, t("in.dly.maxBlock", m.maxBlockVals));
        setAttr(maxRow, "title", t("in.dly.maxBlockTip"));
        setText(totalText, t("in.dly.total"));
        setHidden(badge, !m.badge);
        if (m.badge) setText(badge, t("in.dly.badge." + m.badge));
        setAttr(root, "data-state", m.state);
    }

    /** 十六档时值下拉：标签三语，选中项跟 delay_note，三连音与附点档带一句悬停说明。 */
    function renderNoteSelect(t, cur) {
        const opts = noteSelect.options;
        for (let i = 0; i < opts.length; i += 1) setText(opts[i], t("in.dly.note." + opts[i].value));
        if (noteSelect.value !== cur) noteSelect.value = cur;
        setAttr(noteSelect, "aria-label", t("in.dly.eyebrow"));
        setAttr(
            noteSelect,
            "title",
            /t$/.test(cur) ? t("in.dly.noteTipT") : /d$/.test(cur) ? t("in.dly.noteTipD") : null,
        );
        if (noteSelect.disabled !== ctx.locked()) noteSelect.disabled = ctx.locked();
    }

    /** 宿主的快照还没到：标签照写，读数写「—」，其余各行留空，滑杆与单位分段关着。 */
    function renderPending(t) {
        setText(eyebrow, t("in.dly.eyebrow"));
        setAttr(eyebrow, "title", t("in.dly.eyebrowTip"));
        seg.update({
            value: "",
            labels: [t("in.dly.unit.ms"), t("in.dly.unit.samples"), t("in.dly.unit.sync")],
            label: t("in.dly.eyebrow"),
            disabled: true,
        });
        setText(big, "—");
        setHidden(bigUnit, true);
        setLine(sub, "");
        setHidden(slider, true);
        setHidden(noteSlot, true);
        setAttr(slider, "aria-valuenow", null);
        setAttr(slider, "aria-valuetext", "—");
        setAttr(slider, "aria-label", t("in.dly.eyebrow"));
        setLine(actual, "");
        setHidden(up, true);
        setAttr(actualRow, "data-tone", null);
        setLine(minText, "");
        setLine(maxText, "");
        setText(totalText, "");
        setHidden(badge, true);
        setAttr(root, "data-state", "pending");
    }

    return { el: root, render: renderAll, renderPending };
}
