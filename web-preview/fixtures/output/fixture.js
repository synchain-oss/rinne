// SPDX-License-Identifier: GPL-3.0-or-later
//
// Output 页面夹具：把真页面（web/output/page.js）挂进 #stage，桥是 mock 后端，观测接口挂在 window.__fx 上给冒烟脚本。
// 与 shared 夹具同口径（探针硬编码根 #stage，登记「必须上屏」的文字），区别是这里摆的就是整页，不是单个共享件。
//   __fx.ready          页面已揭幕、字体已就绪、首帧已画；
//   __fx.calls          页面调过的桥函数 [{ name, args }]；
//   __fx.expectedTexts  此刻页上必须看得见的文字（按快照里的视图模型现算）；
//   __fx.mutate(fn)     改 mock 的模型并立即发一轮帧（场景之外的状态组合用它造）；
//   __fx.emit(name, p)  直接往页面发一个桥事件（乱序、坏载荷用它造）。
// defect 参数只给冒烟的坏样例用：故意造出一种问题，检查脚本必须把它抓出来。

import common from "../../../web/i18n/common.js";
import output from "../../../web/i18n/output.js";
import { installDicts } from "../../../web/shared/i18n.js";
import { createBridge, BRIDGE_FUNCTIONS } from "../../../web/shared/bridge.js";
import { makeT } from "../../../web/shared/dom.js";
import { pairLabelKey } from "../../../web/shared/pair-status.js";
import { mountOutputPage } from "../../../web/output/page.js";
import { HINT_KEYS, IDLE_KEY, SEND_KEYS, SUB_KEYS, TAKEOVER_KEY } from "../../../web/output/view-model.js";
import { createPreviewSession } from "../../mock/state-driver.js";

const q = new URLSearchParams(location.search);
const lang = ["zh", "en", "fr"].includes(q.get("lang")) ? q.get("lang") : "zh";
const scenario = q.get("scenario") || "";
const defect = q.get("defect") || "";

installDicts(common, output);
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
    session.start();

    const sc = session.mock.previewScene && session.mock.previewScene.state;
    const page = mountOutputPage({
        host: stage,
        bridge,
        preview: sc && typeof sc === "object" ? { overlay: sc.overlay, countdown: sc.countdown } : null,
    });

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
    await settle();
    applyDefect();
    await settle();
    window.__fx.ready = true;
} catch (e) {
    window.__fxError = String((e && e.stack) || e);
    console.error(window.__fxError);
}
