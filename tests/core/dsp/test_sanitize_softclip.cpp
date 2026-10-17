// SPDX-License-Identifier: GPL-3.0-or-later
//
// 清洗与软削波：回送硬上界的两端。清洗之后一切有限且 |x| ≤ 16；软削波之后 |y| < 1.0f，且永不放大。

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <limits>

#include "DspTestKit.h"
#include "dsp/Sanitize.h"
#include "dsp/SoftClip.h"

using rinne::dsp::isFiniteSample;
using rinne::dsp::kSanitizeLimit;
using rinne::dsp::kSoftClipKnee;
using rinne::dsp::kSoftClipMax;
using rinne::dsp::sanitizeSample;
using rinne::dsp::softClip;
using rinne::test::floatBits;
using rinne::test::fromBits;

TEST_CASE("sanitize zeroes every non-finite pattern and clamps to 16", "[dsp][sanitize]")
{
    const float inf = std::numeric_limits<float>::infinity();
    for (const float bad : {inf, -inf, std::numeric_limits<float>::quiet_NaN(), fromBits(0x7f800001u),
                            fromBits(0xffc00123u), fromBits(0x7fffffffu)})
    {
        bool flagged = false;
        CHECK(floatBits(sanitizeSample(bad, flagged)) == 0u);
        CHECK(flagged);
    }
    bool flagged = false;
    CHECK(sanitizeSample(16.0f, flagged) == 16.0f);
    CHECK(sanitizeSample(16.5f, flagged) == kSanitizeLimit);
    CHECK(sanitizeSample(-1.0e30f, flagged) == -kSanitizeLimit);
    CHECK(sanitizeSample(std::numeric_limits<float>::max(), flagged) == kSanitizeLimit);
    CHECK(sanitizeSample(0.25f, flagged) == 0.25f);
    CHECK(sanitizeSample(1.0e-29f, flagged) == 1.0e-29f);
    // 小于 1e-30 的（含全部 denormal）冲成 +0
    CHECK(floatBits(sanitizeSample(9.0e-31f, flagged)) == 0u);
    CHECK(floatBits(sanitizeSample(fromBits(0x00000001u), flagged)) == 0u);
    CHECK(floatBits(sanitizeSample(-fromBits(0x007fffffu), flagged)) == 0u);
    CHECK_FALSE(flagged);
}

TEST_CASE("sanitize output is always finite and bounded for random bit patterns", "[dsp][sanitize]")
{
    rinne::test::Rng rng(11);
    for (int i = 0; i < 1000000; ++i)
    {
        const float y = sanitizeSample(rng.bits());
        REQUIRE(isFiniteSample(y));
        REQUIRE(std::fabs(y) <= kSanitizeLimit);
        REQUIRE_FALSE(rinne::test::isSubnormal(y));
    }
}

TEST_CASE("soft clip is the identity up to the knee, bit for bit", "[dsp][softclip]")
{
    for (const float x : {0.0f, -0.0f, 1.0e-20f, 0.1f, -0.5f, 0.69999f, kSoftClipKnee, -kSoftClipKnee})
        CHECK(floatBits(softClip(x)) == floatBits(x));
    bool engaged = false;
    softClip(kSoftClipKnee, engaged);
    CHECK_FALSE(engaged);
    softClip(std::nextafter(kSoftClipKnee, 1.0f), engaged);
    CHECK(engaged);
}

TEST_CASE("soft clip stays finite and strictly below 1.0f at the extremes", "[dsp][softclip][bound]")
{
    const float inf = std::numeric_limits<float>::infinity();
    for (const float x : {3.5f, -3.5f, 24.0f, -24.0f, 1.0e6f, -1.0e6f, 16.0f, 1.0f, -1.0f, inf, -inf,
                          std::numeric_limits<float>::max(), std::numeric_limits<float>::quiet_NaN()})
    {
        const float y = softClip(x);
        INFO("x = " << x << " y = " << y);
        REQUIRE(isFiniteSample(y));
        REQUIRE(std::fabs(y) < 1.0f);
    }
    CHECK(softClip(1.0e6f) == kSoftClipMax);
    CHECK(softClip(-inf) == -kSoftClipMax);
    CHECK(floatBits(softClip(std::numeric_limits<float>::quiet_NaN())) == 0u);
}

TEST_CASE("soft clip never expands, is odd and monotonic over a dense sweep", "[dsp][softclip][bound]")
{
    // 从拐点到 +Inf 的每第 512 个 float 位型（约 210 万点），拐点之上的头 4096 个逐 ulp 走
    float prev = softClip(kSoftClipKnee);
    const std::uint32_t lo = floatBits(kSoftClipKnee);
    const std::uint32_t hi = floatBits(std::numeric_limits<float>::infinity());
    for (std::uint32_t b = lo; b < hi; b += (b - lo < 4096u ? 1u : 512u))
    {
        const float x = fromBits(b);
        const float y = softClip(x);
        REQUIRE(y < 1.0f);
        REQUIRE(y <= x);
        // 各平台的 tanhf 只保证误差在 1 ulp 内，单调性按 1 ulp 的容差查
        REQUIRE(y >= std::nextafter(prev, 0.0f));
        REQUIRE(softClip(-x) == -y);
        prev = y;
    }
}

TEST_CASE("soft clip has a continuous slope at the knee", "[dsp][softclip]")
{
    const float d = 1.0e-3f;
    const float slope = (softClip(kSoftClipKnee + d) - softClip(kSoftClipKnee)) / d;
    CHECK(std::fabs(slope - 1.0f) < 0.01f);
}
