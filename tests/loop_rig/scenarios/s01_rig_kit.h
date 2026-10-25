// SPDX-License-Identifier: GPL-3.0-or-later
//
// 首批场景共用的小工具：判定计数、确定性的信号、参数快照、建主机、逐周期的记录器。
//
// 记录器每个周期做三件事：设这一周期的参数，跑一周期，把「Input 之前的干声」「Input 之后的输出」
// 记下来，同时按引擎文档写明的接线次序喂一条独立构造的参照安全链（EngineTestKit 的 InputReference）。
// 干声由信号源这个纯函数按绝对样本号重算（信号源必须是绝对样本号的纯函数），所以记录器不改任何宿主缓冲。
// 场景用「输出与参照逐位相同」把接线钉死，再在这之上断言自己那一项的精确值。
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/dsp/DspTestKit.h"
#include "core/engine/EngineTestKit.h"
#include "rig/EngineHost.h"
#include "rig/ScenarioRegistry.h"

namespace rinne::rig::scn
{

inline constexpr double kSr = 48000.0;
inline constexpr std::int64_t kFadeOutSamples = 240; // 5 ms
inline constexpr std::int64_t kGateSamples = 480; // 10 ms
inline constexpr std::int64_t kFadeInSamples = 24000; // 500 ms

// ---- 判定 ----

// 一个场景的判定：每条断言都经 Catch2 的 CHECK 报出（失败时能看到是哪一条、值是多少），同时数失败条数，
// 场景的 run() 据此返回成败。
struct Verdict
{
    int failed = 0;
    bool ok() const noexcept { return failed == 0; }
};

#define RIG_CHECK(verdict, ...)                              \
    do                                                       \
    {                                                        \
        const bool rigPass = static_cast<bool>(__VA_ARGS__); \
        INFO(#__VA_ARGS__);                                  \
        CHECK(rigPass);                                      \
        if (!rigPass)                                        \
        {                                                    \
            ++(verdict).failed;                              \
        }                                                    \
    } while (false)

// 两个值相等；失败时报出两边的值
#define RIG_EQ(verdict, a, b)       \
    do                              \
    {                               \
        const auto rigLeft = (a);   \
        const auto rigRight = (b);  \
        INFO(#a " == " #b);         \
        CHECK(rigLeft == rigRight); \
        if (!(rigLeft == rigRight)) \
        {                           \
            ++(verdict).failed;     \
        }                           \
    } while (false)

// 当前正在跑的场景与本次运行里记到的空转次数：周期没有处理任何样本时点名用
struct RunState
{
    const char* scenario = "(none)";
    int stalls = 0;
};

inline RunState& runState()
{
    static RunState state;
    return state;
}

// 一个周期没有处理任何样本（没 prepare、块脚本跳过或给了 0 样本、只有 Output 一端）：当场记一次失败并点名场景与周期号，
// 不让上层的「跑满 N 个样本」循环一直空转到超时
inline void reportStall(std::size_t cycleIndex, int samples)
{
    ++runState().stalls;
    FAIL_CHECK("scenario " << runState().scenario << ": cycle " << cycleIndex << " handled " << samples
                           << " samples, the run cannot advance (host not prepared, a skip or zero-length block in the "
                              "script, or only the output side ran)");
}

// ---- 信号 ----

inline float cosine(std::uint64_t t, double hz, float amplitude)
{
    return amplitude * static_cast<float>(std::cos(2.0 * test::kPi * hz * static_cast<double>(t) / kSr));
}

// 余弦每个样本最多变 A·ω
inline float slope(double hz, float amplitude)
{
    return amplitude * static_cast<float>(2.0 * test::kPi * hz / kSr);
}

// 确定性的哈希噪声，值在 [-1, 1)：编号和种子的纯函数，跨平台逐位相同
inline float hashNoise(std::uint64_t index, std::uint64_t seed)
{
    std::uint64_t z = index * 0x9e3779b97f4a7c15ull + seed * 0xbf58476d1ce4e5b9ull + 0x94d049bb133111ebull;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    z ^= z >> 31;
    return static_cast<float>(static_cast<std::int64_t>(z >> 40) - (1 << 23)) / static_cast<float>(1 << 23);
}

// 两个声道各一个低频余弦：左 100 Hz、右 150 Hz，幅度 0.3（相邻差有解析的上界，阶跃判据用得上）
inline SourceFn tones()
{
    return [](std::uint64_t first, float* left, float* right, int n) {
        for (int i = 0; i < n; ++i)
        {
            left[i] = cosine(first + static_cast<std::uint64_t>(i), 100.0, 0.3f);
            right[i] = cosine(first + static_cast<std::uint64_t>(i), 150.0, 0.3f);
        }
    };
}

inline float peakOf(const std::vector<float>& trace, std::size_t from, std::size_t to)
{
    float m = 0.0f;
    for (std::size_t i = from; i < std::min(to, trace.size()); ++i)
    {
        m = std::max(m, std::fabs(trace[i]));
    }
    return m;
}

// [from, to) 里 |x| 最大的下标（并列取最早的）
inline std::size_t argmaxAbs(const std::vector<float>& trace, std::size_t from, std::size_t to)
{
    std::size_t at = from;
    for (std::size_t i = from; i < std::min(to, trace.size()); ++i)
    {
        if (std::fabs(trace[i]) > std::fabs(trace[at]))
        {
            at = i;
        }
    }
    return at;
}

inline bool allZero(const std::vector<float>& trace, std::size_t from, std::size_t to)
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

// [from, to) 里逐位相同
inline bool sameBits(const std::vector<float>& a, const std::vector<float>& b, std::size_t from, std::size_t to)
{
    to = std::min({to, a.size(), b.size()});
    for (std::size_t i = from; i < to; ++i)
    {
        if (test::floatBits(a[i]) != test::floatBits(b[i]))
        {
            return false;
        }
    }
    return from <= to;
}

// 同一段轨迹在 cut 起直接落到 0：阶跃判据的反例
inline std::vector<float> hardCut(std::vector<float> trace, std::size_t cut)
{
    std::fill(trace.begin() + static_cast<std::ptrdiff_t>(cut), trace.end(), 0.0f);
    return trace;
}

// ---- 参数与主机 ----

inline engine::EngineParams loopParams(float feedbackPercent, float mixPercent, float delayMs)
{
    engine::EngineParams p;
    p.feedbackPercent = feedbackPercent;
    p.mixPercent = mixPercent;
    p.delayMs = delayMs;
    return p;
}

inline BlockScript fixedBlocks(int length)
{
    BlockScript script;
    script.lengths = {length};
    return script;
}

// 建段、准备好的一台主机；建段失败时是空指针
inline std::unique_ptr<EngineHost> makeHost(double sampleRate, int maxBlock, std::uint32_t group = 1)
{
    auto host = std::make_unique<EngineHost>();
    if (!host->open(group))
    {
        return nullptr;
    }
    host->prepare(sampleRate, maxBlock);
    return host;
}

// ---- 记录器 ----

class Recorder
{
public:
    Recorder(EngineHost& host, SourceFn source, double sampleRate = kSr)
        : reference(sampleRate), host_(host), source_(source)
    {
        host_.setSource(std::move(source));
    }

    void setInputBypassed(bool on)
    {
        bypassed_ = on;
        host_.setInputBypassed(on);
    }

    // 与引擎的 setCommandBaseline 对应：参照的基线要和引擎的一致
    void setCommandBaseline(const engine::CommandBaseline& baseline)
    {
        host_.setCommandBaseline(baseline);
        reference.setCommandBaseline(baseline);
    }

    // 跑一个周期并记录；返回 Input 这一周期的样本数。本组场景的脚本里每个周期都有样本，
    // 一个周期没处理任何样本就是场景搭错了，当场记失败（见 reportStall）
    int cycle(std::size_t index, const engine::EngineParams& params)
    {
        ParamsScript script;
        script.snapshots = {params};
        host_.setParamsScript(std::move(script));
        const std::uint64_t first = host_.sourcePosition();
        const int n = host_.runCycle(index);
        if (n <= 0)
        {
            reportStall(index, n);
            return n;
        }
        const auto count = static_cast<std::size_t>(n);
        std::vector<float> dl(count);
        std::vector<float> dr(count);
        source_(first, dl.data(), dr.data(), n);
        reference.block(params, host_.inputLastBlock(), bypassed_, dl.data(), dr.data(), n);
        dryLeft.insert(dryLeft.end(), dl.begin(), dl.end());
        dryRight.insert(dryRight.end(), dr.begin(), dr.end());
        outLeft.insert(outLeft.end(), host_.inputOutputLeft().begin(),
                       host_.inputOutputLeft().begin() + static_cast<std::ptrdiff_t>(count));
        outRight.insert(outRight.end(), host_.inputOutputRight().begin(),
                        host_.inputOutputRight().begin() + static_cast<std::ptrdiff_t>(count));
        fedLeft.insert(fedLeft.end(), host_.outputFedLeft().begin(),
                       host_.outputFedLeft().begin() + static_cast<std::ptrdiff_t>(count));
        fedRight.insert(fedRight.end(), host_.outputFedRight().begin(),
                        host_.outputFedRight().begin() + static_cast<std::ptrdiff_t>(count));
        const loop::BlockResult& result = host_.inputLastBlock();
        if (result.resetDsp && result.resetAtSample >= 0)
        {
            resetPoints.push_back(result.resetAtSample);
        }
        return n;
    }

    // 恒定参数、恒定块长地跑 samples 个样本（周期编号接着上一次的）。
    // 周期没处理样本时 cycle 已经报了失败，这里立刻停下，不空转到超时
    void run(const engine::EngineParams& params, std::int64_t samples)
    {
        for (std::int64_t done = 0; done < samples;)
        {
            const int n = cycle(cycles_++, params);
            if (n <= 0)
            {
                return;
            }
            done += n;
        }
    }

    std::size_t cyclesRun() const noexcept { return cycles_; }
    void setCyclesRun(std::size_t cycles) noexcept { cycles_ = cycles; }

    // 输出与参照安全链按文档接线重算的结果逐位相同（左右两个声道）
    bool matchesReference() const
    {
        return test::sameBitsVec(outLeft, reference.wantLeft) && test::sameBitsVec(outRight, reference.wantRight);
    }

    std::vector<float> dryLeft;
    std::vector<float> dryRight;
    std::vector<float> outLeft;
    std::vector<float> outRight;
    // Output 处理之后看到的轨道缓冲（中间链是恒等时，它与 Input 的输出逐位相同才说明 Output 没有改动宿主 buffer）
    std::vector<float> fedLeft;
    std::vector<float> fedRight;
    // 每次起播沿报出的 DSP 复位点（绝对样本号）
    std::vector<std::int64_t> resetPoints;
    test::InputReference reference;

private:
    EngineHost& host_;
    SourceFn source_;
    bool bypassed_ = false;
    std::size_t cycles_ = 0;
};

// ---- 场景入口 ----

// 按 id 造出注册表里的场景并跑完（Catch2 用例用）
inline bool runRegisteredScenario(const char* id)
{
    const ScenarioEntry* entry = findScenario(id);
    if (entry == nullptr || entry->factory == nullptr)
    {
        return false;
    }
    const std::unique_ptr<Scenario> scenario(entry->factory());
    ScenarioContext context;
    runState() = RunState{id, 0};
    const bool passed = scenario->run(context);
    return passed && runState().stalls == 0;
}

} // namespace rinne::rig::scn
