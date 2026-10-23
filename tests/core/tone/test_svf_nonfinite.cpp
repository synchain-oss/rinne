// SPDX-License-Identifier: GPL-3.0-or-later
//
// 非有限值进了 SVF 之后的分工：状态清、输出不清。
//   状态：SvfTpt 的两个状态经 flushState 写回（非有限 → +0，其余同 flushTiny），一个坏样本不会把状态永久卡在 NaN 上，
//   下一个样本起与刚 reset 过的滤波器逐位相同；
//   输出：只过 flushTiny，flushTiny 不清非有限值，坏的这一格输出非有限值，原样往下走，由 ⑥ 的有限性检查跳闸
//   （Input 内部产生的非有限值立即跳闸，不能在音色级被清成 0）。
// 必须变红：
//   状态写回改用 flushTiny：NaN 留在状态里，之后每个样本都是 NaN，「之后与新建的逐位相同、全部有限」变红；
//   输出改用 flushState：坏的这一格输出 +0，「这一格是非有限值」变红；
//   flushTiny 改成清非有限值：「flushTiny 对非有限值原样返回」变红，坏的这一格同样输出 +0。

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
using rinne::dsp::flushState;
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

// 有限值上 flushState 与 flushTiny 都是：|x| < 1e-30 冲成 +0（含 −0.0 与 denormal），其余原样
void checkFiniteFlush(double (*flush)(double))
{
    CHECK(doubleBits(flush(-0.0)) == 0u);
    CHECK(doubleBits(flush(std::numeric_limits<double>::denorm_min())) == 0u);
    CHECK(doubleBits(flush(-1.0e-31)) == 0u);
    for (const double x :
         {1.0e-29, -1.0e-29, 0.5, -16.0, std::numeric_limits<double>::max(), std::numeric_limits<double>::lowest()})
    {
        INFO("x = " << x);
        CHECK(isFiniteSample(x));
        CHECK(doubleBits(flush(x)) == doubleBits(x));
    }
}

double flushTinyDouble(double x)
{
    return flushTiny(x);
}

double flushStateDouble(double x)
{
    return flushState(x);
}

} // namespace

TEST_CASE("flushTiny leaves NaN and infinities as they are, in both overloads", "[tone][svf][nonfinite]")
{
    // 非有限值原样返回（逐位，含 NaN 载荷与符号）：它们要留给 ⑥ 跳闸
    for (const std::uint32_t b : kBadFloatBits)
    {
        INFO("float bits = " << b);
        CHECK_FALSE(isFiniteSample(fromBits(b)));
        CHECK(floatBits(flushTiny(fromBits(b))) == b);
    }
    for (const std::uint64_t b : kBadDoubleBits)
    {
        INFO("double bits = " << b);
        CHECK_FALSE(isFiniteSample(doubleFromBits(b)));
        CHECK(doubleBits(flushTiny(doubleFromBits(b))) == b);
    }

    // 有限值：|x| < 1e-30 冲成 +0（含 −0.0 与 denormal），其余原样
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
    checkFiniteFlush(flushTinyDouble);
}

TEST_CASE("flushState turns NaN and infinities into +0 and otherwise acts like flushTiny", "[tone][svf][nonfinite]")
{
    for (const std::uint64_t b : kBadDoubleBits)
    {
        INFO("double bits = " << b);
        CHECK(doubleBits(flushState(doubleFromBits(b))) == 0u);
    }
    checkFiniteFlush(flushStateDouble);
}

TEST_CASE("a non-finite input gives one non-finite SVF output, then it runs like a freshly reset one",
          "[tone][svf][nonfinite]")
{
    // 先用噪声把状态跑热，注入一个坏样本，再喂一段通带正弦：坏样本那一格输出非有限值（往下交给 ⑥ 跳闸）；之后与刚
    // reset 的滤波器逐位相同（两个状态清回了 +0），输出全部有限，通带正弦的幅度也在（不是一直输出 0）
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
                CHECK_FALSE(isFiniteSample(t.process(fromBits(bad), c, resp)));

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

TEST_CASE("a NaN reaching a running cut filter gives one non-finite sample on its own channel only",
          "[tone][cut][nonfinite]")
{
    // 开着、交叉已走完的低切 / 高切，左声道来一个 NaN：这一格左声道输出非有限值；之后左声道与刚 prepare 的同参数实例
    // 逐位相同（状态清回 +0）、全部有限，右声道（包括坏的那一格）与没进过 NaN 的实例逐位相同（两声道的状态互不影响）
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
            CHECK_FALSE(isFiniteSample(l));
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

TEST_CASE("a NaN mid-crossfade or in bypass leaves the cut filter as a non-finite sample", "[tone][cut][nonfinite]")
{
    // 另两种情形同样往下交给 ⑥：开关交叉进行中（干声那一路的 NaN 混进输出），关态逐位直通（NaN 原样输出，载荷不变）。
    // 交叉进行中的那一格之后，左声道恢复成有限值（状态清回了 +0），右声道与没进过 NaN 的实例逐位相同
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (const double sr : kRates)
    {
        for (const CutKind kind : {CutKind::LowCut, CutKind::HighCut})
        {
            INFO("sr = " << sr << ", kind = " << static_cast<int>(kind));
            const float off = kind == CutKind::LowCut ? 20.0f : 20000.0f;
            const float on = kind == CutKind::LowCut ? 200.0f : 4000.0f;

            // 关态：滤波器不跑，NaN 逐位原样
            CutFilter bypassed(kind);
            bypassed.setCutoff(off);
            bypassed.prepare(sr);
            REQUIRE(bypassed.isBypassed());
            float bl = nan;
            float br = 0.25f;
            bypassed.process(bl, br);
            CHECK(floatBits(bl) == floatBits(nan));
            CHECK(floatBits(br) == floatBits(0.25f));

            // 交叉进行中：从关切到开，走 2 ms（10 ms 交叉的五分之一）后来一个 NaN
            CutFilter f(kind);
            CutFilter untouched(kind);
            for (CutFilter* p : {&f, &untouched})
            {
                p->setCutoff(off);
                p->prepare(sr);
                p->setCutoff(on);
            }
            Rng rng(static_cast<std::uint64_t>(sr) + 7u * static_cast<std::uint64_t>(kind));
            const auto lead = static_cast<int>(0.002 * sr);
            for (int i = 0; i < lead; ++i)
            {
                float l = static_cast<float>(rng.range(-1.0, 1.0));
                float r = static_cast<float>(rng.range(-1.0, 1.0));
                float ul = l;
                float ur = r;
                f.process(l, r);
                untouched.process(ul, ur);
            }
            REQUIRE(f.isSmoothing());
            float l = nan;
            float r = 0.25f;
            float ul = 0.25f;
            float ur = 0.25f;
            f.process(l, r);
            untouched.process(ul, ur);
            CHECK_FALSE(isFiniteSample(l));
            CHECK(floatBits(r) == floatBits(ur));

            bool rightUntouched = true;
            bool finite = true;
            for (int n = 0; n < 4800; ++n)
            {
                float xl = static_cast<float>(rng.range(-1.0, 1.0));
                float xr = static_cast<float>(rng.range(-1.0, 1.0));
                float tl = xl;
                float tr = xr;
                f.process(xl, xr);
                untouched.process(tl, tr);
                rightUntouched = rightUntouched && floatBits(xr) == floatBits(tr);
                finite = finite && isFiniteSample(xl) && isFiniteSample(xr);
            }
            CHECK(rightUntouched);
            CHECK(finite);
        }
    }
}
