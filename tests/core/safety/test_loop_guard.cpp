// SPDX-License-Identifier: GPL-3.0-or-later
//
// 闭环里的失控保护与超大值：Input 的输出经环内链（一个增益）与 Output 的清洗写进环，D 个样本后读回。
//   防误报：反馈 ≤ 90 % 且链增益 ≤ 0 dB 时零介入——热干声把限制器压到 GR ≥ 6 dB 也不进入；
//          反馈 91 %，或链增益 +1 dB，同一段信号就会进入（门两侧各一格）；限制器关时同理。
//   真失控：链内 +12 dB、反馈 100 %：失控保护把它压住，不跳闸，回送始终 ≤ 上限；
//          链内 +34 dB、上限 −24 dBFS：压到 −24 dB 仍在失控，1 s 后跳闸（失控）；guard_on = 0 时不介入。
//   超大值：链内 +24 dB、反馈 20 %：回读超过 +12 dBFS，100 ms 后跳闸。

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>

#include "SafetyTestKit.h"

using namespace rinne::test;
namespace safety = rinne::engine::safety;

namespace
{

struct LoopResult
{
    bool guardEngaged = false;
    bool tripped = false;
    TripReason reason = TripReason::None;
    float maxReturn = 0.0f;
    float maxReduction = 0.0f;
};

// reportedDelay < 0 时引擎报给安全链的 D 就是环的真实延迟；否则环里另有引擎不知道的延迟（环内插件自己的）
template<typename Dry>
LoopResult runLoop(float feedback, float chainGain, bool limiterOn, double seconds, Dry&& dry, bool guardOn = true,
                   float ceilingDb = safety::kCeilingDefaultDb, std::int64_t delay = 4800,
                   std::int64_t reportedDelay = -1)
{
    LoopSim sim(kRate, delay, chainGain);
    if (reportedDelay >= 0)
        sim.controls.loopDelaySamples = reportedDelay;
    sim.controls.feedback = feedback;
    sim.controls.limiterOn = limiterOn;
    sim.controls.guardOn = guardOn;
    sim.controls.ceilingDb = ceilingDb;
    LoopResult res;
    const std::int64_t total = samplesOf(seconds);
    std::int64_t done = 0;
    while (done < total)
    {
        sim.run(256, 256, dry, [&](std::int64_t, float, float, float l, float r) {
            res.maxReturn = std::fmax(res.maxReturn, std::fmax(std::fabs(l), std::fabs(r)));
            res.guardEngaged = res.guardEngaged || sim.chain.machine().guardEngaged();
        });
        res.maxReduction = std::fmax(res.maxReduction, sim.meters.limiterReductionDb.take());
        done += 256;
    }
    res.tripped = sim.chain.machine().latched();
    res.reason = sim.chain.machine().tripReason();
    return res;
}

// +6 dBFS 的 1 kHz 正弦干声：稳定环里也足以把限制器压过 6 dB
float hotSine(std::int64_t n)
{
    return sine(1000.0, kRate, n, 2.0);
}

// 50 ms 的小噪声突发，之后静音：只用来起振
float kick(std::int64_t n)
{
    if (n >= 2400)
        return 0.0f;
    return static_cast<float>(0.1 * std::sin(static_cast<double>(n) * 0.37) * std::sin(static_cast<double>(n) * 0.013));
}

} // namespace

TEST_CASE("guard stays out at 90 % feedback with a unity chain even under heavy limiting", "[safety][loop][guard][fp]")
{
    const LoopResult r = runLoop(0.90f, 1.0f, true, 8.0, hotSine);
    CHECK(r.maxReduction > 10.0f); // 限制器确实压得很重：没有防误报门，失控保护早就进去了
    CHECK_FALSE(r.guardEngaged);
    CHECK_FALSE(r.tripped);
}

TEST_CASE("guard stays out at 90 % feedback with an attenuating chain", "[safety][loop][guard][fp]")
{
    const LoopResult r = runLoop(0.90f, 0.5f, true, 8.0, hotSine);
    CHECK_FALSE(r.guardEngaged);
    CHECK_FALSE(r.tripped);
}

// 慢慢变小的热干声：回读来自 D 之前更响的时刻，估计链增益的送出窗口必须往前盖住 D（与环内插件的延迟）才不误判
float fadingHot(std::int64_t n)
{
    const double t = static_cast<double>(n) / kRate;
    return static_cast<float>(3.0 * std::exp(-t / 5.0) * std::sin(2.0 * kPi * 500.0 * t));
}

TEST_CASE("guard stays out at 90 % feedback with a long loop delay and a fading signal", "[safety][loop][guard][fp]")
{
    const LoopResult r = runLoop(0.90f, 1.0f, true, 8.0, fadingHot, true, safety::kCeilingDefaultDb, samplesOf(1.5));
    CHECK(r.maxReduction > 6.0f);
    CHECK_FALSE(r.guardEngaged);
}

TEST_CASE("guard stays out at 90 % feedback when plugins in the loop add latency", "[safety][loop][guard][fp]")
{
    // 环的真实延迟 0.55 s，引擎只知道其中的 50 ms
    const LoopResult r =
        runLoop(0.90f, 1.0f, true, 8.0, fadingHot, true, safety::kCeilingDefaultDb, samplesOf(0.55), samplesOf(0.05));
    CHECK(r.maxReduction > 6.0f);
    CHECK_FALSE(r.guardEngaged);
}

TEST_CASE("the same signal engages guard at 91 % feedback", "[safety][loop][guard][fp]")
{
    const LoopResult r = runLoop(0.91f, 1.0f, true, 8.0, hotSine);
    CHECK(r.guardEngaged);
}

TEST_CASE("the same signal engages guard with a +1 dB chain at 90 % feedback", "[safety][loop][guard][fp]")
{
    const LoopResult r = runLoop(0.90f, 1.12201845f, true, 8.0, hotSine);
    CHECK(r.guardEngaged);
}

TEST_CASE("with the limiter off the gate works the same on soft-clip time", "[safety][loop][guard][fp]")
{
    const LoopResult out = runLoop(0.90f, 1.0f, false, 8.0, hotSine);
    CHECK_FALSE(out.guardEngaged);
    CHECK(out.maxReturn < 1.0f);
    const LoopResult in = runLoop(0.91f, 1.0f, false, 8.0, hotSine);
    CHECK(in.guardEngaged);
}

TEST_CASE("guard tames a +12 dB chain at 100 % feedback without tripping", "[safety][loop][guard]")
{
    const LoopResult r = runLoop(1.0f, 3.98107171f, true, 20.0, kick, true, safety::kCeilingDefaultDb, 2400);
    CHECK(r.guardEngaged);
    CHECK_FALSE(r.tripped);
    CHECK(r.maxReturn <= static_cast<float>(std::pow(10.0, safety::kCeilingDefaultDb / 20.0)));
}

TEST_CASE("a runaway that survives -24 dB of guard trips as runaway", "[safety][loop][guard]")
{
    const LoopResult r = runLoop(1.0f, 50.0f, true, 10.0, kick, true, -24.0f, 2400);
    CHECK(r.guardEngaged);
    CHECK(r.tripped);
    CHECK(r.reason == TripReason::Runaway);
    CHECK(r.maxReturn <= static_cast<float>(std::pow(10.0, -24.0 / 20.0)));
}

TEST_CASE("with guard_on off the same runaway is only held by the limiter", "[safety][loop][guard][off]")
{
    const LoopResult r = runLoop(1.0f, 50.0f, true, 10.0, kick, false, -24.0f, 2400);
    CHECK_FALSE(r.guardEngaged);
    CHECK_FALSE(r.tripped);
    CHECK(r.maxReturn <= static_cast<float>(std::pow(10.0, -24.0 / 20.0)));
}

TEST_CASE("a +24 dB chain at 20 % feedback trips as huge", "[safety][loop][huge]")
{
    const LoopResult r =
        runLoop(0.2f, 15.8489319f, true, 2.0, [](std::int64_t n) { return sine(220.0, kRate, n, 1.0); });
    CHECK(r.tripped);
    CHECK(r.reason == TripReason::Huge);
}

TEST_CASE("a +12 dBFS dry signal at 90 % feedback with a unity chain does not trip", "[safety][loop][huge][fp]")
{
    for (const double freq : {110.0, 1000.0, 3150.0})
        for (const std::int64_t delay : {480, 4800, 7777})
        {
            INFO("freq " << freq << " delay " << delay);
            const LoopResult r = runLoop(
                0.90f, 1.0f, true, 3.0, [freq](std::int64_t n) { return sine(freq, kRate, n, safety::kHugeLevel); },
                true, safety::kCeilingDefaultDb, delay);
            CHECK_FALSE(r.tripped);
        }
}
