// SPDX-License-Identifier: GPL-3.0-or-later
//
// 回送链第二级：DC 阻断，恒开、没有开关。一阶高通 y[n] = x[n] − x[n−1] + R·y[n−1]，R = exp(−2π·5/sr)，
// 拐点 5 Hz：环里的失真、饱和插件常带直流偏移，绕环一圈就叠一次，不挡掉会把回送一路推向限制器。
// 对 20 Hz 以上几乎无影响（48 kHz 下 20 Hz 约 −0.26 dB、50 Hz 约 −0.04 dB）。
// 状态每个样本冲一次极小值：零输入时 y 按 R 几何衰减，不冲就会落进 denormal。
// 单声道；立体声用两个实例。
#pragma once

#include <cmath>

#include "dsp/Sanitize.h"

namespace rinne::dsp
{

inline constexpr double kDcBlockerHz = 5.0;

class DcBlocker
{
public:
    void prepare(double sampleRate) noexcept
    {
        constexpr double twoPi = 6.283185307179586476925286766559;
        r_ = sampleRate > 0.0 ? static_cast<float>(std::exp(-twoPi * kDcBlockerHz / sampleRate)) : 0.0f;
        reset();
    }

    void reset() noexcept
    {
        x1_ = 0.0f;
        y1_ = 0.0f;
    }

    // 输入须已清洗（有限、|x| ≤ 16）
    float process(float x) noexcept
    {
        const float y = flushTiny(x - x1_ + r_ * y1_);
        x1_ = x;
        y1_ = y;
        return y;
    }

    float coefficient() const noexcept { return r_; }

private:
    float r_ = 0.0f;
    float x1_ = 0.0f;
    float y1_ = 0.0f;
};

} // namespace rinne::dsp
