// SPDX-License-Identifier: GPL-3.0-or-later
//
// 非有限值进了 SVF 之后能出来：flushTiny 把 NaN、±Inf 冲成 +0（float 与 double 两个重载都是），SVF 的两个状态
// 经它写回。一个非有限的输入样本只让这一个样本输出 +0、状态清回 0，之后与刚 reset 过的滤波器逐位相同。
// 必须变红：flushTiny 只留「|x| < 1e-30」那两个比较时，NaN 让两个比较都为假、原样通过，状态从此一直是 NaN，
// 之后每个样本都是 NaN。下面三个用例都会红。

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include "ToneTestKit.h"
#include "dsp/Sanitize.h"
#include "dsp/SvfTpt.h"

using rinne::dsp::CutFilter;
using rinne::dsp::CutKind;
using rinne::dsp::flushTiny;
using rinne::dsp::isFiniteSample;
using rinne::dsp::svfCoefficients;
using rinne::dsp::SvfResponse;
using rinne::dsp::SvfTpt;
using rinne::test::floatBits;
using rinne::test::fromBits;
using rinne::test::Rng;

namespace
{

constexpr double kRates[] = {44100.0, 48000.0, 96000.0, 192000.0};

// 非有限的 float 位型：quiet NaN（正负）、signaling NaN、带载荷的 NaN、最大载荷的 NaN、±Inf
constexpr std::uint32_t kBadFloatBits[] = {0x7fc00000u, 0xffc00000u, 0x7f800001u, 0xffc00123u,
                                           0x7fffffffu, 0x7f800000u, 0xff800000u};

// 同样的几类，double 位型
constexpr std::uint64_t kBadDoubleBits[] = {0x7ff8000000000000ull, 0xfff8000000000000ull, 0x7ff0000000000001ull,
                                            0xfff8000000000123ull, 0x7fffffffffffffffull, 0x7ff0000000000000ull,
                                            0xfff0000000000000ull};

std::uint64_t doubleBits(double d)
{
    std::uint64_t b = 0;
    std::memcpy(&b, &d, sizeof b);
    return b;
}

double doubleFromBits(std::uint64_t b)
{
    double d = 0.0;
    std::memcpy(&d, &b, sizeof d);
    return d;
}

} // namespace

TEST_CASE("flushTiny turns NaN and infinities into +0 in both overloads", "[tone][svf][nonfinite]")
{
    for (const std::uint32_t b : kBadFloatBits)
    {
        INFO("float bits = " << b);
        CHECK_FALSE(isFiniteSample(fromBits(b)));
        CHECK(floatBits(flushTiny(fromBits(b))) == 0u);
    }
    for (const std::uint64_t b : kBadDoubleBits)
    {
        INFO("double bits = " << b);
        CHECK_FALSE(isFiniteSample(doubleFromBits(b)));
        CHECK(doubleBits(flushTiny(doubleFromBits(b))) == 0u);
    }

    // 有限值的行为不变：|x| < 1e-30 冲成 +0（含 −0.0 与 denormal），其余原样
    CHECK(floatBits(flushTiny(fromBits(0x80000000u))) == 0u);
    CHECK(floatBits(flushTiny(fromBits(0x00000001u))) == 0u);
    CHECK(floatBits(flushTiny(-1.0e-31f)) == 0u);
    for (const float x :
         {1.0e-29f, -1.0e-29f, 0.5f, -16.0f, std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest()})
    {
        INFO("x = " << x);
        CHECK(isFiniteSample(x));
        CHECK(floatBits(flushTiny(x)) == floatBits(x));
    }
    CHECK(doubleBits(flushTiny(-0.0)) == 0u);
    CHECK(doubleBits(flushTiny(std::numeric_limits<double>::denorm_min())) == 0u);
    CHECK(doubleBits(flushTiny(-1.0e-31)) == 0u);
    for (const double x :
         {1.0e-29, -1.0e-29, 0.5, -16.0, std::numeric_limits<double>::max(), std::numeric_limits<double>::lowest()})
    {
        INFO("x = " << x);
        CHECK(isFiniteSample(x));
        CHECK(doubleBits(flushTiny(x)) == doubleBits(x));
    }
}

TEST_CASE("a non-finite input costs the SVF one zero sample, then it runs like a freshly reset one",
          "[tone][svf][nonfinite]")
{
    // 先用噪声把状态跑热，注入一个坏样本，再喂一段通带正弦：坏样本那一格输出 +0；之后与刚 reset 的滤波器逐位相同
    // （两个状态清回了 0），输出有限，通带正弦的幅度也在（不是一直输出 0）
    for (const double sr : kRates)
    {
        for (const SvfResponse resp : {SvfResponse::HighPass, SvfResponse::LowPass})
        {
            const bool high = resp == SvfResponse::HighPass;
            const auto c = svfCoefficients(high ? 200.0 : 4000.0, sr);
            const double passFreq = high ? 2000.0 : 400.0;
            for (const std::uint32_t bad : kBadFloatBits)
            {
                INFO("sr = " << sr << ", high-pass = " << high << ", bits = " << bad);
                SvfTpt t;
                Rng rng(static_cast<std::uint64_t>(sr) + bad);
                for (int i = 0; i < 2000; ++i)
                    t.process(static_cast<float>(rng.range(-1.0, 1.0)), c, resp);
                CHECK(floatBits(t.process(fromBits(bad), c, resp)) == 0u);

                SvfTpt fresh;
                bool same = true;
                bool finite = true;
                double peak = 0.0;
                for (int n = 0; n < 4800; ++n)
                {
                    const float x = rinne::test::sine(passFreq, sr, n, 0.5);
                    const float y = t.process(x, c, resp);
                    same = same && floatBits(y) == floatBits(fresh.process(x, c, resp));
                    finite = finite && isFiniteSample(y);
                    if (n >= 2400)
                        peak = std::fmax(peak, std::fabs(static_cast<double>(y)));
                }
                CHECK(same);
                CHECK(finite);
                CHECK(peak > 0.45);
                CHECK(peak < 0.55);
            }
        }
    }
}

TEST_CASE("a NaN reaching a running cut filter costs one sample on its own channel only", "[tone][cut][nonfinite]")
{
    // 开着、交叉已走完的低切 / 高切，左声道来一个 NaN：这一格左声道输出 +0；之后左声道与刚 prepare 的同参数实例
    // 逐位相同（状态清回 0），右声道与没进过 NaN 的实例逐位相同（两声道的状态互不影响）
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (const double sr : kRates)
    {
        for (const CutKind kind : {CutKind::LowCut, CutKind::HighCut})
        {
            INFO("sr = " << sr << ", kind = " << static_cast<int>(kind));
            const float hz = kind == CutKind::LowCut ? 200.0f : 4000.0f;
            CutFilter f(kind);
            CutFilter untouched(kind);
            CutFilter fresh(kind);
            for (CutFilter* p : {&f, &untouched})
            {
                p->setCutoff(hz);
                p->prepare(sr);
            }
            Rng rng(static_cast<std::uint64_t>(sr) + static_cast<std::uint64_t>(kind));
            for (int i = 0; i < 2000; ++i)
            {
                float l = static_cast<float>(rng.range(-1.0, 1.0));
                float r = static_cast<float>(rng.range(-1.0, 1.0));
                float ul = l;
                float ur = r;
                f.process(l, r);
                untouched.process(ul, ur);
            }
            fresh.setCutoff(hz);
            fresh.prepare(sr);
            REQUIRE_FALSE(f.isSmoothing());

            float l = nan;
            float r = 0.25f;
            float ul = 0.25f;
            float ur = 0.25f;
            f.process(l, r);
            untouched.process(ul, ur);
            CHECK(floatBits(l) == 0u);
            CHECK(floatBits(r) == floatBits(ur));

            bool leftLikeFresh = true;
            bool rightUntouched = true;
            bool finite = true;
            for (int n = 0; n < 4800; ++n)
            {
                float xl = static_cast<float>(rng.range(-1.0, 1.0));
                float xr = static_cast<float>(rng.range(-1.0, 1.0));
                float fl = xl;
                float fr = xr;
                float tl = xl;
                float tr = xr;
                f.process(xl, xr);
                fresh.process(fl, fr);
                untouched.process(tl, tr);
                leftLikeFresh = leftLikeFresh && floatBits(xl) == floatBits(fl);
                rightUntouched = rightUntouched && floatBits(xr) == floatBits(tr);
                finite = finite && isFiniteSample(xl) && isFiniteSample(xr);
            }
            CHECK(leftLikeFresh);
            CHECK(rightUntouched);
            CHECK(finite);
        }
    }
}
