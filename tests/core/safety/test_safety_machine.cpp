// SPDX-License-Identifier: GPL-3.0-or-later
//
// 安全状态机：淡入从首个有效读样本起 500 ms；从运行中进入淡入先 10 ms 降到 0（不得阶跃）；
// 跳闸淡出后清空并锁存，预设、setState、重新配对、重 prepare 都清不掉，只有冷却 1.5 s 之后的恢复能清；
// Panic 连点不恢复、不重放；最后用一张转移表把每个状态的每条出边走一遍。

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "SafetyTestKit.h"

using namespace rinne::test;

namespace
{

constexpr std::int64_t k10ms = 480;
constexpr std::int64_t k500ms = 24000;
constexpr std::int64_t kCooldown = 72000;

} // namespace

TEST_CASE("a new instance stays silent until paired, then fades in over 500 ms from the first valid sample",
          "[safety][machine][fadein]")
{
    MachineRig rig(kRate, false);
    CHECK(rig.m.state() == SafetyState::Unpaired);
    rig.run(1000, true);
    CHECK(rig.lastGain == 0.0f);

    rig.c.paired = true;
    rig.begin();
    CHECK(rig.m.state() == SafetyState::FadeIn);
    CHECK(rig.m.fadeReason() == FadeReason::Load);
    // 读到的还不是有效数据：保持 0，淡入不开始计时
    rig.run(3000, false);
    CHECK(rig.lastGain == 0.0f);
    CHECK(rig.m.state() == SafetyState::FadeIn);

    for (std::int64_t k = 0; k < k500ms; ++k)
    {
        rig.step(true);
        REQUIRE(rig.lastGain == static_cast<float>(static_cast<double>(k + 1) / k500ms));
        if (k < k500ms - 1)
            REQUIRE(rig.m.state() == SafetyState::FadeIn);
    }
    CHECK(rig.lastGain == 1.0f);
    CHECK(rig.m.state() == SafetyState::Run);
}

TEST_CASE("entering a fade-in from RUN ramps down over 10 ms before the 500 ms ramp, without a step",
          "[safety][machine][fadein][nostep]")
{
    for (const FadeReason reason : {FadeReason::Preset, FadeReason::SetState, FadeReason::NewWriter})
    {
        INFO("reason " << static_cast<int>(reason));
        MachineRig rig;
        rig.toRun();
        REQUIRE(rig.m.state() == SafetyState::Run);
        rig.m.requestFadeIn(reason);
        CHECK(rig.m.state() == SafetyState::FadeIn);
        CHECK(rig.m.fadeReason() == reason);

        std::vector<float> g;
        for (std::int64_t i = 0; i < k10ms + k500ms + 10; ++i)
        {
            rig.step(true);
            g.push_back(rig.lastGain);
        }
        // 下行正好 10 ms 到 0，紧接着从首个有效读样本起 500 ms 回到 1
        CHECK(g[static_cast<std::size_t>(k10ms - 2)] > 0.0f);
        CHECK(g[static_cast<std::size_t>(k10ms - 1)] == 0.0f);
        CHECK(g[static_cast<std::size_t>(k10ms)] == static_cast<float>(1.0 / k500ms));
        CHECK(g[static_cast<std::size_t>(k10ms + k500ms - 1)] == 1.0f);
        CHECK(rig.m.state() == SafetyState::Run);
        // 相邻样本之间的增益变化不超过 10 ms 斜坡的一步（加 float 量化）
        float maxJump = 1.0f - g[0];
        for (std::size_t i = 1; i < g.size(); ++i)
            maxJump = std::fmax(maxJump, std::fabs(g[i] - g[i - 1]));
        CHECK(maxJump <= 1.0f / static_cast<float>(k10ms) + 1.0e-6f);
    }
}

TEST_CASE("a fade-in after the ramp-down still waits for the first valid read sample", "[safety][machine][fadein]")
{
    MachineRig rig;
    rig.toRun();
    rig.m.requestFadeIn(FadeReason::NewWriter);
    rig.run(k10ms, true);
    CHECK(rig.lastGain == 0.0f);
    rig.run(5000, false); // 换写方后新数据还没到
    CHECK(rig.lastGain == 0.0f);
    CHECK(rig.m.state() == SafetyState::FadeIn);
    rig.step(true);
    CHECK(rig.lastGain == static_cast<float>(1.0 / k500ms));
}

TEST_CASE("a second fade-in request mid-ramp ramps down from where it is", "[safety][machine][fadein][nostep]")
{
    MachineRig rig;
    rig.toRun();
    rig.m.requestFadeIn(FadeReason::Preset);
    rig.run(k10ms + 12000, true); // 上行到一半
    const float mid = rig.lastGain;
    CHECK(mid == 0.5f);
    rig.m.requestFadeIn(FadeReason::SetState);
    float prev = mid;
    for (std::int64_t i = 0; i < k10ms; ++i)
    {
        rig.step(true);
        REQUIRE(rig.lastGain < prev);
        REQUIRE(prev - rig.lastGain <= 1.0f / static_cast<float>(k10ms) + 1.0e-6f);
        prev = rig.lastGain;
    }
    CHECK(rig.lastGain == 0.0f);
}

TEST_CASE("losing the pairing fades out in 10 ms; re-pairing fades in as first_pair", "[safety][machine][pairing]")
{
    MachineRig rig;
    rig.toRun();
    rig.c.paired = false;
    rig.begin();
    CHECK(rig.m.state() == SafetyState::Unpaired);
    rig.run(k10ms - 1, false);
    CHECK(rig.lastGain > 0.0f);
    rig.step(false);
    CHECK(rig.lastGain == 0.0f);
    rig.run(10000, false);
    rig.c.paired = true;
    rig.begin();
    CHECK(rig.m.state() == SafetyState::FadeIn);
    CHECK(rig.m.fadeReason() == FadeReason::FirstPair);
    rig.run(k500ms, true);
    CHECK(rig.lastGain == 1.0f);
    CHECK(rig.m.state() == SafetyState::Run);
}

TEST_CASE("a stall is reported without touching the safety gain", "[safety][machine][stall]")
{
    MachineRig rig;
    rig.toRun();
    rig.c.stalled = true;
    rig.begin();
    CHECK(rig.m.state() == SafetyState::Stalled);
    rig.run(10000, false);
    CHECK(rig.lastGain == 1.0f);
    rig.c.stalled = false;
    rig.begin();
    CHECK(rig.m.state() == SafetyState::Run);
}

TEST_CASE("panic fades out in 10 ms, asks for one flush, and stays latched", "[safety][machine][trip][latch]")
{
    MachineRig rig;
    rig.toRun();
    rig.panic();
    CHECK(rig.m.state() == SafetyState::Tripped);
    CHECK(rig.m.tripReason() == TripReason::Panic);
    rig.run(k10ms - 1);
    CHECK(rig.lastGain > 0.0f);
    CHECK_FALSE(rig.m.takeFlushRequest());
    rig.step();
    CHECK(rig.lastGain == 0.0f);
    CHECK(rig.m.takeFlushRequest());
    CHECK_FALSE(rig.m.takeFlushRequest());

    // 预设、setState、换写方都不解除；失去又恢复配对不解除；同采样率与换采样率的重 prepare 都不解除
    rig.m.requestFadeIn(FadeReason::Preset);
    rig.m.requestFadeIn(FadeReason::SetState);
    rig.m.requestFadeIn(FadeReason::NewWriter);
    rig.run(1000);
    CHECK(rig.m.state() == SafetyState::Tripped);
    rig.c.paired = false;
    rig.begin();
    CHECK(rig.m.state() == SafetyState::Tripped);
    rig.c.paired = true;
    rig.begin();
    rig.run(1000);
    CHECK(rig.m.state() == SafetyState::Tripped);
    rig.m.prepare(kRate);
    rig.run(10);
    CHECK(rig.m.state() == SafetyState::Tripped);
    rig.m.prepare(96000.0);
    rig.begin();
    rig.run(10);
    CHECK(rig.m.state() == SafetyState::Tripped);
    CHECK(rig.lastGain == 0.0f);
    CHECK_FALSE(rig.m.takeFlushRequest()); // 已经清过一次，不重复请求
    CHECK(rig.m.tripReason() == TripReason::Panic);
}

TEST_CASE("recover is refused during the 1.5 s cooldown and never queued", "[safety][machine][recover]")
{
    MachineRig rig;
    rig.toRun();
    rig.panic();
    rig.run(kCooldown - 1);
    CHECK(rig.m.coolingDown());
    rig.recover();
    CHECK(rig.m.state() == SafetyState::Tripped);
    CHECK(rig.sawEvent(SafetyEventCode::RecoverIgnored) == false); // 事件在下一块开头才收走
    rig.run(1);
    rig.begin();
    CHECK(rig.sawEvent(SafetyEventCode::RecoverIgnored));
    // 冷却已过，但被拒的那次恢复没有排队：不会自己恢复
    CHECK_FALSE(rig.m.coolingDown());
    rig.run(50000);
    CHECK(rig.m.state() == SafetyState::Tripped);
    // 冷却过后再点一次：恢复，从首个有效读样本起 500 ms 淡入
    rig.recover();
    CHECK(rig.m.state() == SafetyState::FadeIn);
    CHECK(rig.m.fadeReason() == FadeReason::Recover);
    rig.run(100, false);
    CHECK(rig.lastGain == 0.0f);
    rig.run(k500ms, true);
    CHECK(rig.lastGain == 1.0f);
    CHECK(rig.m.state() == SafetyState::Run);
}

TEST_CASE("recover is accepted at exactly 1.5 s after the cut", "[safety][machine][recover]")
{
    MachineRig rig;
    rig.toRun();
    rig.panic();
    rig.run(kCooldown);
    CHECK_FALSE(rig.m.coolingDown());
    rig.recover();
    CHECK(rig.m.state() == SafetyState::FadeIn);
}

TEST_CASE("pressing panic again while cut does not recover and restarts the cooldown", "[safety][machine][recover]")
{
    MachineRig rig;
    rig.toRun();
    rig.panic();
    rig.run(50000);
    rig.panic();
    CHECK(rig.m.state() == SafetyState::Tripped);
    rig.run(kCooldown - 50000);
    rig.recover(); // 第一次切断后的 1.5 s 到了，但第二次切断重新计了冷却
    CHECK(rig.m.state() == SafetyState::Tripped);
    rig.run(50000);
    rig.recover();
    CHECK(rig.m.state() == SafetyState::FadeIn);
}

TEST_CASE("command numbers already present on the first block are not replayed", "[safety][machine][recover]")
{
    SafetyMachine m;
    m.prepare(kRate);
    SafetyControls c;
    c.paired = true;
    c.panicSeq = 7;
    c.recoverSeq = 3;
    m.beginBlock(c);
    CHECK(m.state() == SafetyState::FadeIn);
    CHECK_FALSE(m.latched());
}

TEST_CASE("the play-edge reset completes load and first_pair fade-ins only", "[safety][machine][playedge]")
{
    {
        MachineRig rig;
        rig.run(100, true);
        REQUIRE(rig.m.fadeReason() == FadeReason::Load);
        rig.m.playEdgeReset();
        CHECK(rig.m.state() == SafetyState::Run);
        rig.step();
        CHECK(rig.lastGain == 1.0f);
    }
    for (const FadeReason reason : {FadeReason::Preset, FadeReason::SetState, FadeReason::NewWriter})
    {
        MachineRig rig;
        rig.toRun();
        rig.m.requestFadeIn(reason);
        rig.run(k10ms + 100, true);
        rig.m.playEdgeReset();
        CHECK(rig.m.state() == SafetyState::FadeIn);
        rig.step();
        CHECK(rig.lastGain < 0.01f);
    }
}

TEST_CASE("a same-rate prepare keeps the stream; a rate change starts a new fade-in", "[safety][machine][prepare]")
{
    MachineRig rig;
    rig.toRun();
    rig.m.prepare(kRate);
    rig.step();
    CHECK(rig.lastGain == 1.0f);
    CHECK(rig.m.state() == SafetyState::Run);
    rig.m.prepare(96000.0);
    rig.begin();
    CHECK(rig.m.state() == SafetyState::FadeIn);
    CHECK(rig.m.fadeReason() == FadeReason::Load);
    rig.step(false);
    CHECK(rig.lastGain == 0.0f);
}

TEST_CASE("an unprepared machine gives zero gain", "[safety][machine][prepare]")
{
    SafetyMachine m;
    SafetyControls c;
    c.paired = true;
    m.beginBlock(c);
    CHECK(m.advance(true).safety == 0.0f);
}

// ---------------------------------------------------------------- 转移表

namespace
{

struct Transition
{
    const char* name;
    std::function<void(MachineRig&)> enter; // 把机器带到起点状态
    SafetyState from;
    std::function<void(MachineRig&)> event;
    SafetyState to;
};

void enterUnpaired(MachineRig& r)
{
    r.c.paired = false;
    r.begin();
    r.run(1000, false);
}

void enterFadeIn(MachineRig& r)
{
    r.run(100, true);
}

void enterRun(MachineRig& r)
{
    r.toRun();
}

void enterStalled(MachineRig& r)
{
    r.toRun();
    r.c.stalled = true;
    r.begin();
}

void enterGuard(MachineRig& r)
{
    r.toRun();
    r.run(samplesOf(2.0), true, hotGuard());
}

void enterTripped(MachineRig& r)
{
    r.toRun();
    r.panic();
    r.run(1000);
}

void pair(MachineRig& r)
{
    r.c.paired = true;
    r.begin();
}

void unpair(MachineRig& r)
{
    r.c.paired = false;
    r.begin();
}

void stall(MachineRig& r)
{
    r.c.stalled = true;
    r.begin();
}

void unstall(MachineRig& r)
{
    r.c.stalled = false;
    r.begin();
}

void preset(MachineRig& r)
{
    r.m.requestFadeIn(FadeReason::Preset);
}

void panicNow(MachineRig& r)
{
    r.panic();
}

void huge(MachineRig& r)
{
    r.run(samplesOf(0.1), true, hugeAt(4.0f));
}

void nanInside(MachineRig& r)
{
    SafetyObservation o;
    o.internalNonFinite = true;
    r.step(true, o);
}

void finishFade(MachineRig& r)
{
    r.run(k500ms, true);
}

void guardRunaway(MachineRig& r)
{
    r.run(samplesOf(5.0), true, hotGuard());
}

void guardCalm(MachineRig& r)
{
    // 平静 3 s 后以 +3 dB/s 回升，压低的量回到 0 dB 就回到运行
    r.run(samplesOf(12.0), true);
}

void recoverAfterCooldown(MachineRig& r)
{
    r.run(kCooldown);
    r.recover();
}

void recoverEarly(MachineRig& r)
{
    r.recover();
}

void guardOff(MachineRig& r)
{
    r.c.guardOn = false;
    r.begin();
    r.run(samplesOf(12.0), true);
}

} // namespace

TEST_CASE("every listed transition of the safety state machine", "[safety][machine][table]")
{
    using S = SafetyState;
    const std::vector<Transition> table = {
        {"unpaired + pair", enterUnpaired, S::Unpaired, pair, S::FadeIn},
        {"unpaired + preset", enterUnpaired, S::Unpaired, preset, S::Unpaired},
        {"unpaired + panic", enterUnpaired, S::Unpaired, panicNow, S::Tripped},
        {"unpaired + nan", enterUnpaired, S::Unpaired, nanInside, S::Tripped},
        {"fadein + done", enterFadeIn, S::FadeIn, finishFade, S::Run},
        {"fadein + unpair", enterFadeIn, S::FadeIn, unpair, S::Unpaired},
        {"fadein + stall", enterFadeIn, S::FadeIn, stall, S::Stalled},
        {"fadein + preset", enterFadeIn, S::FadeIn, preset, S::FadeIn},
        {"fadein + panic", enterFadeIn, S::FadeIn, panicNow, S::Tripped},
        {"fadein + huge", enterFadeIn, S::FadeIn, huge, S::Tripped},
        {"fadein + nan", enterFadeIn, S::FadeIn, nanInside, S::Tripped},
        {"run + unpair", enterRun, S::Run, unpair, S::Unpaired},
        {"run + stall", enterRun, S::Run, stall, S::Stalled},
        {"run + preset", enterRun, S::Run, preset, S::FadeIn},
        {"run + guard", enterRun, S::Run, [](MachineRig& r) { r.run(samplesOf(2.0), true, hotGuard()); }, S::Guard},
        {"run + panic", enterRun, S::Run, panicNow, S::Tripped},
        {"run + huge", enterRun, S::Run, huge, S::Tripped},
        {"run + nan", enterRun, S::Run, nanInside, S::Tripped},
        {"stalled + unstall", enterStalled, S::Stalled, unstall, S::Run},
        {"stalled + unpair", enterStalled, S::Stalled, unpair, S::Unpaired},
        {"stalled + panic", enterStalled, S::Stalled, panicNow, S::Tripped},
        {"guard + calm", enterGuard, S::Guard, guardCalm, S::Run},
        {"guard + guard off", enterGuard, S::Guard, guardOff, S::Run},
        {"guard + floor", enterGuard, S::Guard, guardRunaway, S::Tripped},
        {"guard + preset", enterGuard, S::Guard, preset, S::FadeIn},
        {"guard + stall", enterGuard, S::Guard, stall, S::Stalled},
        {"guard + unpair", enterGuard, S::Guard, unpair, S::Unpaired},
        {"guard + panic", enterGuard, S::Guard, panicNow, S::Tripped},
        {"tripped + recover", enterTripped, S::Tripped, recoverAfterCooldown, S::FadeIn},
        {"tripped + early recover", enterTripped, S::Tripped, recoverEarly, S::Tripped},
        {"tripped + preset", enterTripped, S::Tripped, preset, S::Tripped},
        {"tripped + unpair", enterTripped, S::Tripped, unpair, S::Tripped},
        {"tripped + panic", enterTripped, S::Tripped, panicNow, S::Tripped},
        {"tripped + nan", enterTripped, S::Tripped, nanInside, S::Tripped},
    };
    for (const auto& t : table)
    {
        INFO(t.name);
        MachineRig rig;
        t.enter(rig);
        REQUIRE(rig.m.state() == t.from);
        t.event(rig);
        CHECK(rig.m.state() == t.to);
    }
}

TEST_CASE("state changes are reported as events with the reason", "[safety][machine][events]")
{
    MachineRig rig;
    rig.toRun();
    rig.panic();
    rig.run(k10ms);
    rig.collect();
    bool sawTripState = false;
    bool sawTripped = false;
    bool sawFlush = false;
    for (const auto& e : rig.events)
    {
        sawTripState = sawTripState || (e.code == SafetyEventCode::StateChanged &&
                                        e.a == static_cast<std::uint8_t>(SafetyState::Tripped) &&
                                        e.b == static_cast<std::uint8_t>(TripReason::Panic));
        sawTripped =
            sawTripped || (e.code == SafetyEventCode::Tripped && e.a == static_cast<std::uint8_t>(TripReason::Panic));
        sawFlush = sawFlush || e.code == SafetyEventCode::FlushRequested;
    }
    CHECK(sawTripState);
    CHECK(sawTripped);
    CHECK(sawFlush);
}
