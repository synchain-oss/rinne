// SPDX-License-Identifier: GPL-3.0-or-later
//
// S14 改 D、D 下限抬升、交叉淡化进行中重同步（core 级）：读头的相邻样本跳变不超过推导出的阈值，读头槽不溢出。
//
// 反馈 0、混合 50 %：环里就是延迟了的干声，读头（环路层混好的回送，安全链的输入，与 core 环路层用例同一个观测点）
// 读的信号已知。干声是两个声道各一个低频余弦（左 100 Hz、右 150 Hz，幅度 A = 0.3）。Input 先 Output 后。
//
// 阈值：读头 y = Σ_k e_k·v(t − D_k)，e_k 是各读头的包络（交叉淡化、淡出、onset 的乘积）。相邻差
//   |Δy| ≤ (Σ_k e_k)·S + A·Σ_k |Δe_k|，S = 信号自身最大斜率（余弦 A·2πf/sr），A = 峰值。
// 各段包络每步的变化：40 ms 交叉淡化两路各 1/1920；重同步淡出 5 ms 1/240；onset 20 ms 1/960；宿主违约当场抬 D 下限时
// 旧读头在剩下的 D_old 个样本里淡到 0，1/D_old。于是（各加 1e-6 容浮点）：
//   改 D、按声明值抬 D 下限    B = S + 2A/1920                       （= 交叉淡化最大斜率 × 2 + 信号自身最大斜率）
//   宿主违约当场抬 D 下限       B = S + A·(2/1920 + 1/D_old)
//   交叉淡化中重同步            B = 2S + A·(2/1920 + 1/240 + 1/960)  （旧的一组读头 Σe ≤ 1，新读头 onset ≤ 1）
// core 环路层的同类用例（test_loop_session 的「改 D」三条）在斜坡编码上用的是同一推导的特例：斜坡上 |v1 − v2| =
// ΔD·斜率， 所以那里写成 斜率·(1 + ΔD/1920)；违约一条把 Σe 放宽到 2。两边口径一致，不互相矛盾。
// 每一路都构造反例证明判据会红：同一段轨迹在跳变点直接换到新位置（或直接落到 0）会超过 B。
//
// 五路：
//   ① 改 D：25 → 50 ms；② 改 D：50 → 12.5 ms；
//   ③ D 下限抬升（宿主违约）：两端声明 512，Output 以 2048 重新 prepare，此后宿主送 2048 帧的块：Input 判违约，
//      D 下限当场抬到 2048（原因：宿主违约），D 从 1200 变成 2048；
//   ④ D 下限抬升（按声明值）：Input 以 2048 重新 prepare，块长不变，D 下限按声明值抬到 2048，走 40 ms 交叉淡化；
//   ⑤ 交叉淡化进行中重同步：块长 128，D 12.5 → 25 ms 之后两块，宿主跳过 Input 8 块（1024 帧，小于
//   N_cap），回来的第二块位移重同步，
//      落在 40 ms 交叉淡化里（两个正在交叉的旧读头一起淡出，新读头 onset：读头槽里同时有 3 个）。
// 每一路：每块 tapsActive ≤ 3，结束时 tapDrop = 0；改 D 的交叉淡化恰 1 次；交叉淡化走完之后读的正是新 D（逐位）。
//
// 标 ⛔ 的是删除式验证点：
//   ⛔ 改 D 不走交叉淡化（直接改主读头的 D）→ 读头一步跳 |v(t − D_old) − v(t − D_new)|，①②③④ 变红。

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "loop/LoopStatus.h"
#include "loop/LoopTiming.h"
#include "loop/StreamSync.h"
#include "s01_rig_kit.h"
#include "s14_tap_leg.h"
#include "s14_tap_trace.h"

namespace
{

using namespace rinne::rig;
using namespace rinne::rig::scn;
using namespace rinne::s14;
using rinne::loop::DelayMinReason;
using rinne::loop::SyncReason;

TapLeg timeLeg(int block, float delayMs)
{
    return TapLeg(block, loopParams(0.0f, 50.0f, delayMs));
}

class DelayChangeScenario final : public Scenario
{
public:
    const char* id() const noexcept override { return "s14_delay_change"; }

    bool run(ScenarioContext&) override
    {
        Verdict v;
        userChange(v, 25.0f, 1200, 50.0f, 2400);
        userChange(v, 50.0f, 2400, 12.5f, 600);
        violationLift(v);
        declaredLift(v);
        resyncInCrossfade(v);
        return v.ok();
    }

private:
    static void userChange(Verdict& v, float fromMs, std::int64_t fromD, float toMs, std::int64_t toD)
    {
        INFO("D " << fromMs << " -> " << toMs << " ms");
        TapLeg leg = timeLeg(256, fromMs);
        leg.run(2 * kSecond);
        RIG_EQ(v, leg.status().delayEffective, fromD);
        const std::size_t mark = leg.now();
        leg.setParams(loopParams(0.0f, 50.0f, toMs));
        leg.run(kSecond);
        checkTransition(v, leg, mark, fromD, toD, boundOf(1.0f, 2.0 / kXfade));
    }

    static void violationLift(Verdict& v)
    {
        INFO("D floor raised by a host violation after Output re-prepared with 2048");
        TapLeg leg = timeLeg(512, 25.0f);
        leg.run(2 * kSecond);
        RIG_EQ(v, leg.status().delayEffective, std::int64_t{1200});
        const std::size_t mark = leg.now();
        leg.host->prepareOutput(kSr, 2048);
        leg.setBlocks(2048);
        leg.run(kSecond);
        // 2048 帧的块装得下整段过渡，块末快照只看得到 1 个读头（槽数仍逐块断言 ≤ 3）
        checkTransition(v, leg, mark, 1200, 2048, boundOf(1.0f, 2.0 / kXfade + 1.0 / 1200.0), 1);
        RIG_EQ(v, leg.status().delayMin, std::int64_t{2048});
        RIG_EQ(v, leg.status().delayMinReason, static_cast<std::uint8_t>(DelayMinReason::kHostViolation));
        RIG_EQ(v, leg.status().overrun, std::uint64_t{0});
    }

    static void declaredLift(Verdict& v)
    {
        INFO("D floor raised by Input re-preparing with 2048");
        TapLeg leg = timeLeg(512, 25.0f);
        leg.run(2 * kSecond);
        const std::size_t mark = leg.now();
        leg.host->prepareInput(kSr, 2048);
        leg.run(kSecond);
        checkTransition(v, leg, mark, 1200, 2048, boundOf(1.0f, 2.0 / kXfade));
        RIG_EQ(v, leg.status().delayMin, std::int64_t{2048});
        RIG_EQ(v, leg.status().delayMinReason, static_cast<std::uint8_t>(DelayMinReason::kPrepare));
    }

    static void resyncInCrossfade(Verdict& v)
    {
        INFO("shift resync while the D crossfade is running");
        // 块长 128：重同步那一块的块末，旧读头的 5 ms 淡出（240 个样本）还没走完，快照里看得到 3 个读头
        constexpr int block = 128;
        constexpr int skipped = 8; // 1024 帧，小于 N_cap
        TapLeg leg = timeLeg(block, 12.5f);
        leg.run(2 * kSecond);
        const std::size_t mark = leg.now();
        leg.setParams(loopParams(0.0f, 50.0f, 25.0f));
        // 改 D 之后两块，跳过 Input 8 块，之后照常
        std::vector<bool> skips(leg.cycle() + 2 + skipped, false);
        for (std::size_t i = leg.cycle() + 2; i < skips.size(); ++i)
        {
            skips[i] = true;
        }
        leg.setBlocks(block, skips);
        for (int i = 0; i < 2 + skipped; ++i)
        {
            leg.step();
        }
        RIG_EQ(v, leg.now(), mark + 2 * block);
        leg.run(kSecond);
        const std::size_t end = leg.now();

        const rinne::loop::LoopStatusFields& s = leg.status();
        RIG_EQ(v, s.resyncByReason[reasonIndex(SyncReason::kShift)], 1u);
        RIG_EQ(v, s.resyncByReason[reasonIndex(SyncReason::kDChange)], 1u);
        // 回来的第二块位移重同步，落在交叉淡化里
        RIG_EQ(v, leg.shiftAt, static_cast<std::int64_t>(mark) + 3 * block);
        RIG_CHECK(v, leg.xfadeLeftBeforeShift > 0);
        RIG_CHECK(v, leg.shiftAt - static_cast<std::int64_t>(mark) < kXfade);
        // 读头槽：两个正在交叉的旧读头 + 重同步的新读头，同时 3 个，不溢出
        RIG_EQ(v, leg.maxTapsActive, kMaxTaps);
        RIG_CHECK(v, leg.tapsWithinSlots);
        RIG_EQ(v, s.tapDrop, 0u);

        const Bound bound = boundOf(2.0f, 2.0 / kXfade + 1.0 / kFadeOut + 1.0 / kOnset);
        const float stepLeft = maxStep(leg.tap.left, mark - 1, end);
        const float stepRight = maxStep(leg.tap.right, mark - 1, end);
        INFO("max step " << stepLeft << " / " << stepRight << ", bound " << bound.left << " / " << bound.right);
        RIG_CHECK(v, stepLeft <= bound.left);
        RIG_CHECK(v, stepRight <= bound.right);
        // 反例：重同步时旧读头不淡出、直接落到 0
        const auto at = static_cast<std::size_t>(leg.shiftAt);
        const std::size_t cutLeft = argmaxAbs(leg.tap.left, at, at + static_cast<std::size_t>(kFadeOut));
        const std::size_t cutRight = argmaxAbs(leg.tap.right, at, at + static_cast<std::size_t>(kFadeOut));
        RIG_CHECK(v, maxStep(cutAt(leg.tap.left, cutLeft + 1), mark - 1, end) > bound.left);
        RIG_CHECK(v, maxStep(cutAt(leg.tap.right, cutRight + 1), mark - 1, end) > bound.right);
    }
};

} // namespace

RINNE_RIG_SCENARIO("s14_delay_change")
{
    return new DelayChangeScenario();
}

TEST_CASE("s14: D changes, D floor lifts and a resync inside the crossfade never step the tap, and the tap slots hold",
          "[loop_rig][s14]")
{
    CHECK(rinne::rig::scn::runRegisteredScenario("s14_delay_change"));
}
