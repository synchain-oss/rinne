// SPDX-License-Identifier: GPL-3.0-or-later
//
// 低切 / 高切这一级的行为：关态逐位直通；开关与改截止都平滑（30 ms 截止滑动、10 ms 交叉），没有咔嗒；
// 10^6 次随机跳变不发散、不出非有限值；reset 与 prepare 落定；静音时不进 denormal。

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "ToneTestKit.h"
#include "dsp/LinearSmoother.h"
#include "dsp/SvfTpt.h"
#include "dsp/SwitchRamp.h"

using rinne::dsp::CutFilter;
using rinne::dsp::CutKind;
using rinne::dsp::isFiniteSample;
using rinne::dsp::kCutoffSmoothSeconds;
using rinne::dsp::kCutSwitchSeconds;
using rinne::dsp::kSwitchRampSeconds;
using rinne::dsp::LinearSmoother;
using rinne::dsp::svfCoefficients;
using rinne::dsp::SvfResponse;
using rinne::dsp::SvfTpt;
using rinne::test::floatBits;
using rinne::test::fromBits;
using rinne::test::isSubnormal;
using rinne::test::Rng;
using rinne::test::sineStepBound;

namespace
{

constexpr double kRates[] = {44100.0, 48000.0, 96000.0, 192000.0};

// 随机跳变的次数（规格：10^6 次随机截止跳变不发散）。参数跳变用例跳这么多次，裸 SVF 用例每个样本跳一次、跑这么多个样本
constexpr int kJumpTrials = 1000000;

bool sameCoefficients(const rinne::dsp::SvfCoefficients& a, const rinne::dsp::SvfCoefficients& b)
{
    return a.g == b.g && a.a1 == b.a1 && a.a2 == b.a2 && a.a3 == b.a3;
}

// 一个参数事件：第 at 个样本之前把参数设成 hz
struct Event
{
    std::int64_t at;
    float hz;
};

// 低频正弦过一串参数事件，返回相邻输出样本差的最大值。起始参数 start 在 prepare 时就生效（不滑动）。
// from > 0 时只量第 from 个样本起的差，把滤波器从零状态起来的瞬态排除在外
template<std::size_t N>
double maxStep(CutKind kind, double sr, float start, const Event (&events)[N], std::int64_t total, double freq,
               double amplitude, double phase, std::int64_t from = 0)
{
    CutFilter f(kind);
    f.setCutoff(start);
    f.prepare(sr);
    std::size_t next = 0;
    double worst = 0.0;
    float prev = 0.0f;
    for (std::int64_t n = 0; n < total; ++n)
    {
        while (next < N && events[next].at == n)
            f.setCutoff(events[next++].hz);
        const auto x = static_cast<float>(
            amplitude * std::sin(2.0 * rinne::test::kPi * freq * static_cast<double>(n) / sr + phase));
        const float y = f.process(x);
        if (n > 0 && n >= from)
            worst = std::fmax(worst, std::fabs(static_cast<double>(y) - prev));
        prev = y;
    }
    return worst;
}

} // namespace

TEST_CASE("off passes every bit through, including -0.0 and NaN payloads", "[tone][cut][bypass]")
{
    // 低切 ≤ 20 Hz（含 NaN）、高切 ≥ 20000 Hz（含 NaN）都是关；随机位型（含 NaN、Inf、denormal、−0.0）逐位不变
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    struct Case
    {
        CutKind kind;
        float hz;
    };
    for (const Case c : {Case{CutKind::LowCut, 20.0f}, Case{CutKind::LowCut, 0.0f}, Case{CutKind::LowCut, -inf},
                         Case{CutKind::LowCut, 19.999f}, Case{CutKind::LowCut, nan}, Case{CutKind::HighCut, 20000.0f},
                         Case{CutKind::HighCut, 25000.0f}, Case{CutKind::HighCut, inf}, Case{CutKind::HighCut, nan}})
    {
        for (const double sr : kRates)
        {
            INFO("kind = " << static_cast<int>(c.kind) << ", hz = " << c.hz << ", sr = " << sr);
            CutFilter f(c.kind);
            f.setCutoff(c.hz);
            f.prepare(sr);
            CHECK(f.isBypassed());
            CHECK_FALSE(f.isOn());
            Rng rng(static_cast<std::uint64_t>(sr) + static_cast<std::uint64_t>(c.kind));
            bool same = true;
            for (int i = 0; i < 20000; ++i)
            {
                float l = rng.bits();
                float r = i % 7 == 0 ? fromBits(0x80000000u) : rng.bits();
                const std::uint32_t lb = floatBits(l);
                const std::uint32_t rb = floatBits(r);
                f.setCutoff(c.hz);
                f.process(l, r);
                same = same && floatBits(l) == lb && floatBits(r) == rb;
                const float m = i % 3 == 0 ? fromBits(0xffc00123u) : rng.bits();
                same = same && floatBits(f.process(m)) == floatBits(m);
            }
            CHECK(same);
        }
    }
    // 没有 prepare 过的实例同样逐位直通
    CutFilter unprepared(CutKind::LowCut);
    unprepared.setCutoff(500.0f);
    CHECK(floatBits(unprepared.process(fromBits(0x80000000u))) == 0x80000000u);

    // 必须变红：「关」若只是把截止停在 20 Hz、滤波器照跑，−0.0 与普通样本都会变。这里确认这种实现过不了上面的断言
    SvfTpt running;
    const auto at20 = svfCoefficients(20.0, 48000.0);
    bool anyDiffers = floatBits(running.process(fromBits(0x80000000u), at20, SvfResponse::HighPass)) != 0x80000000u;
    for (int i = 0; i < 100; ++i)
    {
        const float x = 0.5f + 0.001f * static_cast<float>(i);
        anyDiffers = anyDiffers || running.process(x, at20, SvfResponse::HighPass) != x;
    }
    CHECK(anyDiffers);
}

TEST_CASE("turning a cut off glides back to the off corner, crosses over in 10 ms, then passes bits through",
          "[tone][cut][bypass]")
{
    // 交叉时长只有一个真源：CutFilter 用的 kCutSwitchSeconds 就是 SwitchRamp 的标准时长，10 ms
    CHECK(kCutSwitchSeconds == kSwitchRampSeconds);
    for (const double sr : kRates)
    {
        const auto glide = LinearSmoother::samplesFor(sr, kCutoffSmoothSeconds);
        const auto cross = LinearSmoother::samplesFor(sr, kCutSwitchSeconds);
        for (const CutKind kind : {CutKind::LowCut, CutKind::HighCut})
        {
            INFO("sr = " << sr << ", kind = " << static_cast<int>(kind));
            const bool low = kind == CutKind::LowCut;
            CutFilter f(kind);
            f.setCutoff(low ? 800.0f : 3000.0f);
            f.prepare(sr);
            CHECK_FALSE(f.isBypassed());
            Rng rng(5);
            for (int i = 0; i < 4800; ++i)
                f.process(static_cast<float>(rng.range(-1.0, 1.0)));
            f.setCutoff(low ? 20.0f : 20000.0f);
            CHECK_FALSE(f.isOn());
            std::int64_t passFrom = -1;
            bool stayed = true;
            for (std::int64_t n = 0; n < glide + cross + 4800; ++n)
            {
                const auto x = static_cast<float>(rng.range(-1.0, 1.0));
                const float y = f.process(x);
                const bool bypassed = f.isBypassed();
                if (bypassed && passFrom < 0)
                    passFrom = n;
                if (passFrom >= 0)
                    stayed = stayed && bypassed && floatBits(y) == floatBits(x);
            }
            // 滑动 glide 个样本、交叉 cross 个样本；交叉的最后一个样本（增益恰为 0）就已经是干声本身
            CHECK(passFrom == glide + cross - 2);
            CHECK(stayed);
            // 截止停在端点上。高切的端点是 min(20000, 0.45·sr)：44.1 kHz 下 20000 落在 0.45·sr 之外，
            // 端点就是 19845 Hz（这时高切并不真的关，参数在端点之外才报截断）
            CHECK(f.targetCutoffHz() == (low ? 20.0 : std::min(20000.0, 0.45 * sr)));
        }
    }
}

TEST_CASE("prepare with a cutoff already set starts on target without a glide", "[tone][cut]")
{
    for (const double sr : kRates)
    {
        CutFilter f(CutKind::LowCut);
        f.setCutoff(500.0f);
        f.prepare(sr);
        CHECK_FALSE(f.isSmoothing());
        CHECK(sameCoefficients(f.coefficients(), svfCoefficients(500.0, sr)));
        // 与直接用目标系数的单声道 SVF 逐位相同
        SvfTpt ref;
        const auto c = svfCoefficients(500.0, sr);
        Rng rng(9);
        bool same = true;
        for (int i = 0; i < 10000; ++i)
        {
            const auto x = static_cast<float>(rng.range(-2.0, 2.0));
            same = same && floatBits(f.process(x)) == floatBits(ref.process(x, c, SvfResponse::HighPass));
        }
        CHECK(same);
    }
}

TEST_CASE("a cutoff change glides for exactly 30 ms and lands on the target's own coefficients", "[tone][cut]")
{
    for (const double sr : kRates)
    {
        INFO("sr = " << sr);
        const auto glide = LinearSmoother::samplesFor(sr, kCutoffSmoothSeconds);
        CutFilter f(CutKind::HighCut);
        f.setCutoff(2000.0f);
        f.prepare(sr);
        f.setCutoff(8000.0f);
        double prevG = f.coefficients().g;
        bool monotonic = true;
        for (std::int64_t n = 1; n < glide; ++n)
        {
            f.process(0.0f);
            monotonic = monotonic && f.coefficients().g > prevG;
            prevG = f.coefficients().g;
        }
        CHECK(monotonic);
        CHECK(f.isSmoothing());
        CHECK(prevG < svfCoefficients(8000.0, sr).g);
        f.process(0.0f);
        CHECK_FALSE(f.isSmoothing());
        CHECK(sameCoefficients(f.coefficients(), svfCoefficients(8000.0, sr)));
        // 一半时在 log2 域的中点上：2000·√4 = 4000 Hz
        CutFilter h(CutKind::HighCut);
        h.setCutoff(2000.0f);
        h.prepare(sr);
        h.setCutoff(8000.0f);
        for (std::int64_t n = 0; n < glide / 2; ++n)
            h.process(0.0f);
        const double midHz = std::atan(h.coefficients().g) * sr / rinne::test::kPi;
        CHECK(std::fabs(std::log2(midHz / 4000.0)) < 2.0 / static_cast<double>(glide) + 1.0e-6);
    }
}

TEST_CASE("reset clears the state and settles the glide and the crossover on the target", "[tone][cut]")
{
    const double sr = 48000.0;
    CutFilter f(CutKind::LowCut);
    f.setCutoff(100.0f);
    f.prepare(sr);
    Rng rng(13);
    for (int i = 0; i < 1000; ++i)
        f.process(static_cast<float>(rng.range(-1.0, 1.0)));
    f.setCutoff(900.0f);
    for (int i = 0; i < 100; ++i)
        f.process(static_cast<float>(rng.range(-1.0, 1.0)));
    CHECK(f.isSmoothing());
    f.reset();
    CHECK_FALSE(f.isSmoothing());
    CHECK(sameCoefficients(f.coefficients(), svfCoefficients(900.0, sr)));
    SvfTpt fresh;
    const auto c = svfCoefficients(900.0, sr);
    bool same = true;
    for (int i = 0; i < 2000; ++i)
    {
        const float x = i == 0 ? 1.0f : 0.0f;
        same = same && floatBits(f.process(x)) == floatBits(fresh.process(x, c, SvfResponse::HighPass));
    }
    CHECK(same);

    // 开关交叉进行到一半时 reset：直接落到开或关
    CutFilter g(CutKind::HighCut);
    g.prepare(sr);
    g.setCutoff(5000.0f);
    for (int i = 0; i < 100; ++i)
        g.process(0.1f);
    g.setCutoff(20000.0f);
    g.reset();
    CHECK(g.isBypassed());
    CHECK(floatBits(g.process(fromBits(0x80000000u))) == 0x80000000u);
}

TEST_CASE("cutoff jumps, on/off switches and sweeps make no clicks", "[tone][cut][click]")
{
    // 判据：100 Hz、幅度 0.5 的正弦，输出相邻样本差不超过输入自身上界 2a·sin(πf/sr) 的 1.5 倍（实测最坏约 1.19 倍，
    // 出在高切 19 kHz → 1 kHz 的 30 ms 滑动上）。事件落在正弦的 45° 相位上：那里硬切换的台阶最明显。
    // 必须变红：同样的事件换成硬切换（直接旁路、系数一步跳到位），台阶是上界的 7 倍以上，见本用例最后一段。
    constexpr double freq = 100.0;
    constexpr double amp = 0.5;
    for (const double sr : kRates)
    {
        const double bound = 1.5 * sineStepBound(amp, freq, sr);
        const double period = sr / freq;
        const auto s = static_cast<std::int64_t>(0.3 * sr + period / 8.0);
        const auto total = s + static_cast<std::int64_t>(0.15 * sr);
        const auto ms = [sr](double t) { return static_cast<std::int64_t>(t * sr / 1000.0); };
        const double phase = 0.0;
        INFO("sr = " << sr);
        {
            const Event e[] = {{s, 2000.0f}};
            CHECK(maxStep(CutKind::LowCut, sr, 20.0f, e, total, freq, amp, phase) < bound);
        }
        {
            const Event e[] = {{s, 20.0f}};
            CHECK(maxStep(CutKind::LowCut, sr, 2000.0f, e, total, freq, amp, phase) < bound);
        }
        {
            const Event e[] = {{s, 1500.0f}};
            CHECK(maxStep(CutKind::LowCut, sr, 50.0f, e, total, freq, amp, phase) < bound);
        }
        {
            // 过渡中来回切：开 → 5 ms 后关 → 再开 → 再关 → 再开
            const Event e[] = {
                {s, 500.0f}, {s + ms(5), 20.0f}, {s + ms(25), 800.0f}, {s + ms(33), 20.0f}, {s + ms(70), 2000.0f}};
            CHECK(maxStep(CutKind::LowCut, sr, 20.0f, e, total, freq, amp, phase) < bound);
        }
        {
            const Event e[] = {{s, 1000.0f}};
            CHECK(maxStep(CutKind::HighCut, sr, 20000.0f, e, total, freq, amp, phase) < bound);
        }
        {
            const Event e[] = {{s, 20000.0f}};
            CHECK(maxStep(CutKind::HighCut, sr, 1000.0f, e, total, freq, amp, phase) < bound);
        }
        {
            const Event e[] = {{s, 1000.0f}};
            CHECK(maxStep(CutKind::HighCut, sr, 19000.0f, e, total, freq, amp, phase) < bound);
        }
        {
            const Event e[] = {{s, 19000.0f}};
            CHECK(maxStep(CutKind::HighCut, sr, 1000.0f, e, total, freq, amp, phase) < bound);
        }
        {
            const Event e[] = {{s, 1000.0f},
                               {s + ms(5), 20000.0f},
                               {s + ms(25), 3000.0f},
                               {s + ms(33), 20000.0f},
                               {s + ms(70), 1000.0f}};
            CHECK(maxStep(CutKind::HighCut, sr, 20000.0f, e, total, freq, amp, phase) < bound);
        }
    }
}

TEST_CASE("automation sweeps every 32 samples stay click-free", "[tone][cut][click]")
{
    constexpr double freq = 100.0;
    constexpr double amp = 0.5;
    for (const double sr : kRates)
    {
        for (const CutKind kind : {CutKind::LowCut, CutKind::HighCut})
        {
            INFO("sr = " << sr << ", kind = " << static_cast<int>(kind));
            const bool low = kind == CutKind::LowCut;
            const double lo = low ? 21.0 : 1000.0;
            const double hi = low ? 2000.0 : 19800.0;
            CutFilter f(kind);
            f.setCutoff(static_cast<float>(lo));
            f.prepare(sr);
            const auto settle = static_cast<std::int64_t>(0.3 * sr);
            const auto total = settle + static_cast<std::int64_t>(sr);
            // 咔嗒判据只看自动化那一段：开头那一下是滤波器从零状态起来的阶跃响应，
            // 它的上升沿由滤波器自己的时间常数决定，不是咔嗒（另有一格断言它的幅度）。
            double worst = 0.0;
            double startup = 0.0;
            float prev = 0.0f;
            bool finite = true;
            for (std::int64_t n = 0; n < total; ++n)
            {
                if (n >= settle && (n - settle) % 32 == 0)
                {
                    // 1 s 里上下扫两个来回，每段 0.25 s，log 刻度
                    double u = std::fmod(4.0 * static_cast<double>(n - settle) / sr, 2.0);
                    u = u <= 1.0 ? u : 2.0 - u;
                    f.setCutoff(static_cast<float>(lo * std::pow(hi / lo, u)));
                }
                const auto x =
                    static_cast<float>(amp * std::sin(2.0 * rinne::test::kPi * freq * static_cast<double>(n) / sr +
                                                      rinne::test::kPi / 4.0));
                const float y = f.process(x);
                finite = finite && std::isfinite(y);
                if (n > 0)
                {
                    const auto step = std::fabs(static_cast<double>(y) - prev);
                    if (n < settle)
                        startup = std::fmax(startup, step);
                    else
                        worst = std::fmax(worst, step);
                }
                prev = y;
            }
            CHECK(finite);
            CHECK(worst < 1.5 * sineStepBound(amp, freq, sr));
            // 起振那一段：低通的上升沿最陡，1 kHz 截止下每样本约 0.012（正弦斜率上界的 1.8 倍）
            CHECK(startup < 4.0 * sineStepBound(amp, freq, sr));

            // 必须变红：扫到一半硬旁路（直接输出干声）在同一段上是上界的 5 倍以上
            CutFilter hard(kind);
            hard.setCutoff(static_cast<float>(lo));
            hard.prepare(sr);
            const auto bypassAt = settle + static_cast<std::int64_t>(0.5 * sr);
            double hardWorst = 0.0;
            float hardPrev = 0.0f;
            for (std::int64_t n = 0; n < total; ++n)
            {
                if (n >= settle && (n - settle) % 32 == 0)
                {
                    double u = std::fmod(4.0 * static_cast<double>(n - settle) / sr, 2.0);
                    u = u <= 1.0 ? u : 2.0 - u;
                    hard.setCutoff(static_cast<float>(lo * std::pow(hi / lo, u)));
                }
                const auto x =
                    static_cast<float>(amp * std::sin(2.0 * rinne::test::kPi * freq * static_cast<double>(n) / sr +
                                                      rinne::test::kPi / 4.0));
                const float y = n == bypassAt ? x : hard.process(x);
                if (n > 0 && n >= settle)
                    hardWorst = std::fmax(hardWorst, std::fabs(static_cast<double>(y) - hardPrev));
                hardPrev = y;
            }
            CHECK(hardWorst > 3.0 * sineStepBound(amp, freq, sr));
        }
    }
}

TEST_CASE("the click criterion catches hard switches (the counterexample must fail it)", "[tone][cut][click]")
{
    constexpr double freq = 100.0;
    constexpr double amp = 0.5;
    for (const double sr : kRates)
    {
        INFO("sr = " << sr);
        const double bound = 1.5 * sineStepBound(amp, freq, sr);
        const auto s = static_cast<std::int64_t>(0.3 * sr + sr / freq / 8.0);
        // 硬切换：在 s 处系数一步跳到新值，或直接改成干声
        const auto hard = [&](SvfResponse r, double f0, double f1, bool bypass) {
            SvfTpt t;
            const auto c0 = svfCoefficients(f0, sr);
            const auto c1 = svfCoefficients(f1, sr);
            double worst = 0.0;
            float prev = 0.0f;
            for (std::int64_t n = 0; n < s + static_cast<std::int64_t>(0.02 * sr); ++n)
            {
                const auto x =
                    static_cast<float>(amp * std::sin(2.0 * rinne::test::kPi * freq * static_cast<double>(n) / sr));
                float y = t.process(x, n < s ? c0 : c1, r);
                if (bypass && n >= s)
                    y = x;
                if (n > 0)
                    worst = std::fmax(worst, std::fabs(static_cast<double>(y) - prev));
                prev = y;
            }
            return worst;
        };
        CHECK(hard(SvfResponse::HighPass, 20.0, 2000.0, false) > 3.0 * bound);
        CHECK(hard(SvfResponse::HighPass, 2000.0, 2000.0, true) > 3.0 * bound);
        CHECK(hard(SvfResponse::LowPass, 1000.0, 19000.0, false) > 3.0 * bound);
        CHECK(hard(SvfResponse::LowPass, 1000.0, 1000.0, true) > 3.0 * bound);
    }
}

TEST_CASE("the low cut makes no click on a 25-30 Hz sine, where its 20 Hz end is not transparent", "[tone][cut][click]")
{
    // 关的端点是 20 Hz 的二阶高通，它只在约 40 Hz 以上几乎直通：25 Hz 上 −1.5 dB / 72°，30 Hz 上 −0.8 dB / 60°
    // （数字见 dsp/SvfTpt.h 头注释）。所以在 25–30 Hz 的正弦上，交叉的两头（干声与端点的滤波输出）增益和相位都不同，
    // 差可以到一个振幅。10 ms 线性交叉把这个差摊开，输出连续，没有台阶。
    // 事件：从端点开到 30 / 100 / 2000 Hz（开）；从 100 / 30 / 2000 Hz 关回 20 Hz（先滑回端点，再交叉回干声）；
    // 过渡中来回切一串。每个事件对 8 个相位各跑一遍，只量事件前一个样本起的相邻输出样本差，取最坏的。
    // 判据：不超过输入正弦自身上界 2a·sin(πf/sr) 的 kLimit 倍。实测最坏 1.52 倍（25 Hz、来回切，四个采样率一样），
    // 开的方向 1.27–1.43 倍（起振的瞬态叠在交叉上），关的方向 1.00 倍；100 Hz 上的同类判据是 1.5 倍
    // （"cutoff jumps, on/off switches and sweeps make no clicks"），这里留出 2 倍。
    // 必须变红：硬切换，端点的滤波器一直在跑，输出在事件那一格直接从滤波输出跳到干声（关），或从干声跳到滤波输出（开）。
    // 台阶是两路在该相位上的差，实测是上界的 218–1338 倍，用例只要求超过 6 倍，见本用例最后一段。
    constexpr double amp = 0.5;
    constexpr int kPhases = 8;
    constexpr double kLimit = 2.0;
    for (const double sr : kRates)
    {
        const auto s = static_cast<std::int64_t>(0.3 * sr);
        const auto total = s + static_cast<std::int64_t>(0.15 * sr);
        const auto ms = [sr](double t) { return static_cast<std::int64_t>(t * sr / 1000.0); };
        for (const double freq : {25.0, 27.5, 30.0})
        {
            INFO("sr = " << sr << ", freq = " << freq);
            const double ref = sineStepBound(amp, freq, sr);
            const auto sample = [&](std::int64_t n, int k) {
                return static_cast<float>(amp * std::sin(2.0 * rinne::test::kPi * freq * static_cast<double>(n) / sr +
                                                         static_cast<double>(k) * rinne::test::kPi / 4.0));
            };
            // 相邻输出样本差的最坏值，是 ref 的几倍
            const auto worstRatio = [&](float start, const auto& events) {
                double worst = 0.0;
                for (int k = 0; k < kPhases; ++k)
                    worst = std::fmax(worst, maxStep(CutKind::LowCut, sr, start, events, total, freq, amp,
                                                     static_cast<double>(k) * rinne::test::kPi / 4.0, s - 1));
                return worst / ref;
            };
            {
                INFO("on: 20 -> 30 Hz");
                const Event e[] = {{s, 30.0f}};
                CHECK(worstRatio(20.0f, e) < kLimit);
            }
            {
                INFO("on: 20 -> 100 Hz");
                const Event e[] = {{s, 100.0f}};
                CHECK(worstRatio(20.0f, e) < kLimit);
            }
            {
                INFO("on: 20 -> 2000 Hz");
                const Event e[] = {{s, 2000.0f}};
                CHECK(worstRatio(20.0f, e) < kLimit);
            }
            const Event off[] = {{s, 20.0f}};
            for (const float from : {100.0f, 30.0f, 2000.0f})
            {
                INFO("off: " << from << " -> 20 Hz");
                CHECK(worstRatio(from, off) < kLimit);
            }
            {
                INFO("toggling mid-transition");
                const Event e[] = {
                    {s, 60.0f}, {s + ms(5), 20.0f}, {s + ms(25), 100.0f}, {s + ms(33), 20.0f}, {s + ms(70), 40.0f}};
                CHECK(worstRatio(20.0f, e) < kLimit);
            }

            // 必须变红：硬切换。端点滤波器一直在跑（稳态），第 s 个样本直接换路
            for (const bool toFiltered : {false, true})
            {
                INFO("hard switch, to filtered = " << toFiltered);
                const auto c20 = svfCoefficients(20.0, sr);
                double worst = 0.0;
                for (int k = 0; k < kPhases; ++k)
                {
                    SvfTpt t;
                    float prev = 0.0f;
                    for (std::int64_t n = 0; n < s + 64; ++n)
                    {
                        const float x = sample(n, k);
                        const float y = t.process(x, c20, SvfResponse::HighPass);
                        const float out = ((n >= s) == toFiltered) ? y : x;
                        if (n >= s - 1)
                            worst = std::fmax(worst, std::fabs(static_cast<double>(out) - prev));
                        prev = out;
                    }
                }
                CHECK(worst / ref > 3.0 * kLimit);
            }
        }
    }
}

TEST_CASE("10^6 random parameter jumps never make the cut diverge or go non-finite", "[tone][cut][stability]")
{
    // 每 1–32 个样本跳一次参数（含关、越界、NaN、±Inf），每 10^4 次换一个采样率重新 prepare；输入是清洗后范围内的
    // 随机噪声与 ±16 的方波。实测 |y| 最大约 27（高通对 ±16 方波的过冲），断言留到 48。
    // 两条断言各管一件事：挡住发散的是峰值界，finite 断言挡的是非有限输出。std::fmax 遇到 NaN 取另一个数，峰值界
    // 看不见 NaN；SVF 的输出不清非有限值（要留给 ⑥ 跳闸），这里算出的非有限值会原样出来，只有 finite 断言会红
    // （变异验证：让 SVF 的输出每 10^5 个样本出一个 NaN，本用例与下一个用例只有 finite 断言变红）。
    const float specials[] = {std::numeric_limits<float>::quiet_NaN(),
                              std::numeric_limits<float>::infinity(),
                              -std::numeric_limits<float>::infinity(),
                              -100.0f,
                              0.0f,
                              1.0e9f};
    for (const CutKind kind : {CutKind::LowCut, CutKind::HighCut})
    {
        INFO("kind = " << static_cast<int>(kind));
        const bool low = kind == CutKind::LowCut;
        Rng rng(low ? 101 : 202);
        CutFilter f(kind);
        bool finite = true;
        double peak = 0.0;
        for (int jump = 0; jump < kJumpTrials; ++jump)
        {
            if (jump % 10000 == 0)
                f.prepare(kRates[(jump / 10000) % 4]);
            const double r = rng.uniform();
            float hz = 0.0f;
            if (r < 0.1)
                hz = low ? 20.0f : 20000.0f;
            else if (r < 0.13)
                hz = specials[rng.below(6)];
            else
                hz = static_cast<float>(low ? std::exp(rng.range(std::log(10.0), std::log(2100.0)))
                                            : std::exp(rng.range(std::log(900.0), std::log(21000.0))));
            f.setCutoff(hz);
            const double level = rng.uniform() < 0.3 ? 16.0 : std::pow(10.0, rng.range(-3.0, 1.2));
            const bool square = rng.uniform() < 0.3;
            const int len = 1 + rng.below(32);
            for (int i = 0; i < len; ++i)
            {
                float l = static_cast<float>(square ? ((i / 4) % 2 == 0 ? level : -level) : rng.range(-level, level));
                float rr = static_cast<float>(rng.range(-level, level));
                l = rinne::dsp::sanitizeSample(l);
                rr = rinne::dsp::sanitizeSample(rr);
                f.process(l, rr);
                finite = finite && isFiniteSample(l) && isFiniteSample(rr);
                peak =
                    std::fmax(peak, std::fmax(std::fabs(static_cast<double>(l)), std::fabs(static_cast<double>(rr))));
            }
        }
        CHECK(finite);
        CHECK(peak < 48.0);
    }
}

TEST_CASE("the raw SVF stays bounded even when its coefficients jump every sample", "[tone][svf][stability]")
{
    // 比参数跳变更狠：每个样本都换一组随机系数（1 Hz…0.45·sr），输入 ±16 方波与噪声。实测 |y| 最大约 93、
    // 状态约 3 倍输入，不随时间增长；断言 256，并要求后半段的峰值不比前半段高出一倍（没有缓慢发散）。
    // 与上一个用例一样：挡住发散的是峰值界，finite 断言挡的是非有限输出（峰值经 std::fmax 累计，看不见 NaN）
    for (const SvfResponse resp : {SvfResponse::HighPass, SvfResponse::LowPass})
    {
        for (const double sr : {44100.0, 192000.0})
        {
            Rng rng(resp == SvfResponse::HighPass ? 7 : 8);
            SvfTpt t;
            bool finite = true;
            double firstHalf = 0.0;
            double secondHalf = 0.0;
            constexpr int total = kJumpTrials;
            for (int n = 0; n < total; ++n)
            {
                const auto c = svfCoefficients(std::exp(rng.range(0.0, std::log(0.45 * sr))), sr);
                const auto x =
                    static_cast<float>(rng.uniform() < 0.5 ? (rng.below(2) ? 16.0 : -16.0) : rng.range(-16.0, 16.0));
                const float y = t.process(x, c, resp);
                finite = finite && isFiniteSample(y);
                double& half = n < total / 2 ? firstHalf : secondHalf;
                half = std::fmax(half, std::fabs(static_cast<double>(y)));
            }
            INFO("sr = " << sr << ", first = " << firstHalf << ", second = " << secondHalf);
            CHECK(finite);
            CHECK(firstHalf < 256.0);
            CHECK(secondHalf < 256.0);
            CHECK(secondHalf < 2.0 * firstHalf);
        }
    }
}

TEST_CASE("cut state never goes denormal on silence", "[tone][cut][denormal]")
{
    for (const CutKind kind : {CutKind::LowCut, CutKind::HighCut})
    {
        CutFilter f(kind);
        f.setCutoff(kind == CutKind::LowCut ? 40.0f : 1000.0f);
        f.prepare(48000.0);
        Rng rng(17);
        for (int n = 0; n < 4800; ++n)
        {
            float l = static_cast<float>(rng.range(-1.0, 1.0));
            float r = rinne::test::sine(30.0, 48000.0, n);
            f.process(l, r);
        }
        bool sawSubnormal = false;
        float lastL = 1.0f;
        float lastR = 1.0f;
        for (int n = 0; n < 48000 * 10; ++n)
        {
            lastL = 0.0f;
            lastR = 0.0f;
            f.process(lastL, lastR);
            sawSubnormal = sawSubnormal || isSubnormal(lastL) || isSubnormal(lastR);
        }
        CHECK_FALSE(sawSubnormal);
        CHECK(lastL == 0.0f);
        CHECK(lastR == 0.0f);
    }
}

TEST_CASE("stereo channels are independent and share the coefficients", "[tone][cut]")
{
    const double sr = 48000.0;
    CutFilter stereo(CutKind::HighCut);
    CutFilter monoL(CutKind::HighCut);
    CutFilter monoR(CutKind::HighCut);
    for (CutFilter* f : {&stereo, &monoL, &monoR})
    {
        f->setCutoff(3000.0f);
        f->prepare(sr);
        f->setCutoff(1200.0f); // 带着滑动跑
    }
    Rng rng(19);
    bool same = true;
    for (int n = 0; n < 6000; ++n)
    {
        float l = static_cast<float>(rng.range(-1.0, 1.0));
        float r = static_cast<float>(rng.range(-1.0, 1.0));
        const float wantL = monoL.process(l);
        const float wantR = monoR.process(r);
        stereo.process(l, r);
        same = same && floatBits(l) == floatBits(wantL) && floatBits(r) == floatBits(wantR);
    }
    CHECK(same);
}
