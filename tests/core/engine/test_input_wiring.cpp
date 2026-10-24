// SPDX-License-Identifier: GPL-3.0-or-later
//
// InputEngine 接上安全链之后的接线：逐位直通（验收 5）、与宿主怎么切块逐位无关（安全不变量 7）、
// 命令序号（首块之前的紧急切断、基线不重放）、接线次序与参照安全链逐位一致、环内 NaN 的计数与琥珀灯、
// 读环之后的清洗在 ×g 之前（环里的坏样本不跳闸）。
//
// 标 ⛔ 的是删除式验证点：把对应的实现改坏，本用例必须变红。
// 环与会合段都在普通内存里（见 EngineTestKit.h 的 ClosedLoop）。

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "EngineTestKit.h"
#include "core/dsp/DspTestKit.h"
#include "diag/EventQueue.h"
#include "dsp/Sanitize.h"
#include "engine/InputEngine.h"
#include "engine/SafetyEvents.h"
#include "ipc/LoopBinding.h"

namespace
{

using namespace rinne::engine;
using rinne::test::ClosedLoop;
using rinne::test::firstBitDiff;

constexpr double kSr = 48000.0;
constexpr double kPi = rinne::test::kPi;

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
        return rinne::test::fromBits(0x7fc12345u); // quiet NaN，载荷 0x412345
    case 2:
        // signaling NaN，载荷随 t 变：位型在运行时才定。写成常量的话 MSVC 会在编译期把它折成 quiet NaN，
        // 这一格就从没以 signaling NaN 喂进过引擎（下面的用例断言它确实是 signaling NaN）。
        return rinne::test::fromBits(0x7f800000u | (1u + (static_cast<std::uint32_t>(t) & 0xffu)));
    case 3:
        return rinne::test::fromBits(0xffc00abcu); // 负号 NaN
    case 4:
        return 1.0e-40f; // denormal
    case 5:
        return std::numeric_limits<float>::infinity();
    case 6:
        return -0.25f;
    default:
        return cosine(t, 220.0, 0.3f);
    }
}

EngineParams loopParams(float feedbackPercent, float mixPercent, float delayMs)
{
    EngineParams p;
    p.feedbackPercent = feedbackPercent;
    p.mixPercent = mixPercent;
    p.delayMs = delayMs;
    return p;
}

// 从 from 起两串逐位相同
bool sameBitsFrom(const std::vector<float>& a, const std::vector<float>& b, std::size_t from)
{
    if (a.size() != b.size() || from > a.size())
    {
        return false;
    }
    return std::memcmp(a.data() + from, b.data() + from, (a.size() - from) * sizeof(float)) == 0;
}

// 位型是 signaling NaN（指数全 1、quiet 位为 0、尾数不为 0）
bool isSignalingNan(float x)
{
    std::uint32_t b = 0;
    std::memcpy(&b, &x, sizeof b);
    return (b & 0x7fc00000u) == 0x7f800000u && (b & 0x003fffffu) != 0u;
}

// 淡入请求的观察者：记下每一次的原因
struct FadeLog
{
    std::vector<FadeReason> reasons;
    static void record(void* context, FadeReason reason) noexcept
    {
        static_cast<FadeLog*>(context)->reasons.push_back(reason);
    }
};

} // namespace

TEST_CASE("Input 逐位直通：反馈 0、mix 0、没有对端、loop_on 关与 Input 旁路 10 ms 之后，输出逐位等于输入（含 −0.0 与 "
          "NaN 载荷）",
          "[engine][input]")
{
    // ⛔ 删除式验证点：回送为 ±0 时照样写 out = 干声 + 回送 → −0.0 变成 +0.0、signaling NaN 被改成 quiet NaN，
    //    本用例变红。
    SECTION("反馈 0：闭环照常跑，回送增益恰为 0")
    {
        ClosedLoop loop(kSr, 256);
        loop.source = special;
        loop.run(loopParams(0.0f, 25.0f, 10.0f), 256, 48000);
        REQUIRE(isSignalingNan(loop.dryLeft[2])); // signaling NaN 确实喂进去了
        CHECK(isSignalingNan(loop.outLeft[2]));
        CHECK(firstBitDiff(loop.outLeft, loop.dryLeft) == -1);
        CHECK(firstBitDiff(loop.outRight, loop.dryRight) == -1);
        CHECK(firstBitDiff(loop.outLeft, loop.reference.wantLeft) == -1);
        CHECK(firstBitDiff(loop.outRight, loop.reference.wantRight) == -1);
    }
    SECTION("mix 0：g_ret = 0、g_dry = 1")
    {
        ClosedLoop loop(kSr, 256);
        loop.source = special;
        loop.run(loopParams(100.0f, 0.0f, 10.0f), 256, 48000);
        CHECK(firstBitDiff(loop.outLeft, loop.dryLeft) == -1);
        CHECK(firstBitDiff(loop.outRight, loop.dryRight) == -1);
    }
    SECTION("没有对端：环上没有写方")
    {
        InputEngine input;
        input.prepare(kSr, 256);
        const EngineParams p = loopParams(100.0f, 50.0f, 10.0f);
        std::vector<float> out;
        std::vector<float> dry;
        for (std::int64_t at = 0; at < 48000; at += 256)
        {
            std::vector<float> left(256);
            std::vector<float> right(256);
            for (int i = 0; i < 256; ++i)
            {
                left[static_cast<std::size_t>(i)] = special(at + i, 0);
                right[static_cast<std::size_t>(i)] = special(at + i, 1);
            }
            dry.insert(dry.end(), left.begin(), left.end());
            float* channels[2] = {left.data(), right.data()};
            AudioView view;
            view.channels = channels;
            view.numChannels = 2;
            view.length = 256;
            input.process(view, p, TransportInfo{}, false, EngineContext{});
            out.insert(out.end(), left.begin(), left.end());
        }
        CHECK(firstBitDiff(out, dry) == -1);
        CHECK(PackedSafety::state(input.meters().safety.load()) == SafetyState::Unpaired);
    }
    SECTION("loop_on 关 / Input 旁路：1 块 + 10 ms 之后")
    {
        for (const bool viaBypass : {false, true})
        {
            ClosedLoop loop(kSr, 256);
            static constexpr std::int64_t kSwitch = 48000; // 块边界
            loop.source = [](std::int64_t t, int channel) {
                return t < kSwitch ? cosine(t, 100.0 + 50.0 * channel, 0.3f) : special(t, channel);
            };
            EngineParams p = loopParams(80.0f, 50.0f, 10.0f);
            loop.run(p, 256, kSwitch);
            // 关之前回送确实在响（不是空断言）
            CHECK(firstBitDiff(loop.outLeft, loop.dryLeft) != -1);
            if (viaBypass)
            {
                loop.inputBypassed = true;
            }
            else
            {
                p.loopOn = false;
            }
            loop.run(p, 256, 48000);
            const auto from = static_cast<std::size_t>(kSwitch + 256 + 480);
            CHECK(sameBitsFrom(loop.outLeft, loop.dryLeft, from));
            CHECK(sameBitsFrom(loop.outRight, loop.dryRight, from));
            std::size_t signaling = 0; // signaling NaN 原样留在输出里（不是被改成 quiet NaN 之后两边恰好一样）
            for (std::size_t t = from; t < loop.outLeft.size(); ++t)
            {
                signaling += isSignalingNan(loop.outLeft[t]) ? 1u : 0u;
            }
            CHECK(signaling > 0u);
            CHECK(firstBitDiff(loop.outLeft, loop.reference.wantLeft) == -1);
            CHECK(firstBitDiff(loop.outRight, loop.reference.wantRight) == -1);
        }
    }
}

TEST_CASE("Input 不变量 7：参数恒定时输出与宿主怎么切块逐位无关（含跨 8192 与 32768 的块）", "[engine][input]")
{
    // ⛔ 删除式验证点：有效位按块判（整块都用块首那个样本的有效位）→ 淡入的起点随切块漂移，本用例变红。
    // D = 1000 ms（大于最大块长 32768，块长不抬 D 的下限）；声明的最大块长三种切法都是 32768。
    const EngineParams p = loopParams(80.0f, 50.0f, 1000.0f);
    constexpr std::int64_t kTotal = 4 * 48000; // 首个有效读样本在 48000，500 ms 淡入，再绕两圈
    const auto source = [](std::int64_t t, int channel) {
        const float tone = cosine(t, channel == 0 ? 220.0 : 330.0, 0.3f);
        return tone + (t % 997 == 0 ? 0.2f : 0.0f);
    };
    const auto runWith = [&](const std::vector<int>& lengths) {
        ClosedLoop loop(kSr, 32768);
        loop.source = source;
        for (std::size_t k = 0; loop.at < kTotal; ++k)
        {
            const std::int64_t left = kTotal - loop.at;
            const int n = static_cast<int>(std::min<std::int64_t>(lengths[k % lengths.size()], left));
            loop.block(p, n);
        }
        CHECK(firstBitDiff(loop.outLeft, loop.reference.wantLeft) == -1);
        CHECK(firstBitDiff(loop.outRight, loop.reference.wantRight) == -1);
        CHECK(loop.input.loopStatus().resyncCount == 0u);
        CHECK(firstBitDiff(loop.outLeft, loop.dryLeft) != -1); // 回送确实在响（不是空断言）
        return loop.outLeft;
    };
    const auto a = runWith({64});
    const auto b = runWith({32768});
    const auto c = runWith({1, 7, 300, 4096, 8193, 32768, 513, 2048});
    REQUIRE(a.size() == static_cast<std::size_t>(kTotal));
    CHECK(firstBitDiff(a, b) == -1);
    CHECK(firstBitDiff(a, c) == -1);
}

TEST_CASE("Input 命令序号：首块之前按下的紧急切断在首块生效；基线之内的序号不重放；安全状态发布到 Input 槽",
          "[engine][input]")
{
    SECTION("新实例、首块之前按下的紧急切断")
    {
        // ⛔ 删除式验证点：构造时不把命令基线交给安全链（让它把第一块的序号当基线）→ 首块之前的切断被吞，本用例变红。
        ClosedLoop loop(kSr, 256);
        loop.source = [](std::int64_t t, int) { return cosine(t, 100.0, 0.3f); };
        EngineParams p = loopParams(100.0f, 50.0f, 10.0f);
        p.panicSeq = 1;
        loop.block(p, 256);
        CHECK(loop.state() == SafetyState::Tripped);
        loop.run(p, 256, 48000);
        CHECK(loop.state() == SafetyState::Tripped);
        CHECK(loop.input.safetyDiag().trips == 1u);
        CHECK(firstBitDiff(loop.outLeft, loop.dryLeft) == -1); // 回送从来没有进来
        const rinne::ipc::RoleSlot& slot = rinne::ipc::roleSlot(*loop.rv, 1, rinne::ipc::Role::kInput);
        CHECK(slot.safety_state.load() ==
              (rinne::ipc::kSafetyTripped | (rinne::ipc::kTripPanic << rinne::ipc::kTripShift)));
    }
    SECTION("重建的实例：基线之内的序号不重放，之后的变化照常执行")
    {
        // ⛔ 删除式验证点：setCommandBaseline 不把 panic / recover 的基线交给安全链 → 基线里的 5 被当成新命令，
        //    首块就跳闸，本用例变红。
        ClosedLoop loop(kSr, 256);
        CommandBaseline baseline;
        baseline.presetSeq = 3;
        baseline.stateSeq = 2;
        baseline.panicSeq = 5;
        baseline.recoverSeq = 4;
        loop.input.setCommandBaseline(baseline);
        loop.reference.setCommandBaseline(baseline);
        FadeLog log;
        loop.input.setFadeObserver(&log, &FadeLog::record);
        loop.source = [](std::int64_t t, int) { return cosine(t, 100.0, 0.3f); };
        EngineParams p = loopParams(100.0f, 50.0f, 10.0f);
        p.presetSeq = 3;
        p.stateSeq = 2;
        p.panicSeq = 5;
        p.recoverSeq = 4;
        loop.run(p, 256, 48000);
        CHECK(loop.state() != SafetyState::Tripped);
        CHECK(loop.input.safetyDiag().trips == 0u);
        CHECK(log.reasons.empty());
        p.presetSeq = 4;
        loop.block(p, 256);
        REQUIRE(log.reasons.size() == 1u);
        CHECK(log.reasons[0] == FadeReason::Preset);
        CHECK(PackedSafety::fade(loop.input.meters().safety.load()) == FadeReason::Preset);
        p.panicSeq = 6;
        loop.block(p, 256);
        CHECK(loop.state() == SafetyState::Tripped);
        CHECK(loop.input.safetyDiag().trips == 1u);
        CHECK(firstBitDiff(loop.outLeft, loop.reference.wantLeft) == -1);
    }
}

TEST_CASE("Input 接线：读头 → 前段 → （音色级空位）→ 后段 → g_dry·干声 + 回送，与参照安全链逐位一致", "[engine][input]")
{
    // ⛔ 删除式验证点：后段的干声参数不乘 g_dry（链增益估计看错了送出）、或前后两段次序颠倒、或旁路不交给环路门
    //    → 与参照对不上，本用例变红。
    // 参数随块变：反馈、mix（g_dry 不为 1）、限制器开关、上限、旁路、换写方、起播沿都走一遍。
    ClosedLoop loop(kSr, 512);
    loop.source = [](std::int64_t t, int channel) {
        return cosine(t, channel == 0 ? 110.0 : 170.0, 0.35f) + (t % 1201 == 0 ? 0.3f : 0.0f);
    };
    EngineParams p = loopParams(120.0f, 75.0f, 20.0f);
    const int lengths[] = {512, 37, 256, 400};
    TransportInfo transport;
    for (int k = 0; loop.at < 6 * 48000; ++k)
    {
        const std::int64_t at = loop.at;
        p.limiterOn = (at / 48000) % 2 == 0;
        p.ceilingDb = at < 96000 ? -6.0f : -12.0f;
        p.feedbackPercent = at < 144000 ? 120.0f : 60.0f;
        loop.inputBypassed = at >= 150000 && at < 160000;
        transport.isPlaying = at >= 200000;
        if (at >= 240000 && !loop.writerIsSecond)
        {
            loop.switchWriter();
        }
        loop.block(p, lengths[k % 4], transport);
    }
    CHECK(firstBitDiff(loop.outLeft, loop.reference.wantLeft) == -1);
    CHECK(firstBitDiff(loop.outRight, loop.reference.wantRight) == -1);
    CHECK(firstBitDiff(loop.outLeft, loop.dryLeft) != -1); // 回送确实在响
    CHECK(loop.input.loopStatus().resyncCount >= 2u); // 起播与换写方
}

TEST_CASE("Input 环内 NaN：写方清洗计数增加时琥珀灯亮、不跳闸，5 s 之后灭；换段时重取基线", "[engine][input]")
{
    // ⛔ 删除式验证点：琥珀灯不按样本倒数（亮了就不灭）→ 本用例变红；换段时不重取基线 → 新段原有的计数点亮了灯，
    //    本用例变红；环内的 NaN 走到 ×g 才被截住 → 跳闸，本用例变红。
    SECTION("环内插件吐 NaN")
    {
        ClosedLoop loop(kSr, 480);
        loop.source = [](std::int64_t t, int) { return cosine(t, 100.0, 0.3f); };
        static constexpr std::int64_t kFrom = 24000;
        static constexpr std::int64_t kLast = 28800; // 最后一个 NaN 所在的样本
        loop.middle = [](std::int64_t at, float* left, float*, int n) {
            for (int i = 0; i < n; ++i)
            {
                const std::int64_t t = at + i;
                if (t >= kFrom && t <= kLast && t % 480 == 0)
                {
                    left[i] = std::numeric_limits<float>::quiet_NaN();
                }
            }
        };
        const EngineParams p = loopParams(50.0f, 50.0f, 50.0f);
        loop.run(p, 480, kFrom);
        CHECK(!loop.input.safetyDiag().loopNanLamp);
        loop.run(p, 480, kLast + 480 - kFrom); // 最后一个 NaN 由 Output 在这一周期写环前清洗
        loop.block(p, 480); // Input 在下一块块首看见计数增加
        SafetyDiag diag = loop.input.safetyDiag();
        CHECK(diag.loopNanLamp);
        CHECK(diag.nonfiniteRing > 0u);
        CHECK(diag.trips == 0u);
        CHECK(loop.state() != SafetyState::Tripped);
        // 5 s（240000 个样本）之内一直亮，之后灭
        const std::int64_t seen = loop.at - 480; // 看见计数增加的那一块的起点
        while (loop.at + 480 <= seen + 240000)
        {
            loop.block(p, 480);
        }
        CHECK(loop.input.safetyDiag().loopNanLamp);
        loop.block(p, 480);
        loop.block(p, 480);
        CHECK(!loop.input.safetyDiag().loopNanLamp);
        CHECK(loop.input.safetyDiag().trips == 0u);
        CHECK(firstBitDiff(loop.outLeft, loop.reference.wantLeft) == -1);
    }
    SECTION("换段：新段上原有的清洗计数不算")
    {
        auto ringA = std::make_unique<rinne::ipc::RingSegment>();
        auto ringB = std::make_unique<rinne::ipc::RingSegment>();
        rinne::ipc::LoopRing::initGeometry(*ringA);
        rinne::ipc::LoopRing::initGeometry(*ringB);
        rinne::ipc::LoopRing::publishWriter(*ringA, 1, 48000, 2);
        rinne::ipc::LoopRing::publishWriter(*ringB, 2, 48000, 2);
        ringB->header.nonfinite_count.store(77);
        InputEngine input;
        input.prepare(kSr, 256);
        std::vector<float> left(256, 0.0f);
        std::vector<float> right(256, 0.0f);
        float* channels[2] = {left.data(), right.data()};
        AudioView view;
        view.channels = channels;
        view.numChannels = 2;
        view.length = 256;
        EngineContext ctx;
        ctx.ring = ringA.get();
        ctx.ringGeneration = 1;
        for (int k = 0; k < 4; ++k)
        {
            input.process(view, EngineParams{}, TransportInfo{}, false, ctx);
        }
        ctx.ring = ringB.get();
        ctx.ringGeneration = 2;
        for (int k = 0; k < 4; ++k)
        {
            input.process(view, EngineParams{}, TransportInfo{}, false, ctx);
        }
        CHECK(!input.safetyDiag().loopNanLamp);
        ringB->header.nonfinite_count.store(78);
        input.process(view, EngineParams{}, TransportInfo{}, false, ctx);
        CHECK(input.safetyDiag().loopNanLamp);
    }
}

TEST_CASE("Input 读环之后的清洗在 ×g 之前：环里直接写进的 NaN 被置 0 并计数，不当成 Input 内部的非有限值跳闸",
          "[engine][input]")
{
    // ⛔ 删除式验证点：读头不经安全链前段、直接进后段 → NaN 走到 ×g 被当成 Input 内部的非有限值，跳闸，本用例变红。
    auto ring = std::make_unique<rinne::ipc::RingSegment>();
    rinne::ipc::LoopRing::initGeometry(*ring);
    rinne::ipc::LoopRing::publishWriter(*ring, 1, 48000, 2);
    InputEngine input;
    input.prepare(kSr, 256);
    const EngineParams p = loopParams(100.0f, 50.0f, 10.0f);
    EngineContext ctx;
    ctx.ring = ring.get();
    ctx.ringGeneration = 1;
    bool sawTapNonFinite = false;
    for (int k = 0; k < 200; ++k)
    {
        std::vector<float> left(256, 0.1f);
        std::vector<float> right(256, -0.1f);
        float* channels[2] = {left.data(), right.data()};
        AudioView view;
        view.channels = channels;
        view.numChannels = 2;
        view.length = 256;
        input.process(view, p, TransportInfo{}, false, ctx);
        for (float v : left)
        {
            REQUIRE(rinne::dsp::isFiniteSample(v));
        }
        rinne::ipc::LoopRing::write(*ring, 256, [](std::uint32_t, std::uint64_t pos, float& l, float& r) {
            l = pos % 5u == 0u ? std::numeric_limits<float>::quiet_NaN() : 0.3f;
            r = pos % 7u == 0u ? -std::numeric_limits<float>::infinity() : -0.3f;
        });
        input.drainEvents([&](const rinne::diag::Event& e) {
            sawTapNonFinite = sawTapNonFinite || e.code == static_cast<std::uint16_t>(SafetyEventCode::TapNonFinite);
        });
    }
    CHECK(PackedSafety::state(input.meters().safety.load()) != SafetyState::Tripped);
    CHECK(input.safetyDiag().trips == 0u);
    CHECK(input.meters().returnNonFinite.load() > 0u);
    CHECK(sawTapNonFinite);
}
