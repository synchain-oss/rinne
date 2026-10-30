// SPDX-License-Identifier: GPL-3.0-or-later
//
// 共享按钮与状态字的词条键表。页面卡按键取词条，不各写一份字面量，改键名时只有这里和词条文件两处。
// 写法同 scale-ui.js 的 SCALE_KEYS：只登记键，不含词条本身。

/** 通用按钮与开关状态字。 */
export const BUTTON_KEYS = Object.freeze({
    cancel: "common.cancel",
    save: "common.save",
    close: "common.close",
    delete: "common.delete",
    on: "common.on",
    off: "common.off",
});

/** 语言胶囊三项：代码对应词条键。顺序即显示顺序。 */
export const LANG_ITEMS = Object.freeze([
    { code: "zh", key: "common.lang.zh" },
    { code: "en", key: "common.lang.en" },
    { code: "fr", key: "common.lang.fr" },
]);
