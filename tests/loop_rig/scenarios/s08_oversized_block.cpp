// SPDX-License-Identifier: GPL-3.0-or-later
//
// S08 单块 n = 2·N_decl（宿主违约一块，只在 core 层）：两端声明的最大块长是 N_decl = 512，流里有一块是 1024，
// 之后回到 512。
//
// 引擎的约定（D 下限 = max(声明值, 本会话实测的最大块长)，宿主违约时立即抬升）：
//   - 违约那一块：取不到的样本补 0 的前提是 D 小于块长；D 的下限当场抬到 1024，所以主读头读的都是写好的帧；
//     环路层发一条宿主违约事件（载荷是块长 1024）；
//   - 之后回到 512：下限不回落（本会话最大块长 1024 留着，界面常驻显示它），原因是宿主违约；
//   - 不越界：违约块之后的缓冲哨兵没有被改动（引擎与中间链都只写块内），环的写头恰为两端处理过的样本总数；
//   - 全程有限，|回送| < 1.0。
//
// 两遍（反馈 50 %、混合 50 %，两个余弦，恒等中间链，Input 先 Output 后）：
//   D = 100 ms（4800 ≥ 1024）：违约不影响实际 D（仍是 4800）、没有 D 下限抬升事件，Input 的输出与「把这一块切成两个
//     512」的对照逐位相同（块长不变性在违约块上同样成立）；对照里没有违约、下限一直是 512；
//   D = 12.5 ms（600，高于声明值、低于违约块）：违约那一块 D 被抬到 1024，恰一次改 D 重同步，一条 D 下限抬升事件；
//     没有补 0；回送在整个过渡里没有阶跃（相邻样本差不超过余弦斜率加交叉淡化斜率的上界）。
//
// 注意断言的不是「D 下限不变」：违约块要把下限抬上去，之后下限不回落（本会话的最大块长留着）；
// 统计里的补 0 认主读头，下限当场抬升，所以 overrun 为 0。
//
// 标 ⛔ 的是删除式验证点：把对应的实现改坏，本场景必须变红。
//   ⛔ 违约那一块不把块长算进 D 下限（两处记账都不看本块）→ 下限、本会话最大块长与 D = 12.5 ms 一遍的抬升全不对；
//   ⛔ 下限在块长回落后跟着回落（只看本块长度、不保留会话最大值）→ 违约块之后下限回到 512。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "loop/LoopEvents.h"
#include "loop/LoopStatus.h"
#include "loop/LoopTiming.h"
#include "loop/StreamSync.h"
#include "s01_rig_kit.h"

namespace
{

using namespace rinne::rig;
using namespace rinne::rig::scn;
using rinne::loop::DelayMinReason;
using rinne::loop::LoopEventCode;
using rinne::loop::SyncReason;

constexpr int kDeclared = 512;
constexpr int kOversized = 2 * kDeclared;
constexpr std::size_t kCycles = 400;
constexpr std::size_t kViolationAt = 200;

struct Run
{
    std::vector<float> left;
    std::vector<float> right;
    std::vector<float> dryLeft;
    std::size_t violationBlocks = 0;
    std::size_t violationCycle = 0;
    std::int64_t delayMinBefore = 0;
    std::int64_t delayEffectiveBefore = 0;
    rinne::loop::LoopStatusFields end;
    std::size_t violationEvents = 0;
    std::size_t floorRaisedEvents = 0;
    std::uint16_t violationPayload = 0;
    std::size_t canaryHits = 0;
    std::uint64_t writeHead = 0;
    std::uint64_t outputWritten = 0;
    std::uint64_t samples = 0;
    bool matchesReference = false;
};

// split 为真：违约块换成两个 512（对照）
Run runOne(float delayMs, bool split)
{
    Run run;
    auto host = makeHost(kSr, kDeclared);
    REQUIRE(host != nullptr);
    BlockScript script;
    for (std::size_t i = 0; i < kCycles; ++i)
    {
        if (i == kViolationAt)
        {
            if (split)
            {
                script.lengths.push_back(kDeclared);
                script.lengths.push_back(kDeclared);
            }
            else
            {
                script.lengths.push_back(kOversized);
            }
        }
        else
        {
            script.lengths.push_back(kDeclared);
        }
    }
    host->setBlockScripts(script, script);
    Recorder rec(*host, tones());
    const rinne::engine::EngineParams p = loopParams(50.0f, 50.0f, delayMs);
    for (std::size_t i = 0; i < script.lengths.size(); ++i)
    {
        if (i == kViolationAt)
        {
            run.delayMinBefore = host->inputStatus().delayMin;
            run.delayEffectiveBefore = host->inputStatus().delayEffective;
        }
        rec.cycle(i, p);
        if (host->inputLastBlock().hostViolation)
        {
            ++run.violationBlocks;
            run.violationCycle = i;
        }
    }
    host->drainEvents([&](const rinne::diag::Event& e) {
        if (e.code == static_cast<std::uint16_t>(LoopEventCode::kHostViolation))
        {
            ++run.violationEvents;
            run.violationPayload = static_cast<std::uint16_t>((e.a << 8) | e.b);
        }
        else if (e.code == static_cast<std::uint16_t>(LoopEventCode::kDMinRaised))
        {
            ++run.floorRaisedEvents;
        }
    });
    run.left = rec.outLeft;
    run.right = rec.outRight;
    run.dryLeft = rec.dryLeft;
    run.end = host->inputStatus();
    run.canaryHits = host->canaryHits();
    run.writeHead = host->writeHead();
    run.outputWritten = host->outputWrittenFrames();
    run.samples = rec.outLeft.size();
    run.matchesReference = rec.matchesReference();
    return run;
}

// 输出有限、回送 |out − dry| < 1.0
bool finiteAndBounded(const Run& run)
{
    for (std::size_t i = 0; i < run.left.size(); ++i)
    {
        if (!std::isfinite(run.left[i]) || !std::isfinite(run.right[i]) ||
            std::fabs(run.left[i] - run.dryLeft[i]) >= 1.0f)
        {
            return false;
        }
    }
    return true;
}

// 回送（输出减干声）相邻样本差的最大值，区间 [from, to)
float maxReturnStep(const Run& run, std::size_t from, std::size_t to)
{
    float step = 0.0f;
    for (std::size_t i = std::max<std::size_t>(from, 1); i < std::min(to, run.left.size()); ++i)
    {
        const float a = run.left[i] - run.dryLeft[i];
        const float b = run.left[i - 1] - run.dryLeft[i - 1];
        step = std::max(step, std::fabs(a - b));
    }
    return step;
}

class OversizedBlockScenario final : public Scenario
{
public:
    const char* id() const noexcept override { return "s08_oversized_block"; }

    bool run(ScenarioContext&) override
    {
        Verdict v;
        longDelay(v);
        shortDelay(v);
        return v.ok();
    }

private:
    // 通用断言：不越界、有限、写头恰为样本总数、违约只发生在那一块并留下一条事件
    static void common(Verdict& v, const Run& run, bool violated)
    {
        RIG_EQ(v, run.canaryHits, std::size_t{0});
        RIG_CHECK(v, finiteAndBounded(run));
        RIG_CHECK(v, run.matchesReference);
        RIG_EQ(v, run.writeHead, run.samples);
        RIG_EQ(v, run.outputWritten, run.samples);
        RIG_EQ(v, run.end.overrun, std::uint64_t{0});
        RIG_EQ(v, run.end.tapDrop, 0u);
        RIG_EQ(v, run.end.paired, 1u);
        RIG_EQ(v, run.violationBlocks, violated ? std::size_t{1} : std::size_t{0});
        RIG_EQ(v, run.violationEvents, violated ? std::size_t{1} : std::size_t{0});
        if (violated)
        {
            RIG_EQ(v, run.violationCycle, kViolationAt);
            RIG_EQ(v, run.violationPayload, static_cast<std::uint16_t>(kOversized));
            // 违约之后回到 512，下限与本会话最大块长都留着
            RIG_EQ(v, run.end.delayMin, std::int64_t{kOversized});
            RIG_EQ(v, run.end.maxBlockSeen, std::int64_t{kOversized});
            RIG_EQ(v, static_cast<int>(run.end.delayMinReason), static_cast<int>(DelayMinReason::kHostViolation));
        }
        else
        {
            RIG_EQ(v, run.end.delayMin, std::int64_t{kDeclared});
            RIG_EQ(v, run.end.maxBlockSeen, std::int64_t{kDeclared});
            RIG_EQ(v, static_cast<int>(run.end.delayMinReason), static_cast<int>(DelayMinReason::kPrepare));
        }
    }

    // D = 100 ms：下限抬到 1024 也盖不到 D，实际 D 不变，输出与切成两个 512 的对照逐位相同
    static void longDelay(Verdict& v)
    {
        INFO("D = 100 ms");
        const Run run = runOne(100.0f, false);
        const Run control = runOne(100.0f, true);
        common(v, run, true);
        common(v, control, false);
        RIG_EQ(v, run.delayEffectiveBefore, std::int64_t{4800});
        RIG_EQ(v, run.end.delayEffective, std::int64_t{4800});
        RIG_EQ(v, run.end.resyncCount, 0u);
        RIG_EQ(v, run.floorRaisedEvents, std::size_t{0});
        RIG_EQ(v, control.end.resyncCount, 0u);
        RIG_EQ(v, run.samples, control.samples);
        RIG_CHECK(v, sameBits(run.left, control.left, 0, run.samples));
        RIG_CHECK(v, sameBits(run.right, control.right, 0, run.samples));
        // 回送确实在响
        std::size_t audible = 0;
        for (std::size_t t = 0; t < run.samples; ++t)
        {
            audible += rinne::test::floatBits(run.left[t]) != rinne::test::floatBits(run.dryLeft[t]) ? 1u : 0u;
        }
        RIG_CHECK(v, audible > run.samples / 2);
    }

    // D = 12.5 ms（600）：违约块把下限抬到 1024，D 随之抬到 1024，一次改 D 重同步，没有补 0，回送没有阶跃
    static void shortDelay(Verdict& v)
    {
        INFO("D = 12.5 ms");
        const Run run = runOne(12.5f, false);
        const Run control = runOne(12.5f, true);
        common(v, run, true);
        common(v, control, false);
        RIG_EQ(v, run.delayEffectiveBefore, std::int64_t{600});
        RIG_EQ(v, run.end.delayEffective, std::int64_t{kOversized});
        RIG_EQ(v, run.end.resyncByReason[static_cast<std::size_t>(SyncReason::kDChange)], 1u);
        RIG_EQ(v, run.end.resyncCount, 1u);
        RIG_EQ(v, run.floorRaisedEvents, std::size_t{1});
        // 对照没有违约，D 一直是 600，也没有任何重同步
        RIG_EQ(v, control.end.delayEffective, std::int64_t{600});
        RIG_EQ(v, control.end.resyncCount, 0u);
        RIG_EQ(v, control.floorRaisedEvents, std::size_t{0});

        // 阶跃判据：回送是两个余弦（100、150 Hz，幅度 0.3）的回声叠加，幅度和不超过 0.6，余弦斜率不超过 0.6·ω(150 Hz)，
        // 再加 40 ms 交叉淡化与 600 样本淡出的斜率；硬切会跳一个 O(0.1) 的台阶
        const std::size_t mark = kViolationAt * static_cast<std::size_t>(kDeclared);
        const float slope = 0.6f * static_cast<float>(2.0 * 3.14159265358979 * 150.0 / kSr);
        const float bound = slope + 0.6f * (1.0f / 1920.0f + 1.0f / 600.0f) + 1.0e-4f;
        const float step = maxReturnStep(run, mark - 1, mark + 8000);
        INFO("max return step " << step << ", bound " << bound);
        RIG_CHECK(v, step <= bound);
    }
};

} // namespace

RINNE_RIG_SCENARIO("s08_oversized_block")
{
    return new OversizedBlockScenario();
}

TEST_CASE("s08: one block of twice the declared size lifts the D floor, stays in bounds and never steps",
          "[loop_rig][s08]")
{
    CHECK(rinne::rig::scn::runRegisteredScenario("s08_oversized_block"));
}
