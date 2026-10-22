// SPDX-License-Identifier: GPL-3.0-or-later
//
// 音频线程的诊断计数：总数、零样本、超限、旁路各自独立；直方图只收非零块，合计等于非零块数；
// 没有非零块时 nMin 保持 0（哨兵值不泄漏到快照里）。桶边界按 <=64、<=128……分段，64 与 65 必须落在
// 不同的桶，4096 与 4097 也是。
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>

#include "diag/DiagCounters.h"

namespace
{

using namespace rinne::diag;

// 桶计数与总块数同宽：高采样率小块长时 32 位的桶约 16 天就回绕，总数却不回绕
static_assert(sizeof(DiagCountersSnapshot::hist[0]) == sizeof(DiagCountersSnapshot::blocksTotal), "直方图桶要和总块数同宽");

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
    CHECK(empty.maxNSeen == 0u);
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
    CHECK(snapshot.maxNSeen == 1024u);
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
    CHECK(snapshot.maxNSeen == 0u);
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
