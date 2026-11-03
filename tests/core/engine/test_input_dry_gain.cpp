// SPDX-License-Identifier: GPL-3.0-or-later
//
// InputEngine 的干声增益 g_dry：
//   - mix 改变时逐样本线性走 30 ms（kMixSmoothSeconds），终点精确，相邻样本之差不超过一步，与宿主切块无关；
//   - 宿主旁路这一端时目标是 1（不论 mix），同一个平滑器 30 ms 滑回去，到 1 之后干声逐位直通；
//     取消旁路后滑回按 mix 的值；
//   - mix 为 NaN 按 0（干声照常、回送为 0），负数与越界钳回；
//   - 第一次 prepare 与换采样率之后的第一块直接落定，同采样率重新 prepare 不动平滑器。
//
// 标 ⛔ 的是删除式验证点：把对应的实现改坏，本用例必须变红。
// 只看 g_dry 的用例用「没有对端、喂常数 1.0」的单个引擎：回送恒为 0，输出就是每个样本的 g_dry。
// 整条链的用例用 EngineTestKit.h 的闭环，输出与参照逐位比较（参照里的 g_dry 同样逐样本平滑）。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "EngineTestKit.h"
#include "core/dsp/DspTestKit.h"
#include "engine/InputEngine.h"

namespace
{

using namespace rinne::engine;
using rinne::test::ClosedLoop;
using rinne::test::firstBitDiff;
using rinne::test::referenceDryGain;
using rinne::test::Rng;

constexpr double kSr = 48000.0;
constexpr double kPi = rinne::test::kPi;

// 30 ms 在 48 kHz 下的样本数
constexpr int kRampSamples = 1440;

float cosine(std::int64_t t, double hz, float amplitude)
{
    return amplitude * static_cast<float>(std::cos(2.0 * kPi * hz * static_cast<double>(t) / kSr));
}

// 特殊值：−0.0、带载荷的 quiet NaN、signaling NaN、负号 NaN、denormal、±Inf，夹在普通样本之间。
float special(std::int64_t t, int channel)
{
    switch ((t + channel * 3) % 9)
    {
    case 0:
        return -0.0f;
    case 1:
        return rinne::test::fromBits(0x7fc12345u);
    case 2:
        // signaling NaN，载荷随 t 变：位型在运行时才定（写成常量的话 MSVC 会在编译期折成 quiet NaN）
        return rinne::test::fromBits(0x7f800000u | (1u + (static_cast<std::uint32_t>(t) & 0xffu)));
    case 3:
        return rinne::test::fromBits(0xffc00abcu);
    case 4:
        return 1.0e-40f;
    case 5:
        return std::numeric_limits<float>::infinity();
    case 6:
        return -0.25f;
    default:
        return cosine(t, 220.0, 0.3f);
    }
}

bool isSignalingNan(float x)
{
    std::uint32_t b = 0;
    std::memcpy(&b, &x, sizeof b);
    return (b & 0x7fc00000u) == 0x7f800000u && (b & 0x003fffffu) != 0u;
}

EngineParams loopParams(float feedbackPercent, float mixPercent, float delayMs)
{
    EngineParams p;
    p.feedbackPercent = feedbackPercent;
    p.mixPercent = mixPercent;
    p.delayMs = delayMs;
    return p;
}

// 单个引擎、没有环（没有对端，回送恒为 0）、喂常数 1.0：输出的左声道就是每个样本的 g_dry。
struct DryProbe
{
    explicit DryProbe(double rate = kSr) { input.prepare(rate, 4096); }

    // 跑 total 个样本，每块 blockLength 个（最后一块可短）；追加到 gain
    void run(const EngineParams& params, bool bypassed, int total, int blockLength)
    {
        for (int done = 0; done < total; done += blockLength)
        {
            const int n = std::min(blockLength, total - done);
            std::vector<float> left(static_cast<std::size_t>(n), 1.0f);
            std::vector<float> right(static_cast<std::size_t>(n), 1.0f);
            float* channels[2] = {left.data(), right.data()};
            AudioView view;
            view.channels = channels;
            view.numChannels = 2;
            view.length = n;
            input.process(view, params, TransportInfo{}, bypassed, EngineContext{});
            gain.insert(gain.end(), left.begin(), left.end());
        }
    }

    InputEngine input;
    std::vector<float> gain;
};

// [from, to) 里每个样本都恰好等于 value
bool allEqual(const std::vector<float>& trace, std::size_t from, std::size_t to, float value)
{
    for (std::size_t i = from; i < to; ++i)
    {
        if (trace[i] != value)
        {
            return false;
        }
    }
    return true;
}

// 检查 trace 从 index start 起是一段从 from 到 to 的 30 ms 线性斜坡：相邻样本（含斜坡前的最后一个）之差不超过一步、
// 单调、第 kRampSamples 步（下标 start + kRampSamples − 1）恰好等于 to、中点在直线上、之后一直恰好等于 to。
void checkRamp(const std::vector<float>& trace, std::size_t start, float from, float to)
{
    REQUIRE(trace.size() >= start + kRampSamples + 16);
    const float step = std::fabs(to - from) / static_cast<float>(kRampSamples);
    CHECK(trace[start - 1] == from);
    float previous = from;
    float largest = 0.0f;
    bool monotone = true;
    for (std::size_t i = start; i < start + kRampSamples; ++i)
    {
        largest = std::max(largest, std::fabs(trace[i] - previous));
        monotone = monotone && (to >= from ? trace[i] >= previous : trace[i] <= previous);
        previous = trace[i];
    }
    CHECK(largest <= step * 1.02f); // float 取整最多差半个 ulp，窄斜坡上占一两个千分点
    CHECK(largest >= step * 0.98f); // 每一步都在走，不是停一阵再跳
    CHECK(monotone);
    // 第一步已经离开起点，最后一步之前还没到终点
    CHECK(trace[start] != from);
    CHECK(trace[start + kRampSamples - 2] != to);
    // 30 ms 后精确到目标，之后不再动
    CHECK(trace[start + kRampSamples - 1] == to);
    CHECK(allEqual(trace, start + kRampSamples - 1, trace.size(), to));
    // 中点在两端连线上
    const float middle = from + (to - from) * 0.5f;
    CHECK(std::fabs(trace[start + kRampSamples / 2 - 1] - middle) <= step);
}

} // namespace

TEST_CASE("Input 干声增益：mix 跳变时逐样本线性走 30 ms，相邻样本之差不超过一步，30 ms 后精确到目标",
          "[engine][input][drygain]")
{
    // ⛔ 删除式验证点：g_dry 退回按块赋值（块首直接取目标值）→ 块边界上一步从起点跳到终点，相邻样本之差远超一步，
    //    斜坡检查变红；平滑器不在逐样本的循环里推进（改成每块推进一步）→ 30 ms 内走不完，终点检查变红。
    // 起点与终点用独立写出的混合律算（referenceDryGain），不调引擎里的函数。
    struct Jump
    {
        float from;
        float to;
    };
    const Jump jumps[] = {{50.0f, 100.0f}, {100.0f, 25.0f}, {25.0f, 100.0f},
                          {100.0f, 60.0f}, {80.0f, 10.0f},  {60.0f, 0.0f}};
    for (const Jump& jump : jumps)
    {
        CAPTURE(jump.from, jump.to);
        const float gFrom = referenceDryGain(jump.from);
        const float gTo = referenceDryGain(jump.to);
        // 块长 100 不整除 1440：斜坡中途会跨过块边界；块首恰好是斜坡的第一个样本
        for (const int blockLength : {100, 480, 4096})
        {
            CAPTURE(blockLength);
            DryProbe probe;
            probe.run(loopParams(0.0f, jump.from, 10.0f), false, 4800, blockLength);
            // 起点：第一块直接落定，之后一直是 g_dry(from)
            REQUIRE(allEqual(probe.gain, 0, probe.gain.size(), gFrom));
            const std::size_t start = probe.gain.size();
            probe.run(loopParams(0.0f, jump.to, 10.0f), false, 4800, blockLength);
            if (gFrom == gTo)
            {
                CHECK(allEqual(probe.gain, start, probe.gain.size(), gTo));
            }
            else
            {
                checkRamp(probe.gain, start, gFrom, gTo);
            }
            CHECK(probe.input.dryGain() == gTo);
        }
    }
}

TEST_CASE("Input 干声增益：斜坡走到 1 之后恢复逐位快速路径（含 −0.0 与 NaN 载荷）", "[engine][input][drygain]")
{
    // ⛔ 删除式验证点：g_dry 为 1 时仍旧乘一遍（去掉 dryIsUnity 的快速路径）→ signaling NaN 被改成 quiet NaN，
    //    本用例变红。
    DryProbe probe;
    EngineParams p = loopParams(0.0f, 100.0f, 10.0f);
    std::vector<float> in;
    std::vector<float> out;
    const auto runSpecial = [&](const EngineParams& params, int total) {
        for (int done = 0; done < total; done += 256)
        {
            std::vector<float> left(256);
            std::vector<float> right(256);
            for (int i = 0; i < 256; ++i)
            {
                left[static_cast<std::size_t>(i)] = special(static_cast<std::int64_t>(in.size()) + i, 0);
                right[static_cast<std::size_t>(i)] = special(static_cast<std::int64_t>(in.size()) + i, 1);
            }
            in.insert(in.end(), left.begin(), left.end());
            float* channels[2] = {left.data(), right.data()};
            AudioView view;
            view.channels = channels;
            view.numChannels = 2;
            view.length = 256;
            probe.input.process(view, params, TransportInfo{}, false, EngineContext{});
            out.insert(out.end(), left.begin(), left.end());
        }
    };
    runSpecial(p, 4096); // g_dry = 0
    const std::size_t jumpAt = in.size();
    p.mixPercent = 25.0f; // 目标回到 1
    runSpecial(p, 8192);
    // 斜坡 1440 个样本走完（块边界对齐不影响：逐样本）之后逐位直通
    const std::size_t from = jumpAt + kRampSamples;
    REQUIRE(out.size() > from + 256);
    std::size_t signaling = 0;
    for (std::size_t i = from; i < out.size(); ++i)
    {
        signaling += isSignalingNan(out[i]) ? 1u : 0u;
    }
    CHECK(signaling > 0u);
    CHECK(std::memcmp(in.data() + from, out.data() + from, (out.size() - from) * sizeof(float)) == 0);
    // 斜坡之内确实不是直通（不是空断言）
    CHECK(std::memcmp(in.data() + jumpAt, out.data() + jumpAt, kRampSamples * sizeof(float)) != 0);
}

TEST_CASE("Input 干声增益：mix 变化途中整块与随机切分逐位相同，且都与参照一致", "[engine][input][drygain]")
{
    // ⛔ 删除式验证点：g_dry 退回按块赋值 → 跳变落在块边界的位置随切分而变，整块与随机切分对不上，也与参照对不上，
    //    本用例变红；平滑器的进度按块（而不是按样本）推进 → 同上。
    // 分段长度故意有短于 30 ms 的（700、1000 个样本）：目标在斜坡中途又变。首个有效读样本在 1000 ms 处，
    // 前两段 60000 个样本之后回送已经在淡入、持续响着，mix 的跳变因此和回送增益一起走。
    struct Segment
    {
        float mix;
        int length;
    };
    // 每段不超过 32768 个样本（整块一次交给引擎，超过就是环路不走的超长块）
    const Segment plan[] = {{50.0f, 30000}, {50.0f, 30000}, {100.0f, 700},   {25.0f, 5000}, {75.0f, 1000}, {0.0f, 3000},
                            {100.0f, 9000}, {40.0f, 2500},  {100.0f, 20000}, {60.0f, 700},  {55.0f, 7000}};
    // D = 1000 ms：大于任何一块的长度，块长不抬 D 的下限，三种切法的环路延迟相同
    const auto source = [](std::int64_t t, int channel) {
        return cosine(t, channel == 0 ? 220.0 : 330.0, 0.3f) + (t % 997 == 0 ? 0.2f : 0.0f);
    };
    struct Result
    {
        std::vector<float> left;
        std::vector<float> right;
        std::vector<float> dry;
    };
    const auto runWith = [&](int maxPiece, std::uint64_t seed) {
        ClosedLoop loop(kSr, 32768);
        loop.source = source;
        Rng rng(seed);
        for (const Segment& segment : plan)
        {
            EngineParams p = loopParams(80.0f, segment.mix, 1000.0f);
            int left = segment.length;
            while (left > 0)
            {
                const int n = maxPiece <= 0 ? left : std::min(left, 1 + rng.below(maxPiece));
                loop.block(p, n);
                left -= n;
            }
        }
        CHECK(firstBitDiff(loop.outLeft, loop.reference.wantLeft) == -1);
        CHECK(firstBitDiff(loop.outRight, loop.reference.wantRight) == -1);
        CHECK(loop.input.loopStatus().resyncCount == 0u);
        CHECK(firstBitDiff(loop.outLeft, loop.dryLeft) != -1); // 回送确实在响（不是空断言）
        return Result{loop.outLeft, loop.outRight, loop.dryLeft};
    };
    const Result whole = runWith(0, 1);
    const Result tiny = runWith(37, 2);
    const Result mixed = runWith(2048, 3);
    REQUIRE(whole.left.size() == tiny.left.size());
    REQUIRE(whole.left.size() == mixed.left.size());
    CHECK(firstBitDiff(whole.left, tiny.left) == -1);
    CHECK(firstBitDiff(whole.right, tiny.right) == -1);
    CHECK(firstBitDiff(whole.left, mixed.left) == -1);
    CHECK(firstBitDiff(whole.right, mixed.right) == -1);
}

TEST_CASE("Input 干声增益：宿主旁路时目标是 1，30 ms 滑回去，取消旁路后 30 ms 滑回按 mix 的值",
          "[engine][input][drygain]")
{
    // ⛔ 删除式验证点：旁路时干声目标不改成 1（仍按 mix）→ mix 100 % 时旁路后整轨无声、80 % 时只剩 0.4 倍，
    //    「旁路后 30 ms 逐位直通」变红；旁路时 g_dry 直接跳到 1（不走平滑器）→ 斜坡检查变红。
    for (const float mix : {100.0f, 80.0f, 60.0f})
    {
        CAPTURE(mix);
        const float gMix = referenceDryGain(mix);
        REQUIRE(gMix < 1.0f);
        DryProbe probe;
        const EngineParams p = loopParams(0.0f, mix, 10.0f);
        probe.run(p, false, 4800, 480);
        const std::size_t bypassAt = probe.gain.size();
        probe.run(p, true, 4800, 100);
        const std::size_t releaseAt = probe.gain.size();
        probe.run(p, false, 4800, 480);
        // 旁路：g_mix → 1，之后恰为 1
        const std::vector<float> bypassPart(probe.gain.begin(),
                                            probe.gain.begin() + static_cast<std::ptrdiff_t>(releaseAt));
        checkRamp(bypassPart, bypassAt, gMix, 1.0f);
        // 取消旁路：1 → g_mix
        checkRamp(probe.gain, releaseAt, 1.0f, gMix);
    }
}

TEST_CASE(
    "Input 旁路：mix 100 % 与 80 % 下 30 ms 之后输出与输入逐位相同（含 −0.0 与 NaN 载荷），取消旁路后回送与 mix 回来",
    "[engine][input][drygain]")
{
    // ⛔ 删除式验证点：同上（旁路时干声目标仍按 mix）→ 旁路后输出是乘过 g_dry 的干声，逐位对不上，本用例变红。
    for (const float mix : {100.0f, 80.0f, 60.0f})
    {
        CAPTURE(mix);
        ClosedLoop loop(kSr, 256);
        static constexpr std::int64_t kSwitch = 188 * 256; // 块边界，约 1 s：回送早已在响
        loop.source = [](std::int64_t t, int channel) {
            return t < kSwitch ? cosine(t, 100.0 + 50.0 * channel, 0.3f) : special(t, channel);
        };
        const EngineParams p = loopParams(80.0f, mix, 10.0f);
        loop.run(p, 256, kSwitch);
        REQUIRE(loop.at == kSwitch);
        CHECK(firstBitDiff(loop.outLeft, loop.dryLeft) != -1); // 旁路之前回送确实在响

        loop.inputBypassed = true;
        loop.run(p, 256, 16384);
        // 回送 10 ms 淡出、干声 30 ms 回到 1：第 1440 个样本起逐位相同
        const auto from = static_cast<std::size_t>(kSwitch + kRampSamples);
        REQUIRE(loop.outLeft.size() > from + 256);
        CHECK(std::memcmp(loop.outLeft.data() + from, loop.dryLeft.data() + from,
                          (loop.outLeft.size() - from) * sizeof(float)) == 0);
        CHECK(std::memcmp(loop.outRight.data() + from, loop.dryRight.data() + from,
                          (loop.outRight.size() - from) * sizeof(float)) == 0);
        std::size_t signaling = 0; // signaling NaN 原样留在输出里
        for (std::size_t t = from; t < loop.outLeft.size(); ++t)
        {
            signaling += isSignalingNan(loop.outLeft[t]) ? 1u : 0u;
        }
        CHECK(signaling > 0u);
        CHECK(loop.state() != SafetyState::Tripped);
        // 斜坡之内不是直通（不是空断言）
        CHECK(firstBitDiff(loop.outLeft, loop.dryLeft) != -1);
        CHECK(firstBitDiff(loop.outLeft, loop.dryLeft) < static_cast<long long>(from));

        // 取消旁路：本地延迟线没清，回送在 10 ms 里回来，干声 30 ms 滑回按 mix 的值；与参照逐位一致
        const std::size_t releaseAt = loop.outLeft.size();
        loop.inputBypassed = false;
        loop.run(p, 256, 16384);
        CHECK(firstBitDiff(loop.outLeft, loop.reference.wantLeft) == -1);
        CHECK(firstBitDiff(loop.outRight, loop.reference.wantRight) == -1);
        std::size_t differing = 0;
        for (std::size_t t = releaseAt + 2048; t < loop.outLeft.size(); ++t)
        {
            differing += std::memcmp(&loop.outLeft[t], &loop.dryLeft[t], sizeof(float)) != 0 ? 1u : 0u;
        }
        CHECK(differing > 0u);
        // 参照里每个样本的 g_dry：旁路时走向 1，取消后走回 g_mix
        const auto& g = loop.reference.gDryTrace;
        CHECK(g[releaseAt - 1] == 1.0f);
        CHECK(g[loop.outLeft.size() - 1] == referenceDryGain(mix));
    }
}

TEST_CASE("Input mix 为 NaN 按 0：回送为 0、干声逐位直通；负数钳到 0，大于 100 钳到 100", "[engine][input][drygain]")
{
    // ⛔ 删除式验证点：引擎里退回本地的混合律（std::clamp 遇到 NaN 原样返回 NaN，回送增益落到「g < 1 为假 → 1.0」）
    //    → NaN 时回送是最大的 1.0，输出与干声不再逐位相同，增益断言也对不上，本用例变红。
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const float negativeNan = rinne::test::fromBits(0xffc00001u);
    const float signalingNan = rinne::test::fromBits(0x7f800001u);

    SECTION("闭环：整段（含 500 ms 淡入）回送恒为 0")
    {
        for (const float mix : {nan, negativeNan, signalingNan, -5.0f, -inf})
        {
            CAPTURE(mix);
            ClosedLoop loop(kSr, 256);
            loop.source = special;
            // 反馈 100 %、D = 10 ms：mix 若按 50 % 以上算，回送早已在响
            loop.run(loopParams(100.0f, mix, 10.0f), 256, 48000);
            CHECK(firstBitDiff(loop.outLeft, loop.dryLeft) == -1);
            CHECK(firstBitDiff(loop.outRight, loop.dryRight) == -1);
            CHECK(firstBitDiff(loop.outLeft, loop.reference.wantLeft) == -1);
            CHECK(loop.input.dryGain() == 1.0f);
            CHECK(loop.input.returnGain() == 0.0f);
            CHECK(loop.state() != SafetyState::Tripped);
        }
        // 对照：同一条闭环、mix 50 %，回送确实在响
        ClosedLoop control(kSr, 256);
        control.source = [](std::int64_t t, int) { return cosine(t, 100.0, 0.3f); };
        control.run(loopParams(100.0f, 50.0f, 10.0f), 256, 48000);
        CHECK(firstBitDiff(control.outLeft, control.dryLeft) != -1);
    }
    SECTION("增益：NaN 与负数 → g_dry 1、g_ret 0；+Inf 与大于 100 → g_dry 0、g_ret 1")
    {
        struct Case
        {
            float mix;
            float dry;
            float ret;
        };
        const Case cases[] = {{nan, 1.0f, 0.0f},   {negativeNan, 1.0f, 0.0f}, {signalingNan, 1.0f, 0.0f},
                              {-5.0f, 1.0f, 0.0f}, {-inf, 1.0f, 0.0f},        {250.0f, 0.0f, 1.0f},
                              {inf, 0.0f, 1.0f}};
        for (const Case& c : cases)
        {
            CAPTURE(c.mix);
            DryProbe probe;
            probe.run(loopParams(100.0f, c.mix, 10.0f), false, 64, 64);
            CHECK(probe.input.dryGain() == c.dry);
            CHECK(probe.input.returnGain() == c.ret);
        }
    }
}

TEST_CASE("Input 干声增益：第一次 prepare 与换采样率之后的第一块直接落定，同采样率重新 prepare 不动平滑器",
          "[engine][input][drygain]")
{
    // ⛔ 删除式验证点：prepare 之后的第一块不落定（从初值 1 滑向目标）→ 第一块开头不是目标值，「首次」一节变红；
    //    同采样率重新 prepare 也落定 → 斜坡被截断，「同采样率」一节变红；换采样率不落定 → 「换采样率」一节变红。
    SECTION("首次：第一块从第一个样本起就是目标")
    {
        DryProbe probe;
        probe.run(loopParams(0.0f, 100.0f, 10.0f), false, 4800, 480);
        CHECK(allEqual(probe.gain, 0, probe.gain.size(), 0.0f));
        // 第一块是旁路块：目标是 1，同样直接落定
        DryProbe bypassed;
        bypassed.run(loopParams(0.0f, 100.0f, 10.0f), true, 480, 480);
        CHECK(allEqual(bypassed.gain, 0, bypassed.gain.size(), 1.0f));
    }
    SECTION("同采样率重新 prepare：进行中的斜坡接着走，与不 prepare 的一遍逐位相同")
    {
        DryProbe straight;
        straight.run(loopParams(0.0f, 50.0f, 10.0f), false, 2400, 480);
        straight.run(loopParams(0.0f, 100.0f, 10.0f), false, 4800, 480);

        DryProbe again;
        again.run(loopParams(0.0f, 50.0f, 10.0f), false, 2400, 480);
        again.run(loopParams(0.0f, 100.0f, 10.0f), false, 480, 480); // 斜坡走了三分之一
        again.input.prepare(kSr, 4096);
        again.run(loopParams(0.0f, 100.0f, 10.0f), false, 4320, 480);
        REQUIRE(again.gain.size() == straight.gain.size());
        CHECK(firstBitDiff(again.gain, straight.gain) == -1);
        CHECK(again.gain[2400 + 479] != 0.0f); // 重 prepare 时斜坡确实还没走完
    }
    SECTION("换采样率：斜坡长度按新的采样率重算，第一块直接落定")
    {
        DryProbe probe;
        probe.run(loopParams(0.0f, 50.0f, 10.0f), false, 2400, 480);
        probe.run(loopParams(0.0f, 100.0f, 10.0f), false, 480, 480); // 斜坡进行中
        REQUIRE(probe.gain.back() != 0.0f);
        probe.input.prepare(44100.0, 4096);
        const std::size_t at = probe.gain.size();
        probe.run(loopParams(0.0f, 100.0f, 10.0f), false, 480, 480);
        CHECK(allEqual(probe.gain, at, probe.gain.size(), 0.0f));
        // 之后的跳变按 44.1 kHz 的 30 ms = 1323 个样本走
        const std::size_t jump = probe.gain.size();
        probe.run(loopParams(0.0f, 25.0f, 10.0f), false, 4410, 441);
        const std::size_t ramp = static_cast<std::size_t>(std::llround(0.030 * 44100.0));
        CHECK(probe.gain[jump + ramp - 1] == 1.0f);
        CHECK(probe.gain[jump + ramp - 2] != 1.0f);
    }
}
