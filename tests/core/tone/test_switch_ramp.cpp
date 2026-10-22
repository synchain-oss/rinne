// SPDX-License-Identifier: GPL-3.0-or-later
//
// 开关量的定长线性交叉过渡（限制器开关 10 ms、极性反转）：
//   两路增益之和恒为 1（不是约等于，是逐位恰为 1.0f）、端点精确（那一路的样本原样返回，−0.0 的符号与 NaN 载荷不变）、
//   过渡中再切换从当前位置折返（不跳回起点）、相邻样本无阶跃（无咔嗒）。
// 必须变红：把 b 改成等功率的 sin/cos、把 b 换成不量化的 k/n、把端点改成乘加、过渡中改成「跳回起点重来」，
// 下面都有反例格子会红。

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "ToneTestKit.h"
#include "dsp/PeakLimiter.h"
#include "dsp/SwitchRamp.h"

using rinne::dsp::kSwitchRampSeconds;
using rinne::dsp::PeakLimiter;
using rinne::dsp::SwitchRamp;
using rinne::test::floatBits;
using rinne::test::fromBits;
using rinne::test::Rng;
using rinne::test::sineStepBound;

namespace
{

constexpr double kRates[] = {44100.0, 48000.0, 96000.0, 192000.0};

std::int64_t rampLength(double sr, double seconds = kSwitchRampSeconds)
{
    return static_cast<std::int64_t>(std::llround(seconds * sr));
}

double maxStep(const std::vector<float>& v)
{
    double worst = 0.0;
    for (std::size_t i = 1; i < v.size(); ++i)
        worst = std::fmax(worst, std::fabs(static_cast<double>(v[i]) - v[i - 1]));
    return worst;
}

double peakOf(const std::vector<float>& v)
{
    double worst = 0.0;
    for (const float x : v)
        worst = std::fmax(worst, std::fabs(static_cast<double>(x)));
    return worst;
}

// 限制器一路：连续跑（状态是热的），给出「开」那一路的样本
std::vector<float> limitedPath(double sr, double ceiling, const std::vector<float>& x)
{
    PeakLimiter limiter;
    limiter.prepare(sr);
    std::vector<float> out(x.size());
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        float l = x[i];
        float r = x[i];
        limiter.process(l, r, static_cast<float>(ceiling));
        out[i] = l;
    }
    return out;
}

// 相邻样本差的上界：两路各自的斜率，加上交叉期间两路之差摊到 n 个样本上的那一项
double clickBound(const std::vector<float>& off, const std::vector<float>& on, std::int64_t n)
{
    double diff = 0.0;
    for (std::size_t i = 0; i < off.size(); ++i)
        diff = std::fmax(diff, std::fabs(static_cast<double>(off[i]) - on[i]));
    return maxStep(off) + maxStep(on) + diff / static_cast<double>(n);
}

} // namespace

TEST_CASE("the two gains always add up to exactly one", "[tone][switch]")
{
    for (const double sr : kRates)
    {
        INFO("sr = " << sr);
        SwitchRamp r;
        r.prepare(sr, kSwitchRampSeconds);
        CHECK(r.lengthSamples() == rampLength(sr));
        for (std::int64_t k = 0; k <= r.lengthSamples(); ++k)
        {
            // 直接推位置：把目标设成开、一步步走，再逐位核对
            SwitchRamp s;
            s.prepare(sr, kSwitchRampSeconds);
            s.setTarget(true);
            for (std::int64_t i = 0; i < k; ++i)
                s.step();
            INFO("k = " << k);
            CHECK(s.position() == k);
            const float b = s.onGain();
            const float g = s.offGain();
            CHECK(b + g == 1.0f);
            CHECK(b >= 0.0f);
            CHECK(b <= 1.0f);
            CHECK(g >= 0.0f);
            CHECK(g <= 1.0f);
        }
    }
}

TEST_CASE("the ramp is linear, monotonic and lands exactly on its ends", "[tone][switch]")
{
    for (const double sr : kRates)
    {
        INFO("sr = " << sr);
        const auto n = rampLength(sr);
        SwitchRamp r;
        r.prepare(sr, kSwitchRampSeconds);
        r.setTarget(true);
        CHECK(r.position() == 0);
        CHECK(r.onGain() == 0.0f);
        CHECK(r.offGain() == 1.0f);
        CHECK(r.isFullyOff());
        float prev = r.onGain();
        bool monotonic = true;
        bool inRange = true;
        for (std::int64_t i = 1; i <= n; ++i)
        {
            r.step();
            const float b = r.onGain();
            monotonic = monotonic && b >= prev;
            // 量化到 2^-24：与 k/n 的差不超过半个量化单位
            inRange = inRange && std::fabs(static_cast<double>(b) - static_cast<double>(i) / static_cast<double>(n)) <=
                                     1.0 / 16777216.0;
            prev = b;
        }
        CHECK(monotonic);
        CHECK(inRange);
        CHECK(r.position() == n);
        CHECK(r.onGain() == 1.0f);
        CHECK(r.offGain() == 0.0f);
        CHECK(r.isFullyOn());
        CHECK(r.isSettled());
        // 再走一步不动
        r.step();
        CHECK(r.position() == n);
        // 反方向走回去同理
        r.setTarget(false);
        CHECK(r.position() == n);
        r.step();
        CHECK(r.position() == n - 1);
    }
}

TEST_CASE("the ends return their own path bit for bit", "[tone][switch][bypass]")
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    for (const double sr : kRates)
    {
        INFO("sr = " << sr);
        SwitchRamp r;
        r.prepare(sr, kSwitchRampSeconds);
        Rng rng(31);
        // 关的那一端
        for (int i = 0; i < 2000; ++i)
        {
            const float off = rng.bits();
            const float on = rng.bits();
            CHECK(floatBits(r.mix(off, on)) == floatBits(off));
        }
        // 开的那一端
        r.setTarget(true);
        r.snapToTarget();
        CHECK(r.isFullyOn());
        for (int i = 0; i < 2000; ++i)
        {
            const float off = rng.bits();
            const float on = rng.bits();
            CHECK(floatBits(r.mix(off, on)) == floatBits(on));
        }
        // −0.0 的符号与 NaN 载荷原样保留
        r.setTarget(false);
        r.snapToTarget();
        CHECK(floatBits(r.mix(fromBits(0x80000000u), nan)) == 0x80000000u);
        CHECK(floatBits(r.mix(fromBits(0x80000000u), inf)) == 0x80000000u);
        r.setTarget(true);
        r.snapToTarget();
        const auto nanBits = floatBits(nan);
        CHECK(floatBits(r.mix(0.0f, nan)) == nanBits);
    }
    // 必须变红：端点改成乘加，−0.0 会变成 +0.0、另一路的 NaN 会混进来
    const float mixed = (1.0f - 0.0f) * fromBits(0x80000000u) + 0.0f * nan;
    CHECK(floatBits(mixed) != 0x80000000u);
}

TEST_CASE("switching again mid-transition turns around in place", "[tone][switch][continuity]")
{
    for (const double sr : kRates)
    {
        INFO("sr = " << sr);
        const auto n = rampLength(sr);
        SwitchRamp r;
        r.prepare(sr, kSwitchRampSeconds);
        // 开到一半
        r.setTarget(true);
        for (std::int64_t i = 0; i < n / 2; ++i)
            r.step();
        const auto mid = r.position();
        CHECK(mid == n / 2);
        // 反向：位置不动，下一步开始往回走，不跳回起点
        r.setTarget(false);
        CHECK(r.position() == mid);
        r.step();
        CHECK(r.position() == mid - 1);
        r.step();
        CHECK(r.position() == mid - 2);

        // 折返的另一半同样逐样本走回起点
        SwitchRamp q;
        q.prepare(sr, kSwitchRampSeconds);
        q.setTarget(true);
        for (std::int64_t i = 0; i < n / 2; ++i)
            q.step();
        q.setTarget(false);
        for (std::int64_t i = 0; i < n / 2; ++i)
            q.step();
        CHECK(q.position() == 0);
        CHECK(q.isFullyOff());
        CHECK(q.onGain() == 0.0f);

        // 位置在任意一串来回切换下每样本最多走一步
        SwitchRamp s;
        s.prepare(sr, kSwitchRampSeconds);
        Rng rng(37);
        std::int64_t prev = s.position();
        bool continuous = true;
        for (int i = 0; i < 20000; ++i)
        {
            if (rng.uniform() < 0.02)
                s.setTarget(rng.uniform() < 0.5);
            s.step();
            const std::int64_t d = s.position() - prev;
            continuous = continuous && std::llabs(d) <= 1;
            prev = s.position();
        }
        CHECK(continuous);
    }
}

TEST_CASE("the limiter toggle crossfades in 10 ms without a click", "[tone][switch][click]")
{
    // 判据：限制器一路与旁路一路的相邻样本差上界，加上交叉期间两路之差摊到 n 个样本上的那一项。
    // 事件落在正弦的峰上：那里两路差最大，硬切换的台阶最明显。
    constexpr double freq = 100.0;
    constexpr double amp = 0.9;
    for (const double sr : kRates)
    {
        INFO("sr = " << sr);
        const double ceiling = std::pow(10.0, -6.0 / 20.0);
        const auto n = rampLength(sr);
        const auto total = static_cast<std::size_t>(0.4 * sr);
        const auto at = static_cast<std::size_t>(std::llround(0.3 * sr + sr / freq / 4.0));
        std::vector<float> x(total);
        for (std::size_t i = 0; i < total; ++i)
        {
            const double phase = 2.0 * rinne::test::kPi * freq * static_cast<double>(i) / sr;
            x[i] = static_cast<float>(amp * std::sin(phase));
        }
        const std::vector<float> on = limitedPath(sr, ceiling, x);

        // 交叉
        SwitchRamp r;
        r.prepare(sr, kSwitchRampSeconds);
        r.setTarget(false);
        r.snapToTarget();
        std::vector<float> cross(total);
        for (std::size_t i = 0; i < total; ++i)
        {
            if (i >= at)
                r.setTarget(true);
            r.step();
            cross[i] = r.mix(x[i], on[i]);
        }
        const double bound = clickBound(x, on, n);
        CHECK(maxStep(cross) < bound);

        // 必须变红：硬切换（没有交叉）在同一个事件上远远超过上界
        std::vector<float> hard(total);
        for (std::size_t i = 0; i < total; ++i)
            hard[i] = i < at ? x[i] : on[i];
        CHECK(maxStep(hard) > 3.0 * bound);
    }
}

TEST_CASE("the polarity flip crossfades in 10 ms without a click", "[tone][switch][click]")
{
    constexpr double freq = 100.0;
    constexpr double amp = 0.9;
    for (const double sr : kRates)
    {
        INFO("sr = " << sr);
        const auto n = rampLength(sr);
        const auto total = static_cast<std::size_t>(0.4 * sr);
        const auto at = static_cast<std::size_t>(std::llround(0.3 * sr + sr / freq / 4.0));
        std::vector<float> x(total);
        std::vector<float> negated(total);
        for (std::size_t i = 0; i < total; ++i)
        {
            const double phase = 2.0 * rinne::test::kPi * freq * static_cast<double>(i) / sr;
            x[i] = static_cast<float>(amp * std::sin(phase));
            negated[i] = -x[i];
        }
        SwitchRamp r;
        r.prepare(sr, kSwitchRampSeconds);
        r.setTarget(false);
        r.snapToTarget();
        std::vector<float> cross(total);
        for (std::size_t i = 0; i < total; ++i)
        {
            if (i >= at)
                r.setTarget(true);
            r.step();
            cross[i] = r.mix(x[i], negated[i]);
        }
        const double bound = clickBound(x, negated, n);
        CHECK(maxStep(cross) < bound);

        // 必须变红：硬切极性（直接取反）在同一个事件上远远超过上界
        std::vector<float> hard(total);
        for (std::size_t i = 0; i < total; ++i)
            hard[i] = i < at ? x[i] : negated[i];
        CHECK(maxStep(hard) > 3.0 * bound);
    }
}

TEST_CASE("the linear crossfade is chosen because an equal-power one overshoots on correlated paths",
          "[tone][switch][click]")
{
    // 两路完全相关（这里是同一路）时，线性交叉的原样通过：out = (1 − b)·s + b·s = s，永远不超。
    // 等功率交叉是 out = cos(b·π/2)·s1 + sin(b·π/2)·s2；两路相同时在中点 (cos + sin) = √2，抬高 3.01 dB。
    // 这是「用线性不用等功率」的理由，也是等功率实现必须变红的反例。
    constexpr double freq = 100.0;
    constexpr double amp = 0.9;
    const double sr = 48000.0;
    const auto n = rampLength(sr);
    const auto total = static_cast<std::size_t>(0.4 * sr);
    const auto at = static_cast<std::size_t>(std::llround(0.3 * sr));
    // 相位对齐：正弦的峰正好落在交叉的中点（b = 0.5）上，那里等功率的抬高最大
    const double phase0 = rinne::test::kPi / 2.0 -
                          2.0 * rinne::test::kPi * freq * (static_cast<double>(at) + 0.5 * static_cast<double>(n)) / sr;
    double linearPeak = 0.0;
    double equalPowerPeak = 0.0;
    for (std::size_t i = 0; i < total; ++i)
    {
        const double s = amp * std::sin(2.0 * rinne::test::kPi * freq * static_cast<double>(i) / sr + phase0);
        const double b = i < at ? 0.0 : std::min(1.0, static_cast<double>(i - at) / static_cast<double>(n));
        linearPeak = std::fmax(linearPeak, std::fabs((1.0 - b) * s + b * s));
        const double theta = b * rinne::test::kPi / 2.0;
        equalPowerPeak = std::fmax(equalPowerPeak, std::fabs(std::cos(theta) * s + std::sin(theta) * s));
    }
    CHECK(linearPeak <= amp * (1.0 + 1.0e-12));
    // √2 = 1.41421，抬高 3.01 dB
    CHECK(equalPowerPeak > amp * 1.41);
    CHECK(equalPowerPeak < amp * 1.42);
}

TEST_CASE("the ramp length is exactly round(seconds x sample rate) and never shorter than one sample", "[tone][switch]")
{
    for (const double sr : kRates)
    {
        INFO("sr = " << sr);
        SwitchRamp r;
        r.prepare(sr, kSwitchRampSeconds);
        CHECK(r.lengthSamples() == rampLength(sr));
        CHECK(r.lengthSamples() >= 1);
        SwitchRamp tiny;
        tiny.prepare(sr, 1.0e-9);
        CHECK(tiny.lengthSamples() == 1);
        SwitchRamp bad;
        bad.prepare(0.0, kSwitchRampSeconds);
        CHECK(bad.lengthSamples() == 1);
    }
    CHECK(kSwitchRampSeconds == 0.010);
}

TEST_CASE("setting the same target again does nothing", "[tone][switch]")
{
    SwitchRamp r;
    r.prepare(48000.0, kSwitchRampSeconds);
    r.setTarget(true);
    for (int i = 0; i < 100; ++i)
        r.step();
    const auto p = r.position();
    r.setTarget(true);
    r.setTarget(true);
    CHECK(r.position() == p);
    r.step();
    CHECK(r.position() == p + 1);
}
