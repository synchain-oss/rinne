// SPDX-License-Identifier: GPL-3.0-or-later
//
// DSP 与安全测试共用的小工具：跨平台逐位确定的伪随机数（不用标准库分布，它们的结果因实现而异）、float 位型、
// 正弦与幅频测量。
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

namespace rinne::test
{

inline constexpr double kPi = 3.14159265358979323846;

// xorshift64*：种子固定，结果在所有平台上逐位相同
class Rng
{
public:
    explicit Rng(std::uint64_t seed) noexcept : s_(seed ? seed : 0x9e3779b97f4a7c15ull) {}

    std::uint64_t next() noexcept
    {
        s_ ^= s_ >> 12;
        s_ ^= s_ << 25;
        s_ ^= s_ >> 27;
        return s_ * 0x2545f4914f6cdd1dull;
    }

    // [0, 1)
    double uniform() noexcept { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
    double range(double lo, double hi) noexcept { return lo + (hi - lo) * uniform(); }
    int below(int n) noexcept { return static_cast<int>(uniform() * n); }
    float bits() noexcept
    {
        const auto b = static_cast<std::uint32_t>(next() >> 32);
        float f = 0.0f;
        std::memcpy(&f, &b, sizeof f);
        return f;
    }

private:
    std::uint64_t s_;
};

inline std::uint32_t floatBits(float f) noexcept
{
    std::uint32_t b = 0;
    std::memcpy(&b, &f, sizeof b);
    return b;
}

inline float fromBits(std::uint32_t b) noexcept
{
    float f = 0.0f;
    std::memcpy(&f, &b, sizeof f);
    return f;
}

inline bool isSubnormal(float f) noexcept
{
    return std::fpclassify(f) == FP_SUBNORMAL;
}

inline float sine(double freq, double sampleRate, std::int64_t n, double amplitude = 1.0) noexcept
{
    return static_cast<float>(amplitude * std::sin(2.0 * kPi * freq * static_cast<double>(n) / sampleRate));
}

inline double toDb(double gain) noexcept
{
    return 20.0 * std::log10(gain);
}

} // namespace rinne::test
