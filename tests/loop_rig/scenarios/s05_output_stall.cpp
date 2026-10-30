// SPDX-License-Identifier: GPL-3.0-or-later
//
// S05 Output 停调：Input 照常被调用，Output 有几个周期没被调用（宿主挂起了它、用户停了它所在的轨道），之后恢复。
// 环上这段时间没有写方：写头不动，Input 的读位照常前进。
//
// 环路延迟 D = 4800，块长 256（恒定）或 32…512（带种子随机），声明的最大块长 N_max = 512，反馈 0（环里存的就是干声，
// 读头的内容（Input 的回送在进安全链之前的样子，来自参照安全链记下的读头）按 Input 自己的样本坐标 t 看：
//   - 回送样本 t 只可能来自信号源的样本 t − D，且那个样本必须是 Output 真写过的（停调期间的样本从没进过环）；
//   - 每个非零回送样本的符号与 t − D 处的信号源相同、幅度不大于它（比值在 0…1 之间：淡出、淡入只会压低）；
//   - 比值的变化是连续的：相邻两个可比样本之间比值最多变 2/240（淡出 5 ms 的斜率是 1/240），没有阶跃；
//   - 回送被压低的窗口是 [s, e)：断档的三档 s = a + 2·N_max（a = 停调的第一个样本；W 未动且 R 前进满 2·N_max 的那一块
//     开始淡出），1 块一档 s = 重同步那个周期的第一个样本（连续两次检查对不齐，是恢复后的第二个周期）；
//     e = 重同步那个周期的第一个样本 + D + 20 ms（断档恢复在写头重新动起来的第一个周期重建读头；新读头从那一刻的
//     写头起才有数据，数据在 D 之后到，再走 20 ms 的 onset）。窗口之外回送逐位等于 t − D 处的信号源（比值恰为 1，
//     误差 1e-6 以内）：
//     没有多余的静音，也没有旧数据混进来；窗口里必须真有被压低的样本（停调确实起了作用）；
//   - 恢复之后（窗口之后 4 s）回送与停调之前的信号源在「陈旧候选」延迟（D ± 1、2 倍停调长度或块长）上的归一化互相关
//     都小于 −40 dB。回送与 t − D 处的信号源逐位相同时它必然成立，这里只是把「旧数据」这个说法也钉成数。
// 计数：整个跑下来恰 1 次重同步（停调 ≥ 2·N_max 的三档是断档恢复；1 块一档写头少了一块，连续两次对不齐，是位移），
// 没有跳变、没有容忍模式；断档的三档恰一条「断档」事件与一条「断档解除」事件，1 块一档一条也没有；
// 补 0 的样本数恰为（停调长度 − D）加上恢复那一块的长度（停调不到 D 时为 0）：读头读过写头之后取不到的样本补 0；
// 不清空本地延迟线：Input 一直在被调用，环上的写方也还在，只有两端一起停用才清（见停用复燃的场景）。
// Output 写进环的帧数 = 全部样本 − 停调的样本。
//
// 反馈 50 % 再跑一遍四档（固定块长）：输出与参照安全链按文档接线重算的结果逐位相同、全程有限且有界、同样的重同步次数。
//
// 标 ⛔ 的是删除式验证点：把对应的实现改坏，本场景必须变红。
//   ⛔ 断档恢复之后读头不重建（沿用旧的读位）→ 恢复之后的回送延迟不再是 D，窗口之外的逐位相等失败；
//   ⛔ 断档的淡出缩成 1 个样本 → 比值出现阶跃；
//   ⛔ 新读头的 onset 缩成 1 个样本 → 恢复处回送从 0 一步跳到满，比值出现阶跃。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "loop/LoopEvents.h"
#include "loop/StreamSync.h"
#include "s01_rig_kit.h"

namespace
{

using namespace rinne::rig;
using namespace rinne::rig::scn;
using rinne::loop::LoopEventCode;
using rinne::loop::SyncReason;

constexpr int kMaxBlock = 512;
constexpr std::int64_t kDelay = 4800;
constexpr std::int64_t kOnset = 960; // 新读头的 onset：20 ms
constexpr std::int64_t kStallStartSamples = 3 * 48000;
constexpr std::int64_t kSettled = 40000; // 配对的淡入（500 ms）走完之后
constexpr std::int64_t kTail = 200000; // 恢复之后留的长度：窗口之后还有 4 s 以上

struct Stall
{
    const char* name;
    std::int64_t samples; // 不短于这么多个样本（取整数个周期）；0 = 恰 1 块
    bool detected; // 长到会被判为断档（≥ 2·N_max）
};

enum class Blocks
{
    kFixed,
    kRandom
};

struct Trace
{
    std::vector<float> srcLeft, srcRight, tapLeft, tapRight, outLeft, outRight;
    std::vector<bool> written; // 信号源的这个样本有没有被 Output 写进环
    std::int64_t stallFirst = 0; // a
    std::int64_t stallLength = 0; // b − a
    int maxBlockSeen = 0;
    std::vector<std::int64_t> cycleStarts; // 每个周期的第一个样本
    std::size_t resumeCycle = 0; // 恢复后的第一个周期
    bool matchesReference = false;
    std::uint32_t resyncs = 0;
    std::uint32_t byReason[9]{};
    std::uint32_t tolerant = 0;
    std::uint32_t unstable = 0;
    std::uint64_t overrun = 0;
    std::uint32_t stalledEvents = 0;
    std::uint32_t stallClearedEvents = 0;
    std::uint32_t lineClearedEvents = 0;
    std::uint64_t writeHead = 0;
    std::size_t canaryHits = 0;
    std::uint64_t writtenFrames = 0;
};

SourceFn noise()
{
    return [](std::uint64_t first, float* left, float* right, int n) {
        for (int i = 0; i < n; ++i)
        {
            left[i] = 0.25f * hashNoise(first + static_cast<std::uint64_t>(i), 11);
            right[i] = 0.25f * hashNoise(first + static_cast<std::uint64_t>(i), 12);
        }
    };
}

Trace runOne(const Stall& stall, Blocks blocks, float feedbackPercent)
{
    Trace trace;
    rinne::test::Rng rng(0x5a110005u);
    BlockScript script;
    // 先铺满：脚本里的块长一直排到总长够用为止
    const std::int64_t total = kStallStartSamples + 96000 + 2 * kMaxBlock + kTail;
    for (std::int64_t sum = 0; sum < total + 2 * kMaxBlock;)
    {
        const int n = blocks == Blocks::kFixed ? 256 : 32 + rng.below(kMaxBlock - 32 + 1);
        script.lengths.push_back(n);
        sum += n;
    }
    // 停调的周期：第 3 s 之后的第一个周期起，取整数个周期
    std::size_t first = 0;
    std::int64_t position = 0;
    while (position < kStallStartSamples)
    {
        position += script.lengths[first++];
    }
    std::size_t count = 0;
    std::int64_t stalled = 0;
    do
    {
        stalled += script.lengths[first + count++];
    } while (stalled < stall.samples);
    trace.stallFirst = position;
    trace.stallLength = stalled;
    trace.resumeCycle = first + count;

    BlockScript outputScript = script;
    outputScript.skips.assign(script.lengths.size(), false);
    for (std::size_t i = first; i < first + count; ++i)
    {
        outputScript.skips[i] = true;
    }

    auto host = makeHost(kSr, kMaxBlock);
    REQUIRE(host != nullptr);
    host->setBlockScripts(script, outputScript);
    Recorder rec(*host, noise());
    const rinne::engine::EngineParams p = loopParams(feedbackPercent, 50.0f, 100.0f);

    std::int64_t cursor = 0;
    for (std::size_t i = 0; cursor < total; ++i)
    {
        const int n = rec.cycle(i, p);
        if (n <= 0)
        {
            break;
        }
        trace.cycleStarts.push_back(cursor);
        const bool skipped = i >= first && i < first + count;
        trace.written.insert(trace.written.end(), static_cast<std::size_t>(n), !skipped);
        trace.maxBlockSeen = std::max(trace.maxBlockSeen, n);
        cursor += n;
        host->drainEvents([&](const rinne::diag::Event& e) {
            trace.stalledEvents += e.code == static_cast<std::uint16_t>(LoopEventCode::kStalled) ? 1u : 0u;
            trace.stallClearedEvents += e.code == static_cast<std::uint16_t>(LoopEventCode::kStallCleared) ? 1u : 0u;
            trace.lineClearedEvents += e.code == static_cast<std::uint16_t>(LoopEventCode::kLineCleared) ? 1u : 0u;
        });
    }
    trace.srcLeft = rec.dryLeft;
    trace.srcRight = rec.dryRight;
    trace.tapLeft = rec.reference.tapLeft;
    trace.tapRight = rec.reference.tapRight;
    trace.outLeft = rec.outLeft;
    trace.outRight = rec.outRight;
    trace.matchesReference = rec.matchesReference();
    const auto& status = host->inputStatus();
    trace.resyncs = status.resyncCount;
    for (int r = 0; r < 9; ++r)
    {
        trace.byReason[r] = status.resyncByReason[r];
    }
    trace.tolerant = status.tolerant;
    trace.unstable = status.unstable;
    trace.overrun = status.overrun;
    trace.writeHead = host->writeHead();
    trace.canaryHits = host->canaryHits();
    trace.writtenFrames = host->outputWrittenFrames();
    return trace;
}

// 归一化互相关（dB）：x[from + i] 与 y[from + i − lag]，i ∈ [0, count)
double correlationDb(const std::vector<float>& x, const std::vector<float>& y, std::int64_t from, std::int64_t count,
                     std::int64_t lag)
{
    double xy = 0.0;
    double xx = 0.0;
    double yy = 0.0;
    for (std::int64_t i = 0; i < count; ++i)
    {
        const std::int64_t at = from + i;
        const std::int64_t other = at - lag;
        if (at < 0 || other < 0 || at >= static_cast<std::int64_t>(x.size()) ||
            other >= static_cast<std::int64_t>(y.size()))
        {
            return -300.0;
        }
        const double a = x[static_cast<std::size_t>(at)];
        const double b = y[static_cast<std::size_t>(other)];
        xy += a * b;
        xx += a * a;
        yy += b * b;
    }
    if (xx <= 0.0 || yy <= 0.0)
    {
        return -300.0;
    }
    return 20.0 * std::log10(std::max(std::fabs(xy) / std::sqrt(xx * yy), 1.0e-15));
}

class OutputStallScenario final : public Scenario
{
public:
    const char* id() const noexcept override { return "s05_output_stall"; }

    bool run(ScenarioContext&) override
    {
        Verdict v;
        const Stall stalls[] = {
            {"1 block", 0, false},
            {"30 ms", 1440, true},
            {"120 ms", 5760, true},
            {"2 s", 96000, true},
        };
        for (const Blocks blocks : {Blocks::kFixed, Blocks::kRandom})
        {
            for (const Stall& stall : stalls)
            {
                INFO("stall " << stall.name << ", blocks " << (blocks == Blocks::kFixed ? "fixed 256" : "random"));
                const Trace trace = runOne(stall, blocks, 0.0f);
                checkContent(v, stall, trace);
                checkCounters(v, stall, trace);
            }
        }
        for (const Stall& stall : stalls)
        {
            INFO("stall " << stall.name << ", feedback 50 %");
            const Trace trace = runOne(stall, Blocks::kFixed, 50.0f);
            RIG_CHECK(v, trace.matchesReference);
            RIG_CHECK(v, allFiniteBounded(trace.outLeft) && allFiniteBounded(trace.outRight));
            checkCounters(v, stall, trace);
        }
        return v.ok();
    }

private:
    static bool allFiniteBounded(const std::vector<float>& x)
    {
        return std::all_of(x.begin(), x.end(), [](float f) { return std::isfinite(f) && std::fabs(f) < 1.0f; });
    }

    static void checkCounters(Verdict& v, const Stall& stall, const Trace& t)
    {
        RIG_EQ(v, t.resyncs, 1u);
        RIG_EQ(v, t.byReason[static_cast<std::size_t>(stall.detected ? SyncReason::kStallResume : SyncReason::kShift)],
               1u);
        RIG_EQ(v, t.byReason[static_cast<std::size_t>(SyncReason::kJump)], 0u);
        RIG_EQ(v, t.tolerant, 0u);
        RIG_EQ(v, t.unstable, 0u);
        // 读头读过写头之后取不到的样本补 0：停调超过 D 之后的那部分，加上恢复那一块里 Input 还没看到写头动
        const std::int64_t beyond = t.stallLength - kDelay;
        const std::int64_t resumeBlock = t.cycleStarts[t.resumeCycle + 1] - t.cycleStarts[t.resumeCycle];
        RIG_EQ(v, t.overrun, static_cast<std::uint64_t>(beyond > 0 ? beyond + resumeBlock : 0));
        RIG_EQ(v, t.canaryHits, std::size_t{0});
        RIG_EQ(v, t.stalledEvents, stall.detected ? 1u : 0u);
        RIG_EQ(v, t.stallClearedEvents, stall.detected ? 1u : 0u);
        // Input 一直在被调用，环上的写方也还在（只是没写）：两端都停用才清线，这里不清
        RIG_EQ(v, t.lineClearedEvents, 0u);
        // Output 写了的帧数 = 全部样本 − 停调的样本
        RIG_EQ(v, t.writeHead, t.writtenFrames);
        RIG_EQ(v, static_cast<std::int64_t>(t.writeHead), static_cast<std::int64_t>(t.written.size()) - t.stallLength);
    }

    static void checkContent(Verdict& v, const Stall& stall, const Trace& t)
    {
        const auto total = static_cast<std::int64_t>(t.tapLeft.size());
        RIG_CHECK(v, total >= kStallStartSamples + t.stallLength + kTail);
        const std::int64_t a = t.stallFirst;
        // 读头重建的那个周期的第一个样本：断档恢复在写头重新动起来的第一块（恢复后的第一个周期 Input 先于 Output，
        // 还看不到写头动，所以是下一个周期）；位移要连续两次检查对不齐，再晚一个周期
        const std::int64_t resync = t.cycleStarts[t.resumeCycle + (stall.detected ? 1 : 2)];
        // 回送最早在这里开始被压低：断档在 R 前进满 2·N_max 的那一块开始淡出，位移在重同步的那一块开始淡出
        const std::int64_t degradeFrom = stall.detected ? a + 2 * kMaxBlock : resync;
        // 新读头从重建那一刻的写头起才有数据（那一点的回送在 resync + D），再走 20 ms 的 onset
        const std::int64_t degradeTo = resync + kDelay + kOnset;

        std::size_t degraded = 0; // 窗口里被压低或为 0 的样本数
        bool lagOk = true; // 非零回送只来自 t − D 且那里确实写过
        bool signAndBound = true; // 符号相同、幅度不大于信号源
        bool exactOutside = true; // 窗口之外逐位相等
        bool continuous = true; // 比值连续
        std::int64_t lastAt = -1;
        double lastRatio = 1.0;
        const std::vector<float>* taps[2] = {&t.tapLeft, &t.tapRight};
        const std::vector<float>* srcs[2] = {&t.srcLeft, &t.srcRight};
        for (int ch = 0; ch < 2; ++ch)
        {
            const std::vector<float>& tap = *taps[ch];
            const std::vector<float>& src = *srcs[ch];
            lastAt = -1;
            lastRatio = 1.0;
            for (std::int64_t at = kSettled; at < total; ++at)
            {
                const std::int64_t from = at - kDelay;
                const float want = src[static_cast<std::size_t>(from)];
                const float got = tap[static_cast<std::size_t>(at)];
                const bool wasWritten = t.written[static_cast<std::size_t>(from)];
                const bool inWindow = at >= degradeFrom && at < degradeTo;
                if (got != 0.0f)
                {
                    lagOk = lagOk && wasWritten;
                    signAndBound = signAndBound && (got > 0.0f) == (want > 0.0f) &&
                                   std::fabs(got) <= std::fabs(want) * (1.0f + 1.0e-6f);
                }
                if (!inWindow)
                {
                    const bool okHere = wasWritten && std::fabs(got - want) <= 1.0e-6f;
                    exactOutside = exactOutside && okHere;
                }
                else if (std::fabs(got - want) > 1.0e-6f)
                {
                    ++degraded;
                }
                if (wasWritten && std::fabs(want) > 0.02f)
                {
                    const double ratio = static_cast<double>(got) / static_cast<double>(want);
                    if (lastAt >= 0)
                    {
                        continuous = continuous && std::fabs(ratio - lastRatio) <=
                                                       2.0 / 240.0 * static_cast<double>(at - lastAt) + 1.0e-4;
                    }
                    lastAt = at;
                    lastRatio = ratio;
                }
            }
        }
        RIG_CHECK(v, lagOk);
        RIG_CHECK(v, signAndBound);
        RIG_CHECK(v, exactOutside);
        RIG_CHECK(v, continuous);
        RIG_CHECK(v, degraded > static_cast<std::size_t>(kDelay / 2));

        // 恢复之后的回送与停调之前的信号源在陈旧候选延迟上不相关
        const std::int64_t from = degradeTo;
        const std::int64_t count = std::min<std::int64_t>(total - from, 4 * 48000);
        RIG_CHECK(v, count >= 3 * 48000);
        double worst = -300.0;
        for (const std::int64_t k : {-2, -1, 1, 2})
        {
            for (const std::int64_t base : {t.stallLength, std::int64_t{256}})
            {
                const std::int64_t lag = kDelay + k * base;
                if (lag == kDelay)
                {
                    continue;
                }
                worst = std::max(worst, correlationDb(t.tapLeft, t.srcLeft, from, count, lag));
            }
        }
        RIG_CHECK(v, worst < -40.0);
    }
};

} // namespace

RINNE_RIG_SCENARIO("s05_output_stall")
{
    return new OutputStallScenario();
}

TEST_CASE("s05: after the Output stops being called and comes back, every loopback sample is the delayed input",
          "[loop_rig][s05]")
{
    CHECK(rinne::rig::scn::runRegisteredScenario("s05_output_stall"));
}
