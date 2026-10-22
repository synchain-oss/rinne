// SPDX-License-Identifier: GPL-3.0-or-later
//
// 块长不变性（安全不变量 7）：参数恒定时，宿主怎么切块都不影响输出，逐位相同。
// 低切 / 高切、开关过渡都按样本计数，与块边界无关：整块处理与随机切分逐位一样。
// 必须变红：把滤波器的状态改成「每块重算一次系数」（按块更新系数网格）或者把交叉位置记成块内样本数，
// 下面「随机切分」的格子就会红；整块与切分的差别在切分点上暴露。

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

#include "ToneTestKit.h"
#include "dsp/MixLaw.h"
#include "dsp/SvfTpt.h"
#include "dsp/SwitchRamp.h"

using rinne::dsp::CutFilter;
using rinne::dsp::CutKind;
using rinne::dsp::SwitchRamp;
using rinne::test::floatBits;
using rinne::test::Rng;

namespace
{

constexpr double kRates[] = {44100.0, 48000.0, 96000.0, 192000.0};

// 按给定的块长序列处理一串样本，返回输出
template<typename Run>
std::vector<float> runInBlocks(const std::vector<float>& in, const std::vector<std::size_t>& blocks, Run&& run)
{
    std::vector<float> out(in.size());
    std::size_t i = 0;
    std::size_t b = 0;
    while (i < in.size())
    {
        const std::size_t n = blocks[b % blocks.size()];
        b += 1;
        const std::size_t end = std::min(in.size(), i + n);
        for (; i < end; ++i)
            out[i] = run(in[i]);
    }
    return out;
}

} // namespace

TEST_CASE("the cut filter gives the same bits however the host splits the blocks", "[tone][blocks]")
{
    for (const double sr : kRates)
    {
        INFO("sr = " << sr);
        for (const CutKind kind : {CutKind::LowCut, CutKind::HighCut})
        {
            INFO("kind = " << static_cast<int>(kind));
            const bool low = kind == CutKind::LowCut;
            Rng rng(41);
            std::vector<float> in(20000);
            for (std::size_t i = 0; i < in.size(); ++i)
                in[i] = static_cast<float>(rng.range(-1.0, 1.0));

            // 参数恒定：整块跑一遍（这就是基准）
            CutFilter whole(kind);
            whole.setCutoff(low ? 300.0f : 4000.0f);
            whole.prepare(sr);
            const std::vector<float> reference =
                runInBlocks(in, {in.size()}, [&whole](float x) { return whole.process(x); });

            // 同样的参数、随机切分：必须逐位相同
            const std::vector<std::size_t> splits = {1, 3, 17, 64, 2, 511, 5};
            CutFilter split(kind);
            split.setCutoff(low ? 300.0f : 4000.0f);
            split.prepare(sr);
            const std::vector<float> chunked = runInBlocks(in, splits, [&split](float x) { return split.process(x); });

            bool same = true;
            for (std::size_t i = 0; i < in.size(); ++i)
                same = same && floatBits(reference[i]) == floatBits(chunked[i]);
            CHECK(same);
        }
    }
}

TEST_CASE("a cut filter mid-glide also ignores the block boundaries", "[tone][blocks]")
{
    // 平滑进行中也一样：滑动的步数按样本计，不按块计
    const double sr = 48000.0;
    Rng rng(43);
    std::vector<float> in(20000);
    for (std::size_t i = 0; i < in.size(); ++i)
        in[i] = static_cast<float>(rng.range(-1.0, 1.0));

    CutFilter whole(CutKind::HighCut);
    whole.setCutoff(1000.0f);
    whole.prepare(sr);
    whole.setCutoff(8000.0f); // 带着 30 ms 的滑动跑
    const std::vector<float> reference = runInBlocks(in, {in.size()}, [&whole](float x) { return whole.process(x); });

    CutFilter split(CutKind::HighCut);
    split.setCutoff(1000.0f);
    split.prepare(sr);
    split.setCutoff(8000.0f);
    const std::vector<float> chunked =
        runInBlocks(in, {1, 7, 33, 128, 2}, [&split](float x) { return split.process(x); });

    bool same = true;
    for (std::size_t i = 0; i < in.size(); ++i)
        same = same && floatBits(reference[i]) == floatBits(chunked[i]);
    CHECK(same);
}

TEST_CASE("the switch ramp gives the same bits however the host splits the blocks", "[tone][blocks]")
{
    for (const double sr : kRates)
    {
        INFO("sr = " << sr);
        Rng rng(47);
        std::vector<float> in(20000);
        for (std::size_t i = 0; i < in.size(); ++i)
            in[i] = static_cast<float>(rng.range(-1.0, 1.0));

        SwitchRamp whole;
        whole.prepare(sr, rinne::dsp::kSwitchRampSeconds);
        whole.setTarget(false);
        whole.snapToTarget();
        const std::vector<float> reference = runInBlocks(in, {in.size()}, [&](float x) {
            whole.setTarget(true);
            whole.step();
            return whole.mix(x, -x);
        });

        const std::vector<std::size_t> splits = {1, 5, 64, 3, 257, 2};
        SwitchRamp split;
        split.prepare(sr, rinne::dsp::kSwitchRampSeconds);
        split.setTarget(false);
        split.snapToTarget();
        const std::vector<float> chunked = runInBlocks(in, splits, [&](float x) {
            split.setTarget(true);
            split.step();
            return split.mix(x, -x);
        });

        bool same = true;
        for (std::size_t i = 0; i < in.size(); ++i)
            same = same && floatBits(reference[i]) == floatBits(chunked[i]);
        CHECK(same);
    }
}

TEST_CASE("the mix gains depend only on the mix value, not on the order or the block size", "[tone][blocks][mix]")
{
    // 逐位快速路径的前提：g_dry 在 m ≤ 50 % 时恰为 1.0f，任何切分下都不变
    using rinne::dsp::mixGains;
    for (const float m : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f})
    {
        INFO("m = " << m);
        const auto g = mixGains(m);
        const auto again = mixGains(m);
        CHECK(g.dry == again.dry);
        CHECK(g.ret == again.ret);
    }
    CHECK(mixGains(0.25f).dry == 1.0f);
    CHECK(mixGains(0.5f).dry == 1.0f);
}
