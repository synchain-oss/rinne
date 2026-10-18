// SPDX-License-Identifier: GPL-3.0-or-later
//
// 回送链第八级：软削波，恒开、不可关，是回送硬上界 |ret| < 1.0 的最后一道。
//   |x| ≤ 0.7：y = x（逐位不变）；
//   |x| > 0.7：y = sign(x)·min(0.7 + 0.3·tanh((|x| − 0.7)/0.3), kSoftClipMax)。
// 在 0.7 处值与一阶导数都连续（tanh 在 0 的斜率是 1）。末级钳不能省：|x| 稍大时 float 的 tanh 就已经等于 1，
// 0.7f + 0.3f 正好舍入成 1.0f，没有这一钳，输出会碰到 1.0。NaN 输出 0，±Inf 输出 ±kSoftClipMax。
// 数学上 |y| ≤ |x|（tanh(z) ≤ z）；拐点附近 float 舍入可能多出一个 ulp，这里再取 min(y, |x|)，
// 所以软削波逐位不放大，限制器开时「回送 ≤ 上限」经过这一级仍然成立。
#pragma once

#include <cmath>

namespace rinne::dsp
{

// 拐点：|x| 不超过它时软削波逐位直通（约 −3.1 dBFS）
inline constexpr float kSoftClipKnee = 0.7f;
inline constexpr float kSoftClipRange = 0.3f;
// 末级钳：小于 1.0f 的最大 float（nextafter(1.0f, 0.0f)）
inline constexpr float kSoftClipMax = 0.99999994f;

// engaged 在 |x| 超过拐点时置 true（不会置回 false）；失控保护在限制器关闭时数这种样本
inline float softClip(float x, bool& engaged) noexcept
{
    const float a = std::fabs(x);
    if (a <= kSoftClipKnee)
        return x;
    if (!(a > kSoftClipKnee))
        return 0.0f; // NaN：两个比较都不成立
    engaged = true;
    float y = kSoftClipKnee + kSoftClipRange * std::tanh((a - kSoftClipKnee) / kSoftClipRange);
    if (y > a)
        y = a;
    if (y > kSoftClipMax)
        y = kSoftClipMax;
    return x < 0.0f ? -y : y;
}

inline float softClip(float x) noexcept
{
    bool ignored = false;
    return softClip(x, ignored);
}

} // namespace rinne::dsp
