// SPDX-License-Identifier: GPL-3.0-or-later
//
// S25 环内 NaN 策略与 FADE_IN 入口无阶跃。
//
// 一、环内 NaN（环内插件吐出 NaN）：Output 清洗、只计数，Input 点琥珀灯，不跳闸。
//   环路延迟 100 ms、反馈 50 %、混合 50 %，中间链是「NaN 注入」。跑到 Run 之后，让中间链连续两个块（512 个样本）全吐
//   NaN，再关掉：
//   - Output 把它们清成 0 再写环，清洗计数等于被清洗的帧数（512），Input 看到的环头计数与之相同；
//   - Input 的输出始终有限，安全状态一直是 Run，跳闸计数为 0，没有锁存；
//   - 琥珀灯在清洗计数增加之后的 1 个块之内亮起，最后一次增加之后亮满 5 s（240000 个样本，误差 ≤ 2 个块）然后熄灭；
//   - 输出与参照安全链逐位相同，回送在 NaN 过去之后照常在响。
//
// 二、FADE_IN 入口复验：新配对、载入预设、宿主设置状态、换写方、跳闸后恢复，五个入口之后回送的包络都是
//   「从运行中进入先 10 ms 线性降到 0，再从首个有效读样本起 500 ms 线性升到 1」，相邻样本差没有阶跃。
//   信号是低频余弦（左 100 Hz，幅度 0.3），每个入口断言：
//   - 淡入原因对得上（引擎向安全链请求的原因逐个记下；安全链自己发起的恢复看打包状态里的原因）；
//   - 每个块末的安全增益与分段线性的期望值相差不超过 1e-5（下行 480 个样本，上行 24000 个样本，起点是首个有效读样本）；
//   - 淡完之后回到 Run，增益恰为 1；
//   - 回送（安全链的输出）相邻样本之差不超过「信号自身的最大斜率 + 10 ms 下行的一步 + ε」，同一段轨迹在入口处硬切到 0
//     （或不淡入直接接上读头）会超过这个界，证明判据对阶跃会变红；
//   - 输出与参照安全链逐位相同。
//
// 标 ⛔ 的是删除式验证点：把对应的实现改坏，本场景必须变红。
//   ⛔ 载入预设不请求淡入（直接落定）→ 预设入口的增益轨迹是平的，原因对不上；
//   ⛔ 环内 NaN 的清洗计数不接进琥珀灯 → 灯不亮；
//   ⛔ 换写方不请求淡入 → 换写方入口的增益轨迹是平的。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <set>
#include <vector>

#include "engine/SafetyEvents.h"
#include "s01_rig_kit.h"

namespace
{

using namespace rinne::rig;
using namespace rinne::rig::scn;
using rinne::engine::FadeReason;
using rinne::engine::PackedSafety;
using rinne::engine::SafetyState;

constexpr int kBlock = 256;
constexpr std::int64_t kSecond = 48000;
constexpr double kGainTolerance = 1.0e-5;

struct FadeLog
{
    std::vector<FadeReason> reasons;
    static void record(void* context, FadeReason reason) noexcept
    {
        static_cast<FadeLog*>(context)->reasons.push_back(reason);
    }
};

// 一条正在响的环路，逐个周期记下安全增益、琥珀灯与淡入原因
class Track
{
public:
    explicit Track(float delayMs = 100.0f)
    {
        host = makeHost(kSr, kBlock);
        REQUIRE(host != nullptr);
        host->setBlockScripts(fixedBlocks(kBlock), fixedBlocks(kBlock));
        host->input().setFadeObserver(&log, &FadeLog::record);
        rec = std::make_unique<Recorder>(*host, tones());
        params = loopParams(50.0f, 50.0f, delayMs);
    }

    void run(std::int64_t samples)
    {
        for (std::int64_t done = 0; done < samples;)
        {
            done += step();
        }
    }

    int step()
    {
        const int n = rec->cycle(cycles_++, params);
        gains.push_back(host->input().safetyChain().machine().safetyGain());
        ends.push_back(rec->outLeft.size());
        const std::uint32_t packed = host->input().meters().safety.load();
        if (PackedSafety::state(packed) == SafetyState::FadeIn)
        {
            fadeReasons.insert(PackedSafety::fade(packed));
        }
        lamp.push_back(host->inputSafetyDiag().loopNanLamp);
        return n;
    }

    std::size_t now() const { return rec->outLeft.size(); }
    SafetyState state() const { return static_cast<SafetyState>(host->inputSafetyDiag().state); }

    std::unique_ptr<EngineHost> host;
    std::unique_ptr<Recorder> rec;
    rinne::engine::EngineParams params;
    FadeLog log;
    std::vector<float> gains; // 每个块末的 g_safety
    std::vector<std::size_t> ends; // 每个块末已处理的样本数
    std::vector<bool> lamp; // 每个块末的琥珀灯
    std::set<FadeReason> fadeReasons; // 淡入期间见过的原因（打包状态里的）

private:
    std::size_t cycles_ = 0;
};

class LoopNanFadeInScenario final : public Scenario
{
public:
    const char* id() const noexcept override { return "s25_loop_nan_fade_in"; }

    bool run(ScenarioContext&) override
    {
        Verdict v;
        loopNan(v);
        fadeInEntries(v);
        return v.ok();
    }

private:
    static void loopNan(Verdict& v)
    {
        INFO("loop NaN");
        Track track;
        track.host->setChain(ChainKind::kNanInject);
        track.host->chain().setNanInjectAt(-1);
        track.run(2 * kSecond);
        RIG_EQ(v, static_cast<int>(track.state()), static_cast<int>(SafetyState::Run));
        RIG_CHECK(v, !track.lamp.back());
        RIG_EQ(v, track.host->outputNonFiniteCount(), 0u);

        // 连续两个块全吐 NaN
        const std::size_t burstAt = track.now();
        track.host->chain().setNanInjectAt(0);
        track.run(2 * kBlock);
        track.host->chain().setNanInjectAt(-1);
        const std::size_t burstEnd = track.now();
        RIG_EQ(v, burstEnd - burstAt, static_cast<std::size_t>(2 * kBlock));
        track.run(6 * kSecond);

        // Output 清洗并计数；Input 看到的环头计数相同
        RIG_EQ(v, track.host->outputNonFiniteCount(), 2u * static_cast<std::uint32_t>(kBlock));
        RIG_EQ(v, track.host->inputSafetyDiag().nonfiniteRing, track.host->outputNonFiniteCount());
        // 不跳闸、不锁存，Input 的输出始终有限
        RIG_EQ(v, track.host->inputSafetyDiag().trips, 0u);
        RIG_EQ(v, static_cast<int>(track.state()), static_cast<int>(SafetyState::Run));
        std::size_t nonFinite = 0;
        for (std::size_t i = 0; i < track.rec->outLeft.size(); ++i)
        {
            nonFinite += std::isfinite(track.rec->outLeft[i]) && std::isfinite(track.rec->outRight[i]) ? 0u : 1u;
        }
        RIG_EQ(v, nonFinite, std::size_t{0});

        // 琥珀灯：计数增加之后 1 个块之内亮起，最后一次增加之后亮满 5 s
        std::size_t litAt = 0;
        std::size_t unlitAt = 0;
        for (std::size_t k = 0; k < track.lamp.size(); ++k)
        {
            if (track.lamp[k] && litAt == 0)
            {
                litAt = track.ends[k];
            }
            if (litAt != 0 && !track.lamp[k] && unlitAt == 0)
            {
                unlitAt = track.ends[k];
            }
        }
        RIG_CHECK(v, litAt > burstAt && litAt <= burstAt + 2 * kBlock);
        constexpr std::size_t lampSamples = 240000; // 5 s
        static_assert(rinne::engine::kLoopNanLampSeconds == 5.0, "琥珀灯的时长是 5 s");
        // 最后一次增加出现在第二个 NaN 块之后的那个块首被 Input 看到：灯在 burstEnd + kBlock 起算的 5 s 里亮着
        RIG_CHECK(v, unlitAt >= burstEnd + lampSamples - 2 * kBlock && unlitAt <= burstEnd + lampSamples + 2 * kBlock);

        // 回送在 NaN 过去之后照常在响；输出与参照安全链逐位相同
        RIG_CHECK(v, !sameBits(track.rec->outLeft, track.rec->dryLeft, track.now() - kSecond, track.now()));
        RIG_CHECK(v, track.rec->matchesReference());
    }

    // 一个入口之后的包络：s 是入口生效的块首（样本编号），fromRun 表示入口之前回送在响（要先 10 ms 降到 0）
    static void fadeShape(Verdict& v, const Track& track, std::size_t s, bool fromRun)
    {
        const auto& valid = track.rec->reference.tapValid;
        const std::size_t downEnd = fromRun ? s + static_cast<std::size_t>(kGateSamples) : s;
        std::size_t firstValid = downEnd;
        while (firstValid < valid.size() && valid[firstValid] == 0)
        {
            ++firstValid;
        }
        RIG_CHECK(v, firstValid < valid.size());
        double worst = 0.0;
        for (std::size_t k = 0; k < track.gains.size(); ++k)
        {
            const std::size_t end = track.ends[k];
            if (end <= s)
            {
                continue;
            }
            double want = 0.0;
            if (fromRun && end <= downEnd)
            {
                want = 1.0 - static_cast<double>(end - s) / static_cast<double>(kGateSamples);
            }
            else if (end > firstValid)
            {
                want = std::min(1.0, static_cast<double>(end - firstValid) / static_cast<double>(kFadeInSamples));
            }
            worst = std::max(worst, std::fabs(static_cast<double>(track.gains[k]) - want));
        }
        INFO("worst gain deviation = " << worst);
        RIG_CHECK(v, worst <= kGainTolerance);
        RIG_EQ(v, static_cast<int>(track.state()), static_cast<int>(SafetyState::Run));
        RIG_EQ(v, track.gains.back(), 1.0f);

        // 回送包络无阶跃
        const auto& ret = track.rec->reference.retLeft;
        const std::size_t from = s > 4800 ? s - 4800 : 0;
        const std::size_t upEnd = firstValid + static_cast<std::size_t>(kFadeInSamples);
        const std::size_t to = std::min(ret.size(), upEnd + 4800);
        const float level = peakOf(ret, from, ret.size());
        RIG_CHECK(v, level > 0.05f);
        const float bound = slope(100.0, level) + level / static_cast<float>(kGateSamples) + 1.0e-6f;
        RIG_CHECK(v, rinne::test::maxStep(ret, from, to) <= bound);
        // 反例：入口处硬切到 0，或不淡入直接接上读头，都会超过这个界
        if (fromRun)
        {
            std::size_t loudest = s - static_cast<std::size_t>(kGateSamples);
            for (std::size_t t = loudest; t < s; ++t)
            {
                loudest = std::fabs(ret[t]) > std::fabs(ret[loudest]) ? t : loudest;
            }
            RIG_CHECK(v, rinne::test::maxStep(hardCut(ret, loudest + 1), from, to) > bound);
        }
        else
        {
            const auto& tap = track.rec->reference.tapLeft;
            std::size_t jump = firstValid + 960;
            while (jump < tap.size() && std::fabs(tap[jump]) < 0.15f)
            {
                ++jump;
            }
            RIG_CHECK(v, jump < tap.size());
            std::vector<float> hard = ret;
            for (std::size_t t = jump; t < hard.size(); ++t)
            {
                hard[t] = 0.5f * tap[t];
            }
            RIG_CHECK(v, rinne::test::maxStep(hard, from, to) > bound);
        }
        RIG_CHECK(v, track.rec->matchesReference());
    }

    static void fadeInEntries(Verdict& v)
    {
        {
            INFO("new pairing");
            Track track;
            track.run(2 * kSecond);
            fadeShape(v, track, 0, false);
            RIG_CHECK(v, track.fadeReasons.size() == 1u);
            RIG_CHECK(v,
                      track.fadeReasons.count(FadeReason::FirstPair) + track.fadeReasons.count(FadeReason::Load) == 1u);
        }
        {
            INFO("preset");
            Track track;
            track.run(2 * kSecond);
            track.fadeReasons.clear();
            track.log.reasons.clear();
            const std::size_t s = track.now();
            track.params.presetSeq = 1;
            track.run(2 * kSecond);
            fadeShape(v, track, s, true);
            RIG_CHECK(v, track.log.reasons == std::vector<FadeReason>{FadeReason::Preset});
            RIG_CHECK(v, track.fadeReasons == std::set<FadeReason>{FadeReason::Preset});
        }
        {
            INFO("host set state");
            Track track;
            track.run(2 * kSecond);
            track.fadeReasons.clear();
            track.log.reasons.clear();
            const std::size_t s = track.now();
            track.params.stateSeq = 1;
            track.run(2 * kSecond);
            fadeShape(v, track, s, true);
            RIG_CHECK(v, track.log.reasons == std::vector<FadeReason>{FadeReason::SetState});
            RIG_CHECK(v, track.fadeReasons == std::set<FadeReason>{FadeReason::SetState});
        }
        {
            INFO("new writer");
            Track track;
            RIG_CHECK(v, track.host->addSecondOutput());
            track.run(2 * kSecond);
            track.fadeReasons.clear();
            track.log.reasons.clear();
            const std::size_t s = track.now();
            track.host->setSecondOutputActive(true);
            track.host->switchWriter(true);
            track.run(2 * kSecond);
            fadeShape(v, track, s, true);
            RIG_CHECK(v, track.log.reasons == std::vector<FadeReason>{FadeReason::NewWriter});
            RIG_CHECK(v, track.fadeReasons == std::set<FadeReason>{FadeReason::NewWriter});
        }
        {
            INFO("recover after a trip");
            Track track;
            track.run(2 * kSecond);
            track.params.panicSeq = 1;
            track.run(2 * kSecond); // 跳闸，冷却走完
            RIG_EQ(v, static_cast<int>(track.state()), static_cast<int>(SafetyState::Tripped));
            track.fadeReasons.clear();
            track.log.reasons.clear();
            const std::size_t s = track.now();
            track.params.recoverSeq = 1;
            track.run(2 * kSecond);
            // 跳闸时增益已经是 0，恢复没有下行；读头被清空，新数据要等 D 才到，首个有效读样本在那之后
            fadeShape(v, track, s, false);
            RIG_CHECK(v, track.fadeReasons == std::set<FadeReason>{FadeReason::Recover});
        }
    }
};

} // namespace

RINNE_RIG_SCENARIO("s25_loop_nan_fade_in")
{
    return new LoopNanFadeInScenario();
}

TEST_CASE("s25: loop NaN never trips, and every fade-in entry ramps down then up without a step", "[loop_rig][s25]")
{
    CHECK(rinne::rig::scn::runRegisteredScenario("s25_loop_nan_fade_in"));
}
