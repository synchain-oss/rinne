// SPDX-License-Identifier: GPL-3.0-or-later
//
// S15 节拍同步（core 级）：宿主给速度时 D 跟着时值与速度走；宿主不给时回退 120 BPM 并置「没有速度」提示。
//
// 节拍模式，反馈 0、混合 50 %，信号与读头轨迹同 S14（s14_tap_trace.h）。48 kHz、块长 256，Input 先 Output 后。
// D = 拍数 × 60 / BPM × sr。速度与时值挑得让每次的 ΔD 都不是两个余弦周期（480、320 个样本）的整数倍，
// 否则新旧两个位置读到的值相同，「读的正是新 D」与反例都分辨不出来。两条腿：
//   有速度  宿主从第一块起给 144 BPM：1/4 → D = 20000（宿主、无提示、0 次改 D）；
//           时值换成 1/8D → 15000；速度换成 90 → 1/8D 的 D = 24000（两次都走 40 ms 交叉淡化）；
//           之后宿主不给（0）、给超出范围的 1500 与 10、给 NaN：都沿用最后一个有效值 90，D
//           不动、来源仍是宿主、不置提示。
//   没速度  宿主一直不给：按 120 BPM，1/4 → D = 24000，来源是回退，提示位恰为「没有速度」，提示事件恰 1 条（a = 1，b =
//   1）；
//           之后宿主给 144 → D 24000 → 20000 交叉淡化，来源变成宿主，提示撤掉（第二条提示事件 a = 0，b = 1）。
// 每次 D 变化都按 S14 的口径判：步长 ≤ S + 2A/1920、直接换的反例超过它、交叉淡化走完之后读的正是新 D（逐位）。
// 没有 D 变化的时段读头逐位读 D 之前的干声。
//
// 标 ⛔ 的是删除式验证点：
//   ⛔ 拿到有效速度时不记「拿到过」（回退判据失灵）→ 有速度那条腿的来源变成回退、提示位被置上。

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "engine/EngineParams.h"
#include "loop/LoopEvents.h"
#include "loop/LoopStatus.h"
#include "loop/StreamSync.h"
#include "s01_rig_kit.h"
#include "s14_tap_leg.h"
#include "s14_tap_trace.h"

namespace
{

using namespace rinne::rig;
using namespace rinne::rig::scn;
using namespace rinne::s14;
using rinne::engine::DelayMode;
using rinne::loop::BpmSource;
using rinne::loop::kDelayHintNoTempo;
using rinne::loop::LoopEventCode;
using rinne::loop::SyncReason;

constexpr std::uint8_t kNoteQuarter = 10; // 1/4，1 拍
constexpr std::uint8_t kNoteDottedEighth = 9; // 1/8D，3/4 拍
constexpr auto kHintsEvent = static_cast<std::uint16_t>(LoopEventCode::kDelayHints);

rinne::engine::EngineParams syncParams(std::uint8_t note)
{
    rinne::engine::EngineParams p = loopParams(0.0f, 50.0f, 50.0f);
    p.delayMode = DelayMode::kSync;
    p.delayNote = note;
    return p;
}

// 速度、来源、提示位与 D 的快照
void checkTempo(Verdict& v, const TapLeg& leg, std::uint32_t bpmCenti, BpmSource source, std::uint32_t hints,
                std::int64_t delay)
{
    const rinne::loop::LoopStatusFields& s = leg.status();
    RIG_EQ(v, s.bpmCenti, bpmCenti);
    RIG_EQ(v, static_cast<int>(s.bpmSource), static_cast<int>(source));
    RIG_EQ(v, s.delayHints, hints);
    RIG_EQ(v, s.delayTarget, delay);
    RIG_EQ(v, s.delayEffective, delay);
    RIG_EQ(v, s.delayXfadeLeft, std::int64_t{0});
}

class BeatSyncScenario final : public Scenario
{
public:
    const char* id() const noexcept override { return "s15_beat_sync"; }

    bool run(ScenarioContext&) override
    {
        Verdict v;
        hostTempo(v);
        missingTempo(v);
        return v.ok();
    }

private:
    static void hostTempo(Verdict& v)
    {
        INFO("host gives 144 BPM from the first block");
        TapLeg leg(256, syncParams(kNoteQuarter));
        leg.setBpm(144.0f);
        leg.run(2 * kSecond);
        checkTempo(v, leg, 14400, BpmSource::kHost, 0, 20000);
        RIG_CHECK(v, settledOn(leg.tap, leg.now() - 4800, leg.now(), 20000));
        RIG_EQ(v, leg.status().resyncByReason[reasonIndex(SyncReason::kDChange)], 0u);

        // 时值跟着走
        std::size_t mark = leg.now();
        leg.setParams(syncParams(kNoteDottedEighth));
        leg.run(kSecond);
        checkTransition(v, leg, mark, 20000, 15000, boundOf(1.0f, 2.0 / kXfade), 2, 1);
        checkTempo(v, leg, 14400, BpmSource::kHost, 0, 15000);

        // 速度跟着走
        mark = leg.now();
        leg.setBpm(90.0f);
        leg.run(kSecond);
        checkTransition(v, leg, mark, 15000, 24000, boundOf(1.0f, 2.0 / kXfade), 2, 2);
        checkTempo(v, leg, 9000, BpmSource::kHost, 0, 24000);

        // 宿主不给、给超出范围的、给 NaN：沿用最后一个有效值
        for (const float bad : {0.0f, 1500.0f, 10.0f, std::numeric_limits<float>::quiet_NaN()})
        {
            INFO("host bpm " << bad);
            mark = leg.now();
            leg.setBpm(bad);
            leg.run(kSecond);
            checkTempo(v, leg, 9000, BpmSource::kHost, 0, 24000);
            RIG_CHECK(v, settledOn(leg.tap, mark, leg.now(), 24000));
        }
        RIG_EQ(v, leg.status().resyncByReason[reasonIndex(SyncReason::kDChange)], 2u);
        RIG_EQ(v, leg.eventCount(kHintsEvent), std::size_t{0});
    }

    static void missingTempo(Verdict& v)
    {
        INFO("host never gives a tempo, then gives 144 BPM");
        TapLeg leg(256, syncParams(kNoteQuarter));
        leg.setBpm(0.0f);
        leg.run(2 * kSecond);
        checkTempo(v, leg, 12000, BpmSource::kFallback, kDelayHintNoTempo, 24000);
        RIG_CHECK(v, settledOn(leg.tap, leg.now() - 4800, leg.now(), 24000));
        RIG_EQ(v, leg.eventCount(kHintsEvent), std::size_t{1});

        const std::size_t mark = leg.now();
        leg.setBpm(144.0f);
        leg.run(kSecond);
        checkTransition(v, leg, mark, 24000, 20000, boundOf(1.0f, 2.0 / kXfade), 2, 1);
        checkTempo(v, leg, 14400, BpmSource::kHost, 0, 20000);

        // 两条提示事件：置上（a = 1，b = 1）、撤掉（a = 0，b = 1）
        std::vector<rinne::diag::Event> hints;
        for (const rinne::diag::Event& e : leg.events)
        {
            if (e.code == kHintsEvent)
            {
                hints.push_back(e);
            }
        }
        RIG_EQ(v, hints.size(), std::size_t{2});
        if (hints.size() == 2)
        {
            RIG_EQ(v, static_cast<std::uint32_t>(hints[0].a), kDelayHintNoTempo);
            RIG_EQ(v, static_cast<std::uint32_t>(hints[0].b), kDelayHintNoTempo);
            RIG_EQ(v, static_cast<std::uint32_t>(hints[1].a), 0u);
            RIG_EQ(v, static_cast<std::uint32_t>(hints[1].b), kDelayHintNoTempo);
        }
    }
};

} // namespace

RINNE_RIG_SCENARIO("s15_beat_sync")
{
    return new BeatSyncScenario();
}

TEST_CASE("s15: with a host tempo D follows the note and the tempo; without one it falls back to 120 BPM and hints",
          "[loop_rig][s15]")
{
    CHECK(rinne::rig::scn::runRegisteredScenario("s15_beat_sync"));
}
