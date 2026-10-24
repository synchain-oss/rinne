// SPDX-License-Identifier: GPL-3.0-or-later
//
// 记录窗口的边界：beginCase 落在某一块的中途时那一块不进新窗口（它的 mono_ns 早于 C.t_ns）；
// 排队的行放不下 C 行加快照时 beginCase 整体拒绝，不开窗口。都用手动 pump 和假时钟，结果确定。
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "ProbeTestKit.h"
#include "diag/ProbeLog.h"

namespace
{

using namespace rinne::diag;
using namespace rinne::test::probe;

ProbeCase windowCase()
{
    ProbeCase c;
    c.caseId = "CB-01";
    c.variant = "steady";
    c.buf = 256;
    c.guard = ProbeGuard::kLow;
    c.sampleRate = 48000;
    return c;
}

BlockFields blockStartingAt(std::uint64_t monoNs, std::uint64_t callSeq)
{
    BlockFields f;
    f.callSeq = callSeq;
    f.monoNs = monoNs;
    f.n = 64;
    f.nDecl = 64;
    return f;
}

// 最后一个 C 行的 t_ns（文件里已经写出的部分）
std::uint64_t lastCaseTime(const fs::path& path)
{
    const auto rows = rowsOfKind(readFile(path), "C");
    REQUIRE_FALSE(rows.empty());
    return std::stoull(rows.back()[1]);
}

} // namespace

TEST_CASE("a block that began before beginCase but ends after it stays outside the window", "[probe][window]")
{
    TempDir dir;
    FakeClock clock;
    ProbeLog log(manualOptions(dir, ProbeRole::kInput, clock, "a1b2"));
    const fs::path path = fs::u8path(log.filePath());

    clock.value.store(5000000000u);
    REQUIRE(log.beginCase(windowCase()));
    log.pump(); // C 行写出，读出它的 t_ns
    const std::uint64_t cTime = lastCaseTime(path);

    // 块开始在 beginCase 之前、onBlock 在 beginCase 之后：音频线程在块末才调 onBlock，beginCase 正好落在块中途
    log.onBlock(blockStartingAt(cTime - 1, 1));
    CHECK(log.counters().blocks == 0);
    CHECK(log.counters().dropped == 0); // 窗口外：既不记录也不算丢弃
    log.onBlock(blockStartingAt(cTime, 2)); // 恰好与 C 行同一纳秒开始：记
    log.onBlock(blockStartingAt(cTime + 5333333u, 3));
    CHECK(log.counters().blocks == 2);
    CHECK(log.counters().dropped == 0);
    log.close();

    const auto bRows = rowsOfKind(readFile(path), "B");
    REQUIRE(bRows.size() == 2);
    CHECK(bRows[0][1] == "0"); // 被排除的那块没有占序号，文件里没有缺口
    CHECK(bRows[1][1] == "1");
    CHECK(bRows[0][2] == "2"); // call_seq：第一条写出的是第二块
    for (const auto& row : bRows)
    {
        CHECK(std::stoull(row[11]) >= cTime); // mono_ns 不早于 C.t_ns
    }
}

TEST_CASE("a second beginCase without endCase moves the window start forward", "[probe][window]")
{
    TempDir dir;
    FakeClock clock;
    ProbeLog log(manualOptions(dir, ProbeRole::kInput, clock, "a1b2"));
    const fs::path path = fs::u8path(log.filePath());

    clock.value.store(5000000000u);
    REQUIRE(log.beginCase(windowCase()));
    log.pump();
    const std::uint64_t firstTime = lastCaseTime(path);
    log.onBlock(blockStartingAt(firstTime + 10, 1));

    clock.value.store(6000000000u);
    REQUIRE(log.beginCase(windowCase())); // 上一格还开着，直接开下一格
    log.pump();
    const std::uint64_t secondTime = lastCaseTime(path);
    REQUIRE(secondTime > firstTime);

    log.onBlock(blockStartingAt(secondTime - 1, 2)); // 开始在第二个 C 行之前：不进第二格的窗口
    log.onBlock(blockStartingAt(secondTime + 10, 3));
    log.close();

    const auto bRows = rowsOfKind(readFile(path), "B");
    REQUIRE(bRows.size() == 2);
    CHECK(bRows[0][2] == "1");
    CHECK(bRows[1][2] == "3");
    CHECK(std::stoull(bRows[1][11]) >= secondTime);
}

TEST_CASE("beginCase refuses as a whole when the queued rows cannot hold the C row and its snapshot", "[probe][window]")
{
    TempDir dir;
    FakeClock clock;
    ProbeLogOptions options = manualOptions(dir, ProbeRole::kInput, clock, "a1b2");
    options.pendingLimit = 6;
    ProbeLog log(std::move(options));
    const fs::path path = fs::u8path(log.filePath());

    log.logEvent(ProbeEventCode::kPrepare, ProbeEventArgs{48000, 256, {}});
    log.logEvent(ProbeEventCode::kBackend, ProbeEventArgs{0, 0, {}});
    log.logEvent(ProbeEventCode::kReset);
    log.logEvent(ProbeEventCode::kReset); // 排队 4 行
    // 需要 4 行：C、prepare、backend 的快照、空的 track_props。4 + 4 > 6
    CHECK_FALSE(log.beginCase(windowCase()));
    CHECK_FALSE(log.isRecording());
    CHECK(log.counters().dropped == 0); // 没开始就没有丢任何记录
    log.onBlock(blockStartingAt(clock.value.load() + 1000, 1));
    CHECK(log.counters().blocks == 0); // 没有 C 行就不记块：它们会被分析器归到上一格

    log.pump(); // 腾空队列
    CHECK(log.beginCase(windowCase()));
    CHECK(log.isRecording());
    log.pump();
    const std::string text = readFile(path);
    CHECK(rowsOfKind(text, "C").size() == 1); // 被拒绝的那次没有留下 C 行
    CHECK(structureProblem(text).empty());
    log.close();
}

namespace
{

// 队列上限 limit；withStoredEvents 为真时先发生过 prepare 与 backend（写出后队列清空，快照仍记着）
bool beginFits(std::size_t limit, bool withStoredEvents)
{
    TempDir dir;
    FakeClock clock;
    ProbeLogOptions options = manualOptions(dir, ProbeRole::kInput, clock, "a1b2");
    options.pendingLimit = limit;
    ProbeLog log(std::move(options));
    if (withStoredEvents)
    {
        log.logEvent(ProbeEventCode::kPrepare, ProbeEventArgs{48000, 256, {}});
        log.logEvent(ProbeEventCode::kBackend, ProbeEventArgs{0, 0, {}});
    }
    log.pump();
    const bool began = log.beginCase(windowCase());
    CHECK(began == log.isRecording());
    log.close();
    return began;
}

} // namespace

TEST_CASE("beginCase fits exactly when the queue has room for the C row and every snapshot row", "[probe][window]")
{
    // 没发生过的事件不补写，快照只有从未送达的 track_props：C 行加快照共 2 行
    CHECK(beginFits(2, false));
    CHECK_FALSE(beginFits(1, false));
    // C、prepare、backend、track_props 共 4 行
    CHECK(beginFits(4, true));
    CHECK_FALSE(beginFits(3, true));
}
