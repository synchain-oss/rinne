// SPDX-License-Identifier: GPL-3.0-or-later
//
// 闭环里的超大值（≥ +12 dBFS 持续 100 ms 跳闸）。只计回送路径自己放大出来的电平：
//   |tap| 只在链增益估计 > 0 dB 时计入；|x|（×g 之后）只在链增益估计 > 0 dB 或 g > 1 时计入；回送不活动（g =
//   0）时都不计。
// 干声随整条通道进环：恒等链下热干声绕一圈回来就是 |tap|，绝对阈值单看 tap 会误跳（按字面公式 max(|tap|, |x|)
// 无条件计入时，+12 dBFS 干声、反馈 90 %、恒等链的 9 格里有 8 格误跳）；该放大的情形照样在 100 ms 级跳闸。

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <memory>

#include "SafetyTestKit.h"

using namespace rinne::test;
namespace safety = rinne::engine::safety;

namespace
{

LoopSetup loop(float feedback, float returnGain, float chainGain = 1.0f)
{
    LoopSetup s;
    s.feedback = feedback;
    s.returnGain = returnGain;
    s.chainGain = chainGain;
    s.seconds = 3.0;
    return s;
}

auto hotDry(double dbfsLevel, double freq = 1000.0)
{
    const double amplitude = std::pow(10.0, dbfsLevel / 20.0);
    return [=](std::int64_t n) { return sine(freq, kRate, n, amplitude); };
}

// 第一个有效读样本加上淡入走完的时刻：回送的增益从这里起才是设定值
std::int64_t settledAt(const LoopSetup& s)
{
    return s.delay + samplesOf(0.5);
}

} // namespace

TEST_CASE("a +12 dBFS dry signal at 90 % feedback with a unity chain does not trip", "[safety][loop][huge][fp]")
{
    for (const double freq : {110.0, 1000.0, 3150.0})
        for (const std::int64_t delay : {480, 4800, 7777})
        {
            INFO("freq " << freq << " delay " << delay);
            LoopSetup s = loop(0.90f, 1.0f);
            s.delay = delay;
            CHECK_FALSE(runLoop(s, hotDry(12.0, freq)).tripped);
        }
}

TEST_CASE("hot dry signals with a unity chain do not trip at 0 % or 90 % feedback, at either mix",
          "[safety][loop][huge][fp]")
{
    for (const double level : {12.0, 18.0})
        for (const float feedback : {0.0f, 0.90f})
            for (const float returnGain : {0.5f, 1.0f}) // 默认混合 25 % 与混合 50 %
            {
                INFO("level " << level << " feedback " << feedback << " g_ret " << returnGain);
                CHECK_FALSE(runLoop(loop(feedback, returnGain), hotDry(level)).tripped);
            }
}

TEST_CASE("|x| counts only when g exceeds 1: g = 1.0 and 0.75 stay, 1.01 and 1.1 trip",
          "[safety][loop][huge][threshold]")
{
    // +13 dBFS 干声：g > 1 时 |x| 在每个峰上都 ≥ +12 dBFS，与回送叠加的相位无关
    const auto dry = hotDry(13.0);
    struct Cell
    {
        float feedback;
        float returnGain;
        bool trips;
    };
    for (const Cell c :
         {Cell{1.00f, 1.0f, false}, Cell{1.50f, 0.5f, false}, Cell{1.01f, 1.0f, true}, Cell{1.10f, 1.0f, true}})
    {
        INFO("feedback " << c.feedback << " g_ret " << c.returnGain);
        const LoopSetup s = loop(c.feedback, c.returnGain);
        const LoopResult r = runLoop(s, dry);
        CHECK(r.tripped == c.trips);
        if (c.trips)
        {
            CHECK(r.reason == TripReason::Huge);
            CHECK(r.tripSample - settledAt(s) < samplesOf(0.2));
        }
    }
}

TEST_CASE("a +24 dB chain at 20 % feedback trips as huge", "[safety][loop][huge]")
{
    const LoopResult r = runLoop(loop(0.2f, 1.0f, dbfs(24.0)), hotDry(0.0, 220.0));
    CHECK(r.tripped);
    CHECK(r.reason == TripReason::Huge);
}

TEST_CASE("a plugin that emits +40 dBFS into a silent loop trips within about 100 ms", "[safety][loop][huge]")
{
    LoopSetup s = loop(0.2f, 1.0f);
    s.inject = [](std::int64_t n) { return (n / 60) % 2 ? 100.0f : -100.0f; };
    const LoopResult r = runLoop(s, [](std::int64_t) { return 0.0f; });
    CHECK(r.tripped);
    CHECK(r.reason == TripReason::Huge);
    // 第一个有效读样本起，淡入刚开始回送就有贡献，判据开始计时
    CHECK(r.tripSample - s.delay < samplesOf(0.13));
    CHECK(r.maxEstimate > 100.0f); // 送出只有淡入头几步的一点回送，回读却满幅：判为放大
}

TEST_CASE("a +24 dB chain does not trip at 0 % feedback and trips about 100 ms after feedback is raised",
          "[safety][loop][huge]")
{
    LoopSim sim(kRate, 4800, dbfs(24.0));
    const auto dry = hotDry(0.0, 220.0);
    bool tripped = false;
    std::int64_t tripAt = -1;
    const auto probe = [&](std::int64_t n, float, float, float, float) {
        if (tripAt < 0 && sim.chain.machine().latched())
            tripAt = n;
    };
    sim.run(samplesOf(2.0), 256, dry, probe);
    tripped = sim.chain.machine().latched();
    CHECK_FALSE(tripped);
    CHECK(sim.chain.chainAmplifies()); // 链确实在放大，只是回送不活动，不计
    const std::int64_t raisedAt = sim.n;
    sim.controls.feedback = 0.2f;
    sim.run(samplesOf(0.5), 256, dry, probe);
    REQUIRE(sim.chain.machine().latched());
    CHECK(sim.chain.machine().tripReason() == TripReason::Huge);
    CHECK(tripAt - raisedAt >= samplesOf(0.1) - 1);
    CHECK(tripAt - raisedAt < samplesOf(0.12));
}

TEST_CASE("low-frequency hot returns at 20 Hz and 10 Hz still trip", "[safety][loop][huge][threshold]")
{
    // |x| 的峰每半个周期出现一次：20 Hz 每 25 ms、10 Hz 每 50 ms。g = 1.2，×g 之后约 +14.6 dBFS
    for (const double freq : {20.0, 10.0})
    {
        INFO("freq " << freq);
        OpenLoop loop;
        loop.controls.feedback = 1.2f;
        loop.block(static_cast<int>(samplesOf(0.6)), [](std::int64_t) { return TapFrame{}; });
        const double dcGain = freq / std::sqrt(freq * freq + 25.0); // 5 Hz 一阶高通在该频率的增益
        const double amplitude = dbfs(14.6) / 1.2 / dcGain;
        const std::int64_t start = loop.n;
        std::int64_t tripAt = -1;
        for (int b = 0; b < 40 && tripAt < 0; ++b)
            loop.block(480, [&](std::int64_t n) {
                TapFrame f;
                f.tapLeft = sine(freq, kRate, n - start, amplitude);
                f.tapRight = f.tapLeft;
                if (tripAt < 0 && loop.chain.machine().latched())
                    tripAt = n;
                return f;
            });
        REQUIRE(tripAt >= 0);
        CHECK(loop.chain.machine().tripReason() == TripReason::Huge);
        CHECK(tripAt - start < samplesOf(0.2));
    }
}

TEST_CASE("one over-estimated moment does not expose hot dry to huge for the next 3 s", "[safety][loop][huge][fp]")
{
    // 恒等链、反馈 90 %：环内插件在 1 s 处吐出 5 ms 的 +40 dBFS 尖峰，估计在那 100 ms 里 > 1，防误报门因此保持
    // 打开 3 s；1.3 s 起是 +13 dBFS 的热干声。超大值看的是逐格判定，所以不跳；它若跟着保持走，这里就会误跳
    LoopSim sim(kRate, 4800, 1.0f);
    sim.controls.feedback = 0.9f;
    sim.inject = [](std::int64_t n) {
        return (n >= samplesOf(1.0) && n < samplesOf(1.005)) ? ((n / 24) % 2 ? 100.0f : -100.0f) : 0.0f;
    };
    const double amplitude = dbfs(13.0);
    const auto dry = [amplitude](std::int64_t n) {
        return n < samplesOf(1.3) ? 0.0f : sine(1000.0, kRate, n, amplitude);
    };
    bool heldWhileHot = false;
    sim.run(samplesOf(4.0), 256, dry, [&](std::int64_t n, float, float, float, float) {
        if (n > samplesOf(1.4) && sim.chain.chainAmplifies() && !sim.chain.chainAmplifiesNow())
            heldWhileHot = true;
    });
    CHECK(heldWhileHot); // 用例确实落在「保持中、逐格不放大、热干声在」的区间里
    CHECK_FALSE(sim.chain.machine().latched());
}

TEST_CASE("a +12.5 dBFS dry at 90 % feedback does not trip after a plugin burst or through an all-pass chain",
          "[safety][loop][huge][fp]")
{
    // 两种情形都会让链增益估计短时略 > 1：恒等链上插件吐一次 10 ms 突发；幅度恒为 1、只改相位的 200 Hz 全通链。
    // 估计偏大的那一格之后，热干声不能被当成「放大出来的超大值」
    const auto dry = hotDry(12.5);
    for (const float burst : {0.5f, 1.0f, 4.0f, 100.0f})
        for (const double offsetMs : {0.0, 3.0, 7.0}) // 突发与 10 ms 格对齐或跨两格
        {
            INFO("burst " << burst << " offset " << offsetMs << " ms");
            LoopSetup s = loop(0.90f, 1.0f);
            s.seconds = 4.0;
            const std::int64_t from = samplesOf(1.0 + offsetMs / 1000.0);
            const std::int64_t to = from + samplesOf(0.01);
            s.inject = [burst, from, to](std::int64_t n) {
                return (n >= from && n < to) ? ((n / 24) % 2 ? burst : -burst) : 0.0f;
            };
            CHECK_FALSE(runLoop(s, dry).tripped);
        }
    for (const std::int64_t delay : {480, 4800})
    {
        INFO("all-pass, delay " << delay);
        LoopSetup s = loop(0.90f, 1.0f);
        s.seconds = 4.0;
        s.delay = delay;
        auto left = std::make_shared<AllPass>(200.0, kRate);
        auto right = std::make_shared<AllPass>(200.0, kRate);
        s.process = [left, right](float& l, float& r) {
            l = (*left)(l);
            r = (*right)(r);
        };
        const LoopResult r = runLoop(s, dry);
        CHECK(r.maxEstimate > 1.0f); // 全通链确实让估计偏大过
        CHECK_FALSE(r.tripped);
    }
}
