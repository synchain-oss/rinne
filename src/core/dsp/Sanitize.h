// SPDX-License-Identifier: GPL-3.0-or-later
//
// 回送链第一级：清洗。环里来的样本先过这里，之后的每一级都可以假定输入有限、|x| ≤ 16。
//   非有限（NaN、±Inf）→ 0；|x| > 16 → ±16；|x| < 1e-30 → 0（顺手冲掉 denormal，后面的递归滤波器不会被它拖慢）。
// ⑥ 之前清非有限值只在这一级做：之后 ②–⑤ 各级内部算出的非有限值（Input 内部产生的）不在半路清掉，要原样走到 ⑥
// 的有限性检查跳闸，所以 flushTiny 只冲极小值、不碰非有限值。递归滤波器的状态要不要另防坏样本由各级自己定：SVF 的
// 状态写回用 SvfTpt.h 的 flushState（非有限 → +0），状态不会被一个坏样本永久卡在 NaN 上，那一格的非有限输出照样
// 交给 ⑥。
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

// 把 |x| < kTinyFlush 的值冲成 +0。递归滤波器的状态每个样本都过一遍，零输入时不会衰减进 denormal。
// 它不清非有限值：非有限值要留给 ⑥ 跳闸。NaN 让两个比较都为假、原样返回，±Inf 也原样返回
inline float flushTiny(float x) noexcept
{
    return (x < kTinyFlush && x > -kTinyFlush) ? 0.0f : x;
}

inline double flushTiny(double x) noexcept
{
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
