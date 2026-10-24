// SPDX-License-Identifier: GPL-3.0-or-later
//
// S / Z 行的计数：max_n 与 B 行同一个上界；落盘线程丢弃已入环的块时，记录块数不会下溢。
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>

#include "ProbeTestKit.h"
#include "diag/ProbeLog.h"

namespace
{

using namespace rinne::diag;
using namespace rinne::test::probe;

ProbeCase counterCase()
{
    ProbeCase c;
    c.caseId = "CB-01";
    c.variant = "steady";
    c.buf = 256;
    c.guard = ProbeGuard::kLow;
    c.sampleRate = 48000;
    return c;
}

} // namespace

TEST_CASE("max_n is clamped to the contract bound like the n column of a B row", "[probe][counters]")
{
    TempDir dir;
    FakeClock clock;
    ProbeLog log(manualOptions(dir, ProbeRole::kInput, clock, "a1b2"));
    const fs::path path = fs::u8path(log.filePath());

    BlockFields wild;
    wild.monoNs = 2000000000u;
    wild.n = 0xFFFFFFFFu; // 宿主报了个离谱的块长（未初始化之类）
    wild.nDecl = 0xFFFFFFFFu;
    log.onBlock(wild); // 窗口外
    CHECK(log.counters().maxN == kProbeMaxBlock);

    REQUIRE(log.beginCase(counterCase()));
    wild.monoNs = 3000000000u;
    log.onBlock(wild); // 窗口内
    BlockFields tiny;
    tiny.monoNs = 3000000100u;
    tiny.n = 64;
    tiny.nDecl = 64;
    log.onBlock(tiny);
    CHECK(log.counters().maxN == kProbeMaxBlock); // 之后的小块不会把它拉回去

    clock.value.fetch_add(1000000000);
    log.pump();
    log.close();

    const std::string text = readFile(path);
    const auto summaries = rowsOfKind(text, "S");
    REQUIRE(summaries.size() == 1);
    CHECK(summaries[0][4] == std::to_string(kProbeMaxBlock)); // S 行的 max_n 在契约范围内，分析器读得进
    const auto bRows = rowsOfKind(text, "B");
    REQUIRE(bRows.size() == 2);
    CHECK(bRows[0][4] == std::to_string(kProbeMaxBlock)); // 与 B 行的 n 一致
    CHECK(structureProblem(text).empty());
}

TEST_CASE("the recorded block count never wraps while the log thread drops what the audio thread pushes",
          "[probe][counters]")
{
    TempDir dir;
    ProbeLogOptions options;
    options.directory = dir.utf8();
    options.header = testHeader(ProbeRole::kInput, "a1b2");
    options.blockRingSlots = 64;
    options.maxFileBytes = 1; // 文件头已经超了上限：落盘一侧弹出的每一块都被丢弃（块数挪进丢弃数）
    options.startThread = false;
    ProbeLog log(std::move(options));
    REQUIRE(log.beginCase(counterCase()));

    constexpr std::uint64_t kTotal = 600000;
    const std::uint64_t base = probeMonoNs();
    std::atomic<bool> producing{true};
    std::thread audio([&] {
        for (std::uint64_t i = 0; i < kTotal; ++i)
        {
            BlockFields f;
            f.callSeq = i + 1;
            f.monoNs = base + i;
            f.n = 64;
            f.nDecl = 64;
            log.onBlock(f);
        }
        producing.store(false);
    });
    // 落盘一侧一圈接一圈地排空：弹出一块之后立刻处理它，正好撞在音频线程「推进环」与「记数」之间的缝上；
    // 排空之后在本线程里马上读一次计数，最容易撞上缝。记录块数只可能在 0 与已试图记录的块数之间，下溢会读到接近 2^64
    // 的值
    std::atomic<bool> wrapped{false};
    std::thread drain([&] {
        while (producing.load())
        {
            log.pump();
            if (log.counters().blocks > kTotal)
            {
                wrapped.store(true);
            }
        }
    });
    while (producing.load())
    {
        if (log.counters().blocks > kTotal)
        {
            wrapped.store(true);
        }
    }
    audio.join();
    drain.join();
    log.pump();
    CHECK_FALSE(wrapped.load());
    CHECK(log.counters().blocks <= kTotal);
    // 守恒：文件里一行 B 都没写（上限是 1 字节），所以每一块都在丢弃数里
    CHECK(log.counters().blocks == 0);
    CHECK(log.counters().dropped == kTotal);
    log.close();
}
