// SPDX-License-Identifier: GPL-3.0-or-later
//
// 宿主音频缓冲的视图：每声道一串样本。
//
// 声道用「指针数组 + 声道数」表达（贴 engine.process(ptrs, nch, n, …) 的形状）：
//   channels[0] 是左声道，channels[1] 是右声道，依此类推。声道数由 numChannels 给出。
//
// **mono 的约定**：调用方只给一个声道时（numChannels = 1），channels[0] 既是左也是右，
// 引擎把左声道复制到两格。环恒为立体声（两格），写侧按 min(声道数, 2) 复制：
// 声道数 1 时两格写同一个值，声道数 ≥ 2 时写前两格。
//
// 两个引擎共用这一份（各定义一份会出现重定义）。它不是参数：参数面在 EngineParams.h。
#pragma once

namespace rinne::engine
{

struct AudioView
{
    float* const* channels = nullptr; // 每声道一串样本；不小于 numChannels 个
    int numChannels = 0;              // 声道数（1 = mono，2 = stereo）
    int length = 0;                   // 样本数

    // 左右声道的取法：mono 时左右都取第 0 个（环与回送恒按立体声走）。
    float* left() const noexcept { return channels != nullptr && numChannels > 0 ? channels[0] : nullptr; }
    float* right() const noexcept
    {
        if (channels == nullptr || numChannels == 0)
        {
            return nullptr;
        }
        return numChannels > 1 ? channels[1] : channels[0];
    }
};

} // namespace rinne::engine
