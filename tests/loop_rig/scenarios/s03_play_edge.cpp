// SPDX-License-Identifier: GPL-3.0-or-later
//
// S03 起播沿：同一段起播之后的信号，不管之前预滚了多久、宿主怎么切块、是实时还是离线，从起播沿 + T 起输出逐位相同。
//
// 做法：每一路先预滚一段（transport 不走带，信号是各路不同的非静音噪声，淡入早已走完），再起播（transport 走带），
// 起播之后的信号是「相对起播点」的同一个纯函数。环路延迟 D = 4800，环内放一个一阶低通（高切的替身，截止 2 kHz），
// 中间链在引擎报出的 DSP 复位点上复位。四路：
//   实时一  预滚 44137（块长 64…512 随机），起播后块长恒为 256；
//   实时二  预滚 51203（块长 480…512 随机），起播后块长 480…512 随机；
//   离线一  预滚 58301，块长 1…4096 随机，宿主标成离线渲染；
//   离线二  预滚 65537，另一粒种子的 1…4096 随机切分，离线渲染。
// 预滚长度都不是 16 与 480 的整数倍：10 ms 一格的内部计时若还停在预滚的相位上，各路就会对不齐。
// 反馈 50 % 与 100 % 各做一遍（100 % 时限制器与失控保护都会介入）。
//
// 断言：
//   - 每路恰好一次起播重同步（原因：起播），DSP 复位点恰为起播点 + T（T = 240 = 5 ms），其余原因的重同步为 0；
//   - 起播点 + T 起，左右声道的 Input 输出四路逐位相同，比较窗口 3 s；
//   - 起播点到起播点 + T 之间，实时一与离线一的输出确有不同（旧读头 5 ms 淡出的尾巴是各路预滚里的内容），
//     证明比较窗口的起点不是随手挑的；
//   - 每路输出与参照安全链按文档接线重算的结果逐位相同。
//
// 暂缺：高切（high_cut）还没有接进回送链的音色级，这里用中间链的一阶低通顶替环内滤波；
//   音色级接入之后，再把高切开关本身放进本场景。
//
// 标 ⛔ 的是删除式验证点：把对应的实现改坏，本场景必须变红。
//   ⛔ 起播沿不复位回送链（playEdgeReset 不调）→ DC 阻断与限制器的状态各路不同，窗口内逐位对不上；
//   ⛔ 起播沿不复位中间链 → 低通的状态各路不同，窗口内逐位对不上。

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
constexpr std::int64_t kPlaySamples = kWindow + 4096 + 512; // 起播之后至少跑这么多
constexpr int kMaxBlock = 4096;

// 把总长 total 随机切成长度在 [lo, hi] 里的块（最后一块取余数）
std::vector<int> randomBlocks(rinne::test::Rng& rng, std::int64_t total, int lo, int hi)
{
    std::vector<int> lengths;
    for (std::int64_t left = total; left > 0;)
    {
        const int n = static_cast<int>(std::min<std::int64_t>(lo + rng.below(hi - lo + 1), left));
        lengths.push_back(n);
        left -= n;
    }
    return lengths;
}

struct RunSpec
{
    const char* name;
    std::int64_t preroll;
    std::uint64_t seed;
    bool offline;
    // 预滚与起播之后的块长（长度之和分别不小于 preroll 与 kPlaySamples；预滚的块长之和恰为 preroll）
    std::vector<int> preBlocks;
    std::vector<int> playBlocks;
};

struct RunResult
{
    std::vector<float> left; // 从起播点起的 Input 输出
    std::vector<float> right;
    std::uint32_t playResyncs = 0;
    std::uint32_t resyncs = 0;
    std::vector<std::int64_t> resetPoints;
    bool matchesReference = false;
    // 比较窗口里输出不等于干声的样本数：回送确实在响，逐位相同不是两路都静音的平凡相等
    std::size_t audible = 0;
};

// 起播点之前每一路各不相同、非静音；起播点之后是相对起播点的同一个纯函数
SourceFn sourceFor(std::uint64_t edge, std::uint64_t seed)
{
    return [edge, seed](std::uint64_t first, float* left, float* right, int n) {
        for (int i = 0; i < n; ++i)
        {
            const std::uint64_t at = first + static_cast<std::uint64_t>(i);
            if (at < edge)
            {
                left[i] = 0.2f * hashNoise(at, seed);
                right[i] = 0.2f * hashNoise(at, seed + 1);
            }
            else
            {
                left[i] = 0.25f * hashNoise(at - edge, 7);
                right[i] = 0.25f * hashNoise(at - edge, 8);
            }
        }
    };
}

RunResult runOne(const RunSpec& spec, float feedbackPercent)
{
    RunResult result;
    auto host = makeHost(kSr, kMaxBlock);
    REQUIRE(host != nullptr);

    BlockScript blocks;
    blocks.lengths = spec.preBlocks;
    blocks.lengths.insert(blocks.lengths.end(), spec.playBlocks.begin(), spec.playBlocks.end());
    TransportScript transport;
    transport.playing.assign(blocks.lengths.size(), true);
    std::fill(transport.playing.begin(), transport.playing.begin() + static_cast<std::ptrdiff_t>(spec.preBlocks.size()),
              false);
    transport.nonrealtime = {spec.offline};
    host->setBlockScripts(blocks, blocks);
    host->setTransportScript(transport);
    host->setChain(ChainKind::kOnePoleLowpass);
    host->chain().setLowpassHz(2000.0f);

    const auto edge = static_cast<std::uint64_t>(spec.preroll);
    Recorder rec(*host, sourceFor(edge, spec.seed));
    const rinne::engine::EngineParams p = loopParams(feedbackPercent, 50.0f, 100.0f);
    for (std::size_t i = 0; i < blocks.lengths.size(); ++i)
    {
        rec.cycle(i, p);
    }

    const auto from = static_cast<std::size_t>(edge);
    result.left.assign(rec.outLeft.begin() + static_cast<std::ptrdiff_t>(from), rec.outLeft.end());
    result.right.assign(rec.outRight.begin() + static_cast<std::ptrdiff_t>(from), rec.outRight.end());
    const auto& status = host->inputStatus();
    result.playResyncs = status.resyncByReason[static_cast<std::size_t>(SyncReason::kPlay)];
    result.resyncs = status.resyncCount;
    result.resetPoints = rec.resetPoints;
    result.matchesReference = rec.matchesReference();
    for (std::size_t t = from + static_cast<std::size_t>(kEdgeFade + kDelay);
         t < from + static_cast<std::size_t>(kEdgeFade + kWindow); ++t)
    {
        result.audible += rinne::test::floatBits(rec.outLeft[t]) != rinne::test::floatBits(rec.dryLeft[t]) ? 1u : 0u;
    }
    return result;
}

class PlayEdgeScenario final : public Scenario
{
public:
    const char* id() const noexcept override { return "s03_play_edge"; }

    bool run(ScenarioContext&) override
    {
        Verdict v;
        for (const float feedback : {50.0f, 100.0f})
        {
            INFO("feedback = " << feedback << " %");
            rinne::test::Rng rngRt(0x5eed01u);
            rinne::test::Rng rngOff1(0x5eed02u);
            rinne::test::Rng rngOff2(0x5eed03u);
            std::vector<RunSpec> specs;
            specs.push_back({"realtime-1", 44137, 11, false, randomBlocks(rngRt, 44137, 64, 512),
                             std::vector<int>(static_cast<std::size_t>(kPlaySamples / 256 + 1), 256)});
            specs.push_back({"realtime-2", 51203, 12, false, randomBlocks(rngRt, 51203, 480, 512),
                             randomBlocks(rngRt, kPlaySamples, 480, 512)});
            specs.push_back({"offline-1", 58301, 13, true, randomBlocks(rngOff1, 58301, 1, 4096),
                             randomBlocks(rngOff1, kPlaySamples, 1, 4096)});
            specs.push_back({"offline-2", 65537, 14, true, randomBlocks(rngOff2, 65537, 1, 4096),
                             randomBlocks(rngOff2, kPlaySamples, 1, 4096)});

            std::vector<RunResult> results;
            for (const RunSpec& spec : specs)
            {
                results.push_back(runOne(spec, feedback));
            }

            for (std::size_t r = 0; r < results.size(); ++r)
            {
                INFO("run " << specs[r].name);
                const RunResult& res = results[r];
                RIG_EQ(v, res.playResyncs, 1u);
                RIG_EQ(v, res.resyncs, 1u);
                RIG_EQ(v, res.resetPoints.size(), std::size_t{1});
                if (!res.resetPoints.empty())
                {
                    RIG_EQ(v, res.resetPoints[0], specs[r].preroll + kEdgeFade);
                }
                RIG_CHECK(v, res.matchesReference);
                RIG_CHECK(v, res.audible > static_cast<std::size_t>(kWindow / 2));
                RIG_CHECK(v, res.left.size() >= static_cast<std::size_t>(kEdgeFade + kWindow));
            }

            const auto from = static_cast<std::size_t>(kEdgeFade);
            const auto to = static_cast<std::size_t>(kEdgeFade + kWindow);
            for (std::size_t r = 1; r < results.size(); ++r)
            {
                INFO("run " << specs[r].name << " vs " << specs[0].name);
                RIG_CHECK(v, sameBits(results[r].left, results[0].left, from, to));
                RIG_CHECK(v, sameBits(results[r].right, results[0].right, from, to));
            }
            // 分辨力：起播点到起播点 + T 之间，旧读头的 5 ms 淡出尾巴各路不同
            RIG_CHECK(v, !sameBits(results[2].left, results[0].left, 0, from));
        }
        return v.ok();
    }
};

} // namespace

RINNE_RIG_SCENARIO("s03_play_edge")
{
    return new PlayEdgeScenario();
}

TEST_CASE("s03: after a play edge every run is bit-identical from edge plus T on", "[loop_rig][s03]")
{
    CHECK(rinne::rig::scn::runRegisteredScenario("s03_play_edge"));
}
