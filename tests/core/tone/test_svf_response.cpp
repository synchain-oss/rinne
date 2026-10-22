// SPDX-License-Identifier: GPL-3.0-or-later
//
// 低切与高切的幅频：四个采样率上，截止处 −3.01 dB，通带与阻带都与预畸变二阶 Butterworth 的理论值对得上；
// 截止贴近奈奎斯特时钳在 0.45·sr，系数始终有限。
// 必须变红：Q 不是 1/√2、去掉 tan 预畸变（高频截止会偏）、高低通取错输出、截止不钳，下面都有格子会红。

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <limits>

#include "ToneTestKit.h"
#include "dsp/SvfTpt.h"

using rinne::dsp::CutFilter;
using rinne::dsp::CutKind;
using rinne::dsp::svfCoefficients;
using rinne::dsp::SvfResponse;
using rinne::dsp::SvfTpt;
using rinne::test::butterworthGain;
using rinne::test::lockInGain;
using rinne::test::toDb;

namespace
{

constexpr double kRates[] = {44100.0, 48000.0, 96000.0, 192000.0};

// 经 CutFilter 测：prepare 时参数就在 fc 上，滤波器直接在目标上（没有滑动），测的就是这一级本身。
// 0.25 s 稳定、0.1 s 测量；测试频率都是 10 Hz 的整数倍，0.1 s 里正好整周期
double measureDb(CutKind kind, double fc, double freq, double sr)
{
    CutFilter f(kind);
    f.setCutoff(static_cast<float>(fc));
    f.prepare(sr);
    const auto settle = static_cast<std::int64_t>(0.25 * sr);
    const auto n = static_cast<std::int64_t>(sr / 10.0);
    return toDb(lockInGain([&f](float x) { return f.process(x); }, freq, sr, settle, n));
}

double theoryDb(CutKind kind, double fc, double freq, double sr)
{
    return toDb(butterworthGain(kind == CutKind::LowCut, freq, fc, sr));
}

} // namespace

TEST_CASE("low cut is a 12 dB/oct Butterworth high-pass with -3.01 dB at the cutoff at every rate", "[tone][svf]")
{
    for (const double sr : kRates)
    {
        for (const double fc : {40.0, 200.0, 1000.0, 2000.0})
        {
            INFO("sr = " << sr << ", fc = " << fc);
            const double atCutoff = measureDb(CutKind::LowCut, fc, fc, sr);
            const double pass = measureDb(CutKind::LowCut, fc, 4.0 * fc, sr);
            const double stop = measureDb(CutKind::LowCut, fc, fc / 4.0, sr);
            CHECK(std::fabs(atCutoff + 3.0103) < 0.005);
            // 两个八度以上：理论 −0.017 dB 左右，Butterworth 没有起伏，不会高过 0 dB
            CHECK(pass > -0.03);
            CHECK(pass < 1.0e-6);
            // 两个八度以下：理论 −24 dB 左右
            CHECK(stop < -23.9);
            CHECK(stop > -24.6);
            CHECK(std::fabs(pass - theoryDb(CutKind::LowCut, fc, 4.0 * fc, sr)) < 0.005);
            CHECK(std::fabs(stop - theoryDb(CutKind::LowCut, fc, fc / 4.0, sr)) < 0.005);
        }
    }
}

TEST_CASE("high cut is a 12 dB/oct Butterworth low-pass with -3.01 dB at the cutoff at every rate", "[tone][svf]")
{
    for (const double sr : kRates)
    {
        for (const double fc : {1000.0, 4000.0, 12000.0, 19000.0})
        {
            INFO("sr = " << sr << ", fc = " << fc);
            // 阻带取 4·fc；超出 0.45·sr 时改取 0.49·sr（向下取到 10 Hz 的整数倍），贴近奈奎斯特的零点
            const double stopFreq = 4.0 * fc < 0.45 * sr ? 4.0 * fc : std::floor(0.049 * sr) * 10.0;
            const double atCutoff = measureDb(CutKind::HighCut, fc, fc, sr);
            const double pass = measureDb(CutKind::HighCut, fc, fc / 4.0, sr);
            const double stop = measureDb(CutKind::HighCut, fc, stopFreq, sr);
            const double stopTheory = theoryDb(CutKind::HighCut, fc, stopFreq, sr);
            CHECK(std::fabs(atCutoff + 3.0103) < 0.005);
            CHECK(pass > -0.03);
            CHECK(pass < 1.0e-6);
            CHECK(stop < -12.0);
            CHECK(std::fabs(pass - theoryDb(CutKind::HighCut, fc, fc / 4.0, sr)) < 0.005);
            // 理论值低于 −60 dB 的点只要求足够深：那里输入的 float 量化已经与信号同量级
            if (stopTheory > -60.0)
                CHECK(std::fabs(stop - stopTheory) < 0.01);
            else
                CHECK(stop < -55.0);
        }
    }
}

TEST_CASE("response follows the Butterworth curve across the whole band", "[tone][svf]")
{
    // 48 kHz、两档截止，从 20 Hz 到 20 kHz 每三分之一倍频程一个点（取到 10 Hz 的整数倍）
    const double sr = 48000.0;
    struct Case
    {
        CutKind kind;
        double fc;
    };
    for (const Case c : {Case{CutKind::LowCut, 300.0}, Case{CutKind::HighCut, 6000.0}})
    {
        for (int step = 0; step <= 30; ++step)
        {
            const double freq = std::round(20.0 * std::pow(2.0, step / 3.0) / 10.0) * 10.0;
            const double want = theoryDb(c.kind, c.fc, freq, sr);
            if (want < -60.0)
                continue;
            INFO("kind = " << static_cast<int>(c.kind) << ", f = " << freq);
            CHECK(std::fabs(measureDb(c.kind, c.fc, freq, sr) - want) < 0.01);
        }
    }
}

TEST_CASE("cutoffs near or past Nyquist are clamped to 0.45 sr and stay finite", "[tone][svf]")
{
    for (const double sr : kRates)
    {
        INFO("sr = " << sr);
        const auto top = svfCoefficients(0.45 * sr, sr);
        CHECK(std::isfinite(top.g));
        CHECK(top.g > 6.0);
        CHECK(top.g < 6.5);
        for (const double hz : {0.45 * sr + 1.0, 0.5 * sr, sr, 1.0e12, std::numeric_limits<double>::infinity()})
        {
            const auto c = svfCoefficients(hz, sr);
            CHECK(c.g == top.g);
            CHECK(c.a1 == top.a1);
            CHECK(c.a3 == top.a3);
        }
        // 下限 1 Hz；NaN、0、负数都给下限
        const auto bottom = svfCoefficients(1.0, sr);
        for (const double hz : {0.0, -5.0, std::numeric_limits<double>::quiet_NaN()})
        {
            const auto c = svfCoefficients(hz, sr);
            CHECK(c.g == bottom.g);
            CHECK(c.g > 0.0);
        }
        // 钳住的截止上，稳态正弦仍是有限、有界的
        SvfTpt lp;
        SvfTpt hp;
        bool finite = true;
        double peak = 0.0;
        for (int n = 0; n < 20000; ++n)
        {
            const float x = rinne::test::sine(0.3 * sr, sr, n, 1.0);
            const float a = lp.process(x, top, SvfResponse::LowPass);
            const float b = hp.process(x, top, SvfResponse::HighPass);
            finite = finite && std::isfinite(a) && std::isfinite(b);
            peak = std::fmax(peak, std::fmax(std::fabs(static_cast<double>(a)), std::fabs(static_cast<double>(b))));
        }
        CHECK(finite);
        CHECK(peak < 1.5);
    }
    // 没有采样率时：g = 0，高通直通、低通为 0
    const auto none = svfCoefficients(1000.0, 0.0);
    CHECK(none.g == 0.0);
    SvfTpt t;
    CHECK(t.process(0.25f, none, SvfResponse::HighPass) == 0.25f);
    CHECK(t.process(0.25f, none, SvfResponse::LowPass) == 0.0f);
}

TEST_CASE("high cut above 0.45 sr is limited and reports it; supported rates never limit the low cut", "[tone][svf]")
{
    CutFilter hc(CutKind::HighCut);
    hc.setCutoff(19900.0f);
    hc.prepare(44100.0);
    CHECK(hc.isOn());
    CHECK(hc.cutoffLimited());
    CHECK(hc.targetCutoffHz() == 0.45 * 44100.0);
    CHECK(hc.coefficients().g == svfCoefficients(0.45 * 44100.0, 44100.0).g);
    hc.prepare(48000.0);
    CHECK_FALSE(hc.cutoffLimited());
    CHECK(hc.targetCutoffHz() == static_cast<double>(19900.0f));
    // 关不是截断：20000 在 44.1 kHz 下照样是逐位直通，不报截断
    hc.setCutoff(20000.0f);
    hc.prepare(44100.0);
    CHECK_FALSE(hc.cutoffLimited());
    CHECK(hc.isBypassed());

    for (const double sr : kRates)
    {
        CutFilter lc(CutKind::LowCut);
        lc.setCutoff(2000.0f);
        lc.prepare(sr);
        CHECK_FALSE(lc.cutoffLimited());
        CHECK(lc.targetCutoffHz() == 2000.0);
    }
}

TEST_CASE("out-of-range parameters clamp to the parameter range", "[tone][svf]")
{
    const double sr = 48000.0;
    CutFilter lc(CutKind::LowCut);
    lc.prepare(sr);
    lc.setCutoff(5000.0f);
    CHECK(lc.isOn());
    CHECK(lc.targetCutoffHz() == 2000.0);
    lc.setCutoff(std::numeric_limits<float>::infinity());
    CHECK(lc.targetCutoffHz() == 2000.0);

    CutFilter hc(CutKind::HighCut);
    hc.prepare(sr);
    hc.setCutoff(500.0f);
    CHECK(hc.isOn());
    CHECK(hc.targetCutoffHz() == 1000.0);
    hc.setCutoff(-std::numeric_limits<float>::infinity());
    CHECK(hc.targetCutoffHz() == 1000.0);
}
