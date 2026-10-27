// SPDX-License-Identifier: GPL-3.0-or-later
//
// 改 D 的 40 ms 线性交叉淡化：增益本身与它在读头混音里的落点。
//
//   - 两路增益逐样本之和恰为 1.0f（不是约等于）：任何采样率、任何一个样本。
//   - 总长恰为 40 ms：第 0 个样本旧读头满增益，第 L 个样本起只剩新读头。
//   - 直流信号穿过交叉淡化逐位不变：线性交叉在相关信号上不过冲，等功率交叉会在中点鼓起约 3 dB。
//   - 起点落在块中间时，块内起点之前新读头是 0、旧读头是 1。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "TimingKit.h"
#include "loop/DelayRetarget.h"
#include "loop/Taps.h"

namespace
{

using namespace rinne::loop;
using namespace rinne::timing_test;

} // namespace

TEST_CASE("交叉淡化增益 两路之和逐样本恰为 1.0f：44.1 / 48 / 88.2 / 96 / 192 kHz 的每一个样本", "[timing][xfade]")
{
    // ⛔ 删除式验证点：out 不由同一个 in 做减法得到、而是另算 (L − k) / L → 个别样本上之和差一个比特，本用例变红；
    //    改成等功率（sin / cos）→ 之和不再是 1，本用例变红。
    const double rates[] = {44100.0, 48000.0, 88200.0, 96000.0, 192000.0};
    for (const double sr : rates)
    {
        const std::int64_t length = xfadeSamplesFor(sr);
        INFO("sr = " << sr);
        // 总长恰为 40 ms
        CHECK(length == static_cast<std::int64_t>(std::llround(0.040 * sr)));
        std::int64_t bad = 0;
        float previousIn = -1.0f;
        float previousOut = 2.0f;
        for (std::int64_t k = -3; k <= length + 3; ++k)
        {
            const XfadeGains g = linearXfadeGains(k, length);
            bad += (g.in + g.out == 1.0f) ? 0 : 1;
            // 单调：新读头只升不降，旧读头只降不升
            bad += (g.in >= previousIn) ? 0 : 1;
            bad += (g.out <= previousOut) ? 0 : 1;
            previousIn = g.in;
            previousOut = g.out;
        }
        CHECK(bad == 0);
        // 端点：开始之前与第 0 个样本旧读头满增益；第 L 个样本起只剩新读头
        for (std::int64_t k = -3; k <= 0; ++k)
        {
            CHECK(linearXfadeGains(k, length).in == 0.0f);
            CHECK(linearXfadeGains(k, length).out == 1.0f);
        }
        CHECK(linearXfadeGains(length - 1, length).in < 1.0f);
        CHECK(linearXfadeGains(length - 1, length).out > 0.0f);
        for (std::int64_t k = length; k <= length + 3; ++k)
        {
            CHECK(linearXfadeGains(k, length).in == 1.0f);
            CHECK(linearXfadeGains(k, length).out == 0.0f);
        }
        // 线性：in = k / L；中点恰为一半（等功率会是 0.7071）
        CHECK(linearXfadeGains(length / 2, length).in == static_cast<float>(length / 2) / static_cast<float>(length));
        if (length % 2 == 0)
        {
            CHECK(linearXfadeGains(length / 2, length).in == 0.5f);
            CHECK(linearXfadeGains(length / 2, length).out == 0.5f);
        }
    }
}

TEST_CASE("交叉淡化增益 读头集合里的起点可以落在块中间：起点之前旧读头 1、新读头 0，之后逐样本之和仍是 1",
          "[timing][xfade]")
{
    // ⛔ 删除式验证点：块内起点之前也按步数 0 起算（交叉淡化提前开始）→ 起点之前的样本增益不是 (0, 1)，本用例变红。
    constexpr std::int64_t kLength = 1920;
    TapSet taps;
    taps.beginCrossfade(kLength, -300); // 起点在本块第 300 个样本
    CHECK(taps.crossfadeActive());
    CHECK(taps.crossfadeRemaining() == kLength); // 起点之前不算已走
    for (std::int64_t i = 0; i < 700; ++i)
    {
        const XfadeGains g = taps.crossfadeGainsAt(i);
        CHECK(g.in + g.out == 1.0f);
        if (i <= 300)
        {
            CHECK(g.in == 0.0f);
            CHECK(g.out == 1.0f);
        }
        else
        {
            CHECK(g.in > 0.0f);
        }
    }
    // 没有读头挂着角色：推进之后状态收尾（不残留）
    taps.advanceCrossfade(700);
    CHECK_FALSE(taps.crossfadeActive());
}

TEST_CASE("交叉淡化 直流信号穿过 D 的改动逐位不变（线性交叉不过冲）：44.1 / 48 / 96 kHz、D 变大与变小",
          "[timing][xfade]")
{
    // ⛔ 删除式验证点：线性交叉改成等功率（sin / cos）→ 中点 0.7071 + 0.7071 = 1.414，直流被抬高 3 dB，本用例变红；
    //    两路增益之和差一个比特 → 个别样本不再逐位等于直流值，本用例变红。
    struct Case
    {
        double sr;
        float dc;
        bool larger;
    };
    const Case cases[] = {
        {48000.0, 1.0f, true}, {48000.0, 0.5f, true},  {48000.0, -0.25f, true}, {48000.0, 0.5f, false},
        {44100.0, 0.5f, true}, {96000.0, 0.5f, false}, {96000.0, 1.0f, true},
    };
    for (const Case& c : cases)
    {
        INFO("sr = " << c.sr << " dc = " << c.dc << " larger = " << c.larger);
        Rig rig(c.sr, 1024);
        const float dc = c.dc;
        rig.setSignal([dc](std::int64_t) { return dc; });
        const auto samples = [&](double ms) { return static_cast<std::int64_t>(std::llround(ms * 0.001 * c.sr)); };
        const double msA = c.larger ? 150.0 : 300.0;
        const double msB = c.larger ? 300.0 : 150.0;
        EngineParams params;
        params.delayMs = static_cast<float>(msA);
        // 暖机：两个 D 都要有数据可读（D 变大时新读头读更早的位置），再加 20 ms onset
        const std::int64_t settle = std::max(samples(msA), samples(msB)) + samples(20.0) + 1024;
        rig.run(settle, {256}, params);
        const std::size_t mark = rig.trace.size();
        REQUIRE(rig.trace.back() == dc);
        params.delayMs = static_cast<float>(msB);
        rig.run(samples(40.0) + 4096, {97, 256, 1000, 31}, params);

        const LoopStatusFields fields = rig.status();
        CHECK(fields.delayEffective == samples(msB));
        CHECK(fields.resyncByReason[static_cast<std::size_t>(SyncReason::kDChange)] == 1u);
        // 从改动前一个样本起，每一个样本都逐位等于直流值
        std::int64_t off = 0;
        for (std::size_t t = mark - 1; t < rig.trace.size(); ++t)
        {
            off += rig.trace[t] == dc ? 0 : 1;
        }
        CHECK(off == 0);
    }
}

TEST_CASE("交叉淡化 每个样本都等于 旧读头 × out + 新读头 × in：总长恰为 40 ms，起点在块首，第 L 个样本起只剩新读头",
          "[timing][xfade]")
{
    // ⛔ 删除式验证点：总长差一个样本（L − 1 或 L + 1）→ 终点的样本不再是新读头单独的值，本用例变红；
    //    起点晚一个样本 → 第 0 个样本之后的增益整体错位，本用例变红。
    for (const double sr : {48000.0, 44100.0})
    {
        INFO("sr = " << sr);
        Rig rig(sr, 1024);
        const auto length = static_cast<std::int64_t>(std::llround(0.040 * sr));
        EngineParams params;
        params.delayMs = 200.0f;
        const std::int64_t d1 = static_cast<std::int64_t>(std::llround(0.200 * sr));
        rig.run(static_cast<std::int64_t>(std::llround(0.300 * sr)) + 4096, {256}, params); // 两个 D 都有数据可读
        REQUIRE(rig.status().delayEffective == d1);
        const auto mark = static_cast<std::int64_t>(rig.trace.size());

        params.delayMs = 300.0f;
        rig.run(length + 3000, {256}, params);
        const std::int64_t d2 = rig.status().delayEffective;
        REQUIRE(d2 == static_cast<std::int64_t>(std::llround(0.300 * sr)));
        REQUIRE(rig.xfadeStarts().size() == 1u);
        CHECK(rig.xfadeStarts()[0] == mark); // 前沿：当块的第一个样本

        std::int64_t wrong = 0;
        for (std::int64_t t = mark - 4; t < mark + length + 4; ++t)
        {
            const std::int64_t k = t - mark; // 第 k 个样本
            float in = 0.0f;
            float out = 1.0f;
            if (k >= length)
            {
                in = 1.0f;
                out = 0.0f;
            }
            else if (k > 0)
            {
                in = static_cast<float>(k) / static_cast<float>(length);
                out = 1.0f - in;
            }
            const float v1 = noiseAt(t - d1);
            const float v2 = noiseAt(t - d2);
            // 增益为 0 的那一路混音里直接跳过（不加 0 乘积），与加一个 0 逐位相同，除了 −0.0 的符号
            float want = 0.0f;
            want += out > 0.0f ? v1 * out : 0.0f;
            want += in > 0.0f ? v2 * in : 0.0f;
            wrong += rig.trace[static_cast<std::size_t>(t)] == want ? 0 : 1;
        }
        CHECK(wrong == 0);
        // 终点之后：只剩新读头，逐位等于它读的值
        for (std::int64_t t = mark + length; t < mark + length + 1000; t += 13)
        {
            CHECK(rig.trace[static_cast<std::size_t>(t)] == noiseAt(t - d2));
        }
        CHECK(rig.status().delayXfadeLeft == 0);
    }
}
