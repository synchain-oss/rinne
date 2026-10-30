// SPDX-License-Identifier: GPL-3.0-or-later
//
// S24 单端切片（安全不变量 7 的另一半）：参数恒定时，只有一端把宿主给的块再切成小块来调用，输出也与不切时逐位相同。
// S02 钉的是两端用同一个块长序列时与块长无关；真实的宿主会让两个插件各自被切（一条轨道上开了预读、另一条没有），
// 这里钉的是两端的块边界不必对齐。
//
// 做法：同一条环路（环路延迟 D = 4800，环内放一个一阶低通替身，截止 2 kHz，信号源是编号的纯函数、两个声道各一份噪声），
// 参数全程恒定，宿主每个周期给两端同一个块长（256…1024 带种子随机，声明的最大块长 1024）。基线两端都整块调用；
// 对照格只把一端（Input 或 Output）每个周期的块再切成长度在 [1, m] 的带种子随机小块依次调用，另一端仍整块，
// m 取 3、64、300。反馈 50 % 与 100 %（限制器与失控保护都会介入）各一遍，跑 3 s，记 Input 的输出（左右声道）。
// 走带不变（没有起播沿），小块拿到同一份走带信息。
//
// 断言（每一个对照格，对同一遍的基线）：
//   - Input 的输出从第一个样本起逐位相同（左右声道），没有「配对之前」「淡入之中」的豁免；
//   - 环的写头（Output 写进环的帧数）相同，零重同步、没有补 0、没有违约、实际 D 与下限都与基线相同；
//   - 回送确实在响：输出不等于干声的样本数超过一半（逐位相同不是两路都静音的平凡相等）。
//
// 标 ⛔ 的是删除式验证点：把对应的实现改坏，本场景必须变红。
//   ⛔ 对齐检查不再只在写头动了的那一块做（每次调用都算一次）→ 切成小块的 Input 在一个周期里连续几次对不齐，
//      重同步，输出变；
//   ⛔ Output 对小块（少于 8 帧）不写环 → 切成小块的 Output 与基线的写头、输出都不再相同。

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

constexpr int kMaxBlock = 1024;
constexpr std::int64_t kSamples = 3 * 48000;

SourceFn noise()
{
    return [](std::uint64_t first, float* left, float* right, int n) {
        for (int i = 0; i < n; ++i)
        {
            left[i] = 0.25f * hashNoise(first + static_cast<std::uint64_t>(i), 7);
            right[i] = 0.25f * hashNoise(first + static_cast<std::uint64_t>(i), 8);
        }
    };
}

struct Run
{
    std::vector<float> left;
    std::vector<float> right;
    std::vector<float> dryLeft;
    std::uint64_t writeHead = 0;
    std::uint32_t resyncs = 0;
    std::uint64_t overrun = 0;
    std::int64_t delayEffective = 0;
    std::int64_t delayMin = 0;
    bool violation = false;
    bool paired = false;
};

// slicedInput / slicedOutput 至多一个为真；maxSlice 为 0 = 都整块（基线）
Run runOne(float feedbackPercent, bool slicedInput, bool slicedOutput, int maxSlice)
{
    Run run;
    auto host = makeHost(kSr, kMaxBlock);
    REQUIRE(host != nullptr);
    rinne::test::Rng rng(0x5eed0024u);
    BlockScript script;
    for (std::int64_t sum = 0; sum < kSamples;)
    {
        const int n = 256 + rng.below(kMaxBlock - 256 + 1);
        script.lengths.push_back(n);
        sum += n;
    }
    host->setBlockScripts(script, script);
    host->setChain(ChainKind::kOnePoleLowpass);
    host->chain().setLowpassHz(2000.0f);
    if (slicedInput)
    {
        host->setSlicing(true, maxSlice, 0x1234abcdu);
    }
    if (slicedOutput)
    {
        host->setSlicing(false, maxSlice, 0x9876fedcu);
    }
    const SourceFn source = noise();
    host->setSource(source);
    ParamsScript params;
    params.snapshots = {loopParams(feedbackPercent, 50.0f, 100.0f)};
    host->setParamsScript(params);
    for (std::size_t i = 0; run.left.size() < static_cast<std::size_t>(kSamples); ++i)
    {
        const std::uint64_t first = host->sourcePosition();
        const int n = host->runCycle(i);
        REQUIRE(n > 0);
        const auto count = static_cast<std::size_t>(n);
        std::vector<float> dl(count);
        std::vector<float> dr(count);
        source(first, dl.data(), dr.data(), n);
        run.dryLeft.insert(run.dryLeft.end(), dl.begin(), dl.end());
        run.left.insert(run.left.end(), host->inputOutputLeft().begin(),
                        host->inputOutputLeft().begin() + static_cast<std::ptrdiff_t>(count));
        run.right.insert(run.right.end(), host->inputOutputRight().begin(),
                         host->inputOutputRight().begin() + static_cast<std::ptrdiff_t>(count));
        run.violation = run.violation || host->inputLastBlock().hostViolation;
    }
    run.writeHead = host->writeHead();
    const auto& status = host->inputStatus();
    run.resyncs = status.resyncCount;
    run.overrun = status.overrun;
    run.delayEffective = status.delayEffective;
    run.delayMin = status.delayMin;
    run.paired = status.paired == 1u;
    return run;
}

class SingleEndSlicingScenario final : public Scenario
{
public:
    const char* id() const noexcept override { return "s24_single_end_slicing"; }

    bool run(ScenarioContext&) override
    {
        Verdict v;
        for (const float feedback : {50.0f, 100.0f})
        {
            const Run base = runOne(feedback, false, false, 0);
            const auto total = static_cast<std::size_t>(kSamples);
            RIG_CHECK(v, base.left.size() >= total);
            RIG_EQ(v, base.resyncs, 0u);
            RIG_EQ(v, base.overrun, std::uint64_t{0});
            RIG_CHECK(v, base.paired);
            std::size_t audible = 0;
            for (std::size_t t = 0; t < total; ++t)
            {
                audible += rinne::test::floatBits(base.left[t]) != rinne::test::floatBits(base.dryLeft[t]) ? 1u : 0u;
            }
            RIG_CHECK(v, audible > total / 2);

            for (const bool inputSide : {true, false})
            {
                for (const int maxSlice : {3, 64, 300})
                {
                    INFO("feedback " << feedback << " %, " << (inputSide ? "Input" : "Output") << " sliced into 1.."
                                     << maxSlice);
                    const Run sliced = runOne(feedback, inputSide, !inputSide, maxSlice);
                    RIG_CHECK(v, sameBits(sliced.left, base.left, 0, total));
                    RIG_CHECK(v, sameBits(sliced.right, base.right, 0, total));
                    RIG_EQ(v, sliced.left.size(), base.left.size());
                    RIG_EQ(v, sliced.writeHead, base.writeHead);
                    RIG_EQ(v, sliced.resyncs, 0u);
                    RIG_EQ(v, sliced.overrun, std::uint64_t{0});
                    RIG_CHECK(v, !sliced.violation);
                    RIG_CHECK(v, sliced.paired);
                    RIG_EQ(v, sliced.delayEffective, base.delayEffective);
                    RIG_EQ(v, sliced.delayMin, base.delayMin);
                }
            }
        }
        return v.ok();
    }
};

} // namespace

RINNE_RIG_SCENARIO("s24_single_end_slicing")
{
    return new SingleEndSlicingScenario();
}

TEST_CASE("s24: slicing only one end's blocks into smaller pieces does not change a bit of the output",
          "[loop_rig][s24]")
{
    CHECK(rinne::rig::scn::runRegisteredScenario("s24_single_end_slicing"));
}
