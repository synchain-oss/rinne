// SPDX-License-Identifier: GPL-3.0-or-later
//
// S17 交替漂移（模拟跨通道抖动）：Output 的块长在两个值之间交替、Input 的块长恒定，写头相对读位持续漂移。
//
// 环路层的判据是严格模式：漂移量 m = W − (R + off)，连续两次对齐检查 m 都不为 0 就重同步（原因：位移）；
// |m| 超过门限 N_cap = max(两端声明的最大块长, 8192) 立即重同步（原因：跳变）；10 s 内这两类重同步累计 3 次，
// 转入容忍模式并置「时序不稳定」（unstable），此后不再因位移重同步，跳变照旧。
//
// 环路延迟 D = 4800、Input 块长恒为 256、反馈 50 %、混合 50 %，Input 先 Output 后，宿主没给时间戳，信号是两个低频余弦。
// 五路：
//   Output 256 与 288 交替   每两个周期写头多走 32 帧：m 在第 2、3 周期连续两次为 32 → 第一次位移重同步，
//                           此后每两个周期一次，第 3 次（周期 6、7）转容忍模式并置不稳定，一共恰 3 次，全在头 1 s 里；
//                           之后 m 以 +32 / 2 周期线性增长，直到 |m| > 8192 才跳变重同步（原因：跳变，不是位移）；
//   Output 256 与 224 交替   符号相反：同样的 3 次位移重同步，转容忍模式，之后跳变只在 |m| > 8192 时；
//   对照：288 与 224 交替    每两个周期净漂移为 0：m 在 0 与 32 之间交替，从不连续两次不为 0，被吸收，
//                           零重同步、不置不稳定、被吸收的检查次数 > 0；
// 每路断言：重同步的时刻与原因、容忍模式与不稳定的置位（置位后不清）、任意 1 s 窗口里重同步 ≤ 4 次、输出有限且有界。
//
// 标 ⛔ 的是删除式验证点：把对应的实现改坏，本场景必须变红。
//   ⛔ 容忍模式不再屏蔽位移重同步 → 第 3 次之后仍每两个周期重同步一次，1 s 窗口里远超 4 次；
//   ⛔ 位移判据改成「m 不为 0 一次就重同步」→ 对照一路出现重同步。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "loop/LoopStatus.h"
#include "loop/StreamSync.h"
#include "s01_rig_kit.h"

namespace
{

using namespace rinne::rig;
using namespace rinne::rig::scn;
using rinne::loop::SyncReason;

constexpr int kInputBlock = 256;
constexpr int kMaxBlock = 1024;
constexpr std::size_t kCycles = 1600; // 约 8.5 s
constexpr std::int64_t kCap = 8192; // N_cap = max(声明的最大块长, 8192)
constexpr std::size_t kSecondCycles = 188; // 1 s = 48000 样本 ≈ 187.5 个 256 样本的周期
constexpr std::size_t kReasonShift = static_cast<std::size_t>(SyncReason::kShift);
constexpr std::size_t kReasonJump = static_cast<std::size_t>(SyncReason::kJump);

struct CycleState
{
    std::uint32_t resyncCount = 0;
    std::uint32_t shift = 0;
    std::uint32_t jump = 0;
    std::uint32_t tolerant = 0;
    std::uint32_t unstable = 0;
    std::int64_t drift = 0;
    std::uint32_t absorbed = 0;
};

struct Trace
{
    std::vector<float> out;
    std::vector<CycleState> cycles;

    std::vector<std::size_t> resyncCycles() const
    {
        std::vector<std::size_t> r;
        for (std::size_t i = 1; i < cycles.size(); ++i)
        {
            if (cycles[i].resyncCount > cycles[i - 1].resyncCount)
            {
                r.push_back(i);
            }
        }
        return r;
    }
};

Trace runLeg(int outputEven, int outputOdd)
{
    Trace trace;
    auto host = makeHost(kSr, kMaxBlock);
    REQUIRE(host != nullptr);
    BlockScript input;
    input.lengths = {kInputBlock};
    BlockScript output;
    output.lengths = {outputEven, outputOdd};
    host->setBlockScripts(input, output);
    host->setCallOrder(CallOrder::kInputFirst);
    host->setSource(tones());
    ParamsScript params;
    params.snapshots = {loopParams(50.0f, 50.0f, 100.0f)};
    host->setParamsScript(params);
    for (std::size_t i = 0; i < kCycles; ++i)
    {
        const int n = host->runCycle(i);
        const rinne::loop::LoopStatusFields& s = host->inputStatus();
        CycleState c;
        c.resyncCount = s.resyncCount;
        c.shift = s.resyncByReason[kReasonShift];
        c.jump = s.resyncByReason[kReasonJump];
        c.tolerant = s.tolerant;
        c.unstable = s.unstable;
        c.drift = s.drift;
        c.absorbed = s.offsetDriftBlocks;
        trace.cycles.push_back(c);
        trace.out.insert(trace.out.end(), host->inputOutputLeft().begin(), host->inputOutputLeft().begin() + n);
    }
    return trace;
}

bool allFinite(const std::vector<float>& x)
{
    return std::all_of(x.begin(), x.end(), [](float f) { return std::isfinite(f); });
}

class AlternatingDriftScenario final : public Scenario
{
public:
    const char* id() const noexcept override { return "s17_alternating_drift"; }

    bool run(ScenarioContext&) override
    {
        Verdict v;
        {
            INFO("output 256 / 288 alternating");
            drifting(v, runLeg(256, 288));
        }
        {
            INFO("output 256 / 224 alternating");
            drifting(v, runLeg(256, 224));
        }
        {
            INFO("control: output 288 / 224 alternating (no net drift)");
            const Trace t = runLeg(288, 224);
            const CycleState& last = t.cycles.back();
            RIG_EQ(v, last.resyncCount, 0u);
            RIG_EQ(v, last.unstable, 0u);
            RIG_EQ(v, last.tolerant, 0u);
            RIG_CHECK(v, last.absorbed > 0u); // m 在 0 与 32 之间交替，被吸收，不是没看见
            for (const CycleState& c : t.cycles)
            {
                RIG_CHECK(v, std::llabs(c.drift) <= 32);
            }
            RIG_CHECK(v, allFinite(t.out));
            RIG_CHECK(v, peakOf(t.out, 0, t.out.size()) < 1.5f);
        }
        return v.ok();
    }

private:
    // 持续漂移的两路共同的断言
    static void drifting(Verdict& v, const Trace& t)
    {
        const std::vector<std::size_t> resyncs = t.resyncCycles();
        RIG_CHECK(v, resyncs.size() >= 3);
        if (resyncs.size() < 3)
        {
            return;
        }

        // 头三次是位移重同步，全在头 1 s 里；第三次转容忍模式并置不稳定，之后不清
        for (std::size_t k = 0; k < 3; ++k)
        {
            INFO("resync " << k << " at cycle " << resyncs[k]);
            RIG_CHECK(v, resyncs[k] < kSecondCycles);
        }
        const std::size_t third = resyncs[2];
        RIG_EQ(v, t.cycles[third].shift, 3u);
        RIG_EQ(v, t.cycles[third].jump, 0u);
        RIG_EQ(v, t.cycles[third - 1].tolerant, 0u);
        RIG_EQ(v, t.cycles[third - 1].unstable, 0u);
        for (std::size_t i = third; i < t.cycles.size(); ++i)
        {
            RIG_EQ(v, t.cycles[i].tolerant, 1u);
            RIG_EQ(v, t.cycles[i].unstable, 1u);
            RIG_EQ(v, t.cycles[i].shift, 3u); // 容忍模式之后不再有位移重同步
        }

        // 之后的重同步都是跳变，而且只在 |m| 逼近门限时发生（前一周期的 m 在 (N_cap − 64, N_cap] 里）
        for (std::size_t k = 3; k < resyncs.size(); ++k)
        {
            const std::size_t at = resyncs[k];
            INFO("later resync at cycle " << at);
            RIG_EQ(v, t.cycles[at].jump, t.cycles[at - 1].jump + 1u);
            const std::int64_t before = std::llabs(t.cycles[at - 1].drift);
            RIG_CHECK(v, before <= kCap);
            RIG_CHECK(v, before > kCap - 64);
        }
        // 跳变确实发生过，吸收也不是没做事：这一路在 8.5 s 里漂过了门限
        RIG_CHECK(v, t.cycles.back().jump >= 1u);

        // 任意 1 s 窗口里重同步 ≤ 4 次
        std::uint32_t worst = 0;
        for (std::size_t i = kSecondCycles; i < t.cycles.size(); ++i)
        {
            worst = std::max(worst, t.cycles[i].resyncCount - t.cycles[i - kSecondCycles].resyncCount);
        }
        worst = std::max(worst, t.cycles[std::min(kSecondCycles, t.cycles.size() - 1)].resyncCount);
        RIG_CHECK(v, worst <= 4u);

        RIG_CHECK(v, allFinite(t.out));
        RIG_CHECK(v, peakOf(t.out, 0, t.out.size()) < 1.5f);
    }
};

} // namespace

RINNE_RIG_SCENARIO("s17_alternating_drift")
{
    return new AlternatingDriftScenario();
}

TEST_CASE("s17: alternating output block lengths resync at most 3 times then go tolerant and unstable",
          "[loop_rig][s17]")
{
    CHECK(rinne::rig::scn::runRegisteredScenario("s17_alternating_drift"));
}
