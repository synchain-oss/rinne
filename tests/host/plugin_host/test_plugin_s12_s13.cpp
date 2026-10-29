// SPDX-License-Identifier: GPL-3.0-or-later
//
// S12、S13 的插件级复验：同 core 级（tests/loop_rig/scenarios/s12_loop_garbage.cpp、s13_safety_matrix.cpp）的口径，
// 经真处理器走一遍。参数经参数树写，坏值与链增益由插件级宿主的中间链给。
//
// S12：延迟 100 ms、反馈 50 %、混合 50 %，两个余弦；
//   跑到 Run 之后依次来 NaN、+Inf 增益、1e6 增益三种突发，每种 512 个样本。
//   Output 的清洗计数恰为 Output 看到的含非有限值的帧数，
//   钳位计数恰为两个声道都有限且有一个 |v| > 16 的帧数；
//   Input 从环头读到的两个计数与之相同、琥珀灯亮；
//   读头全程有限且 ≤ 16，1e6 突发在 D 之后读回来的是钳过的 ±16；Input 的输出全程有限；
//   不跳闸、状态一直是 Run，回送 |ret| ≤ 上限。对照：1e6 突发持续 150 ms → TRIPPED(huge)。
// S13：格子、信号、预期与判据和 core 级同一份（s13_safety_cells.h），参照安全链也是同一份（s13_reference_chain.h）：
//   不误报一格、超大值一格、+24 dB 一格、限制器关的联锁一格、+12 dB × 100 %（失控保护）一格、默认上限一格，外加
//   失控跳闸的对照格。默认上限那一格不写 ceiling 参数，用新实例本来的值（默认配置）。
//   激励从配对之后至少 600 ms 起（淡入早已走完），并落在与 core 级同一个相位上（激励样本号 ≡ 28800 mod 7680：
//   7680 = 10 ms 的估计格 480 与块长 512 的最小公倍数），所以判据看到的块边界与估计格边界和 core 级完全一样。
//   跳闸与进 GUARD 的时刻取安全状态机这一块的事件（样本号 = Input 实例处理过的第几个样本）。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include "engine/SafetyEvents.h"
#include "engine/SafetyMachine.h"
#include "host/plugin_host/PluginHost.h"
#include "host/plugin_host/PluginScenarioKit.h"
#include "loop_rig/scenarios/s13_reference_chain.h"
#include "loop_rig/scenarios/s13_safety_cells.h"

namespace
{

using namespace rinne::plugin_host;
namespace s13 = rinne::s13;
using rinne::engine::SafetyState;
using rinne::engine::TripReason;
using rinne::wiring_test::formalOnly;
using rinne::wiring_test::needsSharedMemory;

constexpr int kS12Block = 256;
constexpr std::int64_t kSecond = 48000;
constexpr std::int64_t kS12Delay = 4800; // 100 ms

// S12 的一条轨道：逐块记下 Input 的输出、读头、琥珀灯，以及 Output 看到的坏帧
class GarbageTrack
{
public:
    GarbageTrack() : host(options())
    {
        useTones(host);
        setParamExact(host, "feedback", 50.0f);
        setParamExact(host, "mix", 50.0f);
        setParam(host, "delay_ms", 100.0f);
        host.onInputBlock = [this](const InputBlock& b) {
            const rinne::loop::BlockResult& r = host.inputEngine().lastBlockResult();
            const int length = r.returnLeft != nullptr && r.returnRight != nullptr ? std::min(r.returnLength, b.n) : 0;
            for (int i = 0; i < b.n; ++i)
            {
                const float tapLeft = i < length ? r.returnLeft[i] : 0.0f;
                const float tapRight = i < length ? r.returnRight[i] : 0.0f;
                tapPeak.push_back(std::max(std::fabs(tapLeft), std::fabs(tapRight)));
                badTaps += std::isfinite(tapLeft) && std::isfinite(tapRight) ? 0u : 1u;
                badOut += std::isfinite(b.outLeft[i]) && std::isfinite(b.outRight[i]) ? 0u : 1u;
                // 混合 50 %：g_dry = 1，回送 = 输出 − 干声（相减差一个最低位以内）
                maxRet = std::max(
                    {maxRet, std::fabs(b.outLeft[i] - b.dryLeft[i]), std::fabs(b.outRight[i] - b.dryRight[i])});
            }
            lamp = host.inputEngine().safetyDiag().loopNanLamp;
        };
        host.onOutputBlock = [this](const float* left, const float* right, int n) {
            for (int i = 0; i < n; ++i)
            {
                const bool finite = std::isfinite(left[i]) && std::isfinite(right[i]);
                nonFiniteFrames += finite ? 0u : 1u;
                clampFrames += finite && (std::fabs(left[i]) > 16.0f || std::fabs(right[i]) > 16.0f) ? 1u : 0u;
            }
        };
        host.prepare();
        REQUIRE(host.selectGroup(1));
        REQUIRE(host.pairUp(kS12Block));
    }

    static HostOptions options()
    {
        HostOptions o;
        o.inputBlock = kS12Block;
        o.outputBlock = kS12Block;
        return o;
    }

    void run(std::int64_t samples) { host.run(samples, kS12Block); }

    // 中间链从下一个样本起吐 samples 个坏值；返回突发的起点（轨道上的样本号）
    std::int64_t burst(Garbage kind, std::int64_t samples)
    {
        const std::int64_t at = host.chain().position();
        host.chain().addBurst(kind, at, at + samples);
        run(samples);
        return at;
    }

    SafetyState state() const { return static_cast<SafetyState>(host.inputEngine().safetyDiag().state); }

    PluginHost host;
    std::vector<float> tapPeak;
    std::size_t badTaps = 0;
    std::size_t badOut = 0;
    float maxRet = 0.0f;
    bool lamp = false;
    std::uint32_t nonFiniteFrames = 0;
    std::uint32_t clampFrames = 0;
};

float peakIn(const std::vector<float>& trace, std::size_t from, std::size_t to)
{
    float m = 0.0f;
    for (std::size_t i = from; i < std::min(to, trace.size()); ++i)
        m = std::max(m, trace[i]);
    return m;
}

} // namespace

TEST_CASE("plugin_host s12: NaN, Inf and 1e6 bursts from the plug-in in the loop are cleaned by Output, counted, and "
          "never trip",
          "[plugin_host][s12]")
{
    formalOnly();
    needsSharedMemory();
    GarbageTrack track;
    track.run(2 * kSecond);
    REQUIRE(track.state() == SafetyState::Run);

    std::uint32_t wantNonFinite = 0;
    std::uint32_t wantClamps = 0;
    std::int64_t hugeAt = 0;
    for (const Garbage kind : {Garbage::kNan, Garbage::kInf, Garbage::kHuge})
    {
        INFO("burst " << static_cast<int>(kind));
        const std::uint32_t nonFiniteBefore = track.nonFiniteFrames;
        const std::uint32_t clampsBefore = track.clampFrames;
        const std::int64_t at = track.burst(kind, 2 * kS12Block);
        const std::uint32_t nonFinite = track.nonFiniteFrames - nonFiniteBefore;
        const std::uint32_t clamps = track.clampFrames - clampsBefore;
        CHECK(nonFinite == (kind == Garbage::kHuge ? 0u : 2u * kS12Block));
        CHECK(clamps == (kind == Garbage::kHuge ? 2u * kS12Block : 0u));
        wantNonFinite += nonFinite;
        wantClamps += clamps;
        CHECK(track.host.outputEngine().nonFiniteCount() == wantNonFinite);
        CHECK(track.host.outputEngine().clampCount() == wantClamps);
        if (kind == Garbage::kNan)
        {
            track.run(2 * kS12Block);
            CHECK(track.lamp);
        }
        if (kind == Garbage::kHuge)
            hugeAt = at;
        track.run(kSecond);
    }
    track.run(kSecond);

    const rinne::engine::SafetyDiag diag = track.host.inputEngine().safetyDiag();
    CHECK(diag.nonfiniteRing == track.host.outputEngine().nonFiniteCount());
    CHECK(diag.clampRing == track.host.outputEngine().clampCount());
    CHECK(track.badTaps == 0u);
    CHECK(peakIn(track.tapPeak, 0, track.tapPeak.size()) <= 16.0f);
    const auto echo = static_cast<std::size_t>(hugeAt + kS12Delay);
    CHECK(peakIn(track.tapPeak, echo, echo + 2 * kS12Block) == 16.0f);
    CHECK(track.badOut == 0u);
    CHECK(diag.trips == 0u);
    CHECK(track.state() == SafetyState::Run);
    const float ceiling = static_cast<float>(std::pow(10.0, -6.0 / 20.0));
    CHECK(track.maxRet <= ceiling + 1.0e-6f);
    CHECK(track.host.outputTouched == 0u);
    CHECK(track.host.canaryHits == 0u);
}

TEST_CASE("plugin_host s12: the same 1e6 burst held for 150 ms trips on huge", "[plugin_host][s12]")
{
    formalOnly();
    needsSharedMemory();
    GarbageTrack track;
    track.run(2 * kSecond);
    REQUIRE(track.state() == SafetyState::Run);
    track.burst(Garbage::kHuge, 7200);
    track.run(kSecond);
    CHECK(track.host.inputEngine().safetyDiag().trips == 1u);
    CHECK(track.state() == SafetyState::Tripped);
    CHECK(track.host.inputEngine().safetyChain().machine().tripReason() == TripReason::Huge);
    CHECK(track.host.outputEngine().nonFiniteCount() == 0u);
    CHECK(track.badOut == 0u);
}

namespace
{

constexpr std::int64_t kPhase = 7680; // 估计格 480 与块长 512 的最小公倍数

struct PluginCellRun
{
    s13::CellResult result;
    std::int64_t exciteAt = 0;
    bool wiring = true; // Input 的输出与参照逐位相同
    bool quietBeforeExcite = true; // 激励之前既没进 GUARD 也没跳
};

PluginCellRun runPluginCell(const s13::Cell& cell)
{
    PluginCellRun out;
    HostOptions options;
    options.inputBlock = s13::kBlock;
    options.outputBlock = s13::kBlock;
    PluginHost host(options);
    setParamExact(host, "feedback", cell.feedbackPercent);
    setParamExact(host, "mix", s13::kMixPercent);
    setParam(host, "delay_ms", s13::kDelayMs);
    setParamExact(host, "limiter", cell.limiterOn ? 1.0f : 0.0f);
    if (cell.ceilingDb == s13::kDefaultCeilingDb)
        REQUIRE(rinne::wiring_test::paramValue(host.input(), "ceiling") == s13::kDefaultCeilingDb);
    else
        setParamExact(host, "ceiling", cell.ceilingDb);
    host.chain().setGain(s13::dbToGain(static_cast<double>(cell.chainDb)));

    std::int64_t exciteAt = std::numeric_limits<std::int64_t>::max();
    const s13::Signal signal = cell.signal;
    host.source = [&exciteAt, signal](std::int64_t index, float& left, float& right) {
        left = index < exciteAt ? 0.0f : s13::signalAt(signal, index - exciteAt);
        right = left;
    };

    s13::ReferenceChain reference(host.inputParams());
    std::unique_ptr<s13::CellJudge> judge;
    host.onInputBlock = [&](const InputBlock& b) {
        reference.block(host.inputEngine().lastBlockResult(), b.dryLeft, b.dryRight, b.n);
        const auto bytes = static_cast<std::size_t>(b.n) * sizeof(float);
        out.wiring = out.wiring && std::memcmp(reference.wantLeft.data(), b.outLeft, bytes) == 0 &&
                     std::memcmp(reference.wantRight.data(), b.outRight, bytes) == 0;
        const rinne::engine::SafetyMachine& machine = host.inputEngine().safetyChain().machine();
        const rinne::engine::SafetyEventLog& events = machine.events();
        for (int e = 0; e < events.size(); ++e)
        {
            const auto code = static_cast<std::uint16_t>(events[e].code);
            if (judge != nullptr)
                judge->event(code, events[e].a, events[e].sample);
            else if (code == s13::kEventTripped || code == s13::kEventGuardEngaged)
                out.quietBeforeExcite = false;
        }
        if (judge == nullptr)
            return;
        for (int i = 0; i < b.n; ++i)
        {
            const auto k = static_cast<std::size_t>(i);
            const bool finite = std::isfinite(b.outLeft[i]) && std::isfinite(b.outRight[i]);
            judge->sample(b.first + i, reference.retLeft[k], reference.retRight[k], 0.0f, reference.hot[k], finite);
        }
        judge->endBlock(machine.guardGainDb());
    };

    host.prepare();
    REQUIRE(host.selectGroup(1));
    REQUIRE(host.pairUp(s13::kBlock));
    // 激励：配对之后至少 600 ms，且与 core 级同相位
    const std::int64_t earliest = host.trackPosition() + s13::kExciteAt;
    exciteAt = s13::kExciteAt + (earliest - s13::kExciteAt + kPhase - 1) / kPhase * kPhase;
    out.exciteAt = exciteAt;
    judge = std::make_unique<s13::CellJudge>(cell, exciteAt);
    while (!judge->done())
        host.cycle(s13::kBlock);
    out.result = judge->finish();
    host.onInputBlock = nullptr;
    return out;
}

} // namespace

TEST_CASE("plugin_host s13: representative safety cells land on exactly the state in the core table",
          "[plugin_host][s13]")
{
    formalOnly();
    needsSharedMemory();
    const s13::Cell cells[] = {
        // 不误报：反馈 90 %、链 0 dB、干声 +12 dBFS，零介入
        s13::Cell{0.0f, 90.0f, s13::Signal::kSine12, true, s13::Outcome::kQuiet},
        // 超大值跳闸
        s13::Cell{18.0f, 50.0f, s13::Signal::kPink, true, s13::Outcome::kHuge},
        // +24 dB 一列：≤ 2 s 跳闸
        s13::Cell{24.0f, 20.0f, s13::Signal::kImpulse, true, s13::Outcome::kHuge},
        // 限制器关：反馈有效上限 100 %，软削波与失控保护照常，零介入
        s13::Cell{0.0f, 150.0f, s13::Signal::kPink, false, s13::Outcome::kQuiet},
        // +12 dB × 100 %：失控保护压低回送，不跳
        s13::Cell{12.0f, 100.0f, s13::Signal::kImpulse, true, s13::Outcome::kGuard},
        // 默认上限 −6 dBFS：超阈样本稀疏，跳闸恰在凑满 100 ms 的那个超阈样本上
        s13::Cell{12.0f, 150.0f, s13::Signal::kPink, true, s13::Outcome::kHuge, s13::kDefaultCeilingDb},
        // 失控跳闸：压到 −24 dB 仍持续 1 s
        s13::Cell{32.0f, 100.0f, s13::Signal::kImpulse, true, s13::Outcome::kRunaway, -24.0f},
    };
    for (const s13::Cell& cell : cells)
    {
        // 预期与 core 级的表一致（同一张表里的格子）
        bool inTable = false;
        for (const s13::Cell& c : s13::allCells())
            inTable = inTable || (c.chainDb == cell.chainDb && c.feedbackPercent == cell.feedbackPercent &&
                                  c.signal == cell.signal && c.limiterOn == cell.limiterOn &&
                                  c.ceilingDb == cell.ceilingDb && c.expect == cell.expect);
        INFO("cell: chain " << cell.chainDb << " dB, feedback " << cell.feedbackPercent << " %, "
                            << s13::signalName(cell.signal) << ", limiter " << (cell.limiterOn ? "on" : "off")
                            << ", ceiling " << cell.ceilingDb << " dBFS, expect " << s13::outcomeName(cell.expect));
        CHECK(inTable);
        const PluginCellRun run = runPluginCell(cell);
        const s13::CellResult& r = run.result;
        INFO("excite at " << run.exciteAt << ", got " << s13::outcomeName(r.got) << " (trip reason "
                          << static_cast<int>(r.tripReason) << "), guard at " << r.guardAt << ", trip at " << r.tripAt
                          << ", run start " << r.hotStart << ", max |ret| " << r.maxRet << ", guard floor "
                          << r.guardFloorDb << " dB");
        CHECK(run.wiring);
        CHECK(run.quietBeforeExcite);
        for (const s13::Check& check : s13::verdict(cell, r, run.exciteAt))
        {
            INFO(check.what);
            CHECK(check.pass);
        }
    }
}
