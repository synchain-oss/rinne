// SPDX-License-Identifier: GPL-3.0-or-later
//
// 链增益估计（失控保护的防误报门与超大值都靠它）：
//   恒等链与衰减链下估计永远 ≤ 1：D 取 1 ms 与 4000 ms 两端，信号是忽大忽小的突发，块长随机切分；
//   D 变小时旧的较大 D 继续用到旧内容移出回读窗，变大时立即用；
//   送出为 0 而回读很大时判为放大（+∞）。

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>

#include "SafetyTestKit.h"

using namespace rinne::test;

namespace
{

// 每 37 ms 换一次幅度（0…+9 dBFS 之间随机）的噪声突发，中间夹着静音：最不利于峰值比较的非平稳信号
struct Bursts
{
    Rng rng{31};
    float level = 0.0f;
    float operator()(std::int64_t n)
    {
        if (n % 1776 == 0)
            level = rng.below(4) == 0 ? 0.0f : static_cast<float>(rng.range(0.0, 2.8));
        return level * static_cast<float>(rng.range(-1.0, 1.0));
    }
};

float maxEstimateOver(LoopSim& sim, double seconds, std::uint64_t seed, Bursts& dry)
{
    Rng split(seed);
    float worst = 0.0f;
    const std::int64_t total = samplesOf(seconds);
    for (std::int64_t done = 0; done < total;)
    {
        const int len = 1 + split.below(2048);
        sim.run(len, len, dry, [&](std::int64_t, float, float, float, float) {
            worst = std::fmax(worst, sim.chain.chainGainEstimate());
        });
        done += len;
    }
    return worst;
}

} // namespace

TEST_CASE("the estimate never exceeds 1 for unity and attenuating chains", "[safety][estimator]")
{
    for (const std::int64_t delay : {samplesOf(0.001), samplesOf(4.0)})
        for (const float gain : {1.0f, 0.5f})
        {
            INFO("delay " << delay << " chain gain " << gain);
            LoopSim sim(kRate, delay, gain);
            sim.controls.feedback = 0.9f;
            Bursts dry;
            const float worst = maxEstimateOver(sim, 4.0 + 2.0 * static_cast<double>(delay) / kRate, delay + 1, dry);
            CHECK(worst <= 1.0f);
            CHECK(worst > 0.0f);
        }
}

TEST_CASE("an amplifying chain is estimated above 1", "[safety][estimator]")
{
    LoopSim sim(kRate, 4800, dbfs(1.0));
    sim.controls.feedback = 0.5f;
    sim.controls.guardOn = false; // 让环路停在稳态，峰值比就是链增益本身
    sim.run(
        samplesOf(3.0), 256, [](std::int64_t n) { return sine(500.0, kRate, n, 1.0); },
        [](std::int64_t, float, float, float, float) {});
    CHECK(sim.chain.chainAmplifies());
    CHECK(std::fabs(sim.chain.chainGainEstimate() - dbfs(1.0)) < 0.02f);
}

TEST_CASE("a shorter loop delay keeps the longer window until the old content has passed", "[safety][estimator]")
{
    // 3 s 的环跑一段渐弱信号，然后真实延迟与报给安全链的 D 一起改成 0.1 s：回读窗里还有 3 s 前更响的内容
    const auto fading = [](std::int64_t n) {
        const double t = static_cast<double>(n) / kRate;
        return static_cast<float>(3.0 * std::exp(-t / 4.0) * std::sin(2.0 * kPi * 300.0 * t));
    };
    LoopSim sim(kRate, samplesOf(3.0), 1.0f, samplesOf(3.0));
    sim.controls.feedback = 0.9f;
    float worst = 0.0f;
    const auto probe = [&](std::int64_t, float, float, float, float) {
        worst = std::fmax(worst, sim.chain.chainGainEstimate());
    };
    sim.run(samplesOf(6.0), 512, fading, probe);
    sim.setDelay(samplesOf(0.1));
    sim.run(samplesOf(0.2), 512, fading, probe);
    CHECK(sim.chain.estimatorDelay() == samplesOf(3.0)); // 还在保留旧的 D
    sim.run(samplesOf(5.0), 512, fading, probe);
    CHECK(sim.chain.estimatorDelay() == samplesOf(0.1)); // 旧内容移出后换成新的 D
    // 再改回 3 s：变大立即用
    sim.setDelay(samplesOf(3.0));
    sim.run(512, 512, fading, probe);
    CHECK(sim.chain.estimatorDelay() == samplesOf(3.0));
    sim.run(samplesOf(4.0), 512, fading, probe);
    CHECK(worst <= 1.0f);
}

TEST_CASE("a silent send with a loud return reads as amplifying", "[safety][estimator]")
{
    OpenLoop loop; // 反馈为 0：送出的就是干声，这里是静音
    loop.block(static_cast<int>(samplesOf(0.6)), [](std::int64_t) { return TapFrame{}; });
    loop.block(static_cast<int>(samplesOf(0.2)), [](std::int64_t n) {
        TapFrame f;
        f.tapLeft = (n / 30) % 2 ? 16.0f : -16.0f;
        f.tapRight = f.tapLeft;
        return f;
    });
    CHECK(loop.chain.chainAmplifies());
    CHECK(std::isinf(loop.chain.chainGainEstimate()));
    CHECK_FALSE(loop.chain.machine().latched()); // 回送不活动，超大值不计
}

TEST_CASE("the gate's amplify judgment is held for exactly 3 s, the per-hop one is not held", "[safety][estimator]")
{
    // 一次 20 ms 的回读（送出为静音，估计 > 1），之后全静音：估计在回读离开 100 ms 窗口后回到 ≤ 1；
    // 防误报门的判定从最后一次「估计 > 1」起再保持 3 s（误差一格 10 ms），逐格判定在那之前就已经落下
    OpenLoop loop;
    loop.block(static_cast<int>(samplesOf(0.6)), [](std::int64_t) { return TapFrame{}; });
    const std::int64_t burstFrom = loop.n;
    std::int64_t lastOver = -1; // 最后一个「估计 > 1」的样本
    std::int64_t lastHeld = -1; // 最后一个「门的判定为 true」的样本
    std::int64_t lastNow = -1; // 最后一个「逐格判定为 true」的样本
    for (int b = 0; b < 500; ++b)
        loop.block(480, [&](std::int64_t n) {
            TapFrame f;
            if (n < burstFrom + samplesOf(0.02))
                f.tapLeft = f.tapRight = (n / 24) % 2 ? 1.0f : -1.0f;
            if (loop.chain.chainGainEstimate() > 1.0f)
                lastOver = n;
            if (loop.chain.chainAmplifies())
                lastHeld = n;
            if (loop.chain.chainAmplifiesNow())
                lastNow = n;
            return f;
        });
    REQUIRE(lastOver > burstFrom);
    CHECK(lastHeld - lastOver >= samplesOf(3.0) - 480);
    CHECK(lastHeld - lastOver <= samplesOf(3.0) + 480);
    CHECK(lastNow <= lastOver);
}
