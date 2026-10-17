// SPDX-License-Identifier: GPL-3.0-or-later
//
// 元件编号到 data-gb 锚点的对照：设计稿与页面实现都在每个元件的根节点上放 data-gb，
// 场景表里的 mustShow 用元件编号，check-design-manifest 和页面冒烟靠这张表把两者接起来。
// 编号只增不复用；已移除的元件（I-12 单旋钮 Mix 开关、I-13 干声旋钮）不在表内，也不进任何场景。
// 子锚点以「根锚点 + 点号」开头（例如 input.preset.menu 之下的 input.preset.menu.item），匹配时按前缀算。

export const ELEMENT_ANCHORS = {
    "I-01": "input.shell",
    "I-02": "input.header",
    "I-03": "input.preset.trigger",
    "I-04": "input.preset.menu",
    "I-05": "input.lang",
    "I-06": "input.group",
    "I-07": "input.pair.pill",
    "I-08": "input.peer",
    "I-09": "input.band",
    "I-10": "input.loop",
    "I-11": "input.fb.knob",
    "I-14": "input.mix.knob",
    "I-15": "input.dly",
    "I-16": "input.tone",
    "I-17": "input.panic",
    "I-18": "input.limiter",
    "I-19": "input.gr",
    "I-20": "input.lamps",
    "I-21": "input.meters",
    "I-22": "input.footer.hint",
    "I-23": "input.footer.scale",
    "I-24": "input.scale.confirm",
    "I-25": "input.footer.version",
    "I-26": "input.footer.help",
    "I-27": "input.about",
    "I-28": "input.toast",
    "I-29": "input.numEdit",
    "I-30": "input.langStart",
    "I-31": "input.rules",
    "I-32": "input.tour",
    "O-01": "output.shell",
    "O-02": "output.header",
    "O-03": "output.lang",
    "O-04": "output.place",
    "O-05": "output.group",
    "O-06": "output.pair",
    "O-07": "output.send",
    "O-08": "output.band",
    "O-09": "output.hint",
    "O-10": "output.footer",
    "O-11": "output.scale.confirm",
    "O-12": "output.about",
    "O-13": "output.toast",
    "O-14": "output.langStart",
    "O-15": "output.rules",
    "O-16": "output.tour",
};

/** 某个锚点是否覆盖了元件编号：等于根锚点，或以「根锚点 + 点号」开头。 */
export function anchorMatches(gb, code) {
    const root = Object.prototype.hasOwnProperty.call(ELEMENT_ANCHORS, code) ? ELEMENT_ANCHORS[code] : null;
    return root !== null && (gb === root || gb.startsWith(`${root}.`));
}
