// SPDX-License-Identifier: GPL-3.0-or-later
//
// InputEngine 的电平（Meters 的 inputPeak / returnPeak / outputPeak / dryPartPeak / returnPartPeak）：
//   取点：in 是宿主给的干声，ret 是 ⑥ ×g 之前的回送（音色级之后、mono 时取过平均），out 是最终输出，
//   Mix 推子的两路分量是 g_dry·干声与真正加到干声上的回送；
//   已知幅度的正弦进去，五个峰值在容差内，并与逐样本记录的最大值逐位相等；两次 take 之间的最大值不丢，take 之后清零；
//   静音为 0，非有限样本不计。
//
// 标 ⛔ 的是删除式验证点：把对应的实现改坏，本用例必须变红。
// 开环（见 test_input_tone.cpp）：中间链把送进 Output 的信号换成测试信号，环里只有它。

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "EngineTestKit.h"
#include "core/tone/ToneTestKit.h"
#include "engine/InputEngine.h"
#include "engine/Meters.h"

namespace
{

using namespace rinne::engine;
using rinne::test::ClosedLoop;
using rinne::test::kPi;

constexpr double kSr = 48000.0;
constexpr int kBlock = 480;
// 400 Hz 与 1 kHz 在 48 kHz 下每周期 120 / 48 个样本，四分之一周期恰好落在样本上：采样峰值就是幅度本身
constexpr double kDryHz = 400.0;
constexpr double kFeedHz = 1000.0;
// 1 s 起量 0.1 s；窗口过了一半两路幅度都降到四分之一
constexpr std::int64_t kWindow = 48000;
constexpr std::int64_t kDrop = kWindow + 2400;

float sineAt(std::int64_t t, double hz, double amplitude)
{
    return static_cast<float>(amplitude * std::sin(2.0 * kPi * hz * static_cast<double>(t) / kSr));
}

float maxAbs(const std::vector<float>& v, std::size_t from, std::size_t to)
{
    float worst = 0.0f;
    for (std::size_t i = from; i < to && i < v.size(); ++i)
        worst = std::fmax(worst, std::fabs(v[i]));
    return worst;
}

float maxAbs2(const std::vector<float>& a, const std::vector<float>& b, std::size_t from, std::size_t to)
{
    return std::fmax(maxAbs(a, from, to), maxAbs(b, from, to));
}

struct Taken
{
    float in;
    float ret;
    float out;
    float dry;
    float wet;
};

Taken takeAll(Meters& m)
{
    return {m.inputPeak.take(), m.returnPeak.take(), m.outputPeak.take(), m.dryPartPeak.take(),
            m.returnPartPeak.take()};
}

// 已知幅度：干声 in·sin(400 Hz)（右声道减半），环里 feed·sin(1 kHz)（右声道减半）；窗口后半段两路幅度都降到四分之一，
// 所以窗口里的最大值来自前半段：只发布最后一块（store 而不是 publishMax）的话就对不上。
// 反馈 80 %、mix 75 %：g_dry = 0.5、g_ret = 1，⑥ 的 g = 0.8（淡入早已走完）。
struct Window
{
    ClosedLoop loop;
    std::size_t from;
    std::size_t to;
    Taken taken;

    Window(int channels, float lowCut, float highCut, double dryAmp, double feedAmp) : loop(kSr, kBlock)
    {
        loop.inputChannels = channels;
        loop.reference.enableTone();
        loop.source = [dryAmp](std::int64_t t, int channel) {
            const double a = (t < kDrop ? dryAmp : 0.25 * dryAmp) * (channel == 0 ? 1.0 : 0.5);
            return sineAt(t, kDryHz, a);
        };
        loop.middle = [feedAmp](std::int64_t at, float* left, float* right, int n) {
            for (int i = 0; i < n; ++i)
            {
                const std::int64_t t = at + i;
                const double a = t < kDrop - 480 ? feedAmp : 0.25 * feedAmp; // 回送落后 D = 10 ms
                left[i] = sineAt(t, kFeedHz, a);
                right[i] = sineAt(t, kFeedHz, 0.5 * a);
            }
        };
        EngineParams p;
        p.feedbackPercent = 80.0f;
        p.mixPercent = 75.0f;
        p.delayMs = 10.0f;
        p.lowCutHz = lowCut;
        p.highCutHz = highCut;
        loop.run(p, kBlock, kWindow);
        takeAll(loop.input.meters()); // 丢掉窗口之前的
        from = static_cast<std::size_t>(loop.at);
        loop.run(p, kBlock, 4800);
        to = static_cast<std::size_t>(loop.at);
        taken = takeAll(loop.input.meters());
    }
};

bool near(double value, double expected, double relative)
{
    return std::fabs(value - expected) <= relative * expected;
}

} // namespace

TEST_CASE("Input 电平：已知幅度的正弦进去，in / ret / out 与 Mix 推子两路分量的峰值在容差内，并与逐样本记录逐位相等",
          "[engine][input][meters]")
{
    // ⛔ 删除式验证点：电平不写 → 全部是 0，本用例变红；只 store 最后一块 → 窗口后半段幅度降了，对不上，变红；
    //    ret 改取读头（① 之后、② 之前，即 blockPeak）→ 开了高切的那一格对不上，变红。
    SECTION("立体声，音色级关")
    {
        Window w(2, 20.0f, 20000.0f, 0.4, 0.3);
        const auto& ref = w.loop.reference;
        CHECK(w.taken.in == maxAbs2(w.loop.dryLeft, w.loop.dryRight, w.from, w.to));
        CHECK(w.taken.out == maxAbs2(w.loop.outLeft, w.loop.outRight, w.from, w.to));
        CHECK(w.taken.ret == maxAbs2(ref.preLeft, ref.preRight, w.from, w.to));
        CHECK(w.taken.wet == maxAbs2(ref.retLeft, ref.retRight, w.from, w.to));
        CHECK(w.taken.dry == 0.5f * w.taken.in); // g_dry = 0.5 是 2 的幂，乘法没有舍入
        // 容差：采样峰值恰在正弦峰上；DC 阻断在 1 kHz 差不到 0.001 dB
        CHECK(near(w.taken.in, 0.4, 1.0e-6));
        CHECK(near(w.taken.ret, 0.3, 0.01));
        CHECK(near(w.taken.wet, 0.24, 0.01));
        // out 是两路逐样本之和的峰值：不超过两路峰值之和（舍入单调，float 里也成立）
        CHECK(w.taken.out <= w.taken.dry + w.taken.wet);
        CHECK(w.taken.out > 0.0f);
        // take 之后清零
        const Taken again = takeAll(w.loop.input.meters());
        CHECK(again.in == 0.0f);
        CHECK(again.ret == 0.0f);
        CHECK(again.out == 0.0f);
        CHECK(again.dry == 0.0f);
        CHECK(again.wet == 0.0f);
        CHECK(w.loop.state() != SafetyState::Tripped);
    }
    SECTION("立体声，高切 2 kHz：ret 取在音色级之后，读头峰值照旧在它之前")
    {
        Window w(2, 20.0f, 2000.0f, 0.4, 0.3);
        const double lowPass = rinne::test::butterworthGain(false, kFeedHz, 2000.0, kSr); // 约 0.970
        CHECK(w.taken.ret == maxAbs2(w.loop.reference.preLeft, w.loop.reference.preRight, w.from, w.to));
        CHECK(near(w.taken.ret, 0.3 * lowPass, 0.01));
        CHECK(near(w.taken.wet, 0.24 * lowPass, 0.01));
        // 读头（① 之后、② 之前）的峰值不经音色级：本会话最大仍是 0.3
        CHECK(std::fabs(w.loop.input.safetyDiag().tailPeakMaxDb - 20.0f * std::log10(0.3f)) < 0.05f);
        CHECK_FALSE(w.loop.input.meters().highCutLimited.load());
    }
    SECTION("mono：ret 是两格的平均")
    {
        // 环里左 0.3、右 0.15（同相），平均 0.225；干声只有左声道
        Window w(1, 20.0f, 20000.0f, 0.4, 0.3);
        CHECK(w.taken.in == maxAbs(w.loop.dryLeft, w.from, w.to));
        CHECK(w.taken.out == maxAbs(w.loop.outLeft, w.from, w.to));
        CHECK(w.taken.ret == maxAbs(w.loop.reference.preLeft, w.from, w.to));
        CHECK(w.taken.wet == maxAbs(w.loop.reference.retLeft, w.from, w.to));
        CHECK(near(w.taken.in, 0.4, 1.0e-6));
        CHECK(near(w.taken.ret, 0.225, 0.01));
        CHECK(near(w.taken.wet, 0.18, 0.01));
        CHECK(w.taken.dry == 0.5f * w.taken.in);
    }
}

TEST_CASE("Input 电平：静音为 0，非有限样本（NaN、±Inf）不计", "[engine][input][meters]")
{
    // 没有对端：回送恒为 0。干声分别是全 0、只有非有限值与 −0.0、非有限值夹着 0.5
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    struct Case
    {
        std::vector<float> dry;
        float expected;
    };
    const Case cases[] = {
        {std::vector<float>(256, 0.0f), 0.0f},
        {{nan, -inf, inf, -0.0f, nan}, 0.0f},
        {{nan, 0.25f, inf, -0.5f, -inf, 0.125f}, 0.5f},
    };
    for (const Case& k : cases)
    {
        InputEngine input;
        input.prepare(kSr, 256);
        std::vector<float> left = k.dry;
        std::vector<float> right = k.dry;
        float* channels[2] = {left.data(), right.data()};
        AudioView view;
        view.channels = channels;
        view.numChannels = 2;
        view.length = static_cast<int>(left.size());
        EngineParams p;
        p.mixPercent = 75.0f; // g_dry = 0.5：干声分量也走一遍
        input.process(view, p, TransportInfo{}, false, EngineContext{});
        const Taken t = takeAll(input.meters());
        INFO("expected = " << k.expected);
        CHECK(t.in == k.expected);
        CHECK(t.dry == 0.5f * k.expected);
        CHECK(t.out == 0.5f * k.expected);
        CHECK(t.ret == 0.0f);
        CHECK(t.wet == 0.0f);
    }
}
