// SPDX-License-Identifier: GPL-3.0-or-later
//
// S11 同组两个 Output（接管）：两个 Output 实例都在同一条环上被调用，写权（环头里的 token）从一个换给另一个，
// 再换回来。接管的瞬间两个 Output 都在被调用，谁都想写；环上只能有一个写方。
//
// 环路延迟 D = 4800，块长 256（恒定）或 32…512（带种子随机），混合 50 %，Input 先 Output 后，
// 第二个 Output 紧跟在第一个之后。两格：
//   甲 双写  两个 Output 从头到尾每个周期都被调用。第 3 s 起写权交给第二个，第 6 s 起交回第一个；
//   乙 老写方消失  第二个 Output 只在写权给它的那段（第 3 s 到第 6 s）被调用，第一个这段时间不被调用
//                 （它所在的轨道停了），写权交回时第二个不再被调用。
// 一遍反馈 0（噪声信号源，环里存的就是干声，读头的内容逐样本对得回信号源），一遍反馈 50 %（两个声道各一个余弦，
// 回送在响，包络看得见）。
//
// 断言：
//   - 每个周期之后，环的写头比上一周期恰多这一块的长度（W 每周期只增加 n：不是 0，也不是 2n），写公告恒等于写头
//     （没有谁公告了一段而没发布）；
//   - 写权在谁手里谁写：它每个被调用的周期写满 n 帧，另一个即使被调用也一帧不写（写入计数不动）、冲突计数每次调用加 1、
//     状态是冲突；接管的那个周期持有者就写了，没有丢掉任何一块（规格允许合法持有者最多 1 块之后恢复，这里要求不丢）；
//   - 整个跑下来恰 2 次重同步（两次接管各一次），原因都是换写方；没有跳变、位移、断档、容忍模式；
//     没有补 0、撕裂、被环覆盖的帧：Input 的读头没有读到混写的块；
//   - 反馈 0 一遍：回送样本 t 来自信号源的样本 t − D，在接管的周期（Input 在那个周期的开头看到写方变了、重建读头）起到
//     新读头的数据到达并走完 20 ms 的 onset 之前被压低，其余样本比值恰为 1（接管不丢也不改数据，流是连续的）；
//     任何非零回送样本的符号与 t − D 处的信号源相同、幅度不大于它；
//   - 反馈 50 % 一遍：输出与参照安全链按文档接线重算的结果逐位相同；每次接管之后，回送从首个有效读样本起 500 ms 里
//     幅度不超过线性淡入曲线（淡入曲线 × 接管之前稳态的回送峰值，留 5 % 加 2e-3 的余量）；首个有效读样本之前
//     （10 ms 下行走完之后）是静音；淡入走完之后回送回到稳态（峰值不低于稳态的 90 %）；每次接管淡入的原因只有
//     换写方（见 S25）。
//
// 标 ⛔ 的是删除式验证点：把对应的实现改坏，本场景必须变红。
//   ⛔ Output 不核对自己是不是写方（谁被调用谁写）→ 第二个写了环，W 每周期增加 2n；
//   ⛔ Input 看到写方变了不重建读头 → 重同步计数与读头的内容对不上；
//   ⛔ 换写方不请求淡入（回送从首个有效读样本起直接满幅）→ 包络超过线性淡入曲线。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "engine/OutputEngine.h"
#include "loop/LoopEvents.h"
#include "loop/StreamSync.h"
#include "s01_rig_kit.h"

namespace
{

using namespace rinne::rig;
using namespace rinne::rig::scn;
using rinne::engine::WriteState;
using rinne::loop::SyncReason;

constexpr int kMaxBlock = 512;
constexpr std::int64_t kDelay = 4800;
constexpr std::int64_t kOnset = 960; // 新读头的 onset：20 ms
constexpr std::int64_t kSecond = 48000;
constexpr std::int64_t kFirstSwitch = 3 * kSecond;
constexpr std::int64_t kSecondSwitch = 6 * kSecond;
constexpr std::int64_t kTotal = 9 * kSecond + kSecond / 2;
constexpr std::int64_t kSettled = 40000; // 配对的淡入（500 ms）走完之后

enum class Blocks
{
    kFixed,
    kRandom
};

struct Cell
{
    const char* name;
    bool bothAlwaysCalled; // 甲：两个 Output 一直被调用；乙：老写方消失
};

struct Cycle
{
    std::int64_t start = 0;
    int samples = 0;
    std::uint64_t writeHead = 0;
    std::uint64_t intent = 0;
    std::uint64_t firstWritten = 0;
    std::uint64_t secondWritten = 0;
    std::uint32_t firstConflicts = 0;
    std::uint32_t secondConflicts = 0;
    WriteState firstState = WriteState::kIdle;
    WriteState secondState = WriteState::kIdle;
    bool firstCalled = true;
    bool secondCalled = false;
    bool holderIsSecond = false;
};

struct Trace
{
    std::vector<Cycle> cycles;
    std::size_t firstSwitchCycle = 0;
    std::size_t secondSwitchCycle = 0;
    std::vector<float> srcLeft, tapLeft, retLeft, outLeft;
    std::vector<std::uint8_t> tapValid;
    bool matchesReference = false;
    std::uint32_t resyncs = 0;
    std::uint32_t byReason[9]{};
    std::uint32_t tolerant = 0;
    std::uint64_t overrun = 0;
    std::uint64_t overflow = 0;
    std::uint64_t torn = 0;
    std::size_t canaryHits = 0;
};

SourceFn noise()
{
    return [](std::uint64_t first, float* left, float* right, int n) {
        for (int i = 0; i < n; ++i)
        {
            left[i] = 0.25f * hashNoise(first + static_cast<std::uint64_t>(i), 21);
            right[i] = 0.25f * hashNoise(first + static_cast<std::uint64_t>(i), 22);
        }
    };
}

Trace runOne(const Cell& cell, Blocks blocks, float feedbackPercent)
{
    Trace trace;
    rinne::test::Rng rng(0x5a110011u);
    BlockScript script;
    for (std::int64_t sum = 0; sum < kTotal + 2 * kMaxBlock;)
    {
        const int n = blocks == Blocks::kFixed ? 256 : 32 + rng.below(kMaxBlock - 32 + 1);
        script.lengths.push_back(n);
        sum += n;
    }
    // 两次接管的周期：第 3 s、第 6 s 之后的第一个周期
    std::size_t first = 0;
    std::size_t second = 0;
    std::int64_t position = 0;
    for (std::size_t i = 0; position < kSecondSwitch; ++i)
    {
        if (position >= kFirstSwitch && first == 0)
        {
            first = i;
        }
        position += script.lengths[i];
        second = i + 1;
    }
    trace.firstSwitchCycle = first;
    trace.secondSwitchCycle = second;

    BlockScript outputScript = script;
    outputScript.skips.assign(script.lengths.size(), false);
    if (!cell.bothAlwaysCalled)
    {
        for (std::size_t i = first; i < second; ++i)
        {
            outputScript.skips[i] = true;
        }
    }

    auto host = makeHost(kSr, kMaxBlock);
    REQUIRE(host != nullptr);
    host->setBlockScripts(script, outputScript);
    REQUIRE(host->addSecondOutput());
    host->setSecondOutputActive(cell.bothAlwaysCalled);
    const SourceFn source = feedbackPercent == 0.0f ? noise() : tones();
    Recorder rec(*host, source);
    const rinne::engine::EngineParams p = loopParams(feedbackPercent, 50.0f, 100.0f);

    std::int64_t cursor = 0;
    for (std::size_t i = 0; cursor < kTotal; ++i)
    {
        if (i == first)
        {
            host->switchWriter(true);
            host->setSecondOutputActive(true);
        }
        if (i == second)
        {
            host->switchWriter(false);
            host->setSecondOutputActive(cell.bothAlwaysCalled);
        }
        Cycle c;
        c.start = cursor;
        c.holderIsSecond = i >= first && i < second;
        c.firstCalled = !outputScript.skipAt(i);
        c.secondCalled = cell.bothAlwaysCalled || c.holderIsSecond;
        const int n = rec.cycle(i, p);
        if (n <= 0)
        {
            break;
        }
        cursor += n;
        c.samples = n;
        c.writeHead = host->writeHead();
        c.intent = host->writeIntent();
        c.firstWritten = host->outputWrittenFrames();
        c.secondWritten = host->secondOutputWrittenFrames();
        c.firstConflicts = host->outputConflictCount();
        c.secondConflicts = host->secondOutputConflictCount();
        c.firstState = host->outputState();
        c.secondState = host->secondOutputState();
        trace.cycles.push_back(c);
    }
    trace.srcLeft = rec.dryLeft;
    trace.tapLeft = rec.reference.tapLeft;
    // 回送取引擎自己的输出减去干声（混合 50 % 时干声增益恰为 1），不取参照的：参照的淡入是它自己请求的
    trace.retLeft.resize(rec.outLeft.size());
    for (std::size_t i = 0; i < rec.outLeft.size(); ++i)
    {
        trace.retLeft[i] = rec.outLeft[i] - rec.dryLeft[i];
    }
    trace.tapValid = rec.reference.tapValid;
    trace.outLeft = rec.outLeft;
    trace.matchesReference = rec.matchesReference();
    const auto& status = host->inputStatus();
    trace.resyncs = status.resyncCount;
    for (int r = 0; r < 9; ++r)
    {
        trace.byReason[r] = status.resyncByReason[r];
    }
    trace.tolerant = status.tolerant;
    trace.overrun = status.overrun;
    trace.overflow = status.overflow;
    trace.torn = status.torn;
    trace.canaryHits = host->canaryHits();
    return trace;
}

class TwoOutputsScenario final : public Scenario
{
public:
    const char* id() const noexcept override { return "s11_two_outputs"; }

    bool run(ScenarioContext&) override
    {
        Verdict v;
        const Cell cells[] = {
            {"both outputs always called", true},
            {"old writer disappears", false},
        };
        for (const Blocks blocks : {Blocks::kFixed, Blocks::kRandom})
        {
            for (const Cell& cell : cells)
            {
                for (const float feedback : {0.0f, 50.0f})
                {
                    INFO("cell " << cell.name << ", blocks " << (blocks == Blocks::kFixed ? "fixed 256" : "random")
                                 << ", feedback " << feedback << " %");
                    const Trace trace = runOne(cell, blocks, feedback);
                    checkStructure(v, cell, trace);
                    if (feedback == 0.0f)
                    {
                        checkContent(v, trace);
                    }
                    else
                    {
                        checkEnvelope(v, trace);
                    }
                }
            }
        }
        return v.ok();
    }

private:
    static void checkStructure(Verdict& v, const Cell& cell, const Trace& t)
    {
        RIG_CHECK(v, t.cycles.size() > t.secondSwitchCycle + 100);
        bool writeHeadOk = true;
        bool intentOk = true;
        bool holderWrites = true;
        bool otherSilent = true;
        bool conflictsOk = true;
        Cycle before;
        for (const Cycle& c : t.cycles)
        {
            writeHeadOk = writeHeadOk && c.writeHead == before.writeHead + static_cast<std::uint64_t>(c.samples);
            intentOk = intentOk && c.intent == c.writeHead;
            const auto n = static_cast<std::uint64_t>(c.samples);
            const bool holderCalled = c.holderIsSecond ? c.secondCalled : c.firstCalled;
            const bool otherCalled = c.holderIsSecond ? c.firstCalled : c.secondCalled;
            const std::uint64_t holderDelta =
                c.holderIsSecond ? c.secondWritten - before.secondWritten : c.firstWritten - before.firstWritten;
            const std::uint64_t otherDelta =
                c.holderIsSecond ? c.firstWritten - before.firstWritten : c.secondWritten - before.secondWritten;
            const std::uint32_t otherConflicts = c.holderIsSecond ? c.firstConflicts - before.firstConflicts
                                                                  : c.secondConflicts - before.secondConflicts;
            const std::uint32_t holderConflicts = c.holderIsSecond ? c.secondConflicts - before.secondConflicts
                                                                   : c.firstConflicts - before.firstConflicts;
            holderWrites = holderWrites && holderCalled && holderDelta == n && holderConflicts == 0u;
            otherSilent = otherSilent && otherDelta == 0u;
            conflictsOk = conflictsOk && otherConflicts == (otherCalled ? 1u : 0u);
            if (otherCalled)
            {
                const WriteState state = c.holderIsSecond ? c.firstState : c.secondState;
                conflictsOk = conflictsOk && state == WriteState::kConflict;
            }
            before = c;
        }
        RIG_CHECK(v, writeHeadOk);
        RIG_CHECK(v, intentOk);
        RIG_CHECK(v, holderWrites);
        RIG_CHECK(v, otherSilent);
        RIG_CHECK(v, conflictsOk);
        // 总数：两个 Output 写的帧数加起来恰为全部样本，第二个只在持有写权的那段写过
        const Cycle& last = t.cycles.back();
        const Cycle& beforeSecondHold = t.cycles[t.firstSwitchCycle - 1];
        const Cycle& endSecondHold = t.cycles[t.secondSwitchCycle - 1];
        RIG_EQ(v, last.firstWritten + last.secondWritten, last.writeHead);
        RIG_EQ(v, beforeSecondHold.secondWritten, std::uint64_t{0});
        RIG_EQ(v, last.secondWritten, endSecondHold.secondWritten);
        if (cell.bothAlwaysCalled)
        {
            RIG_EQ(v, beforeSecondHold.secondConflicts, static_cast<std::uint32_t>(t.firstSwitchCycle));
        }

        RIG_EQ(v, t.resyncs, 2u);
        RIG_EQ(v, t.byReason[static_cast<std::size_t>(SyncReason::kWriter)], 2u);
        RIG_EQ(v, t.tolerant, 0u);
        RIG_EQ(v, t.overrun, std::uint64_t{0});
        RIG_EQ(v, t.overflow, std::uint64_t{0});
        RIG_EQ(v, t.torn, std::uint64_t{0});
        RIG_EQ(v, t.canaryHits, std::size_t{0});
        RIG_CHECK(v, t.matchesReference);
    }

    static void checkContent(Verdict& v, const Trace& t)
    {
        const auto total = static_cast<std::int64_t>(t.tapLeft.size());
        const std::int64_t resyncs[2] = {t.cycles[t.firstSwitchCycle].start, t.cycles[t.secondSwitchCycle].start};
        bool signAndBound = true;
        bool exactOutside = true;
        std::size_t degraded = 0;
        for (std::int64_t at = kSettled; at < total; ++at)
        {
            const float want = t.srcLeft[static_cast<std::size_t>(at - kDelay)];
            const float got = t.tapLeft[static_cast<std::size_t>(at)];
            bool inWindow = false;
            for (const std::int64_t r : resyncs)
            {
                inWindow = inWindow || (at >= r && at < r + kDelay + kOnset);
            }
            if (got != 0.0f)
            {
                signAndBound = signAndBound && (got > 0.0f) == (want > 0.0f) &&
                               std::fabs(got) <= std::fabs(want) * (1.0f + 1.0e-6f);
            }
            if (!inWindow)
            {
                exactOutside = exactOutside && std::fabs(got - want) <= 1.0e-6f;
            }
            else if (std::fabs(got - want) > 1.0e-6f)
            {
                ++degraded;
            }
        }
        RIG_CHECK(v, signAndBound);
        RIG_CHECK(v, exactOutside);
        // 每次接管之后都有一段被压低的回送（读头确实重建过）
        RIG_CHECK(v, degraded > static_cast<std::size_t>(2 * kDelay / 2));
    }

    static void checkEnvelope(Verdict& v, const Trace& t)
    {
        const std::int64_t resyncs[2] = {t.cycles[t.firstSwitchCycle].start, t.cycles[t.secondSwitchCycle].start};
        for (const std::int64_t r : resyncs)
        {
            INFO("takeover at sample " << r);
            // 接管之前的稳态峰值
            const auto steady = static_cast<double>(
                peakOf(t.retLeft, static_cast<std::size_t>(r - kSecond), static_cast<std::size_t>(r)));
            RIG_CHECK(v, steady > 0.05);
            // 10 ms 下行走完之后，到首个有效读样本为止：静音
            std::int64_t firstValid = r + kGateSamples;
            while (firstValid < static_cast<std::int64_t>(t.tapValid.size()) &&
                   t.tapValid[static_cast<std::size_t>(firstValid)] == 0)
            {
                ++firstValid;
            }
            RIG_CHECK(v, firstValid < static_cast<std::int64_t>(t.tapValid.size()));
            RIG_CHECK(v, firstValid - r >= kDelay - 2 * kMaxBlock);
            RIG_CHECK(v, allZero(t.retLeft, static_cast<std::size_t>(r + kGateSamples),
                                 static_cast<std::size_t>(firstValid)));
            // 首个有效读样本起 500 ms：不超过线性淡入曲线
            bool underRamp = true;
            for (std::int64_t at = firstValid; at < firstValid + kFadeInSamples; ++at)
            {
                const double ramp = static_cast<double>(at - firstValid + 1) / static_cast<double>(kFadeInSamples);
                underRamp = underRamp && std::fabs(static_cast<double>(t.retLeft[static_cast<std::size_t>(at)])) <=
                                             ramp * steady * 1.05 + 2.0e-3;
            }
            RIG_CHECK(v, underRamp);
            // 淡入走完之后回到稳态
            const double after = static_cast<double>(
                peakOf(t.retLeft, static_cast<std::size_t>(firstValid + kFadeInSamples + kSecond / 4),
                       static_cast<std::size_t>(firstValid + kFadeInSamples + kSecond)));
            RIG_CHECK(v, after >= 0.9 * steady);
        }
    }
};

} // namespace

RINNE_RIG_SCENARIO("s11_two_outputs")
{
    return new TwoOutputsScenario();
}

TEST_CASE("s11: two Outputs on one ring, only the writer writes and the loopback fades in after the takeover",
          "[loop_rig][s11]")
{
    CHECK(rinne::rig::scn::runRegisteredScenario("s11_two_outputs"));
}
