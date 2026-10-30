// SPDX-License-Identifier: GPL-3.0-or-later
//
// S16 单声道 / 立体声四种组合：Input 与 Output 各自可以是单声道或立体声（两端在不同的轨道上也能配成一对），
// 环恒按立体声步长（两个声道格），映射正确，不越界。
//
// 映射的约定（AudioView.h、InputEngine.h、OutputEngine.h）：
//   Output 单声道  写环的两格都是第 0 个声道（左复制到右）；Output 立体声  写环的两格是左、右；
//   Input 立体声   读头的两格各加到自己的声道上；Input 单声道  读头取两格的平均 0.5·(L + R) 加到第 0 个声道上，
//                  轨道缓冲的第 1 个声道不碰。
// 试验台里两端读写的是同一份轨道缓冲：单声道的一端只看到第 0 个声道，第 1 个声道（右）它不碰。
//
// 四种组合 (Input, Output) = (立体声, 立体声) / (立体声, 单声道) / (单声道, 立体声) / (单声道, 单声道)，
// 反馈 0 与 50 % 各做一遍（混合 50 %，环路延迟 D = 4800，块长 1…512 带种子随机，信号是两个声道不同的余弦）。
//
// 断言（每一格）：
//   - 读头（环里读出的两格，安全链的输入）从首个有效读样本过了 onset 之后逐位等于 D 个样本之前 Output 写的那一对：
//     Output 立体声时是（左，右），单声道时是（左，左）；
//   - Input 的输出与参照安全链按文档接线重算的结果逐位相同（单声道时参照取平均）；
//   - 单声道 Input 不碰轨道缓冲的第 1 个声道：它在 Input 之后与干声逐位相同；
//   - Output 不改动宿主缓冲（它处理之后看到的轨道缓冲与 Input 的输出逐位相同）；
//   - 环的写头恰为样本总数，没有补 0，没有重同步；越界哨兵没有被改动；
//   - 回送确实在响（反馈 50 % 时输出不等于干声的样本数超过一半），两个声道在立体声 / 立体声一格里读头不同
//     （映射没有把两个声道混成一个）。
//
// 标 ⛔ 的是删除式验证点：把对应的实现改坏，本场景必须变红。
//   ⛔ 单声道 Input 只取读头的左格、不取平均 → 单声道 Input 配立体声 Output 的两格与参照对不上；
//   ⛔ 单声道 Output 写环时右格写 0、不复制左声道 → 读头的右格与应有的（左，左）对不上。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "s01_rig_kit.h"

namespace
{

using namespace rinne::rig;
using namespace rinne::rig::scn;

constexpr int kMaxBlock = 512;
constexpr std::int64_t kDelay = 4800;
constexpr std::int64_t kSamples = 3 * 48000;
constexpr std::int64_t kOnset = 960; // 读头的 onset：20 ms

struct Combo
{
    const char* name;
    int inputChannels;
    int outputChannels;
};

struct Recording
{
    std::vector<float> dryLeft, dryRight, outLeft, outRight, fedLeft, fedRight;
    std::vector<float> tapLeft, tapRight;
    std::vector<float> wantLeft, wantRight;
    bool matchesReference = false;
    std::size_t canaryHits = 0;
    std::uint64_t writeHead = 0;
    rinne::loop::LoopStatusFields status;
};

Recording runCombo(const Combo& combo, float feedbackPercent)
{
    Recording rec;
    auto host = makeHost(kSr, kMaxBlock);
    REQUIRE(host != nullptr);
    rinne::test::Rng rng(0x16c0de);
    BlockScript script;
    for (std::int64_t sum = 0; sum < kSamples;)
    {
        const int n = 1 + rng.below(kMaxBlock);
        script.lengths.push_back(n);
        sum += n;
    }
    host->setBlockScripts(script, script);
    host->setChannels(combo.inputChannels, combo.outputChannels);
    const bool mono = combo.inputChannels == 1;
    const SourceFn source = tones();
    host->setSource(source);
    rinne::test::InputReference reference(kSr);
    const rinne::engine::EngineParams p = loopParams(feedbackPercent, 50.0f, 100.0f);
    ParamsScript params;
    params.snapshots = {p};
    host->setParamsScript(params);

    for (std::size_t i = 0; i < script.lengths.size(); ++i)
    {
        const std::uint64_t first = host->sourcePosition();
        const int n = host->runCycle(i);
        REQUIRE(n > 0);
        const auto count = static_cast<std::size_t>(n);
        std::vector<float> dl(count);
        std::vector<float> dr(count);
        source(first, dl.data(), dr.data(), n);
        reference.block(p, host->inputLastBlock(), false, dl.data(), mono ? nullptr : dr.data(), n);
        const auto append = [count](std::vector<float>& to, const std::vector<float>& from) {
            to.insert(to.end(), from.begin(), from.begin() + static_cast<std::ptrdiff_t>(count));
        };
        append(rec.dryLeft, dl);
        append(rec.dryRight, dr);
        append(rec.outLeft, host->inputOutputLeft());
        append(rec.outRight, host->inputOutputRight());
        append(rec.fedLeft, host->outputFedLeft());
        append(rec.fedRight, host->outputFedRight());
    }
    rec.tapLeft = reference.tapLeft;
    rec.tapRight = reference.tapRight;
    rec.wantLeft = reference.wantLeft;
    rec.wantRight = reference.wantRight;
    rec.matchesReference = rinne::test::sameBitsVec(rec.outLeft, reference.wantLeft) &&
                           (mono || rinne::test::sameBitsVec(rec.outRight, reference.wantRight));
    rec.canaryHits = host->canaryHits();
    rec.writeHead = host->writeHead();
    rec.status = host->inputStatus();
    return rec;
}

class ChannelLayoutScenario final : public Scenario
{
public:
    const char* id() const noexcept override { return "s16_channel_layouts"; }

    bool run(ScenarioContext&) override
    {
        Verdict v;
        const Combo combos[] = {
            {"stereo input, stereo output", 2, 2},
            {"stereo input, mono output", 2, 1},
            {"mono input, stereo output", 1, 2},
            {"mono input, mono output", 1, 1},
        };
        for (const Combo& combo : combos)
        {
            for (const float feedback : {0.0f, 50.0f})
            {
                INFO(combo.name << ", feedback " << feedback << " %");
                check(v, combo, feedback, runCombo(combo, feedback));
            }
        }
        return v.ok();
    }

private:
    static void check(Verdict& v, const Combo& combo, float feedback, const Recording& rec)
    {
        const std::size_t total = rec.outLeft.size();
        RIG_CHECK(v, total >= static_cast<std::size_t>(kSamples));
        RIG_CHECK(v, rec.matchesReference);
        RIG_EQ(v, rec.canaryHits, std::size_t{0});
        RIG_EQ(v, rec.writeHead, static_cast<std::uint64_t>(total));
        RIG_EQ(v, rec.status.overrun, std::uint64_t{0});
        RIG_EQ(v, rec.status.resyncCount, 0u);
        RIG_EQ(v, rec.status.paired, 1u);

        // Output 不改动宿主缓冲；单声道 Input 不碰第 1 个声道
        RIG_CHECK(v, sameBits(rec.fedLeft, rec.outLeft, 0, total));
        RIG_CHECK(v, sameBits(rec.fedRight, rec.outRight, 0, total));
        if (combo.inputChannels == 1)
        {
            RIG_CHECK(v, sameBits(rec.outRight, rec.dryRight, 0, total));
        }

        // 映射：读头 = D 个样本之前 Output 写的那一对
        const bool outputMono = combo.outputChannels == 1;
        bool tapOk = true;
        std::size_t compared = 0;
        for (std::size_t t = static_cast<std::size_t>(kDelay + kOnset + 16); t < total; ++t)
        {
            const std::size_t u = t - static_cast<std::size_t>(kDelay);
            const float wantL = rec.fedLeft[u];
            const float wantR = outputMono ? rec.fedLeft[u] : rec.fedRight[u];
            tapOk = tapOk && rinne::test::floatBits(rec.tapLeft[t]) == rinne::test::floatBits(wantL) &&
                    rinne::test::floatBits(rec.tapRight[t]) == rinne::test::floatBits(wantR);
            ++compared;
        }
        RIG_CHECK(v, tapOk);
        RIG_CHECK(v, compared > static_cast<std::size_t>(kSamples / 2));

        // 两个声道没有被混成一个：立体声 Output 的读头两格不同，单声道 Output 的两格逐位相同
        const std::size_t from = static_cast<std::size_t>(kDelay + kOnset + 16);
        const bool tapsEqual = sameBits(rec.tapLeft, rec.tapRight, from, total);
        if (outputMono)
        {
            RIG_CHECK(v, tapsEqual);
        }
        else
        {
            RIG_CHECK(v, !tapsEqual);
        }

        if (feedback > 0.0f)
        {
            std::size_t audible = 0;
            for (std::size_t t = 0; t < total; ++t)
            {
                audible += rinne::test::floatBits(rec.outLeft[t]) != rinne::test::floatBits(rec.dryLeft[t]) ? 1u : 0u;
            }
            RIG_CHECK(v, audible > total / 2);
        }
    }
};

} // namespace

RINNE_RIG_SCENARIO("s16_channel_layouts")
{
    return new ChannelLayoutScenario();
}

TEST_CASE("s16: mono and stereo ends in all four combinations map correctly and stay in bounds", "[loop_rig][s16]")
{
    CHECK(rinne::rig::scn::runRegisteredScenario("s16_channel_layouts"));
}
