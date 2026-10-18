// SPDX-License-Identifier: GPL-3.0-or-later
//
// 闭环里的失控保护：Input 的输出经环内链（一个增益）与 Output 的清洗写进环，D 个样本后读回。
//   防误报：反馈 ≤ 90 % 且链增益 ≤ 0 dB 时零介入——热干声把限制器压到 GR ≥ 6 dB 也不进入；
//          反馈 91 %，或链增益 +1 dB，同一段信号就会进入（门两侧各一格）；限制器关时同理；
//          D 很长或环内插件另有延迟时，渐弱的信号也不误判。
//   真失控：链内 +12 dB、反馈 100 %：失控保护把它压住，不跳闸，回送始终 ≤ 上限；
//          链内 +34 dB、上限 −24 dBFS：压到 −24 dB 仍在失控，1 s 后跳闸（失控）；guard_on = 0 时不介入。

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>

#include "SafetyTestKit.h"

using namespace rinne::test;
namespace safety = rinne::engine::safety;

namespace
{

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

// 慢慢变小的热干声：回读来自 D 之前更响的时刻，估计链增益的送出窗口必须往前盖住 D（与环内插件的延迟）才不误判
float fadingHot(std::int64_t n)
{
    const double t = static_cast<double>(n) / kRate;
    return static_cast<float>(3.0 * std::exp(-t / 5.0) * std::sin(2.0 * kPi * 500.0 * t));
}

LoopSetup gate(float feedback, float chainGain, bool limiterOn = true)
{
    LoopSetup s;
    s.feedback = feedback;
    s.chainGain = chainGain;
    s.limiterOn = limiterOn;
    return s;
}

} // namespace

TEST_CASE("guard stays out at 90 % feedback with a unity chain even under heavy limiting", "[safety][loop][guard][fp]")
{
    const LoopResult r = runLoop(gate(0.90f, 1.0f), hotSine);
    CHECK(r.maxReduction > 10.0f); // 限制器确实压得很重：没有防误报门，失控保护早就进去了
    CHECK_FALSE(r.guardEngaged);
    CHECK_FALSE(r.tripped);
}

TEST_CASE("guard stays out at 90 % feedback with an attenuating chain", "[safety][loop][guard][fp]")
{
    const LoopResult r = runLoop(gate(0.90f, 0.5f), hotSine);
    CHECK_FALSE(r.guardEngaged);
    CHECK_FALSE(r.tripped);
}

TEST_CASE("guard stays out at 90 % feedback with a long loop delay and a fading signal", "[safety][loop][guard][fp]")
{
    LoopSetup s = gate(0.90f, 1.0f);
    s.delay = samplesOf(1.5);
    const LoopResult r = runLoop(s, fadingHot);
    CHECK(r.maxReduction > 6.0f);
    CHECK_FALSE(r.guardEngaged);
}

TEST_CASE("guard stays out at 90 % feedback when plugins in the loop add latency", "[safety][loop][guard][fp]")
{
    // 环的真实延迟 0.55 s，引擎只知道其中的 50 ms
    LoopSetup s = gate(0.90f, 1.0f);
    s.delay = samplesOf(0.55);
    s.reportedDelay = samplesOf(0.05);
    const LoopResult r = runLoop(s, fadingHot);
    CHECK(r.maxReduction > 6.0f);
    CHECK_FALSE(r.guardEngaged);
}

TEST_CASE("the same signal engages guard at 91 % feedback", "[safety][loop][guard][fp]")
{
    CHECK(runLoop(gate(0.91f, 1.0f), hotSine).guardEngaged);
}

TEST_CASE("the same signal engages guard with a +1 dB chain at 90 % feedback", "[safety][loop][guard][fp]")
{
    CHECK(runLoop(gate(0.90f, dbfs(1.0)), hotSine).guardEngaged);
}

TEST_CASE("with the limiter off the gate works the same on soft-clip time", "[safety][loop][guard][fp]")
{
    const LoopResult out = runLoop(gate(0.90f, 1.0f, false), hotSine);
    CHECK_FALSE(out.guardEngaged);
    CHECK(out.maxReturn < 1.0f);
    CHECK(runLoop(gate(0.91f, 1.0f, false), hotSine).guardEngaged);
}

TEST_CASE("guard tames a +12 dB chain at 100 % feedback without tripping", "[safety][loop][guard]")
{
    LoopSetup s = gate(1.0f, dbfs(12.0));
    s.delay = 2400;
    s.seconds = 20.0;
    const LoopResult r = runLoop(s, kick);
    CHECK(r.guardEngaged);
    CHECK_FALSE(r.tripped);
    CHECK(r.maxReturn <= dbfs(safety::kCeilingDefaultDb));
}

TEST_CASE("a runaway that survives -24 dB of guard trips as runaway", "[safety][loop][guard]")
{
    LoopSetup s = gate(1.0f, 50.0f);
    s.delay = 2400;
    s.ceilingDb = -24.0f;
    s.seconds = 10.0;
    const LoopResult r = runLoop(s, kick);
    CHECK(r.guardEngaged);
    CHECK(r.tripped);
    CHECK(r.reason == TripReason::Runaway);
    CHECK(r.maxReturn <= dbfs(-24.0));
}

TEST_CASE("with guard_on off the same runaway is only held by the limiter", "[safety][loop][guard][off]")
{
    LoopSetup s = gate(1.0f, 50.0f);
    s.delay = 2400;
    s.ceilingDb = -24.0f;
    s.seconds = 10.0;
    s.guardOn = false;
    const LoopResult r = runLoop(s, kick);
    CHECK_FALSE(r.guardEngaged);
    CHECK_FALSE(r.tripped);
    CHECK(r.maxReturn <= dbfs(-24.0));
}

namespace
{

// 间歇的鼓点干声：每 minGap–maxGap 秒一下（默认 0.2–0.8 s），20 ms 长、峰值 peakDb 的衰减噪声，其余时间静音。
// 它会让送出窗口里时不时出现比回读更响的瞬态，把真放大的链盖住
struct Drums
{
    Rng rng;
    float peak;
    std::int64_t next = 0;
    std::int64_t hitAt = -1000000;
    double minGap;
    double maxGap;
    Drums(std::uint64_t seed, double peakDb, double minGapSeconds = 0.2, double maxGapSeconds = 0.8)
        : rng(seed), peak(dbfs(peakDb)), minGap(minGapSeconds), maxGap(maxGapSeconds)
    {
    }
    float operator()(std::int64_t n)
    {
        if (n >= next)
        {
            hitAt = n;
            next = n + samplesOf(rng.range(minGap, maxGap));
        }
        const std::int64_t k = n - hitAt;
        if (k >= samplesOf(0.02))
            return 0.0f;
        const double env = 1.0 - static_cast<double>(k) / static_cast<double>(samplesOf(0.02));
        return static_cast<float>(peak * env * rng.range(-1.0, 1.0));
    }
};

} // namespace

TEST_CASE("louder dry transients in the send window do not hide a real runaway at 90 % feedback",
          "[safety][loop][guard][fp]")
{
    // 链 +7.2 / +8 dB、反馈 90 %：环路增益 2.07–2.25，自激被限制器钉在上限；干声鼓点让估计间歇地偏小
    struct Cell
    {
        float chainGain;
        double peakDb;
        std::uint64_t seed;
        float ceilingDb;
    };
    for (const Cell c : {Cell{2.3f, -1.0, 1, -6.0f}, Cell{2.3f, 0.0, 1, -6.0f}, Cell{2.5f, -1.0, 1, -6.0f},
                         Cell{2.5f, 0.0, 1, -6.0f}, Cell{2.3f, -1.0, 2, -6.0f}, Cell{2.3f, 0.0, 2, -6.0f},
                         Cell{2.5f, -1.0, 2, -6.0f}, Cell{2.5f, 0.0, 2, -6.0f}, Cell{2.5f, 0.0, 3, -12.0f}})
    {
        INFO("chain " << c.chainGain << " peak " << c.peakDb << " seed " << c.seed << " ceiling " << c.ceilingDb);
        LoopSetup s = gate(0.90f, c.chainGain);
        s.delay = samplesOf(0.1);
        s.ceilingDb = c.ceilingDb;
        s.seconds = 12.0;
        Drums dry(c.seed, c.peakDb);
        const LoopResult r = runLoop(s, dry);
        CHECK(r.guardEngaged);
        CHECK(r.maxReturn <= dbfs(c.ceilingDb));
    }
}

TEST_CASE("the amplify hold bridges drum hits 1.3 s apart", "[safety][loop][guard][fp]")
{
    // 每一下鼓点在送出窗口里停约 1.1 s，这段时间估计被盖住；鼓点相隔约 1.3 s 时，保持要不短于约 1.5 s，
    // 失控保护才能在 3 s 窗里攒够 2 s。这一格只钉住保持的下限：任何 ≥ 2 s 的保持都会给出一段连续 ≥ 2 s 的
    // 打开期，进入判据分不出 2 s 与 3 s。3 s 的长度由估计器那一层的用例逐格钉住
    LoopSetup s = gate(0.90f, 2.5f);
    s.delay = samplesOf(0.1);
    s.seconds = 12.0;
    Drums dry(4, 0.0, 1.25, 1.35);
    CHECK(runLoop(s, dry).guardEngaged);
}
