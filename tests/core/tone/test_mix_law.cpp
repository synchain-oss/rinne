// SPDX-License-Identifier: GPL-3.0-or-later
//
// Mix 律（中点两路全开的线性律，契约已定）：
//   g_dry = min(1, 2·(1 − m))、g_ret = min(1, 2·m)。
// 四个点精确：0 % → 干 1 / 回送 0；25 %（默认）→ 1 / 0.5；50 % → 1 / 1；100 % → 0 / 1。
// m ≤ 50 % 时 g_dry 恰为 1.0f：默认插上 Rinne Input 干声音量不变，干声的逐位快速路径也靠它。
// 两条增益都随 m 单调，没有上限档、没有模式切换、没有 0.95 上限。
// 越界钳回 [0, 1]；NaN 按 0 处理（失败只会让回送变成 0，干声照常）。
// 平滑不在这里做：g_ret 并进安全链的增益积（限制器之前），g_dry 只乘干声，两条都由引擎用
// LinearSmoother 按 kMixSmoothSeconds 线性平滑、终点精确。
// 必须变红：把 g_dry 写成 min(1, 1 − m)（单旋钮那套）、写成 cos，或把 g_ret 写成 m / sin，
// 下面「反例」一节的格子会红；改这四个点里的任何一个，「四个点精确」也会红。

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

#include "dsp/MixLaw.h"

using rinne::dsp::clampMix;
using rinne::dsp::kMixDefault;
using rinne::dsp::kMixSmoothSeconds;
using rinne::dsp::mixDryGain;
using rinne::dsp::mixGains;
using rinne::dsp::mixReturnGain;

namespace
{

constexpr double kPiHalf = 1.57079632679489661923;

// 反例用的另外几套律：单旋钮那套（min(1, 1 − m) 与 m）、余弦律、余弦正弦律
float wrongDryLinear(float m) noexcept
{
    const float g = 1.0f - m;
    return g < 1.0f ? g : 1.0f;
}

float wrongRetLinear(float m) noexcept
{
    return m < 1.0f ? m : 1.0f;
}

float wrongDryCos(float m) noexcept
{
    return static_cast<float>(std::cos(m * kPiHalf));
}

float wrongRetSin(float m) noexcept
{
    return static_cast<float>(std::sin(m * kPiHalf));
}

} // namespace

TEST_CASE("the four documented mix points are exact in float", "[tone][mix]")
{
    const auto at0 = mixGains(0.0f);
    CHECK(at0.dry == 1.0f);
    CHECK(at0.ret == 0.0f);

    const auto at25 = mixGains(0.25f);
    CHECK(at25.dry == 1.0f);
    CHECK(at25.ret == 0.5f);

    const auto at50 = mixGains(0.5f);
    CHECK(at50.dry == 1.0f);
    CHECK(at50.ret == 1.0f);

    const auto at100 = mixGains(1.0f);
    CHECK(at100.dry == 0.0f);
    CHECK(at100.ret == 1.0f);

    // 单值入口与二元入口给同一结果
    CHECK(mixDryGain(0.25f) == mixGains(0.25f).dry);
    CHECK(mixReturnGain(0.25f) == mixGains(0.25f).ret);
}

TEST_CASE("the default mix is a quarter and the smoothing window is 30 ms", "[tone][mix]")
{
    CHECK(kMixDefault == 0.25f);
    CHECK(mixGains(kMixDefault).dry == 1.0f);
    CHECK(mixGains(kMixDefault).ret == 0.5f);
    CHECK(kMixSmoothSeconds == 0.030);
}

TEST_CASE("dry is exactly 1.0f for every mix at or below a half", "[tone][mix]")
{
    // 2·(1 − m) 在 m ≤ 0.5 时不小于 1，取 min 之后恰为 1.0f：一个 ulp 都不能差
    for (int i = 0; i <= 20000; ++i)
    {
        const float m = static_cast<float>(i) * 0.5f / 20000.0f;
        INFO("m = " << m);
        CHECK(mixDryGain(m) == 1.0f);
    }
    // 刚过 50 % 就开始小于 1，律是连续的
    CHECK(mixDryGain(0.5f + 1.0e-4f) < 1.0f);
    CHECK(mixDryGain(0.5f + 1.0e-4f) > 0.999f);
}

TEST_CASE("both gains move monotonically with the mix", "[tone][mix]")
{
    float prevDry = mixDryGain(0.0f);
    float prevRet = mixReturnGain(0.0f);
    for (int i = 1; i <= 2000; ++i)
    {
        const float m = static_cast<float>(i) / 2000.0f;
        const float dry = mixDryGain(m);
        const float ret = mixReturnGain(m);
        INFO("m = " << m);
        CHECK(dry <= prevDry);
        CHECK(ret >= prevRet);
        prevDry = dry;
        prevRet = ret;
    }
    // 干声不增、回送不减之外，还要看得出确实在动
    CHECK(mixReturnGain(0.75f) == 1.0f);
    CHECK(mixDryGain(0.75f) == 0.5f);
}

TEST_CASE("out-of-range mixes clamp and NaN is treated as zero", "[tone][mix]")
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    for (const float m : {-1.0f, -0.0f, -inf, nan})
    {
        INFO("m = " << m);
        CHECK(clampMix(m) == 0.0f);
        CHECK(mixDryGain(m) == 1.0f);
        CHECK(mixReturnGain(m) == 0.0f);
    }
    for (const float m : {1.5f, 100.0f, inf})
    {
        INFO("m = " << m);
        CHECK(clampMix(m) == 1.0f);
        CHECK(mixDryGain(m) == 0.0f);
        CHECK(mixReturnGain(m) == 1.0f);
    }
    // 越界的输入与钳过的输入给同一结果
    CHECK(mixDryGain(-3.0f) == mixDryGain(0.0f));
    CHECK(mixReturnGain(7.0f) == mixReturnGain(1.0f));
}

TEST_CASE("the counterexample laws fail the same four points (the red cases)", "[tone][mix]")
{
    // 单旋钮那套在 25 % 上就与契约不同
    CHECK(wrongDryLinear(0.25f) != mixDryGain(0.25f));
    CHECK(wrongRetLinear(0.25f) != mixReturnGain(0.25f));
    // 余弦律在 25 % 上不同
    CHECK(wrongDryCos(0.25f) != mixDryGain(0.25f));
    CHECK(wrongRetSin(0.25f) != mixReturnGain(0.25f));
    // 契约里的律在 50 % 上 g_dry 恰为 1，而 min(1, 1 − m) 只有 0.5
    CHECK(mixDryGain(0.5f) == 1.0f);
    CHECK(wrongDryLinear(0.5f) == 0.5f);
    // 契约里的律在 100 % 上 g_dry 恰为 0，而单旋钮那套也是 0 —— 靠 25 % 与 50 % 两格区分
    CHECK(mixReturnGain(1.0f) == 1.0f);
    CHECK(wrongRetLinear(1.0f) == 1.0f);
    CHECK(wrongRetLinear(0.5f) != mixReturnGain(0.5f));
}
