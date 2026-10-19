// SPDX-License-Identifier: GPL-3.0-or-later
//
// 回收中途停手的压测：写方公告了整整一圈、样本都写好了，只差发布写头；回收开始填零的同时，写方线程一看到第一帧
// 变成 0 就发布。回收每填 1024 帧看一眼写头，看到写方已经发布就停手：那一圈里写方的帧大半保住，被盖成静音的只有
// 开头一小段。每一轮都核对：
//   - 写头最后是 intent（不论谁赢）；
//   - 每一帧要么是 0，要么是本位置的编码值，从不出现别的位置的数据；
//   - 回收赢了写头时整圈都是 0。
// 停在半路要靠调度（写方得在回收填完一圈之前发布），所以这条放在压测里：一轮一轮地跑，直到停在半路的轮数够了，
// 或者到时间封顶。去掉回收的写头检查时，回收总是填满一圈，停在半路的轮数恒为 0，这条变红。
// 工作线程不碰 Catch2，只计数；断言全在主线程做。
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>

#include "ipc/LoopRing.h"

namespace
{

using namespace rinne::ipc;
using Clock = std::chrono::steady_clock;

constexpr u32 kMinTrials = 50;
constexpr u32 kWantedStops = 3;
constexpr long long kCapMs = 3000;

float encodeL(u64 p) noexcept
{
    return static_cast<float>(p & 0xFFFFFFu);
}

float encodeR(u64 p) noexcept
{
    return static_cast<float>((p >> 24) & 0xFFFFFFu);
}

bool decode(u64 p, float l, float r) noexcept
{
    const auto el = static_cast<std::uint32_t>(l);
    const auto er = static_cast<std::uint32_t>(r);
    return static_cast<float>(el) == l && static_cast<float>(er) == r && el == (p & 0xFFFFFFu) &&
           er == ((p >> 24) & 0xFFFFFFu);
}

} // namespace

TEST_CASE("a reclaim stops zeroing once the stalled writer publishes", "[stress][ring]")
{
    if (std::thread::hardware_concurrency() < 2)
    {
        SKIP("需要两个硬件线程");
    }
    auto seg = std::make_unique<RingSegment>(); // 值初始化 = 全零
    LoopRing::initGeometry(*seg);
    const u64 mask = static_cast<u64>(kRingFrames - 1);

    u32 trials = 0;
    u32 stops = 0;
    u32 reclaimWins = 0;
    u32 badHead = 0;
    u32 badOther = 0;
    u32 badNoZero = 0;
    u32 badWinKept = 0;
    const auto start = Clock::now();
    long long elapsedMs = 0;
    while (trials < kMinTrials || (stops < kWantedStops && elapsedMs < kCapMs))
    {
        ++trials;
        // + 1：第一帧的左声道永远不是 0。写方拿「第一帧左声道变成 0」当回收开始的信号，位置是 2^24 的倍数时
        // 左声道本来就是 0，写方会在回收开始之前就发布
        const u64 w0 = static_cast<u64>(trials) * kRingFrames + 1u;
        const u64 intent = w0 + kRingFrames;
        for (u64 p = w0; p != intent; ++p)
        {
            seg->data[2 * (p & mask)].store(encodeL(p), std::memory_order_relaxed);
            seg->data[2 * (p & mask) + 1].store(encodeR(p), std::memory_order_relaxed);
        }
        seg->header.write_head.store(w0);
        seg->header.write_intent.store(intent);

        std::atomic<bool> watching{false};
        std::atomic<bool> giveUp{false}; // 回收若一帧都没填就返回，写方别永远等下去
        std::thread writer([&] {
            watching.store(true);
            while (seg->data[2 * (w0 & mask)].load(std::memory_order_relaxed) != 0.0f)
            {
                if (giveUp.load())
                {
                    return;
                }
            }
            u64 expected = w0;
            (void)seg->header.write_head.compare_exchange_strong(expected, intent, std::memory_order_release,
                                                                 std::memory_order_relaxed);
        });
        while (!watching.load())
        {
        }
        const bool reclaimed = LoopRing::reclaimStalled(*seg);
        giveUp.store(true);
        writer.join();

        badHead += LoopRing::writeHead(*seg) == intent ? 0u : 1u;
        u32 zeros = 0;
        u32 own = 0;
        for (u64 p = w0; p != intent; ++p)
        {
            const float l = seg->data[2 * (p & mask)].load(std::memory_order_relaxed);
            const float r = seg->data[2 * (p & mask) + 1].load(std::memory_order_relaxed);
            if (l == 0.0f && r == 0.0f)
            {
                ++zeros;
            }
            else if (decode(p, l, r))
            {
                ++own;
            }
            else
            {
                ++badOther;
            }
        }
        badNoZero += zeros >= 1u ? 0u : 1u;
        if (reclaimed)
        {
            ++reclaimWins;
            badWinKept += own == 0u ? 0u : 1u; // 回收赢了写头：整圈都是 0，写方随后的 CAS 落空
        }
        stops += (!reclaimed && own > 0u) ? 1u : 0u;
        elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    }

    std::printf("[stress][reclaim] trials=%u stops=%u reclaimWins=%u badHead=%u badOther=%u badNoZero=%u "
                "badWinKept=%u ms=%lld\n",
                trials, stops, reclaimWins, badHead, badOther, badNoZero, badWinKept, elapsedMs);
    std::fflush(stdout);
    CHECK(badHead == 0u);
    CHECK(badOther == 0u);
    CHECK(badNoZero == 0u);
    CHECK(badWinKept == 0u);
    CHECK(stops >= 1u);
}
