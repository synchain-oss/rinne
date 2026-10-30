// SPDX-License-Identifier: GPL-3.0-or-later
//
// S05b 停调与起播沿：Output 停调（30 ms、120 ms，停调边界落在块边界上）恢复之后再起播，起播沿之后的输出与实时/离线、
// 切块方式、停调与否都无关：从起播点 s0 + T 起逐位相同（T = 240，5 ms）。S03 钉的是起播沿本身；这里钉的是起播之前发生过
// 停调与恢复（读头重建、补零、onset、断档或位移重同步）时，这些后遗症不会越过起播沿。
//
// 做法：环路延迟 D = 4800，环内放一个一阶低通（截止 2 kHz，环内插件的替身，中间链在引擎报出的复位点上复位），
// 声明的最大块长 4096，混合 50 %，反馈 50 % 与 100 % 各做一遍。每一路先预滚（transport 不走带，信号是各路不同的噪声），
// 停调的路在起播点之前 64000 个样本（1.33 s）处让 Output 停调若干个周期（停调的样本数恰为 1440 或 5760，
// 块在边界处截短），然后起播（transport 走带），起播之后的信号是相对起播点的同一个纯函数。六路：
//   对照    块长恒为 256，预滚 120011，不停调；
//   实时一  同上预滚，停调 30 ms；
//   实时二  块长 64…512 随机，预滚 131071，停调 120 ms；
//   离线一  块长 1…4096 随机，预滚 140239，停调 30 ms，宿主标成离线渲染；
//   离线二  另一粒种子的 1…4096 随机切分，预滚 152233，停调 120 ms，离线渲染；
//   离线三  预滚同对照，1…4096 随机切分，停调 120 ms，离线渲染。
// 另有四路把起播点紧挨着停调的恢复：恢复点正好是起播点（停调 30 ms，实时与离线各一路），或在起播点之前 3000 个样本
// （停调 120 ms，实时与离线各一路，读头重建的零窗口 D + onset = 5760 还没走完）。这时恢复的重同步可能被起播沿顶替
// （恢复和起播在同一个周期里），不要求有单独的恢复重同步；比较的口径不变。
// 预滚长度都不是 16 与 480 的整数倍。
//
// 比较起点 = max(s0 + T, 配对后第一个有效读样本 + 500 ms 淡入走完)：预滚都长到淡入早已走完，所以起点就是 s0 + T；
// 起点的取法仍按这个式子算，场景里再核一遍它等于 s0 + T。比较窗口 3 s。
// 断言：
//   - 每路恰 1 次起播重同步，DSP 复位点恰为 s0 + T；停调离起播点 64000 个样本的路在恢复时至少有 1 次重同步
//     （断档恢复或位移）；
//   - 左右声道十路在窗口里逐位相同；
//   - 每路输出与参照安全链按文档接线重算的结果逐位相同；
//   - 分辨力：实时一与对照在起播之前确有不同（停调确实改变了回送，对拍不是平凡相等）。
//
// 标 ⛔ 的是删除式验证点：把对应的实现改坏，本场景必须变红。
//   ⛔ 起播沿不复位回送链（playEdgeReset 不调）→ 各路 DC 阻断与限制器的状态不同，窗口内逐位对不上；
//   ⛔ 起播沿不被识别（复位开关取反）→ 没有起播重同步，复位点对不上。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "loop/StreamSync.h"
#include "s01_rig_kit.h"

namespace
{

using namespace rinne::rig;
using namespace rinne::rig::scn;
using rinne::loop::SyncReason;

constexpr std::int64_t kDelay = 4800;
constexpr std::int64_t kEdgeFade = 240; // T = min(5 ms, D)
constexpr std::int64_t kWindow = 3 * 48000;
constexpr int kMaxBlock = 4096;

struct Blocks
{
    int low;
    int high;
    std::uint64_t seed;
};

struct Plan
{
    bool offline;
    std::int64_t edge;
    Blocks blocks;
    std::uint64_t noiseSeed;
    std::int64_t stallLength; // 0 = 不停调
    float feedback;
    std::int64_t gap = 64000; // 停调恢复的样本到起播点的距离
};

struct Result
{
    std::vector<float> left;
    std::vector<float> right;
    std::vector<std::int64_t> resets;
    std::uint32_t playResyncs = 0;
    std::uint32_t stallResyncs = 0;
    std::int64_t stallFirst = 0;
    std::int64_t pairedAt = 0;
    bool matchesReference = false;
};

float noiseFor(std::uint64_t seed, std::int64_t index, int channel)
{
    return 0.3f * hashNoise(static_cast<std::uint64_t>(index), seed * 2 + static_cast<std::uint64_t>(channel));
}

Result runPlan(const Plan& plan)
{
    Result out;
    rinne::test::Rng rng(plan.blocks.seed);
    const std::int64_t total = plan.edge + kWindow + 2 * kMaxBlock;
    out.stallFirst = plan.stallLength > 0 ? plan.edge - plan.gap - plan.stallLength : -1;
    const std::int64_t stallEnd = plan.stallLength > 0 ? out.stallFirst + plan.stallLength : -1;

    // 周期表：块长逐个抽，在停调的两端和起播点处截短（边界落在块边界上）
    BlockScript script;
    BlockScript outputScript;
    TransportScript transport;
    std::int64_t position = 0;
    while (position < total)
    {
        int n = plan.blocks.low + rng.below(plan.blocks.high - plan.blocks.low + 1);
        for (const std::int64_t cut : {out.stallFirst, stallEnd, plan.edge})
        {
            if (cut > position && cut < position + n)
            {
                n = static_cast<int>(cut - position);
            }
        }
        const bool stalled = plan.stallLength > 0 && position >= out.stallFirst && position < stallEnd;
        script.lengths.push_back(n);
        outputScript.skips.push_back(stalled);
        transport.playing.push_back(position >= plan.edge);
        transport.nonrealtime.push_back(plan.offline);
        position += n;
    }
    outputScript.lengths = script.lengths;

    auto host = makeHost(kSr, kMaxBlock);
    REQUIRE(host != nullptr);
    host->setBlockScripts(script, outputScript);
    host->setTransportScript(transport);
    host->setChain(ChainKind::kOnePoleLowpass);
    host->chain().setLowpassHz(2000.0f);
    const std::int64_t edge = plan.edge;
    const std::uint64_t seed = plan.noiseSeed;
    Recorder rec(*host, [edge, seed](std::uint64_t first, float* left, float* right, int n) {
        for (int i = 0; i < n; ++i)
        {
            const auto t = static_cast<std::int64_t>(first) + i;
            // 起播点之前各路不同；之后是相对起播点的同一个纯函数
            const std::int64_t k = t < edge ? t : t - edge;
            const std::uint64_t s = t < edge ? seed : 0x5ca1ab1eull;
            left[i] = noiseFor(s, k, 0);
            right[i] = noiseFor(s, k, 1);
        }
    });

    const rinne::engine::EngineParams p = loopParams(plan.feedback, 50.0f, 100.0f);
    for (std::size_t i = 0; i < script.lengths.size(); ++i)
    {
        if (rec.cycle(i, p) <= 0)
        {
            break;
        }
        if (out.pairedAt == 0 && host->inputStatus().paired == 1u)
        {
            out.pairedAt = static_cast<std::int64_t>(rec.outLeft.size());
        }
    }
    out.left = rec.outLeft;
    out.right = rec.outRight;
    out.resets = rec.resetPoints;
    out.matchesReference = rec.matchesReference();
    const auto& status = host->inputStatus();
    out.playResyncs = status.resyncByReason[static_cast<std::size_t>(SyncReason::kPlay)];
    out.stallResyncs = status.resyncByReason[static_cast<std::size_t>(SyncReason::kStallResume)] +
                       status.resyncByReason[static_cast<std::size_t>(SyncReason::kShift)];
    return out;
}

std::int64_t compareStart(const Plan& plan, const Result& r)
{
    return std::max(kEdgeFade, r.pairedAt + kDelay + kFadeInSamples - plan.edge);
}

class StallEdgeScenario final : public Scenario
{
public:
    const char* id() const noexcept override { return "s05b_stall_edge"; }

    bool run(ScenarioContext&) override
    {
        Verdict v;
        for (const float feedback : {50.0f, 100.0f})
        {
            INFO("feedback " << feedback << " %");
            const Plan plans[] = {
                {false, 120011, Blocks{256, 256, 1}, 201, 0, feedback},
                {false, 120011, Blocks{256, 256, 1}, 201, 1440, feedback},
                {false, 131071, Blocks{64, 512, 22}, 202, 5760, feedback},
                {true, 140239, Blocks{1, 4096, 24}, 203, 1440, feedback},
                {true, 152233, Blocks{1, 4096, 26}, 204, 5760, feedback},
                {true, 120011, Blocks{1, 4096, 28}, 201, 5760, feedback},
                // 起播点紧接着停调的恢复：恢复点正好在起播点，或在它之前 3000 个样本（读头重建的零窗口还没走完）
                {false, 120011, Blocks{256, 256, 1}, 201, 1440, feedback, 0},
                {false, 131071, Blocks{64, 512, 22}, 202, 5760, feedback, 3000},
                {true, 140239, Blocks{1, 4096, 24}, 203, 1440, feedback, 0},
                {true, 152233, Blocks{1, 4096, 26}, 204, 5760, feedback, 3000},
            };
            std::vector<Result> results;
            for (const Plan& plan : plans)
            {
                results.push_back(runPlan(plan));
                const Result& r = results.back();
                RIG_EQ(v, r.playResyncs, 1u);
                RIG_CHECK(v, r.resets == std::vector<std::int64_t>{plan.edge + kEdgeFade});
                RIG_CHECK(v, r.matchesReference);
                RIG_CHECK(v, r.left.size() >= static_cast<std::size_t>(plan.edge + kWindow));
                if (plan.stallLength > 0 && plan.gap >= 64000)
                {
                    RIG_CHECK(v, r.stallResyncs >= 1u);
                }
            }
            std::int64_t from = kEdgeFade;
            for (std::size_t i = 0; i < results.size(); ++i)
            {
                from = std::max(from, compareStart(plans[i], results[i]));
            }
            RIG_EQ(v, from, kEdgeFade);
            for (std::size_t i = 1; i < results.size(); ++i)
            {
                INFO("run " << i << " against the control");
                const auto a = static_cast<std::size_t>(plans[0].edge + from);
                const auto b = static_cast<std::size_t>(plans[i].edge + from);
                const auto len = static_cast<std::size_t>(kWindow);
                RIG_CHECK(v, std::equal(results[0].left.begin() + static_cast<std::ptrdiff_t>(a),
                                        results[0].left.begin() + static_cast<std::ptrdiff_t>(a + len),
                                        results[i].left.begin() + static_cast<std::ptrdiff_t>(b), [](float x, float y) {
                                            return rinne::test::floatBits(x) == rinne::test::floatBits(y);
                                        }));
                RIG_CHECK(v,
                          std::equal(results[0].right.begin() + static_cast<std::ptrdiff_t>(a),
                                     results[0].right.begin() + static_cast<std::ptrdiff_t>(a + len),
                                     results[i].right.begin() + static_cast<std::ptrdiff_t>(b), [](float x, float y) {
                                         return rinne::test::floatBits(x) == rinne::test::floatBits(y);
                                     }));
            }
            // 分辨力：停调的路与对照在起播之前确有不同
            std::size_t differing = 0;
            for (std::int64_t t = results[1].stallFirst; t < plans[1].edge; ++t)
            {
                differing += rinne::test::floatBits(results[1].left[static_cast<std::size_t>(t)]) !=
                                     rinne::test::floatBits(results[0].left[static_cast<std::size_t>(t)])
                                 ? 1u
                                 : 0u;
            }
            RIG_CHECK(v, differing > 1000u);
        }
        return v.ok();
    }
};

} // namespace

RINNE_RIG_SCENARIO("s05b_stall_edge")
{
    return new StallEdgeScenario();
}

TEST_CASE("s05b: a stalled Output leaves no trace in the output after the play edge", "[loop_rig][s05b]")
{
    CHECK(rinne::rig::scn::runRegisteredScenario("s05b_stall_edge"));
}
