// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rinne Output 的导览步骤表（4 步，设计稿 v2.5 的 tourGeom）：锚点、词条前缀与说明框首选位置的写法同 web/input/tour-in.js。
// Output 的版面随放置说明的行数上下移，所以除第 1 步居中以外都相对亮区摆：below 贴亮区下沿，above 贴亮区上沿。
// 第 3 步的锚点是配对卡的头部，它自带内边距，亮区只外扩 3（428×76，设计稿 426×76），说明框贴着亮区下沿往下摆，
// en / fr 的框更高，要靠这一点才放得进设计盒。
// Input 的导览看过时 Output 不弹自己的导览（first-run.js 的 firstRunSteps）。

export const TOUR_OUT_STEPS = Object.freeze([
    { anchor: null, keys: "tour.out.step1.", card: { x: 50, center: true } },
    { anchor: "group", keys: "tour.out.step2.", card: { x: 50, below: 8 } },
    { anchor: "pair", pad: 3, keys: "tour.out.step3.", card: { x: 50, below: 7 } },
    { anchor: "help", keys: "tour.out.step4.", card: { x: 50, above: 6 } },
]);
