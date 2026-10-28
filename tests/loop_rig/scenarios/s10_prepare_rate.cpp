// SPDX-License-Identifier: GPL-3.0-or-later
//
// S10 重新 prepare：同采样率重新 prepare 不打断流；换采样率重新来过，毫秒值保持，旧采样率下的内容不会漏进新的流。
//
// 同采样率（两种做法：整台试验台重新 prepare，或只让 Output 一端重新 prepare）：环路跑到一半，在周期边界上重新
// prepare， 同样的采样率与最大块长。引擎文档的约定是同采样率重新 prepare 不动读头、延迟线与安全链，流保持连续：
//   - 零重同步，Input 的安全状态一直是 Run（没有重新淡入）；
//   - 整段输出与从不重新 prepare 的对照运行逐位相同（左右声道），也与参照安全链逐位相同。
//
// 换采样率（48 kHz → 96 kHz，整台试验台重新 prepare）：延迟线按新采样率重新分配，域重来，安全链重新淡入。
//   - 用户设的 100 ms 不变，实际环路延迟从 4800 变成 9600 个样本；
//   - 换采样率之前在途的冲激（它在 48 kHz 的回声要在换采样率之后才到）不会出现：换采样率之后到首个有效读样本之前，
//     输出逐位是 0；
//   - 淡入走完之后的新冲激，回声恰在 9600 个样本之后、幅度逐位 0.25f，输出与新采样率下的参照安全链逐位相同。
//
// 采样率不一致（只让 Output 一端换采样率，试验台的 prepareOutput 同时按新采样率重新发布写方）：
// 环头里写方的采样率与 Input 的不同，环里的帧按另一个速率解释，读了就是变调的声音。环路层把它当作没有写方：
//   - 切换那一周期起 Input 的配对位为 0、不一致位为 1、对端采样率显示环头里的值，安全状态是「未配对」；
//   - 回送在 1 块 + 10 ms 内归零，之后输出与干声逐位相同（回送一位不碰），读头里逐样本无效，淡出无阶跃；
//   - Output 改回同一采样率：新配对，读头从当时的写头起读，D 个样本之内回送是 0（不读不一致期间留在环里的帧），
//     首个有效读样本起 500 ms 线性淡入，回送包络不超过淡入的斜坡；最后恢复为 Run；
//   - 全程输出与参照安全链逐位相同。
//
// 暂缺（不是实现被改动）：
//   - Input 一端单独换采样率造成的不一致：参照安全链按一个采样率构造，试验台的记录器不跟着换，这条路径
//     只在 tests/core/timing/test_rate_mismatch.cpp 里验（环路层直接 prepare 到另一个采样率）。
//
// 标 ⛔ 的是删除式验证点：把对应的实现改坏，本场景必须变红。
//   ⛔ 环路层不比较环头里的采样率（配对位只看写权）→ 不一致期间回送照读，输出不再等于干声；
//   ⛔ 不一致时回送直接截成 0 而不淡出 → 淡出的阶跃判据变红；
//   ⛔ 不一致期间仍算有对端（恢复一致不按新配对）→ 恢复时记一次格式变化重同步，新配对的断言变红；
//   ⛔ 同采样率重新 prepare 时也重建安全链 → 重新淡入，输出与对照运行不再逐位相同；
//   ⛔ 同采样率重新 prepare 时也按「换采样率」重来域 → 流被打断，输出与对照运行不再逐位相同；
//   ⛔ 换采样率时不重新分配延迟线 → D 仍是 4800，回声不在 9600。

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "engine/SafetyEvents.h"
#include "loop/StreamSync.h"
#include "s01_rig_kit.h"

namespace
{

using namespace rinne::rig;
using namespace rinne::rig::scn;
using rinne::engine::SafetyState;

constexpr int kBlock = 256;
constexpr std::size_t kSameRateCycles = 563; // 约 3 s
constexpr std::size_t kReprepareAt = 300;

enum class Reprepare
{
    kNone, // 对照：从不重新 prepare
    kWholeRig, // 整台试验台（Input、Output、中间链）同采样率重新 prepare
    kOutputOnly, // 只让 Output 同采样率重新 prepare
};

struct SameRateRun
{
    std::vector<float> outLeft;
    std::vector<float> outRight;
    std::vector<float> dryLeft;
    bool matchesReference = false;
    std::uint32_t resyncs = 0;
    std::uint8_t state = 0;
};

SameRateRun runSameRate(Reprepare mode)
{
    SameRateRun run;
    auto host = makeHost(kSr, kBlock);
    REQUIRE(host != nullptr);
    host->setBlockScripts(fixedBlocks(kBlock), fixedBlocks(kBlock));
    Recorder rec(*host, [](std::uint64_t first, float* left, float* right, int n) {
        for (int i = 0; i < n; ++i)
        {
            left[i] = 0.2f * hashNoise(first + static_cast<std::uint64_t>(i), 21);
            right[i] = 0.2f * hashNoise(first + static_cast<std::uint64_t>(i), 22);
        }
    });
    const rinne::engine::EngineParams p = loopParams(50.0f, 50.0f, 100.0f);
    for (std::size_t i = 0; i < kSameRateCycles; ++i)
    {
        if (i == kReprepareAt && mode == Reprepare::kWholeRig)
        {
            host->prepare(kSr, kBlock);
        }
        else if (i == kReprepareAt && mode == Reprepare::kOutputOnly)
        {
            host->output().prepare(kSr, kBlock);
        }
        rec.cycle(i, p);
    }
    run.outLeft = rec.outLeft;
    run.outRight = rec.outRight;
    run.dryLeft = rec.dryLeft;
    run.matchesReference = rec.matchesReference();
    run.resyncs = host->inputStatus().resyncCount;
    run.state = host->inputSafetyDiag().state;
    return run;
}

// 几个绝对位置上的冲激（左右都是）
SourceFn impulsesAt(std::initializer_list<std::uint64_t> positions, float amplitude)
{
    const std::vector<std::uint64_t> at(positions);
    return [at, amplitude](std::uint64_t first, float* left, float* right, int n) {
        for (int i = 0; i < n; ++i)
        {
            float x = 0.0f;
            for (const std::uint64_t position : at)
            {
                x = first + static_cast<std::uint64_t>(i) == position ? amplitude : x;
            }
            left[i] = x;
            right[i] = x;
        }
    };
}

class PrepareRateScenario final : public Scenario
{
public:
    const char* id() const noexcept override { return "s10_prepare_rate"; }

    bool run(ScenarioContext&) override
    {
        Verdict v;
        sameRate(v);
        rateChange(v);
        rateMismatch(v);
        return v.ok();
    }

private:
    static void sameRate(Verdict& v)
    {
        const SameRateRun control = runSameRate(Reprepare::kNone);
        RIG_CHECK(v, control.matchesReference);
        RIG_EQ(v, control.resyncs, 0u);
        RIG_EQ(v, control.state, static_cast<std::uint8_t>(SafetyState::Run));
        // 对照里回送确实在响（输出不等于干声），否则逐位相同没有意义
        std::size_t audible = 0;
        for (std::size_t i = 0; i < control.outLeft.size(); ++i)
        {
            audible +=
                rinne::test::floatBits(control.outLeft[i]) != rinne::test::floatBits(control.dryLeft[i]) ? 1u : 0u;
        }
        RIG_CHECK(v, audible > 50000u);

        for (const Reprepare mode : {Reprepare::kWholeRig, Reprepare::kOutputOnly})
        {
            const char* label = mode == Reprepare::kWholeRig ? "whole rig re-prepared" : "output only re-prepared";
            INFO(label);
            const SameRateRun run = runSameRate(mode);
            RIG_CHECK(v, run.matchesReference);
            RIG_EQ(v, run.resyncs, 0u);
            RIG_EQ(v, run.state, static_cast<std::uint8_t>(SafetyState::Run));
            RIG_CHECK(v, sameBits(run.outLeft, control.outLeft, 0, control.outLeft.size()));
            RIG_CHECK(v, sameBits(run.outRight, control.outRight, 0, control.outRight.size()));
            RIG_EQ(v, run.outLeft.size(), control.outLeft.size());
        }
    }

    // 反馈 50 %、混合 50 %、D = 100 ms 的正在响的环路。Output 在 cycle 300 改到 96 kHz 并重新发布，
    // 在 cycle 488 改回 48 kHz（两段都比 250 ms 长，不一致时的停用清空也走到）
    static void rateMismatch(Verdict& v)
    {
        constexpr std::size_t kMismatchAt = 300;
        constexpr std::size_t kRecoverAt = 488;
        constexpr std::size_t kEnd = 488 + 563;
        constexpr std::size_t kSettle = kBlock + kGateSamples; // 1 块 + 10 ms
        constexpr std::size_t kDelay = 4800;

        auto host = makeHost(kSr, kBlock);
        REQUIRE(host != nullptr);
        host->setBlockScripts(fixedBlocks(kBlock), fixedBlocks(kBlock));
        Recorder rec(*host, tones());
        const rinne::engine::EngineParams p = loopParams(50.0f, 50.0f, 100.0f);
        const auto state = [&] { return static_cast<SafetyState>(host->inputSafetyDiag().state); };

        for (std::size_t i = 0; i < kMismatchAt; ++i)
        {
            rec.cycle(i, p);
        }
        const std::size_t mismatchAt = rec.outLeft.size();
        RIG_EQ(v, static_cast<int>(state()), static_cast<int>(SafetyState::Run));
        RIG_EQ(v, host->inputStatus().paired, 1u);
        RIG_EQ(v, host->inputStatus().rateMismatch, 0u);
        RIG_EQ(v, host->inputStatus().peerSampleRate, 48000u);
        RIG_CHECK(v, !sameBits(rec.outLeft, rec.dryLeft, mismatchAt - 24000, mismatchAt)); // 回送在响

        // Output 改到 96 kHz 并重新发布
        host->prepareOutput(96000.0, kBlock);
        for (std::size_t i = kMismatchAt; i < kRecoverAt; ++i)
        {
            rec.cycle(i, p);
        }
        const std::size_t recoverAt = rec.outLeft.size();
        RIG_EQ(v, host->inputStatus().paired, 0u);
        RIG_EQ(v, host->inputStatus().rateMismatch, 1u);
        RIG_EQ(v, host->inputStatus().peerSampleRate, 96000u);
        RIG_EQ(v, host->inputStatus().tapsActive, 0u);
        RIG_EQ(v, static_cast<int>(state()), static_cast<int>(SafetyState::Unpaired));
        // 1 块 + 10 ms 内归零，之后输出就是干声，读头里逐样本无效
        RIG_CHECK(v, sameBits(rec.outLeft, rec.dryLeft, mismatchAt + kSettle, recoverAt));
        RIG_CHECK(v, sameBits(rec.outRight, rec.dryRight, mismatchAt + kSettle, recoverAt));
        RIG_CHECK(v, allZero(rec.reference.tapLeft, mismatchAt + kSettle, recoverAt));
        RIG_CHECK(v, allZero(rec.reference.tapRight, mismatchAt + kSettle, recoverAt));
        for (std::size_t t = mismatchAt + kSettle; t < recoverAt; ++t)
        {
            if (rec.reference.tapValid[t] != 0u)
            {
                RIG_CHECK(v, rec.reference.tapValid[t] == 0u);
                break;
            }
        }
        // 淡出无阶跃（读头 10 ms 淡出与安全链的 10 ms 降增益叠在一起，斜率最多两倍）
        {
            const auto& ret = rec.reference.retLeft;
            const std::size_t from = mismatchAt - 4800;
            const std::size_t to = mismatchAt + kSettle + 480;
            const float level = peakOf(ret, from, mismatchAt);
            RIG_CHECK(v, level > 0.05f);
            const float bound = slope(100.0, level) + 2.0f * level / static_cast<float>(kGateSamples) + 1.0e-6f;
            RIG_CHECK(v, rinne::test::maxStep(ret, from, to) <= bound);
            // 反例：切换处硬切到 0 会超过这个界
            std::size_t loudest = mismatchAt;
            for (std::size_t t = mismatchAt; t < mismatchAt + kGateSamples; ++t)
            {
                loudest = std::fabs(ret[t]) > std::fabs(ret[loudest]) ? t : loudest;
            }
            RIG_CHECK(v, rinne::test::maxStep(hardCut(ret, loudest + 1), from, to) > bound);
        }

        // Output 改回 48 kHz
        host->prepareOutput(48000.0, kBlock);
        for (std::size_t i = kRecoverAt; i < kEnd; ++i)
        {
            rec.cycle(i, p);
        }
        const std::size_t end = rec.outLeft.size();
        RIG_EQ(v, host->inputStatus().paired, 1u);
        RIG_EQ(v, host->inputStatus().rateMismatch, 0u);
        RIG_EQ(v, host->inputStatus().peerSampleRate, 48000u);
        RIG_EQ(v, static_cast<int>(state()), static_cast<int>(SafetyState::Run));

        // 新配对：读头从恢复时的写头起读，D 个样本之内回送是 0，首个有效读样本恰在 D 之后
        const auto& valid = rec.reference.tapValid;
        std::size_t firstValid = recoverAt;
        while (firstValid < valid.size() && valid[firstValid] == 0)
        {
            ++firstValid;
        }
        RIG_CHECK(v, firstValid < valid.size());
        RIG_CHECK(v, firstValid >= recoverAt + kDelay - 1 && firstValid <= recoverAt + kDelay + kBlock);
        RIG_CHECK(v, sameBits(rec.outLeft, rec.dryLeft, recoverAt, firstValid));
        RIG_CHECK(v, sameBits(rec.outRight, rec.dryRight, recoverAt, firstValid));

        // 500 ms 淡入：回送链里有 DC 阻断（相位超前），逐样本比较不成立，按 10 ms 一窗比包络：
        // 窗内回送峰值不超过「窗末的斜坡 × 读头在这一窗与前一窗里的峰值」（反馈 50 %，所以这个界有一倍的余量）
        {
            const auto& tap = rec.reference.tapLeft;
            const auto& ret = rec.reference.retLeft;
            constexpr std::size_t kWin = static_cast<std::size_t>(kGateSamples);
            constexpr std::size_t kFade = static_cast<std::size_t>(kFadeInSamples);
            bool within = true;
            double worstRatio = 0.0;
            double lastRatio = 0.0;
            std::size_t windows = 0;
            for (std::size_t j = firstValid; j + kWin <= firstValid + kFade; j += kWin)
            {
                const double ramp = static_cast<double>(j + kWin - firstValid + 2) / static_cast<double>(kFade);
                const double tapPeak = peakOf(tap, j - kWin, j + kWin);
                const double retPeak = peakOf(ret, j, j + kWin);
                within = within && retPeak <= ramp * tapPeak + 1.0e-4;
                worstRatio = std::max(worstRatio, tapPeak > 0.0 ? retPeak / (ramp * tapPeak) : 0.0);
                lastRatio = tapPeak > 0.0 ? retPeak / tapPeak : 0.0;
                ++windows;
            }
            INFO("worst ret / ramp bound = " << worstRatio << ", last ret / tap = " << lastRatio);
            RIG_EQ(v, windows, kFade / kWin);
            RIG_CHECK(v, within);
            // 分辨力：淡入尾端回送确实上来了（不然「不超过斜坡」是平凡的）；若不淡入，开头几窗就会超出斜坡
            RIG_CHECK(v, lastRatio > 0.25);
        }
        // 淡入走完之后回送回来了
        RIG_CHECK(v, !sameBits(rec.outLeft, rec.dryLeft, end - 24000, end));
        // 恢复一致是新配对，不是格式变化重同步（不一致期间不算有对端）
        RIG_EQ(v, host->inputStatus().resyncByReason[static_cast<std::size_t>(rinne::loop::SyncReason::kFormat)], 0u);
        RIG_CHECK(v, rec.matchesReference());
    }

    static void rateChange(Verdict& v)
    {
        constexpr std::uint64_t kFirstImpulse = 30100;
        constexpr std::uint64_t kSwitchAt = 150 * kBlock; // 周期边界
        constexpr std::uint64_t kInFlight = kSwitchAt - 1000; // 48 kHz 下它的回声要在 kSwitchAt + 3800 到
        constexpr std::uint64_t kSecondImpulse = kSwitchAt + 70000; // 96 kHz 下淡入（500 ms = 48000 样本）走完之后
        constexpr std::int64_t kDelay96 = 9600;

        auto host = makeHost(kSr, kBlock);
        REQUIRE(host != nullptr);
        host->setBlockScripts(fixedBlocks(kBlock), fixedBlocks(kBlock));
        const SourceFn source = impulsesAt({kFirstImpulse, kInFlight, kSecondImpulse}, 0.5f);
        const rinne::engine::EngineParams p = loopParams(50.0f, 50.0f, 100.0f);

        // 48 kHz：第一个冲激的回声恰在 D = 4800
        {
            Recorder rec(*host, source, kSr);
            for (std::size_t i = 0; i < kSwitchAt / kBlock; ++i)
            {
                rec.cycle(i, p);
            }
            RIG_CHECK(v, rec.matchesReference());
            RIG_EQ(v, host->inputStatus().delayEffective, std::int64_t{4800});
            RIG_EQ(v, host->inputStatus().delayUserMs, 100.0f);
            RIG_EQ(v, argmaxAbs(rec.outLeft, kFirstImpulse + 1, kInFlight), kFirstImpulse + 4800);
        }

        // 换采样率：整台试验台以 96 kHz 重新 prepare
        host->prepare(96000.0, kBlock);
        Recorder rec(*host, source, 96000.0);
        const std::size_t cycles = (kSecondImpulse + kDelay96 + 2000 - kSwitchAt) / kBlock + 1;
        for (std::size_t i = 0; i < cycles; ++i)
        {
            rec.cycle(i, p);
        }
        const auto& status = host->inputStatus();
        RIG_EQ(v, status.delayUserMs, 100.0f); // 毫秒值保持
        RIG_EQ(v, status.delayEffective, kDelay96); // 样本数随采样率翻倍
        RIG_EQ(v, status.drift, std::int64_t{0});
        RIG_EQ(v, host->inputSafetyDiag().state, static_cast<std::uint8_t>(SafetyState::Run));
        RIG_CHECK(v, rec.matchesReference());

        // 在途的冲激不漏进新的流：到第二个冲激之前输出逐位是 0
        const auto second = static_cast<std::size_t>(kSecondImpulse - kSwitchAt);
        RIG_CHECK(v, allZero(rec.outLeft, 0, second));
        RIG_CHECK(v, allZero(rec.outRight, 0, second));
        // 新采样率下的回声恰在 9600 个样本之后，幅度逐位 0.25f
        const std::size_t echo = second + static_cast<std::size_t>(kDelay96);
        RIG_EQ(v, argmaxAbs(rec.outLeft, second + 1, rec.outLeft.size()), echo);
        RIG_EQ(v, rinne::test::floatBits(rec.outLeft[echo]), rinne::test::floatBits(0.25f));
        RIG_EQ(v, rinne::test::floatBits(rec.outRight[echo]), rinne::test::floatBits(0.25f));
    }
};

} // namespace

RINNE_RIG_SCENARIO("s10_prepare_rate")
{
    return new PrepareRateScenario();
}

TEST_CASE("s10: a same-rate re-prepare keeps the stream and a rate change restarts it with the ms value kept",
          "[loop_rig][s10]")
{
    CHECK(rinne::rig::scn::runRegisteredScenario("s10_prepare_rate"));
}
