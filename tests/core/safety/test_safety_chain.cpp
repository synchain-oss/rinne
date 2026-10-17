// SPDX-License-Identifier: GPL-3.0-or-later
//
// 安全链整体：回送硬上界（|ret| < 1.0，限制器开时 ≤ 上限）在随机与极端输入、10^6 次参数跳变下逐样本成立；
// 各级顺序与参考组合逐位一致；增益律（反馈上限、30 ms 平滑终点精确、环路开关与旁路 10 ms、限制器开关 10 ms 交叉）；
// NaN 策略（环里来的只清洗计数，Input 内部的立即跳闸）；测量量；块长不变性。

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>
#include <vector>

#include "SafetyTestKit.h"
#include "dsp/DcBlocker.h"
#include "dsp/PeakLimiter.h"
#include "dsp/SoftClip.h"

using namespace rinne::test;
namespace safety = rinne::engine::safety;
using rinne::engine::PackedSafety;

namespace
{

// 安全链的全部状态都在对象里：不持有堆内存，处理函数不抛异常
static_assert(std::is_trivially_destructible_v<SafetyChain>, "安全链不得持有堆内存");
static_assert(noexcept(std::declval<SafetyChain&>().processFront(std::declval<float&>(), std::declval<float&>(),
                                                                 true)));
static_assert(noexcept(std::declval<SafetyChain&>().processBack(std::declval<float&>(), std::declval<float&>(), 0.0f,
                                                                0.0f)));
static_assert(noexcept(std::declval<SafetyChain&>().beginBlock(std::declval<const SafetyControls&>())));
static_assert(noexcept(std::declval<SafetyChain&>().publish(std::declval<Meters&>())));

float dbToGain(double db)
{
    return static_cast<float>(std::pow(10.0, db / 20.0));
}

TapFrame silence(std::int64_t)
{
    return TapFrame{};
}

// 淡入走完、停在运行态的开环夹具
void settle(OpenLoop& loop, double seconds = 0.6)
{
    loop.block(static_cast<int>(samplesOf(seconds)), silence);
}

float extremeSample(Rng& rng, int kind, std::int64_t n)
{
    switch (kind)
    {
    case 0:
        return std::numeric_limits<float>::quiet_NaN();
    case 1:
        return (n % 2) ? std::numeric_limits<float>::infinity() : -std::numeric_limits<float>::infinity();
    case 2:
        return (n / 37) % 2 ? 100.0f : -100.0f; // +40 dBFS 方波
    case 3:
        return 0.8f; // 直流偏移
    case 4:
        return rng.bits(); // 任意位型
    default:
        return static_cast<float>(rng.range(-3.0, 3.0));
    }
}

struct Recorded
{
    std::vector<float> left;
    std::vector<float> right;
    std::vector<SafetyEvent> events;
};

} // namespace

TEST_CASE("safety controls default to the safe settings", "[safety][chain][defaults]")
{
    const SafetyControls c;
    CHECK(c.feedback == 0.0f);
    CHECK(c.limiterOn);
    CHECK(c.ceilingDb == -6.0f);
    CHECK(c.guardOn);
    CHECK_FALSE(c.paired);
    // 反馈为 0 时即使配上、淡入走完，回送增益也是 0
    OpenLoop loop;
    settle(loop);
    CHECK(loop.chain.lastGain() == 0.0f);
}

TEST_CASE("return stays below 1.0 and within the ceiling for random and extreme input", "[safety][chain][bound]")
{
    Rng rng(2026);
    OpenLoop loop;
    std::int64_t settledAt = 0; // 限制器开、上限不变满 31 ms 之后才按上限断言（10 ms 交叉、30 ms 平滑）
    float ceiling = dbToGain(safety::kCeilingDefaultDb);
    bool limiter = true;
    std::int64_t overCeiling = 0;
    std::int64_t checkedAgainstCeiling = 0;
    int recoveries = 0;
    for (int b = 0; b < 4000; ++b)
    {
        if (b % 8 == 0)
        {
            loop.controls.feedback = static_cast<float>(rng.range(0.0, 1.6));
            const bool newLimiter = rng.below(4) != 0;
            const auto newCeilingDb = static_cast<float>(rng.range(-24.0, 0.0));
            if (newLimiter != limiter || dbToGain(newCeilingDb) != ceiling)
                settledAt = loop.n + samplesOf(0.031);
            limiter = newLimiter;
            ceiling = dbToGain(newCeilingDb);
            loop.controls.limiterOn = limiter;
            loop.controls.ceilingDb = newCeilingDb;
        }
        if (loop.chain.machine().latched() && !loop.chain.machine().coolingDown())
        {
            ++loop.controls.recoverSeq;
            ++recoveries;
        }
        const int kind = rng.below(8);
        loop.block(
            1 + rng.below(2048),
            [&](std::int64_t n) {
                TapFrame f;
                f.tapLeft = extremeSample(rng, kind, n);
                f.tapRight = extremeSample(rng, kind, n + 1);
                f.dryLeft = static_cast<float>(rng.range(-1.0, 1.0));
                f.dryRight = f.dryLeft;
                return f;
            },
            [&](std::int64_t n, float l, float r) {
                REQUIRE(rinne::dsp::isFiniteSample(l));
                REQUIRE(rinne::dsp::isFiniteSample(r));
                REQUIRE(std::fabs(l) < 1.0f);
                REQUIRE(std::fabs(r) < 1.0f);
                if (limiter && n >= settledAt)
                {
                    ++checkedAgainstCeiling;
                    if (std::fabs(l) > ceiling || std::fabs(r) > ceiling)
                        ++overCeiling;
                }
            });
    }
    CHECK(overCeiling == 0);
    CHECK(checkedAgainstCeiling > 1000000);
    CHECK(recoveries > 0); // 极端输入确实跳过闸，恢复后接着测
}

TEST_CASE("a million parameter jumps never make the chain diverge", "[safety][chain][bound]")
{
    Rng rng(99);
    OpenLoop loop;
    double maxAbs = 0.0;
    for (int b = 0; b < 1000000; ++b)
    {
        loop.controls.feedback = static_cast<float>(rng.range(-0.5, 2.0));
        loop.controls.returnGain = static_cast<float>(rng.range(-0.2, 1.2));
        loop.controls.limiterOn = rng.below(2) == 0;
        loop.controls.ceilingDb = static_cast<float>(rng.range(-40.0, 10.0));
        loop.controls.loopOn = rng.below(8) != 0;
        loop.controls.bypassed = rng.below(16) == 0;
        loop.controls.guardOn = rng.below(2) == 0;
        if (loop.chain.machine().latched() && !loop.chain.machine().coolingDown())
            ++loop.controls.recoverSeq;
        loop.block(
            1 + rng.below(8),
            [&](std::int64_t) {
                TapFrame f;
                f.tapLeft = static_cast<float>(rng.range(-2.0, 2.0));
                f.tapRight = static_cast<float>(rng.range(-2.0, 2.0));
                return f;
            },
            [&](std::int64_t, float l, float r) {
                REQUIRE(rinne::dsp::isFiniteSample(l));
                REQUIRE(rinne::dsp::isFiniteSample(r));
                maxAbs = std::fmax(maxAbs, std::fmax(std::fabs(l), std::fabs(r)));
            });
        REQUIRE(loop.chain.lastGain() <= 1.5f);
        REQUIRE(loop.chain.lastGain() >= 0.0f);
    }
    CHECK(maxAbs < 1.0);
}

TEST_CASE("the chain never emits denormals and returns to exact zero on silence", "[safety][chain][denormal]")
{
    OpenLoop loop;
    loop.controls.feedback = 1.0f;
    settle(loop);
    loop.block(static_cast<int>(samplesOf(0.5)), [](std::int64_t n) {
        TapFrame f;
        f.tapLeft = sine(30.0, kRate, n, 0.9);
        f.tapRight = f.tapLeft;
        return f;
    });
    bool sawSubnormal = false;
    float last = 1.0f;
    for (int b = 0; b < 300; ++b)
        loop.block(4800, silence, [&](std::int64_t, float l, float r) {
            sawSubnormal = sawSubnormal || isSubnormal(l) || isSubnormal(r);
            last = l;
        });
    CHECK_FALSE(sawSubnormal);
    CHECK(last == 0.0f);
    CHECK_FALSE(isSubnormal(loop.meters.returnDcMean.load()));
}

TEST_CASE("stages run in the fixed order sanitize, DC, gain, limiter, soft clip", "[safety][chain][order]")
{
    OpenLoop loop;
    loop.controls.feedback = 1.4f;
    settle(loop);
    REQUIRE(loop.chain.lastGain() == 1.4f);

    const auto n = static_cast<std::size_t>(samplesOf(1.0));
    std::vector<float> inL(n);
    std::vector<float> inR(n);
    Rng rng(4);
    for (std::size_t i = 0; i < n; ++i)
    {
        inL[i] = (i % 1000 == 0) ? std::numeric_limits<float>::quiet_NaN() : static_cast<float>(rng.range(-1.2, 1.2));
        inR[i] = (i % 777 == 0) ? 40.0f : static_cast<float>(rng.range(-0.4, 0.4));
    }
    std::vector<float> outL(n);
    std::vector<float> outR(n);
    std::size_t k = 0;
    const std::int64_t start = loop.n;
    loop.block(
        static_cast<int>(n),
        [&](std::int64_t i) {
            TapFrame f;
            f.tapLeft = inL[static_cast<std::size_t>(i - start)];
            f.tapRight = inR[static_cast<std::size_t>(i - start)];
            return f;
        },
        [&](std::int64_t, float l, float r) {
            outL[k] = l;
            outR[k] = r;
            ++k;
        });

    // 参考：同样的级各一份新状态（安全链此前只吃过 0，状态与新建的一样），按 ①②⑥⑦⑧ 手工串起来
    rinne::dsp::DcBlocker dcL;
    rinne::dsp::DcBlocker dcR;
    rinne::dsp::PeakLimiter lim;
    dcL.prepare(kRate);
    dcR.prepare(kRate);
    lim.prepare(kRate);
    const float ceiling = dbToGain(safety::kCeilingDefaultDb);
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < n; ++i)
    {
        float l = dcL.process(rinne::dsp::sanitizeSample(inL[i])) * 1.4f;
        float r = dcR.process(rinne::dsp::sanitizeSample(inR[i])) * 1.4f;
        lim.process(l, r, ceiling);
        l = rinne::dsp::softClip(l);
        r = rinne::dsp::softClip(r);
        if (floatBits(l) != floatBits(outL[i]) || floatBits(r) != floatBits(outR[i]))
            ++mismatches;
    }
    CHECK(mismatches == 0);
    CHECK_FALSE(loop.chain.machine().latched()); // 环里来的 NaN 只清洗，不跳闸
    CHECK(loop.meters.returnNonFinite.load() > 0u);
}

TEST_CASE("feedback is capped at 1.5 with the limiter on and 1.0 with it off", "[safety][chain][gain]")
{
    OpenLoop loop;
    loop.controls.feedback = 1.5f;
    settle(loop);
    CHECK(loop.chain.lastGain() == 1.5f);
    loop.controls.limiterOn = false;
    loop.block(static_cast<int>(samplesOf(0.03)), silence);
    CHECK(loop.chain.lastGain() == 1.0f);
    loop.controls.feedback = 0.8f;
    loop.controls.returnGain = 0.5f;
    loop.block(static_cast<int>(samplesOf(0.03)), silence);
    CHECK(loop.chain.lastGain() == 0.4f);
}

TEST_CASE("feedback changes ramp linearly over exactly 30 ms", "[safety][chain][gain]")
{
    OpenLoop loop;
    loop.controls.feedback = 0.2f;
    settle(loop);
    loop.controls.feedback = 1.0f;
    const std::int64_t ramp = samplesOf(0.03);
    std::vector<float> g;
    loop.block(static_cast<int>(ramp + 10), [&](std::int64_t) {
        g.push_back(loop.chain.lastGain());
        return TapFrame{};
    });
    g.push_back(loop.chain.lastGain());
    // g[i] 是第 i 个样本处理前的值，也就是上一个样本的增益
    CHECK(g[0] == 0.2f);
    CHECK(g[static_cast<std::size_t>(ramp - 1)] < 1.0f);
    CHECK(g[static_cast<std::size_t>(ramp)] == 1.0f);
    for (std::size_t i = 1; i <= static_cast<std::size_t>(ramp); ++i)
        REQUIRE(g[i] > g[i - 1]);
}

TEST_CASE("loop off and host bypass take the return to zero in 10 ms and back", "[safety][chain][gain]")
{
    for (const bool useBypass : {false, true})
    {
        OpenLoop loop;
        loop.controls.feedback = 1.0f;
        settle(loop);
        (useBypass ? loop.controls.bypassed : loop.controls.loopOn) = useBypass ? true : false;
        loop.block(479, silence);
        CHECK(loop.chain.lastGain() > 0.0f);
        loop.block(1, silence);
        CHECK(loop.chain.lastGain() == 0.0f);
        CHECK_FALSE(loop.chain.machine().latched()); // 只淡出，不锁存
        (useBypass ? loop.controls.bypassed : loop.controls.loopOn) = useBypass ? false : true;
        loop.block(480, silence);
        CHECK(loop.chain.lastGain() == 1.0f);
    }
}

TEST_CASE("switching the limiter crossfades over 10 ms", "[safety][chain][limiter]")
{
    OpenLoop loop;
    loop.controls.feedback = 1.0f;
    settle(loop);
    const auto loud = [](std::int64_t n) {
        TapFrame f;
        f.tapLeft = sine(1000.0, kRate, n, 0.68);
        f.tapRight = f.tapLeft;
        return f;
    };
    float peak = 0.0f;
    loop.block(4800, loud, [&](std::int64_t, float l, float) { peak = std::fmax(peak, std::fabs(l)); });
    CHECK(peak <= 0.5012f);
    loop.controls.limiterOn = false;
    std::vector<float> peaks;
    for (int i = 0; i < 4; ++i)
    {
        float p = 0.0f;
        loop.block(240, loud, [&](std::int64_t, float l, float) { p = std::fmax(p, std::fabs(l)); });
        peaks.push_back(p);
    }
    // 关掉之后 10 ms 内逐步放开，之后是不限制的 0.68 左右（低于软削波拐点，原样通过）
    CHECK(peaks[0] < peaks[1]);
    CHECK(peaks[1] < peaks[2]);
    CHECK(peaks[2] > 0.67f);
    CHECK(peaks[3] > 0.67f);
}

TEST_CASE("a non-finite value produced inside the Input trips immediately", "[safety][chain][nan]")
{
    for (const float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), 3.0e38f})
    {
        OpenLoop loop;
        loop.controls.feedback = 1.5f;
        settle(loop);
        loop.chain.beginBlock(loop.controls);
        float l = 0.2f;
        float r = 0.2f;
        loop.chain.processFront(l, r, true);
        // 音色级（③④⑤）在前后两段之间算出了非有限值（3e38 × 1.5 溢出成 +Inf）
        l = bad;
        loop.chain.processBack(l, r, 0.0f, 0.0f);
        CHECK(floatBits(l) == 0u);
        CHECK(floatBits(r) == 0u);
        CHECK(loop.chain.machine().state() == SafetyState::Tripped);
        CHECK(loop.chain.machine().tripReason() == TripReason::Nan);
    }
}

TEST_CASE("meters publish DC mean, near-full time, limiter reduction, guard and state", "[safety][chain][meters]")
{
    OpenLoop loop;
    loop.controls.feedback = 1.0f;
    settle(loop);
    // 直流 0.01 与 1 kHz 0.75 的正弦：×g 之后贴近满幅，限制器压约 3.6 dB
    loop.block(static_cast<int>(samplesOf(1.5)), [](std::int64_t n) {
        TapFrame f;
        f.tapLeft = 0.01f + sine(1000.0, kRate, n, 0.75);
        f.tapRight = 0.01f;
        return f;
    });
    CHECK(std::fabs(loop.meters.returnDcMean.load() - 0.01f) < 0.002f);
    CHECK(std::fabs(loop.meters.nearFullSeconds.load() - 1.5f) < 0.002f);
    const float gr = loop.meters.limiterReductionDb.take();
    CHECK(gr > 3.0f);
    CHECK(gr < 4.0f);
    CHECK(loop.meters.limiterReductionDb.take() == 0.0f); // take 之后清零
    CHECK(loop.meters.guardGainDb.load() == 0.0f);
    const std::uint32_t w = loop.meters.safety.load();
    CHECK(PackedSafety::state(w) == SafetyState::Run);
    // 回落之后：贴近满幅的时长在 10 ms 宽限后归零
    loop.block(960, silence);
    CHECK(loop.meters.nearFullSeconds.load() == 0.0f);
    // 跳闸后状态字带原因与冷却位
    ++loop.controls.panicSeq;
    loop.block(10, silence);
    const std::uint32_t t = loop.meters.safety.load();
    CHECK(PackedSafety::state(t) == SafetyState::Tripped);
    CHECK(PackedSafety::trip(t) == TripReason::Panic);
    CHECK(PackedSafety::coolingDown(t));
}

TEST_CASE("output and state transitions do not depend on how the host splits blocks", "[safety][chain][blocks]")
{
    // 一段会走过淡入、限制、失控保护进入、超大值跳闸的输入；参数全程不变
    const auto n = static_cast<std::size_t>(samplesOf(4.5));
    std::vector<float> tapL(n);
    std::vector<float> tapR(n);
    Rng rng(17);
    for (std::size_t i = 0; i < n; ++i)
    {
        const double t = static_cast<double>(i) / kRate;
        double v = 1.2 * std::sin(2.0 * kPi * 440.0 * t) + rng.range(-0.05, 0.05);
        if (t > 4.2)
            v = (i / 50) % 2 ? 5.0 : -5.0; // +14 dBFS，100 ms 后跳闸
        tapL[i] = i < 2000 ? 0.0f : static_cast<float>(v);
        tapR[i] = tapL[i] * 0.5f;
    }
    const auto runWith = [&](std::uint64_t seed, int fixed) {
        Recorded rec;
        OpenLoop loop;
        loop.controls.feedback = 1.2f;
        Rng split(seed);
        std::size_t done = 0;
        while (done < n)
        {
            const int len = fixed > 0 ? fixed : 1 + split.below(1500);
            const auto take = std::min<std::size_t>(static_cast<std::size_t>(len), n - done);
            loop.block(
                static_cast<int>(take),
                [&](std::int64_t i) {
                    TapFrame f;
                    f.tapLeft = tapL[static_cast<std::size_t>(i)];
                    f.tapRight = tapR[static_cast<std::size_t>(i)];
                    f.valid = i >= 1000;
                    return f;
                },
                [&](std::int64_t, float l, float r) {
                    rec.left.push_back(l);
                    rec.right.push_back(r);
                });
            const auto& ev = loop.chain.machine().events();
            REQUIRE(ev.dropped() == 0);
            for (int e = 0; e < ev.size(); ++e)
                rec.events.push_back(ev[e]);
            done += take;
        }
        REQUIRE(loop.chain.machine().tripReason() == TripReason::Huge);
        return rec;
    };
    const Recorded ref = runWith(0, 512);
    bool sawGuard = false;
    for (const auto& e : ref.events)
        sawGuard = sawGuard || e.code == SafetyEventCode::GuardEngaged;
    REQUIRE(sawGuard);
    for (const auto& variant : {std::pair<std::uint64_t, int>{0, 1}, {0, 7}, {0, 4096}, {1, 0}, {2, 0}, {3, 0}})
    {
        INFO("seed " << variant.first << " fixed " << variant.second);
        const Recorded got = runWith(variant.first, variant.second);
        REQUIRE(got.left.size() == ref.left.size());
        std::size_t diff = 0;
        for (std::size_t i = 0; i < n; ++i)
            diff += (floatBits(got.left[i]) != floatBits(ref.left[i]) ||
                     floatBits(got.right[i]) != floatBits(ref.right[i]));
        CHECK(diff == 0);
        REQUIRE(got.events.size() == ref.events.size());
        for (std::size_t e = 0; e < ref.events.size(); ++e)
        {
            CHECK(got.events[e].code == ref.events[e].code);
            CHECK(got.events[e].a == ref.events[e].a);
            CHECK(got.events[e].sample == ref.events[e].sample);
        }
    }
}

TEST_CASE("guard counts limiter reduction from 6 dB, not below", "[safety][chain][guard][threshold]")
{
    // 1 kHz 正弦在 48 kHz 下每周期有一个样本正好落在峰上；峰与峰之间释放 80 ms 只回升百分之几 dB，
    // 所以增益缩减几乎恒定在 20·log10(A / c)
    const float ceiling = dbToGain(safety::kCeilingDefaultDb);
    const auto engagedWith = [&](double reductionDb) {
        OpenLoop loop;
        loop.controls.feedback = 1.0f;
        settle(loop);
        const double amplitude = ceiling * std::pow(10.0, reductionDb / 20.0);
        bool engaged = false;
        for (int b = 0; b < 400; ++b)
            loop.block(480, [&](std::int64_t n) {
                TapFrame f;
                f.tapLeft = sine(1000.0, kRate, n, amplitude);
                f.tapRight = f.tapLeft;
                engaged = engaged || loop.chain.machine().guardEngaged();
                return f;
            });
        return engaged;
    };
    CHECK_FALSE(engagedWith(5.9));
    CHECK(engagedWith(6.1));
}

TEST_CASE("huge does not count while the return does not contribute", "[safety][chain][huge]")
{
    OpenLoop loop; // 反馈为 0
    settle(loop);
    loop.block(static_cast<int>(samplesOf(1.0)), [](std::int64_t n) {
        TapFrame f;
        f.tapLeft = (n / 40) % 2 ? 6.0f : -6.0f;
        f.tapRight = f.tapLeft;
        return f;
    });
    CHECK_FALSE(loop.chain.machine().latched());
    loop.controls.feedback = 0.1f;
    loop.block(static_cast<int>(samplesOf(0.2)), [](std::int64_t n) {
        TapFrame f;
        f.tapLeft = (n / 40) % 2 ? 6.0f : -6.0f;
        f.tapRight = f.tapLeft;
        return f;
    });
    CHECK(loop.chain.machine().tripReason() == TripReason::Huge);
}
