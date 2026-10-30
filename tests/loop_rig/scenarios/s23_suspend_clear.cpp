// SPDX-License-Identifier: GPL-3.0-or-later
//
// S23 停用复燃：两端一起被宿主挂起（宿主不再调用两端的回调）较久之后恢复。挂起期间环里的数据和 Input 的本地延迟线
// 都原样留着，恢复后若不处理，读头会把挂起之前最后 D 个样本的内容原样放回来（防止止啸之后又自己响起来）。
// 壳量出两端一起挂起超过 250 ms 就请求 Input 清空本地延迟线（requestFlushAfterSuspend）；不超过 250 ms 不请求，
// 内容照常回来。这里钉引擎一侧：请求之后清空，不请求就不清。「超过 250 ms 才请求」是壳的判断，
// 由插件级的复验覆盖（tests/host/plugin_host/test_plugin_s23_s24.cpp）。
//
// 做法：环路延迟 D = 4800，反馈 0（环里存的就是干声，读头的内容逐样本对得回信号源）、混合 50 %，块长 256（恒定）或
// 32…512（带种子随机），Input 先 Output 后。信号源是编号的纯函数噪声，挂起之前（编号小于 T）用一个种子，
// 之后用另一个种子：「旧音频」就是 A 种子的噪声。跑 3 s 之后两端一起不被调用若干个周期（信号源的编号不前进，读位、
// 写头都停着），然后恢复：
//   ① 恢复之前请求清空（壳判定挂起超过 250 ms）；
//   ② 对照格：不请求（壳判定不超过 250 ms）。
// 以 T 为恢复的第一个样本（也是信号源换种子的编号）：
//   ① 读头在 T 起 5 ms 内只是挂起前最后一个回送样本的衰减保持（幅度不超过它），之后到 T + D 之前是静音，T + D
//      起是新音频（B 种子的噪声在 t − D 处的值，从 T + D + 20 ms 起逐位相同，T + D + 5 ms 起每个样本都不为 0）；
//      回送与旧音频（A 种子的噪声在 t − D 处）在 T 起 3 s 里的归一化互相关小于 −40 dB；恰一条「延迟线被清空」事件，
//      原因是壳请求；
//   ② T 起到 T + D 之前读头逐位等于旧音频（挂起之前最后 D 个样本，内容照常回来），T + D 起逐位等于新音频；与旧音频在
//      [T, T + D) 里的归一化互相关大于 0.99；没有「延迟线被清空」事件；
//   两格都没有重同步、没有补 0，环的写头恰为样本总数（挂起期间没有写）。
//
// 标 ⛔ 的是删除式验证点：把对应的实现改坏，本场景必须变红。
//   ⛔ 请求清空之后不清（去掉清线）→ ① 变红：回送里仍有旧音频，互相关升高，静音段不静音。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "loop/LoopEvents.h"
#include "s01_rig_kit.h"

namespace
{

using namespace rinne::rig;
using namespace rinne::rig::scn;
using rinne::loop::LoopEventCode;

constexpr int kMaxBlock = 512;
constexpr std::int64_t kDelay = 4800;
constexpr std::int64_t kOnset = 960;
constexpr std::int64_t kSecond = 48000;
constexpr std::int64_t kSuspendAt = 3 * kSecond;
constexpr std::size_t kGapCycles = 20;

enum class Blocks
{
    kFixed,
    kRandom
};

// 编号小于 turn 的样本用 A 种子，之后用 B 种子
float noiseAt(std::int64_t t, std::int64_t turn, int channel, bool forceOld = false)
{
    const bool old = forceOld || t < turn;
    return 0.25f * hashNoise(static_cast<std::uint64_t>(t), static_cast<std::uint64_t>((old ? 61 : 71) + channel));
}

struct Trace
{
    std::vector<float> tapLeft;
    std::int64_t resumeAt = 0; // T
    std::uint32_t resyncs = 0;
    std::uint64_t overrun = 0;
    std::uint64_t writeHead = 0;
    std::uint64_t samples = 0;
    std::uint32_t flushEvents = 0;
    std::uint32_t flushEventsByShell = 0;
};

Trace runOne(Blocks blocks, bool requestFlush)
{
    Trace trace;
    rinne::test::Rng rng(0x5a110023u);
    BlockScript script;
    const std::int64_t total = kSuspendAt + 4 * kSecond + 4 * kMaxBlock;
    for (std::int64_t sum = 0; sum < total + 2 * kMaxBlock;)
    {
        const int n = blocks == Blocks::kFixed ? 256 : 32 + rng.below(kMaxBlock - 32 + 1);
        script.lengths.push_back(n);
        sum += n;
    }
    // 挂起的第一个周期：第 3 s 之后的第一个周期；T = 它的第一个样本
    std::size_t gapFirst = 0;
    std::int64_t position = 0;
    while (position < kSuspendAt)
    {
        position += script.lengths[gapFirst++];
    }
    trace.resumeAt = position;
    const std::int64_t turn = position;

    BlockScript skipped = script;
    skipped.skips.assign(script.lengths.size(), false);
    for (std::size_t i = gapFirst; i < gapFirst + kGapCycles; ++i)
    {
        skipped.skips[i] = true;
    }

    auto host = makeHost(kSr, kMaxBlock);
    REQUIRE(host != nullptr);
    host->setBlockScripts(skipped, skipped);
    const SourceFn source = [turn](std::uint64_t first, float* left, float* right, int n) {
        for (int i = 0; i < n; ++i)
        {
            const auto t = static_cast<std::int64_t>(first) + i;
            left[i] = noiseAt(t, turn, 0);
            right[i] = noiseAt(t, turn, 1);
        }
    };
    host->setSource(source);
    ParamsScript params;
    params.snapshots = {loopParams(0.0f, 50.0f, 100.0f)};
    host->setParamsScript(params);
    rinne::test::InputReference reference(kSr);
    const rinne::engine::EngineParams p = loopParams(0.0f, 50.0f, 100.0f);

    for (std::size_t i = 0; static_cast<std::int64_t>(trace.samples) < total; ++i)
    {
        if (i == gapFirst + kGapCycles && requestFlush)
        {
            host->input().requestFlushAfterSuspend();
        }
        const std::uint64_t first = host->sourcePosition();
        const int n = host->runCycle(i);
        if (n > 0)
        {
            const auto count = static_cast<std::size_t>(n);
            std::vector<float> dl(count);
            std::vector<float> dr(count);
            source(first, dl.data(), dr.data(), n);
            reference.block(p, host->inputLastBlock(), false, dl.data(), dr.data(), n);
            trace.samples += count;
        }
        host->drainEvents([&](const rinne::diag::Event& e) {
            if (e.code == static_cast<std::uint16_t>(LoopEventCode::kLineCleared))
            {
                ++trace.flushEvents;
                trace.flushEventsByShell += e.a == 2u ? 1u : 0u;
            }
        });
    }
    trace.tapLeft = reference.tapLeft;
    const auto& status = host->inputStatus();
    trace.resyncs = status.resyncCount;
    trace.overrun = status.overrun;
    trace.writeHead = host->writeHead();
    return trace;
}

double correlationDb(const std::vector<float>& tap, std::int64_t from, std::int64_t to, std::int64_t turn)
{
    double xy = 0.0;
    double xx = 0.0;
    double yy = 0.0;
    for (std::int64_t t = from; t < to; ++t)
    {
        const double a = tap[static_cast<std::size_t>(t)];
        const double b = noiseAt(t - kDelay, turn, 0, true);
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

class SuspendClearScenario final : public Scenario
{
public:
    const char* id() const noexcept override { return "s23_suspend_clear"; }

    bool run(ScenarioContext&) override
    {
        Verdict v;
        for (const Blocks blocks : {Blocks::kFixed, Blocks::kRandom})
        {
            INFO("blocks " << (blocks == Blocks::kFixed ? "fixed 256" : "random"));
            {
                INFO("flush requested (suspended longer than 250 ms)");
                const Trace t = runOne(blocks, true);
                checkCommon(v, t);
                checkCleared(v, t);
            }
            {
                INFO("no flush requested (suspended up to 250 ms)");
                const Trace t = runOne(blocks, false);
                checkCommon(v, t);
                checkKept(v, t);
            }
        }
        return v.ok();
    }

private:
    static void checkCommon(Verdict& v, const Trace& t)
    {
        RIG_EQ(v, t.resyncs, 0u);
        RIG_EQ(v, t.overrun, std::uint64_t{0});
        RIG_EQ(v, t.writeHead, t.samples);
        RIG_CHECK(v, t.tapLeft.size() >= static_cast<std::size_t>(t.resumeAt + 3 * kSecond));
    }

    static void checkCleared(Verdict& v, const Trace& t)
    {
        const std::int64_t from = t.resumeAt;
        const auto total = static_cast<std::int64_t>(t.tapLeft.size());
        RIG_EQ(v, t.flushEvents, 1u);
        RIG_EQ(v, t.flushEventsByShell, 1u);
        // 前 5 ms：挂起之前最后一个回送样本的衰减保持
        const float last = std::fabs(t.tapLeft[static_cast<std::size_t>(from - 1)]);
        bool held = true;
        for (std::int64_t at = from; at < from + kFadeOutSamples; ++at)
        {
            held = held && std::fabs(t.tapLeft[static_cast<std::size_t>(at)]) <= last * (1.0f + 1.0e-6f) + 1.0e-7f;
        }
        RIG_CHECK(v, held);
        RIG_CHECK(v, allZero(t.tapLeft, static_cast<std::size_t>(from + kFadeOutSamples + 2),
                             static_cast<std::size_t>(from + kDelay)));
        bool exact = true;
        bool alive = true;
        bool bounded = true;
        for (std::int64_t at = from + kDelay; at < total; ++at)
        {
            const float want = noiseAt(at - kDelay, from, 0);
            const float got = t.tapLeft[static_cast<std::size_t>(at)];
            if (at >= from + kDelay + kOnset)
            {
                exact = exact && std::fabs(got - want) <= 1.0e-6f;
            }
            if (at >= from + kDelay + kFadeOutSamples)
            {
                alive = alive && got != 0.0f;
            }
            if (got != 0.0f)
            {
                bounded =
                    bounded && (got > 0.0f) == (want > 0.0f) && std::fabs(got) <= std::fabs(want) * (1.0f + 1.0e-6f);
            }
        }
        RIG_CHECK(v, exact);
        RIG_CHECK(v, alive);
        RIG_CHECK(v, bounded);
        RIG_CHECK(v, correlationDb(t.tapLeft, from, std::min(total, from + 3 * kSecond), from) < -40.0);
    }

    static void checkKept(Verdict& v, const Trace& t)
    {
        const std::int64_t from = t.resumeAt;
        const auto total = static_cast<std::int64_t>(t.tapLeft.size());
        RIG_EQ(v, t.flushEvents, 0u);
        bool old = true;
        for (std::int64_t at = from; at < from + kDelay; ++at)
        {
            old = old && std::fabs(t.tapLeft[static_cast<std::size_t>(at)] - noiseAt(at - kDelay, from, 0)) <= 1.0e-6f;
        }
        RIG_CHECK(v, old);
        bool fresh = true;
        for (std::int64_t at = from + kDelay; at < total; ++at)
        {
            fresh =
                fresh && std::fabs(t.tapLeft[static_cast<std::size_t>(at)] - noiseAt(at - kDelay, from, 0)) <= 1.0e-6f;
        }
        RIG_CHECK(v, fresh);
        RIG_CHECK(v, correlationDb(t.tapLeft, from, from + kDelay, from) > -0.1);
    }
};

} // namespace

RINNE_RIG_SCENARIO("s23_suspend_clear")
{
    return new SuspendClearScenario();
}

TEST_CASE("s23: after both ends are suspended, a requested flush clears the old loop content and no request keeps it",
          "[loop_rig][s23]")
{
    CHECK(rinne::rig::scn::runRegisteredScenario("s23_suspend_clear"));
}
