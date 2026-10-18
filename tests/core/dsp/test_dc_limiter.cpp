// SPDX-License-Identifier: GPL-3.0-or-later
//
// DC 阻断（5 Hz 一阶高通）与峰值限制器（无前视、释放 80 ms、末级钳 ±c）。

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <limits>

#include "DspTestKit.h"
#include "dsp/DcBlocker.h"
#include "dsp/PeakLimiter.h"

using rinne::dsp::DcBlocker;
using rinne::dsp::PeakLimiter;
using rinne::test::isSubnormal;
using rinne::test::Rng;

namespace
{

// 稳态正弦增益：先走 2 s 让瞬态过去，再取 1 s 内的峰值
double sineGain(double freq, double sr)
{
    DcBlocker dc;
    dc.prepare(sr);
    double peak = 0.0;
    const auto settle = static_cast<std::int64_t>(2.0 * sr);
    const auto total = settle + static_cast<std::int64_t>(sr);
    for (std::int64_t n = 0; n < total; ++n)
    {
        const float y = dc.process(rinne::test::sine(freq, sr, n));
        if (n >= settle)
            peak = std::fmax(peak, std::fabs(static_cast<double>(y)));
    }
    return peak;
}

} // namespace

TEST_CASE("DC blocker corner is 5 Hz at every supported rate", "[dsp][dc]")
{
    for (const double sr : {44100.0, 48000.0, 96000.0, 192000.0})
    {
        INFO("sr = " << sr);
        // 一阶高通的 −3 dB 点
        CHECK(std::fabs(rinne::test::toDb(sineGain(5.0, sr)) + 3.01) < 0.15);
        CHECK(rinne::test::toDb(sineGain(20.0, sr)) > -0.35);
        CHECK(rinne::test::toDb(sineGain(50.0, sr)) > -0.06);
        CHECK(std::fabs(rinne::test::toDb(sineGain(1000.0, sr))) < 0.01);
        CHECK(rinne::test::toDb(sineGain(1.0, sr)) < -13.0);
    }
}

TEST_CASE("DC blocker removes a constant offset", "[dsp][dc]")
{
    DcBlocker dc;
    dc.prepare(48000.0);
    float y = 1.0f;
    for (int n = 0; n < 48000 * 2; ++n)
        y = dc.process(0.5f);
    CHECK(std::fabs(y) < 1.0e-6f);
}

TEST_CASE("DC blocker state never goes denormal on silence", "[dsp][dc][denormal]")
{
    DcBlocker dc;
    dc.prepare(48000.0);
    for (int n = 0; n < 4800; ++n)
        dc.process(rinne::test::sine(3.0, 48000.0, n));
    bool sawSubnormal = false;
    float last = 1.0f;
    for (int n = 0; n < 48000 * 60; ++n)
    {
        last = dc.process(0.0f);
        sawSubnormal = sawSubnormal || isSubnormal(last);
    }
    CHECK_FALSE(sawSubnormal);
    CHECK(last == 0.0f);
}

TEST_CASE("limiter output never exceeds the ceiling, bit for bit", "[dsp][limiter][bound]")
{
    Rng rng(7);
    PeakLimiter lim;
    lim.prepare(48000.0);
    for (int block = 0; block < 2000; ++block)
    {
        const auto ceiling = static_cast<float>(std::pow(10.0, rng.range(-24.0, 0.0) / 20.0));
        const double level = std::pow(10.0, rng.range(-40.0, 30.0) / 20.0);
        for (int i = 0; i < 256; ++i)
        {
            auto l = static_cast<float>(rng.range(-level, level));
            auto r = static_cast<float>(rng.range(-level, level));
            if (rng.below(50) == 0)
                l = static_cast<float>(level); // 方波样的满幅
            lim.process(l, r, ceiling);
            REQUIRE(std::fabs(l) <= ceiling);
            REQUIRE(std::fabs(r) <= ceiling);
        }
    }
}

TEST_CASE("limiter attacks instantly, links channels and releases with 80 ms", "[dsp][limiter]")
{
    const double sr = 48000.0;
    PeakLimiter lim;
    lim.prepare(sr);
    float l = 2.0f;
    float r = 0.1f;
    const float g = lim.process(l, r, 0.5f);
    CHECK(g == 0.25f); // 当样本就压到位
    CHECK(l == 0.5f);
    CHECK(r == 0.1f * 0.25f); // 右声道跟着左声道压
    // 之后静音：缩减量 (1 − env) 每 80 ms 衰减到 1/e
    const auto tau = static_cast<int>(0.080 * sr);
    double reduction = 0.0;
    for (int n = 0; n < tau; ++n)
    {
        float a = 0.0f;
        float b = 0.0f;
        reduction = 1.0 - lim.process(a, b, 0.5f);
    }
    CHECK(std::fabs(reduction / 0.75 - std::exp(-1.0)) < 1.0e-3);
}

TEST_CASE("limiter turns a non-finite pair into silence without poisoning its state", "[dsp][limiter]")
{
    PeakLimiter lim;
    lim.prepare(48000.0);
    float l = std::numeric_limits<float>::quiet_NaN();
    float r = 0.2f;
    lim.process(l, r, 0.5f);
    CHECK(l == 0.0f);
    CHECK(r == 0.0f);
    CHECK(lim.gain() == 1.0f);
    l = std::numeric_limits<float>::infinity();
    r = 0.0f;
    lim.process(l, r, 0.5f);
    CHECK(l == 0.0f);
    float a = 0.3f;
    float b = -0.3f;
    lim.process(a, b, 0.5f);
    CHECK(a == 0.3f);
    CHECK(b == -0.3f);
}

TEST_CASE("limiter gain recovers to exactly 1 without denormals", "[dsp][limiter][denormal]")
{
    PeakLimiter lim;
    lim.prepare(48000.0);
    float l = 8.0f;
    float r = 8.0f;
    lim.process(l, r, 0.0631f);
    bool sawSubnormal = false;
    for (int n = 0; n < 48000 * 10; ++n)
    {
        float a = 1.0e-20f;
        float b = 0.0f;
        const float g = lim.process(a, b, 0.0631f);
        sawSubnormal = sawSubnormal || isSubnormal(g) || isSubnormal(1.0f - g);
    }
    CHECK_FALSE(sawSubnormal);
    CHECK(lim.gain() == 1.0f);
}
