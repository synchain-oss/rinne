// SPDX-License-Identifier: GPL-3.0-or-later
//
// S14、S15 的插件级复验：同 core 级（tests/loop_rig/scenarios/s14_delay_change.cpp、s15_beat_sync.cpp）的口径与阈值，
// 这里经真处理器走一遍：参数经参数树写，块长、重新 prepare 与跳块由宿主给，速度与时间戳由 LanePlayHead 的脚本给。
//
// S14：反馈 0、混合 50 %，信号与阈值见 s14_tap_trace.h 与 s14_delay_change.cpp 的文件头：
//   ① 参数 delay_ms 25 → 50 ms（块长 256）：B = S + 2A/1920；
//   ② Output 以 2048 重新 prepare，此后宿主送 2048 帧的块（Input 声明 512，判违约，D 下限当场抬到 2048）：
//      B = S + A·(2/1920 + 1/1200)；
//   ③ 块长 128，delay_ms 12.5 → 25 ms 之后两块，宿主跳过 Input 8 块（Output
//   照常）：回来的第二块位移重同步，落在交叉淡化里，
//      读头槽里同时 3 个：B = 2S + A·(2/1920 + 1/240 + 1/960)。
// S15：节拍模式、1/4。宿主一直给 144 BPM 与时间戳：D =
// 20000、来源宿主、无提示、时间戳有效；宿主先不给速度也不给时间戳：
//   按 120 BPM（D = 24000）、来源回退、提示位「没有速度」、时间戳无效；之后给了：D 交叉淡化到
//   20000、来源宿主、提示撤掉、 时间戳有效。证明壳把宿主的速度与时间戳（以及它们的缺失）如实交给了引擎。
//
// 标 ⛔ 的是删除式验证点：
//   ⛔ 壳不把宿主给的速度交给引擎 → S15 一直是回退、提示位置上；
//   ⛔ 壳不把宿主给的时间戳交给引擎 → S15 的时间戳一直无效。

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "host/plugin_host/PluginHost.h"
#include "host/plugin_host/PluginScenarioKit.h"
#include "loop/LoopStatus.h"
#include "loop/LoopTiming.h"
#include "loop/StreamSync.h"
#include "loop_rig/scenarios/s14_tap_trace.h"

namespace
{

using namespace rinne::plugin_host;
using namespace rinne::s14;
using rinne::loop::BpmSource;
using rinne::loop::DelayMinReason;
using rinne::loop::kDelayHintNoTempo;
using rinne::loop::LoopStatusFields;
using rinne::loop::SyncReason;
using rinne::wiring_test::formalOnly;
using rinne::wiring_test::needsSharedMemory;

HostOptions blocks(int block)
{
    HostOptions options;
    options.inputBlock = block;
    options.outputBlock = block;
    return options;
}

// 反馈 0、混合 50 %、时间模式 delayMs，两端 prepare、选组、配上对
void openTimeLoop(PluginHost& host, int block, float delayMs)
{
    useTones(host);
    setParamExact(host, "feedback", 0.0f);
    setParamExact(host, "mix", 50.0f);
    setParam(host, "delay_ms", delayMs);
    host.prepare();
    REQUIRE(host.selectGroup(1));
    REQUIRE(host.pairUp(block));
}

// 一次 D 变化之后的断言，同 core 级 s14_tap_leg.h 的 checkTransition
void checkTransition(const TapRecorder& rec, const LoopStatusFields& s, std::size_t mark, std::int64_t oldDelay,
                     std::int64_t newDelay, const Bound& bound, std::uint32_t peakTaps)
{
    const std::size_t end = rec.now();
    CHECK(settledOn(rec.tap, mark - 4800, mark, oldDelay));
    const float stepLeft = maxStep(rec.tap.left, mark - 1, end);
    const float stepRight = maxStep(rec.tap.right, mark - 1, end);
    INFO("max step " << stepLeft << " / " << stepRight << ", bound " << bound.left << " / " << bound.right);
    CHECK(stepLeft <= bound.left);
    CHECK(stepRight <= bound.right);
    CHECK(settledOn(rec.tap, mark + static_cast<std::size_t>(kXfade), end, newDelay));
    const std::size_t window = mark + static_cast<std::size_t>(kXfade);
    CHECK(switchJump(oldDelay, newDelay, mark, window, true) > bound.left);
    CHECK(switchJump(oldDelay, newDelay, mark, window, false) > bound.right);
    CHECK(s.delayEffective == newDelay);
    CHECK(s.resyncByReason[reasonIndex(SyncReason::kDChange)] == 1u);
    CHECK(s.tapDrop == 0u);
    CHECK(rec.tapsWithinSlots);
    CHECK(rec.maxTapsActive == peakTaps);
}

} // namespace

TEST_CASE("plugin_host s14: the delay_ms parameter changes D through the crossfade without a step",
          "[plugin_host][s14]")
{
    formalOnly();
    needsSharedMemory();
    PluginHost host(blocks(256));
    TapRecorder rec(host);
    openTimeLoop(host, 256, 25.0f);
    host.run(2 * kSecond, 256);
    REQUIRE(host.inputEngine().loopStatus().delayEffective == 1200);
    const std::size_t mark = rec.now();
    setParam(host, "delay_ms", 50.0f);
    host.run(kSecond, 256);
    checkTransition(rec, host.inputEngine().loopStatus(), mark, 1200, 2400, boundOf(1.0f, 2.0 / kXfade), 2);
    CHECK(host.outputTouched == 0u);
    CHECK(host.canaryHits == 0u);
}

TEST_CASE("plugin_host s14: Output re-prepared with a larger block and fed 2048-frame blocks lifts the D floor at once "
          "without a step",
          "[plugin_host][s14]")
{
    formalOnly();
    needsSharedMemory();
    PluginHost host(blocks(512));
    TapRecorder rec(host);
    openTimeLoop(host, 512, 25.0f);
    host.run(2 * kSecond, 512);
    REQUIRE(host.inputEngine().loopStatus().delayEffective == 1200);
    const std::size_t mark = rec.now();
    host.prepareOutput(2048);
    host.run(kSecond, 2048);
    const LoopStatusFields s = host.inputEngine().loopStatus();
    // 2048 帧的块装得下整段过渡，块末快照只看得到 1 个读头（槽数仍逐块断言 ≤ 3）
    checkTransition(rec, s, mark, 1200, 2048, boundOf(1.0f, 2.0 / kXfade + 1.0 / 1200.0), 1);
    CHECK(s.delayMin == 2048);
    CHECK(s.delayMinReason == static_cast<std::uint8_t>(DelayMinReason::kHostViolation));
    CHECK(s.overrun == 0u);
    CHECK(host.paired());
}

TEST_CASE("plugin_host s14: a shift resync inside the D crossfade holds three taps without a step",
          "[plugin_host][s14]")
{
    formalOnly();
    needsSharedMemory();
    constexpr int block = 128;
    constexpr int skipped = 8; // 1024 帧
    PluginHost host(blocks(block));
    TapRecorder rec(host);
    openTimeLoop(host, block, 12.5f);
    host.run(2 * kSecond, block);
    const std::size_t mark = rec.now();
    setParam(host, "delay_ms", 25.0f);
    host.cycle(block);
    host.cycle(block);
    for (int i = 0; i < skipped; ++i)
        host.cycle(block, false, true);
    host.run(kSecond, block);
    const std::size_t end = rec.now();
    const LoopStatusFields s = host.inputEngine().loopStatus();
    CHECK(s.resyncByReason[reasonIndex(SyncReason::kShift)] == 1u);
    CHECK(s.resyncByReason[reasonIndex(SyncReason::kDChange)] == 1u);
    CHECK(rec.shiftAt == static_cast<std::int64_t>(mark) + 3 * block);
    CHECK(rec.xfadeLeftBeforeShift > 0);
    CHECK(rec.maxTapsActive == kMaxTaps);
    CHECK(rec.tapsWithinSlots);
    CHECK(s.tapDrop == 0u);

    const Bound bound = boundOf(2.0f, 2.0 / kXfade + 1.0 / kFadeOut + 1.0 / kOnset);
    const float stepLeft = maxStep(rec.tap.left, mark - 1, end);
    const float stepRight = maxStep(rec.tap.right, mark - 1, end);
    INFO("max step " << stepLeft << " / " << stepRight << ", bound " << bound.left << " / " << bound.right);
    CHECK(stepLeft <= bound.left);
    CHECK(stepRight <= bound.right);
    REQUIRE(rec.shiftAt >= 0);
    const auto at = static_cast<std::size_t>(rec.shiftAt);
    const auto loudest = [&](const std::vector<float>& trace) {
        std::size_t best = at;
        for (std::size_t i = at; i < at + static_cast<std::size_t>(kFadeOut) && i < trace.size(); ++i)
            if (std::fabs(trace[i]) > std::fabs(trace[best]))
                best = i;
        return best;
    };
    CHECK(maxStep(cutAt(rec.tap.left, loudest(rec.tap.left) + 1), mark - 1, end) > bound.left);
    CHECK(maxStep(cutAt(rec.tap.right, loudest(rec.tap.right) + 1), mark - 1, end) > bound.right);
}

namespace
{

// 节拍模式、1/4；两端的播放头按 facts 给速度与时间戳
void openSyncLoop(PluginHost& host, const LanePlayHead::FactScript& facts)
{
    useTones(host);
    setParamExact(host, "feedback", 0.0f);
    setParamExact(host, "mix", 50.0f);
    setParamExact(host, "delay_mode", 1.0f);
    setParamExact(host, "delay_note", 10.0f);
    host.inputLane().setFacts(facts);
    host.outputLane().setFacts(facts);
    host.prepare();
    REQUIRE(host.selectGroup(1));
    REQUIRE(host.pairUp(256));
}

LanePlayHead::FactScript tempo(std::optional<double> bpm, bool timestamp)
{
    return [bpm, timestamp](std::int64_t, std::uint64_t clockNs) {
        return HostFacts{bpm, timestamp ? std::optional<std::uint64_t>(clockNs) : std::nullopt};
    };
}

void checkTempo(const LoopStatusFields& s, std::uint32_t bpmCenti, BpmSource source, std::uint32_t hints,
                std::int64_t delay, std::uint32_t hostNsValid)
{
    CHECK(s.bpmCenti == bpmCenti);
    CHECK(static_cast<int>(s.bpmSource) == static_cast<int>(source));
    CHECK(s.delayHints == hints);
    CHECK(s.delayTarget == delay);
    CHECK(s.delayEffective == delay);
    CHECK(s.hostNsValid == hostNsValid);
}

} // namespace

TEST_CASE("plugin_host s15: the shell hands the host tempo and timestamp to the engine and D follows them",
          "[plugin_host][s15]")
{
    formalOnly();
    needsSharedMemory();
    PluginHost host(blocks(256));
    TapRecorder rec(host);
    openSyncLoop(host, tempo(144.0, true));
    host.run(2 * kSecond, 256);
    checkTempo(host.inputEngine().loopStatus(), 14400, BpmSource::kHost, 0, 20000, 1);
    CHECK(settledOn(rec.tap, rec.now() - 4800, rec.now(), 20000));
    CHECK(host.inputEngine().loopStatus().resyncByReason[reasonIndex(SyncReason::kDChange)] == 0u);
}

TEST_CASE("plugin_host s15: without a host tempo the engine falls back to 120 BPM and hints, then follows the tempo "
          "once the host gives one",
          "[plugin_host][s15]")
{
    formalOnly();
    needsSharedMemory();
    PluginHost host(blocks(256));
    TapRecorder rec(host);
    openSyncLoop(host, tempo(std::nullopt, false));
    host.run(2 * kSecond, 256);
    checkTempo(host.inputEngine().loopStatus(), 12000, BpmSource::kFallback, kDelayHintNoTempo, 24000, 0);
    CHECK(settledOn(rec.tap, rec.now() - 4800, rec.now(), 24000));

    const std::size_t mark = rec.now();
    host.inputLane().setFacts(tempo(144.0, true));
    host.outputLane().setFacts(tempo(144.0, true));
    host.run(kSecond, 256);
    const LoopStatusFields s = host.inputEngine().loopStatus();
    checkTempo(s, 14400, BpmSource::kHost, 0, 20000, 1);
    checkTransition(rec, s, mark, 24000, 20000, boundOf(1.0f, 2.0 / kXfade), 2);
}
