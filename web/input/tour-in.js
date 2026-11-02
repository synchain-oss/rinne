// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rinne Input 的导览步骤表（6 步，设计稿 v2.5 的 tourGeom）：锚点是页面上 [data-tour="<名字>"] 的元素，
// keys 是这一步词条的前缀（后接 title / body，词条在 web/i18n/guide.js），card 是说明框的首选位置（设计盒坐标）：
// x / y 是左上角；below 是贴着亮区下沿往下的间距。说明框都放在左两栏（右边界 514），不压右侧的紧急切断；
// 首选位置压到亮区或越出设计盒时由 tour-paint.js 的 pickCalloutPos 换方位。
// 第 2 步的 pair 锚点是 app.js 在组选择、对端行和消息位外面放的一个不可交互的占位框。

export const TOUR_IN_STEPS = Object.freeze([
    { anchor: null, keys: "tour.in.step1.", card: { x: 92, y: 170 } },
    { anchor: "pair", keys: "tour.in.step2.", card: { x: 20, below: 8 } },
    { anchor: "feedback", keys: "tour.in.step3.", card: { x: 20, below: 8 } },
    { anchor: "delay", keys: "tour.in.step4.", card: { x: 20, y: 92 } },
    { anchor: "safety", keys: "tour.in.step5.", card: { x: 154, y: 170 } },
    { anchor: "help", keys: "tour.in.step6.", card: { x: 154, y: 346 } },
]);
