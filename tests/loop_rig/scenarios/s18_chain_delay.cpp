// SPDX-License-Identifier: GPL-3.0-or-later
//
// S18 中间链有延迟：实测环路延迟 = D + 链延迟。
// 环内插件的延迟引擎不补偿也不报，D 仍是用户设的 D，回声整体晚到链延迟那么多。
//
// 两条腿：
//   恒定 D     中间链延迟 L ∈ {0, 64, 480} 个样本，D = 4800，反馈 50 %、混合 50 %。冲激的第 k 个回声的峰位恰为
//              冲激位置 + k·(D + L)（k = 1…4，左右声道各查）；峰之前整段（含冲激位置 + D）逐位是 0，说明回声没有早到；
//              第一个回声逐位是 0.25f；D_eff 恒为 4800，零重同步，漂移为 0；输出与参照安全链逐位相同。
//   改 D       链延迟 64。D 从 100 ms 改到 110 ms，改动落在块首，交叉淡化 1920 个样本（40 ms）。冲激放得让旧读头与
//              新读头读到它的两个时刻都落在交叉淡化里：旧读头在起点后第 800 个样本，新读头在第 1280 个。
//              两个回声的幅度恰为 0.25 × 对应时刻的线性增益：旧 1 − 800/1920，新 1280/1920（等功率交叉会是 0.79 与
//              0.80，差得很远）；交叉淡化的起点恰在改动的那一块首；走完之后 D_eff = 5280，之后的冲激回声的峰位
//              恰为 冲激位置 + k·(5280 + 64)，旧间隔 D1 + 64 的位置上没有回声。
//
// 标 ⛔ 的是删除式验证点：把对应的实现改坏，本场景必须变红。
//   ⛔ 读位置多或少一个样本 → 峰位不在 k·(D + L)；
//   ⛔ 交叉淡化改成等功率，或总长不是 1920，或起点晚一块 → 两个回声的幅度不再是线性增益；
//   ⛔ 改 D 之后新读头的 D 没生效（仍读旧 D）→ 之后的峰位不在 k·(新 D + 64)。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "diag/EventQueue.h"
#include "loop/LoopEvents.h"
#include "s01_rig_kit.h"

namespace
{

using namespace rinne::rig;
using namespace rinne::rig::scn;
using rinne::engine::SafetyState;

constexpr int kBlock = 256;
constexpr std::int64_t kDelay = 4800;
constexpr std::int64_t kDelayAfter = 5280; // 110 ms
constexpr std::int64_t kChainLatency = 64;
constexpr std::int64_t kCrossfade = 1920; // 40 ms
constexpr std::int64_t kEchoes = 4;
// 首个有效读样本在 D，淡入再走 500 ms：冲激放在淡入走完之后
constexpr std::uint64_t kImpulseAt = 30100;

// 两个冲激：绝对编号 first 与 second 处为 amplitude（左右都是）
SourceFn twoImpulses(std::uint64_t first, std::uint64_t second, float amplitude)
{
    return [=](std::uint64_t start, float* left, float* right, int n) {
        for (int i = 0; i < n; ++i)
        {
            const std::uint64_t at = start + static_cast<std::uint64_t>(i);
            const float v = at == first || at == second ? amplitude : 0.0f;
            left[i] = v;
            right[i] = v;
        }
    };
}

// 窗口 [center - half, center + half) 里 |x| 最大的下标
std::size_t peakNear(const std::vector<float>& trace, std::int64_t center, std::int64_t half)
{
    return argmaxAbs(trace, static_cast<std::size_t>(center - half), static_cast<std::size_t>(center + half));
}

class ChainDelayScenario final : public Scenario
{
public:
    const char* id() const noexcept override { return "s18_chain_delay"; }

    bool run(ScenarioContext&) override
    {
        Verdict v;
        for (const std::int64_t latency : {std::int64_t{0}, kChainLatency, std::int64_t{480}})
        {
            constantLeg(v, latency);
        }
        changingLeg(v);
        return v.ok();
    }

private:
    void constantLeg(Verdict& v, std::int64_t latency)
    {
        INFO("chain latency = " << latency);
        auto host = makeHost(kSr, kBlock);
        REQUIRE(host != nullptr);
        host->setBlockScripts(fixedBlocks(kBlock), fixedBlocks(kBlock));
        host->setChain(ChainKind::kDelay);
        host->chain().setDelaySamples(static_cast<int>(latency));
        RIG_EQ(v, host->chain().totalLatency(), latency);
        Recorder rec(*host, impulseSource(kImpulseAt, 0.5f));
        const rinne::engine::EngineParams p = loopParams(50.0f, 50.0f, 100.0f);

        const auto at = static_cast<std::int64_t>(kImpulseAt);
        const std::int64_t period = kDelay + latency; // 实测的环路延迟
        rec.run(p, at - 100);
        RIG_EQ(v, host->inputSafetyDiag().state, static_cast<std::uint8_t>(SafetyState::Run));
        rec.run(p, (kEchoes + 1) * period + 100);
        RIG_CHECK(v, static_cast<std::int64_t>(rec.outLeft.size()) > at + (kEchoes + 1) * period);
        if (static_cast<std::int64_t>(rec.outLeft.size()) <= at + (kEchoes + 1) * period)
        {
            return;
        }

        // 回声没有早到：冲激之后到第一个回声之前，整段逐位是 0（链延迟为 0 时第一个回声就在 at + D）
        RIG_EQ(v, rec.outLeft[static_cast<std::size_t>(at)], 0.5f);
        RIG_CHECK(v, allZero(rec.outLeft, static_cast<std::size_t>(at) + 1, static_cast<std::size_t>(at + period)));
        RIG_CHECK(v, allZero(rec.outRight, static_cast<std::size_t>(at) + 1, static_cast<std::size_t>(at + period)));
        // 峰位：k = 1…4，窗口是以 k·(D + L) 为中心的一整个周期
        for (std::int64_t k = 1; k <= kEchoes; ++k)
        {
            INFO("k = " << k);
            RIG_EQ(v, peakNear(rec.outLeft, at + k * period, period / 2), static_cast<std::size_t>(at + k * period));
            RIG_EQ(v, peakNear(rec.outRight, at + k * period, period / 2), static_cast<std::size_t>(at + k * period));
        }
        // 第一个回声逐位是 0.25f（线性区）
        RIG_EQ(v, rinne::test::floatBits(rec.outLeft[static_cast<std::size_t>(at + period)]),
               rinne::test::floatBits(0.25f));

        // D 仍是用户设的 D（链延迟另计）；零重同步，漂移为 0；输出与参照安全链逐位相同
        const auto& status = host->inputStatus();
        RIG_EQ(v, status.delayEffective, kDelay);
        RIG_EQ(v, status.resyncCount, 0u);
        RIG_EQ(v, status.drift, std::int64_t{0});
        RIG_CHECK(v, rec.matchesReference());
    }

    void changingLeg(Verdict& v)
    {
        auto host = makeHost(kSr, kBlock);
        REQUIRE(host != nullptr);
        host->setBlockScripts(fixedBlocks(kBlock), fixedBlocks(kBlock));
        host->setChain(ChainKind::kDelay);
        host->chain().setDelaySamples(static_cast<int>(kChainLatency));

        // 改动落在第 160 块的块首。冲激的位置反推：旧读头在 起点 + 800 读到它（环里它在 impulse + 链延迟），
        // 新读头晚 D2 − D1 = 480 个样本，在 起点 + 1280，两者都在 1920 的交叉淡化里
        const std::int64_t change = 160 * kBlock;
        const std::int64_t oldStep = 800;
        const std::int64_t newStep = oldStep + (kDelayAfter - kDelay);
        const auto first = static_cast<std::uint64_t>(change + oldStep - kChainLatency - kDelay);
        const auto second = std::uint64_t{50000}; // 交叉淡化走完之后
        Recorder rec(*host, twoImpulses(first, second, 0.5f));
        const rinne::engine::EngineParams before = loopParams(50.0f, 50.0f, 100.0f);
        const rinne::engine::EngineParams after = loopParams(50.0f, 50.0f, 110.0f);

        rec.run(before, change);
        RIG_EQ(v, host->inputSafetyDiag().state, static_cast<std::uint8_t>(SafetyState::Run));
        RIG_EQ(v, host->inputStatus().delayEffective, kDelay);
        rec.run(after, kBlock);
        // 交叉淡化的第一块：已经报新的 D，还剩 1920 − 256 个样本
        RIG_EQ(v, host->inputStatus().delayEffective, kDelayAfter);
        RIG_EQ(v, host->inputStatus().delayTarget, kDelayAfter);
        RIG_EQ(v, host->inputStatus().delayXfadeLeft, kCrossfade - kBlock);
        rec.run(after, 70000 - change - kBlock);

        // 交叉淡化的起点恰在改动的那一块首，只有这一次
        std::vector<std::int64_t> starts;
        host->drainEvents([&](const rinne::diag::Event& e) {
            if (e.code == static_cast<std::uint16_t>(rinne::loop::LoopEventCode::kResync) &&
                e.a == static_cast<std::uint8_t>(rinne::loop::ResyncReason::kDChange))
            {
                starts.push_back(e.sample);
            }
        });
        RIG_EQ(v, starts.size(), std::size_t{1});
        RIG_CHECK(v, !starts.empty() && starts[0] == change);

        // 两个回声的幅度恰为 0.25 × 线性增益
        const auto oldAt = static_cast<std::size_t>(change + oldStep);
        const auto newAt = static_cast<std::size_t>(change + newStep);
        const float oldGain = 1.0f - static_cast<float>(oldStep) / static_cast<float>(kCrossfade);
        const float newGain = static_cast<float>(newStep) / static_cast<float>(kCrossfade);
        RIG_CHECK(v, std::fabs(rec.outLeft[oldAt] - 0.25f * oldGain) < 3.0e-4f);
        RIG_CHECK(v, std::fabs(rec.outLeft[newAt] - 0.25f * newGain) < 3.0e-4f);
        RIG_CHECK(v, std::fabs(rec.outRight[oldAt] - 0.25f * oldGain) < 3.0e-4f);
        RIG_CHECK(v, std::fabs(rec.outRight[newAt] - 0.25f * newGain) < 3.0e-4f);
        // 对照：等功率交叉时这两个值是 0.25 × 0.79 与 0.25 × 0.80，离线性值很远，3e-4 的界分得开
        const float equalPowerOld =
            0.25f * std::cos(1.5707963f * (static_cast<float>(oldStep) / static_cast<float>(kCrossfade)));
        RIG_CHECK(v, std::fabs(equalPowerOld - 0.25f * oldGain) > 10.0f * 3.0e-4f);
        // 不早到：冲激之后到旧读头的回声之前整段逐位是 0；交叉淡化窗口里的峰是新读头的回声
        RIG_EQ(v, rec.outLeft[static_cast<std::size_t>(first)], 0.5f);
        RIG_CHECK(v, allZero(rec.outLeft, static_cast<std::size_t>(first) + 1, oldAt));
        RIG_EQ(v, peakNear(rec.outLeft, change + kCrossfade / 2, kCrossfade / 2), newAt);

        // 走完之后的冲激：回声间隔是 D2 + 64
        const std::int64_t period = kDelayAfter + kChainLatency;
        const auto tb = static_cast<std::int64_t>(second);
        for (std::int64_t k = 1; k <= 3; ++k)
        {
            INFO("k = " << k);
            RIG_EQ(v, peakNear(rec.outLeft, tb + k * period, period / 2), static_cast<std::size_t>(tb + k * period));
            RIG_EQ(v, peakNear(rec.outRight, tb + k * period, period / 2), static_cast<std::size_t>(tb + k * period));
        }
        RIG_CHECK(v, std::fabs(rec.outLeft[static_cast<std::size_t>(tb + period)] - 0.25f) < 5.0e-4f);
        // 旧间隔 D1 + 64 的位置上没有回声（只有前一个冲激留下的小尾巴）
        RIG_CHECK(v, std::fabs(rec.outLeft[static_cast<std::size_t>(tb + kDelay + kChainLatency)]) < 0.1f);

        const auto& status = host->inputStatus();
        RIG_EQ(v, status.delayEffective, kDelayAfter);
        RIG_EQ(v, status.delayXfadeLeft, std::int64_t{0});
        RIG_EQ(v, status.resyncCount, 1u);
        RIG_EQ(v, status.resyncByReason[static_cast<std::size_t>(rinne::loop::SyncReason::kDChange)], 1u);
        RIG_CHECK(v, rec.matchesReference());
    }
};

} // namespace

RINNE_RIG_SCENARIO("s18_chain_delay")
{
    return new ChainDelayScenario();
}

TEST_CASE("s18: with a delaying plugin in the loop the measured loop delay is D plus its latency", "[loop_rig][s18]")
{
    CHECK(rinne::rig::scn::runRegisteredScenario("s18_chain_delay"));
}
