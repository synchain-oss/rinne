// SPDX-License-Identifier: GPL-3.0-or-later
//
// S03 起播沿：同一段起播之后的信号，不管之前预滚了多久、宿主怎么切块、是实时还是离线，从起播沿 + T 起输出逐位相同。
//
// 做法：每一路先预滚一段（transport 不走带，信号是各路不同的非静音噪声，淡入早已走完），再起播（transport 走带），
// 起播之后的信号是「相对起播点」的同一个纯函数。环路延迟 D = 4800，中间链是恒等；回送链的高切开（2 kHz）与关各一遍，
// 开着时参照也带音色级，关着时参照不带（默认参数下 Input 与不接音色级的接线逐位相同）。四路：
//   实时一  预滚 44137（块长 64…512 随机），起播后块长恒为 256；
//   实时二  预滚 51203（块长 480…512 随机），起播后块长 480…512 随机；
//   离线一  预滚 58301，块长 1…4096 随机，宿主标成离线渲染；
//   离线二  预滚 65537，另一粒种子的 1…4096 随机切分，离线渲染。
// 预滚长度都不是 16 与 480 的整数倍：10 ms 一格的内部计时若还停在预滚的相位上，各路就会对不齐。
// 反馈 50 % 与 100 % 各做一遍（100 % 时限制器与失控保护都会介入），每个反馈高切开、关各一遍。
//
// 断言：
//   - 每路恰好一次起播重同步（原因：起播），DSP 复位点恰为起播点 + T（T = 240 = 5 ms），其余原因的重同步为 0；
//   - 起播点 + T 起，左右声道的 Input 输出四路逐位相同，比较窗口 3 s；
//   - 起播点到起播点 + T 之间，实时一与离线一的输出确有不同（旧读头 5 ms 淡出的尾巴是各路预滚里的内容），
//     证明比较窗口的起点不是随手挑的；
//   - 每路输出与参照安全链按文档接线重算的结果逐位相同；
//   - 分辨力：实时一在比较窗口里高切开着的那一遍与关着的那一遍确有不同（高切真的在回送链里，开着时的逐位相同
//     钉住了高切在起播沿复位点上的复位）。
//
// 宿主短暂报停（Cubase 15 实测：走带中间连续 3 块、约 16 ms 把 isPlaying 报成 false 再恢复，十二次里出一次）：
// 报停不到阈值（max(100 ms, 4 × 最大块长)，按样本计，口径见 ADR-0010）不算停走带，恢复时不是起播沿，
// 不触发 5 ms 淡出、本地延迟线与回送链（含音色级）的复位，回声尾巴连续。同一条环路（块长恒为 256、高切 2 kHz、
// 反馈 50 %）预滚之后起播，
// 走带中间停几个周期：
//   对照       不停：一次起播重同步，复位点 = 起播点 + T；
//   短暂报停   3 个周期（768 个样本，实测的长度）：仍只有一次起播重同步、一个复位点，整段输出（左右声道）
//              与对照逐位相同，参照安全链逐位相同，停走带期间与之后的回声照常在响；
//   真停走带   30 个周期（7680 个样本，超过阈值）：第二次起播重同步，复位点 = 重新起播点 + T，之后的输出
//              与对照不同（分辨力：说明短暂报停那一行的逐位相同不是因为去抖把一切都吞了）；
//   反例（挂起）只报停 1 个周期就挂起（壳请求清空，两端停用超过 250 ms），恢复后第一个在走的块仍是起播沿：
//              第二次起播重同步，复位点 = 恢复的那个周期起点 + T。去抖不能把这种真停走带吞掉。
// 宿主停走带之后一块都不再调用我们的反例（样本数不再走）在 tests/core/timing/test_play_debounce.cpp 里：
// 试验台的记录器在 Input 不跑时报空转，这里造不出来。
//
// 标 ⛔ 的是删除式验证点：把对应的实现改坏，本场景必须变红。
//   ⛔ 起播沿不复位回送链（playEdgeReset 不调）→ DC 阻断与限制器的状态各路不同，窗口内逐位对不上；
//   ⛔ 起播沿不复位音色级 → 高切的状态各路不同，高切开着的那一遍窗口内逐位对不上；
//   ⛔ 高切不接（引擎不理 highCutHz）→ 高切开着的那一遍与参照对不上，且与关着的那一遍逐位相同；
//   ⛔ 起播沿不去抖（报停一块就算停走带）→ 短暂报停那一行出现第二次起播重同步，输出与对照不再逐位相同；
//   ⛔ 阈值取得过大（吞掉 30 个周期的真停走带）→ 真停走带那一行没有第二次起播重同步；
//   ⛔ 挂起清空时不通知去抖（flushAfterSuspend 不调 suspended）→ 挂起反例没有第二次起播重同步。

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
constexpr float kHighCutOn = 2000.0f;
constexpr float kHighCutOff = rinne::engine::kDefaultHighCutHz; // 20000 = 关

// 场景的参数：混合 50 %、环路延迟 100 ms，加上这一遍的反馈与高切
rinne::engine::EngineParams scenarioParams(float feedbackPercent, float highCutHz)
{
    rinne::engine::EngineParams p = loopParams(feedbackPercent, 50.0f, 100.0f);
    p.highCutHz = highCutHz;
    return p;
}

// 高切开着时参照也带音色级；关着时不带（参照就是不接音色级的接线）
void referenceFollows(Recorder& rec, float highCutHz)
{
    if (highCutHz != kHighCutOff)
    {
        rec.reference.enableTone();
    }
}

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

RunResult runOne(const RunSpec& spec, float feedbackPercent, float highCutHz)
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

    const auto edge = static_cast<std::uint64_t>(spec.preroll);
    Recorder rec(*host, sourceFor(edge, spec.seed));
    referenceFollows(rec, highCutHz);
    const rinne::engine::EngineParams p = scenarioParams(feedbackPercent, highCutHz);
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

// ---- 宿主短暂报停 ----

constexpr std::size_t kStopPreroll = 40; // 预滚的周期数（不走带）
constexpr std::size_t kStopAt = 300; // 走带中间开始报停的周期
constexpr std::size_t kStopTotal = 560; // 总周期数（约 3 s）
constexpr int kStopBlock = 256;

struct StopRun
{
    std::vector<float> left;
    std::vector<float> right;
    std::uint32_t playResyncs = 0;
    std::uint32_t resyncs = 0;
    std::vector<std::int64_t> resetPoints;
    bool matchesReference = false;
    // 报停开始之后的 3 s 里输出不等于干声的样本数（回声在响）
    std::size_t audible = 0;
};

// stopCycles 个周期把 isPlaying 报成 false（0 = 对照）；suspend 为真时在这一段报停结束的周期之前请求清空
// （壳判定两端一起挂起超过 250 ms 之后做的事），恢复后继续走带
StopRun runStop(std::size_t stopCycles, bool suspend)
{
    StopRun result;
    auto host = makeHost(kSr, kStopBlock);
    REQUIRE(host != nullptr);
    host->setBlockScripts(fixedBlocks(kStopBlock), fixedBlocks(kStopBlock));
    TransportScript transport;
    transport.playing.assign(kStopTotal, true);
    std::fill(transport.playing.begin(), transport.playing.begin() + static_cast<std::ptrdiff_t>(kStopPreroll), false);
    for (std::size_t i = kStopAt; i < kStopAt + stopCycles; ++i)
    {
        transport.playing[i] = false;
    }
    transport.nonrealtime = {false};
    host->setTransportScript(transport);

    Recorder rec(*host, [](std::uint64_t first, float* left, float* right, int n) {
        for (int i = 0; i < n; ++i)
        {
            left[i] = 0.25f * hashNoise(first + static_cast<std::uint64_t>(i), 31);
            right[i] = 0.25f * hashNoise(first + static_cast<std::uint64_t>(i), 32);
        }
    });
    referenceFollows(rec, kHighCutOn);
    const rinne::engine::EngineParams p = scenarioParams(50.0f, kHighCutOn);
    for (std::size_t i = 0; i < kStopTotal; ++i)
    {
        if (suspend && i == kStopAt + stopCycles)
        {
            host->input().requestFlushAfterSuspend();
        }
        rec.cycle(i, p);
    }
    result.left = rec.outLeft;
    result.right = rec.outRight;
    const auto& status = host->inputStatus();
    result.playResyncs = status.resyncByReason[static_cast<std::size_t>(SyncReason::kPlay)];
    result.resyncs = status.resyncCount;
    result.resetPoints = rec.resetPoints;
    result.matchesReference = rec.matchesReference();
    const std::size_t from = kStopAt * kStopBlock;
    for (std::size_t t = from; t < std::min(rec.outLeft.size(), from + 3 * 48000u); ++t)
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
            std::vector<float> firstRun[2]; // 实时一的左声道：[0] 高切开，[1] 高切关
            for (int pass = 0; pass < 2; ++pass)
            {
                const float highCut = pass == 0 ? kHighCutOn : kHighCutOff;
                INFO("high cut = " << highCut << " Hz");
                firstRun[pass] = runPass(v, feedback, highCut);
            }
            // 分辨力：高切开着与关着的两遍在比较窗口里确有不同
            const auto from = static_cast<std::size_t>(kEdgeFade);
            const auto to = static_cast<std::size_t>(kEdgeFade + kWindow);
            RIG_CHECK(v, !sameBits(firstRun[0], firstRun[1], from, to));
        }
        briefStop(v);
        return v.ok();
    }

private:
    // 一个反馈、一个高切的四路；返回实时一从起播点起的左声道输出
    static std::vector<float> runPass(Verdict& v, float feedback, float highCut)
    {
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
            results.push_back(runOne(spec, feedback, highCut));
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
        return results[0].left;
    }

    static void briefStop(Verdict& v)
    {
        constexpr std::int64_t kFirstEdge = static_cast<std::int64_t>(kStopPreroll * kStopBlock) + kEdgeFade;
        const StopRun control = runStop(0, false);
        RIG_EQ(v, control.playResyncs, 1u);
        RIG_EQ(v, control.resyncs, 1u);
        RIG_EQ(v, control.resetPoints.size(), std::size_t{1});
        if (!control.resetPoints.empty())
        {
            RIG_EQ(v, control.resetPoints[0], kFirstEdge);
        }
        RIG_CHECK(v, control.matchesReference);
        RIG_CHECK(v, control.audible > 30000u); // 回声确实在响（报停之后到结束共 66560 个样本）

        // 短暂报停：3 个周期 = 768 个样本
        {
            INFO("brief stop, 3 cycles");
            const StopRun brief = runStop(3, false);
            RIG_EQ(v, brief.playResyncs, 1u);
            RIG_EQ(v, brief.resyncs, 1u);
            RIG_EQ(v, brief.resetPoints.size(), std::size_t{1});
            if (!brief.resetPoints.empty())
            {
                RIG_EQ(v, brief.resetPoints[0], kFirstEdge);
            }
            RIG_CHECK(v, brief.matchesReference);
            RIG_EQ(v, brief.left.size(), control.left.size());
            RIG_CHECK(v, sameBits(brief.left, control.left, 0, control.left.size()));
            RIG_CHECK(v, sameBits(brief.right, control.right, 0, control.right.size()));
            RIG_EQ(v, brief.audible, control.audible);
        }

        // 真停走带：30 个周期 = 7680 个样本，超过阈值 4800
        {
            INFO("real stop, 30 cycles");
            constexpr std::size_t kStop = 30;
            const StopRun real = runStop(kStop, false);
            RIG_EQ(v, real.playResyncs, 2u);
            RIG_EQ(v, real.resetPoints.size(), std::size_t{2});
            if (real.resetPoints.size() == 2u)
            {
                RIG_EQ(v, real.resetPoints[0], kFirstEdge);
                RIG_EQ(v, real.resetPoints[1], static_cast<std::int64_t>((kStopAt + kStop) * kStopBlock) + kEdgeFade);
            }
            RIG_CHECK(v, real.matchesReference);
            // 分辨力：重新起播之前两者逐位相同，之后不同
            const std::size_t again = (kStopAt + kStop) * kStopBlock;
            RIG_CHECK(v, sameBits(real.left, control.left, 0, again));
            RIG_CHECK(v, !sameBits(real.left, control.left, again, control.left.size()));
        }

        // 反例：只报停 1 个周期就挂起，恢复后第一个在走的块仍是起播沿
        {
            INFO("one stop block, then suspended");
            const StopRun suspended = runStop(1, true);
            RIG_EQ(v, suspended.playResyncs, 2u);
            RIG_EQ(v, suspended.resetPoints.size(), std::size_t{2});
            if (suspended.resetPoints.size() == 2u)
            {
                RIG_EQ(v, suspended.resetPoints[0], kFirstEdge);
                RIG_EQ(v, suspended.resetPoints[1], static_cast<std::int64_t>((kStopAt + 1) * kStopBlock) + kEdgeFade);
            }
            RIG_CHECK(v, !sameBits(suspended.left, control.left, 0, control.left.size()));
        }
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
