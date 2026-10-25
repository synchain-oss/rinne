// SPDX-License-Identifier: GPL-3.0-or-later
//
// S21 止啸路径矩阵与锁存保持：每一条能让回送停下来的路径都在规定时间内归零，跳闸之后的锁存跨过所有「看起来像重新开始」
// 的事件仍然保持，只有恢复能清。
//
// 路径矩阵（正在响的环路，反馈 50 %、混合 50 %；归零 = 输出与干声逐位相同，回送一位不碰）：
//   紧急切断    ≤ 1 块 + 10 ms 归零，清空本地延迟线并锁存；冷却 1.5 s 内的恢复被忽略；再按一次紧急切断不恢复、
//               只重起冷却；不会自动恢复；冷却过后的恢复才放行，且切断之前的内容不会回来（清空过）；
//   loop_on 关  ≤ 1 块 + 10 ms 归零，不清空、不锁存；打开之后 10 ms 内回送回来，关之前写下的内容照常回来；
//   Input 旁路  同上，另外旁路期间输出逐位就是干声；
//   Output 旁路 读头在 D + 10 ms 之后逐位为 0，回送按 DC 阻断的尾巴单调衰减，Input 的状态仍是 Run，
//               Output 的状态是「旁路写零」，写头照常前进。
// 每一行里 Output 都不改动宿主 buffer（它看到的轨道缓冲与 Input 的输出逐位相同），输出与参照安全链逐位相同。
//
// 锁存保持：跳闸之后依次经过下列事件，状态始终是 Tripped（Input 槽里是「跳闸 + 紧急切断」），输出始终是干声，
// 跳闸计数不增：载入预设、宿主设置状态、同采样率重新 prepare、换写方、换组（重新配对）、换采样率重新 prepare。
// 之后冷却已过，恢复放行。
//
// 首块之前的紧急切断：新建引擎时命令序号的基线是 0，首块之前按下的紧急切断在首块生效；把基线设成当时的序号
// （重载工程的做法）则不重放。
//
// 标 ⛔ 的是删除式验证点：把对应的实现改坏，本场景必须变红。
//   ⛔ 紧急切断淡出走完之后不清本地延迟线 → 恢复之后读头读回切断之前的内容；
//   ⛔ loop_on 关时清本地延迟线 → 打开之后读头读不回关之前的内容；
//   ⛔ 同采样率重新 prepare 时清掉锁存 → 重新 prepare 之后回送自己回来。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include "engine/SafetyEvents.h"
#include "ipc/SegmentLayout.h"
#include "loop/StreamSync.h"
#include "s01_rig_kit.h"

namespace
{

using namespace rinne::rig;
using namespace rinne::rig::scn;
using rinne::engine::SafetyState;

constexpr int kBlock = 256;
constexpr std::int64_t kSecond = 48000;
// 1 块 + 10 ms：止啸路径要求的归零时限
constexpr std::size_t kSettle = static_cast<std::size_t>(kBlock + kGateSamples);

// 一条正在响的环路：反馈 50 %、混合 50 %，环路延迟 delayMs
class Row
{
public:
    explicit Row(float delayMs, double sampleRate = kSr)
    {
        host = makeHost(sampleRate, kBlock);
        REQUIRE(host != nullptr);
        host->setBlockScripts(fixedBlocks(kBlock), fixedBlocks(kBlock));
        rec = std::make_unique<Recorder>(*host, tones(), sampleRate);
        params = loopParams(50.0f, 50.0f, delayMs);
    }

    void run(std::int64_t samples) { rec->run(params, samples); }
    std::size_t now() const { return rec->outLeft.size(); }

    SafetyState state() const { return static_cast<SafetyState>(host->inputSafetyDiag().state); }
    std::uint32_t slotSafety() const { return host->slot(rinne::ipc::Role::kInput)->safety_state.load(); }

    // [from, to) 里输出与干声逐位相同（左右声道）
    bool silent(std::size_t from, std::size_t to) const
    {
        return sameBits(rec->outLeft, rec->dryLeft, from, to) && sameBits(rec->outRight, rec->dryRight, from, to);
    }
    // [from, to) 里读头（安全链的输入）全是 0
    bool tapZero(std::size_t from, std::size_t to) const
    {
        return allZero(rec->reference.tapLeft, from, to) && allZero(rec->reference.tapRight, from, to);
    }
    // Output 没有改动宿主 buffer：它处理之后看到的缓冲与 Input 的输出逐位相同
    bool outputLeftBufferAlone() const
    {
        return sameBits(rec->fedLeft, rec->outLeft, 0, rec->outLeft.size()) &&
               sameBits(rec->fedRight, rec->outRight, 0, rec->outRight.size());
    }

    std::unique_ptr<EngineHost> host;
    std::unique_ptr<Recorder> rec;
    rinne::engine::EngineParams params;
};

bool tripped(const Row& row)
{
    return row.slotSafety() == (rinne::ipc::kSafetyTripped | (rinne::ipc::kTripPanic << rinne::ipc::kTripShift));
}

class StopPathsScenario final : public Scenario
{
public:
    const char* id() const noexcept override { return "s21_stop_paths_latch"; }

    bool run(ScenarioContext&) override
    {
        Verdict v;
        panicRow(v);
        gateRows(v);
        outputBypassRow(v);
        latchRow(v);
        beforeFirstBlock(v);
        return v.ok();
    }

private:
    // 环路延迟 3 s：比 1.5 s 的冷却长，恢复的时候本地延迟线里要是还留着切断之前的内容，一看便知
    static void panicRow(Verdict& v)
    {
        INFO("panic");
        constexpr std::int64_t kD = 3 * kSecond;
        Row row(3000.0f);
        row.run(9 * kSecond / 2);
        RIG_EQ(v, static_cast<int>(row.state()), static_cast<int>(SafetyState::Run));
        RIG_CHECK(v, !row.silent(4 * static_cast<std::size_t>(kSecond), row.now())); // 回送在响

        // 紧急切断
        row.params.panicSeq = 1;
        const std::size_t panicAt = row.now();
        row.run(kSecond / 2);
        RIG_EQ(v, static_cast<int>(row.state()), static_cast<int>(SafetyState::Tripped));
        RIG_CHECK(v, tripped(row));
        RIG_EQ(v, row.host->inputSafetyDiag().trips, 1u);
        RIG_CHECK(v, row.silent(panicAt + kSettle, row.now()));
        RIG_CHECK(v, row.tapZero(panicAt + kSettle, row.now()));

        // 冷却内（+0.5 s）的恢复被忽略
        row.params.recoverSeq = 1;
        row.run(kSecond / 2);
        RIG_EQ(v, static_cast<int>(row.state()), static_cast<int>(SafetyState::Tripped));
        // 再按一次紧急切断（+1.0 s）：不恢复，冷却从这时重起，到 +2.5 s 才满
        row.params.panicSeq = 2;
        row.run(kSecond);
        RIG_EQ(v, static_cast<int>(row.state()), static_cast<int>(SafetyState::Tripped));
        RIG_EQ(v, row.host->inputSafetyDiag().trips, 1u);
        // +2.0 s 的恢复落在重起的冷却里：仍被忽略（要是冷却没重起，+1.5 s 之后这一条就放行了）
        row.params.recoverSeq = 2;
        row.run(3 * kSecond / 5);
        RIG_EQ(v, static_cast<int>(row.state()), static_cast<int>(SafetyState::Tripped));
        RIG_CHECK(v, row.silent(panicAt + kSettle, row.now()));

        // +2.6 s：冷却已过，恢复放行；这时离切断才 2.6 s < D，读头读不到切断之前的内容
        row.params.recoverSeq = 3;
        const std::size_t recoverAt = row.now();
        RIG_CHECK(v, recoverAt >= panicAt + static_cast<std::size_t>(5 * kSecond / 2));
        row.run(kSecond / 10);
        RIG_CHECK(v, row.state() != SafetyState::Tripped);
        // 切断之前的内容不回来：D 个样本之内读头全是 0，输出仍是干声（回送要等首个有效读样本）
        const std::size_t firstFresh = panicAt + static_cast<std::size_t>(kD);
        row.run(static_cast<std::int64_t>(firstFresh - row.now()) + 2000);
        RIG_CHECK(v, row.tapZero(panicAt + kSettle, firstFresh));
        RIG_CHECK(v, row.silent(panicAt + kSettle, firstFresh));
        // 之后新数据从首个有效读样本起淡入，回送回来
        row.run(kSecond);
        RIG_EQ(v, static_cast<int>(row.state()), static_cast<int>(SafetyState::Run));
        RIG_CHECK(v, !row.silent(firstFresh + static_cast<std::size_t>(kFadeInSamples), row.now()));
        RIG_CHECK(v, row.rec->matchesReference());
        RIG_CHECK(v, row.outputLeftBufferAlone());
    }

    // loop_on 关与 Input 旁路：环路延迟 500 ms，关 300 ms，比 D 短，打开之后读头读回关之前写下的内容
    static void gateRows(Verdict& v)
    {
        constexpr std::int64_t kD = kSecond / 2;
        for (const bool viaBypass : {false, true})
        {
            const char* label = viaBypass ? "input bypass" : "loop_on off";
            INFO(label);
            Row row(500.0f);
            row.run(2 * kSecond + 64 * kBlock);
            RIG_EQ(v, static_cast<int>(row.state()), static_cast<int>(SafetyState::Run));
            const std::size_t offAt = row.now();
            if (viaBypass)
            {
                row.rec->setInputBypassed(true);
            }
            else
            {
                row.params.loopOn = false;
            }
            row.run(56 * kBlock); // 约 300 ms
            const std::size_t onAt = row.now();
            RIG_CHECK(v, row.silent(offAt + kSettle, onAt));
            RIG_EQ(v, static_cast<int>(row.state()), static_cast<int>(SafetyState::Run)); // 不锁存
            row.rec->setInputBypassed(false);
            row.params.loopOn = true;
            row.run(kSecond);

            // 不清空：读头逐位就是 D 个样本之前 Input 的输出，关着的那段也照常；打开之后读到关之前写下的内容
            const auto& tap = row.rec->reference.tapLeft;
            std::size_t mismatched = 0;
            for (std::size_t t = static_cast<std::size_t>(kD) + 2000; t < tap.size(); ++t)
            {
                mismatched += tap[t] == row.rec->outLeft[t - static_cast<std::size_t>(kD)] ? 0u : 1u;
            }
            RIG_EQ(v, mismatched, std::size_t{0});
            RIG_CHECK(v, peakOf(tap, onAt, offAt + static_cast<std::size_t>(kD)) > 0.1f);
            RIG_EQ(v, row.host->inputStatus().resyncCount, 0u);
            // 打开之后回送 10 ms 回来
            RIG_CHECK(v, !row.silent(onAt + kGateSamples, onAt + 4 * kGateSamples));
            RIG_EQ(v, static_cast<int>(row.state()), static_cast<int>(SafetyState::Run));
            RIG_CHECK(v, row.rec->matchesReference());
            RIG_CHECK(v, row.outputLeftBufferAlone());
        }
    }

    static void outputBypassRow(Verdict& v)
    {
        INFO("output bypass");
        constexpr std::int64_t kD = 4800;
        Row row(100.0f);
        row.run(kSecond + 64 * kBlock);
        RIG_EQ(v, static_cast<int>(row.state()), static_cast<int>(SafetyState::Run));
        RIG_EQ(v, static_cast<int>(row.host->outputState()), static_cast<int>(rinne::engine::WriteState::kActive));
        const std::size_t bypassAt = row.now();
        row.host->setOutputBypassed(true);
        row.run(3 * kSecond / 2);

        // 读头在 D + 10 ms 之后逐位为 0
        const std::size_t zeroFrom = bypassAt + static_cast<std::size_t>(kD + kGateSamples);
        RIG_CHECK(v, row.tapZero(zeroFrom, row.now()));
        // 回送是 DC 阻断的尾巴：每 100 ms 的峰值只降不升，1 s 之后小于 1e-6
        const auto& ret = row.rec->reference.retLeft;
        RIG_CHECK(v, peakOf(ret, zeroFrom, zeroFrom + 4800) > 0.0f);
        bool monotone = true;
        for (std::size_t k = 1; k < 10; ++k)
        {
            monotone = monotone && peakOf(ret, zeroFrom + k * 4800, zeroFrom + (k + 1) * 4800) <=
                                       peakOf(ret, zeroFrom + (k - 1) * 4800, zeroFrom + k * 4800);
        }
        RIG_CHECK(v, monotone);
        RIG_CHECK(v, peakOf(ret, zeroFrom + static_cast<std::size_t>(kSecond), row.now()) < 1.0e-6f);
        // 安全链照常运行；Output 写零、写头照常前进
        RIG_EQ(v, static_cast<int>(row.state()), static_cast<int>(SafetyState::Run));
        RIG_EQ(v, row.host->input().safetyChain().machine().safetyGain(), 1.0f);
        RIG_EQ(v, static_cast<int>(row.host->outputState()),
               static_cast<int>(rinne::engine::WriteState::kBypassedZeros));
        RIG_EQ(v, row.host->writeHead(), static_cast<std::uint64_t>(row.now()));
        RIG_CHECK(v, row.rec->matchesReference());
        RIG_CHECK(v, row.outputLeftBufferAlone());
    }

    // 锁存保持：跳闸之后依次经过「看起来像重新开始」的事件
    static void latchRow(Verdict& v)
    {
        INFO("latch");
        Row row(500.0f);
        row.run(2 * kSecond + 64 * kBlock);
        RIG_EQ(v, static_cast<int>(row.state()), static_cast<int>(SafetyState::Run));
        row.params.panicSeq = 1;
        const std::size_t panicAt = row.now();
        row.run(kSecond / 2);
        RIG_CHECK(v, tripped(row));
        RIG_CHECK(v, row.host->addSecondOutput());

        // 每一步之后再走 0.4 s：仍是 Tripped，Input 槽里仍是「跳闸 + 紧急切断」，跳闸计数不增，输出仍是干声
        const auto stillLatched = [&](const char* what) {
            row.run(2 * kSecond / 5);
            INFO(what);
            RIG_EQ(v, static_cast<int>(row.state()), static_cast<int>(SafetyState::Tripped));
            RIG_CHECK(v, tripped(row));
            RIG_EQ(v, row.host->inputSafetyDiag().trips, 1u);
            RIG_CHECK(v, row.silent(panicAt + kSettle, row.now()));
        };
        row.params.presetSeq = 1;
        stillLatched("preset loaded");
        row.params.stateSeq = 1;
        stillLatched("host set state");
        row.host->prepare(kSr, kBlock);
        stillLatched("same-rate re-prepare");
        row.host->setSecondOutputActive(true);
        row.host->switchWriter(true);
        stillLatched("writer switched");
        row.host->setSecondOutputActive(false);
        row.host->switchWriter(false);
        stillLatched("writer switched back");
        RIG_CHECK(v, row.host->open(2));
        stillLatched("paired again in another group");
        RIG_CHECK(v, row.rec->matchesReference());
        RIG_CHECK(v, row.outputLeftBufferAlone());

        // 换采样率重新 prepare：锁存照样保持。参照安全链是 48 kHz 的，从这里起不再逐位比较
        row.host->prepare(96000.0, kBlock);
        stillLatched("rate change re-prepare");
        row.run(3 * kSecond); // 冷却早已走完，没有自动恢复
        RIG_EQ(v, static_cast<int>(row.state()), static_cast<int>(SafetyState::Tripped));
        RIG_CHECK(v, row.silent(panicAt + kSettle, row.now()));

        // 恢复放行，锁存不是永久的
        row.params.recoverSeq = 1;
        row.run(kSecond / 10);
        RIG_CHECK(v, row.state() != SafetyState::Tripped);
    }

    static void beforeFirstBlock(Verdict& v)
    {
        {
            INFO("panic before the first block");
            Row row(100.0f);
            row.params.panicSeq = 1;
            row.run(2 * kSecond);
            RIG_EQ(v, static_cast<int>(row.state()), static_cast<int>(SafetyState::Tripped));
            RIG_EQ(v, row.host->inputSafetyDiag().trips, 1u);
            RIG_CHECK(v, row.silent(0, row.now()));
            RIG_CHECK(v, row.rec->matchesReference());
        }
        {
            INFO("panic inside the command baseline is not replayed");
            Row row(100.0f);
            rinne::engine::CommandBaseline baseline;
            baseline.panicSeq = 1;
            row.rec->setCommandBaseline(baseline);
            row.params.panicSeq = 1;
            row.run(2 * kSecond);
            RIG_EQ(v, static_cast<int>(row.state()), static_cast<int>(SafetyState::Run));
            RIG_EQ(v, row.host->inputSafetyDiag().trips, 0u);
            RIG_CHECK(v, !row.silent(kSecond, row.now()));
            RIG_CHECK(v, row.rec->matchesReference());
        }
    }
};

} // namespace

RINNE_RIG_SCENARIO("s21_stop_paths_latch")
{
    return new StopPathsScenario();
}

TEST_CASE("s21: every silencing path reaches zero in time and a trip stays latched", "[loop_rig][s21]")
{
    CHECK(rinne::rig::scn::runRegisteredScenario("s21_stop_paths_latch"));
}
