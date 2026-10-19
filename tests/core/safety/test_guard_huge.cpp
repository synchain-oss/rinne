// SPDX-License-Identifier: GPL-3.0-or-later
//
// 失控保护与超大值的每个阈值，都在边界两侧各走一次（48 kHz，1 s = 48000 样本）：
//   失控保护：3 s 窗内 GR ≥ 6 dB 累计 2 s 进入（限制器关时软削波占比 50 %，即 1.5 s）；−6 dB/s 下降；
//            平静 3 s 后 +3 dB/s 回升；压到 −24 dB 后仍命中 1 s → 跳闸（失控）；guard_on = 0 不介入。
//   超大值：电平 ≥ +12 dBFS 持续 100 ms → 跳闸；两个超限样本相隔 ≤ 50 ms 算连续（10 Hz 的半周期）。
// 这里直接给状态机「命中」与电平，防误报门（反馈 ≤ 90 % 且链增益 ≤ 0 dB）在闭环测试里验。

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>

#include "SafetyTestKit.h"

using namespace rinne::test;

namespace
{

constexpr std::int64_t k1s = 48000;
constexpr std::int64_t k2s = 2 * k1s;
constexpr std::int64_t k3s = 3 * k1s;
constexpr std::int64_t k100ms = 4800;
constexpr std::int64_t k10ms = 480;

MachineRig runningRig()
{
    MachineRig rig;
    rig.toRun();
    return rig;
}

} // namespace

TEST_CASE("guard engages after exactly 2 s of limiting within 3 s", "[safety][guard][threshold]")
{
    MachineRig rig = runningRig();
    rig.run(k2s - 1, true, hotGuard());
    CHECK_FALSE(rig.m.guardEngaged());
    CHECK(rig.m.state() == SafetyState::Run);
    rig.step(true, hotGuard());
    CHECK(rig.m.guardEngaged());
    CHECK(rig.m.state() == SafetyState::Guard);
}

TEST_CASE("guard counts limiting time cumulatively inside the 3 s window", "[safety][guard][threshold]")
{
    {
        // 1 s 命中、1 s 平静、1 s 命中：最后一个样本时窗内正好 2 s
        MachineRig rig = runningRig();
        rig.run(k1s, true, hotGuard());
        rig.run(k1s, true);
        rig.run(k1s - 1, true, hotGuard());
        CHECK_FALSE(rig.m.guardEngaged());
        rig.step(true, hotGuard());
        CHECK(rig.m.guardEngaged());
    }
    {
        // 平静段拉长到 1.05 s：第一段有 50 ms 以上落出 3 s 窗，后一段命中 1 s 时窗内不到 2 s
        MachineRig rig = runningRig();
        rig.run(k1s, true, hotGuard());
        rig.run(k1s + samplesOf(0.05), true);
        rig.run(k1s, true, hotGuard());
        CHECK_FALSE(rig.m.guardEngaged());
    }
}

TEST_CASE("with the limiter off guard engages at 50 % soft-clip time", "[safety][guard][threshold]")
{
    MachineRig rig = runningRig();
    rig.c.limiterOn = false;
    rig.begin();
    rig.run(samplesOf(1.5) - 1, true, hotGuard());
    CHECK_FALSE(rig.m.guardEngaged());
    rig.step(true, hotGuard());
    CHECK(rig.m.guardEngaged());
}

TEST_CASE("guard falls at exactly 6 dB/s and holds once the criterion is gone", "[safety][guard][rate]")
{
    MachineRig rig = runningRig();
    rig.run(k2s - 1, true, hotGuard());
    rig.run(k1s, true, hotGuard()); // 进入那一样本起，整整 1 s 在降
    CHECK(rig.m.guardGainDb() == -6.0);
    CHECK(std::fabs(rig.m.guardGain() - 0.501187f) < 1.0e-5f);
    rig.run(k1s, true, hotGuard());
    CHECK(rig.m.guardGainDb() == -12.0);
    // 命中停下：窗里还有 ≥ 2 s 时继续降，窗里不足 2 s 后保持
    rig.run(k1s + k10ms * 2, true);
    const double held = rig.m.guardGainDb();
    CHECK(held < -12.0);
    rig.run(k1s, true);
    CHECK(rig.m.guardGainDb() == held);
}

TEST_CASE("guard waits for 3 s of calm, then rises at exactly 3 dB/s back to RUN", "[safety][guard][rate]")
{
    MachineRig rig = runningRig();
    rig.run(k2s - 1, true, hotGuard());
    rig.run(k1s, true, hotGuard()); // −6 dB
    // 平静头 1 s 窗口里还有残留的命中，继续降；之后保持，直到平静满 3 s 才回升
    rig.run(k2s, true);
    const double held = rig.m.guardGainDb();
    rig.run(k1s - 1, true);
    const double before = rig.m.guardGainDb();
    CHECK(before == held);
    rig.step(true);
    const double first = rig.m.guardGainDb();
    CHECK(first > before);
    CHECK(std::fabs((first - before) - 3.0 / 48000.0) < 1.0e-12);
    rig.run(k1s - 1, true);
    CHECK(std::fabs(rig.m.guardGainDb() - (before + 3.0)) < 1.0e-9);
    rig.run(samplesOf(5.0), true);
    CHECK(rig.m.guardGainDb() == 0.0);
    CHECK(rig.m.guardGain() == 1.0f);
    CHECK_FALSE(rig.m.guardEngaged());
    CHECK(rig.m.state() == SafetyState::Run);
}

TEST_CASE("a hit during the rise resets the calm timer", "[safety][guard][rate]")
{
    MachineRig rig = runningRig();
    rig.run(k2s, true, hotGuard());
    rig.run(k3s + k1s / 2, true); // 已在回升
    const double rising = rig.m.guardGainDb();
    rig.step(true, hotGuard());
    rig.run(k1s, true);
    CHECK(rig.m.guardGainDb() <= rising + 1.0e-9); // 平静计时被打断，回升停住
}

TEST_CASE("guard floors at -24 dB and trips as runaway after 1 s more of hits there", "[safety][guard][threshold]")
{
    // 持续命中：2 s 进入（进入那一样本就降第一步），再 4 s 降到 −24 dB，在底部再命中 1 s 跳闸
    const std::int64_t engage = k2s - 1;
    const std::int64_t floorAt = engage + 4 * k1s - 1;
    const std::int64_t tripAt = floorAt + k1s - 1;
    {
        MachineRig rig = runningRig();
        rig.run(floorAt + 1, true, hotGuard());
        CHECK(rig.m.guardGainDb() == -24.0);
        rig.run(tripAt - floorAt - 1, true, hotGuard());
        CHECK(rig.m.guardGainDb() == -24.0);
        CHECK_FALSE(rig.m.latched());
        rig.step(true, hotGuard());
        CHECK(rig.m.latched());
        CHECK(rig.m.tripReason() == TripReason::Runaway);
    }
    {
        // 底部只命中 1 s 减 1 个样本就停：不跳闸，平静后回升
        MachineRig rig = runningRig();
        rig.run(tripAt, true, hotGuard());
        rig.run(samplesOf(20.0), true);
        CHECK_FALSE(rig.m.latched());
        CHECK(rig.m.state() == SafetyState::Run);
    }
}

TEST_CASE("guard_on off never intervenes", "[safety][guard][off]")
{
    MachineRig rig = runningRig();
    rig.c.guardOn = false;
    rig.begin();
    rig.run(samplesOf(10.0), true, hotGuard());
    CHECK_FALSE(rig.m.guardEngaged());
    CHECK(rig.m.guardGain() == 1.0f);
    CHECK_FALSE(rig.m.latched());
}

TEST_CASE("switching guard off while engaged releases at 3 dB/s without waiting for calm", "[safety][guard][off]")
{
    MachineRig rig = runningRig();
    rig.run(k2s - 1 + k1s, true, hotGuard()); // −6 dB
    rig.c.guardOn = false;
    rig.begin();
    rig.run(k1s, true, hotGuard());
    CHECK(std::fabs(rig.m.guardGainDb() + 3.0) < 1.0e-9);
    rig.run(k1s, true, hotGuard());
    CHECK(rig.m.guardGainDb() == 0.0);
    CHECK(rig.m.state() == SafetyState::Run);
}

TEST_CASE("recover clears the guard at once", "[safety][guard]")
{
    MachineRig rig = runningRig();
    rig.run(k2s + k1s, true, hotGuard());
    REQUIRE(rig.m.guardGainDb() < -5.0);
    rig.panic();
    rig.run(3 * k1s / 2);
    rig.recover();
    // 恢复的那一刻（还没走下一个样本）失控保护就已清零
    CHECK(rig.m.guardGainDb() == 0.0);
    CHECK_FALSE(rig.m.guardEngaged());
    CHECK(rig.m.state() == SafetyState::FadeIn);
}

TEST_CASE("a fresh pairing clears the guard once the return is at zero", "[safety][guard]")
{
    MachineRig rig = runningRig();
    rig.run(k2s + k1s, true, hotGuard());
    REQUIRE(rig.m.guardGainDb() < -5.0);
    rig.m.requestFadeIn(FadeReason::NewWriter);
    rig.run(k10ms, true);
    CHECK(rig.lastGain == 0.0f);
    rig.step(true);
    CHECK(rig.m.guardGainDb() == 0.0);
}

TEST_CASE("huge trips after exactly 100 ms at +12 dBFS", "[safety][huge][threshold]")
{
    {
        MachineRig rig = runningRig();
        rig.run(k100ms - 1, true, hugeAt(rinne::engine::safety::kHugeLevel));
        CHECK_FALSE(rig.m.latched());
        rig.step(true, hugeAt(rinne::engine::safety::kHugeLevel));
        CHECK(rig.m.latched());
        CHECK(rig.m.tripReason() == TripReason::Huge);
    }
    {
        // 100 ms 减 1 个样本后回落：不跳闸
        MachineRig rig = runningRig();
        rig.run(k100ms - 1, true, hugeAt(8.0f));
        rig.run(k1s, true, hugeAt(0.5f));
        CHECK_FALSE(rig.m.latched());
    }
    {
        // 刚好低于 +12 dBFS：一直不跳
        MachineRig rig = runningRig();
        rig.run(k1s, true, hugeAt(std::nextafter(rinne::engine::safety::kHugeLevel, 0.0f)));
        CHECK_FALSE(rig.m.latched());
    }
    CHECK(std::fabs(20.0 * std::log10(static_cast<double>(rinne::engine::safety::kHugeLevel)) - 12.0) < 1.0e-6);
    CHECK(std::fabs(20.0 * std::log10(static_cast<double>(rinne::engine::safety::kHugeSentMargin)) - 0.5) < 1.0e-6);
}

TEST_CASE("huge treats hits up to 50 ms apart as continuous", "[safety][huge][threshold]")
{
    const auto spaced = [](std::int64_t spacing) {
        MachineRig rig = runningRig();
        for (std::int64_t i = 0; i < 2 * k100ms; ++i)
            rig.step(true, hugeAt(i % spacing == 0 ? 5.0f : 0.1f));
        return rig.m.latched();
    };
    CHECK(spaced(5 * k10ms));
    CHECK_FALSE(spaced(5 * k10ms + 1));
}

TEST_CASE("huge only counts while the return contributes", "[safety][huge]")
{
    MachineRig rig = runningRig();
    SafetyObservation o = hugeAt(10.0f);
    o.contributing = false;
    rig.run(k1s, true, o);
    CHECK_FALSE(rig.m.latched());
    // 中途掉成不贡献，计数从头来
    rig.run(k100ms / 2, true, hugeAt(10.0f));
    rig.step(true, o);
    rig.run(k100ms - 1, true, hugeAt(10.0f));
    CHECK_FALSE(rig.m.latched());
    rig.step(true, hugeAt(10.0f));
    CHECK(rig.m.latched());
}
