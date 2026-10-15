// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/web.json
//
// design-box.js：全项目「设计盒」常量的唯一真源。
//
//   Input 800×580：三栏信号流（反馈、环路延迟与回送音色、安全与电平）加页眉、配对条和消息行。
//   Output 460×364：只做分组与配对的小窗。
//   hostFrame 是宿主给插件窗加的框（标题栏加插件工具条）的估计值，上机实测后再调；
//   它只用来判断某个缩放档位在当前屏幕上放不放得下：
//   W*F + hostFrame.w > availWidth 或 H*F + hostFrame.h > availHeight 的档位置灰并提示「超出当前屏幕」。
//
//   纪律一：C++ 侧的 src/core/util/DesignBox.h 由 scripts/gen-design-box.py 从本文件生成，
//   任何位置（C++、HTML、CSS、预览页）都不得二次硬编码这些数字；改值只改这里，再重跑生成器。
//   纪律二：本文件必须保持「单个对象字面量加纯数字」的形状，不引入运算、依赖或条件分支，
//   否则生成器无法静态解析。
//   缩放档位是设计值，不是可用值：运行时还要按屏幕可用区过滤。

export const DESIGN = {
    input: { w: 800, h: 580, presets: [0.5, 0.65, 0.8, 1, 1.25, 1.5, 2] },
    output: {
        w: 460,
        h: 364,
        presets: [0.33, 0.5, 0.75, 1, 1.25, 1.5, 1.75, 2, 2.5, 3],
    },
    hostFrame: { w: 16, h: 64 },
};
