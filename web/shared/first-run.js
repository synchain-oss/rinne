// SPDX-License-Identifier: GPL-3.0-or-later
//
// 首启链：语言选择卡 → 使用规则页 → 导览。Input、Output 共用；分三层：
//   纯函数        要走哪几环、规则页用哪种按钮栏、什么时候必须立即关、恢复的实例要不要页脚提示（Node 里直接断言）；
//   状态机        createFirstRunMachine：当前在哪一环、按了什么写哪一位；没有 DOM，写位经调用方给的 write(名字, 参数)；
//   控制器        createFirstRun：把状态机接到三件界面上（lang-start.js、rules-page.js、tour.js）和键盘。
// 模块顶层没有副作用，也不触碰 document。
//
// 判据（快照里的全局位都是本机共用的，不存工程）：
//   - 两路首批快照到齐才判，而且只判一次；没到、本会话关过、恢复的实例（restored）、危险态（L2 或失控灯亮）都不弹；
//   - 规则页：已确认的版本 rulesAck 不等于 RULES_VERSION 就要弹；版本只在规则含义变化时加 1，改版后新实例重弹；
//   - 导览：本角色的引导没看过就要弹；Output 另看 Input 的（inputGuideSeen），Input 看过 Output 就跳过；
//   - 两样都不需要时整条链不出，语言卡也不出；需要时，没选过语言（langChosen 为假）才在最前面加语言卡。
// 写位：选语言 setLang；[开始导览] setRulesAck(版本) 后进导览；[跳过导览] setRulesAck 再 setGuideSeen(true)；
// [我已了解] setRulesAck；导览完成、跳过或按 Esc 都是 setGuideSeen(true)。重看（「?」→ 规则页 [继续导览]）不写任何位。
// 必须立即关：自动弹出的链条进行中出现 L2、失控灯亮或按了紧急切断，关掉叠层，不写位。不论怎么结束，本会话不再自动弹。
// 用户自己从「?」或关于面板打开的规则页（以及从那里接着重看的导览）是普通模态，危险态不自动关，紧急切断照常关掉一切叠层。
// Esc：重看的规则页随时可以 Esc；要确认的规则页只在环路激活（正在回送）时可以 Esc，Esc 只是关掉，不算确认。语言卡不响应 Esc。
// 恢复的实例不弹链，页脚改显示 hint.firstUseRestored，直到在规则页确认了当前版本。
// 写入闸：setLang、setRulesAck、setGuideSeen 不在 Input 写入闸的拦截名单里（快照没到也照常发）；首启链本身等快照到齐才开始。

import { makeT } from "./dom.js";
import { RULES_VERSION } from "./hard-rules.gen.js";
import { createLangStart } from "./lang-start.js";
import { createRulesPage, rulesGateOpen, RULES_GATE_MS } from "./rules-page.js";
import { createTour } from "./tour.js";

export { RULES_VERSION, RULES_GATE_MS, rulesGateOpen };

/**
 * 首启链要走的环。snap 是页面从 rinne.state 摘出的全局位：
 *   { langChosen, rulesAck, guideSeen, inputGuideSeen?, restored, l2 }；l2 为真表示 L2 或失控灯亮。
 * @returns {Array<"lang"|"rules"|"tour">}
 */
export function firstRunSteps(role, snap, closedThisSession, version = RULES_VERSION) {
    if (!snap || closedThisSession) return [];
    if (snap.restored === true || snap.l2 === true) return [];
    const needRules = snap.rulesAck !== version;
    const needTour = snap.guideSeen !== true && !(role === "output" && snap.inputGuideSeen === true);
    if (!needRules && !needTour) return [];
    const steps = [];
    if (snap.langChosen !== true) steps.push("lang");
    if (needRules) steps.push("rules");
    if (needTour) steps.push("tour");
    return steps;
}

/** 首启链里规则页的按钮栏：后面还要接导览是 first，否则 ack。 */
export const chainRulesVariant = (steps) => (steps.includes("tour") ? "first" : "ack");

/** 从「?」或关于面板打开规则页：已确认当前版本是重看（review），否则要确认（ack）。 */
export const reopenRulesVariant = (snap, version = RULES_VERSION) =>
    snap && snap.rulesAck === version ? "review" : "ack";

/** 恢复的实例而本机没确认过当前版本：页脚显示 hint.firstUseRestored。 */
export const needsRestoredHint = (snap, version = RULES_VERSION) =>
    Boolean(snap) && snap.restored === true && snap.rulesAck !== version;

/** 叠层开着时必须立即关掉的情形：L2（含切断）或失控灯亮。 */
export const mustClose = (live) => Boolean(live) && (live.l2 === true || live.guardLit === true);

/** 规则页能否 Esc：重看随时可以；要确认的只在环路激活时。 */
export const rulesEscapable = (variant, loopActive) => variant === "review" || loopActive === true;

/**
 * 首启状态机。phase：idle（没有叠层）| lang | rules | tour。
 * @param {{role:"input"|"output", write:(name:string, arg:unknown)=>void, version?:number}} o
 */
export function createFirstRunMachine(o) {
    const version = Number.isInteger(o.version) ? o.version : RULES_VERSION;
    let phase = "idle";
    let queue = [];
    let variant = null;
    let replay = false;
    let decided = false;
    let closed = false;
    /** 当前叠层是不是自动弹出的首启链（危险态只自动关它）。 */
    let chain = false;

    function advance() {
        phase = queue.length ? queue.shift() : "idle";
        if (phase === "idle") finish();
    }
    function finish() {
        phase = "idle";
        queue = [];
        variant = null;
        replay = false;
        chain = false;
        closed = true;
    }

    return {
        state: () => ({ phase, variant, replay, chain, closedThisSession: closed, decided }),
        /** 两路首批快照到齐时调用；只判一次。返回是否开了链。 */
        begin(snap, live) {
            if (decided) return false;
            decided = true;
            const steps = firstRunSteps(o.role, { ...snap, l2: mustClose(live) }, closed, version);
            if (!steps.length) {
                // 不弹的情形同样算这一会话判过了：危险态解除后不会半路弹出来
                closed = true;
                return false;
            }
            variant = chainRulesVariant(steps);
            queue = steps;
            chain = true;
            advance();
            return true;
        },
        pickLang(code) {
            if (phase !== "lang") return;
            o.write("setLang", code);
            advance();
        },
        /** 规则页按钮：skip、start、ack、close、continue。 */
        rulesAction(name) {
            if (phase !== "rules") return;
            if (name === "start" && variant === "first") {
                o.write("setRulesAck", version);
                advance();
            } else if (name === "skip" && variant === "first") {
                o.write("setRulesAck", version);
                o.write("setGuideSeen", true);
                finish();
            } else if (name === "ack" && variant === "ack") {
                o.write("setRulesAck", version);
                finish();
            } else if (name === "close" && variant === "review") {
                finish();
            } else if (name === "continue" && variant === "review") {
                replay = true;
                phase = "tour";
            }
        },
        /** 导览结束：done、skip、esc。重看不写位。 */
        tourEnd() {
            if (phase !== "tour") return;
            if (!replay) o.write("setGuideSeen", true);
            finish();
        },
        /** 规则页上按 Esc。返回是否关了。 */
        escape(loopActive) {
            if (phase !== "rules" || !rulesEscapable(variant, loopActive)) return false;
            finish();
            return true;
        },
        /** 立即关掉，不写位（L2、失控灯、紧急切断）。 */
        abort() {
            if (phase !== "idle") finish();
        },
        /** 每帧调用：自动弹出的链条开着而 mustClose 成立就关。返回是否关了。 */
        frame(live) {
            if (phase === "idle" || !chain || !mustClose(live)) return false;
            finish();
            return true;
        },
        /** 「?」与关于面板「查看使用规则」。叠层开着时不响应。 */
        reopen(snap) {
            if (phase !== "idle") return;
            decided = true;
            variant = reopenRulesVariant(snap, version);
            replay = false;
            chain = false;
            phase = "rules";
        },
        /** 预览的静态场景：直接摆到某一环。 */
        force(next, rulesVariant) {
            decided = true;
            queue = [];
            chain = false;
            replay = next === "tour";
            variant = next === "rules" ? rulesVariant || "first" : null;
            phase = next;
        },
    };
}

/**
 * 首启控制器。
 * @param {object} o
 * @param {"input"|"output"} o.role
 * @param {object} o.bridge 要有 setLang、setRulesAck、setGuideSeen
 * @param {HTMLElement} o.host 外壳的 top 层
 * @param {HTMLElement} o.shell 外壳根节点（导览量锚点用）
 * @param {HTMLElement[]} o.inert 叠层开着时设为 inert 的节点（外壳的 content 层）
 * @param {Array} o.steps 导览步骤表
 * @param {()=>void} [o.onChange] 叠层开关变化（页面据此更新遮罩态与紧急切断的 overScrim）
 * @param {Document} [o.doc]
 * @param {{setTimeout:Function, clearTimeout:Function}} [o.timers]
 */
export function createFirstRun(o) {
    const doc = o.doc || globalThis.document;
    const onChange = typeof o.onChange === "function" ? o.onChange : () => {};
    const write = (name, arg) => {
        try {
            Promise.resolve(o.bridge[name](arg)).catch(() => {});
        } catch {
            // 桥没接上：本机的全局位没写成，下次打开照常再弹
        }
    };
    const machine = createFirstRunMachine({ role: o.role, write });
    let snap = null;
    let live = {};
    let lang = "zh";
    /** 刚选的语言：宿主的下一帧把它带回来之前，叠层先用它。 */
    let picked = null;
    let auto = true;
    let still = false;
    let shown = "idle";

    const t = () => makeT(picked || lang);
    const lazy = { lang: null, rules: null, tour: null };
    const langCard = () =>
        lazy.lang ||
        (lazy.lang = createLangStart({
            role: o.role,
            host: o.host,
            inert: o.inert,
            onPick: (code) => {
                picked = code;
                machine.pickLang(code);
                sync();
            },
        }));
    const rules = () =>
        lazy.rules ||
        (lazy.rules = createRulesPage({
            role: o.role,
            host: o.host,
            inert: o.inert,
            timers: o.timers,
            onAction: (name) => {
                machine.rulesAction(name);
                sync();
            },
        }));
    const tour = () =>
        lazy.tour ||
        (lazy.tour = createTour({
            role: o.role,
            host: o.host,
            shell: o.shell,
            inert: o.inert,
            steps: o.steps,
            onEnd: () => {
                machine.tourEnd();
                sync();
            },
        }));

    /** 按状态机的当前环摆界面：先收起别的，再开当前的。 */
    function sync(scene) {
        const s = machine.state();
        if (s.phase !== "lang" && lazy.lang) lazy.lang.hide();
        if (s.phase !== "rules" && lazy.rules) lazy.rules.close();
        if (s.phase !== "tour" && lazy.tour) lazy.tour.stop();
        if (s.phase === "lang" && shown !== "lang") langCard().show(t());
        if (s.phase === "rules" && shown !== "rules")
            rules().open({ variant: s.variant, t: t(), still, gated: scene && scene.gated });
        if (s.phase === "tour" && shown !== "tour") tour().start({ step: (scene && scene.step) || 1, t: t() });
        const changed = shown !== s.phase;
        shown = s.phase;
        if (changed) onChange();
    }

    function onKey(e) {
        if (shown === "rules" && e.key === "Escape") {
            if (machine.escape(live.loopActive === true)) {
                e.preventDefault();
                sync();
            }
        } else if (shown === "tour" && lazy.tour.key(e)) {
            e.preventDefault();
            e.stopPropagation();
        }
    }
    if (doc) doc.addEventListener("keydown", onKey, true);

    return {
        machine,
        isOpen: () => shown !== "idle",
        phase: () => shown,
        restoredHint: () => needsRestoredHint(snap),
        parts: () => ({ ...lazy }),
        /** 预览里给了别的场景：场景描述的是一种既定画面，不自动叠首启链。 */
        disableAuto() {
            auto = false;
        },
        /**
         * 每次快照（两路都到齐之后）调用。
         * @param {object} nextSnap firstRunSteps 的 snap 字段，外加 lang
         * @param {{l2?:boolean, guardLit?:boolean, loopActive?:boolean}} nextLive
         */
        update(nextSnap, nextLive) {
            snap = nextSnap;
            live = nextLive || {};
            const nextLang = ["zh", "en", "fr"].includes(snap.lang) ? snap.lang : "zh";
            const relang = nextLang !== lang || (picked && picked === nextLang);
            lang = nextLang;
            if (picked === lang) picked = null;
            if (machine.frame(live)) sync();
            else if (auto && !still && machine.begin(snap, live)) sync();
            if (relang && shown === "rules") lazy.rules.retext(t());
            // 导览开着时每帧重量一次：快照会改亮区里那几件的高（配对卡的副行之类）
            if (shown === "tour") {
                if (relang) lazy.tour.retext(t());
                else lazy.tour.relayout();
            }
        },
        /** 「?」与关于面板「查看使用规则」。 */
        openRules() {
            // 快照没到：不知道本机确认过哪个版本，不开
            if (!snap) return;
            machine.reopen(snap);
            sync();
        },
        /** 紧急切断：叠层立即关掉，不写位。 */
        abort() {
            machine.abort();
            sync();
        },
        /**
         * 预览的静态场景：overlay 为 langStart、rules、tour 时直接摆出来（规则页不起放行计时）。
         * @param {{overlay:string, rules?:string, step?:number}} sc
         */
        showScene(sc) {
            if (!sc || !["langStart", "rules", "tour"].includes(sc.overlay)) return false;
            still = true;
            const variant = sc.rules === "review" ? "review" : "first";
            machine.force(sc.overlay === "langStart" ? "lang" : sc.overlay, variant);
            sync({
                step: sc.step,
                gated: sc.overlay === "rules" ? sc.rules !== "ready" && variant !== "review" : undefined,
            });
            return true;
        },
        destroy() {
            if (doc) doc.removeEventListener("keydown", onKey, true);
        },
    };
}
