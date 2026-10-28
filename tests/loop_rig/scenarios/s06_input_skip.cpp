// SPDX-License-Identifier: GPL-3.0-or-later
//
// S06 Input 跳过 k 块：Output 照常写，Input 有几个周期没被调用（宿主跳过了它），回来之后写头比读位领先了 k 块。
//
// 环路层的判据是严格模式：漂移量 m = W − (R + off)，连续两次对齐检查 m 都不为 0 就重同步（原因：位移），
// |m| 超过门限 N_cap = max(两端声明的最大块长, 8192) 立即重同步（原因：跳变）；10 s 内这两类重同步
// 累计 3 次，转入容忍模式并置「时序不稳定」，此后不再因位移重同步（跳变照旧）。
//
// 环路延迟 D = 4800、块长恒为 256、反馈 50 %、混合 50 %，Input 先 Output 后，宿主没给时间戳。
// 重同步之后的延迟「恰为 D」按 Input 自己的样本坐标算：Input 被跳过的那些周期不在它的流里，
// 所以 Input 在本地第 p 个样本处出现的冲激，回声恰在本地第 p + D 个样本。幅度：跳过之前的第一个冲激（不受更早的回声拖尾
// 影响）逐位 0.25f；之后的冲激前面有别的冲激的回声拖尾（直流阻断的尾巴，量级 1e-7），幅度取 0.25 ± 1e-5。
//
// 五路（前三路各是一次跳过，第四路是更大的一次，第五路是连续几次）：
//   小跳 k = 4        写头领先 1024 < N_cap：回来的第一块 m = 1024（吸收），第二块连续第二次不为 0 → 位移重同步，
//                     恰 1 次、原因位移；重同步后 m 恒为 0，同步偏移恰多 1024；之后的冲激回声恰在 D；
//   临界 k = 32       领先 8192 = N_cap，不大于门限：仍是位移重同步（不是跳变）；
//   大跳 k = 33       领先 8448 > N_cap：回来的第一块立即跳变重同步，恰 1 次、原因跳变；同步偏移恰多 8448；
//   更大 k = 100      领先 25600：同样立即跳变；
//   重复小跳 ×4 + 大跳  相隔 200 个周期的三次小跳 → 三次位移重同步，第三次转容忍模式并置不稳定；第四次小跳不再重同步
//                     （m 停在 1024，回声晚到 D + 1024）；之后一次大跳仍立即跳变重同步，延迟回到 D。
// 每个周期记下状态；输出全程有限、有界。冲激都放在新配对的 500 ms 淡入走完之后，回声的幅度才是满的。
//
// 标 ⛔ 的是删除式验证点：把对应的实现改坏，本场景必须变红。
//   ⛔ 位移判据改成「m 不为 0 一次就重同步」→ 小跳的第一块就重同步；
//   ⛔ 跳变门限改小 → 临界一路变成跳变；
//   ⛔ 容忍模式不再屏蔽位移重同步 → 第四次小跳也重同步。

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

constexpr std::int64_t kDelay = 4800;
constexpr int kBlock = 256;
// 第一个冲激：新配对的 500 ms 淡入（24000 样本）走完之后，在第一次跳过之前
constexpr std::uint64_t kEarly = 30100;
constexpr int kMaxBlock = 1024;
constexpr std::size_t kReasonShift = static_cast<std::size_t>(SyncReason::kShift);
constexpr std::size_t kReasonJump = static_cast<std::size_t>(SyncReason::kJump);

struct Skip
{
    std::size_t first;
    std::size_t count;
};

// 多个冲激：绝对编号 at 处左右都是 amplitude，其余为 0
SourceFn impulsesAt(std::vector<std::uint64_t> at, float amplitude)
{
    return [at = std::move(at), amplitude](std::uint64_t first, float* left, float* right, int n) {
        for (int i = 0; i < n; ++i)
        {
            left[i] = 0.0f;
            right[i] = 0.0f;
        }
        for (const std::uint64_t p : at)
        {
            if (p >= first && p < first + static_cast<std::uint64_t>(n))
            {
                left[p - first] = amplitude;
                right[p - first] = amplitude;
            }
        }
    };
}

struct CycleState
{
    bool ran = false;
    std::uint32_t resyncCount = 0;
    std::uint32_t shift = 0;
    std::uint32_t jump = 0;
    std::uint32_t tolerant = 0;
    std::uint32_t unstable = 0;
    std::int64_t drift = 0;
    std::int64_t offset = 0;
};

struct Trace
{
    std::vector<float> out; // Input 的输出，只含 Input 被调用的周期（Input 本地样本坐标）
    std::vector<CycleState> cycles;
    std::vector<Skip> skips;
    std::uint64_t writeHead = 0;

    // 绝对样本号 → Input 本地样本坐标（被跳过的周期不在 Input 的流里）。冲激必须落在没被跳过的周期里。
    std::int64_t localOf(std::uint64_t absolute) const
    {
        const std::size_t cycle = static_cast<std::size_t>(absolute / kBlock);
        std::size_t skipped = 0;
        for (const Skip& s : skips)
        {
            if (s.first + s.count <= cycle)
            {
                skipped += s.count;
            }
            else if (s.first < cycle)
            {
                skipped += cycle - s.first;
            }
        }
        return static_cast<std::int64_t>(absolute) - static_cast<std::int64_t>(skipped * kBlock);
    }

    // 第一个重同步计数增加的周期；没有时是 npos
    std::size_t firstResync(std::size_t from = 0) const
    {
        for (std::size_t i = std::max<std::size_t>(from, 1); i < cycles.size(); ++i)
        {
            if (cycles[i].resyncCount > cycles[i - 1].resyncCount)
            {
                return i;
            }
        }
        return npos;
    }
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    std::vector<std::size_t> resyncCycles() const
    {
        std::vector<std::size_t> found;
        for (std::size_t i = 1; i < cycles.size(); ++i)
        {
            if (cycles[i].resyncCount > cycles[i - 1].resyncCount)
            {
                found.push_back(i);
            }
        }
        return found;
    }
};

Trace runLeg(const std::vector<Skip>& skips, std::size_t totalCycles, const std::vector<std::uint64_t>& impulses)
{
    Trace trace;
    trace.skips = skips;
    auto host = makeHost(kSr, kMaxBlock);
    REQUIRE(host != nullptr);

    BlockScript input;
    input.lengths = {kBlock};
    input.skips.assign(totalCycles, false);
    for (const Skip& s : skips)
    {
        for (std::size_t i = s.first; i < s.first + s.count && i < totalCycles; ++i)
        {
            input.skips[i] = true;
        }
    }
    BlockScript output;
    output.lengths = {kBlock};
    host->setBlockScripts(input, output);
    host->setCallOrder(CallOrder::kInputFirst);
    host->setSource(impulsesAt(impulses, 0.5f));
    ParamsScript params;
    params.snapshots = {loopParams(50.0f, 50.0f, 100.0f)};
    host->setParamsScript(params);

    for (std::size_t i = 0; i < totalCycles; ++i)
    {
        const int n = host->runCycle(i);
        const rinne::loop::LoopStatusFields& s = host->inputStatus();
        CycleState c;
        c.ran = n > 0;
        c.resyncCount = s.resyncCount;
        c.shift = s.resyncByReason[kReasonShift];
        c.jump = s.resyncByReason[kReasonJump];
        c.tolerant = s.tolerant;
        c.unstable = s.unstable;
        c.drift = s.drift;
        c.offset = s.offset;
        trace.cycles.push_back(c);
        if (n > 0)
        {
            trace.out.insert(trace.out.end(), host->inputOutputLeft().begin(), host->inputOutputLeft().begin() + n);
        }
    }
    trace.writeHead = host->writeHead();
    return trace;
}

// 冲激在 Input 本地第 p 个样本：干声恰在 p，回声恰在 p + delay。exact 为真时回声幅度逐位 0.25f，否则 0.25 ± 1e-5
void expectEcho(Verdict& v, const Trace& t, std::int64_t p, std::int64_t delay, bool exact = false)
{
    const auto at = static_cast<std::size_t>(p);
    const auto echo = static_cast<std::size_t>(p + delay);
    INFO("impulse at local " << p << ", expected echo at " << echo);
    RIG_CHECK(v, echo + 64 < t.out.size());
    if (echo + 64 >= t.out.size())
    {
        return;
    }
    RIG_CHECK(v, std::fabs(t.out[at] - 0.5f) < 1.0e-6f);
    RIG_EQ(v, argmaxAbs(t.out, at + 100, at + static_cast<std::size_t>(delay) + 200), echo);
    if (exact)
    {
        RIG_EQ(v, rinne::test::floatBits(t.out[echo]), rinne::test::floatBits(0.25f));
    }
    else
    {
        RIG_CHECK(v, std::fabs(t.out[echo] - 0.25f) < 1.0e-5f);
    }
}

bool allFinite(const std::vector<float>& x)
{
    return std::all_of(x.begin(), x.end(), [](float f) { return std::isfinite(f); });
}

class InputSkipScenario final : public Scenario
{
public:
    const char* id() const noexcept override { return "s06_input_skip"; }

    bool run(ScenarioContext&) override
    {
        Verdict v;
        smallSkip(v);
        boundary(v);
        largeSkip(v, 33, "k = 33");
        largeSkip(v, 100, "k = 100");
        repeated(v);
        return v.ok();
    }

private:
    // 一段规律的跳过之后的检查：第一个重同步周期 / 原因 / 同步偏移多出多少 / 之后 m 恒为 0
    static void afterSkip(Verdict& v, const Trace& t, std::size_t skipFirst, std::size_t k, std::size_t resyncAt,
                          bool expectJump)
    {
        const std::size_t resume = skipFirst + k;
        RIG_EQ(v, t.firstResync(), resyncAt);
        RIG_EQ(v, t.cycles.back().resyncCount, 1u);
        RIG_EQ(v, t.cycles.back().jump, expectJump ? 1u : 0u);
        RIG_EQ(v, t.cycles.back().shift, expectJump ? 0u : 1u);
        RIG_EQ(v, t.cycles.back().tolerant, 0u);
        RIG_EQ(v, t.cycles.back().unstable, 0u);
        // 跳过之前一切正常：零重同步，m 恒为 0
        for (std::size_t i = 1; i < skipFirst; ++i)
        {
            RIG_EQ(v, t.cycles[i].resyncCount, 0u);
            RIG_EQ(v, t.cycles[i].drift, std::int64_t{0});
        }
        // Input 回来的第一块：写头领先了 k 块（跳变那一路在这一块就重同步了，发布的 m 已是 0）
        if (!expectJump)
        {
            RIG_EQ(v, t.cycles[resume].drift, static_cast<std::int64_t>(k) * kBlock);
        }
        // 重同步之后 m 恒为 0，同步偏移恰多出 k 块
        for (std::size_t i = resyncAt; i < t.cycles.size(); ++i)
        {
            RIG_EQ(v, t.cycles[i].drift, std::int64_t{0});
        }
        RIG_EQ(v, t.cycles[resyncAt].offset - t.cycles[skipFirst - 1].offset, static_cast<std::int64_t>(k) * kBlock);
        RIG_EQ(v, t.writeHead, static_cast<std::uint64_t>(t.cycles.size()) * kBlock);
        RIG_CHECK(v, allFinite(t.out));
        RIG_CHECK(v, peakOf(t.out, 0, t.out.size()) < 1.0f);
    }

    static void smallSkip(Verdict& v)
    {
        INFO("small skip k = 4");
        const std::uint64_t early = kEarly; // 跳过之前
        const std::uint64_t late = (204 + 40) * kBlock + 31; // 重同步之后很久
        const Trace t = runLeg({{200, 4}}, 400, {early, late});
        // 回来的第一块（周期 204）不重同步，第二块（205）才重同步
        RIG_EQ(v, t.cycles[204].resyncCount, 0u);
        RIG_EQ(v, t.cycles[204].drift, std::int64_t{1024});
        afterSkip(v, t, 200, 4, 205, false);
        expectEcho(v, t, t.localOf(early), kDelay, true);
        expectEcho(v, t, t.localOf(late), kDelay);
    }

    static void boundary(Verdict& v)
    {
        INFO("boundary k = 32 (lead == N_cap)");
        const std::uint64_t early = kEarly;
        const std::uint64_t late = (232 + 40) * kBlock + 31;
        const Trace t = runLeg({{200, 32}}, 430, {early, late});
        RIG_EQ(v, t.cycles[232].resyncCount, 0u); // 领先恰为 8192，不大于门限：先吸收
        afterSkip(v, t, 200, 32, 233, false);
        expectEcho(v, t, t.localOf(early), kDelay, true);
        expectEcho(v, t, t.localOf(late), kDelay);
    }

    static void largeSkip(Verdict& v, std::size_t k, const char* name)
    {
        INFO("large skip " << name);
        const std::size_t resume = 200 + k;
        const std::uint64_t early = kEarly;
        const std::uint64_t late = (resume + 40) * kBlock + 31;
        const Trace t = runLeg({{200, k}}, resume + 200, {early, late});
        // 回来的第一块就重同步（跳变）
        afterSkip(v, t, 200, k, resume, true);
        expectEcho(v, t, t.localOf(early), kDelay, true);
        expectEcho(v, t, t.localOf(late), kDelay);
    }

    static void repeated(Verdict& v)
    {
        INFO("repeated small skips, then a large one");
        // 小跳 ×4（周期 200、400、600、800，每次 4 块），大跳 ×1（周期 1000，40 块）
        const std::vector<Skip> skips = {{200, 4}, {400, 4}, {600, 4}, {800, 4}, {1000, 40}};
        const std::uint64_t afterFirst = 300 * kBlock + 11; // 第一次位移重同步之后
        const std::uint64_t afterSecond = 500 * kBlock + 11;
        const std::uint64_t afterThird = 700 * kBlock + 11; // 第三次：转容忍模式
        const std::uint64_t afterFourth = 900 * kBlock + 11; // 第四次小跳：容忍模式不再重同步
        const std::uint64_t afterJump = 1100 * kBlock + 11; // 大跳之后
        const Trace t = runLeg(skips, 1300, {kEarly, afterFirst, afterSecond, afterThird, afterFourth, afterJump});

        // 重同步恰在：三次小跳各自回来的第二块、大跳回来的第一块（跳变在容忍模式下照旧）
        const std::vector<std::size_t> want = {205, 405, 605, 1040};
        RIG_CHECK(v, t.resyncCycles() == want);
        RIG_EQ(v, t.cycles.back().shift, 3u);
        RIG_EQ(v, t.cycles.back().jump, 1u);
        RIG_EQ(v, t.cycles.back().resyncCount, 4u);

        // 第三次重同步时转容忍模式并置不稳定；此后不清
        RIG_EQ(v, t.cycles[604].tolerant, 0u);
        RIG_EQ(v, t.cycles[604].unstable, 0u);
        for (std::size_t i = 605; i < t.cycles.size(); ++i)
        {
            RIG_EQ(v, t.cycles[i].tolerant, 1u);
            RIG_EQ(v, t.cycles[i].unstable, 1u);
        }

        // 第四次小跳：容忍模式不重同步，m 停在 1024，直到大跳
        for (std::size_t i = 804; i < 1000; ++i)
        {
            RIG_EQ(v, t.cycles[i].resyncCount, 3u);
            RIG_EQ(v, t.cycles[i].drift, std::int64_t{1024});
        }
        // 大跳回来的第一块（周期 1040）跳变重同步，之后 m 恒为 0
        for (std::size_t i = 1040; i < t.cycles.size(); ++i)
        {
            RIG_EQ(v, t.cycles[i].drift, std::int64_t{0});
        }

        // 回声：每次重同步之后恰在 D；第四次小跳之后没有重同步，延迟被拉长了 1024（写头领先的量）
        expectEcho(v, t, t.localOf(kEarly), kDelay, true);
        expectEcho(v, t, t.localOf(afterFirst), kDelay);
        expectEcho(v, t, t.localOf(afterSecond), kDelay);
        expectEcho(v, t, t.localOf(afterThird), kDelay);
        expectEcho(v, t, t.localOf(afterFourth), kDelay + 1024);
        expectEcho(v, t, t.localOf(afterJump), kDelay);
        RIG_CHECK(v, allFinite(t.out));
        RIG_CHECK(v, peakOf(t.out, 0, t.out.size()) < 1.0f);
    }
};

} // namespace

RINNE_RIG_SCENARIO("s06_input_skip")
{
    return new InputSkipScenario();
}

TEST_CASE("s06: input skipping k blocks resyncs once after two checks, or at once past the cap", "[loop_rig][s06]")
{
    CHECK(rinne::rig::scn::runRegisteredScenario("s06_input_skip"));
}
