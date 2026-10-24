// SPDX-License-Identifier: GPL-3.0-or-later
//
// ProbeLog 的行为：文件头与收尾、环满丢最新并计数、记录窗口、C 行与首块的先后、快照、S 行由自己的线程写、
// 双线程下块数守恒、文件上限、tok4 与文件名。除并发用例外都用手动 pump 和假时钟，结果确定。
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ProbeTestKit.h"
#include "diag/ProbeLog.h"

namespace
{

using namespace rinne::diag;
using namespace rinne::test::probe;

ProbeCase standardCase()
{
    ProbeCase c;
    c.caseId = "CB-01";
    c.variant = "steady";
    c.buf = 256;
    c.guard = ProbeGuard::kLow;
    c.sampleRate = 48000;
    return c;
}

BlockFields plainBlock(std::uint64_t i)
{
    BlockFields f;
    f.callSeq = i;
    f.monoNs = 2000000000u + i;
    f.n = 256;
    f.nDecl = 256;
    f.d = 9600;
    return f;
}

std::uint64_t asNumber(const std::string& s)
{
    return std::stoull(s);
}

} // namespace

TEST_CASE("the log file is created with the magic line and the H row, LF only and ASCII", "[probe][log]")
{
    TempDir dir;
    FakeClock clock;
    ProbeLog log(manualOptions(dir, ProbeRole::kInput, clock, "a1b2"));
    REQUIRE(log.isOpen());
    CHECK(log.tok4() == "a1b2");
    const fs::path path = fs::u8path(log.filePath());
    CHECK(path.filename().string() == "probe-I-4100-a1b2.csv");
    // 构造完成，其他什么都没做之前，文件里就有头
    CHECK(readFile(path) == "rinne-probe-log,1\nH,I,vst3,cubase,win,x64,4100,900,a1b2,0123abcd\n");

    log.close();
    const std::string text = readFile(path);
    CHECK(structureProblem(text).empty());
    CHECK(text.find('\r') == std::string::npos);
}

TEST_CASE("the probe log ends with a Z row when the instance is destroyed", "[probe][log]")
{
    TempDir dir;
    FakeClock clock;
    fs::path path;
    {
        ProbeLog log(manualOptions(dir, ProbeRole::kOutput, clock, "c3d4"));
        path = fs::u8path(log.filePath());
        REQUIRE(log.beginCase(standardCase()));
        for (std::uint64_t i = 0; i < 5; ++i)
        {
            log.onBlock(plainBlock(i));
        }
        // 没有显式 close：析构负责收尾
    }
    const std::vector<std::string> lines = splitLines(readFile(path));
    REQUIRE_FALSE(lines.empty());
    const std::vector<std::string> z = splitFields(lines.back());
    REQUIRE(z.size() == 4);
    CHECK(z[0] == "Z");
    CHECK(z[2] == "5"); // blocks_total：析构时环里还没排空的 5 块也要写出来
    CHECK(z[3] == "0");
    CHECK(rowsOfKind(readFile(path), "B").size() == 5);
}

TEST_CASE("close is idempotent and a closed log ignores every call", "[probe][log]")
{
    TempDir dir;
    FakeClock clock;
    ProbeLog log(manualOptions(dir, ProbeRole::kInput, clock, "a1b2"));
    const fs::path path = fs::u8path(log.filePath());
    REQUIRE(log.beginCase(standardCase()));
    log.onBlock(plainBlock(0));
    log.close();
    log.close();
    const std::string closedText = readFile(path);
    CHECK(rowsOfKind(closedText, "Z").size() == 1);

    log.onBlock(plainBlock(1));
    log.onAudioEvent(ProbeEventCode::kFirstBlock, 0, 0, 1);
    log.logEvent(ProbeEventCode::kReset);
    CHECK_FALSE(log.beginCase(standardCase()));
    log.pump();
    CHECK(readFile(path) == closedText);
    CHECK(log.counters().blocks == 1);
}

TEST_CASE("the block ring drops the newest record and counts it when full", "[probe][log]")
{
    TempDir dir;
    FakeClock clock;
    ProbeLog log(manualOptions(dir, ProbeRole::kInput, clock, "a1b2", 8)); // 8 槽
    const fs::path path = fs::u8path(log.filePath());
    REQUIRE(log.beginCase(standardCase()));

    for (std::uint64_t i = 0; i < 20; ++i)
    {
        log.onBlock(plainBlock(i)); // 没人排空：前 8 条进环，后 12 条被丢
    }
    CHECK(log.counters().blocks == 8);
    CHECK(log.counters().dropped == 12);

    clock.value.fetch_add(1000000000); // 到点写 S
    log.pump();
    for (std::uint64_t i = 20; i < 23; ++i)
    {
        log.onBlock(plainBlock(i)); // 腾出了地方：序号接着丢掉的那 12 个之后走
    }
    log.close();

    const std::string text = readFile(path);
    std::vector<std::string> seqs;
    for (const auto& row : rowsOfKind(text, "B"))
    {
        seqs.push_back(row[1]);
    }
    // 留在文件里的是最早的 8 条（丢最新），然后是缺口，再是 20、21、22
    CHECK(seqs == std::vector<std::string>{"0", "1", "2", "3", "4", "5", "6", "7", "20", "21", "22"});
    // 被丢掉的是 8..19：缺口两侧的 call_seq 也对得上，没有哪条被改写
    CHECK(rowsOfKind(text, "B")[7][2] == "7");
    CHECK(rowsOfKind(text, "B")[8][2] == "20");

    const auto summaries = rowsOfKind(text, "S");
    REQUIRE(summaries.size() == 1);
    CHECK(summaries[0][2] == "8"); // blocks
    CHECK(summaries[0][3] == "12"); // dropped
    const auto z = rowsOfKind(text, "Z");
    REQUIRE(z.size() == 1);
    CHECK(z[0][2] == "11");
    CHECK(z[0][3] == "12");
}

TEST_CASE("blocks outside a recording window only feed the summary counters", "[probe][log]")
{
    TempDir dir;
    FakeClock clock;
    ProbeLog log(manualOptions(dir, ProbeRole::kInput, clock, "a1b2"));
    const fs::path path = fs::u8path(log.filePath());

    BlockFields f = plainBlock(0);
    f.n = 999;
    f.rs = 3;
    log.onBlock(f); // 窗口外
    CHECK_FALSE(log.isRecording());
    CHECK(log.counters().blocks == 0);
    CHECK(log.counters().maxN == 999);
    CHECK(log.counters().resync == 3);

    REQUIRE(log.beginCase(standardCase()));
    CHECK(log.isRecording());
    for (std::uint64_t i = 0; i < 5; ++i)
    {
        log.onBlock(plainBlock(i));
    }
    log.endCase();
    CHECK_FALSE(log.isRecording());
    for (std::uint64_t i = 0; i < 10; ++i)
    {
        log.onBlock(plainBlock(i)); // 又是窗口外：不写，也不占序号
    }
    REQUIRE(log.beginCase(standardCase()));
    for (std::uint64_t i = 0; i < 3; ++i)
    {
        log.onBlock(plainBlock(i));
    }
    log.close();

    std::vector<std::string> seqs;
    for (const auto& row : rowsOfKind(readFile(path), "B"))
    {
        seqs.push_back(row[1]);
    }
    // 序号只为写出的块递增，窗口之间不归零，也不为没有记录的回调递增
    CHECK(seqs == std::vector<std::string>{"0", "1", "2", "3", "4", "5", "6", "7"});
    CHECK(rowsOfKind(readFile(path), "C").size() == 2);
}

TEST_CASE("a case with values the analyzer would reject is refused and starts no window", "[probe][log]")
{
    TempDir dir;
    FakeClock clock;
    ProbeLog log(manualOptions(dir, ProbeRole::kInput, clock, "a1b2"));
    const fs::path path = fs::u8path(log.filePath());
    ProbeCase bad = standardCase();
    bad.variant = "deactivate";
    CHECK_FALSE(log.beginCase(bad));
    CHECK_FALSE(log.isRecording());
    log.onBlock(plainBlock(0));
    log.close();
    const std::string text = readFile(path);
    CHECK(rowsOfKind(text, "C").empty());
    CHECK(rowsOfKind(text, "B").empty());
}

namespace
{

// 假时钟的钩子：每读一次时钟，就让「音频线程」在读之前先跑一块，块的时间戳取读出值之前的一刻
void injectBlockBeforeRead(FakeClock& clock)
{
    auto* log = static_cast<ProbeLog*>(clock.hookContext);
    BlockFields f;
    f.callSeq = clock.value.load();
    f.monoNs = clock.value.load() - 1;
    f.n = 64;
    f.nDecl = 64;
    log->onBlock(f);
}

} // namespace

TEST_CASE("the C row is written before the first block of its window", "[probe][log]")
{
    TempDir dir;
    FakeClock clock;
    ProbeLog log(manualOptions(dir, ProbeRole::kInput, clock, "a1b2"));
    const fs::path path = fs::u8path(log.filePath());

    // beginCase 里每读一次时钟，音频线程就抢先跑一块。先取时间戳、最后才打开块记录的话，这些抢跑的块都不进窗口；
    // 要是先打开记录再取时间戳，就会有一块的 mono_ns 早于 C 行的 t_ns，成了孤块
    clock.hook = &injectBlockBeforeRead;
    clock.hookContext = &log;
    REQUIRE(log.beginCase(standardCase()));
    clock.hook = nullptr;
    for (std::uint64_t i = 0; i < 4; ++i)
    {
        BlockFields f;
        f.callSeq = 1000 + i;
        f.monoNs = clock.value.load() + i;
        f.n = 64;
        f.nDecl = 64;
        log.onBlock(f);
    }
    log.close();

    const std::string text = readFile(path);
    const auto cRows = rowsOfKind(text, "C");
    REQUIRE(cRows.size() == 1);
    const std::uint64_t cTime = asNumber(cRows[0][1]);
    const auto bRows = rowsOfKind(text, "B");
    REQUIRE(bRows.size() == 4);
    for (const auto& row : bRows)
    {
        CHECK(asNumber(row[11]) >= cTime); // mono_ns 不早于 C.t_ns
    }
}

TEST_CASE("every C row is followed by a state snapshot including an empty track_props", "[probe][log]")
{
    TempDir dir;
    FakeClock clock;
    ProbeLog log(manualOptions(dir, ProbeRole::kInput, clock, "a1b2"));
    const fs::path path = fs::u8path(log.filePath());

    log.logEvent(ProbeEventCode::kPrepare, ProbeEventArgs{48000, 256, {}});
    log.logEvent(ProbeEventCode::kBackend, ProbeEventArgs{0, 0, {}});
    log.logEvent(ProbeEventCode::kPaired, ProbeEventArgs{4100, 2, {}});
    log.logEvent(ProbeEventCode::kSetState, ProbeEventArgs{2, 0, {}});
    log.logEvent(ProbeEventCode::kReset); // 不在快照里的事件不会被补写
    REQUIRE(log.beginCase(standardCase()));
    log.endCase();
    log.logEvent(ProbeEventCode::kShmDenied);
    log.logEvent(ProbeEventCode::kTrackProps, ProbeEventArgs{0, probeColourArg(0x112233), "Lead Vox"});
    REQUIRE(log.beginCase(standardCase()));
    log.close();

    const std::vector<std::string> lines = splitLines(readFile(path));
    std::vector<std::size_t> cIndex;
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        if (lines[i].rfind("C,", 0) == 0)
        {
            cIndex.push_back(i);
        }
    }
    REQUIRE(cIndex.size() == 2);

    const auto eventName = [&](std::size_t i) { return splitFields(lines[i])[2]; };
    // 第一个 C：没发生过的 shm_denied 不写；从未送达的 track_props 写 a、b 都为空的一条
    CHECK(eventName(cIndex[0] + 1) == "prepare");
    CHECK(lines[cIndex[0] + 1].substr(lines[cIndex[0] + 1].find("prepare")) == "prepare,48000,256");
    CHECK(eventName(cIndex[0] + 2) == "backend");
    CHECK(eventName(cIndex[0] + 3) == "paired");
    CHECK(lines[cIndex[0] + 3].substr(lines[cIndex[0] + 3].find("paired")) == "paired,4100,C");
    CHECK(eventName(cIndex[0] + 4) == "set_state");
    CHECK(eventName(cIndex[0] + 5) == "track_props");
    CHECK(lines[cIndex[0] + 5].substr(lines[cIndex[0] + 5].find("track_props")) == "track_props,,");
    // 第二个 C：多了发生过的 shm_denied 和收到的轨道名，顺序是 prepare backend paired shm_denied set_state track_props
    const std::vector<std::string> expected = {"prepare",    "backend",   "paired",
                                               "shm_denied", "set_state", "track_props"};
    for (std::size_t k = 0; k < expected.size(); ++k)
    {
        INFO("snapshot " << k);
        CHECK(eventName(cIndex[1] + 1 + k) == expected[k]);
    }
    CHECK(lines[cIndex[1] + 6].substr(lines[cIndex[1] + 6].find("track_props")) == "track_props,Lead%20Vox,112233");
    // 快照的 t_ns 是补写的时间，不早于 C 行；原事件仍按发生时间排在前面，没有被快照盖掉
    CHECK(asNumber(splitFields(lines[cIndex[0] + 1])[1]) >= asNumber(splitFields(lines[cIndex[0]])[1]));
    CHECK(asNumber(splitFields(lines[1 + 1])[1]) < asNumber(splitFields(lines[cIndex[0]])[1]));
}

TEST_CASE("events from the audio thread keep the timestamp the caller read", "[probe][log]")
{
    TempDir dir;
    FakeClock clock;
    ProbeLogOptions options = manualOptions(dir, ProbeRole::kInput, clock, "a1b2");
    options.audioEventSlots = 4;
    ProbeLog log(std::move(options));
    const fs::path path = fs::u8path(log.filePath());

    log.onAudioEvent(ProbeEventCode::kFirstBlock, 0, 0, 12345);
    log.onAudioEvent(ProbeEventCode::kProcessMode, 1, 0, 12346);
    for (int i = 0; i < 8; ++i)
    {
        log.onAudioEvent(ProbeEventCode::kFirstPairedBlock, 0, 0, 20000 + i);
    }
    // 槽只有 4 个：前 4 条进，后面 6 条被丢并计数
    CHECK(log.counters().dropped == 6);
    log.close();

    const std::string text = readFile(path);
    CHECK(text.find("E,12345,first_block,,\n") != std::string::npos);
    CHECK(text.find("E,12346,process_mode,offline,\n") != std::string::npos);
    CHECK(rowsOfKind(text, "Z")[0][3] == "6");
}

TEST_CASE("the S row is written by the log's own thread without any message thread", "[probe][log]")
{
    TempDir dir;
    ProbeLogOptions options;
    options.directory = dir.utf8();
    options.header = testHeader(ProbeRole::kInput, "a1b2");
    options.blockRingSlots = 64;
    options.summaryIntervalNs = 10000000; // 10 ms，缩短等待
    options.startThread = true;
    ProbeLog log(std::move(options));
    const fs::path path = fs::u8path(log.filePath());
    REQUIRE(log.isOpen());

    // 这段时间里调用方不碰日志：不调 pump，也不记事件。摘要只能由日志自己的线程写
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    log.close();
    const std::string text = readFile(path);
    const auto summaries = rowsOfKind(text, "S");
    CHECK(summaries.size() >= 5);
    CHECK(structureProblem(text).empty());
    // 摘要的时间戳单调递增
    for (std::size_t i = 1; i < summaries.size(); ++i)
    {
        CHECK(asNumber(summaries[i][1]) > asNumber(summaries[i - 1][1]));
    }
}

TEST_CASE("the S row is due once per interval of the injected clock", "[probe][log]")
{
    TempDir dir;
    FakeClock clock;
    ProbeLog log(manualOptions(dir, ProbeRole::kInput, clock, "a1b2"));
    const fs::path path = fs::u8path(log.filePath());
    log.pump();
    clock.value.fetch_add(500000000);
    log.pump(); // 半秒：还不到
    CHECK(rowsOfKind(readFile(path), "S").empty());
    clock.value.fetch_add(600000000);
    log.pump(); // 过了一秒
    CHECK(rowsOfKind(readFile(path), "S").size() == 1);
    log.pump(); // 紧接着再来一圈：不重复
    CHECK(rowsOfKind(readFile(path), "S").size() == 1);
    clock.value.fetch_add(1000000000);
    log.pump();
    CHECK(rowsOfKind(readFile(path), "S").size() == 2);
    log.close();
}

TEST_CASE("record counts are conserved when the audio thread outruns the log thread", "[probe][log]")
{
    TempDir dir;
    ProbeLogOptions options;
    options.directory = dir.utf8();
    options.header = testHeader(ProbeRole::kInput, "a1b2");
    options.blockRingSlots = 4096; // 小环 + 全速生产：一定会丢
    options.startThread = true;
    ProbeLog log(std::move(options));
    const fs::path path = fs::u8path(log.filePath());
    REQUIRE(log.beginCase(standardCase()));

    constexpr std::uint64_t kTotal = 150000;
    const std::uint64_t base = probeMonoNs(); // 用的是真时钟：块的开始时刻必须不早于 beginCase 取的 C.t_ns
    std::thread audio([&] {
        for (std::uint64_t i = 0; i < kTotal; ++i)
        {
            BlockFields f;
            f.callSeq = i + 1;
            f.monoNs = base + i;
            f.n = 128;
            f.nDecl = 128;
            f.w = i * 128;
            log.onBlock(f);
        }
    });
    audio.join();
    log.endCase();
    log.close();

    const std::string text = readFile(path);
    REQUIRE(structureProblem(text).empty());
    const auto bRows = rowsOfKind(text, "B");
    const auto z = rowsOfKind(text, "Z");
    REQUIRE(z.size() == 1);
    const std::uint64_t blocksTotal = asNumber(z[0][2]);
    const std::uint64_t droppedTotal = asNumber(z[0][3]);
    // 守恒：写出的 + 丢弃的 = 试图记录的；Z 的块数就是文件里的 B 行数
    CHECK(bRows.size() == blocksTotal);
    CHECK(blocksTotal + droppedTotal == kTotal);
    // seq 严格递增；每条丢弃都在序号上留了缺口，所以最后一条的序号 + 1 - 行数 =
    // 缺口总数（尾部丢的不一定可见，只会更少）
    std::uint64_t previous = 0;
    bool first = true;
    for (const auto& row : bRows)
    {
        const std::uint64_t seq = asNumber(row[1]);
        if (!first)
        {
            REQUIRE(seq > previous);
        }
        previous = seq;
        first = false;
    }
    if (!bRows.empty())
    {
        CHECK(previous + 1 - bRows.size() <= droppedTotal);
    }
}

TEST_CASE("a file over its size limit stops writing B rows and counts them as dropped", "[probe][log]")
{
    TempDir dir;
    FakeClock clock;
    ProbeLogOptions options = manualOptions(dir, ProbeRole::kInput, clock, "a1b2", 256);
    options.maxFileBytes = 2000;
    ProbeLog log(std::move(options));
    const fs::path path = fs::u8path(log.filePath());
    REQUIRE(log.beginCase(standardCase()));
    for (std::uint64_t i = 0; i < 100; ++i)
    {
        log.onBlock(plainBlock(i));
    }
    log.pump();
    log.close();

    const std::string text = readFile(path);
    const auto bRows = rowsOfKind(text, "B");
    const auto z = rowsOfKind(text, "Z");
    REQUIRE(z.size() == 1); // 收尾的 Z 行照写
    CHECK(bRows.size() < 100);
    CHECK(bRows.size() > 5);
    CHECK(asNumber(z[0][2]) == bRows.size());
    CHECK(asNumber(z[0][2]) + asNumber(z[0][3]) == 100);
    CHECK(text.size() < 2500);
    CHECK(structureProblem(text).empty());
}

TEST_CASE("two instances of one process get different tok4 values and different files", "[probe][log]")
{
    TempDir dir;
    FakeClock clock;
    ProbeLogOptions first = manualOptions(dir, ProbeRole::kInput, clock, "");
    ProbeLogOptions second = manualOptions(dir, ProbeRole::kInput, clock, "");
    ProbeLog a(std::move(first));
    ProbeLog b(std::move(second));
    REQUIRE(a.isOpen());
    REQUIRE(b.isOpen());
    CHECK(a.tok4().size() == 4);
    CHECK(b.tok4().size() == 4);
    CHECK(a.tok4() != b.tok4());
    CHECK(a.filePath() != b.filePath());
    CHECK(fs::u8path(a.filePath()).filename().string() == "probe-I-4100-" + a.tok4() + ".csv");
}

namespace
{

// 按顺序吐出预设的 tok4 候选
struct FixedTok4s
{
    explicit FixedTok4s(std::vector<std::string> list) : names(std::move(list)) {}
    std::vector<std::string> names;
    std::size_t next = 0;
    static std::string take(void* context)
    {
        auto* self = static_cast<FixedTok4s*>(context);
        return self->names[self->next++ % self->names.size()];
    }
};

void writeMarker(const fs::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

} // namespace

TEST_CASE("a free log name skips files that already exist and never touches them", "[probe][log]")
{
    TempDir dir;
    writeMarker(dir.path() / "probe-I-4100-aaaa.csv", "someone else's log");
    writeMarker(dir.path() / "probe-I-4100-bbbb.csv", "another one");

    FixedTok4s source({"aaaa", "bbbb", "cccc"});
    std::string tok4;
    std::string pathUtf8;
    REQUIRE(findFreeProbeLogPath(dir.utf8(), ProbeRole::kInput, 4100, 256, &FixedTok4s::take, &source, tok4, pathUtf8));
    CHECK(tok4 == "cccc");
    CHECK(fs::u8path(pathUtf8).filename().string() == "probe-I-4100-cccc.csv");
    CHECK(source.next == 3);
    CHECK(readFile(dir.path() / "probe-I-4100-aaaa.csv") == "someone else's log");
    CHECK(readFile(dir.path() / "probe-I-4100-bbbb.csv") == "another one");
}

TEST_CASE("no free log name is found when every candidate is taken, and nothing is overwritten", "[probe][log]")
{
    TempDir dir;
    writeMarker(dir.path() / "probe-O-4100-aaaa.csv", "first");
    writeMarker(dir.path() / "probe-O-4100-bbbb.csv", "second");

    FixedTok4s source({"aaaa", "bbbb"});
    std::string tok4 = "keep";
    std::string pathUtf8 = "keep";
    CHECK_FALSE(
        findFreeProbeLogPath(dir.utf8(), ProbeRole::kOutput, 4100, 256, &FixedTok4s::take, &source, tok4, pathUtf8));
    CHECK(source.next == 256); // 试满了才放弃
    CHECK(tok4 == "keep"); // 失败时出参不动，调用方不会拿着一个已存在的路径去打开
    CHECK(pathUtf8 == "keep");
    CHECK(readFile(dir.path() / "probe-O-4100-aaaa.csv") == "first");
    CHECK(readFile(dir.path() / "probe-O-4100-bbbb.csv") == "second");

    // 尝试次数是上限，不是下限
    FixedTok4s again({"aaaa", "bbbb"});
    CHECK_FALSE(
        findFreeProbeLogPath(dir.utf8(), ProbeRole::kOutput, 4100, 3, &FixedTok4s::take, &again, tok4, pathUtf8));
    CHECK(again.next == 3);
    CHECK_FALSE(
        findFreeProbeLogPath(dir.utf8(), ProbeRole::kOutput, 4100, 0, &FixedTok4s::take, &again, tok4, pathUtf8));
    CHECK_FALSE(findFreeProbeLogPath(dir.utf8(), ProbeRole::kOutput, 4100, 5, nullptr, nullptr, tok4, pathUtf8));
}

TEST_CASE("ticks convert to nanoseconds without dividing by a zero frequency", "[probe][log]")
{
    CHECK(probeTicksToNs(12345, 0) == 0); // 静态初始化阶段频率还没取到
    CHECK(probeTicksToNs(0, 10000000) == 0);
    CHECK(probeTicksToNs(10000000, 10000000) == 1000000000u); // 10 MHz 的计数器，一秒
    CHECK(probeTicksToNs(5, 10000000) == 500u);
    CHECK(probeTicksToNs(48000, 48000) == 1000000000u);
    CHECK(probeTicksToNs(1, 3) == 333333333u); // 余数那一段不丢
    // 开机几年的刻度数也不溢出（先除后乘）
    const std::uint64_t years = std::uint64_t{10000000} * 3600 * 24 * 365 * 5;
    CHECK(probeTicksToNs(years, 10000000) == std::uint64_t{3600} * 24 * 365 * 5 * 1000000000u);
}

TEST_CASE("a missing directory is created and an empty one leaves the log closed but safe", "[probe][log]")
{
    TempDir dir;
    FakeClock clock;
    {
        ProbeLogOptions options = manualOptions(dir, ProbeRole::kInput, clock, "a1b2");
        options.directory = (dir.path() / "nested" / "logs").u8string();
        ProbeLog log(std::move(options));
        CHECK(log.isOpen());
        const fs::path path = fs::u8path(log.filePath());
        log.close();
        CHECK(fs::exists(path));
        std::error_code ec; // 只删文件和空目录，不做递归删除
        fs::remove(path, ec);
        fs::remove(dir.path() / "nested" / "logs", ec);
        fs::remove(dir.path() / "nested", ec);
    }
    {
        ProbeLogOptions options = manualOptions(dir, ProbeRole::kInput, clock, "a1b2");
        options.directory.clear(); // core 不读环境变量：没给目录就是没有日志
        ProbeLog log(std::move(options));
        CHECK_FALSE(log.isOpen());
        CHECK_FALSE(log.beginCase(standardCase()));
        log.onBlock(plainBlock(0));
        log.logEvent(ProbeEventCode::kReset);
        log.pump();
        log.close();
        CHECK(log.counters().blocks == 0);
    }
}

TEST_CASE("the header helper fills in the process and platform", "[probe][log]")
{
    const ProbeHeader header = makeProbeHeader(ProbeRole::kOutput, ProbeFormat::kAu, ProbeHost::kLuna, "89abcdef");
    CHECK(header.role == ProbeRole::kOutput);
    CHECK(header.format == ProbeFormat::kAu);
    CHECK(header.host == ProbeHost::kLuna);
    CHECK(header.build == "89abcdef");
    CHECK(header.pid == rinne::ipc::currentProcessId());
    CHECK(header.pid != 0);
#if defined(_WIN32)
    CHECK(header.os == ProbeOs::kWin);
#else
    CHECK(header.os == ProbeOs::kMac);
#endif
}

namespace
{

// 静态初始化阶段读时钟。Windows 上计数器频率是 rinne_core 里的全局常量，别的翻译单元的全局对象先构造时它还是 0：
// 读数仍要有意义，不能除以零，也不能返回 0
const std::uint64_t gClockReadDuringStaticInit = probeMonoNs();

} // namespace

TEST_CASE("the monotonic clock can be read during static initialization", "[probe][log]")
{
    CHECK(gClockReadDuringStaticInit != 0);
    CHECK(gClockReadDuringStaticInit <= probeMonoNs());
}

TEST_CASE("the monotonic clock never goes backwards", "[probe][log]")
{
    std::uint64_t previous = probeMonoNs();
    CHECK(previous != 0);
    for (int i = 0; i < 100000; ++i)
    {
        const std::uint64_t current = probeMonoNs();
        REQUIRE(current >= previous);
        previous = current;
    }
    // 纳秒读数：几百微秒内必须有变化（毫秒时钟做不到）
    const std::uint64_t start = probeMonoNs();
    std::uint64_t distinct = 0;
    std::uint64_t last = start;
    while (probeMonoNs() - start < 2000000u)
    {
        const std::uint64_t t = probeMonoNs();
        distinct += t != last ? 1 : 0;
        last = t;
    }
    CHECK(distinct > 100);
}

TEST_CASE("the standard scenario writes the checked-in fixture files byte for byte", "[probe][log][fixture]")
{
    TempDir dir;
    struct Expected
    {
        ProbeRole role;
        const char* tok4;
        const char* name;
    };
    for (const Expected& e : {Expected{ProbeRole::kInput, "a1b2", "probe-I-4100-a1b2.csv"},
                              Expected{ProbeRole::kOutput, "c3d4", "probe-O-4100-c3d4.csv"}})
    {
        INFO(e.name);
        const fs::path written = writeStandardLog(dir, e.role, e.tok4);
        REQUIRE(written.filename().string() == e.name);
        const std::string fresh = readFile(written);
        const std::string checkedIn = readFile(fixtureDir() / e.name);
        REQUIRE_FALSE(checkedIn.empty());
        CHECK(fresh == checkedIn);
        CHECK(structureProblem(fresh).empty());
        CHECK(rowsOfKind(fresh, "B").size() == kStandardBlocks);
        CHECK(rowsOfKind(fresh, "C").size() == 1);
        CHECK(rowsOfKind(fresh, "Z").size() == 1);
    }
}

// 重新生成夹具：改了日志格式并确认分析器仍然接受之后，手动跑 rinne_core_tests "[regen]"。默认不跑（带 . 标签）
TEST_CASE("regenerate the probe fixture files", "[.][regen]")
{
    std::error_code ec;
    fs::create_directories(fixtureDir(), ec);
    TempDir dir;
    for (const auto& [role, tok4] : {std::pair{ProbeRole::kInput, "a1b2"}, std::pair{ProbeRole::kOutput, "c3d4"}})
    {
        const fs::path written = writeStandardLog(dir, role, tok4);
        fs::copy_file(written, fixtureDir() / written.filename(), fs::copy_options::overwrite_existing, ec);
        REQUIRE_FALSE(ec);
    }
}
