// SPDX-License-Identifier: GPL-3.0-or-later
//
// 线性平滑器（按样本计、终点精确、目标不变不重起）与 10 ms 格上的滑动统计（与暴力实现逐个一致）。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "DspTestKit.h"
#include "dsp/HopEnvelope.h"
#include "dsp/LinearSmoother.h"

using rinne::dsp::HopCountWindow;
using rinne::dsp::HopPeakHistory;
using rinne::dsp::LinearSmoother;
using rinne::test::Rng;

TEST_CASE("smoother lands exactly on the target after exactly n steps", "[dsp][smoother]")
{
    for (const double sr : {44100.0, 48000.0, 96000.0, 192000.0})
    {
        LinearSmoother s(0.0f);
        s.reset(sr, 0.030);
        const auto n = LinearSmoother::samplesFor(sr, 0.030);
        CHECK(s.defaultRampSamples() == n);
        s.setTargetValue(0.7f);
        float prev = 0.0f;
        for (std::int64_t i = 1; i < n; ++i)
        {
            const float v = s.getNextValue();
            REQUIRE(v > prev);
            REQUIRE(v < 0.7f);
            prev = v;
        }
        CHECK(s.isSmoothing());
        CHECK(s.getNextValue() == 0.7f);
        CHECK_FALSE(s.isSmoothing());
        CHECK(s.getNextValue() == 0.7f);
    }
}

TEST_CASE("long ramps have no step at the end (no accumulated drift)", "[dsp][smoother]")
{
    // 500 ms @ 192 kHz：每一步的增量都是 1/n 加减 float 在 1 附近的量化（两个 ulp 以内），最后一步不会出现
    // 逐步累加攒成的台阶（float 累加九万六千步，误差约 1e-3，是 1/n 的上百倍）
    const double sr = 192000.0;
    const auto n = LinearSmoother::samplesFor(sr, 0.5);
    LinearSmoother s(0.0f);
    s.setTargetValue(1.0f, n);
    float prev = 0.0f;
    double maxStep = 0.0;
    for (std::int64_t i = 0; i < n; ++i)
    {
        const float v = s.getNextValue();
        maxStep = std::fmax(maxStep, static_cast<double>(v) - prev);
        prev = v;
    }
    CHECK(prev == 1.0f);
    CHECK(maxStep < 1.0 / static_cast<double>(n) + 2.0 * 5.96e-8);
}

TEST_CASE("setting the same target does not restart the ramp", "[dsp][smoother]")
{
    LinearSmoother s(0.0f);
    s.reset(1000.0, 0.010); // 10 步
    s.setTargetValue(1.0f);
    for (int i = 0; i < 5; ++i)
        s.getNextValue();
    s.setTargetValue(1.0f);
    for (int i = 0; i < 4; ++i)
        s.getNextValue();
    CHECK(s.getNextValue() == 1.0f);
    CHECK_FALSE(s.isSmoothing());
}

TEST_CASE("a new target mid-ramp starts from the current value", "[dsp][smoother]")
{
    LinearSmoother s(1.0f);
    s.setTargetValue(0.0f, 10);
    for (int i = 0; i < 5; ++i)
        s.getNextValue();
    CHECK(s.getCurrentValue() == 0.5f);
    s.setTargetValue(1.0f, 5);
    CHECK(s.getNextValue() == 0.6f);
    s.setTargetValue(0.0f, 0); // 0 步：直接落定
    CHECK(s.getCurrentValue() == 0.0f);
    CHECK_FALSE(s.isSmoothing());
}

TEST_CASE("hop peak history matches a brute-force sliding maximum", "[dsp][hop]")
{
    constexpr int cap = 64;
    const std::int64_t hop = 37;
    HopPeakHistory<cap> h;
    h.prepare(hop);
    Rng rng(3);
    std::vector<float> samples;
    for (int i = 0; i < 37 * 200; ++i)
    {
        const float v = static_cast<float>(rng.uniform());
        samples.push_back(v);
        const bool done = h.push(v);
        const auto count = static_cast<std::int64_t>(samples.size());
        REQUIRE(done == (count % hop == 0));
        if (!done)
            continue;
        const auto completed = count / hop;
        for (const int window : {1, 5, 10, 63, 64, 100})
        {
            const std::int64_t first = std::max<std::int64_t>(0, completed - std::min<std::int64_t>(window, cap));
            float want = 0.0f;
            for (std::int64_t k = first * hop; k < completed * hop; ++k)
                want = std::fmax(want, samples[static_cast<std::size_t>(k)]);
            REQUIRE(h.maxOfLast(window) == want);
        }
    }
}

TEST_CASE("hop count window matches brute force: n whole hops plus the partial one", "[dsp][hop]")
{
    constexpr int window = 30;
    const std::int64_t hop = 17;
    HopCountWindow<window + 1> w;
    w.prepare(hop, window);
    Rng rng(5);
    std::vector<int> hits;
    for (int i = 0; i < 17 * 120; ++i)
    {
        const bool hit = rng.below(3) == 0;
        hits.push_back(hit ? 1 : 0);
        w.push(hit);
        const auto count = static_cast<std::int64_t>(hits.size());
        const std::int64_t partialStart = (count / hop) * hop;
        const std::int64_t first = std::max<std::int64_t>(0, partialStart - window * hop);
        std::int64_t want = 0;
        for (std::int64_t k = first; k < count; ++k)
            want += hits[static_cast<std::size_t>(k)];
        REQUIRE(w.sum() == want);
    }
}

TEST_CASE("hop length is round(10 ms) of the sample rate", "[dsp][hop]")
{
    CHECK(rinne::dsp::hopSamplesFor(48000.0) == 480);
    CHECK(rinne::dsp::hopSamplesFor(44100.0) == 441);
    CHECK(rinne::dsp::hopSamplesFor(192000.0) == 1920);
    CHECK(rinne::dsp::hopSamplesFor(0.0) == 1);
}
