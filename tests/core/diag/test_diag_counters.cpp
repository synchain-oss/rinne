// SPDX-License-Identifier: GPL-3.0-or-later
//
// 音频线程的诊断计数：总数、零样本、超限、旁路各自独立；直方图只收非零块，合计等于非零块数；
// 没有非零块时 nMin 保持 0（哨兵值不泄漏到快照里）。桶边界按 <=64、<=128……分段，64 与 65 必须落在
// 不同的桶，4096 与 4097 也是。
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include "diag/DiagCounters.h"

namespace
{

using namespace rinne::diag;

// 桶计数与总块数同宽：高采样率小块长时 32 位的桶约 16 天就回绕，总数却不回绕
static_assert(sizeof(DiagCountersSnapshot::hist[0]) == sizeof(DiagCountersSnapshot::blocksTotal),
              "直方图桶要和总块数同宽");

std::uint64_t histogramTotal(const DiagCountersSnapshot& snapshot)
{
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < kNsamplesBuckets; ++i)
    {
        total += snapshot.hist[i];
    }
    return total;
}

} // namespace

TEST_CASE("block counters separate zero, over-max and bypassed calls", "[diag]")
{
    DiagCounters counters;
    const DiagCountersSnapshot empty = counters.snapshot();
    CHECK(empty.blocksTotal == 0u);
    CHECK(empty.zeroLen == 0u);
    CHECK(empty.overMax == 0u);
    CHECK(empty.bypassed == 0u);
    CHECK(empty.nMin == 0u);
    CHECK(empty.nMax == 0u);
    CHECK(histogramTotal(empty) == 0u);

    counters.onBlock(0, 512, true);
    counters.onBlock(512, 512, false);
    counters.onBlock(513, 512, false);
    counters.onBlock(256, 512, true);
    // declaredMax 为 0 表示宿主还没给出上限：不算超限，但直方图与最值照常记
    counters.onBlock(1024, 0, false);

    const DiagCountersSnapshot snapshot = counters.snapshot();
    CHECK(snapshot.blocksTotal == 5u);
    CHECK(snapshot.zeroLen == 1u);
    CHECK(snapshot.overMax == 1u);
    CHECK(snapshot.bypassed == 2u);
    CHECK(snapshot.nMin == 256u);
    CHECK(snapshot.nMax == 1024u);
    CHECK(snapshot.hist[0] == 0u);
    CHECK(snapshot.hist[1] == 0u);
    CHECK(snapshot.hist[2] == 1u);
    CHECK(snapshot.hist[3] == 1u);
    CHECK(snapshot.hist[4] == 2u);
    CHECK(snapshot.hist[5] == 0u);
    CHECK(snapshot.hist[6] == 0u);
    CHECK(snapshot.hist[7] == 0u);
    CHECK(histogramTotal(snapshot) == 4u);
}

TEST_CASE("zero-length calls never create a minimum", "[diag]")
{
    DiagCounters counters;
    counters.onBlock(0, 512, false);
    counters.onBlock(0, 512, true);

    const DiagCountersSnapshot snapshot = counters.snapshot();
    CHECK(snapshot.blocksTotal == 2u);
    CHECK(snapshot.zeroLen == 2u);
    CHECK(snapshot.bypassed == 1u);
    CHECK(snapshot.nMin == 0u);
    CHECK(snapshot.nMax == 0u);
    CHECK(histogramTotal(snapshot) == 0u);
}

TEST_CASE("histogram buckets split at the upper bound of each power of two", "[diag]")
{
    CHECK(DiagCounters::bucketOf(64) == 0u);
    CHECK(DiagCounters::bucketOf(65) == 1u);
    CHECK(DiagCounters::bucketOf(128) == 1u);
    CHECK(DiagCounters::bucketOf(129) == 2u);
    CHECK(DiagCounters::bucketOf(4096) == 6u);
    CHECK(DiagCounters::bucketOf(4097) == 7u);
    CHECK(DiagCounters::bucketOf(0xffffffffu) == 7u);
}

// 计数是单写者、多读者的：写线程在音频线程的位置上跑，读线程在消息线程的位置上跑。两个字段都要是原子的，
// 否则这里会读到撕裂的值；把成员改成普通整型在 TSan 下会报 race。
TEST_CASE("a reader can snapshot while the writer counts", "[diag]")
{
    DiagCounters counters;
    constexpr std::uint32_t kBlocks = 20000u;
    std::atomic<bool> readerReady{false};
    std::atomic<bool> writing{true};

    // 写线程等读线程先取到第一份快照再开始计数，两边一定有交叠
    std::thread writer([&] {
        while (!readerReady.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
        for (std::uint32_t i = 1u; i <= kBlocks; ++i)
        {
            // 块长在 1..4096 之间循环，每 4097 块里有一块是 0 样本；声明上限取一半，好让超限计数也动起来
            counters.onBlock(i % 4097u, 2048u, (i % 8u) == 0u);
            // 偶尔让出时间片，读线程才能在计数进行中取到快照
            if ((i % 64u) == 0u)
            {
                std::this_thread::yield();
            }
        }
        writing.store(false, std::memory_order_release);
    });

    // 每个字段只朝一个方向动：计数与直方图桶只增不减，终身最大块长只增，最小块长只减
    // （快照报 0 表示还没见到非零块，不参与比较）
    std::uint64_t lastTotal = 0u;
    std::uint64_t lastZeroLen = 0u;
    std::uint64_t lastOverMax = 0u;
    std::uint64_t lastBypassed = 0u;
    std::uint32_t lastNMax = 0u;
    std::uint32_t lastNMin = 0u;
    std::uint64_t lastHist[kNsamplesBuckets] = {};

    do
    {
        const DiagCountersSnapshot snapshot = counters.snapshot();
        // 相邻两次快照之间，每个字段都不能倒退
        CHECK(snapshot.blocksTotal >= lastTotal);
        CHECK(snapshot.zeroLen >= lastZeroLen);
        CHECK(snapshot.overMax >= lastOverMax);
        CHECK(snapshot.bypassed >= lastBypassed);
        CHECK(snapshot.nMax >= lastNMax);
        if (snapshot.nMin != 0u)
        {
            // 快照报 0 表示还没见到非零块；见到过之后，最小块长只会更小
            if (lastNMin != 0u)
            {
                CHECK(snapshot.nMin <= lastNMin);
            }
            lastNMin = snapshot.nMin;
        }
        for (std::size_t i = 0; i < kNsamplesBuckets; ++i)
        {
            CHECK(snapshot.hist[i] >= lastHist[i]);
        }
        lastTotal = snapshot.blocksTotal;
        lastZeroLen = snapshot.zeroLen;
        lastOverMax = snapshot.overMax;
        lastBypassed = snapshot.bypassed;
        lastNMax = snapshot.nMax;
        for (std::size_t i = 0; i < kNsamplesBuckets; ++i)
        {
            lastHist[i] = snapshot.hist[i];
        }
        readerReady.store(true, std::memory_order_release);
    } while (writing.load(std::memory_order_acquire));

    writer.join();

    const DiagCountersSnapshot final = counters.snapshot();
    CHECK(final.blocksTotal == kBlocks);
    CHECK(final.bypassed == kBlocks / 8u);
    CHECK(histogramTotal(final) + final.zeroLen == kBlocks);
    // 块长在 1..4096 循环：最小 1，最大 4096；声明上限 2048，每轮 4097 块里有 2048 块超限
    CHECK(final.nMin == 1u);
    CHECK(final.nMax == 4096u);
    CHECK(final.zeroLen == 4u);
    CHECK(final.overMax == 9756u);
}
