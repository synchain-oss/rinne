// SPDX-License-Identifier: GPL-3.0-or-later
//
// S02 块长不变性（安全不变量 7）：参数恒定时，宿主怎么切块，闭合的环路输出都逐位相同。
//
// 做法：同一条环路（环路延迟 D = 4800，环内放一个一阶低通替身，截止 2 kHz，信号源是编号的纯函数、两个声道各一份噪声），
// 参数全程恒定，只换块长序列，跑 4 s（左右声道，Input 的输出）：
//   恒定 32 / 64 / 256 / 1024 / 4096
//   带种子的随机 1…4096（每块都变）
//   256 与 2048 交替
// 两端每个周期用同一个块长，块长都不大于声明的最大块长 4096。三遍：
//   反馈 50 %、D = 100 ms；反馈 100 %（限制器与失控保护都会介入）、D = 100 ms；
//   反馈 50 %、D = 25 ms：比声明的最大块长还短，D 的下限是声明值而不是实测块长，所以每个序列的实际 D 都是 4096，
//   小块序列不会因为「见过的块都很小」而用 1200。
//
// 断言（每一遍）：
//   - 七个序列的输出从第一个样本起逐位相同（左右声道），没有「配对之前」「淡入之中」的豁免：
//     配对、首个有效读样本、淡入和读头 onset 全按样本定位，与怎么切块无关；
//   - 每个序列零重同步、没有补 0、没有违约（块长不大于声明值）、Input 一直配着对端，实际 D 与下限都与序列无关；
//   - 每个序列的输出与参照安全链按文档接线重算的结果逐位相同；
//   - 回送确实在响：4 s 里输出不等于干声的样本数超过一半（逐位相同不是两路都静音的平凡相等）。
//
// 标 ⛔ 的是删除式验证点：把对应的实现改坏，本场景必须变红。
//   ⛔ D 的下限不看声明的最大块长、只看实测块长 → D = 25 ms 一遍里小块序列用 1200、大块序列用 4096，输出不再相同。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "s01_rig_kit.h"

namespace
{

using namespace rinne::rig;
using namespace rinne::rig::scn;

constexpr int kMaxBlock = 4096;
constexpr float kDelayMs = 100.0f; // 4800 个样本
constexpr float kShortDelayMs = 25.0f; // 1200 个样本，比声明的最大块长短：D 被抬到下限 4096
constexpr std::int64_t kDeclared = 4096;
constexpr std::int64_t kSamples = 4 * 48000;

// 把总长 total 随机切成长度在 [lo, hi] 里的块（总长不小于 total）
std::vector<int> randomLengths(rinne::test::Rng& rng, std::int64_t total, int lo, int hi)
{
    std::vector<int> lengths;
    for (std::int64_t sum = 0; sum < total;)
    {
        const int n = lo + rng.below(hi - lo + 1);
        lengths.push_back(n);
        sum += n;
    }
    return lengths;
}

SourceFn noise()
{
    return [](std::uint64_t first, float* left, float* right, int n) {
        for (int i = 0; i < n; ++i)
        {
            left[i] = 0.25f * hashNoise(first + static_cast<std::uint64_t>(i), 5);
            right[i] = 0.25f * hashNoise(first + static_cast<std::uint64_t>(i), 6);
        }
    };
}

struct Sequence
{
    std::string name;
    std::vector<int> lengths;
};

struct Run
{
    std::vector<float> left;
    std::vector<float> right;
    std::uint32_t resyncs = 0;
    std::uint64_t overrun = 0;
    std::int64_t delayEffective = 0;
    std::int64_t delayMin = 0;
    bool violation = false;
    bool paired = false;
    bool matchesReference = false;
    std::size_t audible = 0;
};

Run runOne(const Sequence& sequence, float feedbackPercent, float delayMs)
{
    Run run;
    auto host = makeHost(kSr, kMaxBlock);
    REQUIRE(host != nullptr);
    BlockScript script;
    script.lengths = sequence.lengths;
    host->setBlockScripts(script, script);
    host->setChain(ChainKind::kOnePoleLowpass);
    host->chain().setLowpassHz(2000.0f);

    Recorder rec(*host, noise());
    const rinne::engine::EngineParams p = loopParams(feedbackPercent, 50.0f, delayMs);
    for (std::size_t i = 0; rec.outLeft.size() < static_cast<std::size_t>(kSamples); ++i)
    {
        if (rec.cycle(i, p) <= 0)
        {
            break;
        }
        run.violation = run.violation || host->inputLastBlock().hostViolation;
    }
    run.left = rec.outLeft;
    run.right = rec.outRight;
    run.matchesReference = rec.matchesReference();
    const auto& status = host->inputStatus();
    run.resyncs = status.resyncCount;
    run.overrun = status.overrun;
    run.delayEffective = status.delayEffective;
    run.delayMin = status.delayMin;
    run.paired = status.paired == 1u;
    for (std::size_t t = 0; t < static_cast<std::size_t>(kSamples) && t < rec.outLeft.size(); ++t)
    {
        run.audible += rinne::test::floatBits(rec.outLeft[t]) != rinne::test::floatBits(rec.dryLeft[t]) ? 1u : 0u;
    }
    return run;
}

class BlockInvarianceScenario final : public Scenario
{
public:
    const char* id() const noexcept override { return "s02_block_invariance"; }

    bool run(ScenarioContext&) override
    {
        Verdict v;
        rinne::test::Rng rng(0x5eed0002u);
        const std::vector<Sequence> sequences = {
            {"constant 256", {256}},
            {"constant 32", {32}},
            {"constant 64", {64}},
            {"constant 1024", {1024}},
            {"constant 4096", {4096}},
            {"random 1..4096", randomLengths(rng, kSamples, 1, kMaxBlock)},
            {"alternating 256/2048", {256, 2048}},
        };

        struct Set
        {
            float feedback;
            float delayMs;
            std::int64_t expectDelay; // 实际的 D：要的 D 与声明的最大块长里大的那个
        };
        for (const Set set :
             {Set{50.0f, kDelayMs, 4800}, Set{100.0f, kDelayMs, 4800}, Set{50.0f, kShortDelayMs, kDeclared}})
        {
            INFO("feedback = " << set.feedback << " %, delay = " << set.delayMs << " ms");
            std::vector<Run> runs;
            for (const Sequence& sequence : sequences)
            {
                runs.push_back(runOne(sequence, set.feedback, set.delayMs));
            }
            const auto total = static_cast<std::size_t>(kSamples);
            for (std::size_t r = 0; r < runs.size(); ++r)
            {
                INFO("sequence " << sequences[r].name);
                const Run& run = runs[r];
                RIG_CHECK(v, run.left.size() >= total);
                RIG_EQ(v, run.resyncs, 0u);
                RIG_EQ(v, run.overrun, std::uint64_t{0});
                RIG_CHECK(v, !run.violation);
                RIG_CHECK(v, run.paired);
                RIG_EQ(v, run.delayEffective, set.expectDelay);
                RIG_EQ(v, run.delayMin, kDeclared);
                RIG_CHECK(v, run.matchesReference);
                RIG_CHECK(v, run.audible > total / 2);
                if (r > 0)
                {
                    RIG_CHECK(v, sameBits(run.left, runs[0].left, 0, total));
                    RIG_CHECK(v, sameBits(run.right, runs[0].right, 0, total));
                }
            }
        }
        return v.ok();
    }
};

} // namespace

RINNE_RIG_SCENARIO("s02_block_invariance")
{
    return new BlockInvarianceScenario();
}

TEST_CASE("s02: the closed loop gives the same bits however the host splits the blocks", "[loop_rig][s02]")
{
    CHECK(rinne::rig::scn::runRegisteredScenario("s02_block_invariance"));
}
