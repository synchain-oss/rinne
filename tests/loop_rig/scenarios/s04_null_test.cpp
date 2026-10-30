// SPDX-License-Identifier: GPL-3.0-or-later
//
// S04 null test：回送被关掉的几种条件下，Input 的输出与输入逐位相同，Output 对宿主缓冲逐位直通。
//
// 条件（mix ≤ 50 % 时干声增益恰为 1；feedback 为 0 或 loop_on 关时回送恒为 0；mix 为 0 时不论 feedback 多大都不回送）：
//   默认参数            feedback 0、mix 25 %、loop_on 开
//   feedback 0          mix 25 / 50 / 0 %
//   loop_on 关          feedback 100 %，mix 25（默认）/ 50 %
//   mix 0               feedback 100 / 150 %
//   Input 旁路          feedback 100 %、mix 50 %
//   未配对              feedback 100 %、mix 50 %，只有 Input 一端在跑（Output 从不被调用）
// 每一格都在环路里跑（Input → 中间链 → Output，两端都在、真共享内存；未配对一格除外），块长 1…8192 带种子随机。
//
// 信号：带种子的随机位型，其中 1/16 的样本是特殊值：+0.0、−0.0、带载荷的 quiet NaN 与 signaling NaN（两种符号）、
// ±Inf、非规格化数、±1e30。−0.0 之后加 +0.0 会变成 +0.0，NaN 经任何算术都会改位型（signaling NaN 变 quiet NaN），
// 所以「回送为 ±0 的样本一位不碰」只能是不写，不能是加 0 或乘 1：逐位比较抓得住这几种写法。
//
// 断言（每一格）：
//   - Input 的输出（左右声道）与它的输入逐位相同，从第一个样本起，全程，包括配对之前、淡入之中与之后；
//   - Output 处理之后的宿主缓冲与它处理之前（Input 的输出，中间链是恒等）逐位相同；
//   - 信号里的非有限值确实到了 Output 手里：Output 的清洗计数恰为含非有限值的帧数，钳位计数恰为两个声道都有限、
//     有一个 |v| > 16 的帧数（环里因此只有干净的值，不是 NaN 把一切都变成了平凡相同）；
//   - 输出与参照安全链按文档接线重算的结果逐位相同。
// 对照（必须不同，证明逐位比较看得见变化）：feedback 50 % × mix 50 % 的回送、mix 75 % × feedback 0 的干声增益 0.5、
// mix 100 % 的干声增益 0，三格里输出与输入都有许多样本不相同。
//
// 标 ⛔ 的是删除式验证点：把对应的实现改坏，本场景必须变红。
//   ⛔ Input 回送为 ±0 的样本也写（dry + 0.0）→ −0.0 变 +0.0、signaling NaN 变 quiet NaN，每一格都红；
//   ⛔ loop_on 关时回送增益不归零 → loop_on 关的两格变红；
//   ⛔ Output 在宿主缓冲里就地清洗 → 直通一项变红。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "s01_rig_kit.h"

namespace
{

using namespace rinne::rig;
using namespace rinne::rig::scn;

constexpr int kMaxBlock = 8192;
constexpr float kDelayMs = 200.0f; // 9600 个样本，不小于最大块长
constexpr std::int64_t kSamples = 4 * 48000;

float fromBits(std::uint32_t bits)
{
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

std::uint64_t mix64(std::uint64_t z)
{
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

// 一个样本：15/16 是 [-1, 1) 的随机浮点，1/16 是特殊值（八种各占一份）。编号和种子的纯函数。
float nullSample(std::uint64_t index, std::uint64_t seed)
{
    const std::uint64_t h = mix64(index * 0x9e3779b97f4a7c15ull + seed * 0xd1b54a32d192ed03ull + 1u);
    if ((h & 15u) != 0u)
    {
        return hashNoise(index, seed);
    }
    const std::uint32_t payload = static_cast<std::uint32_t>(h >> 24) & 0x003fffffu;
    switch ((h >> 8) & 7u)
    {
    case 0:
        return 0.0f;
    case 1:
        return fromBits(0x80000000u); // −0.0
    case 2:
        return fromBits(0x7fc00000u | payload); // quiet NaN，带载荷
    case 3:
        return fromBits(0xff800000u | (payload | 1u)); // signaling NaN（符号位置上、安静位为 0、载荷非 0）
    case 4:
        return fromBits(0x7f800000u); // +Inf
    case 5:
        return fromBits(0xff800000u); // −Inf
    case 6:
        return fromBits(0x80000000u | ((payload | 1u) & 0x007fffffu)); // 负的非规格化数
    default:
        return (h & 0x1000000u) != 0u ? 1.0e30f : -1.0e30f;
    }
}

SourceFn nullSource()
{
    return [](std::uint64_t first, float* left, float* right, int n) {
        for (int i = 0; i < n; ++i)
        {
            const std::uint64_t at = first + static_cast<std::uint64_t>(i);
            left[i] = nullSample(at, 101);
            right[i] = nullSample(at, 202);
        }
    };
}

// 块长 1…8192 带种子随机（一半取小块，一半取大块），总长不小于 total
BlockScript randomBlocks(std::uint64_t seed, std::int64_t total)
{
    BlockScript script;
    std::uint64_t state = seed;
    for (std::int64_t sum = 0; sum < total;)
    {
        state = mix64(state + 0x9e3779b97f4a7c15ull);
        const int n = (state & 1u) != 0u ? 1 + static_cast<int>((state >> 8) % 64u)
                                         : 1 + static_cast<int>((state >> 8) % static_cast<std::uint64_t>(kMaxBlock));
        script.lengths.push_back(n);
        sum += n;
    }
    return script;
}

struct Cell
{
    const char* name;
    rinne::engine::EngineParams params;
    bool inputBypassed = false;
    bool outputAbsent = false; // 只有 Input 一端在跑
    bool expectNull = true;
};

rinne::engine::EngineParams params(float feedback, float mix, bool loopOn = true)
{
    rinne::engine::EngineParams p = loopParams(feedback, mix, kDelayMs);
    p.loopOn = loopOn;
    return p;
}

struct CellResult
{
    bool outputEqualsInput = false;
    bool outputUntouched = false;
    bool matchesReference = false;
    std::size_t differing = 0; // 输出与输入位型不同的样本数（左声道）
    std::uint32_t nonFinite = 0;
    std::uint32_t clamped = 0;
    std::uint32_t wantNonFinite = 0;
    std::uint32_t wantClamped = 0;
    std::size_t samples = 0;
};

CellResult runCell(const Cell& cell)
{
    CellResult result;
    auto host = makeHost(kSr, kMaxBlock);
    REQUIRE(host != nullptr);
    const BlockScript blocks = randomBlocks(0x4e554c4cu, kSamples);
    host->setBlockScripts(blocks, blocks);
    if (cell.outputAbsent)
    {
        host->setCallOrder(CallOrder::kInputOnly);
    }
    host->setInputBypassed(cell.inputBypassed);

    Recorder rec(*host, nullSource());
    rec.setInputBypassed(cell.inputBypassed);
    std::size_t index = 0;
    while (rec.outLeft.size() < static_cast<std::size_t>(kSamples))
    {
        if (rec.cycle(index++, cell.params) <= 0)
        {
            break;
        }
    }

    result.samples = rec.outLeft.size();
    result.outputEqualsInput = sameBits(rec.outLeft, rec.dryLeft, 0, result.samples) &&
                               sameBits(rec.outRight, rec.dryRight, 0, result.samples);
    if (!cell.outputAbsent)
    {
        result.outputUntouched = sameBits(rec.fedLeft, rec.outLeft, 0, result.samples) &&
                                 sameBits(rec.fedRight, rec.outRight, 0, result.samples);
    }
    result.matchesReference = rec.matchesReference();
    // Output 看到的是 Input 的输出：null 格里就是输入本身，所以应有的清洗与钳位计数按输入算
    for (std::size_t i = 0; i < result.samples; ++i)
    {
        result.differing += rinne::test::floatBits(rec.outLeft[i]) != rinne::test::floatBits(rec.dryLeft[i]) ? 1u : 0u;
        const float l = rec.dryLeft[i];
        const float r = rec.dryRight[i];
        const bool bad = !std::isfinite(l) || !std::isfinite(r);
        result.wantNonFinite += bad ? 1u : 0u;
        result.wantClamped += !bad && (std::fabs(l) > 16.0f || std::fabs(r) > 16.0f) ? 1u : 0u;
    }
    result.nonFinite = host->outputNonFiniteCount();
    result.clamped = host->outputClampCount();
    return result;
}

class NullTestScenario final : public Scenario
{
public:
    const char* id() const noexcept override { return "s04_null_test"; }

    bool run(ScenarioContext&) override
    {
        Verdict v;
        const Cell nullCells[] = {
            {"default parameters", rinne::engine::EngineParams{}},
            {"feedback 0, mix 25", params(0.0f, 25.0f)},
            {"feedback 0, mix 50", params(0.0f, 50.0f)},
            {"feedback 0, mix 0", params(0.0f, 0.0f)},
            {"loop off, feedback 100, mix 25", params(100.0f, 25.0f, false)},
            {"loop off, feedback 100, mix 50", params(100.0f, 50.0f, false)},
            {"mix 0, feedback 100", params(100.0f, 0.0f)},
            {"mix 0, feedback 150", params(150.0f, 0.0f)},
            {"input bypassed", params(100.0f, 50.0f), true},
            {"unpaired", params(100.0f, 50.0f), false, true},
        };
        for (const Cell& cell : nullCells)
        {
            INFO("cell: " << cell.name);
            const CellResult r = runCell(cell);
            RIG_CHECK(v, r.samples >= static_cast<std::size_t>(kSamples));
            RIG_CHECK(v, r.outputEqualsInput);
            RIG_CHECK(v, r.matchesReference);
            if (!cell.outputAbsent)
            {
                RIG_CHECK(v, r.outputUntouched);
                // 非有限值与超大值确实到了 Output 手里，被它清洗、钳位并计数
                RIG_CHECK(v, r.wantNonFinite > 1000u);
                RIG_EQ(v, r.nonFinite, r.wantNonFinite);
                RIG_EQ(v, r.clamped, r.wantClamped);
            }
        }

        const Cell controls[] = {
            {"control: feedback 50, mix 50", params(50.0f, 50.0f), false, false, false},
            {"control: mix 75, feedback 0", params(0.0f, 75.0f), false, false, false},
            {"control: mix 100, feedback 0", params(0.0f, 100.0f), false, false, false},
        };
        for (const Cell& cell : controls)
        {
            INFO("cell: " << cell.name);
            const CellResult r = runCell(cell);
            RIG_CHECK(v, r.matchesReference);
            RIG_CHECK(v, !r.outputEqualsInput);
            RIG_CHECK(v, r.differing > 10000u);
        }
        return v.ok();
    }
};

} // namespace

RINNE_RIG_SCENARIO("s04_null_test")
{
    return new NullTestScenario();
}

TEST_CASE("s04: with the return off the Input output is the input bit for bit, NaN payloads and -0.0 included",
          "[loop_rig][s04]")
{
    CHECK(rinne::rig::scn::runRegisteredScenario("s04_null_test"));
}
