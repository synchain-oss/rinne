// SPDX-License-Identifier: GPL-3.0-or-later
//
// 回送链第一级：清洗。环里来的样本先过这里，之后的每一级都可以假定输入有限、|x| ≤ 16。
//   非有限（NaN、±Inf）→ 0；|x| > 16 → ±16；|x| < 1e-30 → 0（顺手冲掉 denormal，后面的递归滤波器不会被它拖慢）。
// flushTiny 也把非有限值冲成 0：递归滤波器（SVF、DC 阻断）的状态经它写回，状态里万一算出 NaN 或 ±Inf
// 会被清回 0，不会永久卡在 NaN 上。
// 有限性按位模式判断：指数位全 1 就是非有限。不用 std::isfinite，免得哪个编译选项把它当成恒真优化掉。
#pragma once

#include <cstdint>
#include <cstring>

namespace rinne::dsp
{

// 清洗的钳位（+24 dBFS）。Output 写环前用同一个值，Input 这里是第二道
inline constexpr float kSanitizeLimit = 16.0f;
// 比它小的值当作 0：约 −600 dBFS，听不见，也远在 float 的 denormal 区（约 1.2e-38）之上
inline constexpr float kTinyFlush = 1.0e-30f;

inline bool isFiniteSample(float x) noexcept
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &x, sizeof bits);
    return (bits & 0x7f800000u) != 0x7f800000u;
}

// double 版，同样按位模式判断：11 位指数全 1 就是非有限
inline bool isFiniteSample(double x) noexcept
{
    std::uint64_t bits = 0;
    std::memcpy(&bits, &x, sizeof bits);
    return (bits & 0x7ff0000000000000ull) != 0x7ff0000000000000ull;
}

// 把非有限值与 |x| < kTinyFlush 的值冲成 +0。递归滤波器的状态每个样本都过一遍：零输入时不会衰减进 denormal；
// 万一状态里算出了 NaN 或 ±Inf，这一步就把它清回 0，不会一直带着它往下算。
// 非有限要单独判：NaN 与任何数比较都为假，只靠下面两个比较，NaN 会原样通过。
inline float flushTiny(float x) noexcept
{
    if (!isFiniteSample(x))
        return 0.0f;
    return (x < kTinyFlush && x > -kTinyFlush) ? 0.0f : x;
}

inline double flushTiny(double x) noexcept
{
    if (!isFiniteSample(x))
        return 0.0;
    return (x < 1.0e-30 && x > -1.0e-30) ? 0.0 : x;
}

// 清洗一个样本。nonFinite 在样本非有限时置 true（不会置回 false），调用方据此计数
inline float sanitizeSample(float x, bool& nonFinite) noexcept
{
    if (!isFiniteSample(x))
    {
        nonFinite = true;
        return 0.0f;
    }
    if (x > kSanitizeLimit)
        return kSanitizeLimit;
    if (x < -kSanitizeLimit)
        return -kSanitizeLimit;
    return flushTiny(x);
}

inline float sanitizeSample(float x) noexcept
{
    bool ignored = false;
    return sanitizeSample(x, ignored);
}

} // namespace rinne::dsp
