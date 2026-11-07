// SPDX-License-Identifier: GPL-3.0-or-later
//
// 安静的起播沿（LoopSession::quietPlayEdge）在环路层的落点。
//
// 起播沿上没有旧回送要淡出、写头之前 20 ms 是数字静音时，T 取 0：读头直接释放，DSP 的复位点就是沿所在块的块首 s0，
// 新读头从 W 起读、不带 onset，沿后头几毫秒写下的声音照常回来。不安静时照旧：复位点 s0 + 5 ms，W + T 之前的帧不读。
//
// 驱动方式：每个周期 Input 先处理一块，Output 接着往环里写同样多的帧（帧值是绝对位置的函数），所以 R 与 W 同步前进、
// 偏移为 0，读头在 R 上读的是环位置 R − D。回送是读头的原始混音（这一层没有增益），冲激读回来就是它本身。
//
// 判据的每一条各有正反例：
//   - 写头之前 20 ms 有一个非零帧就不安静，20 ms 之外的不算；
//   - 读头在沿上要读的帧（W − D 起）有一个非零帧就不安静，W − D 之前的不算；
//   - 挂起清空时末值不为 0（末值保持在走）就不安静，末值为 0 时安静；
//   - 第一次配上的那一块照旧走 5 ms 的路径；
//   - NaN 不算 0（已经拉进来的部分与本块还没拉的部分各一条），-0.0 算 0；
//   - 本块还没拉的帧超过只读扫描的上界（32768 帧）就判不安静；
//   - 同一输入两种切法：判定、复位点与回送逐位相同。
//
// 标 ⛔ 的是删除式验证点：把对应的实现改坏，本文件必须有用例变红。
//   ⛔ 安静判据恒为假 → 「静音之后的起播沿」变红；
//   ⛔ 去掉「写头之前 20 ms」一条 → 「写头之前 20 ms 有声」变红；
//   ⛔ 去掉「读头在沿上要读的帧」一条 → 「读头要读的帧有声」变红；
//   ⛔ 去掉「末值保持在走」一条 → 「挂起清空时末值不为 0」变红；
//   ⛔ 第一次配上的那一块也判 → 「第一次配上的那一块」变红；
//   ⛔ 拉环时不记非零帧 / 只读扫描不看环 → 两条 NaN 用例各自变红；
//   ⛔ 去掉扫描上界 → 「超过扫描上界」变红。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include "engine/EngineParams.h"
#include "engine/TransportInfo.h"
#include "ipc/LoopRing.h"
#include "loop/LoopSession.h"
#include "loop/LoopStatus.h"
#include "loop/StreamSync.h"

namespace
{

using namespace rinne::loop;
using namespace rinne::engine;
namespace ipc = rinne::ipc;

constexpr double kSr = 48000.0;
constexpr std::int64_t kPlayFade = 240; // T = 5 ms
constexpr std::size_t kMixCapacity = static_cast<std::size_t>(ipc::kMaxSupportedBlock) + 8u;
constexpr std::uint32_t kWriteChunk = 8192;

std::unique_ptr<ipc::RingSegment> makeRing()
{
    auto seg = std::make_unique<ipc::RingSegment>();
    ipc::LoopRing::initGeometry(*seg);
    return seg;
}

// 往环里写 frames 帧，帧值 frameAt(绝对位置)（左右相同）。长的分段写。
template<typename FrameAt>
void writeWith(ipc::RingSegment& ring, std::int64_t frames, FrameAt&& frameAt)
{
    while (frames > 0)
    {
        const auto chunk = static_cast<std::uint32_t>(std::min<std::int64_t>(frames, kWriteChunk));
        const ipc::RingWriteResult result =
            ipc::LoopRing::write(ring, chunk, [&](std::uint32_t, std::uint64_t p, float& l, float& r) {
                l = frameAt(static_cast<std::int64_t>(p));
                r = l;
            });
        REQUIRE(result == ipc::RingWriteResult::kOk);
        frames -= chunk;
    }
}

EngineParams delayParams(float delayMs)
{
    EngineParams params;
    params.delayMs = delayMs;
    return params;
}

struct EdgeSetup
{
    std::vector<int> blocks; // 每个周期的块长
    std::size_t stopCycles = 0; // 前几个周期报停：第 stopCycles 个周期就是起播沿
    float delayMs = 10.0f;
    std::int64_t declared = 256;
    bool flushBeforeEdge = false; // 沿那一块之前请求挂起清空（两端停用超过 250 ms）
    std::int64_t extraBeforeEdge = 0; // 沿那一块之前 Output 多写的帧（帧值同样由 frameAt 给）
};

struct EdgeRun
{
    std::vector<float> out; // 每个样本的回送（左声道）
    bool edgeResynced = false;
    SyncReason edgeReason = SyncReason::kOffset;
    bool edgeResetDsp = false;
    std::int64_t edgeResetAt = -1;
    std::int64_t edgeStart = 0; // 沿所在块的块首（R）
    std::int64_t edgeW = 0; // 沿上的写头
    std::int64_t delay = 0;
    std::vector<std::int64_t> resets; // resetDsp 的复位点
    std::uint32_t playResyncs = 0;
};

template<typename FrameAt>
EdgeRun runEdge(const EdgeSetup& setup, FrameAt&& frameAt)
{
    LoopSession session;
    REQUIRE(session.prepare(kSr));
    std::vector<float> mixLeft(kMixCapacity, 0.0f);
    std::vector<float> mixRight(kMixCapacity, 0.0f);
    std::vector<std::uint8_t> mixValid(kMixCapacity, 0u);
    auto ring = makeRing();
    ipc::LoopRing::publishWriter(*ring, 1, static_cast<std::uint32_t>(kSr), 2);
    const EngineParams params = delayParams(setup.delayMs);

    EdgeRun run;
    std::int64_t written = 0;
    for (std::size_t i = 0; i < setup.blocks.size(); ++i)
    {
        const int n = setup.blocks[i];
        TransportInfo transport;
        transport.isPlaying = i >= setup.stopCycles;
        if (i == setup.stopCycles)
        {
            if (setup.extraBeforeEdge > 0)
            {
                writeWith(*ring, setup.extraBeforeEdge, frameAt);
                written += setup.extraBeforeEdge;
            }
            if (setup.flushBeforeEdge)
            {
                session.flushAfterSuspend();
            }
            run.edgeStart = session.processed();
            run.edgeW = written;
        }
        BlockContext ctx(params, transport);
        ctx.ring = ring.get();
        ctx.role = ipc::Role::kInput;
        ctx.declaredBlock = setup.declared;
        ctx.observedBlock = setup.declared;
        ctx.blockLength = n;
        ctx.returnLeft = mixLeft.data();
        ctx.returnRight = mixRight.data();
        ctx.returnValid = mixValid.data();
        const BlockResult r = session.process(ctx);
        for (int k = 0; k < n; ++k)
        {
            run.out.push_back(r.returnLeft[k]);
        }
        if (i == setup.stopCycles)
        {
            run.edgeResynced = r.resynced;
            run.edgeReason = r.resyncReason;
            run.edgeResetDsp = r.resetDsp;
            run.edgeResetAt = r.resetAtSample;
            run.delay = r.delayEffective;
        }
        if (r.resetDsp && r.resetAtSample >= 0)
        {
            run.resets.push_back(r.resetAtSample);
        }
        writeWith(*ring, n, frameAt);
        written += n;
    }
    run.playResyncs = session.statusFields().resyncByReason[static_cast<std::size_t>(SyncReason::kPlay)];
    return run;
}

std::vector<int> fixedBlocks(int n, std::size_t count)
{
    return std::vector<int>(count, n);
}

// 安静：复位点就是沿所在块的块首，沿那一块就置 resetDsp。
void checkQuiet(const EdgeRun& run)
{
    CHECK(run.edgeResynced);
    CHECK(run.edgeReason == SyncReason::kPlay);
    CHECK(run.edgeResetDsp);
    CHECK(run.edgeResetAt == run.edgeStart);
    REQUIRE(run.resets.size() == 1u);
    CHECK(run.resets[0] == run.edgeStart);
    CHECK(run.playResyncs == 1u);
}

// 不安静：照旧，复位点 s0 + 5 ms（沿上就预告，复位点所在的那一块复位）。
void checkNotQuiet(const EdgeRun& run)
{
    CHECK(run.edgeResynced);
    CHECK(run.edgeReason == SyncReason::kPlay);
    CHECK(run.edgeResetAt == run.edgeStart + kPlayFade);
    REQUIRE(run.resets.size() == 1u);
    CHECK(run.resets[0] == run.edgeStart + kPlayFade);
    CHECK(run.playResyncs == 1u);
}

// 沿后 offset 处的冲激（0.5）：安静时读头从 W 起读、不带 onset，回声恰为 0.5；不安静时它落在 W + T 之前，回声为 0。
float echoOf(const EdgeRun& run, std::int64_t offset)
{
    const auto at = static_cast<std::size_t>(run.edgeW + offset + run.delay);
    REQUIRE(at < run.out.size());
    return run.out[at];
}

} // namespace

TEST_CASE("安静的起播沿：静音之后的起播沿 T 取 0，复位点在沿所在块的块首，沿后的冲激满幅回来", "[loop][quiet-edge]")
{
    EdgeSetup setup;
    setup.blocks = fixedBlocks(256, 40);
    setup.stopCycles = 8;
    const EdgeRun probe = runEdge(setup, [](std::int64_t) { return 0.0f; });
    const std::int64_t W = probe.edgeW;
    REQUIRE(probe.delay == 480);

    // 冲激在沿后 100 个样本：落在旧口径的 5 ms 排除窗口里
    const EdgeRun run = runEdge(setup, [W](std::int64_t p) { return p == W + 100 ? 0.5f : 0.0f; });
    checkQuiet(run);
    REQUIRE(run.resets.size() == 1u);
    CHECK(run.resets[0] == run.edgeStart);
    CHECK(echoOf(run, 100) == 0.5f);
    CHECK(echoOf(run, 99) == 0.0f);
    CHECK(echoOf(run, 101) == 0.0f);
    // 冲激在沿上第一帧：同样满幅（没有 onset）
    const EdgeRun first = runEdge(setup, [W](std::int64_t p) { return p == W ? 0.5f : 0.0f; });
    checkQuiet(first);
    CHECK(echoOf(first, 0) == 0.5f);
}

TEST_CASE("安静的起播沿：写头之前 20 ms 有声就不安静，20 ms 之外的不算", "[loop][quiet-edge]")
{
    // D = 480（10 ms）：读头在沿上要读的帧从 W − 480 起，这里只看「写头之前 20 ms」那一条。
    EdgeSetup setup;
    setup.blocks = fixedBlocks(256, 40);
    setup.stopCycles = 8;
    const std::int64_t W = 8 * 256;

    const EdgeRun inside =
        runEdge(setup, [W](std::int64_t p) { return p == W - 700 ? 0.25f : (p == W + 100 ? 0.5f : 0.0f); });
    REQUIRE(inside.delay == 480);
    checkNotQuiet(inside);
    CHECK(echoOf(inside, 100) == 0.0f); // 旧口径：沿后 T 帧之内写下的冲激被跳过

    const EdgeRun outside =
        runEdge(setup, [W](std::int64_t p) { return p == W - 961 ? 0.25f : (p == W + 100 ? 0.5f : 0.0f); });
    checkQuiet(outside);
    CHECK(echoOf(outside, 100) == 0.5f);

    // 本块还没拉的部分（对端上一块 [W − 256, W)）里有声也一样不安静
    const EdgeRun unpulled = runEdge(setup, [W](std::int64_t p) { return p == W - 100 ? 0.25f : 0.0f; });
    checkNotQuiet(unpulled);
}

TEST_CASE("安静的起播沿：读头在沿上要读的帧有声就不安静，读不到的不算", "[loop][quiet-edge]")
{
    // D = 4800（100 ms）：读头在沿上从 W − 4800 起读，比 20 ms 的窗口长。
    EdgeSetup setup;
    setup.blocks = fixedBlocks(256, 60);
    setup.stopCycles = 24;
    setup.delayMs = 100.0f;
    const std::int64_t W = 24 * 256;

    const EdgeRun inside = runEdge(setup, [W](std::int64_t p) { return p == W - 4000 ? 0.25f : 0.0f; });
    REQUIRE(inside.delay == 4800);
    checkNotQuiet(inside);

    // W − 4801：读头在沿前最后一个样本上读到它（回送在沿前一个样本是 0.25），沿上起要读的帧全是 0：安静。
    // 释放读头与淡出逐位相同（沿上都只出 0）。
    const EdgeRun justBefore = runEdge(setup, [W](std::int64_t p) { return p == W - 4801 ? 0.25f : 0.0f; });
    checkQuiet(justBefore);
    CHECK(justBefore.out[static_cast<std::size_t>(W - 1)] == 0.25f);
    CHECK(justBefore.out[static_cast<std::size_t>(W)] == 0.0f);
}

TEST_CASE("安静的起播沿：挂起清空时末值不为 0 就不安静，末值为 0 时安静", "[loop][quiet-edge]")
{
    // D = 4800。挂起清空作废读头、清空延迟线；上一块最后一个回送样本按末值保持 5 ms 衰减。
    EdgeSetup setup;
    setup.blocks = fixedBlocks(256, 60);
    setup.stopCycles = 24;
    setup.delayMs = 100.0f;
    setup.flushBeforeEdge = true;
    const std::int64_t W = 24 * 256;

    // 帧 p < 3000 是 0.25：沿前最后一个样本（R = W − 1）读的是 W − 4801 = 1343，末值 0.25，保持在走
    const EdgeRun held = runEdge(setup, [](std::int64_t p) { return p < 3000 ? 0.25f : 0.0f; });
    checkNotQuiet(held);
    CHECK(held.out[static_cast<std::size_t>(W)] == 0.25f); // 保持的第一个样本等于末值

    // 帧 p < 1000 是 0.25：末值是 0，没有保持；清空之后沿上什么都读不到，W 之前 20 ms 也是 0：安静
    const EdgeRun clean = runEdge(setup, [](std::int64_t p) { return p < 1000 ? 0.25f : 0.0f; });
    checkQuiet(clean);
}

TEST_CASE("安静的起播沿：第一次配上的那一块照旧走 5 ms 的路径", "[loop][quiet-edge]")
{
    EdgeSetup setup;
    setup.blocks = fixedBlocks(256, 20);
    setup.stopCycles = 0; // 第一块就在走带：起播沿与第一次配上落在同一块
    const EdgeRun run = runEdge(setup, [](std::int64_t) { return 0.0f; });
    checkNotQuiet(run);
    CHECK(run.edgeStart == 0);
}

TEST_CASE("安静的起播沿：NaN 不算 0，-0.0 算 0", "[loop][quiet-edge]")
{
    EdgeSetup setup;
    setup.blocks = fixedBlocks(256, 40);
    setup.stopCycles = 8;
    const std::int64_t W = 8 * 256;
    const float nan = std::numeric_limits<float>::quiet_NaN();

    // 已经拉进来的部分（W − 700 在对端上一块之前）
    const EdgeRun pulled = runEdge(setup, [W, nan](std::int64_t p) { return p == W - 700 ? nan : 0.0f; });
    checkNotQuiet(pulled);
    // 本块还没拉的部分（W − 100 在对端上一块 [W − 256, W) 里）
    const EdgeRun unpulled = runEdge(setup, [W, nan](std::int64_t p) { return p == W - 100 ? nan : 0.0f; });
    checkNotQuiet(unpulled);
    // 全是 -0.0：安静，沿后的冲激满幅回来
    const EdgeRun negZero = runEdge(setup, [W](std::int64_t p) { return p == W + 100 ? 0.5f : -0.0f; });
    checkQuiet(negZero);
    CHECK(echoOf(negZero, 100) == 0.5f);
}

TEST_CASE("安静的起播沿：本块还没拉的帧超过只读扫描的上界就判不安静", "[loop][quiet-edge]")
{
    // 沿那一块之前对端多写了一大段（全是 0）：本块还没拉的帧 = 对端上一块 256 + 多写的部分。
    EdgeSetup setup;
    setup.blocks = fixedBlocks(256, 20);
    setup.stopCycles = 8;

    setup.extraBeforeEdge = 30000; // 30256 帧，在 32768 之内：看完全是 0，安静
    checkQuiet(runEdge(setup, [](std::int64_t) { return 0.0f; }));

    setup.extraBeforeEdge = 40000; // 40256 帧，超过上界：不看，判不安静
    checkNotQuiet(runEdge(setup, [](std::int64_t) { return 0.0f; }));
}

TEST_CASE("安静的起播沿：同一输入两种切法，判定、复位点与回送逐位相同", "[loop][quiet-edge]")
{
    // D = 4800，声明块长 1024（两种切法下 D 的下限相同）。沿在 6144：两种切法都以它为块边界。
    // 切法 b 沿前最后一块 700 帧：W − 500 在一种切法里已经拉进来，在另一种里还在环里没拉。
    const std::vector<int> pre = {1000, 37, 1024, 513, 999, 1, 777, 1000, 93, 700};
    const std::vector<int> post = {300, 1024, 17, 640, 1024, 1024, 999, 1024, 1024, 1000, 116};
    EdgeSetup a;
    a.blocks = fixedBlocks(256, 24 + 32);
    a.stopCycles = 24;
    a.delayMs = 100.0f;
    a.declared = 1024;
    EdgeSetup b = a;
    b.blocks = pre;
    b.blocks.insert(b.blocks.end(), post.begin(), post.end());
    b.stopCycles = pre.size();
    const std::int64_t W = 6144;

    const auto quietInput = [W](std::int64_t p) { return p == W + 100 ? 0.5f : (p < W - 4801 ? 0.25f : 0.0f); };
    const auto noisyInput = [W](std::int64_t p) { return p == W - 500 || p == W + 300 ? 0.5f : 0.0f; };

    const EdgeRun qa = runEdge(a, quietInput);
    const EdgeRun qb = runEdge(b, quietInput);
    REQUIRE(qa.edgeStart == W);
    REQUIRE(qb.edgeStart == W);
    REQUIRE(qa.delay == 4800);
    REQUIRE(qb.delay == 4800);
    checkQuiet(qa);
    checkQuiet(qb);
    CHECK(echoOf(qa, 100) == 0.5f);
    REQUIRE(qa.out.size() == qb.out.size());
    CHECK(std::memcmp(qa.out.data(), qb.out.data(), qa.out.size() * sizeof(float)) == 0);

    const EdgeRun na = runEdge(a, noisyInput);
    const EdgeRun nb = runEdge(b, noisyInput);
    checkNotQuiet(na);
    checkNotQuiet(nb);
    CHECK(na.resets == nb.resets);
    REQUIRE(na.out.size() == nb.out.size());
    CHECK(std::memcmp(na.out.data(), nb.out.data(), na.out.size() * sizeof(float)) == 0);
}
