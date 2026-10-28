// SPDX-License-Identifier: GPL-3.0-or-later
//
// S22 载入预设：运行中先 10 ms 线性降到 0，再从首个有效读样本起 500 ms 线性淡入，淡入原因是 Preset；
// 锁存（跳闸）时载入预设不解除锁存、回送保持 0，Recover 之后照常淡入。
//
// 载入预设在引擎里就是「预设序号变了」加上 8 个声音参数的新值。参数的新值照常进引擎（这里随序号一起给），
// 序号带来的是安全链的淡入请求。环路延迟 100 ms、混合 50 %、反馈 50 %、中间链是恒等；跑够 2 s 到 Run 之后：
//
// 一、运行中载入预设（两种）：
//   甲 声音参数换成反馈 70 %、混合 30 %，延迟不变（读头不动）；
//   乙 声音参数换成反馈 65 %、混合 50 %、环路延迟 250 ms（预设里常见的延迟改动）。
//   每一种都断言：
//   - 引擎向安全链请求的淡入原因恰好一次，是 Preset；淡入期间打包状态里见到的原因只有 Preset
//     （没有 FirstPair、NewWriter：预设不触发重新配对，也不换写方，组号因此不变）；
//   - 每个块末的安全增益与分段线性的期望值相差不超过 1e-5：载入点起 480 个样本（10 ms）线性降到 0，
//     再从首个有效读样本起 24000 个样本（500 ms）线性升到 1；
//   - 淡完之后回到 Run，增益恰为 1；没有跳闸；除了乙里改 D 的交叉淡化（引擎记作一次「改 D」重同步）之外没有重同步；
//     回送在淡入之后照常在响；
//   - 输出与参照安全链逐位相同（参照用的是预设之后的参数）。
//
// 二、锁存时载入预设：先紧急切断（跳闸、锁存），冷却走完，再换一组新的声音参数并让预设序号加一：
//   - 状态仍是 Tripped，Input 槽里仍是「跳闸 + 紧急切断」，跳闸计数不增，安全增益保持 0；
//   - 载入点之后回送全是 0，输出与干声逐位相同，打包状态里没有出现 FadeIn；
//   - 之后恢复放行：淡入原因是 Recover，增益从首个有效读样本起 500 ms 线性升到 1，误差同上；之后回送在响，
//     输出与参照安全链逐位相同。
//
// 三、读头还没有有效数据时载入预设：环路刚建立（两个块之后），回送增益本来就是 0，没有下行可做；
//   500 ms 淡入不从载入点起，而是等到首个有效读样本（环路延迟 100 ms 之后，距载入点 4800 个样本减去已经过去的
//   一两个块）才开始；原因是 Preset，增益轨迹同上。
//
// 「载入预设前后 loop_on、limiter、ceiling 与组号不变」不在这里断言：这几项不属于预设，载入只经参数树写 8 个声音参数，
// 由 tests/params/test_presets_load.cpp 核（三个参数没有任何手势或值变化，载入前后原样）；组号不在参数树里，
// 预设不碰；本场景从引擎一侧核的是预设不触发重新配对（见上，淡入原因只有 Preset）。
// 淡入包络本身（含「入口处硬切到 0 会超界」的反例）由 S25 的入口复验覆盖，这里不重复。
//
// 标 ⛔ 的是删除式验证点：把对应的实现改坏，本场景必须变红。
//   ⛔ 载入预设不请求淡入（直接落定）→ 增益轨迹是平的，原因对不上；
//   ⛔ 锁存时预设请求不再被挡住（跳闸状态下也开始淡入）→ 状态离开 Tripped，回送回来；
//   ⛔ 淡入不等首个有效读样本（一进淡入就上行）→ 第三格的增益轨迹与期望不符（增益提前 4000 多个样本开始上行）；
//   ⛔ 预设请求报成 SetState（原因串线）→ 原因对不上；
//   ⛔ 下行时长不是 10 ms，或上行时长不是 500 ms → 增益轨迹与分段线性的期望不符。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <set>
#include <vector>

#include "engine/SafetyEvents.h"
#include "ipc/SegmentLayout.h"
#include "loop/StreamSync.h"
#include "s01_rig_kit.h"

namespace
{

using namespace rinne::rig;
using namespace rinne::rig::scn;
using rinne::engine::FadeReason;
using rinne::engine::PackedSafety;
using rinne::engine::SafetyState;

constexpr int kBlock = 256;
constexpr std::int64_t kSecond = 48000;
constexpr double kGainTolerance = 1.0e-5;

struct FadeLog
{
    std::vector<FadeReason> reasons;
    static void record(void* context, FadeReason reason) noexcept
    {
        static_cast<FadeLog*>(context)->reasons.push_back(reason);
    }
};

// 一条正在响的环路，逐个周期记下安全增益与淡入原因
class Track
{
public:
    Track()
    {
        host = makeHost(kSr, kBlock);
        REQUIRE(host != nullptr);
        host->setBlockScripts(fixedBlocks(kBlock), fixedBlocks(kBlock));
        host->input().setFadeObserver(&log, &FadeLog::record);
        rec = std::make_unique<Recorder>(*host, tones());
        params = loopParams(50.0f, 50.0f, 100.0f);
    }

    // 周期没处理样本时 Recorder::cycle 已经报了失败，这里立刻停下，不空转到超时
    void run(std::int64_t samples)
    {
        for (std::int64_t done = 0; done < samples;)
        {
            const int n = step();
            if (n <= 0)
            {
                return;
            }
            done += n;
        }
    }

    int step()
    {
        const int n = rec->cycle(cycles_++, params);
        gains.push_back(host->input().safetyChain().machine().safetyGain());
        ends.push_back(rec->outLeft.size());
        const std::uint32_t packed = host->input().meters().safety.load();
        if (PackedSafety::state(packed) == SafetyState::FadeIn)
        {
            fadeReasons.insert(PackedSafety::fade(packed));
        }
        return n;
    }

    // 把这一组声音参数当成「预设的内容」换进去，预设序号加一
    void loadPreset(float feedbackPercent, float mixPercent, float delayMs)
    {
        params.feedbackPercent = feedbackPercent;
        params.mixPercent = mixPercent;
        params.delayMs = delayMs;
        ++params.presetSeq;
    }

    void clearLogs()
    {
        fadeReasons.clear();
        log.reasons.clear();
    }

    std::size_t now() const { return rec->outLeft.size(); }
    SafetyState state() const { return static_cast<SafetyState>(host->inputSafetyDiag().state); }
    std::uint32_t slotSafety() const { return host->slot(rinne::ipc::Role::kInput)->safety_state.load(); }

    std::unique_ptr<EngineHost> host;
    std::unique_ptr<Recorder> rec;
    rinne::engine::EngineParams params;
    FadeLog log;
    std::vector<float> gains; // 每个块末的 g_safety
    std::vector<std::size_t> ends; // 每个块末已处理的样本数
    std::set<FadeReason> fadeReasons; // 淡入期间见过的原因（打包状态里的）

private:
    std::size_t cycles_ = 0;
};

class LoadPresetScenario final : public Scenario
{
public:
    const char* id() const noexcept override { return "s22_load_preset"; }

    bool run(ScenarioContext&) override
    {
        Verdict v;
        loadInRun(v, "same delay", 70.0f, 30.0f, 100.0f, 2 * kSecond);
        loadInRun(v, "longer delay", 65.0f, 50.0f, 250.0f, 2 * kSecond);
        loadInRun(v, "delay longer than the history", 65.0f, 50.0f, 1000.0f, 7 * kSecond / 10);
        loadBeforeFirstRead(v);
        loadWhileTripped(v);
        return v.ok();
    }

private:
    // 入口 s（样本编号，载入生效的块首）之后的增益轨迹。fromRun：入口之前回送在响，要先 10 ms 降到 0。
    // 返回下行结束到首个有效读样本之间隔了多少个样本（淡入要等的时间）
    static std::size_t gainShape(Verdict& v, const Track& track, std::size_t s, bool fromRun)
    {
        const auto& valid = track.rec->reference.tapValid;
        const std::size_t downEnd = fromRun ? s + static_cast<std::size_t>(kGateSamples) : s;
        std::size_t firstValid = downEnd;
        while (firstValid < valid.size() && valid[firstValid] == 0)
        {
            ++firstValid;
        }
        RIG_CHECK(v, firstValid < valid.size());
        double worst = 0.0;
        std::size_t compared = 0;
        for (std::size_t k = 0; k < track.gains.size(); ++k)
        {
            const std::size_t end = track.ends[k];
            if (end <= s)
            {
                continue;
            }
            double want = 0.0;
            if (fromRun && end <= downEnd)
            {
                want = 1.0 - static_cast<double>(end - s) / static_cast<double>(kGateSamples);
            }
            else if (end > firstValid)
            {
                want = std::min(1.0, static_cast<double>(end - firstValid) / static_cast<double>(kFadeInSamples));
            }
            worst = std::max(worst, std::fabs(static_cast<double>(track.gains[k]) - want));
            ++compared;
        }
        INFO("worst gain deviation = " << worst << " over " << compared << " blocks");
        RIG_CHECK(v, compared > 100u);
        RIG_CHECK(v, worst <= kGainTolerance);
        RIG_EQ(v, static_cast<int>(track.state()), static_cast<int>(SafetyState::Run));
        RIG_EQ(v, track.gains.back(), 1.0f);
        return firstValid - downEnd;
    }

    // ---- 一、运行中载入预设 ----
    static void loadInRun(Verdict& v, const char* name, float feedback, float mix, float delayMs,
                          std::int64_t runBefore)
    {
        INFO("load in run: " << name);
        Track track;
        track.run(runBefore);
        RIG_EQ(v, static_cast<int>(track.state()), static_cast<int>(SafetyState::Run));
        RIG_EQ(v, track.gains.back(), 1.0f);
        track.clearLogs();
        const std::size_t s = track.now();
        track.loadPreset(feedback, mix, delayMs);
        track.run(3 * kSecond);

        // 淡入原因：恰好一次 Preset，打包状态里只见到 Preset（不是重新配对，也不是换写方）
        RIG_CHECK(v, track.log.reasons == std::vector<FadeReason>{FadeReason::Preset});
        RIG_CHECK(v, track.fadeReasons == std::set<FadeReason>{FadeReason::Preset});
        RIG_EQ(v, gainShape(v, track, s, true), std::size_t{0}); // 运行中读头是有效的，上行紧接着下行

        // 没有跳闸；除了改 D 的 40 ms 交叉淡化（引擎把它记作一次「改 D」重同步）之外没有任何重同步；
        // 回送在淡入之后照常在响
        RIG_EQ(v, track.host->inputSafetyDiag().trips, 0u);
        const std::uint32_t expectDChange = delayMs != 100.0f ? 1u : 0u;
        RIG_EQ(v, track.host->inputStatus().resyncCount, expectDChange);
        RIG_EQ(v, track.host->inputStatus().resyncByReason[static_cast<std::size_t>(rinne::loop::SyncReason::kDChange)],
               expectDChange);
        RIG_EQ(v, track.host->inputStatus().delayEffective,
               static_cast<std::int64_t>(std::lround(static_cast<double>(delayMs) * kSr / 1000.0)));
        RIG_CHECK(v, !sameBits(track.rec->outLeft, track.rec->dryLeft, track.now() - kSecond, track.now()));
        RIG_CHECK(v, track.rec->matchesReference());
    }

    // 环路刚建立、读头还没有有效数据时载入预设：增益本来就是 0，没有下行；500 ms 淡入从首个有效读样本（D = 100 ms
    // 之后）才开始，不是从载入点开始
    static void loadBeforeFirstRead(Verdict& v)
    {
        INFO("load before the first valid read");
        constexpr std::size_t kD = 4800; // 100 ms
        Track track;
        track.run(2 * kBlock);
        RIG_EQ(v, track.gains.back(), 0.0f);
        track.clearLogs();
        const std::size_t s = track.now();
        track.loadPreset(70.0f, 30.0f, 100.0f);
        track.run(3 * kSecond);
        RIG_CHECK(v, track.log.reasons == std::vector<FadeReason>{FadeReason::Preset});
        RIG_CHECK(v, track.fadeReasons == std::set<FadeReason>{FadeReason::Preset});
        const std::size_t wait = gainShape(v, track, s, false);
        INFO("fade-in waited " << wait << " samples for the first valid read");
        RIG_CHECK(v, wait > kD - 4 * kBlock && wait <= kD);
        RIG_EQ(v, track.host->inputSafetyDiag().trips, 0u);
        RIG_CHECK(v, track.rec->matchesReference());
    }

    // ---- 二、锁存时载入预设 ----
    static void loadWhileTripped(Verdict& v)
    {
        INFO("load while tripped");
        Track track;
        track.run(2 * kSecond);
        track.params.panicSeq = 1;
        const std::size_t panicAt = track.now();
        track.run(2 * kSecond); // 跳闸，冷却（1.5 s）走完
        RIG_EQ(v, static_cast<int>(track.state()), static_cast<int>(SafetyState::Tripped));
        RIG_EQ(v, track.slotSafety(), rinne::ipc::kSafetyTripped | (rinne::ipc::kTripPanic << rinne::ipc::kTripShift));
        RIG_EQ(v, track.host->inputSafetyDiag().trips, 1u);

        // 锁存时载入预设（反馈 70 %、混合 30 %、延迟 250 ms）
        track.clearLogs();
        const std::size_t s = track.now();
        track.loadPreset(70.0f, 30.0f, 250.0f);
        track.run(kSecond);
        RIG_EQ(v, static_cast<int>(track.state()), static_cast<int>(SafetyState::Tripped));
        RIG_EQ(v, track.slotSafety(), rinne::ipc::kSafetyTripped | (rinne::ipc::kTripPanic << rinne::ipc::kTripShift));
        RIG_EQ(v, track.host->inputSafetyDiag().trips, 1u);
        RIG_CHECK(v, track.fadeReasons.empty());
        float loudestGain = 0.0f;
        for (std::size_t k = 0; k < track.gains.size(); ++k)
        {
            loudestGain = track.ends[k] > s ? std::max(loudestGain, track.gains[k]) : loudestGain;
        }
        RIG_EQ(v, loudestGain, 0.0f);
        // 载入点之后回送全是 0，输出就是干声（切断淡出走完之后的部分整段都是）
        RIG_CHECK(v, allZero(track.rec->reference.retLeft, s, track.now()));
        RIG_CHECK(v, allZero(track.rec->reference.retRight, s, track.now()));
        RIG_CHECK(v, sameBits(track.rec->outLeft, track.rec->dryLeft, panicAt + kBlock + kGateSamples, track.now()));
        RIG_CHECK(v, sameBits(track.rec->outRight, track.rec->dryRight, panicAt + kBlock + kGateSamples, track.now()));

        // 恢复：原因是 Recover，从首个有效读样本起 500 ms 线性淡入
        track.clearLogs();
        const std::size_t r = track.now();
        track.params.recoverSeq = 1;
        track.run(3 * kSecond);
        gainShape(v, track, r, false);
        RIG_CHECK(v, track.fadeReasons == std::set<FadeReason>{FadeReason::Recover});
        RIG_EQ(v, track.host->inputSafetyDiag().trips, 1u);
        RIG_CHECK(v, !sameBits(track.rec->outLeft, track.rec->dryLeft, track.now() - kSecond, track.now()));
        RIG_CHECK(v, track.rec->matchesReference());
    }
};

} // namespace

RINNE_RIG_SCENARIO("s22_load_preset")
{
    return new LoadPresetScenario();
}

TEST_CASE("s22: loading a preset fades down then up with the Preset reason and never clears a latch", "[loop_rig][s22]")
{
    CHECK(rinne::rig::scn::runRegisteredScenario("s22_load_preset"));
}
