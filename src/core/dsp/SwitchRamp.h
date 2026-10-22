// SPDX-License-Identifier: GPL-3.0-or-later
//
// 开关量的定长线性交叉过渡：限制器开关（10 ms）、极性反转、低切 / 高切的开与关都用它，不允许硬切。
// 两路输入按 out = (1 − b)·off + b·on 混合，b 从一端走到另一端恰好 n 个样本，n = round(时长·sr)，至少 1。
//   位置是整数 k ∈ [0, n]，step() 每个样本朝目标走一步，b 由 k 算出：全程按样本计数，与宿主怎么切块无关。
//   过渡中再切换：从当前的 k 原路折返，增益连续、斜率不变，不会跳回起点重来；同一个目标重复设置什么都不做。
//   b 取 k/n 舍入到 2^-24 的整数倍：这样 b 与 1 − b 在 float 里都恰好可表示，两路增益之和恒为 1，不只是约等于。
//   落在端点（k = 0 或 n）时 mix() 直接返回那一路的样本、不做乘加：−0.0 的符号、NaN 载荷原样保留，
//   另一路的非有限值也混不进来（0 乘 NaN 仍是 NaN）。
// 线性而不是等功率：两路通常高度相关（限制前后、滤波前后、正反相），等功率交叉在相关信号上会过冲约 3 dB。
#pragma once

#include <cmath>
#include <cstdint>

namespace rinne::dsp
{

inline constexpr double kSwitchRampSeconds = 0.010;

class SwitchRamp
{
public:
    SwitchRamp() = default;

    // on 为初始目标，并且就落在那一端
    explicit SwitchRamp(bool on) noexcept : on_(on), k_(on ? 1 : 0) {}

    // 换过渡长度：round(seconds·sampleRate) 个样本，至少 1；并落定到当前目标（prepare 时不带着半截过渡）
    void prepare(double sampleRate, double seconds = kSwitchRampSeconds) noexcept
    {
        std::int64_t n = 1;
        if (sampleRate > 0.0 && seconds > 0.0)
        {
            n = static_cast<std::int64_t>(std::llround(seconds * sampleRate));
            n = n > 0 ? n : 1;
        }
        setLengthSamples(n);
    }

    void setLengthSamples(std::int64_t samples) noexcept
    {
        n_ = samples > 0 ? samples : 1;
        scale_ = kUnit / static_cast<double>(n_);
        snapToTarget();
    }

    // 只改方向，位置不动；下一次 step() 起朝新目标走
    void setTarget(bool on) noexcept { on_ = on; }

    void snapToTarget() noexcept { k_ = on_ ? n_ : 0; }

    // 前进一个样本；已在目标端时不动
    void step() noexcept
    {
        if (on_)
        {
            if (k_ < n_)
                ++k_;
        }
        else if (k_ > 0)
        {
            --k_;
        }
    }

    // on 那一路的增益 b ∈ [0, 1]，2^-24 的整数倍
    float onGain() const noexcept
    {
        if (k_ <= 0)
            return 0.0f;
        if (k_ >= n_)
            return 1.0f;
        return static_cast<float>(std::floor(static_cast<double>(k_) * scale_ + 0.5) / kUnit);
    }

    // off 那一路的增益 1 − b：b 是 2^-24 的整数倍，这个减法没有舍入
    float offGain() const noexcept { return 1.0f - onGain(); }

    // 按当前位置混合两路；端点上原样返回那一路
    float mix(float off, float on) const noexcept
    {
        if (k_ <= 0)
            return off;
        if (k_ >= n_)
            return on;
        const float b = onGain();
        return (1.0f - b) * off + b * on;
    }

    bool target() const noexcept { return on_; }
    bool isSettled() const noexcept { return k_ == (on_ ? n_ : 0); }
    bool isFullyOn() const noexcept { return k_ >= n_; }
    bool isFullyOff() const noexcept { return k_ <= 0; }
    std::int64_t lengthSamples() const noexcept { return n_; }
    std::int64_t position() const noexcept { return k_; }

private:
    static constexpr double kUnit = 16777216.0; // 2^24

    bool on_ = false;
    std::int64_t n_ = 1;
    std::int64_t k_ = 0;
    double scale_ = kUnit;
};

} // namespace rinne::dsp
