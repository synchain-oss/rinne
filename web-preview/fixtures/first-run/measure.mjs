// SPDX-License-Identifier: GPL-3.0-or-later
//
// 首启场景（语言卡、规则页、导览）进适配 / 对比度 / 无障碍套件时，Input 与 Output 两边共用的常量与页内函数。
// 探针本身两边各写各的（Input 的探针在 smoke-input-kit-page.mjs，Output 的由 fixtures/output/lib.mjs 在共用探针上改出来），
// 这里只放两边必须一致的东西，免得两边各抄一份慢慢漂开；探针里两边必须取同一种算法的小块（滚动区裁剪、可访问名称）
// 也写成页内函数放在这里，两边都从这一份源码带进页面（见 inlineProbe）。

import { findLeaks as findBaseLeaks } from "../../tests/shared-measure.mjs";

/**
 * 界面上不该出现的东西：共用的那一份（in / out / common / band / about / toast 前缀的裸词条键、没替换的占位符、null / undefined）
 * 之外，再认首启词条的前缀 rules.、tour.、hint.：词条缺了时界面直接显示键本身，共用的那一份认不出这三个前缀。
 */
export function findLeaks(text) {
    const hits = findBaseLeaks(text);
    for (const m of text.matchAll(/\b(?:rules|tour|hint)\.[A-Za-z][A-Za-z0-9]*(?:\.[A-Za-z][A-Za-z0-9]*)*/g))
        hits.push(m[0]);
    return hits;
}

/**
 * 首启链的场景 ID：Input 的 in-lang / in-rules / in-tour-N（共 8 个），Output 的 out-lang / out-rules / out-tour-N（共 6 个）。
 * 两边的套件都从这里认首启场景，不各自抄一份正则。
 */
export const isFirstRunSceneId = (id) => /^(in|out)-(lang|rules|tour-[0-9]+)$/.test(id);

/** 场景 ID 对应的叠层：{ kind: "lang" | "rules" | "tour", step?: 导览第几步 }；不是首启场景返回 null。 */
export function expectedOverlay(id) {
    const m = /^(?:in|out)-(lang|rules|tour)(?:-([0-9]+))?$/.exec(id);
    if (!m) return null;
    return m[1] === "tour" ? { kind: "tour", step: Number(m[2]) } : { kind: m[1] };
}

/**
 * 文字在祖先的滚动区里（规则页正文）：字框只取滚动区露出来的那一段（纵向）。段落比滚动区还高时（法文的规则页），
 * 越出去的部分被裁掉，那里露出来的是滚动区外面的东西（页脚的按钮），不是这段字的底，量进去就是假红。
 * box 是 { left, top, right, bottom }，sc 是最近的滚动祖先（没有则 null，原样返回）；返回裁过的同形状的 box。
 * Input、Output 两边的对比度探针都用这一个函数；页内求值，不能引用外面的变量。
 */
export function clipToAncestorScroller(box, sc) {
    if (!sc) return box;
    const sr = sc.getBoundingClientRect();
    return {
        left: box.left,
        top: Math.max(box.top, sr.top),
        right: box.right,
        bottom: Math.min(box.bottom, sr.bottom),
    };
}

/**
 * 简化版可访问名称，取法与 Output 名称审计（fixtures/output/lib.mjs 的 NAME_AUDIT）一致：
 * aria-labelledby 指到的文字 > aria-label > 文字 > title；原生下拉不取文字（那是当前值，不是名称）。
 * 叠层按钮的名称（overlayA11yProbe）与名称审计用这一个函数，顺序不会各写各的；页内求值，不能引用外面的变量。
 */
export function accessibleName(e, doc) {
    const by = e.getAttribute("aria-labelledby");
    if (by) {
        const txt = by
            .split(/\s+/)
            .map((id) => (doc.getElementById(id) || {}).textContent || "")
            .join(" ")
            .trim();
        if (txt) return txt;
    }
    const al = (e.getAttribute("aria-label") || "").trim();
    if (al) return al;
    const tc = (e.textContent || "").trim();
    if (tc && e.tagName !== "SELECT") return tc;
    return (e.getAttribute("title") || "").trim();
}

/**
 * 把一个页内函数序列化成一段求值为该函数的表达式源码。函数靠 toString 送进页面，引用不到模块里的东西；
 * 它用到的共用函数（clipToAncestorScroller、accessibleName）挂在 fn.deps 上，这里把它们的源码先声明在函数外面一层，
 * 函数里就能按名字用。没有 deps 的函数原样返回。用法：`(${inlineProbe(fn)})(实参...)`。
 */
export function inlineProbe(fn) {
    const deps = (fn.deps || []).map((d) => d.toString()).join("\n");
    return deps ? `(() => { ${deps}\n return (${fn.toString()}); })()` : `(${fn.toString()})`;
}

/**
 * 叠层的无障碍现状（页内求值，两边的无障碍冒烟共用：Input 经 iframe，Output 直接在夹具页里；函数不能引用外面的变量）。
 * 返回 { open, kind, role, modal, name, contentInert, focusIn, step, buttons, rules }：
 *   open        此刻打开着的叠层种类（应恰好一个）；kind 是其中唯一的那个；
 *   role / modal / name   叠层根节点的 role、aria-modal 与可访问名称（aria-labelledby 指向的文字，或 aria-label）；
 *   contentInert 外壳的内容层是否 inert（叠层之下的页面不能被点到、Tab 到）；
 *   focusIn     键盘焦点是否在叠层根节点之内；
 *   step        导览当前步（data-state=step-N），其余为 null；
 *   buttons     叠层里的按钮 [{ part, name, disabled, title }]（名称取法同名称审计，见 accessibleName，没有则空串）；
 *   rules       规则页的 data-state 与正文区能否用键盘到达（tabindex 为 0），其余为 null。
 */
export function overlayA11yProbe(doc, win, role) {
    const visible = (e) => !!e && !e.closest("[hidden]");
    const kinds = { lang: `${role}.langStart`, rules: `${role}.rules`, tour: `${role}.tour` };
    const open = Object.keys(kinds).filter((k) => visible(doc.querySelector(`[data-gb="${kinds[k]}"]`)));
    const kind = open.length === 1 ? open[0] : null;
    const root = kind ? doc.querySelector(`[data-gb="${kinds[kind]}"]`) : null;
    const content = doc.querySelector(`[data-gb="${role}.shell"] > [data-part="content"]`);
    const textOfIds = (ids) =>
        ids
            .split(/\s+/)
            .map((id) => (doc.getElementById(id) || {}).textContent || "")
            .join(" ")
            .trim();
    let name = "";
    if (root) {
        const by = root.getAttribute("aria-labelledby");
        name = (by ? textOfIds(by) : "") || (root.getAttribute("aria-label") || "").trim();
    }
    const buttons = root
        ? [...root.querySelectorAll("button")].map((b) => ({
              part: b.getAttribute("data-part") || "",
              name: accessibleName(b, doc),
              disabled: b.disabled,
              title: b.getAttribute("title") || "",
          }))
        : [];
    const body = kind === "rules" ? root.querySelector('[data-part="rules.body"]') : null;
    return {
        open,
        kind,
        role: root ? root.getAttribute("role") : null,
        modal: root ? root.getAttribute("aria-modal") : null,
        name,
        contentInert: Boolean(content && content.hasAttribute("inert")),
        focusIn: Boolean(root && root.contains(doc.activeElement)),
        step: kind === "tour" ? Number((root.getAttribute("data-state") || "").replace("step-", "")) || null : null,
        buttons,
        rules: body ? { state: root.getAttribute("data-state"), bodyTabbable: body.tabIndex === 0 } : null,
    };
}

overlayA11yProbe.deps = [accessibleName];

/**
 * 「只有 aria-labelledby 的按钮不算没有名称」这条反例的摆法（页内求值，两边的无障碍冒烟共用）：
 * 把 sel 选中的按钮改成没有文字、没有 aria-label、没有 title，只靠 aria-labelledby 指到两个有字的节点
 * （两个 id 用空白隔开、都带字母 s：按空白切分的写法才取得到，错切成按字母 s 切就取不到）。返回此时应得的名称。
 */
export function stripToLabelledBy(doc, win, sel) {
    const b = doc.querySelector(sel);
    b.textContent = "";
    b.removeAttribute("aria-label");
    b.removeAttribute("title");
    for (const [id, text] of [
        ["fx-lbl-s1", "甲"],
        ["fx-lbl-s2", "乙"],
    ]) {
        const e = doc.createElement("span");
        e.id = id;
        e.hidden = true;
        e.textContent = text;
        doc.body.append(e);
    }
    b.setAttribute("aria-labelledby", "fx-lbl-s1 fx-lbl-s2");
    return "甲 乙";
}

/**
 * 截图量对比度时，在共用的「文字透明」之外多藏的东西：
 *   SVG 文字：SVG 文字画的是 fill 不是 color，共用的透明样式管不到，不藏的话截图里还留着字，文字色合成上去就是 1:1；
 *   插入链示意的描边：规则页旁那张示意图里，槽的描边与推子竖线是穿过标签字框的图形，不是文字的底；逐像素取最差时，
 *     与字同色的描边会被当成「底」，量出 1.00 的假红。只藏这一张示意图自己的描边，别的 SVG 不动（别处的描边可能真的是文字的底）。
 */
export const SVG_HIDE_STYLE =
    " svg text, svg tspan { fill: transparent !important; }" +
    " .rn-chain svg rect, .rn-chain svg line { stroke: transparent !important; }";

/**
 * 首启叠层的无障碍问题清单，空表示都对。o 是 overlayA11yProbe 的结果，frame 是 { scene, sub }。
 * 角色、名称、内容层 inert、焦点都在叠层里；按钮的名称与放行状态：规则页未放行时两颗按钮真禁用并带 title，放行后可用；
 * 导览第 1 步的「上一步」禁用、其余步可用；lastStep 是导览的总步数（最后一步的按钮是 tour.done）。
 * Input、Output 两边的无障碍冒烟共用。
 */
export function overlayProblems(o, frame, lastStep) {
    const want = expectedOverlay(frame.scene.id);
    if (o.kind !== want.kind) return [`叠层应是 ${want.kind}，实得 ${JSON.stringify(o.open)}`];
    const out = [];
    if (o.role !== "dialog") out.push(`role 应是 dialog，实得 ${o.role}`);
    if (o.modal !== "true") out.push(`aria-modal 应是 true，实得 ${o.modal}`);
    if (!o.name) out.push("叠层没有可访问名称（aria-labelledby 指不到有字的节点）");
    if (!o.contentInert) out.push("叠层打开时内容层应是 inert");
    if (!o.focusIn) out.push("键盘焦点应在叠层之内");
    for (const b of o.buttons) if (!b.name) out.push(`按钮 ${b.part} 没有名称`);
    const parts = o.buttons.map((b) => b.part);
    const wantParts = {
        lang: ["langStart.zh", "langStart.en", "langStart.fr"],
        rules: ["rules.skip", "rules.start"],
        tour: ["tour.skip", "tour.prev", want.step === lastStep ? "tour.done" : "tour.next"],
    }[want.kind];
    if (JSON.stringify(parts) !== JSON.stringify(wantParts))
        out.push(`按钮应是 ${JSON.stringify(wantParts)}，实得 ${JSON.stringify(parts)}`);
    if (want.kind === "rules") {
        const gated = frame.sub !== "ready";
        if (!o.rules || !o.rules.bodyTabbable) out.push("规则页正文区应能用键盘到达（tabindex=0）");
        if (o.rules && o.rules.state !== (gated ? "gated" : "ready"))
            out.push(`规则页应是 ${gated ? "gated" : "ready"}`);
        for (const b of o.buttons) {
            if (b.disabled !== gated)
                out.push(`按钮 ${b.part} 在${gated ? "未放行" : "放行后"}应${gated ? "禁用" : "可用"}`);
            if (gated && !b.title) out.push(`禁用的按钮 ${b.part} 应带 title 说明原因`);
            if (!gated && b.title) out.push(`放行后的按钮 ${b.part} 不该再带「请先读」的 title`);
        }
    }
    if (want.kind === "tour") {
        for (const b of o.buttons) {
            const off = b.part === "tour.prev" && want.step === 1;
            if (b.disabled !== off) out.push(`导览第 ${want.step} 步的按钮 ${b.part} 应${off ? "禁用" : "可用"}`);
        }
    }
    return out;
}
