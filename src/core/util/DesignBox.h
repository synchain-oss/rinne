// SPDX-License-Identifier: GPL-3.0-or-later
// 本文件由 scripts/gen-design-box.py 从 web/shared/design-box.js 生成，请勿手工编辑。
// 改值只改 design-box.js，再重跑：python scripts/gen-design-box.py
#pragma once

#include <array>

namespace rinne::design
{
// Input 设计盒（1x）：800×580，7 档缩放。
inline constexpr int kInputDesignW = 800;
inline constexpr int kInputDesignH = 580;
inline constexpr int kInputPresetCount = 7;
inline constexpr std::array<double, 7> kInputPresets = {0.5, 0.65, 0.8, 1, 1.25, 1.5, 2};

// Output 设计盒（1x）：460×364，10 档缩放。
inline constexpr int kOutputDesignW = 460;
inline constexpr int kOutputDesignH = 364;
inline constexpr int kOutputPresetCount = 10;
inline constexpr std::array<double, 10> kOutputPresets = {0.33, 0.5, 0.75, 1, 1.25, 1.5, 1.75, 2, 2.5, 3};

// 宿主给插件窗加的框（标题栏加工具条）的估计值，用来判断某个缩放档位放不放得下。
inline constexpr int kHostFrameW = 16;
inline constexpr int kHostFrameH = 64;
} // namespace rinne::design
