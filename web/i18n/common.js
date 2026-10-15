// SPDX-License-Identifier: GPL-3.0-or-later
//
// 两个插件共用的界面词条：语言选择卡、缩放确认与通用按钮。
// 键前缀 common.*。每个页面各有自己的词条文件（input.js、output.js、guide.js），
// 由 web/shared/i18n.js 的 mergeDicts 合并，键不得重名。
//
// 排版规则（scripts/check-i18n.mjs 检查）：
//   - zh 用全角标点，en 用弯引号和弯撇号；
//   - fr 在 ; : ? ! % 之前、« » 的内侧用 U+00A0 或 U+202F（源码里写成转义，肉眼才看得出），撇号用 ’；
//   - en 与 fr 的值不含汉字，只有语言选择卡的几条例外（check-i18n 的白名单）。
export default {
    zh: {
        "common.cancel": "取消",
        "common.langStart.title": "请选择语言 / Choose your language / Choisissez votre langue",
        "common.langStart.zh": "中文",
        "common.langStart.en": "English",
        "common.langStart.fr": "Français",
        "common.scale.overflow": "超出当前屏幕",
        "common.scale.confirmBody": "缩放已应用，{s} 秒后回退",
        "common.scale.keep": "保存",
        "common.scale.lockedLegacy": "WebKit 低于 26.4，缩放固定为 100%",
    },
    en: {
        "common.cancel": "Cancel",
        "common.langStart.title": "请选择语言 / Choose your language / Choisissez votre langue",
        "common.langStart.zh": "中文",
        "common.langStart.en": "English",
        "common.langStart.fr": "Français",
        "common.scale.overflow": "Exceeds current screen",
        "common.scale.confirmBody": "Scale applied, reverting in {s} s",
        "common.scale.keep": "Save",
        "common.scale.lockedLegacy": "WebKit older than 26.4: zoom fixed at 100%",
    },
    fr: {
        "common.cancel": "Annuler",
        "common.langStart.title": "请选择语言 / Choose your language / Choisissez votre langue",
        "common.langStart.zh": "中文",
        "common.langStart.en": "English",
        "common.langStart.fr": "Français",
        "common.scale.overflow": "Dépasse l’écran actuel",
        "common.scale.confirmBody": "Zoom appliqué, retour dans {s}\u00a0s",
        "common.scale.keep": "Enregistrer",
        "common.scale.lockedLegacy": "WebKit antérieur à 26.4\u202f: zoom fixé à 100\u00a0%",
    },
};
