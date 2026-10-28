// SPDX-License-Identifier: GPL-3.0-or-later
//
// 环路延迟 D 的毫秒与样本互换（src/input/InputParams.h 里的 delayMsToSamples / delaySamplesToMs）。
// 参数里存毫秒（delay_ms，1…4000 ms，步进 0.001 ms，对数刻度）；界面可以按样本数显示和输入。
//   · 同一个采样率下，样本数 → 毫秒 → 样本数精确还原；
//   · 经过真实的 delay_ms 参数（步进取整、归一化值是 float、APVTS 再读一遍）之后仍还原：44.1 / 48 / 88.2 / 96 / 176.4 /
//     192 kHz 下每个合法的样本数逐个核对，所以 0.001 ms 的步进在这六个采样率下都够用。
//     余量不大：4000 ms 附近对数刻度上 1 个 float 归一化单位约 0.001 ms，往返最远偏出 0.48 个样本（192 kHz）、
//     0.45 个样本（176.4 kHz）。限制往返精度的是 float 归一化值，不是 0.001 ms 的步进；再高的采样率会先在这里出问题。
//   · 采样率变了，参数里的毫秒值不动，样本数按新采样率重算（48 kHz 的 100 ms 是 4800，96 kHz 是 9600）。

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "../../src/input/InputParams.h"
#include "params/ParamsTestProcessor.h"

namespace
{

using Catch::Approx;
using rinne::input::delayMsToSamples;
using rinne::input::delaySamplesToMs;
using rinne::test::params::ParamsTestProcessor;

constexpr double kRates[] = {44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0};
constexpr double kMinDelayMs = 1.0;
constexpr double kMaxDelayMs = 4000.0;
constexpr std::size_t kDelayMsIndex = 4; // 参数表里 delay_ms 的序号

// 合法的样本数范围：D 的下限 1 ms 取上整，上限 4000 ms 取下整
std::int64_t minSamples(double rate)
{
    return static_cast<std::int64_t>(std::ceil(kMinDelayMs * rate / 1000.0));
}

std::int64_t maxSamples(double rate)
{
    return static_cast<std::int64_t>(std::floor(kMaxDelayMs * rate / 1000.0));
}

// 界面设一个毫秒值到 delay_ms 参数里再读出来的全程：明文 → float 归一化 → 参数存的值（步进取整）→ APVTS 的原子量
// 又经归一化值换回来一次（ParameterAdapter 读 getValue() 再 convertFrom0to1）
float throughParameter(const juce::RangedAudioParameter& param, double ms)
{
    const float stored = param.convertFrom0to1(param.convertTo0to1(static_cast<float>(ms)));
    return param.convertFrom0to1(param.convertTo0to1(stored));
}

} // namespace

TEST_CASE("samples to milliseconds and back is exact at every sample rate", "[params][input-params][dtime]")
{
    for (const double rate : kRates)
    {
        INFO("采样率 " << rate);
        std::int64_t mismatches = 0;
        std::int64_t firstBad = 0;
        for (std::int64_t n = 1; n <= maxSamples(rate); ++n)
        {
            if (delayMsToSamples(delaySamplesToMs(n, rate), rate) != n)
            {
                if (mismatches == 0)
                {
                    firstBad = n;
                }
                ++mismatches;
            }
        }
        INFO("第一个对不上的样本数 " << firstBad);
        CHECK(mismatches == 0);
    }
}

TEST_CASE("a delay set in samples survives the delay_ms parameter at every sample rate",
          "[params][input-params][dtime][step]")
{
    const auto param = rinne::input::makeInputParameter(kDelayMsIndex);
    REQUIRE(param != nullptr);
    REQUIRE(param->getParameterID() == juce::String("delay_ms"));
    REQUIRE(param->getNormalisableRange().start == static_cast<float>(kMinDelayMs));
    REQUIRE(param->getNormalisableRange().end == static_cast<float>(kMaxDelayMs));
    REQUIRE(param->getNormalisableRange().interval == 0.001f);

    for (const double rate : kRates)
    {
        INFO("采样率 " << rate);
        std::int64_t mismatches = 0;
        std::int64_t firstBad = 0;
        std::int64_t checked = 0;
        double worst = 0.0; // 存回参数再读出来的毫秒值换算成样本数，离原来的样本数最远差多少个样本
        for (std::int64_t n = minSamples(rate); n <= maxSamples(rate); ++n, ++checked)
        {
            const float stored = throughParameter(*param, delaySamplesToMs(n, rate));
            worst = std::max(worst, std::fabs(static_cast<double>(stored) * rate / 1000.0 - static_cast<double>(n)));
            if (delayMsToSamples(static_cast<double>(stored), rate) != n)
            {
                if (mismatches == 0)
                {
                    firstBad = n;
                }
                ++mismatches;
            }
        }
        INFO("核对了 " << checked << " 个样本数，第一个对不上的 " << firstBad << "，最远偏差 " << worst << " 个样本");
        CHECK(checked > 0);
        CHECK(mismatches == 0);
        CHECK(worst < 0.5);
    }
}

TEST_CASE("the parameter tree reads back the same delay as the stepwise emulation", "[params][input-params][dtime]")
{
    // 上面的逐格核对用参数对象自己的换算模拟了「界面设值 → APVTS 原子量」；这里用真实的参数树抽样证明模拟与实际一致
    ParamsTestProcessor processor;
    rinne::input::InputParams params{processor};
    auto* param = dynamic_cast<juce::RangedAudioParameter*>(params.tree().getParameter("delay_ms"));
    REQUIRE(param != nullptr);
    const std::atomic<float>* raw = params.tree().getRawParameterValue("delay_ms");
    REQUIRE(raw != nullptr);

    for (const double rate : kRates)
    {
        INFO("采样率 " << rate);
        const std::int64_t lo = minSamples(rate);
        const std::int64_t hi = maxSamples(rate);
        const std::int64_t span = hi - lo;
        // 两端和中间分布的 64 个样本数，再加几个整数毫秒
        for (int i = 0; i <= 64; ++i)
        {
            const std::int64_t n = lo + span * i / 64;
            const double ms = delaySamplesToMs(n, rate);
            param->setValueNotifyingHost(param->convertTo0to1(static_cast<float>(ms)));
            INFO("样本数 " << n);
            CHECK(raw->load() == throughParameter(*param, ms));
            CHECK(delayMsToSamples(static_cast<double>(raw->load()), rate) == n);
            rinne::engine::EngineParams snapshot;
            params.snapshot(snapshot);
            CHECK(snapshot.delayMs == raw->load());
        }
    }
}

TEST_CASE("changing the sample rate keeps the milliseconds and recounts the samples",
          "[params][input-params][dtime][sr]")
{
    ParamsTestProcessor processor;
    rinne::input::InputParams params{processor};
    auto* param = dynamic_cast<juce::RangedAudioParameter*>(params.tree().getParameter("delay_ms"));
    REQUIRE(param != nullptr);
    param->setValueNotifyingHost(param->convertTo0to1(100.0f));

    rinne::engine::EngineParams before;
    params.snapshot(before);
    CHECK(before.delayMs == Approx(100.0).margin(0.001));
    const double ms = static_cast<double>(before.delayMs);
    CHECK(delayMsToSamples(ms, 48000.0) == 4800);

    // 宿主把采样率换成 96 kHz：参数层不看采样率，快照里的毫秒值不动，样本数翻倍
    processor.setRateAndBufferSizeDetails(96000.0, 512);
    REQUIRE(processor.getSampleRate() == 96000.0);
    rinne::engine::EngineParams after;
    params.snapshot(after);
    CHECK(after.delayMs == before.delayMs);
    CHECK(delayMsToSamples(static_cast<double>(after.delayMs), 96000.0) == 9600);
    CHECK(delayMsToSamples(ms, 96000.0) == 2 * delayMsToSamples(ms, 48000.0));

    // 其余几个采样率下 100 ms 的样本数
    CHECK(delayMsToSamples(ms, 44100.0) == 4410);
    CHECK(delayMsToSamples(ms, 88200.0) == 8820);
    CHECK(delayMsToSamples(ms, 176400.0) == 17640);
    CHECK(delayMsToSamples(ms, 192000.0) == 19200);
    // 毫秒值落不到整数样本时四舍五入：1 ms 在 44.1 kHz 是 44.1 个样本
    CHECK(delayMsToSamples(1.0, 44100.0) == 44);
    CHECK(delayMsToSamples(1.0004, 44100.0) == 44);
    CHECK(delayMsToSamples(1.0114, 44100.0) == 45); // 44.605 个样本
    CHECK(delaySamplesToMs(48, 48000.0) == Approx(1.0).margin(1.0e-12));
}

TEST_CASE("degenerate inputs convert to zero instead of garbage", "[params][input-params][dtime]")
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    CHECK(delayMsToSamples(100.0, 0.0) == 0);
    CHECK(delayMsToSamples(100.0, -48000.0) == 0);
    CHECK(delayMsToSamples(100.0, nan) == 0);
    CHECK(delayMsToSamples(0.0, 48000.0) == 0);
    CHECK(delayMsToSamples(-5.0, 48000.0) == 0);
    CHECK(delayMsToSamples(nan, 48000.0) == 0);
    CHECK(delayMsToSamples(inf, 48000.0) == 0);
    CHECK(delayMsToSamples(-inf, 48000.0) == 0);
    CHECK(delayMsToSamples(100.0, inf) == 0);
    // 有限但大得离谱：夹在 10^15，不溢出
    CHECK(delayMsToSamples(1.0e300, 48000.0) == static_cast<std::int64_t>(1.0e15));
    CHECK(delaySamplesToMs(0, 48000.0) == 0.0);
    CHECK(delaySamplesToMs(-10, 48000.0) == 0.0);
    CHECK(delaySamplesToMs(4800, 0.0) == 0.0);
    CHECK(delaySamplesToMs(4800, nan) == 0.0);
}
