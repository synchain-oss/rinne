// SPDX-License-Identifier: GPL-3.0-or-later
//
// 限制器开关的 10 ms 交叉过渡在真实路径上没有咔嗒，逐样本断言。只经 SafetyChain 的公开接口（prepare、beginBlock、
// processFront、processBack）驱动：交叉由 SafetyChain 里的 limiterMix_（dsp::LinearSmoother，时长
// safety::kLimiterCrossfadeSeconds）做，本文件只从外面测它。
// 信号：100 Hz、幅度 0.68 的正弦。上限 −6 dBFS（约 0.501）下限制器压约 2.6 dB；0.68 低于软削波拐点 0.7，限制器关时
// 原样通过，测到的就是限制器开关本身。feedback = 1：限制器开关只改反馈上限（1.5 / 1.0），不改这里 ⑥ 的增益。
// 开关在块边界生效（beginBlock 读控制量），块长不超过 64，事件所在的样本另起一块：正弦的峰上关、过渡走到一半反向开、
// 谷底上关、峰上开，之后每 3 ms 来回切一串（每一下都落在上一段过渡的半路上）。
// 判据：回送的相邻样本差不超过输入正弦相邻样本差上界 2a·sin(πf/sr) 的 1.5 倍（与 test_cut_filter.cpp 同一个判据）。
// 必须变红：硬切换。两条同样的安全链，一条限制器常开、一条常关，在同一事件上直接拼接，相邻样本差是判据的 3 倍以上。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "ToneTestKit.h"
#include "engine/SafetyMachine.h"

using rinne::engine::SafetyChain;
using rinne::engine::SafetyControls;
using rinne::test::sineStepBound;

namespace
{

constexpr double kRates[] = {44100.0, 48000.0, 96000.0, 192000.0};
constexpr double kFreq = 100.0;
constexpr double kAmp = 0.68;
constexpr std::int64_t kMaxBlock = 64;
constexpr double kCriterion = 1.5; // 相邻样本差 ≤ 正弦自身上界的这么多倍

// 从正弦段的第 at 个样本起，限制器开关设成 on
struct Toggle
{
    std::int64_t at;
    bool on;
};

// 正弦的第 k 个峰（quarter = 0.25）或谷（quarter = 0.75）所在的样本
std::int64_t phaseSample(double sr, int k, double quarter)
{
    return static_cast<std::int64_t>(std::llround(sr / kFreq * (static_cast<double>(k) + quarter)));
}

// 一条安全链：配对、反馈 1、上限默认 −6 dBFS；先 0.6 s 静音让 500 ms 淡入走完，再喂 total 个样本的正弦。
// 返回正弦段的左声道回送
std::vector<float> runChain(double sr, bool startOn, const std::vector<Toggle>& toggles, std::int64_t total)
{
    SafetyChain chain;
    chain.prepare(sr);
    SafetyControls c;
    c.paired = true;
    c.feedback = 1.0f;
    c.limiterOn = startOn;

    const auto settle = static_cast<std::int64_t>(std::llround(0.6 * sr));
    for (std::int64_t done = 0; done < settle;)
    {
        const std::int64_t len = std::min(kMaxBlock, settle - done);
        chain.beginBlock(c);
        for (std::int64_t i = 0; i < len; ++i)
        {
            float l = 0.0f;
            float r = 0.0f;
            chain.processFront(l, r, true);
            chain.processBack(l, r, 0.0f, 0.0f);
        }
        done += len;
    }

    std::vector<float> out(static_cast<std::size_t>(total));
    std::size_t next = 0;
    std::int64_t n = 0;
    while (n < total)
    {
        while (next < toggles.size() && toggles[next].at <= n)
            c.limiterOn = toggles[next++].on;
        std::int64_t end = std::min(n + kMaxBlock, total);
        if (next < toggles.size() && toggles[next].at < end)
            end = toggles[next].at;
        chain.beginBlock(c);
        for (; n < end; ++n)
        {
            float l = rinne::test::sine(kFreq, sr, n, kAmp);
            float r = l;
            chain.processFront(l, r, true);
            chain.processBack(l, r, 0.0f, 0.0f);
            out[static_cast<std::size_t>(n)] = l;
        }
    }
    return out;
}

// [from, to) 里相邻样本差的最大值
double maxStep(const std::vector<float>& v, std::int64_t from, std::int64_t to)
{
    double worst = 0.0;
    for (auto i = static_cast<std::size_t>(from + 1); i < static_cast<std::size_t>(to); ++i)
        worst = std::fmax(worst, std::fabs(static_cast<double>(v[i]) - v[i - 1]));
    return worst;
}

double peakOf(const std::vector<float>& v, std::int64_t from, std::int64_t to)
{
    double worst = 0.0;
    for (auto i = static_cast<std::size_t>(from); i < static_cast<std::size_t>(to); ++i)
        worst = std::fmax(worst, std::fabs(static_cast<double>(v[i])));
    return worst;
}

} // namespace

TEST_CASE("switching the limiter on the real path makes no click, sample by sample", "[tone][limiter][click]")
{
    for (const double sr : kRates)
    {
        INFO("sr = " << sr);
        const std::int64_t total = std::llround(0.8 * sr);
        const std::int64_t offAtPeak = phaseSample(sr, 30, 0.25);
        const std::int64_t backOn = offAtPeak + std::llround(0.005 * sr); // 10 ms 过渡走到一半
        const std::int64_t offAtTrough = phaseSample(sr, 40, 0.75);
        const std::int64_t onAtPeak = phaseSample(sr, 50, 0.25);
        std::vector<Toggle> toggles = {{offAtPeak, false}, {backOn, true}, {offAtTrough, false}, {onAtPeak, true}};
        const std::int64_t burst = phaseSample(sr, 60, 0.25);
        const std::int64_t every = std::llround(0.003 * sr);
        for (int i = 0; i < 10; ++i)
            toggles.push_back({burst + i * every, i % 2 != 0});

        const std::vector<float> ret = runChain(sr, true, toggles, total);
        const double bound = kCriterion * sineStepBound(kAmp, kFreq, sr);
        // 从限制器已经稳住的 0.25 s 起量到结尾（第一个事件在 0.3025 s）
        const std::int64_t from = std::llround(0.25 * sr);
        const double worst = maxStep(ret, from, total);
        INFO("worst step = " << worst << ", sine bound = " << sineStepBound(kAmp, kFreq, sr));
        CHECK(worst < bound);

        // 开关确实生效了：开着时峰值压在上限（约 0.501）上，关掉 10 ms 之后是不限制的约 0.679（DC 阻断在 100 Hz 衰减约
        // 0.01 dB），不是一直开着或一直关着
        CHECK(peakOf(ret, from, offAtPeak) <= 0.5012);
        CHECK(peakOf(ret, offAtTrough + std::llround(0.011 * sr), onAtPeak) > 0.67);
        CHECK(peakOf(ret, onAtPeak + std::llround(0.011 * sr), burst) <= 0.5012);

        // 必须变红：硬切换。常开与常关两条链在同一事件上直接拼接（峰上关、峰上开），相邻样本差远超判据
        const std::vector<float> on = runChain(sr, true, {}, total);
        const std::vector<float> off = runChain(sr, false, {}, total);
        std::vector<float> hardOff(on);
        std::copy(off.begin() + offAtPeak, off.end(), hardOff.begin() + offAtPeak);
        std::vector<float> hardOn(off);
        std::copy(on.begin() + onAtPeak, on.end(), hardOn.begin() + onAtPeak);
        INFO("hard off = " << maxStep(hardOff, from, total) << ", hard on = " << maxStep(hardOn, from, total));
        CHECK(maxStep(hardOff, from, total) > 3.0 * bound);
        CHECK(maxStep(hardOn, from, total) > 3.0 * bound);
    }
}
