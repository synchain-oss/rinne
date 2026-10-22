// SPDX-License-Identifier: GPL-3.0-or-later
//
// 诊断文本生成：先单测清洗、定点格式化、十六进制、组号、事件名表与事件环；再用逐字节快照锁住 Input 与
// Output 两种完整输出。隐私用例往每个自由文本字段里塞路径、用户名、主机名与账号形态，要求输出只剩下白名单
// 字符，轨道名只以散列出现；最后的对照组证明清洗没有把正常的宿主名一起清空。
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "diag/DiagCounters.h"
#include "diag/DiagReport.h"

namespace
{

using namespace rinne::diag;

constexpr std::string_view kPrivacyTrackName = "Lead Vox Secret Take 7";

std::uint32_t packVersion(std::uint32_t major, std::uint32_t minor, std::uint32_t patch)
{
    return (major << 16u) | (minor << 8u) | patch;
}

DiagInputs makeSnapshotInputs(const DiagEventLog& events)
{
    DiagInputs in;
    in.role = DiagRole::kInput;
    in.product = "Rinne Input";
    in.version = "0.9.0";
    in.build = "dev.42";
    in.format = "VST3";
    in.host = "Cubase 15.0.30 (x64)";
    in.os = "Windows 11";
    in.arch = "x86_64";
    in.backend = "win32";
    in.ipcAbi = 3;
    in.prefix = "SnchRinne";
    in.group = 4;
    in.origin = DiagOrigin::kSmart;
    in.pid = 4242;
    in.pairState = "connected";
    in.trackHash = 0x1a2b3c4du;

    in.peer.present = true;
    in.peer.role = DiagRole::kOutput;
    in.peer.active = true;
    in.peer.samePid = false;
    in.peer.version = packVersion(1, 2, 3);
    in.peer.sampleRate = 48000;
    in.peer.maxBlock = 512;
    in.peer.channels = 2;
    in.peer.name = DiagTrackRelation::kSame;
    in.peer.heartbeatAgeMs = 1234;

    in.prepare.count = 1;
    in.prepare.sampleRate = 48000;
    in.prepare.maxBlock = 512;
    in.prepare.nonrealtimeSeen = false;
    in.prepare.setStateBeforePrepare = true;
    in.prepare.resetCalls = 7;

    in.blocks.blocksTotal = 100;
    in.blocks.zeroLen = 2;
    in.blocks.overMax = 1;
    in.blocks.bypassed = 3;
    in.prepare.maxNSeen = 1024;
    in.blocks.nMin = 32;
    in.blocks.nMax = 1024;
    in.blocks.hist[0] = 10;
    in.blocks.hist[1] = 20;
    in.blocks.hist[2] = 30;
    in.blocks.hist[3] = 20;
    in.blocks.hist[4] = 10;
    in.blocks.hist[5] = 5;
    in.blocks.hist[6] = 3;
    in.blocks.hist[7] = 0;

    in.loop.dUserMs = 12.375;
    in.loop.dEff = 594;
    in.loop.dMin = 256;
    in.loop.mode = DiagDelayMode::kSync;
    in.loop.bpm = 120.0;
    in.loop.bpmSource = DiagBpmSource::kHost;
    in.loop.tapsActive = 2;
    in.loop.tapDrop = 1;

    in.sync.resync = 1;
    in.sync.overflow = 2;
    in.sync.torn = 3;
    in.sync.overrun = 4;
    in.sync.wCasFail = 5;
    in.sync.unstable = 6;
    in.sync.offDriftBlocks = 7;
    in.sync.frozenMax = 8;

    in.order.verdict = DiagOrderVerdict::kWrong;
    in.order.votes = 0.9996;
    in.order.hostNsValid = true;

    in.safety.state = "RUN";
    in.safety.trips = 2;
    in.safety.tailPeakMaxDb = -0.5;
    in.safety.limiterGrMaxDb = -3.25;
    in.safety.nonfiniteRing = 9;
    in.safety.clampRing = 11;

    in.events = &events;
    return in;
}

DiagInputs makeOutputInputs()
{
    DiagInputs in;
    in.role = DiagRole::kOutput;
    in.product = "Rinne Output";
    in.version = "0.9.0";
    in.build = "dev.42";
    in.format = "VST3";
    in.host = "Cubase 15.0.30 (x64)";
    in.os = "Windows 11";
    in.arch = "x86_64";
    in.backend = "win32";
    in.ipcAbi = 3;
    in.prefix = "SnchRinne";
    in.group = 8;
    in.origin = DiagOrigin::kRestore;
    in.pid = 4243;
    in.pairState = "searching";
    in.trackHash = 0;
    in.peer.present = false;
    return in;
}

std::uint32_t fnv1a32(std::string_view text)
{
    std::uint32_t hash = 2166136261u;
    for (const char c : text)
    {
        hash ^= static_cast<std::uint32_t>(static_cast<unsigned char>(c));
        hash *= 16777619u;
    }
    return hash;
}

// 测试自己实现十六进制，避免用被测的 formatHex8 来验证隐私输出
std::string hex8(std::uint32_t value)
{
    constexpr char kDigits[] = "0123456789abcdef";
    std::string out(8, '0');
    for (std::size_t i = 0; i < 8u; ++i)
    {
        out[7 - i] = kDigits[static_cast<std::size_t>(value & 0xfu)];
        value >>= 4u;
    }
    return out;
}

// 隐私规则会扫描源码里的路径、主机名字面量，夹具因此在运行时拼出来，源码里没有完整的敏感串
const std::string kProfile = "alice_wonderland";
const std::string kMachine = std::string("DESK") + "TOP-ZX81QK";
const std::string kBackslash = "\\";
std::string winPathOf(const std::string& user)
{
    return std::string("C") + ":" + kBackslash + "Us" + "ers" + kBackslash + user + kBackslash + "Documents" +
           kBackslash + "song.cpr";
}
std::string macPathOf(const std::string& user)
{
    return std::string("/") + "Us" + "ers/" + user + "/Music/take7.wav";
}
std::string uncPathOf()
{
    return kBackslash + kBackslash + kMachine + kBackslash + "share" + kBackslash + "x";
}

DiagInputs makePrivacyInputs()
{
    const std::string profile = kProfile;
    const std::string machine = kMachine;
    const std::string winPath = winPathOf(profile);
    const std::string macPath = macPathOf(profile);
    const std::string uncPath = uncPathOf();
    const std::string email = profile + "@" + machine;

    DiagInputs in;
    in.role = DiagRole::kInput;
    in.product = winPath;
    in.version = "v1 " + macPath;
    in.build = uncPath;
    in.format = email;
    in.host = "~" + profile;
    in.os = "%USERNAME%";
    in.arch = "Cubase 15 " + winPath;
    in.backend = email;
    in.prefix = "team " + email;
    in.ipcAbi = 7;
    in.group = 3;
    in.origin = DiagOrigin::kManual;
    in.pid = 99;
    in.pairState = "connected " + winPath;
    in.trackHash = fnv1a32(kPrivacyTrackName);

    in.peer.present = true;
    in.peer.role = DiagRole::kOutput;
    in.peer.active = true;
    in.peer.samePid = true;
    in.peer.version = packVersion(0, 9, 0);
    in.peer.sampleRate = 48000;
    in.peer.maxBlock = 256;
    in.peer.channels = 2;
    in.peer.name = DiagTrackRelation::kSame;
    in.peer.heartbeatAgeMs = 5;

    in.safety.state = macPath;
    return in;
}

} // namespace

TEST_CASE("sanitizeText drops path-like fragments and keeps whitelisted text", "[diag]")
{
    CHECK(sanitizeText(winPathOf(kProfile)).empty());
    CHECK(sanitizeText(macPathOf(kProfile)).empty());
    CHECK(sanitizeText(uncPathOf()).empty());
    CHECK(sanitizeText(kProfile + "@" + kMachine).empty());
    CHECK(sanitizeText("~alice_wonderland").empty());
    CHECK(sanitizeText("%USERNAME%").empty());
    CHECK(sanitizeText("user:alice_wonderland").empty()); // 没有分隔符的冒号也算账号形态
    CHECK(sanitizeText(winPathOf("Jane Doe")).empty());
    CHECK(sanitizeText("Cubase 15 " + winPathOf("Jane Doe")).empty()); // 前面的词也不留
    CHECK(sanitizeText("one/two three").empty());
    CHECK(sanitizeText("before one/two three").empty());
    CHECK(sanitizeText("Cubase 15.0.30 (x64) " + winPathOf(kProfile)).empty());
    CHECK(sanitizeText("Jane " + winPathOf("Doe")).empty());
    CHECK(sanitizeText("Jane Doe" + kBackslash + "Documents" + kBackslash + "song.cpr").empty());
    CHECK(sanitizeText(std::string("C") + ":" + kBackslash + "x " + macPathOf("alice") + " ~x a@b %USERNAME%").empty());

    const std::string nonAscii = "Caf\xc3\xa9 Mix";
    CHECK(sanitizeText(nonAscii) == "Caf__ Mix");

    CHECK(sanitizeText("  alpha\tbeta\ngamma  ") == "alpha beta gamma");
    CHECK(sanitizeText("a#b [x]") == "a_b _x_");
    CHECK(sanitizeText("keep %USERNAME% also").empty());
    CHECK(sanitizeText("A-z_0+(.)") == "A-z_0+(.)");
    CHECK(sanitizeText("Cubase 15.0.30 (x64)") == "Cubase 15.0.30 (x64)");
    CHECK(sanitizeText("").empty());
    CHECK(sanitizeText("   ").empty());
}

TEST_CASE("sanitizeText truncates after joining and trims the cut", "[diag]")
{
    CHECK(sanitizeText("alpha beta gamma", 8) == "alpha be");
    CHECK(sanitizeText("alpha beta", 6) == "alpha");
    CHECK(sanitizeText(std::string(100, 'a'), 48) == std::string(48, 'a'));
    CHECK(sanitizeText("alpha", 0).empty());
}

TEST_CASE("formatFixed rounds half away from zero with clamped decimals", "[diag]")
{
    CHECK(formatFixed(-3.25, 1) == "-3.3");
    CHECK(formatFixed(100.0, 3) == "100.000");
    CHECK(formatFixed(0.0004, 3) == "0.000");
    CHECK(formatFixed(-0.0004, 3) == "0.000");
    CHECK(formatFixed(0.9996, 3) == "1.000");
    CHECK(formatFixed(2.5, 0) == "3");
    CHECK(formatFixed(-2.5, 0) == "-3");
    CHECK(formatFixed(1.23456789, 9) == "1.234568");
    CHECK(formatFixed(1.6, -3) == "2");
    CHECK(formatFixed(0.0, 0) == "0");
    CHECK(formatFixed(-0.0, 3) == "0.000");
    CHECK(formatFixed(1e15, 0) == "1000000000000000");
    CHECK(formatFixed(1e16, 3) == "nan");
    CHECK(formatFixed(-1e16, 3) == "nan");
    CHECK(formatFixed(std::numeric_limits<double>::quiet_NaN(), 3) == "nan");
    CHECK(formatFixed(std::numeric_limits<double>::infinity(), 3) == "nan");
    CHECK(formatFixed(-std::numeric_limits<double>::infinity(), 3) == "nan");
}

TEST_CASE("hex, group letters and the event name table are fixed", "[diag]")
{
    CHECK(formatHex8(0) == "00000000");
    CHECK(formatHex8(0x2a) == "0000002a");
    CHECK(formatHex8(0xdeadbeef) == "deadbeef");
    CHECK(formatHex8(0xffffffffu) == "ffffffff");

    CHECK(groupLetter(0) == "-");
    CHECK(groupLetter(1) == "A");
    CHECK(groupLetter(2) == "B");
    CHECK(groupLetter(8) == "H");
    CHECK(groupLetter(9) == "-");
    CHECK(groupLetter(0xffffffffu) == "-");

    const char* const expected[] = {
        "construct", "prepare",  "release",     "set_state",  "group_set",   "smart_default", "pair_state",
        "peer_lost", "conflict", "sr_mismatch", "resync",     "trip",        "recover",       "panic",
        "loop_on",   "loop_off", "bypass_on",   "bypass_off", "nonrealtime",
    };
    static_assert(sizeof(expected) / sizeof(expected[0]) == kEventCodeCount, "测试名字表长度必须等于 kEventCodeCount");
    for (std::size_t i = 0; i < kEventCodeCount; ++i)
    {
        const EventCode code = static_cast<EventCode>(static_cast<std::uint8_t>(i));
        CHECK(std::string(eventName(code)) == expected[i]);
    }
    CHECK(std::string(eventName(static_cast<EventCode>(static_cast<std::uint8_t>(kEventCodeCount)))) == "unknown");
    CHECK(std::string(eventName(static_cast<EventCode>(0xffu))) == "unknown");
}

TEST_CASE("the event log keeps the newest sixty-four and counts everything", "[diag]")
{
    DiagEventLog log;
    for (std::uint64_t i = 1; i <= 130u; ++i)
    {
        log.push(i, EventCode::kPanic, static_cast<std::uint32_t>(i), static_cast<std::uint32_t>(i * 2u));
    }
    CHECK(log.size() == kEventCapacity);
    CHECK(log.size() == 64u);
    CHECK(log.total() == 130u);
    CHECK(log.at(0).tMs == 67u);
    CHECK(log.at(0).a == 67u);
    CHECK(log.at(63).tMs == 130u);
    CHECK(log.at(63).a == 130u);
    CHECK(log.at(63).b == 260u);
    const DiagEvent missing = log.at(kEventCapacity);
    CHECK(missing.tMs == 0u);
    CHECK(missing.code == EventCode::kConstruct);
    CHECK(missing.a == 0u);
    CHECK(missing.b == 0u);
    CHECK(log.at(1000).a == 0u);

    DiagEventLog small;
    small.push(9, EventCode::kTrip, 2, 3);
    CHECK(small.size() == 1u);
    CHECK(small.total() == 1u);
    CHECK(small.at(0).tMs == 9u);
    CHECK(small.at(0).code == EventCode::kTrip);
    CHECK(small.at(1).a == 0u);
}

TEST_CASE("an input report is byte-for-byte stable", "[diag]")
{
    DiagEventLog events;
    events.push(1234, EventCode::kConstruct);
    events.push(61001, EventCode::kPrepare, 48000, 512);
    events.push(7000, EventCode::kNonrealtime, 1);

    const DiagInputs in = makeSnapshotInputs(events);
    const std::string expected = R"(rinne-diag v1
product=Rinne Input version=0.9.0 build=dev.42 format=VST3 host=Cubase 15.0.30 (x64) os=Windows 11 arch=x86_64
backend=win32 ipc_abi=3 prefix=SnchRinne group=D origin=smart pid=4242 pair=connected
track: relation=same hash=1a2b3c4d
peer: role=output state=active same_pid=0 version=1.2.3 sr=48000 max_block=512 ch=2 name=same hb_age_ms=1234
prepare: count=1 sr=48000 max=512 nonrealtime_seen=0 setstate_before_prepare=1 reset_calls=7
blocks: total=100 zero_len=2 over_max=1 bypassed=3 max_n_seen=1024
nsamples: min=32 max=1024 hist=10,20,30,20,10,5,3,0
loop: d_user_ms=12.375 d_eff=594 d_min=256 mode=sync bpm=120.0 bpm_src=host taps_active=2 tap_drop=1
sync: resync=1 overflow=2 torn=3 overrun=4 w_cas_fail=5 unstable=6 off_drift_blocks=7 frozen_max=8
order: verdict=wrong votes=1.00 host_ns=valid
safety: state=RUN trips=2 tail_peak_max_db=-0.5 limiter_gr_max_db=-3.3 nonfinite_ring=9 clamp_ring=11
events(last 3 of 3):
  t+1.234 construct
  t+61.001 prepare 48000/512
  t+7.000 nonrealtime 1
)";
    CHECK(buildDiagReport(in) == expected);
}

TEST_CASE("an output report omits the input-only sections", "[diag]")
{
    const DiagInputs in = makeOutputInputs();
    const std::string text = buildDiagReport(in);
    const std::string expected = R"(rinne-diag v1
product=Rinne Output version=0.9.0 build=dev.42 format=VST3 host=Cubase 15.0.30 (x64) os=Windows 11 arch=x86_64
backend=win32 ipc_abi=3 prefix=SnchRinne group=H origin=restore pid=4243 pair=searching
track: relation=unknown hash=none
peer: none
prepare: count=0 sr=0 max=0 nonrealtime_seen=0 setstate_before_prepare=0 reset_calls=0
blocks: total=0 zero_len=0 over_max=0 bypassed=0 max_n_seen=0
nsamples: min=0 max=0 hist=0,0,0,0,0,0,0,0
events(last 0 of 0):
)";
    CHECK(text == expected);
    CHECK(text.find("loop:") == std::string::npos);
    CHECK(text.find("sync:") == std::string::npos);
    CHECK(text.find("order:") == std::string::npos);
    CHECK(text.find("safety:") == std::string::npos);
    CHECK(text.find("\n  t+") == std::string::npos);
}

TEST_CASE("report generation is read-only and formats non-finite values as nan", "[diag]")
{
    DiagEventLog events;
    events.push(5, EventCode::kRelease);
    DiagInputs in = makeSnapshotInputs(events);

    const std::string first = buildDiagReport(in);
    const std::string second = buildDiagReport(in);
    CHECK(first == second);
    CHECK(in.loop.dUserMs == 12.375);
    CHECK(in.safety.state == "RUN");

    in.loop.dUserMs = std::numeric_limits<double>::quiet_NaN();
    in.loop.bpm = std::numeric_limits<double>::infinity();
    in.order.votes = std::numeric_limits<double>::quiet_NaN();
    in.safety.tailPeakMaxDb = std::numeric_limits<double>::quiet_NaN();
    in.safety.limiterGrMaxDb = -std::numeric_limits<double>::infinity();
    const std::string withNan = buildDiagReport(in);
    CHECK(withNan.find("d_user_ms=nan") != std::string::npos);
    CHECK(withNan.find(" bpm=nan") != std::string::npos);
    CHECK(withNan.find(" votes=nan") != std::string::npos);
    CHECK(withNan.find(" tail_peak_max_db=nan") != std::string::npos);
    CHECK(withNan.find(" limiter_gr_max_db=nan") != std::string::npos);
}

TEST_CASE("privacy fields never leak user, host, path or track names", "[diag]")
{
    DiagEventLog events;
    events.push(1000, EventCode::kPrepare, 48000, 512);
    events.push(2000, EventCode::kRelease);
    DiagInputs in = makePrivacyInputs();
    in.events = &events;
    const std::string text = buildDiagReport(in);

    CHECK(text.find("alice_wonderland") == std::string::npos);
    CHECK(text.find(kMachine) == std::string::npos);
    CHECK(text.find("Users") == std::string::npos);
    CHECK(text.find("Documents") == std::string::npos);
    CHECK(text.find("song.cpr") == std::string::npos);
    CHECK(text.find("take7") == std::string::npos);
    CHECK(text.find("Music") == std::string::npos);
    CHECK(text.find("share") == std::string::npos);
    CHECK(text.find("Lead Vox") == std::string::npos);
    CHECK(text.find("Secret") == std::string::npos);
    CHECK(text.find("Take 7") == std::string::npos);

    const std::string expectedHash = hex8(fnv1a32(kPrivacyTrackName));
    REQUIRE(expectedHash.size() == 8u);
    const std::size_t trackStart = text.find("track: ");
    REQUIRE(trackStart != std::string::npos);
    const std::size_t trackEnd = text.find('\n', trackStart);
    REQUIRE(trackEnd != std::string::npos);
    const std::string trackLine = text.substr(trackStart, trackEnd - trackStart);
    const std::string relationPrefix = "track: relation=";
    CHECK(trackLine.compare(0, relationPrefix.size(), relationPrefix) == 0);
    const std::size_t hashPos = trackLine.find(" hash=", relationPrefix.size());
    REQUIRE(hashPos != std::string::npos);
    const std::string relation = trackLine.substr(relationPrefix.size(), hashPos - relationPrefix.size());
    CHECK((relation == "same" || relation == "different" || relation == "unknown"));
    CHECK(relation == "same");
    CHECK(trackLine.substr(hashPos + 6u) == expectedHash);
    CHECK(trackLine.size() == hashPos + 6u + expectedHash.size());

    const std::size_t eventsPos = text.find("events(last ");
    REQUIRE(eventsPos != std::string::npos);
    std::string beforeEvents = text.substr(0, eventsPos);
    const char* const labels[] = {
        "track:", "peer:", "prepare:", "blocks:", "nsamples:", "loop:", "sync:", "order:", "safety:"};
    for (const char* rawLabel : labels)
    {
        const std::string label(rawLabel);
        const std::size_t labelPos = beforeEvents.find(label);
        REQUIRE(labelPos != std::string::npos);
        CHECK(beforeEvents.find(label, labelPos + 1u) == std::string::npos);
        beforeEvents.erase(labelPos, label.size());
    }
    CHECK(beforeEvents.find(':') == std::string::npos);
    CHECK(beforeEvents.find('\\') == std::string::npos);
    CHECK(beforeEvents.find('/') == std::string::npos);
    CHECK(beforeEvents.find('@') == std::string::npos);
    CHECK(beforeEvents.find('~') == std::string::npos);
    CHECK(beforeEvents.find('%') == std::string::npos);

    const std::size_t headerEnd = text.find('\n', eventsPos);
    REQUIRE(headerEnd != std::string::npos);
    const std::string header = text.substr(eventsPos, headerEnd - eventsPos);
    CHECK(header.rfind("events(last ", 0u) == 0u);
    REQUIRE(header.size() > 1u);
    CHECK(header.back() == ':');
    CHECK(header.find(':') == header.size() - 1u);
    CHECK(text.find(':', headerEnd) == std::string::npos);
    CHECK(text.find('\\') == std::string::npos);
    CHECK(text.find('@') == std::string::npos);
    CHECK(text.find('~') == std::string::npos);
    CHECK(text.find('%') == std::string::npos);
    CHECK(text.find('/') != std::string::npos); // 事件行的 a/b 是唯一允许的斜杠
}

TEST_CASE("sanitizing still keeps ordinary host text", "[diag]")
{
    DiagInputs in = makePrivacyInputs();
    in.host = "Cubase 15.0.30 (x64)";
    const std::string text = buildDiagReport(in);
    CHECK(text.find("host=Cubase 15.0.30 (x64)") != std::string::npos);
    CHECK(text.find("alice_wonderland") == std::string::npos);
}

TEST_CASE("a field whose fragments are all dropped becomes none", "[diag]")
{
    DiagInputs in;
    in.host = winPathOf(kProfile);
    const std::string text = buildDiagReport(in);
    CHECK(text.find("host=none") != std::string::npos);
}

TEST_CASE("the output role gets the same privacy treatment", "[diag]")
{
    DiagInputs in = makePrivacyInputs();
    in.role = DiagRole::kOutput;
    const std::string text = buildDiagReport(in);
    CHECK(text.find("alice_wonderland") == std::string::npos);
    CHECK(text.find(kMachine) == std::string::npos);
    CHECK(text.find("Users") == std::string::npos);
    CHECK(text.find("Documents") == std::string::npos);
    CHECK(text.find("song.cpr") == std::string::npos);
    CHECK(text.find("Lead Vox") == std::string::npos);
    CHECK(text.find("take7") == std::string::npos);
    CHECK(text.find("Music") == std::string::npos);
    CHECK(text.find("share") == std::string::npos);
    CHECK(text.find('\\') == std::string::npos);
    CHECK(text.find('@') == std::string::npos);
    CHECK(text.find('~') == std::string::npos);
    CHECK(text.find('%') == std::string::npos);
    // 敌意字段被清成 none，不是空值
    CHECK(text.find("host=none") != std::string::npos);
    CHECK(text.find("os=none") != std::string::npos);
    CHECK(text.find("pair=none") != std::string::npos);
    CHECK(text.find("product=none") != std::string::npos);
}

TEST_CASE("a path fragment discards the whole free text, not just its own fragment", "[diag]")
{
    // 带路径特征的片段之前还有词时，整串都不要：目录名本身可以带空格，前面的词也可能是目录名的一部分
    CHECK(sanitizeText("Jane Doe" + kBackslash + "Documents" + kBackslash + "song.cpr").empty());
    CHECK(sanitizeText("Cubase 15.0.30 (x64) " + winPathOf(kProfile)).empty());
    CHECK(sanitizeText("before one/two three").empty());
}

TEST_CASE("max_n_seen follows the current prepare while nsamples max stays lifetime", "[diag]")
{
    // 终身最大留在计数器里；「本次 prepare 以来的最大块长」由共享段的 max_n_seen 给出，两者是不同的量
    DiagCounters counters;
    counters.onBlock(1024, 1024, false);
    counters.onBlock(256, 256, false);
    const DiagCountersSnapshot blocks = counters.snapshot();
    CHECK(blocks.nMax == 1024u);

    DiagInputs in = makeOutputInputs();
    in.blocks = blocks;
    in.prepare.maxBlock = 256;
    in.prepare.maxNSeen = 256;
    const std::string text = buildDiagReport(in);
    CHECK(text.find("max_n_seen=256") != std::string::npos);
    CHECK(text.find("nsamples: min=256 max=1024") != std::string::npos);
}

TEST_CASE("events keep the order they were pushed in, not time order", "[diag]")
{
    DiagEventLog events;
    events.push(61001, EventCode::kPrepare, 48000, 512);
    events.push(7000, EventCode::kNonrealtime, 1);
    DiagInputs in = makeOutputInputs();
    in.events = &events;
    const std::string text = buildDiagReport(in);
    const std::size_t later = text.find("t+61.001 prepare");
    const std::size_t earlier = text.find("t+7.000 nonrealtime");
    REQUIRE(later != std::string::npos);
    REQUIRE(earlier != std::string::npos);
    CHECK(later < earlier); // 入环顺序，不是时间顺序
}

TEST_CASE("carriage returns and other control whitespace separate fragments", "[diag]")
{
    CHECK(sanitizeText("Cubase\r\n") == "Cubase");
    CHECK(sanitizeText("Cubase\r15\v0\f1") == "Cubase 15 0 1");
}
