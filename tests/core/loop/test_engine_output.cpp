// SPDX-License-Identifier: GPL-3.0-or-later
//
// 引擎层：OutputEngine 写环前清洗与逐位直通、写环分段、写权检查、块长上限、旁路斜坡、端点槽；
// InputEngine 的最小实现（干声 + 回送）、命令序号基线、旁路与超长块的处理、诊断安全段；
// 两端接起来的闭环：环路延迟恰为 D、与宿主怎么切块逐位无关、错序判据。
//
// 这些用例是环路引擎验收 3 的落点。标 ⛔ 的是删除式验证点：把对应的实现改坏，本用例必须变红。
//
// 环与会合段用普通内存里的 RingSegment / RvSegment（不碰命名段）：这些都是纯内存序与算术。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include "diag/EventQueue.h"
#include "engine/EngineContext.h"
#include "engine/EngineEvents.h"
#include "engine/EngineParams.h"
#include "engine/InputEngine.h"
#include "engine/OutputEngine.h"
#include "engine/TransportInfo.h"
#include "ipc/LoopBinding.h"
#include "ipc/LoopRing.h"
#include "loop/LoopEvents.h"

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

// 一个干净的会合段：值初始化 = 全零（端点槽全是 0）。
std::unique_ptr<RvSegment> makeRv()
{
    return std::make_unique<RvSegment>();
}

// 一份可以驱动引擎的宿主缓冲：左右各一串。
struct HostBuffer
{
    std::vector<float> left;
    std::vector<float> right;
    std::vector<float*> channels{nullptr, nullptr};

    explicit HostBuffer(int n) : left(static_cast<std::size_t>(n), 0.0f), right(static_cast<std::size_t>(n), 0.0f)
    {
        channels[0] = left.data();
        channels[1] = right.data();
    }

    HostBuffer(const HostBuffer& other) : left(other.left), right(other.right)
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

// Output 侧的块首上下文：环 + 写权（+ 可选的会合段）。
EngineContext outputCtx(RingSegment* ring, std::uint64_t token = kToken, RvSegment* rv = nullptr)
{
    EngineContext ctx;
    ctx.ring = ring;
    ctx.token = token;
    ctx.rendezvous = rv;
    ctx.group = rv != nullptr ? 1u : 0u;
    return ctx;
}

// Input 侧的块首上下文：环（+ 可选的会合段）。
EngineContext inputCtx(RingSegment* ring, RvSegment* rv = nullptr)
{
    EngineContext ctx;
    ctx.ring = ring;
    ctx.rendezvous = rv;
    ctx.group = rv != nullptr ? 1u : 0u;
    return ctx;
}

// 两端接成一条闭环：一条轨道上 Input 在前、Output 在后（正确的摆放），中间没有别的插件。
// 每块：轨道缓冲先给 Input（就地加回送），再给 Output（写环）。返回 Input 处理之后的整段输出。
//
// 参数恒定；块长由 blockLength 给，总长 total；信号源是 source(绝对样本号)。
struct Loop
{
    std::unique_ptr<RingSegment> ring = makeRing();
    std::unique_ptr<RvSegment> rv = makeRv();
    InputEngine input;
    OutputEngine output;
    bool outputFirst = false;

    Loop(int maxBlock)
    {
        LoopRing::publishWriter(*ring, kToken, static_cast<std::uint32_t>(kSr), 2);
        input.prepare(kSr, maxBlock);
        output.prepare(kSr, maxBlock);
    }

    template<typename Source, typename TransportAt>
    std::vector<float> run(const EngineParams& params, int blockLength, int total, Source&& source,
                           TransportAt&& transportAt)
    {
        std::vector<float> out;
        out.reserve(static_cast<std::size_t>(total));
        int at = 0;
        while (at < total)
        {
            const int n = std::min(blockLength, total - at);
            HostBuffer host(n);
            for (int i = 0; i < n; ++i)
            {
                host.left[static_cast<std::size_t>(i)] = source(at + i);
                host.right[static_cast<std::size_t>(i)] = source(at + i);
            }
            const TransportInfo transport = transportAt(at);
            if (outputFirst)
            {
                output.process(host.view(), params, transport, false, outputCtx(ring.get(), kToken, rv.get()));
                input.process(host.view(), params, transport, false, inputCtx(ring.get(), rv.get()));
            }
            else
            {
                input.process(host.view(), params, transport, false, inputCtx(ring.get(), rv.get()));
                output.process(host.view(), params, transport, false, outputCtx(ring.get(), kToken, rv.get()));
            }
            out.insert(out.end(), host.left.begin(), host.left.end());
            at += n;
        }
        return out;
    }

    template<typename Source>
    std::vector<float> run(const EngineParams& params, int blockLength, int total, Source&& source)
    {
        return run(params, blockLength, total, source, [](int) { return TransportInfo{}; });
    }
};

// 反馈 100 %、混合 50 %：g_ret = 1、g_dry = 1，回送增益恰为 1（30 ms 平滑走完之后）。
EngineParams unityLoopParams(float delayMs)
{
    EngineParams params;
    params.feedbackPercent = 100.0f;
    params.mixPercent = 50.0f;
    params.delayMs = delayMs;
    return params;
}

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
    const bool wrote = output.process(host.view(), params, TransportInfo{}, false, outputCtx(nullptr));
    CHECK_FALSE(wrote);
    CHECK(output.state() == WriteState::kIdle);
    CHECK(output.writtenFrames() == 0);

    // Input 没有环：只出干声。
    InputEngine input;
    input.prepare(kSr, 1024);
    HostBuffer dry = host;
    CHECK(input.process(host.view(), params, TransportInfo{}, false, inputCtx(nullptr)));
    CHECK(sameBits(dry.left.data(), host.left.data(), 64));
    CHECK(sameBits(dry.right.data(), host.right.data(), 64));
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
    host.left[0] = -0.0f; // −0.0 也要逐位保留
    HostBuffer before = host;

    EngineParams params;
    params.feedbackPercent = 0.0f; // 反馈为 0（默认安全）
    params.mixPercent = 25.0f; // g_dry = 1，g_ret = 0.5

    auto ring = makeRing();
    LoopRing::publishWriter(*ring, kToken, static_cast<std::uint32_t>(kSr), 2);
    const bool ok = input.process(host.view(), params, TransportInfo{}, false, inputCtx(ring.get()));
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
    const bool wrote = output.process(host.view(), params, TransportInfo{}, false, outputCtx(ring.get()));
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
    REQUIRE(output.process(host.view(), params, TransportInfo{}, false, outputCtx(ring.get())));

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

    // 计数：3 个非有限（左声道 3 个），2 个钳位。环头里的计数与引擎自己的计数一致。
    CHECK(output.nonFiniteCount() == 3u);
    CHECK(output.clampCount() == 2u);
    CHECK(ring->header.nonfinite_count.load() == 3u);
    CHECK(ring->header.clamp_count.load() == 2u);
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
    REQUIRE(output.process(host.view(), params, TransportInfo{}, false, outputCtx(ring.get())));

    // 写头连续推进 20000 帧：8192 + 8192 + 3616。
    CHECK(LoopRing::writeHead(*ring) == 20000u);
    CHECK(output.writtenFrames() == 20000u);

    // 每一段都写进去了：读回第 0 帧与第 19999 帧，值对得上位置。
    std::vector<float> left(20000, 0.0f);
    std::vector<float> right(20000, 0.0f);
    const RingRead read = LoopRing::read(*ring, 0, 20000, left.data(), right.data());
    REQUIRE(read.writeHead == 20000u);
    CHECK(left[0] == 0.0f);
    CHECK(left[8191] == host.left[8191]);
    CHECK(left[8192] == host.left[8192]);
    CHECK(left[19999] == host.left[19999]);
}

TEST_CASE("引擎 Output 写环分段：宿主给 32768 帧时切成 4 段 8192，写头推进 32768", "[engine]")
{
    OutputEngine output;
    output.prepare(kSr, 32768);
    auto ring = makeRing();
    LoopRing::publishWriter(*ring, kToken, static_cast<std::uint32_t>(kSr), 2);

    HostBuffer host(32768);
    for (int i = 0; i < 32768; ++i)
    {
        host.left[static_cast<std::size_t>(i)] = static_cast<float>(i % 1000) * 1.0e-3f;
    }
    EngineParams params;
    REQUIRE(output.process(host.view(), params, TransportInfo{}, false, outputCtx(ring.get())));
    CHECK(LoopRing::writeHead(*ring) == 32768u);
    CHECK(output.writtenFrames() == 32768u);
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
    const bool wrote = output.process(host.view(), params, TransportInfo{}, false, outputCtx(ring.get()));
    CHECK_FALSE(wrote);
    CHECK(output.state() == WriteState::kConflict);
    CHECK(output.conflictCount() == 1u);
    CHECK(LoopRing::writeHead(*ring) == 0u);

    // 冲突持续很多块也只记一条事件（状态变了才记），不会把队列灌满。
    for (int i = 0; i < 300; ++i)
    {
        output.process(host.view(), params, TransportInfo{}, false, outputCtx(ring.get()));
    }
    int events = 0;
    output.drainEvents([&](const rinne::diag::Event&) { ++events; });
    CHECK(events == 1);
    CHECK(output.conflictCount() == 301u);
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
    const bool wrote = output.process(host.view(), params, TransportInfo{}, false, outputCtx(ring.get()));
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

    // Input 侧同样只出干声。
    InputEngine input;
    input.prepare(kSr, 1024);
    HostBuffer inHost(32769);
    for (auto& v : inHost.left)
    {
        v = 0.125f;
    }
    HostBuffer before = inHost;
    params.feedbackPercent = 100.0f;
    params.mixPercent = 50.0f;
    input.process(inHost.view(), params, TransportInfo{}, false, inputCtx(ring.get()));
    CHECK(input.lastBlockResult().unsupportedBlock);
    CHECK(sameBits(before.left.data(), inHost.left.data(), inHost.left.size()));
}

TEST_CASE("引擎 L3-09 EventQueue：SPSC 定长；满了丢掉并计数；不覆盖未读条目；drain 后为空", "[engine]")
{
    // ⛔ 删除式验证点：改成覆盖最老条目 → 本用例变红（最早的那几条被换掉）。
    rinne::diag::EventQueue<8> queue; // 保留一格：最多装 7 条
    for (int i = 0; i < 10; ++i)
    {
        queue.push(rinne::diag::Event{static_cast<std::uint16_t>(0x0300 + i), static_cast<std::uint8_t>(i), 0, i});
    }
    CHECK(queue.size() == 7u);
    CHECK(queue.dropped() == 3u);

    std::vector<std::uint16_t> codes;
    const int drained = queue.drain([&](const rinne::diag::Event& e) { codes.push_back(e.code); });
    CHECK(drained == 7);
    REQUIRE(codes.size() == 7u);
    // 最早的 7 条按推入顺序留着：满了丢的是新来的，不覆盖还没读走的。
    for (int i = 0; i < 7; ++i)
    {
        CHECK(codes[static_cast<std::size_t>(i)] == static_cast<std::uint16_t>(0x0300 + i));
    }
    CHECK(queue.size() == 0u);
    // 排空之后接着能推。
    queue.push(rinne::diag::Event{0x0400, 0, 0, 0});
    CHECK(queue.size() == 1u);
}

TEST_CASE("引擎 L3-10 事件码不落在 0x0200–0x02FF（安全链段）", "[engine]")
{
    // ⛔ 删除式验证点：事件码用 0x0200 段 → 本用例变红。
    static_assert(static_cast<std::uint16_t>(EngineEventCode::kUnsupportedBlock) >= 0x0400u, "引擎段从 0x0400 起");
    static_assert(static_cast<std::uint16_t>(EngineEventCode::kNonRealtime) <= 0x04FFu, "引擎段到 0x04FF 止");
    static_assert(static_cast<std::uint16_t>(rinne::loop::LoopEventCode::kResync) >= 0x0300u, "环路段从 0x0300 起");
    static_assert(static_cast<std::uint16_t>(rinne::loop::LoopEventCode::kTapDropped) <= 0x03FFu, "环路段到 0x03FF 止");

    OutputEngine output;
    output.prepare(kSr, 1024);
    auto ring = makeRing();
    LoopRing::publishWriter(*ring, kToken, static_cast<std::uint32_t>(kSr), 2);

    HostBuffer host(32769);
    EngineParams params;
    output.process(host.view(), params, TransportInfo{}, false, outputCtx(ring.get()));

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
    CHECK(p.feedbackPercent == 0.0f); // 反馈 0（默认安全）
    CHECK(p.loopOn == true);
    CHECK(p.mixPercent == 25.0f); // 混合 25 %
    CHECK(p.delayMode == DelayMode::kTime);
    CHECK(p.delayMs == 100.0f); // 延迟 100 ms
    CHECK(p.delayNote == 10); // 1/4
    CHECK(p.lowCutHz == 20.0f); // 20 = 关
    CHECK(p.highCutHz == 20000.0f); // 20000 = 关
    CHECK(p.polarityInvert == false);
    CHECK(p.limiterOn == true); // 限制器默认开
    CHECK(p.ceilingDb == -6.0f); // 上限 −6 dBFS
    CHECK(p.guardOn == true); // 失控保护默认开
    CHECK(p.resetOnPlay == true); // 起播复位默认开
    CHECK(p.tailMode == TailMode::kAuto);
}

TEST_CASE("引擎 L3-12 TransportInfo 的默认与语义：时间戳有效位、bpm 无效时回退 120", "[engine]")
{
    const TransportInfo t;
    CHECK(t.hostNs == 0u);
    CHECK_FALSE(t.hostNsValid);
    CHECK_FALSE(t.isPlaying);
    CHECK_FALSE(t.nonrealtime);
    CHECK(t.bpm == 0.0f);
    CHECK(kMinHostBpm == 20.0f);
    CHECK(kMaxHostBpm == 999.0f);
    CHECK(kFallbackBpm == 120.0f);

    // 节拍同步、宿主没给速度：按 120 BPM 算 1/4 拍 = 0.5 s。
    InputEngine input;
    input.prepare(kSr, 256);
    auto ring = makeRing();
    LoopRing::publishWriter(*ring, kToken, static_cast<std::uint32_t>(kSr), 2);
    EngineParams params;
    params.delayMode = DelayMode::kSync;
    params.delayNote = 10; // 1/4
    HostBuffer host(64);
    input.process(host.view(), params, TransportInfo{}, false, inputCtx(ring.get()));
    CHECK(input.loopStatus().delayEffective == 24000);
    CHECK(input.loopStatus().bpmSource == static_cast<std::uint8_t>(rinne::loop::BpmSource::kFallback));

    // 宿主给了 150 BPM：1/4 拍 = 0.4 s。之后宿主不给了，沿用最后一个有效值。
    TransportInfo withBpm;
    withBpm.bpm = 150.0f;
    input.process(host.view(), params, withBpm, false, inputCtx(ring.get()));
    CHECK(input.loopStatus().delayEffective == 19200);
    input.process(host.view(), params, TransportInfo{}, false, inputCtx(ring.get()));
    CHECK(input.loopStatus().delayEffective == 19200);
    CHECK(input.loopStatus().bpmSource == static_cast<std::uint8_t>(rinne::loop::BpmSource::kHost));
}

TEST_CASE("引擎 混合律：m = mixPercent / 100 的四个点", "[engine]")
{
    // 干声与回送的增益律：g_dry = min(1, 2(1−m))、g_ret = min(1, 2m)。
    // 四个点：0 % → 1/0；25 % → 1/0.5；50 % → 1/1；100 % → 0/1。
    struct Point
    {
        float mix;
        float dry;
        float ret;
    };
    const Point points[] = {{0.0f, 1.0f, 0.0f}, {25.0f, 1.0f, 0.5f}, {50.0f, 1.0f, 1.0f}, {100.0f, 0.0f, 1.0f}};
    for (const Point& p : points)
    {
        InputEngine input;
        input.prepare(kSr, 64);
        EngineParams params;
        params.mixPercent = p.mix;
        params.feedbackPercent = 100.0f;
        HostBuffer host(16);
        input.process(host.view(), params, TransportInfo{}, false, inputCtx(nullptr));
        CHECK(input.dryGain() == p.dry);
        CHECK(input.returnGain() == p.ret);
    }
}

TEST_CASE("引擎 命令序号：基线之外的变化才执行，基线本身不重放；预设与宿主设状态各报各的原因", "[engine]")
{
    struct Requests
    {
        std::vector<FadeReason> reasons;
        static void request(void* context, FadeReason reason) noexcept
        {
            static_cast<Requests*>(context)->reasons.push_back(reason);
        }
    };
    Requests requests;

    InputEngine input;
    CommandBaseline baseline;
    baseline.panicSeq = 7;
    baseline.recoverSeq = 3;
    baseline.presetSeq = 2;
    baseline.stateSeq = 5;
    input.setCommandBaseline(baseline);
    input.setSafetyChain(&requests, &Requests::request);
    input.prepare(kSr, 64);
    CHECK(input.commandBaseline().panicSeq == 7u);

    HostBuffer host(16);
    EngineParams params;
    params.panicSeq = baseline.panicSeq;
    params.recoverSeq = baseline.recoverSeq;
    params.presetSeq = baseline.presetSeq;
    params.stateSeq = baseline.stateSeq;

    // 基线没有变 → 不执行任何一条（没有淡入请求）。
    input.process(host.view(), params, TransportInfo{}, false, inputCtx(nullptr));
    CHECK(requests.reasons.empty());

    // presetSeq 变了 → 请求淡入，原因是 Preset；只请求一次，不重放。
    params.presetSeq = 3;
    input.process(host.view(), params, TransportInfo{}, false, inputCtx(nullptr));
    input.process(host.view(), params, TransportInfo{}, false, inputCtx(nullptr));
    REQUIRE(requests.reasons.size() == 1u);
    CHECK(requests.reasons[0] == FadeReason::Preset);

    // stateSeq 变了 → 原因是 SetState。
    params.stateSeq = 6;
    input.process(host.view(), params, TransportInfo{}, false, inputCtx(nullptr));
    REQUIRE(requests.reasons.size() == 2u);
    CHECK(requests.reasons[1] == FadeReason::SetState);

    // 紧急切断变了：本引擎不发任何引擎段的事件（切断由安全链执行，它有自己的事件段）。
    params.panicSeq = 8;
    input.process(host.view(), params, TransportInfo{}, false, inputCtx(nullptr));
    int engineEvents = 0;
    input.drainEvents([&](const rinne::diag::Event& e) {
        if (e.code >= 0x0400u && e.code <= 0x04FFu)
        {
            ++engineEvents;
        }
    });
    CHECK(engineEvents == 0);
}

TEST_CASE("引擎 旁路：干声逐位直通，回送增益为 0", "[engine]")
{
    InputEngine input;
    input.prepare(kSr, 1024);

    HostBuffer host(32);
    for (int i = 0; i < 32; ++i)
    {
        host.left[static_cast<std::size_t>(i)] = 0.5f;
    }
    host.left[1] = -0.0f;
    HostBuffer before = host;

    EngineParams params;
    params.feedbackPercent = 100.0f;
    params.mixPercent = 50.0f;

    auto ring = makeRing();
    LoopRing::publishWriter(*ring, kToken, static_cast<std::uint32_t>(kSr), 2);
    input.process(host.view(), params, TransportInfo{}, true, inputCtx(ring.get()));
    CHECK(input.returnGain() == 0.0f);
    // 旁路且回送增益从 0 起：干声逐位直通（含 −0.0）。
    CHECK(sameBits(before.left.data(), host.left.data(), 32));
}

TEST_CASE("引擎 0 样本的调用立刻返回，不推进任何计数", "[engine]")
{
    InputEngine input;
    input.prepare(kSr, 1024);

    HostBuffer host(0);
    EngineParams params;
    CHECK_FALSE(input.process(host.view(), params, TransportInfo{}, false, inputCtx(nullptr)));

    // N_obs 没有被 0 样本推进。
    CHECK(input.maxBlockSeen() == 0);

    OutputEngine output;
    output.prepare(kSr, 1024);
    auto ring = makeRing();
    LoopRing::publishWriter(*ring, kToken, static_cast<std::uint32_t>(kSr), 2);
    CHECK_FALSE(output.process(host.view(), params, TransportInfo{}, false, outputCtx(ring.get())));
    CHECK(LoopRing::writeHead(*ring) == 0u);
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

    // 连续写 1 s 多的音频（48 kHz → 48000 帧）：中途拿到 kInFlight，挡满 1 s 之后回收。
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
        if (output.process(host.view(), params, TransportInfo{}, false, outputCtx(ring.get())))
        {
            written += kChunk;
        }
    }

    // 回收发生过了：回收计数 > 0，写头推进了（不再是永久静音）。
    CHECK(output.reclaimCount() > 0u);
    CHECK(LoopRing::writeHead(*ring) > 4096u);
    CHECK(written > 0u);
}

TEST_CASE("引擎 Output 旁路：写进环的数据 10 ms 线性淡到 0，写头照常推进；解除旁路 10 ms 淡回", "[engine]")
{
    // ⛔ 删除式验证点：旁路时直接写 0（阶跃）→ 本用例变红。
    OutputEngine output;
    output.prepare(kSr, 1024);
    auto ring = makeRing();
    LoopRing::publishWriter(*ring, kToken, static_cast<std::uint32_t>(kSr), 2);

    constexpr int kRamp = 480; // 10 ms @ 48 kHz
    HostBuffer host(1024);
    for (auto& v : host.left)
    {
        v = 0.5f;
    }
    for (auto& v : host.right)
    {
        v = 0.5f;
    }
    HostBuffer before = host;
    EngineParams params;

    REQUIRE(output.process(host.view(), params, TransportInfo{}, true, outputCtx(ring.get())));
    CHECK(output.state() == WriteState::kBypassedZeros);
    CHECK(LoopRing::writeHead(*ring) == 1024u); // 写头照常推进
    CHECK(sameBits(before.left.data(), host.left.data(), 1024)); // 宿主 buffer 不动

    std::vector<float> left(1024, 0.0f);
    std::vector<float> right(1024, 0.0f);
    LoopRing::read(*ring, 0, 1024, left.data(), right.data());
    // 单调不增、没有阶跃：相邻两帧的差不超过一步（0.5 / 480）。
    for (int i = 1; i < 1024; ++i)
    {
        CHECK(left[static_cast<std::size_t>(i)] <= left[static_cast<std::size_t>(i - 1)]);
        CHECK(left[static_cast<std::size_t>(i - 1)] - left[static_cast<std::size_t>(i)] <= 0.5f / kRamp + 1.0e-6f);
    }
    CHECK(left[0] > 0.49f);
    CHECK(left[kRamp - 1] == 0.0f);
    CHECK(left[1023] == 0.0f);

    // 解除旁路：10 ms 淡回到原值。
    REQUIRE(output.process(host.view(), params, TransportInfo{}, false, outputCtx(ring.get())));
    LoopRing::read(*ring, 1024, 1024, left.data(), right.data());
    CHECK(left[0] < 0.01f);
    CHECK(left[kRamp - 1] == 0.5f);
    CHECK(left[1023] == 0.5f);
}

TEST_CASE("引擎 mono：Output 两格写同一个值；Input 的回送取两格的平均", "[engine]")
{
    auto ring = makeRing();
    LoopRing::publishWriter(*ring, kToken, static_cast<std::uint32_t>(kSr), 2);
    OutputEngine output;
    output.prepare(kSr, 256);

    std::vector<float> mono(64, 0.0f);
    for (int i = 0; i < 64; ++i)
    {
        mono[static_cast<std::size_t>(i)] = static_cast<float>(i) * 0.01f;
    }
    float* channels[1] = {mono.data()};
    AudioView view;
    view.channels = channels;
    view.numChannels = 1;
    view.length = 64;
    EngineParams params;
    REQUIRE(output.process(view, params, TransportInfo{}, false, outputCtx(ring.get())));

    float left[64]{};
    float right[64]{};
    LoopRing::read(*ring, 0, 64, left, right);
    for (int i = 0; i < 64; ++i)
    {
        CHECK(left[i] == mono[static_cast<std::size_t>(i)]);
        CHECK(right[i] == mono[static_cast<std::size_t>(i)]);
    }
}

TEST_CASE("引擎 闭环：一个冲激恰在 D 个样本之后回到读头，增益为 1 时逐位是原值", "[engine][loop]")
{
    // ⛔ 删除式验证点：混音用推进之后的读位（R + n）→ 冲激提前 n 个样本出现 → 本用例变红。
    constexpr int kD = 480; // 10 ms
    constexpr int kAt = 2000;
    Loop loop(256);
    const EngineParams params = unityLoopParams(10.0f);
    const auto out = loop.run(params, 256, 4096, [=](int t) { return t == kAt ? 0.5f : 0.0f; });

    CHECK(out[kAt] == 0.5f); // 干声
    CHECK(out[kAt + kD - 1] == 0.0f);
    CHECK(out[kAt + kD] == 0.5f); // 第一次回来：恰好 D 个样本之后
    CHECK(out[kAt + kD + 1] == 0.0f);
    CHECK(out[kAt + 2 * kD] == 0.5f); // 第二次（反馈 100 %）
    CHECK(loop.input.loopStatus().delayEffective == kD);
    CHECK(loop.input.loopStatus().resyncCount == 0u);
}

TEST_CASE("引擎 闭环：Output 先于 Input 配上、写头早已很大时，拉环照样追上写头", "[engine][loop]")
{
    // ⛔ 删除式验证点：配对时不把本地延迟线锚在写头（从 0 起拉）→ 回送永远是 0 → 本用例变红。
    Loop loop(256);
    // Output 先单独跑了 1 s（写头 48000 帧），Input 才来。
    EngineParams params = unityLoopParams(10.0f);
    for (int i = 0; i < 48000 / 256; ++i)
    {
        HostBuffer host(256);
        loop.output.process(host.view(), params, TransportInfo{}, false, outputCtx(loop.ring.get()));
    }
    REQUIRE(LoopRing::writeHead(*loop.ring) == static_cast<std::uint64_t>((48000 / 256) * 256));

    constexpr int kAt = 3000;
    const auto out = loop.run(params, 256, 6000, [=](int t) { return t == kAt ? 0.25f : 0.0f; });
    CHECK(out[kAt + 480] == 0.25f);
    const auto status = loop.input.loopStatus();
    CHECK(status.overflow == 0u); // 配对时锚在写头，不是「丢了一圈」
    CHECK(status.drift == 0);
}

TEST_CASE("引擎 闭环 L1-05：参数恒定时同一段输入按不同块长处理，输出逐位相同", "[engine][loop]")
{
    // ⛔ 删除式验证点：包络或增益按块推进（而不是按样本）→ 本用例变红。
    auto source = [](int t) {
        // 一段确定的信号：几个冲激加一段慢变的斜坡，幅度都在 ±0.3 里。
        const float ramp = static_cast<float>((t * 7) % 1000) * 0.0003f - 0.15f;
        return (t % 997 == 0 ? 0.15f : 0.0f) + ramp;
    };
    const EngineParams params = unityLoopParams(12.5f); // 600 样本

    auto runWith = [&](int blockLength) {
        Loop loop(256);
        return loop.run(params, blockLength, 8192, source);
    };
    const auto a = runWith(64);
    const auto b = runWith(128);
    const auto c = runWith(256);
    REQUIRE(a.size() == b.size());
    REQUIRE(a.size() == c.size());
    CHECK(sameBits(a.data(), b.data(), a.size()));
    CHECK(sameBits(a.data(), c.data(), a.size()));
}

TEST_CASE("引擎 闭环：起播复位按样本定位，从 s0 起与宿主怎么切块无关", "[engine][loop]")
{
    // ⛔ 删除式验证点：起播复位在「淡出走完的那一块」才清本地延迟线（按块定位）→ 本用例变红。
    auto source = [](int t) { return static_cast<float>((t * 13) % 701) * 0.0004f - 0.14f; };
    const EngineParams params = unityLoopParams(10.0f);
    constexpr int kEdge = 2048; // 64、128、256 的公倍数：三种切块都在这个样本上起播
    auto transportAt = [=](int at) {
        TransportInfo t;
        t.isPlaying = at >= kEdge;
        return t;
    };
    auto runWith = [&](int blockLength) {
        Loop loop(256);
        return loop.run(params, blockLength, 8192, source, transportAt);
    };
    const auto a = runWith(64);
    const auto b = runWith(128);
    const auto c = runWith(256);
    CHECK(sameBits(a.data(), b.data(), a.size()));
    CHECK(sameBits(a.data(), c.data(), a.size()));

    // 起播沿之后的 T = 5 ms（240 样本）里写下的数据不回来：s0 + D 起的 240 个样本只有干声。
    Loop loop(256);
    const auto out = loop.run(params, 256, 8192, [=](int t) { return t == kEdge + 100 ? 0.5f : 0.0f; }, transportAt);
    CHECK(out[kEdge + 100 + 480] == 0.0f); // 沿后 T 帧之内写下的冲激被跳过
}

TEST_CASE("引擎 闭环：错序判据——Input 在前判为正确，Output 在前连续 3 块后判为错序", "[engine][loop]")
{
    // ⛔ 删除式验证点：把判据写反（对端时间戳小于本块就记一票）→ 本用例变红。
    auto transportAt = [](int at) {
        TransportInfo t;
        t.hostNs = 1000000u + static_cast<std::uint64_t>(at) * 1000u;
        t.hostNsValid = true;
        return t;
    };
    const EngineParams params = unityLoopParams(10.0f);

    Loop correct(256);
    correct.run(params, 256, 256 * 8, [](int) { return 0.0f; }, transportAt);
    auto status = correct.input.loopStatus();
    CHECK(status.hostNsValid == 1u);
    CHECK(status.orderVerdict == static_cast<std::uint8_t>(rinne::loop::OrderVerdict::kCorrect));
    CHECK(status.orderVotes == 0u);

    Loop wrong(256);
    wrong.outputFirst = true;
    wrong.run(params, 256, 256 * 2, [](int) { return 0.0f; }, transportAt);
    status = wrong.input.loopStatus();
    CHECK(status.orderVerdict == static_cast<std::uint8_t>(rinne::loop::OrderVerdict::kUnknown)); // 2 票还不够
    wrong.run(params, 256, 256 * 2, [](int) { return 0.0f; }, transportAt);
    status = wrong.input.loopStatus();
    CHECK(status.orderVerdict == static_cast<std::uint8_t>(rinne::loop::OrderVerdict::kOutputFirst));
    CHECK(status.orderVotes == 3u);

    // 宿主不给时间戳：恒为 unknown，票数清零。
    wrong.run(params, 256, 256, [](int) { return 0.0f; });
    status = wrong.input.loopStatus();
    CHECK(status.orderVerdict == static_cast<std::uint8_t>(rinne::loop::OrderVerdict::kUnknown));
    CHECK(status.orderVotes == 0u);
    CHECK(status.hostNsValid == 0u);
}

TEST_CASE("引擎 端点槽：两端各自发布自己的槽（块序号、时间戳、实时标志），d_eff 在 Input 槽", "[engine][loop]")
{
    Loop loop(256);
    TransportInfo transport;
    transport.hostNs = 777u;
    transport.hostNsValid = true;
    transport.nonrealtime = true;
    const EngineParams params = unityLoopParams(10.0f);
    loop.run(params, 256, 256 * 3, [](int) { return 0.0f; }, [&](int) { return transport; });

    const RoleSlot& in = roleSlot(*loop.rv, 1, Role::kInput);
    const RoleSlot& out = roleSlot(*loop.rv, 1, Role::kOutput);
    CHECK(in.block_seq.load() == 3u);
    CHECK(out.block_seq.load() == 3u);
    CHECK(in.last_host_ns.load() == 777u);
    CHECK(out.last_host_ns.load() == 777u);
    CHECK((in.rt_flags.load() & kRtNonrealtime) != 0u);
    CHECK((out.rt_flags.load() & kRtNonrealtime) != 0u);
    CHECK(in.d_eff.load() == 480u);
    CHECK(out.d_eff.load() == 0u); // Output 不写 d_eff

    // 进入离线渲染的事件只发一次。
    int nonRealtime = 0;
    loop.input.drainEvents([&](const rinne::diag::Event& e) {
        if (e.code == static_cast<std::uint16_t>(EngineEventCode::kNonRealtime))
        {
            ++nonRealtime;
        }
    });
    CHECK(nonRealtime == 1);
}

TEST_CASE("引擎 诊断：环路层的事件进引擎的队列；诊断安全段的六个量都有出口", "[engine][loop]")
{
    Loop loop(256);
    const EngineParams params = unityLoopParams(10.0f);
    // 宿主违约：Input 收到 512 帧（声明 256）→ 环路层发 kHostViolation 与 kDMinRaised。
    {
        HostBuffer host(512);
        loop.input.process(host.view(), params, TransportInfo{}, false, inputCtx(loop.ring.get(), loop.rv.get()));
    }
    std::vector<std::uint16_t> codes;
    loop.input.drainEvents([&](const rinne::diag::Event& e) { codes.push_back(e.code); });
    const auto has = [&](rinne::loop::LoopEventCode code) {
        return std::find(codes.begin(), codes.end(), static_cast<std::uint16_t>(code)) != codes.end();
    };
    CHECK(has(rinne::loop::LoopEventCode::kHostViolation));
    const auto status = loop.input.loopStatus();
    CHECK(status.maxBlockSeen == 512);
    CHECK(status.delayMin == 512);
    CHECK(status.delayMinReason == static_cast<std::uint8_t>(rinne::loop::DelayMinReason::kHostViolation));

    // 诊断安全段：写方清洗的计数从环头读出来；读头峰值按 dBFS 给。
    {
        HostBuffer host(64);
        host.left[0] = std::numeric_limits<float>::quiet_NaN();
        host.left[1] = 1.0e6f;
        loop.output.process(host.view(), params, TransportInfo{}, false, outputCtx(loop.ring.get()));
        HostBuffer in(64);
        loop.input.process(in.view(), params, TransportInfo{}, false, inputCtx(loop.ring.get(), loop.rv.get()));
    }
    const SafetyDiag diag = loop.input.safetyDiag();
    CHECK(diag.nonfiniteRing == 1u);
    CHECK(diag.clampRing == 1u);
    CHECK(diag.trips == 0u);
    CHECK(diag.limiterGrMaxDb == 0.0f);
    CHECK(diag.tailPeakMaxDb <= 0.0f);
}
