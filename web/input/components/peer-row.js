// SPDX-License-Identifier: GPL-3.0-or-later
//
// 对端行（配对条下，300×30）：「Output 在：」加轨道色点、轨道名、轨道名核验徽标和声道徽标。
// 没有对端时整行留空；此时若消息位正在提示「下一步」，在空位里画插入链示意（Output 放哪一格）。

import { el } from "../../shared/dom.js";
import { createVerifyBadge, createChannelBadge } from "../../shared/pair-status.js";
import { createChainDiagram } from "../../shared/chain-diagram.js";
import { channelKind, peerPresent, trackColor, verifyKind } from "../model.js";
import { setAttr, setHidden, setText, setVar } from "./util.js";

/** 创建对端行。 */
export function createPeerRow() {
    const label = el("span", { class: "in-peer__label" });
    const dot = el("span", { class: "in-peer__dot", "aria-hidden": "true" });
    const name = el("span", { class: "in-peer__name", "data-part": "peer.name" });
    const verify = createVerifyBadge();
    const channels = createChannelBadge();
    const chain = createChainDiagram({ compact: true });
    const chainHost = el("div", { class: "in-peer__chain", "data-part": "peer.chain" }, chain.el);
    setHidden(chainHost, true);
    const root = el(
        "div",
        { class: "in-peer", "data-gb": "input.peer", "data-state": "empty" },
        label,
        dot,
        name,
        verify.el,
        channels.el,
        chainHost,
    );
    let chainLang = null;
    let badgeKey = "";

    return {
        el: root,
        /**
         * @param {object} p
         * @param {object} p.pair rinne.pair 最近一帧
         * @param {boolean} p.showChain 消息位正在提示「下一步」
         * @param {string} p.lang
         * @param {(k:string, v?:object)=>string} p.t
         */
        update(p) {
            const pair = p.pair || {};
            const peer = pair.peer || {};
            const self = pair.self || {};
            const present = peerPresent(pair);
            const named = Boolean(peer.name);
            setAttr(root, "data-state", !present ? "empty" : named ? "named" : "noName");
            for (const node of [label, dot, name, verify.el, channels.el]) setHidden(node, !present);
            if (present) {
                setText(label, p.t("in.peer.label"));
                const color = trackColor(peer.color);
                setAttr(dot, "data-hollow", color ? null : "1");
                setVar(dot, "--c", color || "transparent");
                setText(name, named ? peer.name : p.t("in.peer.noName"));
                setAttr(name, "title", named ? peer.name : p.t("in.peer.noName"));
                setAttr(name, "data-empty", named ? null : "1");
                // 徽标每次更新都会重建子节点：键不变就不动，免得 1 Hz 兜底帧也产生 DOM 变更
                const vk = verifyKind(pair.verify);
                const selfKind = channelKind(self.channels);
                const peerKind = channelKind(peer.channels);
                const key = `${p.lang}|${vk}|${selfKind}|${peerKind}`;
                if (key !== badgeKey) {
                    badgeKey = key;
                    verify.update({ kind: vk, t: p.t });
                    channels.update({ input: selfKind || peerKind, output: selfKind ? peerKind : null, t: p.t });
                }
                setAttr(verify.el, "title", p.t("in.peer.selfTip", { name: self.name || "—" }));
            }
            const showChain = !present && p.showChain;
            setHidden(chainHost, !showChain);
            if (showChain && chainLang !== p.lang) {
                chainLang = p.lang;
                chain.update({ highlight: "out", still: false, t: p.t });
            }
        },
    };
}
