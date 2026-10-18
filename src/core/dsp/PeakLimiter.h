// SPDX-License-Identifier: GPL-3.0-or-later
//
// 回送链第七级：峰值限制器。左右声道联动、无前视（不引入延迟）：
//   目标增益 g_t = min(1, c / max(|L|, |R|))，起音瞬时（当样本就压到位）；
//   释放一阶，时间常数 80 ms：缩减量 (1 − env) 每个样本乘 exp(−1/(0.08·sr))；
//   env = min(g_t, 释放后的值)，输出 = 输入 × env，最后再钳到 ±c。
// 末级钳保证「限制器开时 |输出| ≤ c」逐位成立：c / peak 与乘法各有一次舍入，单靠增益可能高出 c 一个 ulp。
// 状态存的是缩减量 r = 1 − env 而不是 env：env 贴近 1 时，float 里 1 − (1 − env)·a 每步的变化小于半个 ulp，
// 会卡在约 −0.001 dB 再也回不到 1；r 自己按 a 几何衰减不会卡住，小于 2^-25 后 1 − r 正好舍入成 1.0f，
// 空闲的限制器因此逐位直通。r 小到 1e-30 以下直接冲 0，不进 denormal。
// 上限 c 由调用方给（线性幅度，−24…0 dBFS 对应约 0.063…1.0），可以逐样本变化（平滑由调用方做）。
// 输入须已清洗；万一来了非有限值，本样本输出 0，增益状态不动，不会被 NaN 污染。
#pragma once

#include <cmath>

#include "dsp/Sanitize.h"

namespace rinne::dsp
{

inline constexpr double kLimiterReleaseSeconds = 0.080;

class PeakLimiter
{
public:
    void prepare(double sampleRate) noexcept
    {
        release_ = sampleRate > 0.0 ? static_cast<float>(std::exp(-1.0 / (kLimiterReleaseSeconds * sampleRate))) : 0.0f;
        reset();
    }

    void reset() noexcept
    {
        reduction_ = 0.0f;
        env_ = 1.0f;
    }

    // 就地处理一对样本，返回本样本用的增益（1 = 没有缩减），供增益缩减表与失控保护判据使用
    float process(float& left, float& right, float ceiling) noexcept
    {
        if (!isFiniteSample(left) || !isFiniteSample(right) || !(ceiling > 0.0f))
        {
            left = 0.0f;
            right = 0.0f;
            return env_;
        }
        const float al = std::fabs(left);
        const float ar = std::fabs(right);
        const float peak = al > ar ? al : ar;
        const float needed = peak > ceiling ? 1.0f - ceiling / peak : 0.0f;
        const float released = flushTiny(reduction_ * release_);
        reduction_ = needed > released ? needed : released;
        env_ = 1.0f - reduction_;
        left = clampTo(left * env_, ceiling);
        right = clampTo(right * env_, ceiling);
        return env_;
    }

    float gain() const noexcept { return env_; }

private:
    static float clampTo(float x, float c) noexcept
    {
        if (x > c)
            return c;
        if (x < -c)
            return -c;
        return x;
    }

    float release_ = 0.0f;
    float reduction_ = 0.0f;
    float env_ = 1.0f;
};

} // namespace rinne::dsp
