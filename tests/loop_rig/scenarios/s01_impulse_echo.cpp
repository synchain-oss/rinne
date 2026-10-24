// SPDX-License-Identifier: GPL-3.0-or-later
//
// S01 冲激回声：一个冲激经真实共享内存的闭环，回声的峰位恰为 k·D，幅度与独立的 float64 参考模型相符。
//
// 做法：淡入走完之后（g_safety 恒为 1）在轨道上打一个冲激，反馈 50 %、混合 50 %（g_dry = g_ret = 1，回送增益恰为
// 0.5）， 环路延迟 D = 4800（100 ms）。每绕一圈，回送过 DC 阻断 → ×0.5 → 限制器 → 软削波，再和干声加起来写回环。
//   - 左右声道 k = 1…6 的峰位逐样本等于冲激位置 + k·D；
//   - 整段输出与 float64 参考模型（同一套级联：DC 阻断 → ×g → 限制器 → 软削波，系数取引擎同样的 float 值）
//     逐样本相差不超过 1e-5；第一个回声逐位是 0.25f；
//   - 参考模型故意去掉 DC 阻断，与引擎的差必须大于 1e-5（证明这条界有分辨力）；
//   - 再加两条腿覆盖限制器与软削波：热冲激（1.2，限制器介入，回送钳在上限）、限制器关加 2.0（软削波介入，|回送| < 1）；
//   - 输出与参照安全链按文档接线重算的结果逐位相同；D 恒为 4800，零重同步，漂移为 0。
//
// 标 ⛔ 的是删除式验证点：把对应的实现改坏，本场景必须变红。
//   ⛔ 回送链跳过 DC 阻断（processFront 不做）→ 第一个回声之后的尾巴与参考模型相差超过 1e-5；
//   ⛔ 环路延迟多加一个样本 → 峰位不在 k·D；
//   ⛔ 参考模型去掉 DC 阻断仍与引擎相符 → 对照项变红。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "s01_rig_kit.h"

namespace
{

using namespace rinne::rig;
using namespace rinne::rig::scn;
using rinne::engine::SafetyState;

constexpr int kBlock = 256;
constexpr std::int64_t kDelay = 4800;
// 首个有效读样本在 D，淡入再走 500 ms：冲激放在淡入走完之后
constexpr std::uint64_t kImpulseAt = 30100;
constexpr int kEchoes = 6;
constexpr double kBound = 1.0e-5;

// float64 参考模型的开关与增益
struct Model
{
    double gain = 0.5;
    bool dcBlock = true;
    bool limiterOn = true;
    double ceilingDb = -6.0;
};

// 冲激幅度 amplitude 打在第 0 个样本上，之后 length 个样本的输出（干声 + 回送）。
// 滤波系数取引擎同样的 float 值（先舍入成 float 再提成 double），算术用 double。
std::vector<double> modelOutput(double amplitude, const Model& model, std::size_t length)
{
    constexpr double kTwoPi = 6.283185307179586476925286766559;
    const double r = static_cast<double>(static_cast<float>(std::exp(-kTwoPi * 5.0 / kSr)));
    const double release = static_cast<double>(static_cast<float>(std::exp(-1.0 / (0.080 * kSr))));
    const double ceiling = static_cast<double>(static_cast<float>(std::pow(10.0, model.ceilingDb / 20.0)));

    std::vector<double> written(length, 0.0);
    double x1 = 0.0;
    double y1 = 0.0;
    double reduction = 0.0;
    for (std::size_t t = 0; t < length; ++t)
    {
        const double tap = t >= static_cast<std::size_t>(kDelay) ? written[t - static_cast<std::size_t>(kDelay)] : 0.0;
        double y = tap;
        if (model.dcBlock)
        {
            y = tap - x1 + r * y1;
            x1 = tap;
            y1 = y;
        }
        double x = y * model.gain;
        if (model.limiterOn)
        {
            const double peak = std::fabs(x);
            const double needed = peak > ceiling ? 1.0 - ceiling / peak : 0.0;
            reduction = std::max(needed, reduction * release);
            x = std::clamp(x * (1.0 - reduction), -ceiling, ceiling);
        }
        const double a = std::fabs(x);
        if (a > 0.7)
        {
            const double soft = std::min({0.7 + 0.3 * std::tanh((a - 0.7) / 0.3), a, 0.99999994});
            x = x < 0.0 ? -soft : soft;
        }
        written[t] = (t == 0 ? amplitude : 0.0) + x;
    }
    return written;
}

double maxError(const std::vector<float>& out, const std::vector<double>& model)
{
    double worst = 0.0;
    for (std::size_t t = 0; t < model.size(); ++t)
    {
        worst = std::max(worst, std::fabs(static_cast<double>(out[kImpulseAt + t]) - model[t]));
    }
    return worst;
}

class ImpulseEchoScenario final : public Scenario
{
public:
    const char* id() const noexcept override { return "s01_impulse_echo"; }

    bool run(ScenarioContext&) override
    {
        Verdict v;
        // 腿一：线性区。0.5 的冲激经 ×0.5 是 0.25，限制器（上限 −6 dBFS = 0.5012）与软削波（0.7）都不介入
        leg(v, 0.5f, true, true);
        // 腿二：热冲激。1.2 经 ×0.5 是 0.6，超过上限：限制器介入，回送钳在上限
        leg(v, 1.2f, true, false);
        // 腿三：限制器关，2.0 经 ×0.5 是 1.0：软削波介入
        leg(v, 2.0f, false, false);
        return v.ok();
    }

private:
    void leg(Verdict& v, float amplitude, bool limiterOn, bool linear)
    {
        INFO("amplitude = " << amplitude << ", limiter = " << limiterOn);
        auto host = makeHost(kSr, kBlock);
        REQUIRE(host != nullptr);
        host->setBlockScripts(fixedBlocks(kBlock), fixedBlocks(kBlock));
        host->setChain(ChainKind::kIdentity);
        Recorder rec(*host, impulseSource(kImpulseAt, amplitude));
        rinne::engine::EngineParams p = loopParams(50.0f, 50.0f, 100.0f);
        p.limiterOn = limiterOn;

        // 冲激之前淡入已经走完：状态回到 Run
        rec.run(p, static_cast<std::int64_t>(kImpulseAt) - 100);
        RIG_EQ(v, host->inputSafetyDiag().state, static_cast<std::uint8_t>(SafetyState::Run));
        rec.run(p, static_cast<std::int64_t>((kEchoes + 1) * kDelay) + 100);

        const auto at = static_cast<std::size_t>(kImpulseAt);
        const auto d = static_cast<std::size_t>(kDelay);
        RIG_CHECK(v, rec.outLeft.size() > at + (kEchoes + 1) * d);
        if (rec.outLeft.size() <= at + (kEchoes + 1) * d)
        {
            return;
        }

        // 干声里只有这一个冲激；冲激之前整条输出都是 0
        std::size_t nonZero = 0;
        for (const float x : rec.dryLeft)
        {
            nonZero += x != 0.0f ? 1u : 0u;
        }
        RIG_EQ(v, nonZero, std::size_t{1});
        RIG_EQ(v, rec.outLeft[at], amplitude);
        RIG_EQ(v, rec.outRight[at], amplitude);
        RIG_CHECK(v, allZero(rec.outLeft, 0, at) && allZero(rec.outRight, 0, at));

        // 峰位：k = 1…6，窗口是以 k·D 为中心的一整个 D，峰恰在冲激位置 + k·D（左右声道各查）
        for (std::size_t k = 1; k <= kEchoes; ++k)
        {
            const std::size_t from = at + k * d - d / 2;
            const std::size_t to = at + k * d + d / 2;
            INFO("k = " << k);
            RIG_EQ(v, argmaxAbs(rec.outLeft, from, to), at + k * d);
            RIG_EQ(v, argmaxAbs(rec.outRight, from, to), at + k * d);
        }

        // 与 float64 参考模型逐样本相差不超过 1e-5
        Model model;
        model.limiterOn = limiterOn;
        const std::size_t length = (kEchoes + 1) * d;
        const std::vector<double> want = modelOutput(amplitude, model, length);
        const double errLeft = maxError(rec.outLeft, want);
        const double errRight = maxError(rec.outRight, want);
        INFO("max error left = " << errLeft << ", right = " << errRight);
        RIG_CHECK(v, errLeft <= kBound);
        RIG_CHECK(v, errRight <= kBound);
        // 对照：参考模型去掉 DC 阻断，与引擎的差必须大于界
        Model noDc = model;
        noDc.dcBlock = false;
        const double errNoDc = maxError(rec.outLeft, modelOutput(amplitude, noDc, length));
        INFO("without the DC stage the model is off by " << errNoDc);
        RIG_CHECK(v, errNoDc > kBound);

        if (linear)
        {
            // 线性区：第一个回声逐位是 0.25f（0.5f 经 DC 阻断原样通过，× 0.5 精确，限制器与软削波不碰它）
            RIG_EQ(v, rinne::test::floatBits(rec.outLeft[at + d]), rinne::test::floatBits(0.25f));
        }
        else if (limiterOn)
        {
            // 限制器开：回送从不超过上限，第一个回声被钳在上限
            const float ceiling = std::pow(10.0f, -6.0f / 20.0f);
            RIG_CHECK(v, peakOf(rec.outLeft, at + 1, rec.outLeft.size()) <= ceiling * 1.0000001f);
            RIG_CHECK(v, std::fabs(rec.outLeft[at + d] - ceiling) < 1.0e-6f);
        }
        else
        {
            // 软削波：0.7 + 0.3·tanh(1.0)，回送始终小于 1.0
            RIG_CHECK(v, std::fabs(rec.outLeft[at + d] - 0.92848f) < 1.0e-4f);
            RIG_CHECK(v, peakOf(rec.outLeft, at + 1, rec.outLeft.size()) < 1.0f);
        }

        // 环路层：D 恒为 4800，没有重同步，漂移为 0；输出与参照安全链逐位相同
        const auto& status = host->inputStatus();
        RIG_EQ(v, status.delayEffective, kDelay);
        RIG_EQ(v, status.resyncCount, 0u);
        RIG_EQ(v, status.drift, std::int64_t{0});
        RIG_CHECK(v, rec.matchesReference());
    }
};

} // namespace

RINNE_RIG_SCENARIO("s01_impulse_echo")
{
    return new ImpulseEchoScenario();
}

TEST_CASE("s01: an impulse echoes at exactly k times D and matches the float64 model", "[loop_rig][s01]")
{
    CHECK(rinne::rig::scn::runRegisteredScenario("s01_impulse_echo"));
}
