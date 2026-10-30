// SPDX-License-Identifier: GPL-3.0-or-later
//
// S14 与 S15 的读头轨迹判据：core 级与插件级共用，只依赖标准库。
//
// 反馈 0、混合 50 %：环里就是延迟了的干声，读头（环路层混好的回送，安全链的输入）读的信号已知。干声是两个声道各一个
// 低频余弦（左 100 Hz、右 150 Hz，幅度 A = 0.3），按 Input 自己的样本号（第几个样本）算。
//
// 阈值 boundOf(Σe, Σ|Δe|) = Σe·S + A·Σ|Δe| + 1e-6，推导与各段包络每步的变化见 s14_delay_change.cpp 的文件头。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rinne::s14
{

inline constexpr double kSampleRate = 48000.0;
inline constexpr std::int64_t kSecond = 48000;
inline constexpr float kAmplitude = 0.3f;
inline constexpr double kHzLeft = 100.0;
inline constexpr double kHzRight = 150.0;
inline constexpr std::int64_t kXfade = 1920; // 40 ms
inline constexpr std::int64_t kFadeOut = 240; // 5 ms
inline constexpr std::int64_t kOnset = 960; // 20 ms
inline constexpr std::uint32_t kMaxTaps = 3;

inline float tone(std::int64_t t, double hz)
{
    constexpr double pi = 3.14159265358979323846;
    return t < 0 ? 0.0f
                 : kAmplitude * static_cast<float>(std::cos(2.0 * pi * hz * static_cast<double>(t) / kSampleRate));
}

inline float dryLeft(std::int64_t t)
{
    return tone(t, kHzLeft);
}

inline float dryRight(std::int64_t t)
{
    return tone(t, kHzRight);
}

// 余弦每个样本最多变 A·ω
inline float toneSlope(double hz)
{
    constexpr double pi = 3.14159265358979323846;
    return kAmplitude * static_cast<float>(2.0 * pi * hz / kSampleRate);
}

// 读头轨迹：按 Input 的样本号排好的两个声道
struct TapTrace
{
    std::vector<float> left;
    std::vector<float> right;

    std::size_t size() const { return left.size(); }
};

// 两个声道的阈值：B = sumGain·S + A·envelopeSteps + 1e-6
struct Bound
{
    float left;
    float right;
};

inline Bound boundOf(float sumGain, double envelopeSteps)
{
    const float steps = static_cast<float>(static_cast<double>(kAmplitude) * envelopeSteps);
    return Bound{sumGain * toneSlope(kHzLeft) + steps + 1.0e-6f, sumGain * toneSlope(kHzRight) + steps + 1.0e-6f};
}

// [from, to) 里相邻两个样本之差的最大值
inline float maxStep(const std::vector<float>& trace, std::size_t from, std::size_t to)
{
    float worst = 0.0f;
    to = std::min(to, trace.size());
    for (std::size_t i = from + 1; i < to; ++i)
        worst = std::max(worst, std::fabs(trace[i] - trace[i - 1]));
    return worst;
}

// [from, to) 里读头读的正是 D 个样本之前的干声（逐位）
inline bool settledOn(const TapTrace& tap, std::size_t from, std::size_t to, std::int64_t delay)
{
    for (std::size_t t = from; t < std::min(to, tap.size()); ++t)
    {
        const auto at = static_cast<std::int64_t>(t) - delay;
        if (tap.left[t] != dryLeft(at) || tap.right[t] != dryRight(at))
            return false;
    }
    return from < to;
}

// 反例：在 [from, to) 里任挑一个样本 m，m 之前读旧 D、m 起直接读新 D（不淡），那一步的跳变；取最坏的 m。
// 只在 ΔD 不是信号周期的整数倍时才有意义（整数倍时两个位置读到的值相同），用到它的场景都避开了整数倍。
inline float switchJump(std::int64_t oldDelay, std::int64_t newDelay, std::size_t from, std::size_t to, bool left)
{
    float worst = 0.0f;
    for (std::size_t m = from; m < to; ++m)
    {
        const auto t = static_cast<std::int64_t>(m);
        const float before = left ? dryLeft(t - 1 - oldDelay) : dryRight(t - 1 - oldDelay);
        const float after = left ? dryLeft(t - newDelay) : dryRight(t - newDelay);
        worst = std::max(worst, std::fabs(after - before));
    }
    return worst;
}

// 同一段轨迹在 cut 起直接落到 0：阶跃判据的反例
inline std::vector<float> cutAt(std::vector<float> trace, std::size_t cut)
{
    std::fill(trace.begin() + static_cast<std::ptrdiff_t>(std::min(cut, trace.size())), trace.end(), 0.0f);
    return trace;
}

} // namespace rinne::s14
