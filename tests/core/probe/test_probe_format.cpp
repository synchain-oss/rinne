// SPDX-License-Identifier: GPL-3.0-or-later
//
// 六种行的格式化逐字对拍：期望的文本都是手写的字面量（照 scripts/probe/LOG-FORMAT.md 的列名与列序），
// 不用被测代码自己生成。另测百分号编码、块记录的 32 + 32 字节打包、文件名与 tok4。
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <utility>

#include "ProbeTestKit.h"
#include "diag/ProbeEvents.h"
#include "diag/ProbeLog.h"

namespace
{

using namespace rinne::diag;
using namespace rinne::test::probe;

std::string eventRow(ProbeEventCode code, std::uint64_t a = 0, std::uint64_t b = 0, std::string_view text = {})
{
    std::string out;
    REQUIRE(appendProbeEventRow(out, 100, code, ProbeEventArgs{a, b, text}));
    return out;
}

BlockFields sampleFields()
{
    BlockFields f;
    f.callSeq = 14;
    f.monoNs = 1000000000;
    f.hostNs = 7000000000;
    f.hostNsValid = true;
    f.playing = true;
    f.nonrt = false;
    f.bypassed = true;
    f.n = 256;
    f.nDecl = 512;
    f.thread = 0x1a2b3c4du;
    f.m = -3;
    f.w = 4800;
    f.off = -480;
    f.rs = 2;
    f.d = 9600;
    return f;
}

} // namespace

TEST_CASE("the probe csv starts with the magic line followed by exactly one H row", "[probe][format]")
{
    std::string out;
    appendProbeMagicAndHeader(out, testHeader(ProbeRole::kInput, "a1b2"));
    CHECK(out == "rinne-probe-log,1\nH,I,vst3,cubase,win,x64,4100,900,a1b2,0123abcd\n");

    ProbeHeader header = testHeader(ProbeRole::kOutput, "c3d4");
    header.format = ProbeFormat::kAu;
    header.host = ProbeHost::kLuna;
    header.os = ProbeOs::kMac;
    header.arch = ProbeArch::kArm64;
    header.pid = 4294967295u;
    header.ppid = 0;
    out.clear();
    appendProbeMagicAndHeader(out, header);
    CHECK(out == "rinne-probe-log,1\nH,O,au,luna,mac,arm64,4294967295,0,c3d4,0123abcd\n");

    header.host = ProbeHost::kOther;
    header.tok4 = "XYZ"; // 不合格式的 tok4 与 build 写成全 0，文件仍能被分析器读进去
    header.build = "ABC";
    out.clear();
    appendProbeMagicAndHeader(out, header);
    CHECK(out == "rinne-probe-log,1\nH,O,au,other,mac,arm64,4294967295,0,0000,00000000\n");
}

TEST_CASE("the C row carries the case context in contract order", "[probe][format]")
{
    std::string out;
    ProbeCase c;
    c.caseId = "CB-01";
    c.variant = "steady";
    c.buf = 256;
    c.guard = ProbeGuard::kLow;
    c.sampleRate = 48000;
    REQUIRE(appendProbeCaseRow(out, 999999900, c));
    CHECK(out == "C,999999900,CB-01,steady,256,low,48000\n");

    out.clear();
    c.caseId = "LU-08";
    c.variant = "-";
    c.buf = 0; // 不适用写 -
    c.guard = ProbeGuard::kNa;
    c.sampleRate = 44100;
    REQUIRE(appendProbeCaseRow(out, 5, c));
    CHECK(out == "C,5,LU-08,-,-,na,44100\n");

    for (const auto [guard, name] : {std::pair{ProbeGuard::kOff, "off"}, std::pair{ProbeGuard::kNormal, "normal"},
                                     std::pair{ProbeGuard::kHigh, "high"}})
    {
        out.clear();
        c.guard = guard;
        REQUIRE(appendProbeCaseRow(out, 1, c));
        CHECK(out == std::string("C,1,LU-08,-,-,") + name + ",44100\n");
    }
}

TEST_CASE("the C row refuses values the analyzer would reject", "[probe][format]")
{
    ProbeCase c;
    c.caseId = "CB-01";
    c.variant = "steady";
    c.buf = 256;
    c.sampleRate = 48000;
    std::string out = "keep";

    ProbeCase bad = c;
    bad.variant = "deactivate"; // 表外的 (case, variant) 组合
    CHECK_FALSE(appendProbeCaseRow(out, 1, bad));
    bad = c;
    bad.buf = 65537;
    CHECK_FALSE(appendProbeCaseRow(out, 1, bad));
    bad = c;
    bad.sampleRate = 768001;
    CHECK_FALSE(appendProbeCaseRow(out, 1, bad));
    bad = c;
    bad.caseId = "CB-01,x";
    CHECK_FALSE(appendProbeCaseRow(out, 1, bad));
    CHECK(out == "keep"); // 拒绝时输出不动

    bad = c;
    bad.buf = 65536; // 上界本身可以
    CHECK(appendProbeCaseRow(out, 1, bad));
}

TEST_CASE("every B row column is written in contract order", "[probe][format]")
{
    // seq call_seq role n n_decl host_ns host_ns_valid playing nonrt bypassed mono_ns thread m w off rs d
    std::string out;
    appendProbeBlockRow(out, ProbeRole::kInput, packBlock(7, sampleFields()));
    CHECK(out == "B,7,14,I,256,512,7000000000,1,1,0,1,1000000000,1a2b3c4d,-3,4800,-480,2,9600\n");

    // 宿主时间戳无效时 host_ns 写 0；Output 一侧 role 写 O；每个布尔位各自独立
    BlockFields f;
    f.monoNs = 5;
    f.hostNs = 999;
    f.hostNsValid = false;
    f.nonrt = true;
    out.clear();
    appendProbeBlockRow(out, ProbeRole::kOutput, packBlock(0, f));
    CHECK(out == "B,0,0,O,0,0,0,0,0,1,0,5,00000000,0,0,0,0,0\n");
}

TEST_CASE("block values beyond the contract bounds are clamped, not wrapped", "[probe][format]")
{
    BlockFields f;
    f.callSeq = 2;
    f.monoNs = 3;
    f.n = std::numeric_limits<std::uint32_t>::max();
    f.nDecl = 1048576;
    f.thread = 0xffffffffu;
    f.rs = 1000;
    f.d = std::numeric_limits<std::uint32_t>::max();
    f.m = std::numeric_limits<std::int64_t>::min();
    f.w = std::numeric_limits<std::uint64_t>::max();
    f.off = 1099511627776; // 2^40，41 位有符号装不下，钳到 2^40 - 1
    std::string out;
    appendProbeBlockRow(out, ProbeRole::kInput, packBlock(1, f));
    CHECK(out == "B,1,2,I,1048576,1048576,0,0,0,0,0,3,ffffffff,-1099511627775,1099511627776,1099511627775,255,"
                 "2147483648\n");
}

TEST_CASE("the block record is exactly 32 bytes and a ring slot exactly 64", "[probe][format]")
{
    CHECK(sizeof(BlockRecord) == 32);
    CHECK(sizeof(BlockSnapshot) == 32);
    CHECK(sizeof(BlockSlot) == 64);
}

TEST_CASE("packing a block loses nothing inside the contract bounds", "[probe][format]")
{
    std::uint64_t state = 99;
    const auto next = [&state] {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return state >> 11;
    };
    for (int i = 0; i < 2000; ++i)
    {
        BlockFields f;
        f.callSeq = next();
        f.monoNs = next();
        f.hostNsValid = (next() & 1u) != 0;
        f.hostNs = f.hostNsValid ? next() : 0;
        f.playing = (next() & 1u) != 0;
        f.nonrt = (next() & 1u) != 0;
        f.bypassed = (next() & 1u) != 0;
        f.n = static_cast<std::uint32_t>(next() % 1048577u);
        f.nDecl = static_cast<std::uint32_t>(next() % 1048577u);
        f.thread = static_cast<std::uint32_t>(next());
        f.rs = static_cast<std::uint32_t>(next() % 256u);
        f.d = static_cast<std::uint32_t>(next() % 2147483649u);
        f.m = static_cast<std::int64_t>(next() % 2199023255551u) - 1099511627775;
        f.off = static_cast<std::int64_t>(next() % 2199023255551u) - 1099511627775;
        f.w = next() % 1099511627777u;

        const BlockSlot slot = packBlock(static_cast<std::uint64_t>(i), f);
        const BlockFields g = unpackBlock(slot);
        REQUIRE(slot.record.seq == static_cast<std::uint64_t>(i));
        REQUIRE(g.callSeq == f.callSeq);
        REQUIRE(g.monoNs == f.monoNs);
        REQUIRE(g.hostNs == f.hostNs);
        REQUIRE(g.hostNsValid == f.hostNsValid);
        REQUIRE(g.playing == f.playing);
        REQUIRE(g.nonrt == f.nonrt);
        REQUIRE(g.bypassed == f.bypassed);
        REQUIRE(g.n == f.n);
        REQUIRE(g.nDecl == f.nDecl);
        REQUIRE(g.thread == f.thread);
        REQUIRE(g.rs == f.rs);
        REQUIRE(g.d == f.d);
        REQUIRE(g.m == f.m);
        REQUIRE(g.off == f.off);
        REQUIRE(g.w == f.w);
    }
}

TEST_CASE("the S and Z rows carry their counters in contract order", "[probe][format]")
{
    std::string out;
    ProbeCounters counters;
    counters.blocks = 48;
    counters.dropped = 3;
    counters.maxN = 256;
    counters.resync = 1;
    counters.trips = 0;
    appendProbeSummaryRow(out, 2000000000, counters);
    CHECK(out == "S,2000000000,48,3,256,1,0\n");

    out.clear();
    appendProbeCloseRow(out, 3000000000, 48, 3);
    CHECK(out == "Z,3000000000,48,3\n");
}

TEST_CASE("every event row writes its arguments as the contract describes", "[probe][format]")
{
    CHECK(eventRow(ProbeEventCode::kPrepare, 48000, 256) == "E,100,prepare,48000,256\n");
    CHECK(eventRow(ProbeEventCode::kRelease) == "E,100,release,,\n");
    CHECK(eventRow(ProbeEventCode::kReset) == "E,100,reset,,\n");
    CHECK(eventRow(ProbeEventCode::kSetProcessing, 1) == "E,100,set_processing,1,\n");
    CHECK(eventRow(ProbeEventCode::kSetProcessing, 7) == "E,100,set_processing,1,\n"); // 非零写 1
    CHECK(eventRow(ProbeEventCode::kSetProcessing, 0) == "E,100,set_processing,0,\n");
    CHECK(eventRow(ProbeEventCode::kOfflineRender, 0) == "E,100,offline_render,0,\n");
    CHECK(eventRow(ProbeEventCode::kSetState, 2) == "E,100,set_state,C,\n");
    CHECK(eventRow(ProbeEventCode::kSetState, 255) == "E,100,set_state,-,\n"); // 没有组
    CHECK(eventRow(ProbeEventCode::kGetState) == "E,100,get_state,,\n");
    CHECK(eventRow(ProbeEventCode::kFirstBlock) == "E,100,first_block,,\n");
    CHECK(eventRow(ProbeEventCode::kFirstPairedBlock) == "E,100,first_paired_block,,\n");
    CHECK(eventRow(ProbeEventCode::kPaired, 4100, 1) == "E,100,paired,4100,B\n");
    CHECK(eventRow(ProbeEventCode::kPaired, std::uint64_t{1} << 33, 7) == "E,100,paired,4294967295,H\n");
    CHECK(eventRow(ProbeEventCode::kBackend, 0, 0) == "E,100,backend,win32,0\n");
    CHECK(eventRow(ProbeEventCode::kBackend, 1, 13) == "E,100,backend,posix,13\n");
    CHECK(eventRow(ProbeEventCode::kShmDenied) == "E,100,shm_denied,,\n");
    CHECK(eventRow(ProbeEventCode::kMonitorToggle, 1) == "E,100,monitor_toggle,1,\n");
    CHECK(eventRow(ProbeEventCode::kArmToggle, 0) == "E,100,arm_toggle,0,\n");
    CHECK(eventRow(ProbeEventCode::kProcessMode, 0) == "E,100,process_mode,realtime,\n");
    CHECK(eventRow(ProbeEventCode::kProcessMode, 1) == "E,100,process_mode,offline,\n");
    CHECK(eventRow(ProbeEventCode::kPing, 9600) == "E,100,ping,9600,\n");
    CHECK(eventRow(ProbeEventCode::kDestruct) == "E,100,destruct,,\n");
}

TEST_CASE("track_props writes an encoded name and an optional colour", "[probe][format]")
{
    CHECK(eventRow(ProbeEventCode::kTrackProps, 0, probeColourArg(0xff8800), "Vox, Lead") ==
          "E,100,track_props,Vox%2C%20Lead,ff8800\n");
    CHECK(eventRow(ProbeEventCode::kTrackProps, 0, probeColourArgWithAlpha(0xff880080u), "Gtr") ==
          "E,100,track_props,Gtr,ff880080\n");
    CHECK(eventRow(ProbeEventCode::kTrackProps, 0, probeColourArg(0x000001), "Kick") ==
          "E,100,track_props,Kick,000001\n"); // 前导零保留，6 位
    // 从未送达：a 与 b 都为空
    CHECK(eventRow(ProbeEventCode::kTrackProps) == "E,100,track_props,,\n");
    // 逗号、换行、引号、非 ASCII 和百分号本身都被编码
    CHECK(eventRow(ProbeEventCode::kTrackProps, 0, 0, "Vox, Lead\n\"\xC3\xA9\xE4\xB8\xAD\"%") ==
          "E,100,track_props,Vox%2C%20Lead%0A%22%C3%A9%E4%B8%AD%22%25,\n");
}

TEST_CASE("an event code outside the probe segment writes nothing", "[probe][format]")
{
    std::string out = "keep";
    CHECK_FALSE(appendProbeEventRow(out, 1, static_cast<ProbeEventCode>(0x0300), {}));
    CHECK_FALSE(appendProbeEventRow(out, 1, static_cast<ProbeEventCode>(0x0512), {}));
    CHECK(out == "keep");
}

TEST_CASE("text fields are percent encoded and never contain a comma", "[probe][format]")
{
    // 全部 256 个字节值：输出只有 A-Za-z0-9._~- 和 %XX，没有逗号、引号、换行、回车，更没有非 ASCII
    std::string all;
    for (int b = 0; b < 256; ++b)
    {
        all.push_back(static_cast<char>(b));
    }
    const std::string encoded = probeEncodeText(all, 256);
    for (std::size_t i = 0; i < encoded.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(encoded[i]);
        const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
                           c == '_' || c == '~' || c == '-';
        if (c == '%')
        {
            REQUIRE(i + 2 < encoded.size());
            CHECK(std::string("0123456789ABCDEF").find(encoded[i + 1]) != std::string::npos);
            CHECK(std::string("0123456789ABCDEF").find(encoded[i + 2]) != std::string::npos);
            i += 2;
        }
        else
        {
            CHECK(plain);
        }
    }
    CHECK(encoded.find(',') == std::string::npos);
    CHECK(probeEncodeText("A-z_0.9~") == "A-z_0.9~"); // 保留字符原样
    CHECK(probeEncodeText("") == "");
}

TEST_CASE("long track names are cut at a utf-8 character boundary before encoding", "[probe][format]")
{
    CHECK(probeEncodeText(std::string(300, 'a')) == std::string(200, 'a'));

    // 100 个「中」是 300 字节；200 字节的上限落在第 67 个字的中间，退回到 198 字节（66 个字）
    std::string cjk;
    for (int i = 0; i < 100; ++i)
    {
        cjk += "\xE4\xB8\xAD";
    }
    const std::string encoded = probeEncodeText(cjk);
    CHECK(encoded.size() == 198 * 3);
    CHECK(encoded.substr(encoded.size() - 9) == "%E4%B8%AD");

    // 199 个 a 后面接一个三字节字符：上限 200 落在字符中间，只留 199 个 a
    CHECK(probeEncodeText(std::string(199, 'a') + "\xE4\xB8\xAD") == std::string(199, 'a'));
    // 正好装满 200 字节时整个字符保留
    CHECK(probeEncodeText(std::string(197, 'a') + "\xE4\xB8\xAD").size() == 197 + 9);
}

TEST_CASE("the file name is probe-role-pid-tok4.csv", "[probe][format]")
{
    CHECK(probeLogFileName(ProbeRole::kInput, 4100, "a1b2") == "probe-I-4100-a1b2.csv");
    CHECK(probeLogFileName(ProbeRole::kOutput, 7, "00ff") == "probe-O-7-00ff.csv");
}

TEST_CASE("tok4 is four lowercase hex digits and two instances of one process never share it", "[probe][format]")
{
    const std::string a = deriveProbeTok4(4100, 123456, 0);
    CHECK(a == deriveProbeTok4(4100, 123456, 0)); // 纯函数
    CHECK(a.size() == 4);
    std::set<std::string> varied;
    for (std::uint32_t counter = 0; counter < 64; ++counter)
    {
        varied.insert(deriveProbeTok4(4100, 123456, counter));
    }
    CHECK(varied.size() > 32); // 实例计数确实参与了混合

    // 同一个 pid、同一毫秒里连续分配：数量远超生日悖论的碰撞门槛，仍然一个都不重复
    std::set<std::string> seen;
    for (int i = 0; i < 3000; ++i)
    {
        const std::string tok = allocateProbeTok4(4100, 123456);
        REQUIRE(tok.size() == 4);
        for (const char c : tok)
        {
            REQUIRE(((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')));
        }
        REQUIRE(seen.insert(tok).second);
    }
}
