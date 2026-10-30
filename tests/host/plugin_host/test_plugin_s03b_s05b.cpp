// SPDX-License-Identifier: GPL-3.0-or-later
//
// S03b、S05b：起播沿与 Output 停调的插件级复验。判据同 core 级（tests/loop_rig/scenarios/s03_play_edge.cpp、
// s05b_stall_edge.cpp），这里经真处理器走一遍：参数经参数树写，走带由 LanePlayHead 的脚本给（Start 事件），
// 离线渲染由处理器的 setNonRealtime 标出，块长与跳过 Output 的周期由宿主给。
//
// S03b 起播沿：同一段起播之后的信号（相对起播点的同一个纯函数），不管之前预滚了多久、宿主怎么切块、是实时还是离线，
// 从起播点 s0 + T 起输出逐位相同。四路：
//   实时一  预滚 61133（块长 64…512 随机），起播后块长恒为 256；
//   实时二  预滚 72407（块长 480…512 随机），起播后块长 480…512 随机；
//   离线一  预滚 83521，块长 1…4096 随机，处理器标成离线渲染；
//   离线二  预滚 94793，另一粒种子的 1…4096 随机切分，离线渲染。
// 预滚长度都不是 16 与 480 的整数倍。反馈 50 % 与 100 % 各做一遍。信号源在起播点之前是各路不同的噪声，
// 之后是同一个函数。中间链是恒等（插件级宿主的中间链没有滤波替身），起播沿复位的是引擎自己的回送链与本地延迟线。
//
// 比较起点 = max(s0 + T, 配对后第一个有效读样本 + 500 ms 淡入走完)，T = 240（5 ms）。预滚都长到淡入早已走完，
// 所以起点就是 s0 + T；起点的取法仍按这个式子算，场景里再核一遍它等于 s0 + T。比较窗口 3 s。
// 断言：每路恰 1 次起播重同步，DSP 复位点恰为 s0 + T；窗口里左右声道四路逐位相同；起播点到 s0 + T
// 之间实时一与离线一确有不同（比较起点不是随手挑的）；Output 逐位不碰宿主缓冲，两端都不写块尾之后。
// 高切：参数 high_cut 写 8 kHz（开）与 20000（关）各一遍，两遍各自满足上面的断言（开着时窗口里四路照样逐位相同：
// 高切在起播沿的复位点上与回送链一起复位）；另外实时一在窗口里开着与关着的两遍确有不同，且开着时回送的高频占比
// （相邻差的能量 / 自身的能量）明显更低：参数经参数树写进去，高切真的在回送链里。
//
// S05b 停调与起播沿：Output 停调（30 ms、120 ms，停调边界落在块边界上）恢复之后再起播，起播沿之后的输出与实时/离线、
// 切块方式、停调与否都无关：从 s0 + T 起逐位相同。起播点离停调的恢复点 64000 个样本（1.33 s），
// 停调的后遗症（读头重建、补零、onset）早已走完，起播沿把本地延迟线与回送链复位，之后的状态与停调无关。
// 六路（预滚与切块各不相同）：
//   对照    实时，块长恒为 256，预滚 120011，不停调；
//   实时一  同上预滚，停调 30 ms；
//   实时二  块长 64…512 随机，预滚 131071，停调 120 ms；
//   离线一  块长 1…4096 随机，预滚 140239，停调 30 ms；
//   离线二  另一粒种子，预滚 152233，停调 120 ms；
//   离线三  预滚同对照，1…4096 随机切分，停调 120 ms。
// 另有四路把起播点紧挨着停调的恢复（恢复点正好是起播点，或在它之前 3000 个样本，实时与离线各两路），口径同 core 级。
// 停调的周期里 Output 不被调用，Output 一侧的游标这期间不走，恢复时按 Input 的游标对齐（宿主的时间线没有停）。
// 断言：每路恰 1 次起播重同步，复位点恰为 s0 + T；停调离起播点 64000 个样本的路在停调恢复时有重同步（断档恢复或位移，
// 至少 1 次）；起点 s0 + T 起 3 s 里左右声道十路逐位相同；实时一与对照在起播之前的某段确有不同（停调确实改变了回送，
// 对拍不是平凡相等）。高切开、关各一遍，对照在窗口里两遍的分辨力同 S03b。
//
// 标 ⛔ 的是删除式验证点：把对应的实现改坏，本场景必须变红。
//   ⛔ 起播沿不复位回送链（playEdgeReset 不调）→ DC 阻断与限制器的状态各路不同，窗口内逐位对不上；
//   ⛔ 起播沿不被识别（复位开关取反）→ 没有起播重同步，复位点对不上；
//   ⛔ 起播沿不复位音色级 → 高切的状态带着预滚（与停调）的痕迹，高切开着的那一遍窗口内逐位对不上；
//   ⛔ 高切不接（引擎不理 highCutHz）→ 开着与关着的两遍逐位相同，高频占比也一样。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/dsp/DspTestKit.h"
#include "host/plugin_host/PluginHost.h"
#include "host/plugin_host/PluginScenarioKit.h"
#include "loop/LoopStatus.h"
#include "loop/StreamSync.h"

namespace
{

using namespace rinne::plugin_host;
using rinne::loop::SyncReason;
using rinne::wiring_test::formalOnly;
using rinne::wiring_test::needsSharedMemory;

constexpr std::int64_t kDelay = 4800;
constexpr std::int64_t kEdgeFade = 240; // T = min(5 ms, D)
constexpr std::int64_t kFirstPairFade = 24000; // 500 ms
constexpr std::int64_t kWindow = 3 * 48000;
constexpr int kDeclaredBlock = 4096;
constexpr float kHighCutOn = 8000.0f;
constexpr float kHighCutOff = rinne::engine::kDefaultHighCutHz; // 20000 = 关
constexpr std::uint64_t kPlaySeed = 0x5ca1ab1eull; // 起播点之后的信号源（各路共用）

struct Blocks
{
    int low;
    int high;
    std::uint64_t seed;
};

class BlockSource
{
public:
    explicit BlockSource(Blocks blocks) : blocks_(blocks), rng_(blocks.seed) {}
    int next() { return blocks_.low + rng_.below(blocks_.high - blocks_.low + 1); }

private:
    Blocks blocks_;
    rinne::test::Rng rng_;
};

struct Plan
{
    bool offline = false;
    std::int64_t edge = 0;
    Blocks before{256, 256, 1};
    Blocks after{256, 256, 1};
    std::uint64_t noiseSeed = 1;
    std::int64_t stallLength = 0; // 0 = 不停调
    float feedback = 50.0f;
    std::int64_t gap = 64000; // 停调恢复的样本到起播点的距离
};

struct Result
{
    std::vector<float> left;
    std::vector<float> right;
    std::vector<std::int64_t> resets;
    std::int64_t pairedAt = 0;
    std::uint32_t playResyncs = 0;
    std::uint32_t stallResyncs = 0;
    std::int64_t stallFirst = 0;
    std::size_t outputTouched = 0;
    std::size_t canaryHits = 0;
};

float noiseFor(std::uint64_t seed, std::int64_t index, int channel)
{
    std::uint64_t z = static_cast<std::uint64_t>(index) * 0x9e3779b97f4a7c15ull + seed * 0xbf58476d1ce4e5b9ull +
                      static_cast<std::uint64_t>(channel) * 0x94d049bb133111ebull + 0x2545f4914f6cdd1dull;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    z ^= z >> 31;
    return 0.3f * (static_cast<float>(static_cast<std::int64_t>(z >> 40) - (1 << 23)) / static_cast<float>(1 << 23));
}

void runTo(PluginHost& host, std::int64_t target, BlockSource& blocks, bool runOutput = true)
{
    while (host.trackPosition() < target)
    {
        const int n = static_cast<int>(std::min<std::int64_t>(blocks.next(), target - host.trackPosition()));
        host.cycle(n, true, runOutput);
    }
}

Result runPlan(const Plan& plan, float highCutHz)
{
    Result out;
    HostOptions options;
    options.inputBlock = kDeclaredBlock;
    options.outputBlock = kDeclaredBlock;
    PluginHost host(options);
    if (plan.offline)
    {
        host.input().setNonRealtime(true);
        host.output().setNonRealtime(true);
    }
    setParamExact(host, "feedback", plan.feedback);
    setParamExact(host, "mix", 50.0f);
    setParam(host, "delay_ms", 100.0f);
    setParamExact(host, "high_cut", highCutHz);

    const std::int64_t edge = plan.edge;
    const std::uint64_t seed = plan.noiseSeed;
    host.source = [edge, seed](std::int64_t index, float& left, float& right) {
        if (index < edge)
        {
            left = noiseFor(seed, index, 0);
            right = noiseFor(seed, index, 1);
        }
        else
        {
            // 起播点之后：相对起播点的同一个纯函数（四路共用）
            left = noiseFor(kPlaySeed, index - edge, 0);
            right = noiseFor(kPlaySeed, index - edge, 1);
        }
    };

    const std::int64_t total = edge + kWindow + 2 * kDeclaredBlock;
    out.left.assign(static_cast<std::size_t>(total + kDeclaredBlock), 0.0f);
    out.right.assign(static_cast<std::size_t>(total + kDeclaredBlock), 0.0f);
    host.onInputBlock = [&](const InputBlock& block) {
        for (int i = 0; i < block.n; ++i)
        {
            out.left[static_cast<std::size_t>(block.first + i)] = block.outLeft[i];
            out.right[static_cast<std::size_t>(block.first + i)] = block.outRight[i];
        }
        const rinne::loop::BlockResult& r = host.inputEngine().lastBlockResult();
        if (r.resetDsp && r.resetAtSample >= 0)
        {
            out.resets.push_back(r.resetAtSample);
        }
    };

    host.transport().add(TransportEvent{edge, TransportKind::Start, 0, 0, false});
    host.prepare();
    REQUIRE(host.selectGroup(1));
    REQUIRE(host.pairUp(256));
    out.pairedAt = host.trackPosition();

    BlockSource before(plan.before);
    BlockSource after(plan.after);
    if (plan.stallLength > 0)
    {
        // 停调：第一个停调样本在起播点之前 64000 + 停调长度处；边界落在块边界上（块在边界处截短）
        out.stallFirst = edge - plan.gap - plan.stallLength;
        REQUIRE(out.stallFirst > out.pairedAt + kFirstPairFade + 2 * kDelay);
        runTo(host, out.stallFirst, before);
        runTo(host, out.stallFirst + plan.stallLength, before, false);
        // 宿主的时间线没有停：Output 一侧的游标按 Input 的游标对齐
        host.outputLane().setCursor(host.inputLane().cursor());
    }
    runTo(host, edge, before);
    runTo(host, total, after);

    const rinne::loop::LoopStatusFields status = host.inputEngine().loopStatus();
    out.playResyncs = status.resyncByReason[reasonIndex(SyncReason::kPlay)];
    out.stallResyncs = status.resyncByReason[reasonIndex(SyncReason::kStallResume)] +
                       status.resyncByReason[reasonIndex(SyncReason::kShift)];
    out.outputTouched = host.outputTouched;
    out.canaryHits = host.canaryHits;
    return out;
}

// 比较起点（相对起播点的偏移）：max(T, 配对后第一个有效读样本 + 500 ms 淡入走完)
std::int64_t compareStart(const Plan& plan, const Result& r)
{
    return std::max(kEdgeFade, r.pairedAt + kDelay + kFirstPairFade - plan.edge);
}

bool sameWindow(const Result& a, const Plan& pa, const Result& b, const Plan& pb, std::int64_t from)
{
    for (std::int64_t k = from; k < from + kWindow; ++k)
    {
        const auto ia = static_cast<std::size_t>(pa.edge + k);
        const auto ib = static_cast<std::size_t>(pb.edge + k);
        if (rinne::test::floatBits(a.left[ia]) != rinne::test::floatBits(b.left[ib]) ||
            rinne::test::floatBits(a.right[ia]) != rinne::test::floatBits(b.right[ib]))
        {
            return false;
        }
    }
    return true;
}

std::size_t differingSamples(const Result& a, const Plan& pa, const Result& b, const Plan& pb, std::int64_t from,
                             std::int64_t to)
{
    std::size_t count = 0;
    for (std::int64_t k = from; k < to; ++k)
    {
        count += rinne::test::floatBits(a.left[static_cast<std::size_t>(pa.edge + k)]) !=
                         rinne::test::floatBits(b.left[static_cast<std::size_t>(pb.edge + k)])
                     ? 1u
                     : 0u;
    }
    return count;
}

// 窗口里左声道回送（输出减干声；混合 50 % 时 g_dry = 1）的高频占比：相邻差的能量 / 自身的能量。
// 白噪声约为 2，低通过的噪声小得多
double returnHighShare(const Result& r, const Plan& plan, std::int64_t from)
{
    double diff = 0.0;
    double energy = 0.0;
    double previous = 0.0;
    for (std::int64_t k = from; k < from + kWindow; ++k)
    {
        const double ret = static_cast<double>(r.left[static_cast<std::size_t>(plan.edge + k)]) -
                           static_cast<double>(noiseFor(kPlaySeed, k, 0));
        if (k > from)
        {
            diff += (ret - previous) * (ret - previous);
        }
        energy += ret * ret;
        previous = ret;
    }
    return energy > 0.0 ? diff / energy : 0.0;
}

// 高切开着与关着的两遍（同一路）：窗口里确有不同，开着时回送的高频占比明显更低
void checkHighCutAudible(const Result& on, const Result& off, const Plan& plan, std::int64_t from)
{
    CHECK(differingSamples(on, plan, off, plan, from, from + kWindow) > 0u);
    const double shareOn = returnHighShare(on, plan, from);
    const double shareOff = returnHighShare(off, plan, from);
    INFO("high-band share: high cut on " << shareOn << ", off " << shareOff);
    CHECK(shareOff > 0.0);
    CHECK(shareOn < 0.5 * shareOff);
}

void checkEdge(const Plan& plan, const Result& r)
{
    CHECK(r.playResyncs == 1u);
    REQUIRE(r.resets.size() == 1u);
    CHECK(r.resets[0] == plan.edge + kEdgeFade);
    CHECK(r.outputTouched == 0u);
    CHECK(r.canaryHits == 0u);
}

} // namespace

TEST_CASE("plugin_host s03b: from the play edge on, the output is bit for bit the same however the host got there",
          "[plugin_host][s03b]")
{
    formalOnly();
    needsSharedMemory();
    for (const float feedback : {50.0f, 100.0f})
    {
        INFO("feedback " << feedback << " %");
        std::vector<Plan> plans(4);
        plans[0] = Plan{false, 61133, Blocks{64, 512, 11}, Blocks{256, 256, 1}, 101, 0, feedback};
        plans[1] = Plan{false, 72407, Blocks{480, 512, 12}, Blocks{480, 512, 13}, 102, 0, feedback};
        plans[2] = Plan{true, 83521, Blocks{1, 4096, 14}, Blocks{1, 4096, 15}, 103, 0, feedback};
        plans[3] = Plan{true, 94793, Blocks{1, 4096, 16}, Blocks{1, 4096, 17}, 104, 0, feedback};
        Result first[2]; // 实时一：[0] 高切开，[1] 高切关
        std::int64_t from = kEdgeFade;
        for (int pass = 0; pass < 2; ++pass)
        {
            const float highCut = pass == 0 ? kHighCutOn : kHighCutOff;
            INFO("high cut " << highCut << " Hz");
            std::vector<Result> results;
            for (const Plan& plan : plans)
            {
                results.push_back(runPlan(plan, highCut));
                checkEdge(plan, results.back());
            }
            for (std::size_t i = 0; i < plans.size(); ++i)
            {
                from = std::max(from, compareStart(plans[i], results[i]));
            }
            CHECK(from == kEdgeFade);
            for (std::size_t i = 1; i < plans.size(); ++i)
            {
                INFO("run " << i << " against run 0");
                CHECK(sameWindow(results[0], plans[0], results[i], plans[i], from));
            }
            // 起播点到 s0 + T 之间，旧读头 5 ms 淡出的尾巴是各路预滚里的内容：实时一与离线一确有不同
            CHECK(differingSamples(results[0], plans[0], results[2], plans[2], 0, kEdgeFade) > 0u);
            first[pass] = results[0];
        }
        checkHighCutAudible(first[0], first[1], plans[0], from);
    }
}

TEST_CASE("plugin_host s05b: a stalled Output does not leave a trace past the play edge", "[plugin_host][s05b]")
{
    formalOnly();
    needsSharedMemory();
    for (const float feedback : {50.0f, 100.0f})
    {
        INFO("feedback " << feedback << " %");
        std::vector<Plan> plans(10);
        plans[0] = Plan{false, 120011, Blocks{256, 256, 1}, Blocks{256, 256, 1}, 201, 0, feedback};
        plans[1] = Plan{false, 120011, Blocks{256, 256, 1}, Blocks{256, 256, 1}, 201, 1440, feedback};
        plans[2] = Plan{false, 131071, Blocks{64, 512, 22}, Blocks{64, 512, 23}, 202, 5760, feedback};
        plans[3] = Plan{true, 140239, Blocks{1, 4096, 24}, Blocks{1, 4096, 25}, 203, 1440, feedback};
        plans[4] = Plan{true, 152233, Blocks{1, 4096, 26}, Blocks{1, 4096, 27}, 204, 5760, feedback};
        plans[5] = Plan{true, 120011, Blocks{1, 4096, 28}, Blocks{1, 4096, 29}, 201, 5760, feedback};
        // 起播点紧挨着停调的恢复：恢复点正好是起播点，或在它之前 3000 个样本
        plans[6] = Plan{false, 120011, Blocks{256, 256, 1}, Blocks{256, 256, 1}, 201, 1440, feedback, 0};
        plans[7] = Plan{false, 131071, Blocks{64, 512, 22}, Blocks{64, 512, 23}, 202, 5760, feedback, 3000};
        plans[8] = Plan{true, 140239, Blocks{1, 4096, 24}, Blocks{1, 4096, 25}, 203, 1440, feedback, 0};
        plans[9] = Plan{true, 152233, Blocks{1, 4096, 26}, Blocks{1, 4096, 27}, 204, 5760, feedback, 3000};
        Result control[2]; // 对照：[0] 高切开，[1] 高切关
        std::int64_t from = kEdgeFade;
        for (int pass = 0; pass < 2; ++pass)
        {
            const float highCut = pass == 0 ? kHighCutOn : kHighCutOff;
            INFO("high cut " << highCut << " Hz");
            std::vector<Result> results;
            for (const Plan& plan : plans)
            {
                results.push_back(runPlan(plan, highCut));
                checkEdge(plan, results.back());
                if (plan.stallLength > 0 && plan.gap >= 64000)
                {
                    CHECK(results.back().stallResyncs >= 1u);
                }
            }
            for (std::size_t i = 0; i < plans.size(); ++i)
            {
                from = std::max(from, compareStart(plans[i], results[i]));
            }
            CHECK(from == kEdgeFade);
            for (std::size_t i = 1; i < plans.size(); ++i)
            {
                INFO("run " << i << " against the control");
                CHECK(sameWindow(results[0], plans[0], results[i], plans[i], from));
            }
            // 分辨力：停调的路与对照在起播之前确有不同（停调改变了回送）
            const Result& stalled = results[1];
            std::size_t differing = 0;
            for (std::int64_t t = stalled.stallFirst; t < plans[1].edge; ++t)
            {
                differing += rinne::test::floatBits(stalled.left[static_cast<std::size_t>(t)]) !=
                                     rinne::test::floatBits(results[0].left[static_cast<std::size_t>(t)])
                                 ? 1u
                                 : 0u;
            }
            CHECK(differing > 1000u);
            control[pass] = results[0];
        }
        checkHighCutAudible(control[0], control[1], plans[0], from);
    }
}
