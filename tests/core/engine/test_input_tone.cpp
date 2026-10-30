// SPDX-License-Identifier: GPL-3.0-or-later
//
// InputEngine 接上音色级（③ 低切 → ④ 高切 → ⑤ 极性，夹在安全链前后两段之间）之后：
//   默认参数（以及越界的「关」值与 NaN）下与不接音色级的参照逐位相同（立体声与 mono）；
//   开着时与按 InputEngine.h 写明的次序与复位时机另算的参照逐位相同（参数随块变、起播沿、换写方、mono）；
//   低切 / 高切的量级（只看量级，幅频曲线本身在 tests/core/tone/test_svf_response.cpp）；
//   极性：稳态逐样本反号；切换 10 ms 交叉、无咔嗒，直接翻符号的反例超出判据；
//   音色级开着时，参数恒定的输出与宿主怎么切块逐位无关；反馈 0、mix 0、loop_on 关与旁路时干声照旧逐位直通；
//   环内 NaN（Output 清洗过的、环里直接写进的）照旧不跳闸；音色级交出的非有限值在 ⑥ 跳闸并锁存；
//   高切被采样率截断的提示位。
//
// 标 ⛔ 的是删除式验证点：把对应的实现改坏，本用例必须变红。
// 开环：中间链（middle）把送进 Output 的信号整个换成测试信号，环里就只有它，回送不再绕回来；Input 的干声为 0、
// mix 100 % 时 Input 的输出就是回送本身。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

#include "EngineTestKit.h"
#include "core/tone/ToneTestKit.h"
#include "dsp/Sanitize.h"
#include "dsp/SvfTpt.h"
#include "dsp/SwitchRamp.h"
#include "engine/InputEngine.h"
#include "engine/SafetyEvents.h"
#include "ipc/LoopBinding.h"

namespace
{

using namespace rinne::engine;
using rinne::test::ClosedLoop;
using rinne::test::firstBitDiff;
using rinne::test::kPi;
using rinne::test::sineStepBound;

constexpr double kSr = 48000.0;
constexpr double kRates[] = {44100.0, 48000.0, 96000.0, 192000.0};

EngineParams loopParams(float feedbackPercent, float mixPercent, float delayMs)
{
    EngineParams p;
    p.feedbackPercent = feedbackPercent;
    p.mixPercent = mixPercent;
    p.delayMs = delayMs;
    return p;
}

EngineParams withTone(EngineParams p, float lowCutHz, float highCutHz, bool polarity)
{
    p.lowCutHz = lowCutHz;
    p.highCutHz = highCutHz;
    p.polarityInvert = polarity;
    return p;
}

float sineAt(std::int64_t t, double hz, double sr, double amplitude)
{
    return static_cast<float>(amplitude * std::sin(2.0 * kPi * hz * static_cast<double>(t) / sr));
}

// 特殊值：−0.0、带载荷的 quiet NaN、signaling NaN（载荷随 t 变，位型在运行时才定）、负号 NaN、denormal、±Inf
float special(std::int64_t t, int channel)
{
    switch ((t + channel * 3) % 9)
    {
    case 0:
        return -0.0f;
    case 1:
        return rinne::test::fromBits(0x7fc12345u);
    case 2:
        return rinne::test::fromBits(0x7f800000u | (1u + (static_cast<std::uint32_t>(t) & 0xffu)));
    case 3:
        return rinne::test::fromBits(0xffc00abcu);
    case 4:
        return 1.0e-40f;
    case 5:
        return std::numeric_limits<float>::infinity();
    case 6:
        return -0.25f;
    default:
        return sineAt(t, 220.0, kSr, 0.3);
    }
}

// 开环：送进 Output 的信号换成 feed，环里只有它
void openLoop(ClosedLoop& loop, std::function<float(std::int64_t, int)> feed)
{
    loop.middle = [feed](std::int64_t at, float* left, float* right, int n) {
        for (int i = 0; i < n; ++i)
        {
            left[i] = feed(at + i, 0);
            right[i] = feed(at + i, 1);
        }
    };
}

// [from, from + count) 上频率 f 的稳态幅度：整数个周期上与正余弦求相关，与相位无关
double amplitudeAt(const std::vector<float>& y, double f, double sr, std::size_t from, std::size_t count)
{
    double s = 0.0;
    double c = 0.0;
    for (std::size_t i = from; i < from + count; ++i)
    {
        const double phase = 2.0 * kPi * f * static_cast<double>(i) / sr;
        s += static_cast<double>(y[i]) * std::sin(phase);
        c += static_cast<double>(y[i]) * std::cos(phase);
    }
    return 2.0 / static_cast<double>(count) * std::sqrt(s * s + c * c);
}

double peakOf(const std::vector<float>& v, std::size_t from, std::size_t to)
{
    double worst = 0.0;
    for (std::size_t i = from; i < std::min(to, v.size()); ++i)
        worst = std::fmax(worst, std::fabs(static_cast<double>(v[i])));
    return worst;
}

double maxStepOf(const std::vector<float>& v, std::size_t from, std::size_t to)
{
    double worst = 0.0;
    for (std::size_t i = from + 1; i < std::min(to, v.size()); ++i)
        worst = std::fmax(worst, std::fabs(static_cast<double>(v[i]) - v[i - 1]));
    return worst;
}

// 从 from 起两串逐位相同
bool sameBitsFrom(const std::vector<float>& a, const std::vector<float>& b, std::size_t from)
{
    if (a.size() != b.size() || from > a.size())
        return false;
    return std::memcmp(a.data() + from, b.data() + from, (a.size() - from) * sizeof(float)) == 0;
}

bool isSignalingNan(float x)
{
    const std::uint32_t b = rinne::test::floatBits(x);
    return (b & 0x7fc00000u) == 0x7f800000u && (b & 0x003fffffu) != 0u;
}

// 参数随块变的一段闭环（与 test_input_wiring.cpp 的接线用例同一套走法：反馈、mix（g_dry 不为 1）、限制器开关、上限、
// 旁路、起播沿、换写方），tone 给出每块的音色级参数
void runVaried(ClosedLoop& loop, const std::function<EngineParams(std::int64_t at, EngineParams p)>& tone)
{
    loop.source = [](std::int64_t t, int channel) {
        return static_cast<float>(0.35 *
                                  std::cos(2.0 * kPi * (channel == 0 ? 110.0 : 170.0) * static_cast<double>(t) / kSr)) +
               (t % 1201 == 0 ? 0.3f : 0.0f);
    };
    const int lengths[] = {512, 37, 256, 400};
    TransportInfo transport;
    for (int k = 0; loop.at < 6 * 48000; ++k)
    {
        const std::int64_t at = loop.at;
        EngineParams p = loopParams(at < 144000 ? 90.0f : 60.0f, 75.0f, 20.0f);
        p.limiterOn = (at / 48000) % 2 == 0;
        p.ceilingDb = at < 96000 ? -6.0f : -12.0f;
        loop.inputBypassed = at >= 150000 && at < 160000;
        transport.isPlaying = at >= 200000;
        if (at >= 240000 && !loop.writerIsSecond)
            loop.switchWriter();
        loop.block(tone(at, p), lengths[k % 4], transport);
    }
}

} // namespace

TEST_CASE("Input 音色级：默认参数（含越界的「关」值与 NaN）下与不接音色级的参照逐位相同，立体声与 mono",
          "[engine][input][tone]")
{
    // 证据的选法：参照（EngineTestKit.h 的 InputReference，不开 enableTone）就是「不接音色级」的接线，引擎与它逐位
    // 比较；现有的引擎、环路与宿主用例一条不改也照样全绿，是另一半证据。
    // 低切 ≤ 20 Hz、高切 ≥ 20000 Hz、NaN 都按关：三级都逐位直通（−0.0 与 NaN 载荷原样）。
    const float nan = std::numeric_limits<float>::quiet_NaN();
    struct Off
    {
        float low;
        float high;
    };
    for (const Off off : {Off{20.0f, 20000.0f}, Off{5.0f, 30000.0f}, Off{-1.0e9f, 1.0e9f}, Off{nan, nan}})
    {
        for (const int channels : {2, 1})
        {
            INFO("low = " << off.low << ", high = " << off.high << ", channels = " << channels);
            ClosedLoop loop(kSr, 512);
            loop.inputChannels = channels;
            runVaried(loop, [&](std::int64_t, EngineParams p) { return withTone(p, off.low, off.high, false); });
            CHECK(firstBitDiff(loop.outLeft, loop.reference.wantLeft) == -1);
            if (channels == 2)
                CHECK(firstBitDiff(loop.outRight, loop.reference.wantRight) == -1);
            CHECK(firstBitDiff(loop.outLeft, loop.dryLeft) != -1); // 回送确实在响
            CHECK(loop.input.loopStatus().resyncCount >= 2u); // 起播与换写方
        }
    }
}

TEST_CASE("Input 音色级接线：前段 → 低切 → 高切 → 极性 → 后段，开着时与按文档次序另算的参照逐位相同",
          "[engine][input][tone]")
{
    // ⛔ 删除式验证点：音色级不调用 → 与参照对不上，本用例变红；起播沿的复位点上不复位音色级 → 起播之后对不上，变红；
    //    极性不交叉、直接翻符号 → 切换那几块对不上，变红。
    // 参数随块变：低切在关、120 Hz、400 Hz 之间换，高切在关、3 kHz、8 kHz 之间换，极性每 0.25 s 翻一次，
    // 加上反馈、mix、限制器、旁路、起播沿与换写方。mono 时音色级在取平均之前、每个声道各走一遍。
    const auto tone = [](std::int64_t at, EngineParams p) {
        const float lows[] = {20.0f, 120.0f, 400.0f, 120.0f};
        const float highs[] = {3000.0f, 20000.0f, 8000.0f};
        const auto slot = static_cast<std::size_t>(at / 30000);
        return withTone(p, lows[slot % 4], highs[slot % 3], (at / 12000) % 2 == 1);
    };
    for (const int channels : {2, 1})
    {
        INFO("channels = " << channels);
        ClosedLoop loop(kSr, 512);
        loop.inputChannels = channels;
        loop.reference.enableTone();
        runVaried(loop, tone);
        CHECK(firstBitDiff(loop.outLeft, loop.reference.wantLeft) == -1);
        if (channels == 2)
            CHECK(firstBitDiff(loop.outRight, loop.reference.wantRight) == -1);
        CHECK(peakOf(loop.reference.retLeft, 0, loop.reference.retLeft.size()) > 0.05); // 回送确实在响
        CHECK(loop.input.loopStatus().resyncCount >= 2u);

        // 同一段走法、不开音色级的参照对不上：音色级确实改变了输出（上面的逐位相同不是两边都没接）
        ClosedLoop plain(kSr, 512);
        plain.inputChannels = channels;
        runVaried(plain, tone);
        CHECK(firstBitDiff(plain.outLeft, plain.reference.wantLeft) != -1);
    }
}

TEST_CASE("Input 低切与高切的量级：低切 200 Hz 压低 50 Hz、2 kHz 基本不变；高切 2 kHz 压低 10 kHz、200 Hz 基本不变",
          "[engine][input][tone]")
{
    // ⛔ 删除式验证点：音色级不调用 → 衰减消失，本用例变红。
    // 开环，环里是幅度 0.25 的正弦；反馈 80 %、mix 100 %（g_dry = 0、g_ret = 1），Input 的输出就是回送。
    // 量 0.6–1.0 s 的稳态幅度（500 ms 淡入早已走完），与同一频率上音色级关着的幅度相比。
    // 理论值（预畸变二阶 Butterworth）：50 Hz 过 200 Hz 低切约 −24 dB，10 kHz 过 2 kHz 高切约 −30 dB；
    // 2 kHz 过 200 Hz 低切、200 Hz 过 2 kHz 高切都不到 0.01 dB。这里只断言量级。
    const auto amplitude = [](double hz, float lowCut, float highCut) {
        ClosedLoop loop(kSr, 480);
        openLoop(loop, [hz](std::int64_t t, int) { return sineAt(t, hz, kSr, 0.25); });
        loop.run(withTone(loopParams(80.0f, 100.0f, 10.0f), lowCut, highCut, false), 480, 48000);
        REQUIRE(loop.state() != SafetyState::Tripped);
        return amplitudeAt(loop.outLeft, hz, kSr, 28800, 19200);
    };
    const double plain50 = amplitude(50.0, 20.0f, 20000.0f);
    const double plain2k = amplitude(2000.0, 20.0f, 20000.0f);
    const double plain10k = amplitude(10000.0, 20.0f, 20000.0f);
    const double plain200 = amplitude(200.0, 20.0f, 20000.0f);
    REQUIRE(plain50 > 0.19); // 0.25 × 0.8，DC 阻断在 50 Hz 只差约 0.04 dB
    REQUIRE(plain10k > 0.19);

    const double low50 = amplitude(50.0, 200.0f, 20000.0f) / plain50;
    const double low2k = amplitude(2000.0, 200.0f, 20000.0f) / plain2k;
    const double high10k = amplitude(10000.0, 20.0f, 2000.0f) / plain10k;
    const double high200 = amplitude(200.0, 20.0f, 2000.0f) / plain200;
    INFO("low cut 200 Hz: 50 Hz " << low50 << ", 2 kHz " << low2k << "; high cut 2 kHz: 10 kHz " << high10k
                                  << ", 200 Hz " << high200);
    CHECK(low50 < 0.1);
    CHECK(low2k > 0.97);
    CHECK(low2k < 1.03);
    CHECK(high10k < 0.1);
    CHECK(high200 > 0.97);
    CHECK(high200 < 1.03);
}

TEST_CASE("Input 极性：开着时回送与关着时逐样本反号；切换走 10 ms 交叉、相邻样本无咔嗒，直接翻符号的反例超出判据",
          "[engine][input][tone][click]")
{
    // ⛔ 删除式验证点：极性不交叉、直接翻符号 → 相邻样本差超出判据，本用例变红；极性不接 → 「逐样本反号」变红。
    // 开环，环里是 100 Hz、幅度 0.25 的正弦；反馈 80 %、mix 100 %，回送幅度约 0.2（远低于限制器上限与软削波拐点）。
    // 极性在块首生效，事件所在的样本另起一块（块长不超过 64）：峰上开、交叉走到一半关、谷底上开、之后每 3 ms 来回切
    // 一串。判据与 tests/core/tone/test_limiter_switch_click.cpp 相同：相邻样本差 ≤ 输入正弦相邻差上界的 1.5 倍。
    // 判据的余量：交叉中 (1 − 2b)·x 的斜率不超过 √((aω/sr)² + (2a/n)²)，在四个采样率上都低于 1.5 倍的正弦上界。
    static constexpr double kFreq = 100.0;
    static constexpr double kAmp = 0.25;
    static constexpr double kOutAmp = 0.2;
    static constexpr double kCriterion = 1.5;
    static constexpr std::int64_t kMaxBlock = 64;
    for (const double sr : kRates)
    {
        INFO("sr = " << sr);
        const auto period = sr / kFreq;
        const auto phaseSample = [&](int k, double quarter) {
            return static_cast<std::int64_t>(std::llround(period * (static_cast<double>(k) + quarter)));
        };
        const std::int64_t total = std::llround(0.9 * sr);
        struct Toggle
        {
            std::int64_t at;
            bool on;
        };
        const std::int64_t onAtPeak = phaseSample(60, 0.25);
        const std::int64_t backOff = onAtPeak + std::llround(0.005 * sr); // 10 ms 交叉走到一半
        const std::int64_t onAtTrough = phaseSample(66, 0.75);
        const std::int64_t burst = phaseSample(75, 0.25);
        const std::int64_t every = std::llround(0.003 * sr);
        std::vector<Toggle> toggles = {{onAtPeak, true}, {backOff, false}, {onAtTrough, true}};
        for (int i = 0; i < 10; ++i)
            toggles.push_back({burst + i * every, i % 2 != 0});

        const auto run = [&](bool startOn, const std::vector<Toggle>& events) {
            ClosedLoop loop(sr, static_cast<int>(kMaxBlock));
            openLoop(loop, [sr](std::int64_t t, int) { return sineAt(t, kFreq, sr, kAmp); });
            EngineParams p = loopParams(80.0f, 100.0f, 10.0f);
            p.polarityInvert = startOn;
            std::size_t next = 0;
            while (loop.at < total)
            {
                while (next < events.size() && events[next].at <= loop.at)
                    p.polarityInvert = events[next++].on;
                std::int64_t end = std::min(loop.at + kMaxBlock, total);
                if (next < events.size() && events[next].at < end)
                    end = events[next].at;
                loop.block(p, static_cast<int>(end - loop.at));
            }
            REQUIRE(loop.state() != SafetyState::Tripped);
            return loop.outLeft;
        };

        const std::vector<float> off = run(false, {});
        const std::vector<float> on = run(true, {});
        // 稳态（其实是从头起：prepare 之后的第一块就落定在参数上）逐样本反号
        bool negated = true;
        for (std::size_t i = 0; i < off.size(); ++i)
            negated = negated && on[i] == -off[i];
        CHECK(negated);
        const auto from = static_cast<std::size_t>(std::llround(0.6 * sr)); // 淡入早已走完
        REQUIRE(peakOf(off, from, off.size()) > 0.19);

        const std::vector<float> switched = run(false, toggles);
        const double bound = kCriterion * sineStepBound(kOutAmp, kFreq, sr);
        const double worst = maxStepOf(switched, from, switched.size());
        INFO("worst step = " << worst << ", bound = " << bound);
        CHECK(worst < bound);
        // 切换确实生效了：开之前与常关的相同，谷底上开之后 10 ms 起到下一串之前与常开的相同
        bool beforeLikeOff = true;
        for (std::size_t i = from; i < static_cast<std::size_t>(onAtPeak); ++i)
            beforeLikeOff = beforeLikeOff && switched[i] == off[i];
        bool afterLikeOn = true;
        const auto settled = static_cast<std::size_t>(onAtTrough + std::llround(0.010 * sr));
        for (std::size_t i = settled; i < static_cast<std::size_t>(burst); ++i)
            afterLikeOn = afterLikeOn && switched[i] == on[i];
        CHECK(beforeLikeOff);
        CHECK(afterLikeOn);

        // 必须变红：直接翻符号。常关与常开两条在峰上直接拼接，相邻样本差远超判据
        std::vector<float> hard(off);
        std::copy(on.begin() + onAtPeak, on.end(), hard.begin() + onAtPeak);
        INFO("hard flip = " << maxStepOf(hard, from, hard.size()));
        CHECK(maxStepOf(hard, from, hard.size()) > 3.0 * bound);
    }
}

TEST_CASE("Input 音色级开着：参数恒定时输出与宿主怎么切块逐位无关（含跨 8192 与 32768 的块）", "[engine][input][tone]")
{
    // ⛔ 删除式验证点：音色级不调用 → 与开了音色级的参照对不上，本用例变红。
    // 参数恒定、prepare 之后第一块就落定，所以这里没有平滑与交叉在走；参数随块变时的逐位一致由上面的接线用例覆盖。
    // 闭环（回送绕回来），D = 1000 ms；低切 150 Hz、高切 6 kHz、极性开，三种切法与开了音色级的参照逐位相同。
    const EngineParams p = withTone(loopParams(80.0f, 50.0f, 1000.0f), 150.0f, 6000.0f, true);
    constexpr std::int64_t kTotal = 4 * 48000;
    const auto source = [](std::int64_t t, int channel) {
        return sineAt(t, channel == 0 ? 220.0 : 330.0, kSr, 0.3) + (t % 997 == 0 ? 0.2f : 0.0f);
    };
    const auto runWith = [&](const std::vector<int>& lengths) {
        ClosedLoop loop(kSr, 32768);
        loop.reference.enableTone();
        loop.source = source;
        for (std::size_t k = 0; loop.at < kTotal; ++k)
        {
            const std::int64_t left = kTotal - loop.at;
            const int n = static_cast<int>(std::min<std::int64_t>(lengths[k % lengths.size()], left));
            loop.block(p, n);
        }
        CHECK(firstBitDiff(loop.outLeft, loop.reference.wantLeft) == -1);
        CHECK(firstBitDiff(loop.outRight, loop.reference.wantRight) == -1);
        CHECK(firstBitDiff(loop.outLeft, loop.dryLeft) != -1);
        return loop.outLeft;
    };
    const auto a = runWith({64});
    const auto b = runWith({32768});
    const auto c = runWith({1, 7, 300, 4096, 8193, 32768, 513, 2048});
    REQUIRE(a.size() == static_cast<std::size_t>(kTotal));
    CHECK(firstBitDiff(a, b) == -1);
    CHECK(firstBitDiff(a, c) == -1);
}

TEST_CASE("Input 音色级开着：反馈 0、mix 0、loop_on 关与 Input 旁路 10 ms 之后，干声照旧逐位直通",
          "[engine][input][tone]")
{
    // 回送增益为 0 时音色级照样在跑（状态、平滑、交叉都按样本走），但 ⑥ 乘出的是 ±0，宿主缓冲一位不碰。
    const auto toned = [](EngineParams p) { return withTone(p, 300.0f, 5000.0f, true); };
    SECTION("反馈 0")
    {
        ClosedLoop loop(kSr, 256);
        loop.reference.enableTone();
        loop.source = special;
        loop.run(toned(loopParams(0.0f, 25.0f, 10.0f)), 256, 48000);
        REQUIRE(isSignalingNan(loop.dryLeft[2]));
        CHECK(isSignalingNan(loop.outLeft[2]));
        CHECK(firstBitDiff(loop.outLeft, loop.dryLeft) == -1);
        CHECK(firstBitDiff(loop.outRight, loop.dryRight) == -1);
    }
    SECTION("mix 0")
    {
        ClosedLoop loop(kSr, 256);
        loop.source = special;
        loop.run(toned(loopParams(100.0f, 0.0f, 10.0f)), 256, 48000);
        CHECK(firstBitDiff(loop.outLeft, loop.dryLeft) == -1);
        CHECK(firstBitDiff(loop.outRight, loop.dryRight) == -1);
    }
    SECTION("loop_on 关 / Input 旁路：1 块 + 10 ms 之后")
    {
        for (const bool viaBypass : {false, true})
        {
            ClosedLoop loop(kSr, 256);
            loop.reference.enableTone();
            static constexpr std::int64_t kSwitch = 48000;
            loop.source = [](std::int64_t t, int channel) {
                return t < kSwitch ? sineAt(t, 100.0 + 50.0 * channel, kSr, 0.3) : special(t, channel);
            };
            EngineParams p = toned(loopParams(80.0f, 50.0f, 10.0f));
            loop.run(p, 256, kSwitch);
            CHECK(firstBitDiff(loop.outLeft, loop.dryLeft) != -1);
            if (viaBypass)
                loop.inputBypassed = true;
            else
                p.loopOn = false;
            loop.run(p, 256, 48000);
            const auto from = static_cast<std::size_t>(kSwitch + 256 + 480);
            CHECK(sameBitsFrom(loop.outLeft, loop.dryLeft, from));
            CHECK(sameBitsFrom(loop.outRight, loop.dryRight, from));
            CHECK(firstBitDiff(loop.outLeft, loop.reference.wantLeft) == -1);
            CHECK(firstBitDiff(loop.outRight, loop.reference.wantRight) == -1);
        }
    }
}

TEST_CASE("Input 音色级开着：环内 NaN（Output 清洗过的）只点琥珀灯，环里直接写进的 NaN 在 ① 置 0，都不跳闸",
          "[engine][input][tone][nan]")
{
    // 音色级在 ① 之后：环里来的非有限值在进音色级之前就清成 0，不当成 Input 内部的非有限值。
    SECTION("环内插件吐 NaN，Output 写环前清洗")
    {
        ClosedLoop loop(kSr, 480);
        loop.reference.enableTone();
        loop.source = [](std::int64_t t, int) { return sineAt(t, 100.0, kSr, 0.3); };
        loop.middle = [](std::int64_t at, float* left, float*, int n) {
            for (int i = 0; i < n; ++i)
            {
                const std::int64_t t = at + i;
                if (t >= 24000 && t <= 28800 && t % 480 == 0)
                    left[i] = std::numeric_limits<float>::quiet_NaN();
            }
        };
        const EngineParams p = withTone(loopParams(50.0f, 50.0f, 50.0f), 200.0f, 4000.0f, true);
        loop.run(p, 480, 28800 + 960);
        const SafetyDiag diag = loop.input.safetyDiag();
        CHECK(diag.loopNanLamp);
        CHECK(diag.nonfiniteRing > 0u);
        CHECK(diag.trips == 0u);
        CHECK(loop.state() != SafetyState::Tripped);
        CHECK(firstBitDiff(loop.outLeft, loop.reference.wantLeft) == -1);
    }
    SECTION("环里直接写进 NaN 与 −Inf（绕过 Output 的清洗）")
    {
        auto ring = std::make_unique<rinne::ipc::RingSegment>();
        rinne::ipc::LoopRing::initGeometry(*ring);
        rinne::ipc::LoopRing::publishWriter(*ring, 1, 48000, 2);
        InputEngine input;
        input.prepare(kSr, 256);
        const EngineParams p = withTone(loopParams(100.0f, 50.0f, 10.0f), 200.0f, 4000.0f, true);
        EngineContext ctx;
        ctx.ring = ring.get();
        ctx.ringGeneration = 1;
        for (int k = 0; k < 200; ++k)
        {
            std::vector<float> left(256, 0.1f);
            std::vector<float> right(256, -0.1f);
            float* channels[2] = {left.data(), right.data()};
            AudioView view;
            view.channels = channels;
            view.numChannels = 2;
            view.length = 256;
            input.process(view, p, TransportInfo{}, false, ctx);
            for (const float v : left)
                REQUIRE(rinne::dsp::isFiniteSample(v));
            rinne::ipc::LoopRing::write(*ring, 256, [](std::uint32_t, std::uint64_t pos, float& l, float& r) {
                l = pos % 5u == 0u ? std::numeric_limits<float>::quiet_NaN() : 0.3f;
                r = pos % 7u == 0u ? -std::numeric_limits<float>::infinity() : -0.3f;
            });
        }
        CHECK(PackedSafety::state(input.meters().safety.load()) != SafetyState::Tripped);
        CHECK(input.safetyDiag().trips == 0u);
        CHECK(input.meters().returnNonFinite.load() > 0u);
    }
}

TEST_CASE("③④⑤ 不清非有限值：按引擎的次序组装，音色级交出的非有限值在 ⑥ 跳闸（TRIPPED(nan)）并锁存",
          "[engine][input][tone][nan]")
{
    // 引擎的公开接口注入不了这一条：音色级的输入已经 ① 清洗过（有限、|x| ≤ 16），两个 SVF 稳定、极性交叉是凸组合，
    // 从任何宿主可给的输入都算不出非有限值。所以分两层给证据：
    //   引擎这一层：上面的接线用例逐位证明引擎就是「前段 → 低切 → 高切 → 极性 → 后段」，两段之间没有别的运算；
    //   这里：同样的几级、同样的设定（开着、交叉进行中、关着），在 ① 之后注入非有限值，断言三级之后它仍是非有限值、
    //   ⑥ 跳闸原因是 nan、回送归零，之后一直锁存（不设自动恢复）。三级里任何一级把非有限值清掉，「三级都没清掉它」与
    //   「跳闸」都会变红。
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    enum class Where
    {
        LowCutRunning,
        HighCutCrossing,
        PolarityCrossing,
        AllOff,
    };
    for (const Where where : {Where::LowCutRunning, Where::HighCutCrossing, Where::PolarityCrossing, Where::AllOff})
    {
        for (const float bad : {nan, inf, -inf})
        {
            INFO("where = " << static_cast<int>(where) << ", bad = " << bad);
            SafetyChain chain;
            chain.prepare(kSr);
            rinne::dsp::CutFilter lowCut(rinne::dsp::CutKind::LowCut);
            rinne::dsp::CutFilter highCut(rinne::dsp::CutKind::HighCut);
            rinne::dsp::SwitchRamp polarity;
            lowCut.prepare(kSr);
            highCut.prepare(kSr);
            polarity.prepare(kSr, rinne::dsp::kSwitchRampSeconds);
            SafetyControls c;
            c.paired = true;
            c.feedback = 1.0f;
            Meters meters;
            std::int64_t n = 0;
            const auto run = [&](int samples, bool inject) {
                chain.beginBlock(c);
                bool allZero = true;
                for (int i = 0; i < samples; ++i, ++n)
                {
                    float l = sineAt(n, 300.0, kSr, 0.3);
                    float r = l;
                    chain.processFront(l, r, true);
                    if (inject && i == 0)
                        l = bad; // 音色级的输入里出现非有限值：看它能不能原样过三级
                    lowCut.process(l, r);
                    highCut.process(l, r);
                    polarity.step();
                    l = polarity.mix(l, -l);
                    r = polarity.mix(r, -r);
                    if (inject && i == 0)
                        CHECK_FALSE(rinne::dsp::isFiniteSample(l)); // 三级都没清掉它
                    chain.processBack(l, r, 0.0f, 0.0f);
                    allZero = allZero && l == 0.0f && r == 0.0f;
                }
                chain.publish(meters);
                return allZero;
            };
            // 跑热：开着的级先走完平滑与交叉，0.6 s 之后 500 ms 淡入也走完
            lowCut.setCutoff(where == Where::LowCutRunning ? 200.0f : 20.0f);
            highCut.setCutoff(20000.0f);
            polarity.setTarget(false);
            for (int k = 0; k < 60; ++k)
                run(480, false);
            REQUIRE(chain.machine().state() == SafetyState::Run);
            // 注入前 2 ms 才拨的开关：交叉正在进行
            if (where == Where::HighCutCrossing)
                highCut.setCutoff(4000.0f);
            if (where == Where::PolarityCrossing)
                polarity.setTarget(true);
            run(96, false);
            if (where == Where::HighCutCrossing)
                REQUIRE(highCut.isSmoothing());
            if (where == Where::PolarityCrossing)
                REQUIRE(!polarity.isSettled());
            run(1, true);
            CHECK(chain.machine().state() == SafetyState::Tripped);
            CHECK(chain.machine().tripReason() == TripReason::Nan);
            CHECK(PackedSafety::trip(meters.safety.load()) == TripReason::Nan);
            // 锁存：之后 2 s 一直跳闸、回送一直是 0（淡出 10 ms 之后）；不设自动恢复
            run(480, false);
            bool zero = true;
            for (int k = 0; k < 200; ++k)
                zero = run(480, false) && zero;
            CHECK(zero);
            CHECK(chain.machine().state() == SafetyState::Tripped);
            CHECK(chain.machine().latched());
        }
    }
}

TEST_CASE("Input 音色级：同采样率重新 prepare 不动它（流保持连续），换采样率才从头来", "[engine][input][tone]")
{
    // ⛔ 删除式验证点：每次 prepare 都重来音色级 → 开着的滤波器在重新 prepare 的那一块被清状态，与没重 prepare 的参照
    //    对不上，本用例变红。
    // 闭环，低切 150 Hz、高切 5 kHz、极性开；跑到稳态之后宿主同采样率重新 prepare 一次（参照不 prepare），
    // 之后照样与参照逐位相同。
    ClosedLoop loop(kSr, 512);
    loop.reference.enableTone();
    loop.source = [](std::int64_t t, int channel) { return sineAt(t, channel == 0 ? 90.0 : 130.0, kSr, 0.3); };
    const EngineParams p = withTone(loopParams(80.0f, 50.0f, 20.0f), 150.0f, 5000.0f, true);
    loop.run(p, 512, 48128);
    loop.input.prepare(kSr, 512);
    loop.run(p, 512, 24064);
    CHECK(firstBitDiff(loop.outLeft, loop.reference.wantLeft) == -1);
    CHECK(firstBitDiff(loop.outRight, loop.reference.wantRight) == -1);
    CHECK(peakOf(loop.reference.retLeft, 48128, loop.reference.retLeft.size()) > 0.05);
}

TEST_CASE("Input 高切被采样率截断时给提示位：44.1 kHz 下 19900 Hz 截断，19000 Hz、关、48 kHz 都不截断",
          "[engine][input][tone]")
{
    // ⛔ 删除式验证点：提示位不写 → 本用例变红。
    struct Case
    {
        double sr;
        float highCut;
        bool limited;
    };
    for (const Case k : {Case{44100.0, 19900.0f, true}, Case{44100.0, 19000.0f, false}, Case{44100.0, 20000.0f, false},
                         Case{48000.0, 19900.0f, false}})
    {
        INFO("sr = " << k.sr << ", high cut = " << k.highCut);
        InputEngine input;
        input.prepare(k.sr, 128);
        std::vector<float> left(128, 0.0f);
        std::vector<float> right(128, 0.0f);
        float* channels[2] = {left.data(), right.data()};
        AudioView view;
        view.channels = channels;
        view.numChannels = 2;
        view.length = 128;
        EngineParams p;
        p.highCutHz = k.highCut;
        input.process(view, p, TransportInfo{}, false, EngineContext{});
        CHECK(input.meters().highCutLimited.load() == k.limited);
        // 改回关之后下一块就清掉
        input.process(view, EngineParams{}, TransportInfo{}, false, EngineContext{});
        CHECK_FALSE(input.meters().highCutLimited.load());
    }
}
