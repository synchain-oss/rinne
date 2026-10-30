// SPDX-License-Identifier: GPL-3.0-or-later
//
// S19 Input 换组：Input 在两个组之间来回换，一个组的环写头已经很大（约 2^40），另一个是刚建的新环（写头 0）。
// 换组之后 Input 的读位（本地已处理帧数）比新环的写头大得多（从大写头换到 0），或小得多（从 0 换到大写头）：
// 差值一律走有符号运算，读头重建在新环的写头上，旧组的音频不能漏进新组的回送，也不能从此静音。
//
// 做法：两个组各有一个独立的 Output（它们在别的轨道上，各写自己的噪声，信号源是全局样本号的纯函数，两组的种子不同），
// Input 在自己的轨道上：干声是一个固定的信号，回送取绑定的那一组。环路延迟 D = 4800，块长 256（恒定）或
// 32…512（带种子随机），声明的最大块长 512，混合 50 %。组 1 的环头一开始就预置成写头 = 公告 = 2^40，组 2 的环写头为
// 0，它的 Output 在第一次换到组 2 的那个周期才开始写（Input 先跑：换过去的那一块里环上还一帧没有）。组 1 的 Output
// 一直在写。换组的顺序：组 1（第 0–2 s）→ 组 2（2–4 s）→ 组 1（4–6 s）→ 组 2（6–8 s）→ 组 1（8–10 s），共 4 次，
// 每次换组壳都发布一份新的绑定代号（段映射变了）。一遍反馈 0（读头的内容逐样本对得回对应那一组的噪声），一遍反馈
// 50 %（回送在响）。
//
// 断言（第一次配上的那一段按同样的口径查，T 取配上的那个周期的第一个样本）：
//   - 全程恰 4 次重同步，原因都是换绑定；没有跳变、位移、断档、容忍模式；没有补 0、撕裂、被环覆盖的帧；
//   - 反馈 0 一遍：设换组的第一个样本是 T。T 之后读头的内容只可能来自新组：t ≥ T + D + 20 ms（新读头的数据在 D 之后到，
//     再走 20 ms 的 onset）起，回送样本 t 逐位等于新组噪声在 t − D 处的值（比值恰为 1）；
//     T + 5 ms（旧读头淡出走完）起到 T + D 之前是静音；T + D + 5 ms 起每个样本都不为 0（回送恢复，没有永久静音）；
//     整个换组期间的非零回送样本符号与新组噪声在 t − D 处相同、幅度不大于它；
//     旧组的音频没有漏进来：恢复之后的回送与旧组噪声在 t − D 处的归一化互相关小于 −40 dB；
//   - 反馈 50 % 一遍：输出与参照安全链按文档接线重算的结果逐位相同；每个停留段的最后 0.5 s 里，回送（引擎输出减干声）
//     的峰值不低于 0.05 且不超过 1（500 ms 淡入之后回送回来，不是永久静音）。
//
// 标 ⛔ 的是删除式验证点：把对应的实现改坏，本场景必须变红。
//   ⛔ 换了段映射不算换段（地址与映射代号都不比）→ 读头停在旧组的读位上，重同步的原因与读头的内容都对不上；
//   ⛔ 第一次配上时本地延迟线不锚在写头（从 0 起拉）→ 组 1 的写头在 2^40 附近，读头永远追不上，第一段回送是 0。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "engine/InputEngine.h"
#include "engine/OutputEngine.h"
#include "loop/StreamSync.h"
#include "s01_rig_kit.h"

namespace
{

using namespace rinne::rig;
using namespace rinne::rig::scn;
using rinne::loop::SyncReason;

constexpr int kMaxBlock = 512;
constexpr std::int64_t kDelay = 4800;
constexpr std::int64_t kOnset = 960; // 新读头的 onset：20 ms
constexpr std::int64_t kSecond = 48000;
constexpr std::int64_t kStint = 2 * kSecond;
constexpr int kStints = 5;
constexpr std::uint64_t kBigHead = 1ull << 40;

enum class Blocks
{
    kFixed,
    kRandom
};

float noiseOf(int group, std::int64_t t, int channel)
{
    return 0.25f * hashNoise(static_cast<std::uint64_t>(t), static_cast<std::uint64_t>(40 + 2 * group + channel));
}

// 一台由两个组的环、一个 Input、两个 Output 搭成的小台子。Output 在别的轨道上，各自独立，写各自组的环
class Bench
{
public:
    bool open()
    {
        if (!segments_.open(1) || !segments_.open(2))
        {
            return false;
        }
        for (const std::uint32_t group : {1u, 2u})
        {
            auto* ring = segments_.ring(group);
            if (ring == nullptr)
            {
                return false;
            }
            rinne::ipc::LoopRing::publishWriter(*ring, group, rinne::loop::publishedSampleRate(kSr), 2);
            rinne::ipc::roleSlot(*segments_.rv(), group, rinne::ipc::Role::kInput).max_block.store(kMaxBlock);
            rinne::ipc::roleSlot(*segments_.rv(), group, rinne::ipc::Role::kOutput).max_block.store(kMaxBlock);
        }
        // 组 1 的环一开始就在 2^40 附近
        auto* big = segments_.ring(1);
        big->header.write_head.store(kBigHead, std::memory_order_relaxed);
        big->header.write_intent.store(kBigHead, std::memory_order_relaxed);
        input_.prepare(kSr, kMaxBlock);
        output_[0].prepare(kSr, kMaxBlock);
        output_[1].prepare(kSr, kMaxBlock);
        for (auto& v : buffers_)
        {
            v.assign(kMaxBlock + 8, 0.0f);
        }
        return true;
    }

    // 换到另一个组：段映射变了，壳发布新的绑定代号
    void switchTo(std::uint32_t group)
    {
        bound_ = group;
        ++generation_;
    }
    std::uint32_t bound() const noexcept { return bound_; }
    // 组 2 的 Output 要等到第一次换过去之后才开始写
    void startSecondOutput() noexcept { secondOutputRuns_ = true; }

    // 一个周期：Input 先跑，两个 Output 随后各写自己的噪声
    int cycle(std::int64_t first, int n, const rinne::engine::EngineParams& params, const SourceFn& dry,
              std::vector<float>& outLeft)
    {
        dry(static_cast<std::uint64_t>(first), buffers_[0].data(), buffers_[1].data(), n);
        std::array<float*, 2> inChannels{buffers_[0].data(), buffers_[1].data()};
        rinne::engine::AudioView inView;
        inView.channels = inChannels.data();
        inView.numChannels = 2;
        inView.length = n;
        rinne::engine::EngineContext inContext;
        inContext.ring = segments_.ring(bound_);
        inContext.rendezvous = segments_.rv();
        inContext.group = bound_;
        inContext.ringGeneration = generation_;
        input_.process(inView, params, rinne::engine::TransportInfo{}, false, inContext);
        outLeft.insert(outLeft.end(), buffers_[0].begin(), buffers_[0].begin() + n);

        for (int g = 0; g < 2; ++g)
        {
            if (g == 1 && !secondOutputRuns_)
            {
                continue;
            }
            auto& left = buffers_[static_cast<std::size_t>(2 + 2 * g)];
            auto& right = buffers_[static_cast<std::size_t>(3 + 2 * g)];
            for (int i = 0; i < n; ++i)
            {
                left[static_cast<std::size_t>(i)] = noiseOf(g + 1, first + i, 0);
                right[static_cast<std::size_t>(i)] = noiseOf(g + 1, first + i, 1);
            }
            std::array<float*, 2> channels{left.data(), right.data()};
            rinne::engine::AudioView view;
            view.channels = channels.data();
            view.numChannels = 2;
            view.length = n;
            rinne::engine::EngineContext context;
            context.ring = segments_.ring(static_cast<std::uint32_t>(g + 1));
            context.rendezvous = segments_.rv();
            context.group = static_cast<std::uint32_t>(g + 1);
            context.token = static_cast<std::uint64_t>(g + 1);
            context.ringGeneration = 1;
            output_[static_cast<std::size_t>(g)].process(view, params, rinne::engine::TransportInfo{}, false, context);
        }
        return n;
    }

    const rinne::loop::BlockResult& lastBlock() const noexcept { return input_.lastBlockResult(); }
    rinne::loop::LoopStatusFields status() const { return input_.loopStatus(); }

private:
    Segments segments_;
    rinne::engine::InputEngine input_;
    std::array<rinne::engine::OutputEngine, 2> output_;
    // 0、1：Input 轨道的左右；2、3：组 1 的 Output 轨道；4、5：组 2 的 Output 轨道
    std::array<std::vector<float>, 6> buffers_;
    std::uint32_t bound_ = 1;
    std::uint64_t generation_ = 1;
    bool secondOutputRuns_ = false;
};

struct Trace
{
    std::vector<std::int64_t> switchAt; // 每次换组的第一个样本，第 0 项是第一次配上的那个周期的第一个样本
    std::vector<std::uint32_t> groupAt; // 换过去的组
    std::vector<float> dryLeft, tapLeft, retLeft, outLeft;
    bool matchesReference = false;
    std::uint32_t resyncs = 0;
    std::uint32_t byReason[9]{};
    std::uint32_t tolerant = 0;
    std::uint64_t overrun = 0;
    std::uint64_t overflow = 0;
    std::uint64_t torn = 0;
};

Trace runOne(Blocks blocks, float feedbackPercent)
{
    Trace trace;
    Bench bench;
    REQUIRE(bench.open());
    rinne::test::Rng rng(0x5a110019u);
    const std::int64_t total = kStints * kStint;
    const SourceFn dry = feedbackPercent == 0.0f ? silenceSource() : tones();
    rinne::test::InputReference reference(kSr);
    const rinne::engine::EngineParams p = loopParams(feedbackPercent, 50.0f, 100.0f);

    std::int64_t cursor = 0;
    int stint = 0;
    while (cursor < total)
    {
        if (cursor >= static_cast<std::int64_t>(stint + 1) * kStint && stint + 1 < kStints)
        {
            ++stint;
            const std::uint32_t group = stint % 2 == 1 ? 2u : 1u;
            bench.switchTo(group);
            if (group == 2u)
            {
                bench.startSecondOutput();
            }
            trace.switchAt.push_back(cursor);
            trace.groupAt.push_back(group);
        }
        const int n = blocks == Blocks::kFixed ? 256 : 32 + rng.below(kMaxBlock - 32 + 1);
        const auto count = static_cast<std::size_t>(n);
        std::vector<float> dl(count);
        std::vector<float> dr(count);
        dry(static_cast<std::uint64_t>(cursor), dl.data(), dr.data(), n);
        bench.cycle(cursor, n, p, dry, trace.outLeft);
        if (trace.switchAt.empty() && bench.status().paired == 1u)
        {
            trace.switchAt.push_back(cursor);
            trace.groupAt.push_back(1u);
        }
        reference.block(p, bench.lastBlock(), false, dl.data(), dr.data(), n);
        trace.dryLeft.insert(trace.dryLeft.end(), dl.begin(), dl.end());
        cursor += n;
    }
    trace.tapLeft = reference.tapLeft;
    trace.retLeft.resize(trace.outLeft.size());
    for (std::size_t i = 0; i < trace.outLeft.size(); ++i)
    {
        trace.retLeft[i] = trace.outLeft[i] - trace.dryLeft[i];
    }
    trace.matchesReference = rinne::test::sameBitsVec(trace.outLeft, reference.wantLeft);
    const auto status = bench.status();
    trace.resyncs = status.resyncCount;
    for (int r = 0; r < 9; ++r)
    {
        trace.byReason[r] = status.resyncByReason[r];
    }
    trace.tolerant = status.tolerant;
    trace.overrun = status.overrun;
    trace.overflow = status.overflow;
    trace.torn = status.torn;
    return trace;
}

double correlationDb(const std::vector<float>& tap, std::int64_t from, std::int64_t to, std::uint32_t otherGroup)
{
    double xy = 0.0;
    double xx = 0.0;
    double yy = 0.0;
    for (std::int64_t t = from; t < to; ++t)
    {
        const double a = tap[static_cast<std::size_t>(t)];
        const double b = noiseOf(static_cast<int>(otherGroup), t - kDelay, 0);
        xy += a * b;
        xx += a * a;
        yy += b * b;
    }
    if (xx <= 0.0 || yy <= 0.0)
    {
        return -300.0;
    }
    return 20.0 * std::log10(std::max(std::fabs(xy) / std::sqrt(xx * yy), 1.0e-15));
}

class GroupSwitchScenario final : public Scenario
{
public:
    const char* id() const noexcept override { return "s19_group_switch"; }

    bool run(ScenarioContext&) override
    {
        Verdict v;
        for (const Blocks blocks : {Blocks::kFixed, Blocks::kRandom})
        {
            for (const float feedback : {0.0f, 50.0f})
            {
                INFO("blocks " << (blocks == Blocks::kFixed ? "fixed 256" : "random") << ", feedback " << feedback
                               << " %");
                const Trace trace = runOne(blocks, feedback);
                RIG_EQ(v, trace.switchAt.size(), std::size_t{kStints});
                RIG_EQ(v, trace.resyncs, static_cast<std::uint32_t>(kStints - 1));
                RIG_EQ(v, trace.byReason[static_cast<std::size_t>(SyncReason::kBindSwitch)],
                       static_cast<std::uint32_t>(kStints - 1));
                RIG_EQ(v, trace.tolerant, 0u);
                RIG_EQ(v, trace.overrun, std::uint64_t{0});
                RIG_EQ(v, trace.overflow, std::uint64_t{0});
                RIG_EQ(v, trace.torn, std::uint64_t{0});
                RIG_CHECK(v, trace.matchesReference);
                if (feedback == 0.0f)
                {
                    checkContent(v, trace);
                }
                else
                {
                    checkAudible(v, trace);
                }
            }
        }
        return v.ok();
    }

private:
    static void checkContent(Verdict& v, const Trace& t)
    {
        const auto total = static_cast<std::int64_t>(t.tapLeft.size());
        for (std::size_t s = 0; s < t.switchAt.size(); ++s)
        {
            INFO("switch " << s << " to group " << t.groupAt[s] << " at sample " << t.switchAt[s]);
            const std::int64_t from = t.switchAt[s];
            const std::int64_t end = s + 1 < t.switchAt.size() ? t.switchAt[s + 1] : total;
            const int group = static_cast<int>(t.groupAt[s]);
            const int oldGroup = 3 - group;
            // 旧读头淡出走完到新数据到达之前：静音
            RIG_CHECK(v, allZero(t.tapLeft, static_cast<std::size_t>(from + kFadeOutSamples + 16),
                                 static_cast<std::size_t>(from + kDelay)));
            bool exact = true;
            bool bounded = true;
            bool alive = true;
            for (std::int64_t at = from; at < end; ++at)
            {
                const float want = noiseOf(group, at - kDelay, 0);
                const float got = t.tapLeft[static_cast<std::size_t>(at)];
                if (at >= from + kDelay + kOnset)
                {
                    exact = exact && std::fabs(got - want) <= 1.0e-6f;
                }
                if (at >= from + kDelay + kFadeOutSamples)
                {
                    alive = alive && got != 0.0f;
                }
                if (at >= from + kFadeOutSamples + 16 && got != 0.0f)
                {
                    bounded = bounded && (got > 0.0f) == (want > 0.0f) &&
                              std::fabs(got) <= std::fabs(want) * (1.0f + 1.0e-6f);
                }
            }
            RIG_CHECK(v, exact);
            RIG_CHECK(v, bounded);
            RIG_CHECK(v, alive);
            RIG_CHECK(v, correlationDb(t.tapLeft, from + kDelay + kOnset, end, static_cast<std::uint32_t>(oldGroup)) <
                             -40.0);
        }
    }

    static void checkAudible(Verdict& v, const Trace& t)
    {
        RIG_CHECK(v, std::all_of(t.outLeft.begin(), t.outLeft.end(),
                                 [](float f) { return std::isfinite(f) && std::fabs(f) < 1.0f; }));
        const auto total = static_cast<std::int64_t>(t.retLeft.size());
        for (std::size_t s = 0; s < t.switchAt.size(); ++s)
        {
            const std::int64_t end = s + 1 < t.switchAt.size() ? t.switchAt[s + 1] : total;
            const float peak =
                peakOf(t.retLeft, static_cast<std::size_t>(end - kSecond / 2), static_cast<std::size_t>(end));
            INFO("stint " << s << " ends at " << end << ", return peak " << peak);
            RIG_CHECK(v, peak >= 0.05f && peak < 1.0f);
        }
    }
};

} // namespace

RINNE_RIG_SCENARIO("s19_group_switch")
{
    return new GroupSwitchScenario();
}

TEST_CASE("s19: the Input switches between a huge-head ring and a fresh ring and the loopback always comes back",
          "[loop_rig][s19]")
{
    CHECK(rinne::rig::scn::runRegisteredScenario("s19_group_switch"));
}
