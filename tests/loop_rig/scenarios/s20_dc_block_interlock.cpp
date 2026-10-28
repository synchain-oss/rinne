// SPDX-License-Identifier: GPL-3.0-or-later
//
// S20 直流阻断、限制器关时的联锁、高采样率上界。
//
// 高切关时逐位直通、不另设频带保护（回送链里只有恒开的 5 Hz 直流阻断），所以这一场景不测频带保护，测下面三格。
// 三格都用恒等的中间链（链增益 0 dB），混合 50 %（g_dry = g_ret = 1），输出与参照安全链逐位相同。
//
// 一、直流阻断：反馈 100 %、限制器开（上限 −6 dBFS）、环路延迟 D = 250 ms，轨道上放 0.25 的直流，持续 2·D 后归零。
//   以 D 为窗口把回送切成「圈」：第 n 圈是 [n·D, (n+1)·D)，m[n] 是这一圈回送的均值。直流从第 1 圈起进环，
//   第 3 圈是它的下行沿回到读头的那一圈。
//   - 直流进不了回送：直流还在的第 2 圈 |m[2]| < 0.01（没有直流阻断时是 0.07 上下）；
//   - 均值逐圈衰减：从第 3 圈起，|m[n]| ≤ |m[3]| · 2^(−(n−3)/2)，即平均每圈至少缩到 0.71 倍；
//     逐圈的 |m[n]| 本身不单调（直流阻断的拖尾在窗口边界上忽正忽负，量级在 1e-4），所以判据取这条衰减包络；
//   - 第 10 圈之后（n = 10…15）|m[n]| < 1e-3；
//   - 全程 |回送| ≤ 上限（−6 dBFS），有限，D 恒为 12000，零重同步。
//
// 二、联锁：限制器关、反馈 150 %、环内放一个冲激。限制器关时反馈的有效上限是 100 %，所以每圈回送的峰值不增长：
//   - 冲激幅度 0.005（−46 dBFS）：第 k 圈（k = 1…12）的峰值 p[k] 取 [t0 + k·D, t0 + (k+1)·D) 里的最大绝对值，
//     p[1] 等于冲激幅度，p[k+1] ≤ p[k] + 1e-6，第 12 圈仍不低于冲激幅度的 90 %（有效反馈是 100 %，不是更低）；
//     幅度取这么小是为了让绝对容差 1e-6 有意义：单位冲激一圈圈绕回来，会叠上直流阻断的拖尾，峰值在幅度上下
//     摆动约 6e-5 倍（幅度 0.25 时实测第 2 圈到第 3 圈涨 1.5e-5），这是有界的摆动，不是增长；
//   - 对照：同样的冲激、同样的 150 %，限制器开时反馈上限是 150 %，峰值逐圈涨（第 1 圈到第 2 圈涨 50 %）。
//     用同一条判据量这一条，必须判出增长，证明判据对增长会变红；
//   - 热冲激 2.0：软削波介入，全程 |回送| < 1.0。
//
// 三、192 kHz 上界：采样率 192 kHz、反馈 150 %、限制器开、高切关、白噪声 0 dBFS（均匀分布，峰值 ±1）：
//   输出与回送全程有限，|回送| ≤ 上限，回送确实在响（峰值到达上限的九成以上，限制器在工作）；
//   D = 100 ms = 19200 个样本，零重同步，输出与参照安全链逐位相同。
//
// 标 ⛔ 的是删除式验证点：把对应的实现改坏，本场景必须变红。
//   ⛔ 回送链跳过 5 Hz 直流阻断 → 直流进环，第 2 圈均值远大于 0.01，第 10 圈之后也不衰减；
//   ⛔ 限制器关时反馈上限放宽到 150 % → 联锁一格的峰值逐圈上涨；
//   ⛔ 限制器不钳上限 → 192 kHz 一格的回送超过上限；
//   ⛔ 软削波不再限幅 → 热冲激的回送到 2.0，超过 1.0。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "s01_rig_kit.h"

namespace
{

using namespace rinne::rig;
using namespace rinne::rig::scn;

constexpr int kBlock = 256;

float meanOf(const std::vector<float>& x, std::size_t from, std::size_t to)
{
    double sum = 0.0;
    for (std::size_t i = from; i < std::min(to, x.size()); ++i)
    {
        sum += static_cast<double>(x[i]);
    }
    return static_cast<float>(sum / static_cast<double>(to - from));
}

bool allFinite(const std::vector<float>& x)
{
    return std::all_of(x.begin(), x.end(), [](float v) { return std::isfinite(v); });
}

// 上限 −6 dBFS 的线性值，与引擎同一个 float 算法
float ceilingLinear(float ceilingDb)
{
    return std::pow(10.0f, ceilingDb / 20.0f);
}

class DcInterlockScenario final : public Scenario
{
public:
    const char* id() const noexcept override { return "s20_dc_block_interlock"; }

    bool run(ScenarioContext&) override
    {
        Verdict v;
        dcBlock(v);
        interlock(v);
        highRateBound(v);
        return v.ok();
    }

private:
    // ---- 一、直流阻断 ----
    static void dcBlock(Verdict& v)
    {
        INFO("dc block");
        constexpr std::int64_t kD = 12000; // 250 ms
        constexpr std::int64_t kLaps = 16;
        auto host = makeHost(kSr, kBlock);
        REQUIRE(host != nullptr);
        host->setBlockScripts(fixedBlocks(kBlock), fixedBlocks(kBlock));
        host->setChain(ChainKind::kIdentity);
        // static：lambda 不用捕获也能用它。const 局部常量显式捕获，clang 报 -Wunused-lambda-capture；
        // 只声明 constexpr 不捕获，MSVC 又报无法隐式捕获
        static constexpr std::uint64_t dcLength = static_cast<std::uint64_t>(2 * kD);
        const SourceFn dc = [](std::uint64_t first, float* left, float* right, int n) {
            for (int i = 0; i < n; ++i)
            {
                const float x = first + static_cast<std::uint64_t>(i) < dcLength ? 0.25f : 0.0f;
                left[i] = x;
                right[i] = x;
            }
        };
        Recorder rec(*host, dc);
        const rinne::engine::EngineParams p = loopParams(100.0f, 50.0f, 250.0f);
        rec.run(p, kLaps * kD);
        RIG_EQ(v, host->inputStatus().delayEffective, kD);
        RIG_EQ(v, host->inputStatus().resyncCount, 0u);

        const auto& ret = rec.reference.retLeft;
        RIG_CHECK(v, ret.size() >= static_cast<std::size_t>(kLaps * kD));
        float mean[kLaps] = {};
        for (std::int64_t n = 0; n < kLaps; ++n)
        {
            mean[n] = meanOf(ret, static_cast<std::size_t>(n * kD), static_cast<std::size_t>((n + 1) * kD));
        }

        // 直流进不了回送：第 2 圈直流还在输入端
        RIG_CHECK(v, std::fabs(mean[2]) < 0.01f);
        // 下行沿所在的这一圈有可见的响应（判据有东西可量），之后逐圈衰减
        RIG_CHECK(v, std::fabs(mean[3]) > 0.01f);
        for (std::int64_t n = 3; n < kLaps; ++n)
        {
            INFO("lap " << n << ": mean = " << mean[n]);
            const double envelope =
                static_cast<double>(std::fabs(mean[3])) * std::pow(2.0, -0.5 * static_cast<double>(n - 3));
            RIG_CHECK(v, static_cast<double>(std::fabs(mean[n])) <= envelope);
        }
        // 第 10 圈之后
        for (std::int64_t n = 10; n < kLaps; ++n)
        {
            INFO("lap " << n << ": mean = " << mean[n]);
            RIG_CHECK(v, std::fabs(mean[n]) < 1.0e-3f);
        }

        // 全程不超过上限，有限
        const float ceiling = ceilingLinear(-6.0f);
        RIG_CHECK(v, peakOf(rec.reference.retLeft, 0, ret.size()) <= ceiling * 1.0000001f);
        RIG_CHECK(v, peakOf(rec.reference.retRight, 0, ret.size()) <= ceiling * 1.0000001f);
        RIG_CHECK(v, allFinite(rec.outLeft) && allFinite(rec.outRight));
        RIG_CHECK(v, rec.matchesReference());
    }

    // ---- 二、联锁 ----
    // 一条冲激列：返回第 1…laps 圈的峰值（下标 0 不用）
    static std::vector<float> impulsePeaks(Verdict& v, float amplitude, bool limiterOn, std::size_t laps)
    {
        INFO("impulse " << amplitude << ", limiter " << limiterOn);
        constexpr std::uint64_t kAt = 30100; // 淡入走完之后
        constexpr std::size_t kD = 4800;
        auto host = makeHost(kSr, kBlock);
        REQUIRE(host != nullptr);
        host->setBlockScripts(fixedBlocks(kBlock), fixedBlocks(kBlock));
        host->setChain(ChainKind::kIdentity);
        Recorder rec(*host, impulseSource(kAt, amplitude));
        rinne::engine::EngineParams p = loopParams(150.0f, 50.0f, 100.0f);
        p.limiterOn = limiterOn;
        rec.run(p, static_cast<std::int64_t>(kAt + (laps + 1) * kD));
        RIG_EQ(v, host->inputStatus().delayEffective, static_cast<std::int64_t>(kD));
        RIG_CHECK(v, rec.matchesReference());

        std::vector<float> peaks(laps + 1, 0.0f);
        for (std::size_t k = 1; k <= laps; ++k)
        {
            peaks[k] = peakOf(rec.reference.retLeft, kAt + k * kD, kAt + (k + 1) * kD);
        }
        // 全程有限、回送小于 1.0；热冲激时由调用方另查
        RIG_CHECK(v, allFinite(rec.outLeft) && allFinite(rec.outRight));
        lastOverallPeak_ = std::max(peakOf(rec.reference.retLeft, 0, rec.reference.retLeft.size()),
                                    peakOf(rec.reference.retRight, 0, rec.reference.retRight.size()));
        return peaks;
    }

    static void interlock(Verdict& v)
    {
        constexpr std::size_t kLaps = 12;
        constexpr float kAmplitude = 0.005f;
        // 限制器关：有效反馈 100 %，峰值不增长
        const std::vector<float> off = impulsePeaks(v, kAmplitude, false, kLaps);
        RIG_CHECK(v, std::fabs(off[1] - kAmplitude) < 1.0e-6f);
        for (std::size_t k = 1; k < kLaps; ++k)
        {
            INFO("lap " << k << ": " << off[k] << " -> " << off[k + 1]);
            RIG_CHECK(v, off[k + 1] <= off[k] + 1.0e-6f);
        }
        RIG_CHECK(v, off[kLaps] >= 0.9f * kAmplitude);
        RIG_CHECK(v, lastOverallPeak_ < 1.0f);

        // 对照：限制器开，同样的 150 %，反馈上限是 150 %，峰值逐圈涨；同一条判据必须判出增长
        const std::vector<float> on = impulsePeaks(v, kAmplitude, true, kLaps);
        RIG_CHECK(v, on[2] > on[1] + 1.0e-6f);
        RIG_CHECK(v, on[3] > on[2] + 1.0e-6f);
        RIG_CHECK(v, on[2] >= 1.4f * on[1]);

        // 热冲激：软削波介入，回送始终小于 1.0
        const std::vector<float> hot = impulsePeaks(v, 2.0f, false, kLaps);
        RIG_CHECK(v, hot[1] > 0.9f);
        RIG_CHECK(v, lastOverallPeak_ < 1.0f);
    }

    // ---- 三、192 kHz 上界 ----
    static void highRateBound(Verdict& v)
    {
        INFO("192 kHz");
        constexpr double kRate = 192000.0;
        constexpr std::int64_t kDelay = 19200; // 100 ms
        auto host = makeHost(kRate, kBlock);
        REQUIRE(host != nullptr);
        host->setBlockScripts(fixedBlocks(kBlock), fixedBlocks(kBlock));
        host->setChain(ChainKind::kIdentity);
        const SourceFn noise = [](std::uint64_t first, float* left, float* right, int n) {
            for (int i = 0; i < n; ++i)
            {
                left[i] = hashNoise(first + static_cast<std::uint64_t>(i), 1);
                right[i] = hashNoise(first + static_cast<std::uint64_t>(i), 2);
            }
        };
        Recorder rec(*host, noise, kRate);
        // 限制器与高切都取默认：限制器开、上限 −6 dBFS，高切 20000 Hz（关）
        const rinne::engine::EngineParams p = loopParams(150.0f, 50.0f, 100.0f);
        RIG_EQ(v, p.limiterOn, true);
        RIG_EQ(v, p.highCutHz, rinne::engine::kMaxHighCutHz);
        rec.run(p, 3 * static_cast<std::int64_t>(kRate));
        RIG_EQ(v, host->inputStatus().delayEffective, kDelay);
        RIG_EQ(v, host->inputStatus().resyncCount, 0u);

        const auto& retLeft = rec.reference.retLeft;
        const auto& retRight = rec.reference.retRight;
        RIG_CHECK(v, retLeft.size() >= static_cast<std::size_t>(3 * kRate));
        RIG_CHECK(v, allFinite(rec.outLeft) && allFinite(rec.outRight));
        RIG_CHECK(v, allFinite(retLeft) && allFinite(retRight));
        const float ceiling = ceilingLinear(-6.0f);
        const float peakLeft = peakOf(retLeft, 0, retLeft.size());
        const float peakRight = peakOf(retRight, 0, retRight.size());
        INFO("peak left = " << peakLeft << ", right = " << peakRight << ", ceiling = " << ceiling);
        RIG_CHECK(v, peakLeft <= ceiling * 1.0000001f);
        RIG_CHECK(v, peakRight <= ceiling * 1.0000001f);
        // 回送确实在响：限制器把它钳在上限上
        RIG_CHECK(v, peakLeft >= 0.9f * ceiling);
        RIG_CHECK(v, peakRight >= 0.9f * ceiling);
        RIG_CHECK(v, rec.matchesReference());
    }

    // 最近一次 impulsePeaks 里整条回送的峰值（左右声道取大）。场景对象不跨线程，用一个静态量传出
    static inline float lastOverallPeak_ = 0.0f;
};

} // namespace

RINNE_RIG_SCENARIO("s20_dc_block_interlock")
{
    return new DcInterlockScenario();
}

TEST_CASE("s20: DC is blocked, the interlock keeps the peak from growing, 192 kHz stays under the ceiling",
          "[loop_rig][s20]")
{
    CHECK(rinne::rig::scn::runRegisteredScenario("s20_dc_block_interlock"));
}
