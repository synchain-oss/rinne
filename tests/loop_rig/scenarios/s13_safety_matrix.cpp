// SPDX-License-Identifier: GPL-3.0-or-later
//
// S13 安全矩阵（core 级）：环内链增益 {0, +6, +12, +18, +24} dB × 反馈 {20, 50, 100, 150} % × 信号 {冲激, 粉噪, 鼓
// loop}， 外加不误报的三格、限制器关的联锁、默认上限的五格与失控跳闸的对照。格子、信号、预期与断言在
// s13_safety_cells.h（插件级复验用同一份）。
//
// 每格一台闭环试验台：信号源 → Input → 中间链（增益）→ Output → 环。另起一条参照安全链（同一个 SafetyChain 类，
// 与 EngineTestKit 的 InputReference 同一接法），逐样本喂引擎这一块的读头与干声：
//   - 参照的输出与引擎的 Input 输出逐位相同（接线没走样），于是参照给出的回送就是引擎的回送，|ret| 逐位比较；
//   - 「超阈」按引擎同一口径逐样本重算：×g 之后 max(|tap|, |x|) ≥ +12 dBFS，|tap| 只在判为放大时计入、|x| 在判为放大
//     或 g > 1 时计入，回送不活动（g = 0）时不计。相邻超阈样本相隔 ≤ 50 ms 算同一串由本场景按样本号自己重建，
//     并找出第一个让所在那一串凑满 100 ms 的超阈样本：引擎必须恰在这个样本跳闸。
// 跳闸与进 GUARD 的时刻取引擎事件里的样本号（实例创建以来的第几个样本，与信号源的绝对样本号相同）。
//
// 隐藏用例 [.s13-survey]：每格跑到激励后 12 s，并把链增益上下偏 0.02 / 0.1 / 0.3 dB 再跑，打印结果（表就是这样定的）。
//
// 标 ⛔ 的是删除式验证点：
//   ⛔ 限制器末级不钳到上限 → |ret| ≤ 上限变红；
//   ⛔ 超大值判据要求的持续时长改长 → 跳闸晚于凑满 100 ms 的那个超阈样本，+24 dB 一列也会晚。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "diag/EventQueue.h"
#include "engine/SafetyEvents.h"
#include "s01_rig_kit.h"
#include "s13_reference_chain.h"
#include "s13_safety_cells.h"

namespace
{

using namespace rinne::rig;
using namespace rinne::rig::scn;
namespace s13 = rinne::s13;
using rinne::s13::ReferenceChain;

static_assert(static_cast<std::uint16_t>(rinne::engine::SafetyEventCode::Tripped) == s13::kEventTripped);
static_assert(static_cast<std::uint16_t>(rinne::engine::SafetyEventCode::GuardEngaged) == s13::kEventGuardEngaged);
static_assert(static_cast<std::uint8_t>(rinne::engine::TripReason::Huge) == s13::kTripHuge);
static_assert(static_cast<std::uint8_t>(rinne::engine::TripReason::Runaway) == s13::kTripRunaway);
static_assert(rinne::engine::safety::kCeilingDefaultDb == s13::kDefaultCeilingDb);

struct CellRun
{
    s13::CellResult result;
    bool wiring = true; // 引擎的 Input 输出与参照逐位相同
};

CellRun runCell(const s13::Cell& cell, float chainOffsetDb = 0.0f)
{
    CellRun out;
    auto host = makeHost(kSr, s13::kBlock);
    REQUIRE(host != nullptr);
    host->setBlockScripts(fixedBlocks(s13::kBlock), fixedBlocks(s13::kBlock));
    host->setChain(ChainKind::kGain);
    host->chain().setGain(s13::dbToGain(static_cast<double>(cell.chainDb + chainOffsetDb)));
    const s13::Signal signal = cell.signal;
    const auto sourceAt = [signal](std::int64_t index) { return s13::signalAt(signal, index - s13::kExciteAt); };
    host->setSource([sourceAt](std::uint64_t first, float* left, float* right, int n) {
        for (int i = 0; i < n; ++i)
        {
            left[i] = sourceAt(static_cast<std::int64_t>(first) + i);
            right[i] = left[i];
        }
    });
    rinne::engine::EngineParams params = loopParams(cell.feedbackPercent, s13::kMixPercent, s13::kDelayMs);
    params.limiterOn = cell.limiterOn;
    params.ceilingDb = cell.ceilingDb;
    ParamsScript script;
    script.snapshots = {params};
    host->setParamsScript(script);

    ReferenceChain reference(params);
    s13::CellJudge judge(cell, s13::kExciteAt);
    std::vector<float> dryLeft(static_cast<std::size_t>(s13::kBlock));
    for (std::size_t cycle = 0;; ++cycle)
    {
        const auto first = static_cast<std::int64_t>(host->sourcePosition());
        const int n = host->runCycle(cycle);
        if (n <= 0)
        {
            reportStall(cycle, n);
            break;
        }
        for (int i = 0; i < n; ++i)
        {
            dryLeft[static_cast<std::size_t>(i)] = sourceAt(first + i);
        }
        reference.block(host->inputLastBlock(), dryLeft.data(), dryLeft.data(), n);
        const auto bytes = static_cast<std::size_t>(n) * sizeof(float);
        out.wiring = out.wiring && std::memcmp(reference.wantLeft.data(), host->inputOutputLeft().data(), bytes) == 0 &&
                     std::memcmp(reference.wantRight.data(), host->inputOutputRight().data(), bytes) == 0;
        host->drainEvents([&](const rinne::diag::Event& e) { judge.event(e.code, e.a, e.sample); });
        for (int i = 0; i < n; ++i)
        {
            const auto k = static_cast<std::size_t>(i);
            const bool finite = std::isfinite(host->inputOutputLeft()[k]) && std::isfinite(host->inputOutputRight()[k]);
            judge.sample(first + i, reference.retLeft[k], reference.retRight[k], 0.0f, reference.hot[k], finite);
        }
        judge.endBlock(host->input().safetyChain().machine().guardGainDb());
        if (judge.done())
        {
            break;
        }
    }
    out.result = judge.finish();
    return out;
}

class SafetyMatrixScenario final : public Scenario
{
public:
    const char* id() const noexcept override { return "s13_safety_matrix"; }

    bool run(ScenarioContext&) override
    {
        Verdict v;
        for (const s13::Cell& cell : s13::allCells())
        {
            INFO("cell: chain " << cell.chainDb << " dB, feedback " << cell.feedbackPercent << " %, "
                                << s13::signalName(cell.signal) << ", limiter " << (cell.limiterOn ? "on" : "off")
                                << ", ceiling " << cell.ceilingDb << " dBFS, expect " << s13::outcomeName(cell.expect));
            const CellRun run = runCell(cell);
            const s13::CellResult& r = run.result;
            INFO("got " << s13::outcomeName(r.got) << " (trip reason " << static_cast<int>(r.tripReason)
                        << "), guard at " << r.guardAt << ", trip at " << r.tripAt << ", run start " << r.hotStart
                        << ", max |ret| " << r.maxRet << ", guard floor " << r.guardFloorDb << " dB");
            RIG_CHECK(v, run.wiring);
            for (const s13::Check& check : s13::verdict(cell, r, s13::kExciteAt))
            {
                INFO(check.what);
                RIG_CHECK(v, check.pass);
            }
        }
        return v.ok();
    }
};

double msAfterExcite(std::int64_t at)
{
    return at < 0 ? -1.0 : 1000.0 * static_cast<double>(at - s13::kExciteAt) / kSr;
}

} // namespace

RINNE_RIG_SCENARIO("s13_safety_matrix")
{
    return new SafetyMatrixScenario();
}

TEST_CASE("s13: safety matrix, every cell lands on exactly the state in the table and |ret| stays bounded",
          "[loop_rig][s13]")
{
    CHECK(rinne::rig::scn::runRegisteredScenario("s13_safety_matrix"));
}

TEST_CASE("s13 survey: every cell to 12 s after the excitation, chain gain nudged up and down", "[.s13-survey]")
{
    constexpr float offsets[] = {-0.3f, -0.1f, -0.02f, 0.0f, 0.02f, 0.1f, 0.3f};
    std::vector<s13::Cell> cells = s13::allCells();
    for (const float chain : {30.0f, 31.0f, 32.0f, 33.0f})
    {
        for (const s13::Signal signal : {s13::Signal::kImpulse, s13::Signal::kDrumsLow})
        {
            cells.push_back(s13::Cell{chain, 100.0f, signal, true, s13::Outcome::kRunaway, -24.0f});
        }
    }
    for (const s13::Cell& cell : cells)
    {
        const auto t0 = std::chrono::steady_clock::now();
        // 每格都按 Guard 的跑法跑满 12 s（跳闸即停），打印的是表里的预期
        s13::Cell probe = cell;
        probe.expect = s13::Outcome::kGuard;
        const CellRun mid = runCell(probe);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        char outcomes[16] = {};
        bool stable = true;
        for (std::size_t i = 0; i < std::size(offsets); ++i)
        {
            const s13::CellResult r = offsets[i] == 0.0f ? mid.result : runCell(probe, offsets[i]).result;
            // 链 0 dB 的格往上偏就过了防误报门（链增益 > 0 dB），那一侧不算
            const bool counts = !(cell.chainDb == 0.0f && offsets[i] > 0.0f);
            stable = stable && (!counts || r.got == mid.result.got);
            outcomes[i] = s13::outcomeName(r.got)[0];
        }
        const s13::CellResult& r = mid.result;
        std::printf(
            "%5.0f dB %4.0f %% %-9s lim=%d ceil %3.0f | expect %-7s %s %s | guard %8.1f ms trip %8.1f ms (a=%u) "
            "span %6lld first %8.1f ms maxRet %.6f floor %6.2f dB wiring %d bounded %d finite %d | %.2f s\n",
            static_cast<double>(cell.chainDb), static_cast<double>(cell.feedbackPercent), s13::signalName(cell.signal),
            cell.limiterOn ? 1 : 0, static_cast<double>(cell.ceilingDb), s13::outcomeName(cell.expect),
            stable ? "   " : "<<<", outcomes, msAfterExcite(r.guardAt), msAfterExcite(r.tripAt),
            static_cast<unsigned>(r.tripReason),
            static_cast<long long>(r.tripAt >= 0 && r.hotStart >= 0 ? r.tripAt - r.hotStart + 1 : -1),
            msAfterExcite(r.firstHot), static_cast<double>(r.maxRet), r.guardFloorDb, mid.wiring ? 1 : 0,
            r.retBounded ? 1 : 0, r.outputFinite ? 1 : 0, seconds);
    }
}
