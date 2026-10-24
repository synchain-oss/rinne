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
// 暂缺（不是实现被改动）：
//   - 「Output 换采样率而 Input 保持原采样率 → format 重同步」：试验台的 prepare 总是两端一起重新 prepare，
//     没有只让 Output 换采样率并重新发布写方的接口，Input 自己换采样率走的是域重来，不走 format 重同步；
//   - 「采样率不一致 → 回送为 0」：引擎不比较环头里的采样率，采样率不一致只在壳的配对状态里作为显示状态存在。
//
// 标 ⛔ 的是删除式验证点：把对应的实现改坏，本场景必须变红。
//   ⛔ 同采样率重新 prepare 时也重建安全链 → 重新淡入，输出与对照运行不再逐位相同；
//   ⛔ 同采样率重新 prepare 时也按「换采样率」重来域 → 流被打断，输出与对照运行不再逐位相同；
//   ⛔ 换采样率时不重新分配延迟线 → D 仍是 4800，回声不在 9600。

#include <catch2/catch_test_macros.hpp>

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
