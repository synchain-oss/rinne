// SPDX-License-Identifier: GPL-3.0-or-later
//
// InputEngine 的止啸路径与过渡：
//   紧急切断 ≤ 1 块 + 10 ms 归零并清空本地延迟线（冷却之后恢复，旧内容不回来）；
//   loop_on 关与 Input 旁路 ≤ 1 块 + 10 ms 归零、不清空（打开之后旧内容照常回来）；
//   Output 旁路：读头在 D + 10 ms 之内逐位为 0，回送按 DC 阻断的尾巴衰减，安全链照常运行；
//   写头恰好在块边界落后：回送保持末值 5 ms 线性衰减，不从上一块的末值直接落到 0；
//   写头倒退（读头整体作废）：同样保持末值 5 ms 衰减；
//   两端停用超过 250 ms 之后的清空：保持末值 5 ms 衰减，停用之前的内容不回来；
//   新配对、换写方：从首个有效读样本起 500 ms 淡入（从运行中进入时先 10 ms 降到 0），逐样本无阶跃。
//
// 阶跃判据用低频余弦：相邻差不超过「信号自身的最大斜率 + 淡变的一步 + ε」。每条判据都配一条硬切换的反例
// （同一段轨迹在切换点直接落到 0），证明判据对阶跃会变红。
// 标 ⛔ 的是删除式验证点：把对应的实现改坏，本用例必须变红。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "EngineTestKit.h"
#include "core/dsp/DspTestKit.h"
#include "diag/EventQueue.h"
#include "engine/InputEngine.h"
#include "engine/SafetyEvents.h"
#include "loop/LoopEvents.h"

namespace
{

using namespace rinne::engine;
using rinne::test::ClosedLoop;
using rinne::test::firstBitDiff;
using rinne::test::maxStep;

constexpr double kSr = 48000.0;
constexpr double kPi = rinne::test::kPi;
constexpr std::int64_t kFadeOut = 240; // 5 ms
constexpr std::int64_t kGate = 480; // 10 ms
constexpr std::int64_t kFadeIn = 24000; // 500 ms

float cosine(std::int64_t t, double hz, float amplitude)
{
    return amplitude * static_cast<float>(std::cos(2.0 * kPi * hz * static_cast<double>(t) / kSr));
}

// 余弦每个样本最多变 A·ω
float slope(double hz, float amplitude)
{
    return amplitude * static_cast<float>(2.0 * kPi * hz / kSr);
}

EngineParams loopParams(float feedbackPercent, float mixPercent, float delayMs)
{
    EngineParams p;
    p.feedbackPercent = feedbackPercent;
    p.mixPercent = mixPercent;
    p.delayMs = delayMs;
    return p;
}

float peak(const std::vector<float>& trace, std::size_t from, std::size_t to)
{
    float m = 0.0f;
    for (std::size_t i = from; i < std::min(to, trace.size()); ++i)
    {
        m = std::max(m, std::fabs(trace[i]));
    }
    return m;
}

// 同一段轨迹在 cut 起直接落到 0：判据的反例
std::vector<float> hardCut(std::vector<float> trace, std::size_t cut)
{
    std::fill(trace.begin() + static_cast<std::ptrdiff_t>(cut), trace.end(), 0.0f);
    return trace;
}

bool allZero(const std::vector<float>& trace, std::size_t from, std::size_t to)
{
    for (std::size_t i = from; i < std::min(to, trace.size()); ++i)
    {
        if (trace[i] != 0.0f)
        {
            return false;
        }
    }
    return true;
}

bool sameFrom(const std::vector<float>& a, const std::vector<float>& b, std::size_t from, std::size_t to)
{
    for (std::size_t i = from; i < std::min(to, a.size()); ++i)
    {
        if (rinne::test::floatBits(a[i]) != rinne::test::floatBits(b[i]))
        {
            return false;
        }
    }
    return true;
}

struct FadeLog
{
    std::vector<FadeReason> reasons;
    static void record(void* context, FadeReason reason) noexcept
    {
        static_cast<FadeLog*>(context)->reasons.push_back(reason);
    }
};

} // namespace

TEST_CASE("Input 紧急切断：1 块 + 10 ms 之内回送归零并清空本地延迟线；冷却之后恢复，切断之前的内容不回来",
          "[engine][input][stop]")
{
    // ⛔ 删除式验证点：跳闸淡出走完之后不清本地延迟线 → 恢复之后读头读回切断之前 3 s 的内容，本用例变红；
    //    紧急切断不交给安全链 → 回送不归零，本用例变红。
    // D = 3 s，比 1.5 s 的冷却长：恢复的时候延迟线里还是切断之前写下的内容，清没清一看便知。
    constexpr int kN = 256;
    constexpr std::int64_t kD = 3 * 48000;
    ClosedLoop loop(kSr, kN);
    loop.source = [](std::int64_t t, int channel) { return cosine(t, channel == 0 ? 100.0 : 150.0, 0.3f); };
    EngineParams p = loopParams(50.0f, 50.0f, 3000.0f);
    loop.run(p, kN, 4 * 48000 + 64 * kN);
    REQUIRE(loop.state() == SafetyState::Run);
    REQUIRE(firstBitDiff(loop.outLeft, loop.dryLeft) != -1); // 回送在响

    const auto panicAt = static_cast<std::size_t>(loop.at);
    p.panicSeq = 1;
    loop.run(p, kN, 77 * 1024); // 约 1.64 s，过了 1.5 s 冷却
    CHECK(loop.state() == SafetyState::Tripped);
    CHECK(PackedSafety::trip(loop.input.meters().safety.load()) == TripReason::Panic);
    const auto settled = panicAt + kN + kGate; // 1 块 + 10 ms
    CHECK(sameFrom(loop.outLeft, loop.dryLeft, settled, loop.outLeft.size()));
    CHECK(allZero(loop.reference.tapLeft, settled, loop.outLeft.size())); // 清空之后读头只读得到新写的帧

    p.recoverSeq = 1;
    loop.run(p, kN, 2 * 48000);
    const auto recovered = panicAt + static_cast<std::size_t>(kD); // 清空之后的新数据 D 之后才读得到
    CHECK(allZero(loop.reference.tapLeft, settled, recovered));
    CHECK(allZero(loop.reference.tapRight, settled, recovered));
    CHECK(sameFrom(loop.outLeft, loop.dryLeft, settled, recovered));
    // 恢复照常：新数据从首个有效读样本起淡入
    CHECK(loop.state() != SafetyState::Tripped);
    CHECK(firstBitDiff(loop.outLeft, loop.dryLeft) != -1);
    CHECK(!sameFrom(loop.outLeft, loop.dryLeft, recovered + kGate, loop.outLeft.size()));
    CHECK(firstBitDiff(loop.outLeft, loop.reference.wantLeft) == -1);
    CHECK(firstBitDiff(loop.outRight, loop.reference.wantRight) == -1);
}

TEST_CASE("Input loop_on 关与 Input 旁路：1 块 + 10 ms 之内回送归零、不清空，打开之后旧内容照常回来",
          "[engine][input][stop]")
{
    // ⛔ 删除式验证点：loop_on 关 / 旁路不交给安全链的环路门 → 回送不归零，本用例变红；
    //    关的时候清了本地延迟线 → 打开之后读头读不到关之前的内容，本用例变红。
    constexpr int kN = 256;
    constexpr std::int64_t kD = 24000; // 500 ms，比关着的 300 ms 长
    for (const bool viaBypass : {false, true})
    {
        ClosedLoop loop(kSr, kN);
        loop.source = [](std::int64_t t, int) { return cosine(t, 100.0, 0.3f); };
        EngineParams p = loopParams(70.0f, 50.0f, 500.0f);
        loop.run(p, kN, 2 * 48000 + 64 * kN);
        REQUIRE(loop.state() == SafetyState::Run);
        const auto offAt = static_cast<std::size_t>(loop.at);
        if (viaBypass)
        {
            loop.inputBypassed = true;
        }
        else
        {
            p.loopOn = false;
        }
        loop.run(p, kN, 56 * kN); // 约 300 ms
        const auto onAt = static_cast<std::size_t>(loop.at);
        CHECK(sameFrom(loop.outLeft, loop.dryLeft, offAt + kN + kGate, onAt));
        loop.inputBypassed = false;
        p.loopOn = true;
        loop.run(p, kN, 48000);

        // 不清空：读头逐位就是 D 个样本之前 Input 的输出，关着的那段也照常；打开之后读到的是关之前写下的内容
        const auto& tap = loop.reference.tapLeft;
        std::size_t delayed = 0;
        for (std::size_t t = static_cast<std::size_t>(kD) + 2000; t < tap.size(); ++t)
        {
            delayed += tap[t] == loop.outLeft[t - static_cast<std::size_t>(kD)] ? 0u : 1u;
        }
        CHECK(delayed == 0u);
        CHECK(peak(tap, onAt, offAt + static_cast<std::size_t>(kD)) > 0.1f);
        CHECK(loop.input.loopStatus().resyncCount == 0u);
        // 打开之后回送 10 ms 回来
        CHECK(!sameFrom(loop.outLeft, loop.dryLeft, onAt + kGate, onAt + 4 * kGate));
        CHECK(loop.state() == SafetyState::Run);
        CHECK(firstBitDiff(loop.outLeft, loop.reference.wantLeft) == -1);
    }
}

TEST_CASE("Input Output 旁路：读头在 D + 10 ms 之内逐位为 0，回送按 DC 阻断的尾巴衰减，安全链照常运行",
          "[engine][input][stop]")
{
    // ⛔ 删除式验证点：Output 旁路不把写进环的数据淡到 0 → 读头不归零，本用例变红。
    constexpr int kN = 256;
    constexpr std::int64_t kD = 4800; // 100 ms
    ClosedLoop loop(kSr, kN);
    loop.source = [](std::int64_t t, int) { return cosine(t, 100.0, 0.2f); };
    const EngineParams p = loopParams(50.0f, 50.0f, 100.0f);
    loop.run(p, kN, 48000 + 64 * kN);
    REQUIRE(loop.state() == SafetyState::Run);
    const auto bypassAt = static_cast<std::size_t>(loop.at);
    loop.outputBypassed = true;
    loop.run(p, kN, 3 * 48000 / 2);

    const auto zeroFrom = bypassAt + static_cast<std::size_t>(kD + kGate);
    CHECK(allZero(loop.reference.tapLeft, zeroFrom, loop.outLeft.size()));
    CHECK(allZero(loop.reference.tapRight, zeroFrom, loop.outLeft.size()));
    // 回送：DC 阻断的尾巴，每 100 ms 的峰值只降不升，1 s 之后小于 1e-6
    const auto& ret = loop.reference.retLeft;
    CHECK(peak(ret, zeroFrom, zeroFrom + 4800) > 0.0f);
    for (std::size_t k = 1; k < 10; ++k)
    {
        const std::size_t a = zeroFrom + (k - 1) * 4800;
        const std::size_t b = zeroFrom + k * 4800;
        CHECK(peak(ret, b, b + 4800) <= peak(ret, a, a + 4800));
    }
    CHECK(peak(ret, zeroFrom + 48000, loop.outLeft.size()) < 1.0e-6f);
    // 安全链照常运行：没有淡出、没有跳闸，对端仍在
    CHECK(loop.state() == SafetyState::Run);
    CHECK(loop.input.safetyChain().machine().safetyGain() == 1.0f);
    CHECK(firstBitDiff(loop.outLeft, loop.reference.wantLeft) == -1);
}

TEST_CASE("Input 写头恰好在块边界落后：回送保持末值 5 ms 线性衰减，不从上一块的末值直接落到 0", "[engine][input][stop]")
{
    // ⛔ 删除式验证点：块边界上落后时不做末值保持（读头本块不出声、直接释放）→ 回送从末值一步落到 0，本用例变红。
    // D = 480 是块长 240 的整数倍：Output 停写之后第 2 块起，主读头本块一个可读的样本都没有。
    constexpr int kN = 240;
    static constexpr double kHz = 100.0;
    static constexpr float kA = 0.3f;
    SECTION("读头（安全链的输入）")
    {
        ClosedLoop loop(kSr, kN);
        loop.source = [](std::int64_t t, int) { return cosine(t, kHz, kA); };
        const EngineParams p = loopParams(0.0f, 50.0f, 10.0f); // 反馈 0：读头就是 D 之前的干声
        loop.run(p, kN, 100 * kN);
        loop.outputActive = false;
        loop.run(p, kN, 20 * kN);
        const auto cut = static_cast<std::size_t>(102 * kN);
        const auto& tap = loop.reference.tapLeft;
        REQUIRE(std::fabs(tap[cut - 1]) > 0.2f); // 末值不小：硬切换会是一个大阶跃
        CHECK(tap[cut] == tap[cut - 1]); // 保持的第一个样本就是末值
        for (std::size_t i = 0; i < static_cast<std::size_t>(kFadeOut); ++i)
        {
            const float want = tap[cut - 1] * static_cast<float>(kFadeOut - static_cast<std::int64_t>(i)) /
                               static_cast<float>(kFadeOut);
            CHECK(std::fabs(tap[cut + i] - want) <= 1.0e-6f);
        }
        CHECK(allZero(tap, cut + kFadeOut, tap.size()));
        const float bound = slope(kHz, kA) + kA / static_cast<float>(kFadeOut) + 1.0e-6f;
        CHECK(maxStep(tap, cut - kN, cut + 2 * kFadeOut) <= bound);
        CHECK(maxStep(hardCut(tap, cut), cut - kN, cut + 2 * kFadeOut) > bound); // 反例：判据对阶跃会红
    }
    SECTION("回送（安全链的输出）")
    {
        ClosedLoop loop(kSr, kN);
        loop.source = [](std::int64_t t, int) { return cosine(t, kHz, kA); };
        const EngineParams p = loopParams(50.0f, 50.0f, 10.0f);
        loop.run(p, kN, 400 * kN);
        loop.outputActive = false;
        loop.run(p, kN, 20 * kN);
        const auto cut = static_cast<std::size_t>(402 * kN);
        const auto& ret = loop.reference.retLeft;
        const float level = peak(ret, cut - 4800, cut);
        REQUIRE(std::fabs(ret[cut - 1]) > 0.25f * level);
        // 回送里的 DC 阻断与增益都是连续的：阶跃界取稳态里最大的一步，加上末值保持的一步
        const float steady = maxStep(ret, cut - 4800, cut);
        const float bound = steady + level / static_cast<float>(kFadeOut) + 1.0e-6f;
        CHECK(maxStep(ret, cut - 1, cut + 2 * kFadeOut) <= bound);
        CHECK(maxStep(hardCut(ret, cut), cut - 1, cut + 2 * kFadeOut) > bound);
        CHECK(firstBitDiff(loop.outLeft, loop.reference.wantLeft) == -1);
    }
}

TEST_CASE("Input 写头倒退：读头整体作废时回送保持末值 5 ms 线性衰减，不从上一块的末值直接落到 0",
          "[engine][input][stop]")
{
    // ⛔ 删除式验证点：写头倒退时不做末值保持（直接清读头）→ 回送从末值一步落到 0，本用例变红。
    // 同一个段、同一个映射代号上写头回到 0（换段遗漏一类）：环路层整个域重来、读头作废，按换绑定请求淡入。
    constexpr int kN = 240;
    static constexpr double kHz = 100.0;
    static constexpr float kA = 0.3f;
    const auto rewind = [](ClosedLoop& loop) {
        loop.ring->header.write_intent.store(0u);
        loop.ring->header.write_head.store(0u);
    };
    SECTION("读头（安全链的输入）")
    {
        ClosedLoop loop(kSr, kN);
        loop.source = [](std::int64_t t, int) { return cosine(t, kHz, kA); };
        const EngineParams p = loopParams(0.0f, 50.0f, 10.0f); // 反馈 0：读头就是 D 之前的干声
        loop.run(p, kN, 100 * kN);
        rewind(loop);
        loop.run(p, kN, 20 * kN);
        const auto cut = static_cast<std::size_t>(100 * kN);
        const auto& tap = loop.reference.tapLeft;
        REQUIRE(std::fabs(tap[cut - 1]) > 0.2f); // 末值不小：硬切换会是一个大阶跃
        CHECK(loop.input.loopStatus().resyncByReason[static_cast<int>(rinne::loop::SyncReason::kBindSwitch)] == 1u);
        CHECK(tap[cut] == tap[cut - 1]); // 保持的第一个样本就是末值
        for (std::size_t i = 0; i < static_cast<std::size_t>(kFadeOut); ++i)
        {
            const float want = tap[cut - 1] * static_cast<float>(kFadeOut - static_cast<std::int64_t>(i)) /
                               static_cast<float>(kFadeOut);
            CHECK(std::fabs(tap[cut + i] - want) <= 1.0e-6f);
        }
        CHECK(allZero(tap, cut + kFadeOut, cut + 480)); // 倒退之前的数据不读；新数据 D 之后才到
        CHECK(peak(tap, cut + 480 + 960, tap.size()) > 0.2f); // 之后照常读新数据
        const float bound = slope(kHz, kA) + kA / static_cast<float>(kFadeOut) + 1.0e-6f;
        CHECK(maxStep(tap, cut - kN, cut + 2 * kFadeOut) <= bound);
        CHECK(maxStep(hardCut(tap, cut), cut - kN, cut + 2 * kFadeOut) > bound); // 反例：判据对阶跃会红
    }
    SECTION("回送（安全链的输出）")
    {
        ClosedLoop loop(kSr, kN);
        loop.source = [](std::int64_t t, int) { return cosine(t, kHz, kA); };
        const EngineParams p = loopParams(50.0f, 50.0f, 10.0f);
        loop.run(p, kN, 400 * kN);
        rewind(loop);
        loop.run(p, kN, 20 * kN);
        const auto cut = static_cast<std::size_t>(400 * kN);
        const auto& ret = loop.reference.retLeft;
        const float level = peak(ret, cut - 4800, cut);
        REQUIRE(std::fabs(ret[cut - 1]) > 0.25f * level);
        // 阶跃界：稳态里最大的一步，加上末值保持的一步与安全链 10 ms 降到 0 的一步（换绑定请求淡入，从运行中进入）
        const float steady = maxStep(ret, cut - 4800, cut);
        const float bound = steady + level / static_cast<float>(kFadeOut) + level / static_cast<float>(kGate) + 1.0e-6f;
        CHECK(maxStep(ret, cut - 1, cut + 2 * kFadeOut) <= bound);
        CHECK(maxStep(hardCut(ret, cut), cut - 1, cut + 2 * kFadeOut) > bound);
        CHECK(firstBitDiff(loop.outLeft, loop.reference.wantLeft) == -1);
    }
}

TEST_CASE("Input 两端停用超过 250 ms 之后的清空：保持末值 5 ms 衰减，停用之前的内容不回来", "[engine][input][stop]")
{
    // ⛔ 删除式验证点：不理会清空请求 → 恢复之后读头接着读停用之前的内容，本用例变红；
    //    清空时不做末值保持（直接作废读头）→ 回送从末值一步落到 0，本用例变红。
    constexpr int kN = 256;
    static constexpr double kHz = 100.0;
    static constexpr float kA = 0.3f;
    constexpr std::int64_t kD = 24000;
    ClosedLoop loop(kSr, kN);
    loop.source = [](std::int64_t t, int) { return cosine(t + 289, kHz, kA); }; // 停用前最后一个读头样本落在峰上
    const EngineParams p = loopParams(0.0f, 50.0f, 500.0f);
    loop.run(p, kN, 3 * 48000 / 2);
    // 宿主把两端一起挂起（这期间谁都不被调用，样本轴不前进），壳判定超过 250 ms，请求清空
    loop.input.requestFlushAfterSuspend();
    const auto resume = static_cast<std::size_t>(loop.at);
    loop.run(p, kN, 48000);

    const auto& tap = loop.reference.tapLeft;
    REQUIRE(std::fabs(tap[resume - 1]) > 0.2f);
    CHECK(tap[resume] == tap[resume - 1]);
    CHECK(allZero(tap, resume + kFadeOut, resume + static_cast<std::size_t>(kD))); // 旧内容不回来
    CHECK(peak(tap, resume + static_cast<std::size_t>(kD) + 2000, tap.size()) > 0.2f); // 之后照常读新数据
    const float bound = slope(kHz, kA) + kA / static_cast<float>(kFadeOut) + 1.0e-6f;
    CHECK(maxStep(tap, resume - kN, resume + 2 * kFadeOut) <= bound);
    CHECK(maxStep(hardCut(tap, resume), resume - kN, resume + 2 * kFadeOut) > bound);
    bool cleared = false;
    loop.input.drainEvents([&](const rinne::diag::Event& e) {
        cleared =
            cleared || (e.code == static_cast<std::uint16_t>(rinne::loop::LoopEventCode::kLineCleared) && e.a == 2);
    });
    CHECK(cleared);
    CHECK(firstBitDiff(loop.outLeft, loop.reference.wantLeft) == -1);
}

TEST_CASE("Input 新配对：从首个有效读样本起 500 ms 线性淡入，逐样本无阶跃", "[engine][input][fade]")
{
    // ⛔ 删除式验证点：有效位不交给安全链（一律当有效）→ 淡入从首块就起，D 之后数据一到已经是半满，本用例变红。
    constexpr int kN = 128;
    ClosedLoop loop(kSr, kN);
    loop.source = [](std::int64_t t, int) { return cosine(t, 100.0, 0.2f); };
    const EngineParams p = loopParams(50.0f, 50.0f, 50.0f);
    std::vector<float> gains; // 每块末的 g_safety
    while (loop.at < 48000)
    {
        loop.block(p, kN);
        gains.push_back(loop.input.safetyChain().machine().safetyGain());
    }
    const auto& valid = loop.reference.tapValid;
    const auto first = static_cast<std::int64_t>(std::find(valid.begin(), valid.end(), 1) - valid.begin());
    REQUIRE(first >= 2400); // D = 50 ms 之前没有有效数据
    for (std::size_t k = 0; k < gains.size(); ++k)
    {
        const std::int64_t last = static_cast<std::int64_t>(k + 1) * kN - 1;
        const double want = std::clamp(static_cast<double>(last - first + 1) / static_cast<double>(kFadeIn), 0.0, 1.0);
        CHECK(std::fabs(gains[k] - want) <= 1.0e-6);
    }
    const auto& ret = loop.reference.retLeft;
    const float bound = slope(100.0, 0.4f) + 0.4f / static_cast<float>(kFadeIn) + 1.0e-6f;
    CHECK(maxStep(ret, 0, ret.size()) <= bound);
    // 反例：环路层的 20 ms onset 走完之后不再淡入、直接满增益接上读头
    const auto& tap = loop.reference.tapLeft;
    auto jump = static_cast<std::size_t>(first + 960);
    while (jump < tap.size() && std::fabs(tap[jump]) < 0.15f)
    {
        ++jump;
    }
    REQUIRE(jump < tap.size());
    std::vector<float> hard = ret;
    std::copy(tap.begin() + static_cast<std::ptrdiff_t>(jump), tap.end(),
              hard.begin() + static_cast<std::ptrdiff_t>(jump));
    CHECK(maxStep(hard, 0, hard.size()) > bound);
    CHECK(firstBitDiff(loop.outLeft, loop.reference.wantLeft) == -1);
}

TEST_CASE("Input 换写方：先 10 ms 降到 0，再从首个有效读样本起 500 ms 淡入，逐样本无阶跃，原因是 NewWriter",
          "[engine][input][fade]")
{
    // ⛔ 删除式验证点：环路层报了换写方而引擎不向安全链请求淡入 → 回送照常满增益接上新写方，本用例变红。
    constexpr int kN = 128;
    ClosedLoop loop(kSr, kN);
    FadeLog log;
    loop.input.setFadeObserver(&log, &FadeLog::record);
    loop.source = [](std::int64_t t, int channel) { return cosine(t, channel == 0 ? 100.0 : 80.0, 0.2f); };
    const EngineParams p = loopParams(50.0f, 50.0f, 50.0f);
    loop.run(p, kN, 48000 + 64 * kN);
    REQUIRE(loop.state() == SafetyState::Run);
    REQUIRE(log.reasons.empty());
    const auto switchAt = static_cast<std::size_t>(loop.at);
    loop.switchWriter();
    std::vector<float> gains;
    std::vector<SafetyState> states;
    while (loop.at < static_cast<std::int64_t>(switchAt) + 48000)
    {
        loop.block(p, kN);
        gains.push_back(loop.input.safetyChain().machine().safetyGain());
        states.push_back(loop.state());
    }
    REQUIRE(log.reasons.size() == 1u);
    CHECK(log.reasons[0] == FadeReason::NewWriter);
    CHECK(states[0] == SafetyState::FadeIn);
    CHECK(PackedSafety::fade(loop.input.meters().safety.load()) == FadeReason::NewWriter);
    // 先 10 ms 降到 0：切换之后第 k 块末 g = 1 − 已走样本数 / 480
    for (std::size_t k = 0; (k + 1) * kN <= static_cast<std::size_t>(kGate); ++k)
    {
        const double want = 1.0 - static_cast<double>((k + 1) * kN) / static_cast<double>(kGate);
        CHECK(std::fabs(gains[k] - want) <= 1.0e-6);
    }
    // 降到 0 之后从首个有效读样本起 500 ms 上行；走完回到 Run
    const auto& valid = loop.reference.tapValid;
    const auto firstAfter =
        std::find(valid.begin() + static_cast<std::ptrdiff_t>(switchAt) + kGate, valid.end(), 1) - valid.begin();
    const std::size_t upEnd = static_cast<std::size_t>(firstAfter) + static_cast<std::size_t>(kFadeIn);
    CHECK(states.back() == SafetyState::Run);
    CHECK(states[(upEnd - switchAt) / kN - 2] == SafetyState::FadeIn);
    // 阶跃界：信号的斜率，加上旧读头 5 ms 尾巴淡出与 10 ms 下行叠在一起的一步（level/240 + level/480）
    const auto& ret = loop.reference.retLeft;
    const float level = peak(ret, switchAt - 4800, switchAt);
    const float bound = slope(100.0, level) + level / 240.0f + level / static_cast<float>(kGate) + 1.0e-6f;
    REQUIRE(level > 0.1f);
    CHECK(maxStep(ret, switchAt - 4800, ret.size()) <= bound);
    // 反例：在切换前最响的那个样本之后直接落到 0
    std::size_t loudest = switchAt - kGate;
    for (std::size_t t = switchAt - kGate; t < switchAt; ++t)
    {
        loudest = std::fabs(ret[t]) > std::fabs(ret[loudest]) ? t : loudest;
    }
    CHECK(maxStep(hardCut(ret, loudest + 1), switchAt - 4800, ret.size()) > bound);
    CHECK(firstBitDiff(loop.outLeft, loop.reference.wantLeft) == -1);
    CHECK(firstBitDiff(loop.outRight, loop.reference.wantRight) == -1);
}
