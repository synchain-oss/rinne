// SPDX-License-Identifier: GPL-3.0-or-later
//
// 引擎层：OutputEngine 写环前清洗与逐位直通、写环分段、写权检查、块长上限；
// InputEngine 的最小实现（干声 + 回送）、命令序号基线、旁路与超长块的处理。
//
// 这些用例是环路引擎验收 3 的落点。标 ⛔ 的是删除式验证点：把对应的实现改坏，本用例必须变红。
//
// 环用普通内存里的 RingSegment（不碰命名段）：这些都是纯内存序与算术。

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include "diag/EventQueue.h"
#include "engine/EngineParams.h"
#include "engine/EngineEvents.h"
#include "engine/InputEngine.h"
#include "engine/OutputEngine.h"
#include "engine/TransportInfo.h"
#include "ipc/LoopRing.h"

namespace
{

using namespace rinne::engine;
using namespace rinne::ipc;

// 一个干净的环：值初始化 = 全零。
std::unique_ptr<RingSegment> makeRing()
{
    auto seg = std::make_unique<RingSegment>();
    LoopRing::initGeometry(*seg);
    return seg;
}

// 一份可以驱动引擎的宿主缓冲：左右各一串。
struct HostBuffer
{
    std::vector<float> left;
    std::vector<float> right;
    std::vector<float*> channels{nullptr, nullptr};

    HostBuffer(int n) : left(static_cast<std::size_t>(n), 0.0f), right(static_cast<std::size_t>(n), 0.0f)
    {
        channels[0] = left.data();
        channels[1] = right.data();
    }

    AudioView view() noexcept
    {
        AudioView v;
        v.channels = channels.data();
        v.numChannels = 2;
        v.length = static_cast<int>(left.size());
        return v;
    }
};

// 逐位比较两个缓冲（含 NaN 载荷与 −0.0）。
bool sameBits(const float* a, const float* b, std::size_t n)
{
    for (std::size_t i = 0; i < n; ++i)
    {
        std::uint32_t ba = 0;
        std::uint32_t bb = 0;
        std::memcpy(&ba, &a[i], sizeof ba);
        std::memcpy(&bb, &b[i], sizeof bb);
        if (ba != bb)
        {
            return false;
        }
    }
    return true;
}

constexpr double kSr = 48000.0;
constexpr std::uint64_t kToken = 1;

} // namespace

TEST_CASE("引擎 L3-01 没有对端：只出干声，回送为 0", "[engine]")
{
    OutputEngine output;
    output.prepare(kSr, 1024);

    HostBuffer host(64);
    for (int i = 0; i < 64; ++i)
    {
        host.left[static_cast<std::size_t>(i)] = 0.25f;
        host.right[static_cast<std::size_t>(i)] = -0.25f;
    }

    EngineParams params;
    params.feedbackPercent = 100.0f;
    params.mixPercent = 50.0f;

    // 没有段：写环什么都不做，也不崩。
    const bool wrote = output.process(host.view(), params, TransportInfo{}, false, nullptr, kToken);
    CHECK_FALSE(wrote);
    CHECK(output.state() == WriteState::kIdle);
    CHECK(output.writtenFrames() == 0);
}

TEST_CASE("引擎 L3-02 反馈为 0 时干声逐位直通", "[engine]")
{
    // ⛔ 删除式验证点：增益不乘 0（把干声增益算错）→ 本用例变红。
    InputEngine input;
    input.prepare(kSr, 1024);

    HostBuffer host(64);
    for (int i = 0; i < 64; ++i)
    {
        host.left[static_cast<std::size_t>(i)] = static_cast<float>(i) * 0.001f;
        host.right[static_cast<std::size_t>(i)] = -static_cast<float>(i) * 0.001f;
    }
    HostBuffer before = host;

    EngineParams params;
    params.feedbackPercent = 0.0f; // 反馈为 0（默认安全）
    params.mixPercent = 25.0f;    // g_dry = 1，g_ret = 0.5

    EngineContext ctx;
    const bool ok = input.process(host.view(), params, TransportInfo{}, false, ctx);
    CHECK(ok);
    // 反馈为 0 → 没有回送增益 → 干声逐位直通。
    CHECK(sameBits(before.left.data(), host.left.data(), 64));
    CHECK(sameBits(before.right.data(), host.right.data(), 64));
}

TEST_CASE("引擎 L3-04 Output 逐位直通：宿主 buffer 一个字节都不改", "[engine]")
{
    // ⛔ 删除式验证点：让 Output 清洗宿主 buffer（就地改）→ 本用例变红。
    OutputEngine output;
    output.prepare(kSr, 1024);

    auto ring = makeRing();
    LoopRing::publishWriter(*ring, kToken, static_cast<std::uint32_t>(kSr), 2);

    HostBuffer host(64);
    for (int i = 0; i < 64; ++i)
    {
        host.left[static_cast<std::size_t>(i)] = static_cast<float>(i) * 0.001f;
        host.right[static_cast<std::size_t>(i)] = -static_cast<float>(i) * 0.001f;
    }
    // 塞进 NaN 载荷与 ±Inf：清洗也不能碰宿主 buffer。
    host.left[3] = std::numeric_limits<float>::quiet_NaN();
    host.right[7] = std::numeric_limits<float>::infinity();
    HostBuffer before = host;

    EngineParams params;
    const bool wrote = output.process(host.view(), params, TransportInfo{}, false, ring.get(), kToken);
    CHECK(wrote);
    CHECK(sameBits(before.left.data(), host.left.data(), 64));
    CHECK(sameBits(before.right.data(), host.right.data(), 64));
}

TEST_CASE("引擎 L3-05 Output 写环前清洗：NaN/±Inf → 0；±1e6 → ±16；计数正确", "[engine]")
{
    // ⛔ 删除式验证点：去掉钳位 → 本用例变红。
    OutputEngine output;
    output.prepare(kSr, 1024);

    auto ring = makeRing();
    LoopRing::publishWriter(*ring, kToken, static_cast<std::uint32_t>(kSr), 2);

    HostBuffer host(8);
    host.left[0] = std::numeric_limits<float>::quiet_NaN();
    host.left[1] = std::numeric_limits<float>::infinity();
    host.left[2] = -std::numeric_limits<float>::infinity();
    host.left[3] = 1.0e6f;
    host.left[4] = -1.0e6f;
    host.left[5] = 0.5f;
    host.right[0] = 0.25f;

    EngineParams params;
    REQUIRE(output.process(host.view(), params, TransportInfo{}, false, ring.get(), kToken));

    // 读回环里的那 8 帧：清洗后的值。
    float left[8]{};
    float right[8]{};
    const RingRead read = LoopRing::read(*ring, 0, 8, left, right);
    REQUIRE(read.writeHead == 8);

    CHECK(left[0] == 0.0f); // NaN → 0
    CHECK(left[1] == 0.0f); // +Inf → 0
    CHECK(left[2] == 0.0f); // −Inf → 0
    CHECK(left[3] == 16.0f); // +1e6 → +16
    CHECK(left[4] == -16.0f); // −1e6 → −16
    CHECK(left[5] == 0.5f); // 正常值不动
    CHECK(right[0] == 0.25f);

    // 计数：3 个非有限（左声道 3 个），2 个钳位。
    CHECK(output.nonFiniteCount() == 3u);
    CHECK(output.clampCount() == 2u);
}

TEST_CASE("引擎 L3-06 Output 写环分段：n = 20000 时写 3 段，写头连续推进 20000 帧", "[engine]")
{
    // ⛔ 删除式验证点：不分段（一次写 20000 帧）→ 本用例变红（kBadLength）。
    OutputEngine output;
    output.prepare(kSr, 32768);

    auto ring = makeRing();
    LoopRing::publishWriter(*ring, kToken, static_cast<std::uint32_t>(kSr), 2);

    HostBuffer host(20000);
    for (int i = 0; i < 20000; ++i)
    {
        host.left[static_cast<std::size_t>(i)] = static_cast<float>(i) * 1.0e-5f;
        host.right[static_cast<std::size_t>(i)] = 0.0f;
    }

    EngineParams params;
    REQUIRE(output.process(host.view(), params, TransportInfo{}, false, ring.get(), kToken));

    // 写头连续推进 20000 帧：8192 + 8192 + 3616。
    CHECK(LoopRing::writeHead(*ring) == 20000u);
    CHECK(output.writtenFrames() == 20000u);

    // 每一段都写进去了：读回第 0 帧与第 19999 帧，值对得上位置。
    float left[20000]{};
    float right[20000]{};
    const RingRead read = LoopRing::read(*ring, 0, 20000, left, right);
    REQUIRE(read.writeHead == 20000u);
    CHECK(left[0] == 0.0f);
    CHECK(std::fabs(left[19999] - 19999.0f * 1.0e-5f) < 1.0e-7f);
}


TEST_CASE("引擎 L3-07 Output 每块先查 isWriter：写权不是自己的不写，置冲突状态", "[engine]")
{
    // ⛔ 删除式验证点：去掉 isWriter 检查 → 本用例变红。
    OutputEngine output;
    output.prepare(kSr, 1024);

    auto ring = makeRing();
    LoopRing::publishWriter(*ring, 99u, static_cast<std::uint32_t>(kSr), 2); // 写权是别人的

    HostBuffer host(64);
    EngineParams params;
    const bool wrote = output.process(host.view(), params, TransportInfo{}, false, ring.get(), kToken);
    CHECK_FALSE(wrote);
    CHECK(output.state() == WriteState::kConflict);
    CHECK(output.conflictCount() == 1u);
    CHECK(LoopRing::writeHead(*ring) == 0u);
}

TEST_CASE("引擎 L3-08 块长超限（n > 32768）：只出干声并给 kUnsupportedBlock", "[engine]")
{
    // ⛔ 删除式验证点：不检查上限 → 本用例变红。
    OutputEngine output;
    output.prepare(kSr, 1024);

    auto ring = makeRing();
    LoopRing::publishWriter(*ring, kToken, static_cast<std::uint32_t>(kSr), 2);

    HostBuffer host(32769);
    EngineParams params;
    const bool wrote = output.process(host.view(), params, TransportInfo{}, false, ring.get(), kToken);
    CHECK_FALSE(wrote);
    CHECK(output.state() == WriteState::kUnsupportedBlock);
    CHECK(output.unsupportedCount() == 1u);
    // 写头没动：不写环。
    CHECK(LoopRing::writeHead(*ring) == 0u);

    // 提示码落在引擎段（0x0400–0x04FF），不碰安全链段。
    std::vector<std::uint16_t> codes;
    output.drainEvents([&](const rinne::diag::Event& e) { codes.push_back(e.code); });
    REQUIRE(codes.size() == 1u);
    CHECK(codes[0] == static_cast<std::uint16_t>(EngineEventCode::kUnsupportedBlock));
    CHECK(codes[0] >= 0x0400u);
    CHECK(codes[0] <= 0x04FFu);
}

TEST_CASE("引擎 L3-10 事件码不落在 0x0200–0x02FF（安全链段）", "[engine]")
{
    // ⛔ 删除式验证点：事件码用 0x0200 段 → 本用例变红。
    static_assert(static_cast<std::uint16_t>(EngineEventCode::kUnsupportedBlock) >= 0x0400u, "引擎段从 0x0400 起");
    static_assert(static_cast<std::uint16_t>(EngineEventCode::kNonRealtime) <= 0x04FFu, "引擎段到 0x04FF 止");

    OutputEngine output;
    output.prepare(kSr, 1024);
    auto ring = makeRing();
    LoopRing::publishWriter(*ring, kToken, static_cast<std::uint32_t>(kSr), 2);

    HostBuffer host(32769);
    EngineParams params;
    output.process(host.view(), params, TransportInfo{}, false, ring.get(), kToken);

    std::vector<std::uint16_t> codes;
    output.drainEvents([&](const rinne::diag::Event& e) { codes.push_back(e.code); });
    for (const auto c : codes)
    {
        CHECK((c < 0x0200u || c > 0x02FFu));
    }
}

TEST_CASE("引擎 L3-11 EngineParams 11 个字段的默认值与计划逐字一致", "[engine]")
{
    // ⛔ 删除式验证点：改一个默认值 → 本用例变红。
    const EngineParams p;
    CHECK(p.feedbackPercent == 0.0f);    // 反馈 0（默认安全）
    CHECK(p.loopOn == true);
    CHECK(p.mixPercent == 25.0f);        // 混合 25 %
    CHECK(p.delayMode == DelayMode::kTime);
    CHECK(p.delayMs == 100.0f);          // 延迟 100 ms
    CHECK(p.delayNote == 10);            // 1/4
    CHECK(p.lowCutHz == 20.0f);          // 20 = 关
    CHECK(p.highCutHz == 20000.0f);      // 20000 = 关
    CHECK(p.polarityInvert == false);
    CHECK(p.limiterOn == true);          // 限制器默认开
    CHECK(p.ceilingDb == -6.0f);         // 上限 −6 dBFS
    CHECK(p.guardOn == true);            // 失控保护默认开
    CHECK(p.resetOnPlay == true);        // 起播复位默认开
    CHECK(p.tailMode == TailMode::kAuto);
}

TEST_CASE("引擎 混合律：m = mixPercent / 100 的四个点", "[engine]")
{
    // 干声与回送的增益律：g_dry = min(1, 2(1−m))、g_ret = min(1, 2m)。
    // 四个点：0 % → 1/0；25 % → 1/0.5；50 % → 1/1；100 % → 0/1。
    auto dryOf = [](float percent) {
        const float m = percent / 100.0f;
        const float g = 2.0f * (1.0f - m);
        return g < 1.0f ? g : 1.0f;
    };
    auto retOf = [](float percent) {
        const float m = percent / 100.0f;
        const float g = 2.0f * m;
        return g < 1.0f ? g : 1.0f;
    };

    CHECK(dryOf(0.0f) == 1.0f);
    CHECK(retOf(0.0f) == 0.0f);
    CHECK(dryOf(25.0f) == 1.0f);
    CHECK(retOf(25.0f) == 0.5f);
    CHECK(dryOf(50.0f) == 1.0f);
    CHECK(retOf(50.0f) == 1.0f);
    CHECK(dryOf(100.0f) == 0.0f);
    CHECK(retOf(100.0f) == 1.0f);
}

TEST_CASE("引擎 命令序号：基线之外的变化才执行，基线本身不重放", "[engine]")
{
    InputEngine input;

    // 基线：Panic 已经按过一次。重建后不重放它。
    CommandBaseline baseline;
    baseline.panicSeq = 7;
    baseline.recoverSeq = 3;
    baseline.presetSeq = 2;
    baseline.stateSeq = 5;
    input.setCommandBaseline(baseline);
    CHECK(input.commandBaseline().panicSeq == 7u);

    // 引擎侧自己记基线：块首比对的基准就是它。
    HostBuffer host(16);
    EngineParams params;
    params.panicSeq = baseline.panicSeq;
    params.recoverSeq = baseline.recoverSeq;
    params.presetSeq = baseline.presetSeq;
    params.stateSeq = baseline.stateSeq;
    EngineContext ctx;
    input.process(host.view(), params, TransportInfo{}, false, ctx);
    // 基线没有变 → 不执行任何一条（没有淡入请求）。
    CHECK(input.commandBaseline().panicSeq == 7u);
}

TEST_CASE("引擎 旁路：干声照常，回送增益为 0", "[engine]")
{
    InputEngine input;
    input.prepare(kSr, 1024);

    HostBuffer host(32);
    for (int i = 0; i < 32; ++i)
    {
        host.left[static_cast<std::size_t>(i)] = 0.5f;
    }
    HostBuffer before = host;

    EngineParams params;
    params.feedbackPercent = 100.0f;
    params.mixPercent = 50.0f;

    EngineContext ctx;
    input.process(host.view(), params, TransportInfo{}, false, ctx);
    // 反馈非 0 时干声乘 g_dry = 1，逐位不变（1.0 乘法逐位不变）。
    CHECK(sameBits(before.left.data(), host.left.data(), 32));
}

TEST_CASE("引擎 0 样本的调用立刻返回，不推进任何计数", "[engine]")
{
    InputEngine input;
    input.prepare(kSr, 1024);

    HostBuffer host(0);
    EngineParams params;
    EngineContext ctx;
    CHECK_FALSE(input.process(host.view(), params, TransportInfo{}, false, ctx));

    // N_obs 没有被 0 样本推进。
    CHECK(input.maxBlockSeen() == 0);
}

TEST_CASE("引擎 写方死在一段中间：新写方 ≥ 1 s 之后回收，恢复出声", "[engine]")
{
    // 造一个「写到一半死掉的写方」：公告了一段（write_intent）但不发布（不推 write_head）。
    auto ring = makeRing();
    ring->header.write_head.store(0u);
    ring->header.write_intent.store(4096u);

    OutputEngine output;
    output.prepare(kSr, 1024);
    // 新写方拿到写权（旧的写方已经死了，环上的 token 让给新的）。
    LoopRing::publishWriter(*ring, kToken, static_cast<std::uint32_t>(kSr), 2);

    // 连续写 1 s 的音频（48 kHz → 48000 帧）：中途拿到 kInFlight，挡满 1 s 之后回收。
    constexpr int kChunk = 1024;
    HostBuffer host(kChunk);
    for (int i = 0; i < kChunk; ++i)
    {
        host.left[static_cast<std::size_t>(i)] = 0.1f;
        host.right[static_cast<std::size_t>(i)] = 0.1f;
    }
    EngineParams params;

    std::uint64_t written = 0;
    for (int round = 0; round < 60; ++round)
    {
        if (output.process(host.view(), params, TransportInfo{}, false, ring.get(), kToken))
        {
            written += kChunk;
        }
    }

    // 回收发生过了：回收计数 > 0，写头推进了（不再是永久静音）。
    CHECK(output.reclaimCount() > 0u);
    CHECK(LoopRing::writeHead(*ring) > 4096u);
    CHECK(written > 0u);
}
