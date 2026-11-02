// SPDX-License-Identifier: GPL-3.0-or-later
//
// Output 页面夹具：把真页面（web/output/page.js）挂进 #stage，桥是 mock 后端，观测接口挂在 window.__fx 上给冒烟脚本。
// 与 shared 夹具同口径（探针硬编码根 #stage，登记「必须上屏」的文字），区别是这里摆的就是整页，不是单个共享件。
//   __fx.ready          页面已揭幕、字体已就绪、首帧已画；
//   __fx.calls          页面调过的桥函数 [{ name, args }]；
//   __fx.expectedTexts  此刻页上必须看得见的文字（按快照里的视图模型现算）；
//   __fx.mutate(fn)     改 mock 的模型并立即发一轮帧（场景之外的状态组合用它造）；
//   __fx.emit(name, p)  直接往页面发一个桥事件（乱序、坏载荷用它造）；帧送到了页面才返回 true，页面还没请求初始状态（没发出去）返回 false，
//                       页面的处理函数抛错则原样抛出；用它造「页面必须忽略」的帧时，调用方要断言返回 true，不然「被忽略」和「没送到」分不开；
//   __fx.firstRun       首启控制器（只有场景是语言卡、规则页、导览时才有，其余场景为 null，「?」照旧不接线）；
//   __fx.raw            桥上收到的最近一帧原始 rinne.state / rinne.pair（和 web/output/app.js 的 raw 同一份），首启断言用它求 outputFlowing。
// defect 参数只给冒烟的坏样例用：故意造出一种问题，检查脚本必须把它抓出来。

import common from "../../../web/i18n/common.js";
import output from "../../../web/i18n/output.js";
import guide from "../../../web/i18n/guide.js";
import { installDicts } from "../../../web/shared/i18n.js";
import { createBridge, BRIDGE_FUNCTIONS } from "../../../web/shared/bridge.js";
import { makeT } from "../../../web/shared/dom.js";
import { pairLabelKey } from "../../../web/shared/pair-status.js";
import { mountOutputPage } from "../../../web/output/page.js";
import { HINT_KEYS, IDLE_KEY, SEND_KEYS, SUB_KEYS, TAKEOVER_KEY } from "../../../web/output/view-model.js";
import { TOUR_OUT_STEPS } from "../../../web/output/tour-out.js";
import { createFirstRun } from "../../../web/shared/first-run.js";
import { createPreviewSession } from "../../mock/state-driver.js";

const q = new URLSearchParams(location.search);
const lang = ["zh", "en", "fr"].includes(q.get("lang")) ? q.get("lang") : "zh";
const scenario = q.get("scenario") || "";
const defect = q.get("defect") || "";

installDicts(common, output, guide);
document.documentElement.lang = lang;

const calls = [];
const stage = document.getElementById("stage");

try {
    // 回声照常跑（含乱序的旧帧），页面必须自己去重；初始状态由页面自己请求
    const session = createPreviewSession({
        role: "output",
        params: { fresh: "1", lang, ...(scenario ? { scenario } : {}) },
    });
    const real = createBridge({ role: "output", mockBackend: session.mock });
    const bridge = { ...real };
    for (const name of BRIDGE_FUNCTIONS.output) {
        bridge[name] = (...args) => {
            calls.push({ name, args });
            return real[name](...args);
        };
    }
    // 桥上收到的最近一帧原始 rinne.state / rinne.pair：和 web/output/app.js 的 raw 同样只认序号递增的帧（乱序的旧帧不算），
    // 页面快照里的 state / pair 是页面规范化之后的值，真页面求 outputFlowing 用的是这里的原帧
    const raw = { state: null, pair: null, seq: { state: -Infinity, pair: -Infinity } };
    const take = (kind) => (f) => {
        if (!f || typeof f !== "object") return;
        if (Number.isFinite(f.seq)) {
            if (f.seq <= raw.seq[kind]) return;
            raw.seq[kind] = f.seq;
        }
        raw[kind] = f;
    };
    bridge.on("rinne.state", take("state"));
    bridge.on("rinne.pair", take("pair"));
    session.start();

    const sc = session.mock.previewScene && session.mock.previewScene.state;
    const page = mountOutputPage({
        host: stage,
        bridge,
        preview: sc && typeof sc === "object" ? { overlay: sc.overlay, countdown: sc.countdown } : null,
    });

    // 首启的静态场景（语言卡、规则页、导览第 N 步）：与 web/output/app.js 同样接线，用同一个首启控制器摆画面，不另写状态机。
    // 只在这几个场景里接：别的场景保持「首启链没接入」的原样（行为冒烟里「?」在接入前不动作也不报错的判据靠它）。
    // app.js 是带副作用的入口模块、不能 import，所以这里按它的写法再接一遍；两边要一起改，冒烟里的叠层判据会在不一致时报红。
    // 两路首批快照到齐（页面揭幕）之后才能接：控制器要读快照里的语言与全局位。
    let firstRun = null;
    const wireFirstRun = () => {
        if (!(sc && typeof sc === "object" && ["langStart", "rules", "tour"].includes(sc.overlay))) return;
        const { shell } = page.parts;
        firstRun = createFirstRun({
            role: "output",
            bridge,
            host: shell.top,
            shell: shell.el,
            inert: [shell.content],
            steps: TOUR_OUT_STEPS,
            onChange: () => shell.refresh(),
        });
        shell.setOverlaySource(() => firstRun.isOpen());
        firstRun.disableAuto();
        const { state: st, view } = page.snapshot();
        // 喂给控制器的运行态与 web/output/app.js 的 feed 是同一套判断：l2 取页面视图模型的 danger（和真页面一样现读）；
        // guardLit 真页面也恒为 false；loopActive 在真页面里由 outputFlowing 判断（app.js 是带副作用的入口模块，不能 import），
        // 这里写死 false。静态的首启场景不带危险态（配对不是冲突 / 版本不匹配），也没有环路活动（没有已连接的配对在回送），
        // 所以真页面在这些场景里喂的也是 false。这一点不靠这里的注释：fixtures/output/lib.mjs 的 overlayFailures 在每个首启画面上
        // 用真页面同一个判断断言（view.danger 取页面视图模型，outputFlowing 用上面存的桥上原帧求），夹具与真页面分叉会被发现
        firstRun.update(
            {
                lang: st.lang,
                langChosen: st.langChosen,
                rulesAck: st.rulesAck,
                guideSeen: st.guideSeen,
                inputGuideSeen: st.inputGuideSeen,
                restored: st.restored,
            },
            { l2: view.danger === true, guardLit: false, loopActive: false },
        );
        firstRun.showScene(sc);
    };

    /** 此刻必须看得见的文字：页面快照里的视图模型决定了哪些串应当上屏。 */
    const expectedTexts = () => {
        const { view } = page.snapshot();
        const t = makeT(view.lang);
        const out = [];
        const add = (text, what) => text && out.push({ text, what });
        add(t("out.place"), "常驻红字");
        const label = pairLabelKey(view.pair.state, "output");
        if (label) add(t(label), "配对 pill");
        add(t(SUB_KEYS[view.pair.sub]), "副行");
        const chKey = view.send.ch === "MONO" ? "common.ch.mono" : "common.ch.st";
        add(t(SEND_KEYS[view.send.kind], { ch: t(chKey), sr: view.send.sr }), "发送行");
        const first = view.messages[0];
        if (view.confirm) add(t(TAKEOVER_KEY, { g: view.confirm.g }), "确认条");
        else if (first) add(t(first.key, first.vals), `消息位 ${first.id}`);
        else add(t(IDLE_KEY), "空闲灰字");
        add(t(HINT_KEYS[view.hint]), "页脚提示");
        const { name } = view.tracks.thisTrack;
        if (name) add(name, "本轨名");
        if (view.tracks.input.name) add(view.tracks.input.name, "Input 所在轨名");
        return out;
    };

    // ---- 坏样例：故意造一种问题，冒烟脚本必须把它抓出来 ----
    const applyDefect = () => {
        const style = (css) => {
            const s = document.createElement("style");
            s.textContent = css;
            document.head.append(s);
        };
        if (defect === "overflow") {
            // 轨道名既不省略也不裁，一路长到外壳外面
            style(
                ".rn-track__name{overflow:visible !important;text-overflow:clip !important;max-width:none !important}",
            );
            const n = stage.querySelector('[data-part="track.this.name"]');
            if (n) n.textContent = "超长轨道名".repeat(18);
        } else if (defect === "place3") {
            const n = stage.querySelector(".rn-place__text");
            if (n) n.textContent += " " + n.textContent;
        } else if (defect === "rawkey") {
            const n = stage.querySelector('[data-part="pair.sub"]');
            if (n) n.textContent = "out.neverDefined";
        } else if (defect === "lowcontrast") {
            style(".rn-place__text{color:rgb(176,150,154) !important}");
        } else if (defect === "noname") {
            const b = stage.querySelector('[data-gb="output.footer.help"]');
            if (b) {
                b.removeAttribute("aria-label");
                b.removeAttribute("title");
                b.textContent = "";
            }
        } else if (defect === "noring") {
            style("*:focus,*:focus-visible{outline:none !important;box-shadow:none !important}");
        }
    };

    const frames = () => new Promise((r) => requestAnimationFrame(() => requestAnimationFrame(r)));
    const settle = async () => {
        await frames();
        await document.fonts.ready;
        await frames();
    };

    window.__fx = {
        ready: false,
        role: "output",
        lang,
        scenario,
        calls,
        page,
        raw,
        get firstRun() {
            return firstRun;
        },
        session,
        bridge,
        get expectedTexts() {
            return expectedTexts();
        },
        async mutate(fn) {
            fn(session.model);
            session.core.pushAll();
            await settle();
            await new Promise((r) => setTimeout(r, 30));
            await settle();
        },
        emit(name, payload) {
            return session.core.emit(name, payload);
        },
        settle,
    };

    // 等页面揭幕（两路快照到齐）、字体与首帧就绪；超时不吞：夹具自己报错
    const t0 = performance.now();
    while (!page.isRevealed()) {
        if (performance.now() - t0 > 4000) throw new Error("页面 4 秒内没有揭幕");
        await new Promise((r) => setTimeout(r, 20));
    }
    wireFirstRun();
    await settle();
    applyDefect();
    await settle();
    window.__fx.ready = true;
} catch (e) {
    window.__fxError = String((e && e.stack) || e);
    console.error(window.__fxError);
}
