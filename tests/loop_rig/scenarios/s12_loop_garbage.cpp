// SPDX-License-Identifier: GPL-3.0-or-later
//
// S12 环内坏值突发（core 级）：环内插件吐出 NaN、±Inf、1e6，Output 写环前清洗掉，Input 的输出始终有限，
// 环内 NaN 的计数看得见，不跳闸（环内插件产生的坏值由 Output 清洗，不跳闸；Input 内部产生的才跳）。
//
// 环路延迟 100 ms、反馈 50 %、混合 50 %，信号是两个声道各一个低频余弦。跑到 Run 之后依次来三种突发，每种连续两个块
// （512 个样本），之间隔 1 s：
//   NaN   中间链「NaN 注入」；
//   ±Inf  中间链增益 = +∞（正半周 +Inf、负半周 −Inf，恰为 0 的样本得 NaN）；
//   1e6   中间链增益 = 1e6（有限的超大值）。
// 断言：
//   - Output 的清洗计数恰为 Output 看到的含非有限值的帧数，钳位计数恰为两个声道都有限、有一个 |v| > 16 的帧数；
//     Input 从环头读到的两个计数与之相同（计数看得见）；
//   - 从环里读回来的读头全程有限、|tap| ≤ 16；1e6 突发在 D 之后读回来的正是钳过的 ±16（清洗后的值照常进环）；
//   - Input 的输出全程有限；琥珀灯在 NaN 突发之后亮起；
//   - 不跳闸：跳闸计数 0、状态一直是 Run；回送 |ret| ≤ 上限；输出与参照安全链逐位相同。
// 对照：同样的 1e6 突发持续 150 ms → TRIPPED(huge)：钳过的 ±16 仍会被超大值判据接住，上面不跳闸不是因为判据失灵。
//
// 标 ⛔ 的是删除式验证点：
//   ⛔ Output 写环前不清洗（原样写进环）→ 计数为 0、读头里出现非有限值与超过 16 的值。

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "engine/SafetyEvents.h"
#include "s01_rig_kit.h"

namespace
{

using namespace rinne::rig;
using namespace rinne::rig::scn;
using rinne::engine::SafetyState;
using rinne::engine::TripReason;

constexpr int kBlock = 256;
constexpr std::int64_t kSecond = 48000;
constexpr std::size_t kDelaySamples = 4800; // 100 ms

enum class Burst
{
    kNan,
    kInf,
    kHuge,
};

const char* burstName(Burst b)
{
    switch (b)
    {
    case Burst::kNan:
        return "NaN";
    case Burst::kInf:
        return "Inf";
    case Burst::kHuge:
        return "1e6";
    }
    return "?";
}

class Track
{
public:
    Track()
    {
        host = makeHost(kSr, kBlock);
        REQUIRE(host != nullptr);
        host->setBlockScripts(fixedBlocks(kBlock), fixedBlocks(kBlock));
        rec = std::make_unique<Recorder>(*host, tones());
        params = loopParams(50.0f, 50.0f, 100.0f);
    }

    // 周期没处理样本时 Recorder::cycle 已经报了失败，这里立刻停下，不空转到超时
    void run(std::int64_t samples)
    {
        for (std::int64_t done = 0; done < samples;)
        {
            const int n = rec->cycle(cycles_++, params);
            lamp.push_back(host->inputSafetyDiag().loopNanLamp);
            ends.push_back(rec->outLeft.size());
            if (n <= 0)
            {
                return;
            }
            done += n;
        }
    }

    // 中间链吐一段坏值，再回到恒等
    void burst(Burst kind, std::int64_t samples)
    {
        switch (kind)
        {
        case Burst::kNan:
            host->setChain(ChainKind::kNanInject);
            host->chain().setNanInjectAt(0);
            break;
        case Burst::kInf:
            host->setChain(ChainKind::kGain);
            host->chain().setGain(std::numeric_limits<float>::infinity());
            break;
        case Burst::kHuge:
            host->setChain(ChainKind::kGain);
            host->chain().setGain(1.0e6f);
            break;
        }
        run(samples);
        host->chain().setNanInjectAt(-1);
        host->chain().setGain(1.0f);
        host->setChain(ChainKind::kIdentity);
    }

    std::size_t now() const { return rec->outLeft.size(); }
    SafetyState state() const { return static_cast<SafetyState>(host->inputSafetyDiag().state); }

    // [from, to) 里 Output 看到的含非有限值的帧数，与两个声道都有限、有一个 |v| > 16 的帧数
    std::uint32_t nonFiniteFrames(std::size_t from, std::size_t to) const
    {
        std::uint32_t count = 0;
        for (std::size_t i = from; i < to; ++i)
        {
            count += std::isfinite(rec->fedLeft[i]) && std::isfinite(rec->fedRight[i]) ? 0u : 1u;
        }
        return count;
    }

    std::uint32_t clampFrames(std::size_t from, std::size_t to) const
    {
        std::uint32_t count = 0;
        for (std::size_t i = from; i < to; ++i)
        {
            const float l = rec->fedLeft[i];
            const float r = rec->fedRight[i];
            count += std::isfinite(l) && std::isfinite(r) && (std::fabs(l) > 16.0f || std::fabs(r) > 16.0f) ? 1u : 0u;
        }
        return count;
    }

    std::unique_ptr<EngineHost> host;
    std::unique_ptr<Recorder> rec;
    rinne::engine::EngineParams params;
    std::vector<bool> lamp; // 每个块末的琥珀灯
    std::vector<std::size_t> ends; // 每个块末已处理的样本数

private:
    std::size_t cycles_ = 0;
};

class LoopGarbageScenario final : public Scenario
{
public:
    const char* id() const noexcept override { return "s12_loop_garbage"; }

    bool run(ScenarioContext&) override
    {
        Verdict v;
        bursts(v);
        hugeContrast(v);
        return v.ok();
    }

private:
    static void bursts(Verdict& v)
    {
        Track track;
        track.run(2 * kSecond);
        RIG_EQ(v, static_cast<int>(track.state()), static_cast<int>(SafetyState::Run));
        RIG_EQ(v, track.host->outputNonFiniteCount(), 0u);
        RIG_EQ(v, track.host->outputClampCount(), 0u);

        std::uint32_t wantNonFinite = 0;
        std::uint32_t wantClamps = 0;
        std::size_t hugeAt = 0;
        for (const Burst kind : {Burst::kNan, Burst::kInf, Burst::kHuge})
        {
            INFO("burst " << burstName(kind));
            const std::size_t at = track.now();
            track.burst(kind, 2 * kBlock);
            const std::size_t end = track.now();
            RIG_EQ(v, end - at, static_cast<std::size_t>(2 * kBlock));
            const std::uint32_t nonFinite = track.nonFiniteFrames(at, end);
            const std::uint32_t clamps = track.clampFrames(at, end);
            // NaN 与 ±Inf 突发里每一帧都是坏的；1e6 突发里每一帧都有限、都超过 16
            RIG_EQ(v, nonFinite, kind == Burst::kHuge ? 0u : 2u * static_cast<std::uint32_t>(kBlock));
            RIG_EQ(v, clamps, kind == Burst::kHuge ? 2u * static_cast<std::uint32_t>(kBlock) : 0u);
            wantNonFinite += nonFinite;
            wantClamps += clamps;
            RIG_EQ(v, track.host->outputNonFiniteCount(), wantNonFinite);
            RIG_EQ(v, track.host->outputClampCount(), wantClamps);
            if (kind == Burst::kNan)
            {
                // 琥珀灯在清洗计数增加之后的 2 个块之内亮起
                track.run(2 * kBlock);
                RIG_CHECK(v, track.lamp.back());
            }
            if (kind == Burst::kHuge)
            {
                hugeAt = at;
            }
            track.run(kSecond);
        }
        track.run(kSecond);

        // Input 从环头读到的计数与 Output 的相同
        const rinne::engine::SafetyDiag diag = track.host->inputSafetyDiag();
        RIG_EQ(v, diag.nonfiniteRing, track.host->outputNonFiniteCount());
        RIG_EQ(v, diag.clampRing, track.host->outputClampCount());

        // 读回来的全程有限、≤ 16；1e6 突发在 D 之后读回来的是钳过的 ±16
        const auto& tapLeft = track.rec->reference.tapLeft;
        const auto& tapRight = track.rec->reference.tapRight;
        std::size_t badTaps = 0;
        for (std::size_t i = 0; i < tapLeft.size(); ++i)
        {
            const bool ok = std::isfinite(tapLeft[i]) && std::isfinite(tapRight[i]) && std::fabs(tapLeft[i]) <= 16.0f &&
                            std::fabs(tapRight[i]) <= 16.0f;
            badTaps += ok ? 0u : 1u;
        }
        RIG_EQ(v, badTaps, std::size_t{0});
        const std::size_t echo = hugeAt + kDelaySamples;
        RIG_EQ(v, peakOf(tapLeft, echo, echo + 2 * kBlock), 16.0f);
        RIG_EQ(v, peakOf(tapRight, echo, echo + 2 * kBlock), 16.0f);

        // Input 的输出全程有限
        std::size_t nonFiniteOut = 0;
        for (std::size_t i = 0; i < track.rec->outLeft.size(); ++i)
        {
            nonFiniteOut += std::isfinite(track.rec->outLeft[i]) && std::isfinite(track.rec->outRight[i]) ? 0u : 1u;
        }
        RIG_EQ(v, nonFiniteOut, std::size_t{0});

        // 不跳闸，回送不超过上限，接线逐位对得上
        RIG_EQ(v, diag.trips, 0u);
        RIG_EQ(v, static_cast<int>(track.state()), static_cast<int>(SafetyState::Run));
        const float ceiling = static_cast<float>(std::pow(10.0, -6.0 / 20.0));
        RIG_CHECK(v, peakOf(track.rec->reference.retLeft, 0, track.now()) <= ceiling);
        RIG_CHECK(v, peakOf(track.rec->reference.retRight, 0, track.now()) <= ceiling);
        RIG_CHECK(v, track.rec->matchesReference());
    }

    static void hugeContrast(Verdict& v)
    {
        INFO("1e6 burst held for 150 ms");
        Track track;
        track.run(2 * kSecond);
        RIG_EQ(v, static_cast<int>(track.state()), static_cast<int>(SafetyState::Run));
        track.burst(Burst::kHuge, 7200);
        track.run(kSecond);
        RIG_EQ(v, track.host->inputSafetyDiag().trips, 1u);
        RIG_EQ(v, static_cast<int>(track.state()), static_cast<int>(SafetyState::Tripped));
        RIG_EQ(v, static_cast<int>(track.host->input().safetyChain().machine().tripReason()),
               static_cast<int>(TripReason::Huge));
        RIG_EQ(v, track.host->outputNonFiniteCount(), 0u);
        RIG_CHECK(v, track.rec->matchesReference());
    }
};

} // namespace

RINNE_RIG_SCENARIO("s12_loop_garbage")
{
    return new LoopGarbageScenario();
}

TEST_CASE("s12: NaN, Inf and 1e6 bursts from the loop are cleaned by Output, counted, and never trip",
          "[loop_rig][s12]")
{
    CHECK(rinne::rig::scn::runRegisteredScenario("s12_loop_garbage"));
}
