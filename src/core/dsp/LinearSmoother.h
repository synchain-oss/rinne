// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/core.json
//
// 线性平滑器，不依赖 JUCE，语义对齐 juce::LinearSmoothedValue 的 setTargetValue / getNextValue：
//   reset(sampleRate, rampSeconds) 只换默认斜坡长度，不动当前值；
//   setTargetValue(v) 从当前值起按默认斜坡线性滑向 v；目标没变就什么都不做，进行中的斜坡不会被重新起算
//     （按块设目标时，跨块的斜坡因此不会在下一块被截断重来）；
//   setTargetValue(v, samples) 用指定的样本数做这一段斜坡（淡入淡出各有自己的时长）；
//   getNextValue() 每调用一次前进一个样本，走完最后一步时恰好落在目标上。
// 全部按样本计数，与块长无关。第 k 步的值直接按 start + (target − start)·k/n 用 double 算出，而不是逐步累加：
// 500 ms 的淡入在 192 kHz 下有约十万步，float 累加的误差到终点会攒成一个肉眼可见的台阶。
#pragma once

#include <cmath>
#include <cstdint>

namespace rinne::dsp
{

class LinearSmoother
{
public:
    LinearSmoother() = default;

    explicit LinearSmoother(float initial) noexcept : current_(initial), start_(initial), target_(initial) {}

    // 换默认斜坡：时长按 round(rampSeconds·sampleRate) 个样本，至少 1 个；sampleRate 或时长不为正时目标直接生效
    void reset(double sampleRate, double rampSeconds) noexcept
    {
        rampSamples_ = sampleRate > 0.0 && rampSeconds > 0.0 ? samplesFor(sampleRate, rampSeconds) : 0;
    }

    // 硬置位：当前值与目标都改成 value，没有斜坡
    void setCurrentAndTargetValue(float value) noexcept
    {
        current_ = value;
        start_ = value;
        target_ = value;
        step_ = 0;
        steps_ = 0;
    }

    void setTargetValue(float target) noexcept { setTargetValue(target, rampSamples_); }

    // samples 为 0 时直接落到目标
    void setTargetValue(float target, std::int64_t samples) noexcept
    {
        if (target == target_)
            return;
        target_ = target;
        if (samples > 0 && target_ != current_)
        {
            start_ = current_;
            step_ = 0;
            steps_ = samples;
        }
        else
        {
            setCurrentAndTargetValue(target);
        }
    }

    float getNextValue() noexcept
    {
        if (step_ >= steps_)
        {
            current_ = target_;
            return current_;
        }
        ++step_;
        if (step_ == steps_)
            current_ = target_;
        else
            current_ = static_cast<float>(static_cast<double>(start_) + (static_cast<double>(target_) - start_) *
                                                                            static_cast<double>(step_) /
                                                                            static_cast<double>(steps_));
        return current_;
    }

    float getCurrentValue() const noexcept { return current_; }
    float getTargetValue() const noexcept { return target_; }
    bool isSmoothing() const noexcept { return step_ < steps_; }
    std::int64_t defaultRampSamples() const noexcept { return rampSamples_; }

    static std::int64_t samplesFor(double sampleRate, double seconds) noexcept
    {
        const auto n = static_cast<std::int64_t>(std::llround(seconds * sampleRate));
        return n > 0 ? n : 1;
    }

private:
    float current_ = 0.0f;
    float start_ = 0.0f;
    float target_ = 0.0f;
    std::int64_t step_ = 0;
    std::int64_t steps_ = 0;
    std::int64_t rampSamples_ = 0;
};

} // namespace rinne::dsp
