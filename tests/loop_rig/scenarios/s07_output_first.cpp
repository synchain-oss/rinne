// SPDX-License-Identifier: GPL-3.0-or-later
//
// S07 错序（Output 先于 Input）：错放的轨道上环路照常工作，延迟恒为 D − n0，宿主给了时间戳就记下错序的证据。
//
// 错放时每个周期里 Output 先写、Input 后读：Input 读的那一刻写头已经领先本块 n 个样本，第一次配对把这个领先量锚定成
// 同步偏移 off = n0（首块的块长），之后读头始终比正确摆放时靠前 n0，回声的延迟就是 D − n0。回送这时进不了环
// （Output 写的时候 Input 还没加上它），所以环里只有干声，一个冲激只有一个回声，没有第二个。
//
// 五路，环路延迟 D = 4800、反馈 50 %、混合 50 %，宿主时间戳每个周期两端相同、随样本数递增：
//   恒定 256        首次配对之后零重同步；off 恒为 256；每个周期 D_eff = 4800；回声恰在冲激后 4544 = D − 256 个样本、
//                   幅度逐位 0.25f，没有第二个回声；漂移量 m 恒为 0；Input 记下 kOutputFirst 的证据；
//   256 与 2048 交替  同上（m 在 0 与 1792 之间交替，从不连续两次不为 0，被吸收）；
//   2048 与 256 交替  首块是 2048：off 恒为 2048，回声恰在冲激后 2752 = D − 2048 个样本；
//   带种子随机 64…2048 块长每块都变，m 连续两次不为 0：严格模式照设计重同步（原因：对齐），10 s 内 3 次转容忍模式、
//                   置「时序不稳定」，之后不再重同步；延迟稳定在 D − off（off 是最后一次锚定的值），回声幅度逐位
//                   0.25f；
//   对照            Input 先 Output 后：off = 0，回声恰在 D，证据是「顺序正确」；宿主没给时间戳时证据是 unknown
//                   （不猜），其余行为不变。
// 每路输出与参照安全链按文档接线重算的结果逐位相同。
//
// 随机一路的预期：不是「首次之后零重同步、off 恒定」。引擎实现的是严格模式（m 连续两次不为 0 就
// 重同步，10 s 内 3 次转容忍模式），错序加逐块变化的块长下 m 每块都变，所以随机一路按引擎的设计断言「有界、不发散、
// 转容忍之后不再重同步」。
//
// 标 ⛔ 的是删除式验证点：把对应的实现改坏，本场景必须变红。
//   ⛔ 实际环路延迟多加一个样本 → 回声不在 D − n0；
//   ⛔ 严格模式的重同步判据改成「m 不为 0 一次就重同步」→ 交替块长的两路出现重同步。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

#include "loop/LoopStatus.h"
#include "loop/StreamSync.h"
#include "s01_rig_kit.h"

namespace
{

using namespace rinne::rig;
using namespace rinne::rig::scn;
using rinne::loop::OrderVerdict;
using rinne::loop::SyncReason;

constexpr std::int64_t kDelay = 4800;
constexpr int kMaxBlock = 4096;

struct Leg
{
    const char* name;
    std::vector<int> lengths; // 每个周期的块长（两端相同）；周期数 = 长度
    CallOrder order;
    bool hostNsValid;
    std::uint64_t impulseAt;
};

struct Outcome
{
    std::vector<float> out;
    bool matchesReference = false;
    rinne::loop::LoopStatusFields last;
    std::int64_t firstOffset = 0;
    std::int64_t offsetMin = 0;
    std::int64_t offsetMax = 0;
    std::int64_t delayMin = 0;
    std::int64_t delayMax = 0;
    std::int64_t driftMin = 0;
    std::int64_t driftMax = 0;
    // 冲激之前（周期 settle 起）的重同步总数：settle 之后不得再增
    std::uint32_t resyncAtSettle = 0;
};

Outcome runLeg(const Leg& leg)
{
    Outcome outcome;
    const std::size_t cycles = leg.lengths.size();
    auto host = makeHost(kSr, kMaxBlock);
    REQUIRE(host != nullptr);
    BlockScript blocks;
    blocks.lengths = leg.lengths;
    host->setBlockScripts(blocks, blocks);
    host->setCallOrder(leg.order);

    // 宿主时间戳：每个周期两端拿到同一个值，随这个周期的第一个样本编号递增
    TransportScript transport;
    transport.hostNs.resize(cycles);
    transport.hostNsValid = {leg.hostNsValid};
    std::uint64_t position = 0;
    for (std::size_t i = 0; i < cycles; ++i)
    {
        transport.hostNs[i] = 1000000u + static_cast<std::uint64_t>(static_cast<double>(position) * 1.0e9 / kSr);
        position += static_cast<std::uint64_t>(leg.lengths[i]);
    }
    host->setTransportScript(transport);

    Recorder rec(*host, impulseSource(leg.impulseAt, 0.5f));
    const rinne::engine::EngineParams p = loopParams(50.0f, 50.0f, 100.0f);
    const std::size_t settle = std::min<std::size_t>(cycles, 60);
    for (std::size_t i = 0; i < cycles; ++i)
    {
        rec.cycle(i, p);
        const auto& s = host->inputStatus();
        if (i == 0)
        {
            outcome.firstOffset = outcome.offsetMin = outcome.offsetMax = s.offset;
            outcome.delayMin = outcome.delayMax = s.delayEffective;
            outcome.driftMin = outcome.driftMax = s.drift;
        }
        outcome.offsetMin = std::min(outcome.offsetMin, s.offset);
        outcome.offsetMax = std::max(outcome.offsetMax, s.offset);
        outcome.delayMin = std::min(outcome.delayMin, s.delayEffective);
        outcome.delayMax = std::max(outcome.delayMax, s.delayEffective);
        outcome.driftMin = std::min(outcome.driftMin, s.drift);
        outcome.driftMax = std::max(outcome.driftMax, s.drift);
        if (i + 1 == settle)
        {
            outcome.resyncAtSettle = s.resyncCount;
        }
    }
    outcome.out = rec.outLeft;
    outcome.matchesReference = rec.matchesReference();
    outcome.last = host->inputStatus();
    return outcome;
}

std::vector<int> alternating(int first, int second, std::size_t cycles)
{
    std::vector<int> lengths(cycles);
    for (std::size_t i = 0; i < cycles; ++i)
    {
        lengths[i] = i % 2 == 0 ? first : second;
    }
    return lengths;
}

std::vector<int> seededRandom(std::uint64_t seed, int lo, int hi, std::size_t cycles)
{
    rinne::test::Rng rng(seed);
    std::vector<int> lengths(cycles);
    for (int& n : lengths)
    {
        n = lo + rng.below(hi - lo + 1);
    }
    return lengths;
}

class OutputFirstScenario final : public Scenario
{
public:
    const char* id() const noexcept override { return "s07_output_first"; }

    bool run(ScenarioContext&) override
    {
        Verdict v;
        constexpr std::uint64_t kAt = 30100;
        constexpr std::uint64_t kLateAt = 150000;

        // 三路块长规律的错序：延迟恒为 D − n0（n0 是首块的块长），首次之后零重同步，off 恒定
        {
            INFO("constant 256");
            const Outcome o = runLeg({"constant", std::vector<int>(160, 256), CallOrder::kOutputFirst, true, kAt});
            steady(v, o, 256, kAt);
            RIG_EQ(v, o.driftMin, std::int64_t{0});
            RIG_EQ(v, o.driftMax, std::int64_t{0});
        }
        {
            INFO("256 / 2048 alternating");
            const Outcome o = runLeg({"alt-256", alternating(256, 2048, 90), CallOrder::kOutputFirst, true, kAt});
            steady(v, o, 256, kAt);
            // m 在 0 与 1792 之间交替：从不连续两次不为 0，被吸收
            RIG_EQ(v, o.driftMin, std::int64_t{0});
            RIG_EQ(v, o.driftMax, std::int64_t{1792});
            RIG_CHECK(v, o.last.offsetDriftBlocks > 0u);
        }
        {
            INFO("2048 / 256 alternating");
            const Outcome o = runLeg({"alt-2048", alternating(2048, 256, 90), CallOrder::kOutputFirst, true, kAt});
            steady(v, o, 2048, kAt);
            RIG_EQ(v, o.driftMin, std::int64_t{-1792});
            RIG_EQ(v, o.driftMax, std::int64_t{0});
        }

        // 带种子随机：照设计重同步，转容忍模式之后不再重同步，延迟稳定在 D − off
        {
            INFO("seeded random 64..2048");
            const Outcome o =
                runLeg({"random", seededRandom(99, 64, 2048, 300), CallOrder::kOutputFirst, true, kLateAt});
            RIG_CHECK(v, o.matchesReference);
            RIG_EQ(v, o.last.resyncCount, 3u);
            RIG_EQ(v, o.last.resyncByReason[static_cast<std::size_t>(SyncReason::kShift)], 3u);
            RIG_EQ(v, o.last.resyncCount, o.resyncAtSettle); // 转容忍之后不再重同步
            RIG_EQ(v, o.last.tolerant, 1u);
            RIG_EQ(v, o.last.unstable, 1u);
            RIG_EQ(v, o.delayMin, kDelay);
            RIG_EQ(v, o.delayMax, kDelay);
            const std::int64_t off = o.last.offset;
            RIG_CHECK(v, off >= 64 && off <= 2048);
            const auto echo = static_cast<std::size_t>(kLateAt) + static_cast<std::size_t>(kDelay - off);
            RIG_EQ(v, argmaxAbs(o.out, static_cast<std::size_t>(kLateAt) + 1, o.out.size()), echo);
            RIG_EQ(v, rinne::test::floatBits(o.out[echo]), rinne::test::floatBits(0.25f));
            RIG_CHECK(v, o.last.orderVerdict == static_cast<std::uint8_t>(OrderVerdict::kOutputFirst));
        }

        // 对照：Input 先 Output 后，同样的块长与时间戳
        {
            INFO("control: input first");
            const Outcome o = runLeg({"control", std::vector<int>(160, 256), CallOrder::kInputFirst, true, kAt});
            RIG_CHECK(v, o.matchesReference);
            RIG_EQ(v, o.last.resyncCount, 0u);
            RIG_EQ(v, o.offsetMin, std::int64_t{0});
            RIG_EQ(v, o.offsetMax, std::int64_t{0});
            RIG_EQ(v, argmaxAbs(o.out, kAt + 1, o.out.size()), kAt + static_cast<std::uint64_t>(kDelay));
            RIG_CHECK(v, o.last.orderVerdict == static_cast<std::uint8_t>(OrderVerdict::kCorrect));
            RIG_EQ(v, o.last.hostNsValid, 1u);
        }
        // 对照：错序但宿主没给时间戳：证据是 unknown（不猜），回声与重同步照旧
        {
            INFO("control: output first without host time");
            const Outcome o = runLeg({"no-time", std::vector<int>(160, 256), CallOrder::kOutputFirst, false, kAt});
            RIG_CHECK(v, o.matchesReference);
            RIG_EQ(v, o.last.resyncCount, 0u);
            RIG_EQ(v, argmaxAbs(o.out, kAt + 1, o.out.size()), kAt + static_cast<std::uint64_t>(kDelay - 256));
            RIG_CHECK(v, o.last.orderVerdict == static_cast<std::uint8_t>(OrderVerdict::kUnknown));
            RIG_EQ(v, o.last.hostNsValid, 0u);
        }
        return v.ok();
    }

private:
    // 规律块长的错序：首次之后零重同步，off 与 D_eff 恒定，回声恰在 D − n0，只有这一个回声，证据置位
    static void steady(Verdict& v, const Outcome& o, std::int64_t n0, std::uint64_t impulseAt)
    {
        RIG_CHECK(v, o.matchesReference);
        RIG_EQ(v, o.last.resyncCount, 0u);
        RIG_EQ(v, o.firstOffset, n0);
        RIG_EQ(v, o.offsetMin, n0);
        RIG_EQ(v, o.offsetMax, n0);
        RIG_EQ(v, o.delayMin, kDelay);
        RIG_EQ(v, o.delayMax, kDelay);
        const auto d = static_cast<std::uint64_t>(kDelay - n0);
        const auto echo = static_cast<std::size_t>(impulseAt + d);
        RIG_EQ(v, argmaxAbs(o.out, static_cast<std::size_t>(impulseAt) + 1, o.out.size()), echo);
        RIG_EQ(v, rinne::test::floatBits(o.out[echo]), rinne::test::floatBits(0.25f));
        // 环里只有干声：第二个回声（再过 D − n0）不存在，DC 阻断的尾巴比它小得多
        RIG_CHECK(v, peakOf(o.out, echo + 1, o.out.size()) < 1.0e-3f);
        RIG_CHECK(v, o.last.orderVerdict == static_cast<std::uint8_t>(OrderVerdict::kOutputFirst));
        RIG_EQ(v, o.last.hostNsValid, 1u);
    }
};

} // namespace

RINNE_RIG_SCENARIO("s07_output_first")
{
    return new OutputFirstScenario();
}

TEST_CASE("s07: with output before input the loop delay is D minus n0 and nothing resyncs", "[loop_rig][s07]")
{
    CHECK(rinne::rig::scn::runRegisteredScenario("s07_output_first"));
}
