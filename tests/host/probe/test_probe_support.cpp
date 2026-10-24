// SPDX-License-Identifier: GPL-3.0-or-later
//
// 探针支持件：隐藏参数解析、日志目录、恢复冷却与按钮规则、反馈上限、选择器到 C 行、Ping、探针 state、复制诊断。
// 都是纯逻辑，不起界面。

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "ProbeSupport.h"
#include "state/StateDocument.h"
#include "state/StateMigration.h"

namespace
{

using namespace rinne::probe;
namespace diag = rinne::diag;
namespace state = rinne::state;

constexpr std::size_t kMaxDelay = 5000;

bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

int variantNumber(const std::string& name)
{
    if (name == "-")
    {
        return 0;
    }
    for (std::size_t i = 0; i < variantCount(); ++i)
    {
        if (variantAt(i) == name)
        {
            return static_cast<int>(i) + 1;
        }
    }
    return -1;
}

int caseNumber(const std::string& name)
{
    for (std::size_t i = 0; i < caseCount(); ++i)
    {
        if (caseAt(i) == name)
        {
            return static_cast<int>(i) + 1;
        }
    }
    return -1;
}

std::size_t countKeys()
{
    const std::string_view keys = diag::probeCaseKeys();
    std::size_t n = 0;
    bool inToken = false;
    for (const char c : keys)
    {
        if (c != ' ' && !inToken)
        {
            ++n;
        }
        inToken = c != ' ';
    }
    return n;
}

void patchFloat(std::vector<std::uint8_t>& bytes, std::size_t offset, float value)
{
    std::memcpy(bytes.data() + offset, &value, sizeof value);
}

} // namespace

TEST_CASE("hidden parameters resolve to a case, a variant and a record time", "[host][probe][auto]")
{
    REQUIRE(caseCount() == 21);
    CHECK(caseAt(0) == "CB-01");
    CHECK(caseAt(11) == "CB-12");
    CHECK(caseAt(12) == "LU-01");
    CHECK(caseAt(20) == "LU-09");
    CHECK(caseAt(21).empty());

    SECTION("a legal triple resolves")
    {
        const AutoRequest r = parseAutoParams(caseNumber("CB-04"), variantNumber("rip"), 45);
        CHECK(r.status == AutoStatus::kOk);
        CHECK(r.caseId == "CB-04");
        CHECK(r.variant == "rip");
        CHECK(r.recordSeconds == 45);

        const AutoRequest dash = parseAutoParams(caseNumber("LU-01"), 0, 10);
        CHECK(dash.status == AutoStatus::kOk);
        CHECK(dash.variant == "-");
    }

    SECTION("record seconds zero means do nothing, whatever the rest says")
    {
        CHECK(parseAutoParams(0, 0, 0).status == AutoStatus::kIdle);
        CHECK(parseAutoParams(99, -4, 0).status == AutoStatus::kIdle);
    }

    SECTION("out of range numbers are refused")
    {
        CHECK(parseAutoParams(0, 1, 30).status == AutoStatus::kBadCase);
        CHECK(parseAutoParams(-1, 1, 30).status == AutoStatus::kBadCase);
        CHECK(parseAutoParams(static_cast<int>(caseCount()) + 1, 1, 30).status == AutoStatus::kBadCase);
        CHECK(parseAutoParams(1, -1, 30).status == AutoStatus::kBadVariant);
        CHECK(parseAutoParams(1, static_cast<int>(variantCount()) + 1, 30).status == AutoStatus::kBadVariant);
        CHECK(parseAutoParams(1, variantNumber("steady"), -1).status == AutoStatus::kBadRecordSeconds);
        CHECK(parseAutoParams(1, variantNumber("steady"), static_cast<int>(kAutoRecordMaxSeconds) + 1).status ==
              AutoStatus::kBadRecordSeconds);
        CHECK(parseAutoParams(1, variantNumber("steady"), static_cast<int>(kAutoRecordMaxSeconds)).status ==
              AutoStatus::kOk);
    }

    SECTION("a combination outside the rule table is refused")
    {
        CHECK(parseAutoParams(caseNumber("CB-01"), variantNumber("deactivate"), 30).status ==
              AutoStatus::kBadCombination);
        CHECK(parseAutoParams(caseNumber("CB-01"), 0, 30).status == AutoStatus::kBadCombination);
        CHECK(parseAutoParams(caseNumber("LU-01"), variantNumber("steady"), 30).status == AutoStatus::kBadCombination);
        CHECK(parseAutoParams(caseNumber("LU-03"), variantNumber("rename"), 30).status == AutoStatus::kBadCombination);
    }

    SECTION("every pair is accepted exactly when the contract table lists it")
    {
        std::size_t accepted = 0;
        for (int c = 1; c <= static_cast<int>(caseCount()); ++c)
        {
            for (int v = 0; v <= static_cast<int>(variantCount()); ++v)
            {
                const AutoRequest r = parseAutoParams(c, v, 1);
                const std::string variant = v == 0 ? "-" : variantAt(static_cast<std::size_t>(v) - 1);
                const bool listed = diag::isValidProbeCase(caseAt(static_cast<std::size_t>(c) - 1), variant);
                CHECK((r.status == AutoStatus::kOk) == listed);
                CHECK((r.status == AutoStatus::kBadCombination) == !listed);
                accepted += r.status == AutoStatus::kOk ? 1u : 0u;
            }
        }
        CHECK(accepted == countKeys());
    }

    SECTION("variants per case come from the same table")
    {
        CHECK(variantsFor("CB-01") == std::vector<std::string>{"steady"});
        CHECK(variantsFor("LU-01") == std::vector<std::string>{"-"});
        CHECK(variantsFor("CB-05").size() == 4);
        CHECK(variantsFor("nope").empty());
    }
}

TEST_CASE("the log directory comes from the environment first, then the documents folder", "[host][probe][logdir]")
{
    std::string asked;
    const auto env = [&asked](std::string value) {
        return EnvReader([&asked, value](const char* name) {
            asked = name;
            return value;
        });
    };

    SECTION("an absolute path in the environment wins")
    {
        const ProbeLogDir dir = resolveLogDir(env("C:\\runs\\p0"), "D:\\accounts\\someone\\Documents");
        CHECK(asked == "RINNE_PROBE_LOG_DIR");
        CHECK(dir.path == "C:\\runs\\p0");
        CHECK(dir.fromEnvironment);
        CHECK(resolveLogDir(env("/var/probe"), "/srv/someone/Documents").path == "/var/probe");
        CHECK(resolveLogDir(env("  D:/logs  "), "C:\\docs").path == "D:/logs");
    }

    SECTION("nothing usable in the environment falls back to the documents folder")
    {
        for (const char* value : {"", "   ", "relative/dir", "logs", "~/logs"})
        {
            const ProbeLogDir dir = resolveLogDir(env(value), "D:\\accounts\\someone\\Documents\\");
            CHECK(dir.path == "D:\\accounts\\someone\\Documents\\Rinne Probe Logs");
            CHECK_FALSE(dir.fromEnvironment);
        }
        CHECK(resolveLogDir(EnvReader{}, "/srv/someone/Documents").path == "/srv/someone/Documents/Rinne Probe Logs");
    }

    SECTION("no environment and no documents folder leaves the path empty")
    {
        const ProbeLogDir dir = resolveLogDir(env(""), "");
        CHECK(dir.path.empty());
        CHECK(logDirLabel(dir) == "none");
    }

    SECTION("the label is default or the last folder name, never the whole path")
    {
        CHECK(logDirLabel(resolveLogDir(env(""), "D:\\accounts\\someone\\Documents")) == "default");
        CHECK(logDirLabel(resolveLogDir(env("D:\\accounts\\someone\\runs\\ProbeRun7\\"), "")) == "ProbeRun7");
        CHECK(logDirLabel(resolveLogDir(env("/Volumes/data/probe-2"), "")) == "probe-2");
    }

    SECTION("the real documents folder is wired, the environment only through the reader handed in")
    {
        const ProbeLogDir fallback = resolveLogDir(EnvReader{});
        CHECK_FALSE(fallback.path.empty());
        CHECK_FALSE(fallback.fromEnvironment);
        CHECK(fallback.path.size() > std::string(kDefaultLogDirName).size());
        CHECK(fallback.path.substr(fallback.path.size() - std::string(kDefaultLogDirName).size()) ==
              kDefaultLogDirName);
        CHECK(resolveLogDir(env("D:/elsewhere")).path == "D:/elsewhere");
    }
}

TEST_CASE("recover waits 1.5 s after a panic", "[host][probe][cooldown]")
{
    RecoverCooldown cooldown;
    CHECK(cooldown.ready(0));
    CHECK(cooldown.remainingMs(12345) == 0);

    cooldown.onPanic(10000);
    CHECK_FALSE(cooldown.ready(10000));
    CHECK(cooldown.remainingMs(10000) == 1500);
    CHECK(cooldown.remainingMs(10400) == 1100);
    CHECK_FALSE(cooldown.ready(11499));
    CHECK(cooldown.ready(11500));
    CHECK(cooldown.ready(60000));

    // 时钟倒退不能让冷却提前结束
    CHECK_FALSE(cooldown.ready(9000));

    // 再点一次 Panic 重新计时
    cooldown.onPanic(20000);
    CHECK_FALSE(cooldown.ready(21000));
    CHECK(cooldown.ready(21500));
}

TEST_CASE("ping is only allowed with the transport stopped or the dry signal silent", "[host][probe][buttons]")
{
    for (const bool playing : {false, true})
    {
        for (const bool silent : {false, true})
        {
            for (const bool running : {false, true})
            {
                ProbeStatus status;
                status.transportPlaying = playing;
                status.drySilent = silent;
                status.pingRunning = running;
                INFO("playing " << playing << " silent " << silent << " running " << running);
                CHECK(pingAllowed(status) == (!running && (!playing || silent)));
            }
        }
    }
    ProbeStatus playingLoud;
    playingLoud.transportPlaying = true;
    CHECK_FALSE(pingAllowed(playingLoud));

    CHECK(autoPingTrigger(true, false));
    CHECK_FALSE(autoPingTrigger(false, false));
    CHECK_FALSE(autoPingTrigger(true, true));
    CHECK_FALSE(autoPingTrigger(false, true));
}

TEST_CASE("recording needs an idle recorder and a selection from the rule table", "[host][probe][buttons]")
{
    ProbeStatus status;
    ProbeSelection good;
    CHECK(recordAllowed(status, good));

    ProbeSelection bad;
    bad.variant = "deactivate"; // CB-01 没有这个 variant
    CHECK_FALSE(recordAllowed(status, bad));

    status.recording = true;
    CHECK_FALSE(recordAllowed(status, good));
}

TEST_CASE("feedback never goes above 80 percent and defaults to zero", "[host][probe][limits]")
{
    CHECK(kFeedbackMaxPercent == 80.0f);
    CHECK(clampFeedbackPercent(100.0f) == 80.0f);
    CHECK(clampFeedbackPercent(150.0f) == 80.0f);
    CHECK(clampFeedbackPercent(80.0f) == 80.0f);
    CHECK(clampFeedbackPercent(79.5f) == 79.5f);
    CHECK(clampFeedbackPercent(-3.0f) == 0.0f);
    CHECK(clampFeedbackPercent(std::nanf("")) == 0.0f);

    CHECK(ProbeStatus{}.feedbackPercent == 0.0f);
    CHECK(ProbeStateData{}.feedbackPercent == 0.0f);

    CHECK(kCeilingDefaultDb == -18.0f);
    CHECK(clampCeilingDb(-30.0f) == -24.0f);
    CHECK(clampCeilingDb(6.0f) == 0.0f);
    CHECK(clampCeilingDb(std::nanf("")) == -18.0f);
    CHECK(clampDelayMs(0.0f) == 1.0f);
    CHECK(clampDelayMs(9000.0f) == 4000.0f);
    CHECK(clampDelayMs(std::nanf("")) == 100.0f);
}

TEST_CASE("the panel selection becomes the fields of the C row", "[host][probe][selection]")
{
    ProbeSelection selection;
    selection.caseId = "CB-01";
    selection.variant = "steady";
    selection.buf = 256;
    selection.guard = diag::ProbeGuard::kLow;

    std::string row;
    REQUIRE(diag::appendProbeCaseRow(row, 1000, toProbeCase(selection, 48000)));
    CHECK(row == "C,1000,CB-01,steady,256,low,48000\n");

    selection.caseId = "LU-02";
    selection.variant = "au-vst3";
    selection.buf = 0;
    selection.guard = diag::ProbeGuard::kNa;
    row.clear();
    REQUIRE(diag::appendProbeCaseRow(row, 7, toProbeCase(selection, 44100)));
    CHECK(row == "C,7,LU-02,au-vst3,-,na,44100\n");

    // 面板能选出来的每个组合，C 行都收；规则表外的，选择无效，C 行也拒
    for (std::size_t c = 0; c < caseCount(); ++c)
    {
        for (const std::string& variant : variantsFor(caseAt(c)))
        {
            for (const std::uint32_t buf : kBufferChoices)
            {
                for (const diag::ProbeGuard guard : kGuardChoices)
                {
                    ProbeSelection each;
                    each.caseId = caseAt(c);
                    each.variant = variant;
                    each.buf = buf;
                    each.guard = guard;
                    std::string line;
                    CHECK(isValidSelection(each));
                    CHECK(diag::appendProbeCaseRow(line, 1, toProbeCase(each, 48000)));
                }
            }
        }
    }
    ProbeSelection outside;
    outside.variant = "deactivate";
    std::string line;
    CHECK_FALSE(isValidSelection(outside));
    CHECK_FALSE(diag::appendProbeCaseRow(line, 1, toProbeCase(outside, 48000)));
    CHECK(line.empty());
}

TEST_CASE("ping finds the loop delay in the raw readback", "[host][probe][ping]")
{
    // 把 Ping 当作一个已知延迟的环：回读 = 注入序列延迟 delay 个样本再乘增益，加一点确定性的噪声。
    // 回读由「应该注入什么」算出，不看 process 实际写了什么，注入内容另行核对。
    const auto run = [](std::size_t delay, float gain, float noise, const std::vector<std::size_t>& blocks,
                        std::vector<float>* injected) {
        ProbePing ping(kMaxDelay);
        REQUIRE(ping.start());
        std::uint32_t lcg = 12345u;
        std::size_t position = 0;
        std::size_t blockIndex = 0;
        while (ping.state() != ProbePing::State::kDone)
        {
            const std::size_t n = blocks[blockIndex++ % blocks.size()];
            std::vector<float> inject(n);
            std::vector<float> readback(n);
            for (std::size_t i = 0; i < n; ++i)
            {
                const std::size_t p = position + i;
                lcg = lcg * 1664525u + 1013904223u;
                const float jitter = (static_cast<float>(lcg >> 8) / 8388608.0f - 1.0f) * noise;
                const bool inSequence = p >= delay && p - delay < kPingBuffer.size();
                readback[i] = (inSequence ? gain * kPingBuffer[p - delay] : 0.0f) + jitter;
            }
            REQUIRE(ping.process(inject.data(), readback.data(), n));
            if (injected != nullptr)
            {
                injected->insert(injected->end(), inject.begin(), inject.end());
            }
            position += n;
        }
        return ping.finish();
    };

    SECTION("the delay comes back exactly, whatever the block sizes")
    {
        for (const std::size_t delay :
             {std::size_t{0}, std::size_t{1}, std::size_t{127}, std::size_t{480}, std::size_t{4799}, kMaxDelay})
        {
            for (const std::vector<std::size_t>& blocks :
                 {std::vector<std::size_t>{64}, std::vector<std::size_t>{7, 1000, 33}, std::vector<std::size_t>{1}})
            {
                const PingResult r = run(delay, 1.0f, 0.0f, blocks, nullptr);
                INFO("delay " << delay << " first block " << blocks.front());
                CHECK(r.ok);
                CHECK(r.delaySamples == delay);
                CHECK(r.peak > 12.0);
            }
        }
    }

    SECTION("an attenuated, noisy readback still gives the right delay")
    {
        const PingResult r = run(2400, 0.2f, 0.02f, {128}, nullptr);
        CHECK(r.ok);
        CHECK(r.delaySamples == 2400);
    }

    SECTION("a silent or noise-only readback is not a result")
    {
        CHECK_FALSE(run(100, 0.0f, 0.0f, {256}, nullptr).ok);
        CHECK_FALSE(run(100, 0.0f, 0.05f, {256}, nullptr).ok);
    }

    SECTION("the injection is the 127 point sequence at minus 20 dBFS, then silence")
    {
        std::vector<float> injected;
        (void)run(10, 1.0f, 0.0f, {50}, &injected);
        REQUIRE(injected.size() >= kPingBuffer.size() + 1);
        float peak = 0.0f;
        for (std::size_t i = 0; i < kPingBuffer.size(); ++i)
        {
            CHECK(injected[i] == kPingBuffer[i]);
            peak = std::max(peak, std::fabs(injected[i]));
        }
        CHECK(20.0 * std::log10(static_cast<double>(peak)) == Catch::Approx(-20.0).margin(1e-4));
        for (std::size_t i = kPingBuffer.size(); i < injected.size(); ++i)
        {
            CHECK(injected[i] == 0.0f);
        }
    }
}

TEST_CASE("the ping state machine runs once at a time and mutes the loop while it runs", "[host][probe][ping]")
{
    ProbePing ping(200);
    std::vector<float> inject(64, 5.0f);
    const std::vector<float> readback(64, 0.0f);

    CHECK(ping.state() == ProbePing::State::kIdle);
    CHECK_FALSE(ping.active());
    CHECK_FALSE(ping.process(inject.data(), readback.data(), 64));
    CHECK(inject[0] == 5.0f); // 没在跑，注入缓冲不动
    CHECK(ping.returnGain(0.6f) == 0.6f);
    CHECK(ping.dryGain(1.0f) == 1.0f);
    CHECK_FALSE(ping.finish().ok);

    REQUIRE(ping.start());
    CHECK_FALSE(ping.start());
    CHECK(ping.active());
    CHECK(ping.returnGain(0.6f) == 0.0f);
    CHECK(ping.dryGain(1.0f) == 0.0f);

    REQUIRE(ping.process(inject.data(), readback.data(), 64));
    CHECK(ping.state() == ProbePing::State::kRunning);
    CHECK(ping.returnGain(0.6f) == 0.0f);

    ping.cancel();
    CHECK(ping.state() == ProbePing::State::kIdle);
    CHECK(ping.returnGain(0.6f) == 0.6f);

    // 取消之后可以重新开始，窗口从头算
    REQUIRE(ping.start());
    std::size_t blocks = 0;
    while (ping.state() != ProbePing::State::kDone && blocks < 100)
    {
        REQUIRE(ping.process(inject.data(), readback.data(), 64));
        ++blocks;
    }
    CHECK(blocks == 6); // 127 + 200 = 327 个样本，64 一块要 6 块
    CHECK(ping.returnGain(0.6f) == 0.6f); // 攒满之后回送恢复
    CHECK_FALSE(ping.finish().ok); // 全 0 的回读
    CHECK(ping.state() == ProbePing::State::kIdle);
}

TEST_CASE("the probe state is an abi 0 header plus one probe chunk", "[host][probe][state]")
{
    ProbeStateData data;
    data.group = 3;
    data.feedbackPercent = 40.0f;
    data.delayMs = 250.0f;
    data.ceilingDb = -12.0f;

    const std::vector<std::uint8_t> bytes = encodeState(data, state::Role::Input);
    REQUIRE(bytes.size() == kStateBytes);
    CHECK(kStateBytes == 40);

    SECTION("the header carries RNST, abi 0, the role and one chunk")
    {
        state::Header header;
        REQUIRE(state::parseHeader(bytes.data(), bytes.size(), header));
        CHECK(header.magic == state::kMagic);
        CHECK(header.abi == 0u);
        CHECK(header.abi == state::kProbeAbi);
        CHECK(header.role == static_cast<std::uint32_t>(state::Role::Input));
        CHECK(header.chunkCount == 1u);
        CHECK(bytes[0] == 'R');
        CHECK(bytes[1] == 'N');
        CHECK(bytes[2] == 'S');
        CHECK(bytes[3] == 'T');
        CHECK(bytes[16] == 'P');
        CHECK(bytes[17] == 'R');
        CHECK(bytes[18] == 'B');
        CHECK(bytes[19] == 'E');
    }

    SECTION("a formal build reads only the header and ignores the rest, for either role")
    {
        for (const state::Role role : {state::Role::Input, state::Role::Output})
        {
            state::StateDocument document = state::makeDefaultDocument(role);
            const state::StateLoadResult result = state::loadState(bytes.data(), bytes.size(), role, document);
            CHECK(result.status == state::StateLoadStatus::IgnoredProbe);
            CHECK(result.sourceAbi == 0u);
        }
        const std::vector<std::uint8_t> asOutput = encodeState(data, state::Role::Output);
        state::StateDocument document = state::makeDefaultDocument(state::Role::Input);
        CHECK(state::loadState(asOutput.data(), asOutput.size(), state::Role::Input, document).status ==
              state::StateLoadStatus::IgnoredProbe);
    }

    SECTION("group, feedback, delay and ceiling round trip")
    {
        ProbeStateData back;
        REQUIRE(decodeState(bytes.data(), bytes.size(), back) == StateStatus::kOk);
        CHECK(back.group == 3);
        CHECK(back.feedbackPercent == 40.0f);
        CHECK(back.delayMs == 250.0f);
        CHECK(back.ceilingDb == -12.0f);

        ProbeStateData unset;
        unset.group = -1;
        const std::vector<std::uint8_t> unsetBytes = encodeState(unset, state::Role::Output);
        REQUIRE(decodeState(unsetBytes.data(), unsetBytes.size(), back) == StateStatus::kOk);
        CHECK(back.group == -1);
        CHECK(back.feedbackPercent == 0.0f);
        CHECK(back.ceilingDb == -18.0f);
    }

    SECTION("every truncation is refused and leaves the output alone")
    {
        ProbeStateData untouched;
        untouched.group = 6;
        untouched.feedbackPercent = 12.0f;
        for (std::size_t size = 1; size < bytes.size(); ++size)
        {
            ProbeStateData out = untouched;
            INFO("size " << size);
            CHECK(decodeState(bytes.data(), size, out) == StateStatus::kCorrupt);
            CHECK(out.group == 6);
            CHECK(out.feedbackPercent == 12.0f);
        }
        ProbeStateData out;
        CHECK(decodeState(bytes.data(), 0, out) == StateStatus::kEmpty);
        CHECK(decodeState(nullptr, 0, out) == StateStatus::kEmpty);

        std::vector<std::uint8_t> longer = bytes;
        longer.push_back(0);
        CHECK(decodeState(longer.data(), longer.size(), out) == StateStatus::kCorrupt);
    }

    SECTION("bytes a formal build wrote are not taken over")
    {
        state::Container container;
        container.role = state::Role::Input;
        std::vector<std::uint8_t> formal;
        REQUIRE(state::encodeContainer(container, formal));
        ProbeStateData out;
        CHECK(decodeState(formal.data(), formal.size(), out) == StateStatus::kNotProbe);
    }

    SECTION("damaged fields are refused, out of range numbers are pulled in")
    {
        ProbeStateData out;

        std::vector<std::uint8_t> badMagic = bytes;
        badMagic[0] = 'X';
        CHECK(decodeState(badMagic.data(), badMagic.size(), out) == StateStatus::kCorrupt);

        std::vector<std::uint8_t> badChunk = bytes;
        badChunk[16] = 'Q';
        CHECK(decodeState(badChunk.data(), badChunk.size(), out) == StateStatus::kCorrupt);

        std::vector<std::uint8_t> badGroup = bytes;
        badGroup[24] = 9;
        CHECK(decodeState(badGroup.data(), badGroup.size(), out) == StateStatus::kCorrupt);

        std::vector<std::uint8_t> nan = bytes;
        patchFloat(nan, 28, std::nanf(""));
        CHECK(decodeState(nan.data(), nan.size(), out) == StateStatus::kCorrupt);

        std::vector<std::uint8_t> loud = bytes;
        patchFloat(loud, 28, 120.0f);
        patchFloat(loud, 32, 90000.0f);
        patchFloat(loud, 36, 5.0f);
        REQUIRE(decodeState(loud.data(), loud.size(), out) == StateStatus::kOk);
        CHECK(out.feedbackPercent == 80.0f);
        CHECK(out.delayMs == 4000.0f);
        CHECK(out.ceilingDb == 0.0f);
    }
}

TEST_CASE("copied diagnostics add the probe lines and carry no path, user or host name", "[host][probe][diag]")
{
    diag::DiagInputs base;
    base.product = "Rinne Probe Input";
    base.version = "0.1.0";
    base.format = "VST3";
    base.host = "Cubase 15";
    base.os = "Windows 11";
    base.arch = "x64";

    ProbeStatus status;
    status.recording = true;
    status.lastPingSamples = 4800;
    status.counters.blocks = 1234;
    status.counters.dropped = 2;
    status.ceilingDb = -18.0f;

    ProbeSelection selection;
    selection.buf = 256;
    selection.guard = diag::ProbeGuard::kLow;

    const auto envDir = [](const std::string& value) {
        return resolveLogDir(EnvReader([value](const char*) { return value; }), "D:\\accounts\\alice\\Documents");
    };

    SECTION("the base report comes first, then the probe lines")
    {
        const std::string text = buildDiagText(base, status, envDir(""), selection);
        CHECK(text.rfind("rinne-diag v1\n", 0) == 0);
        CHECK(contains(text, "probe: log_dir=default recording=1 feedback_cap=80 ceiling_db=-18.0\n"));
        CHECK(contains(text, "probe_case: case=CB-01 variant=steady buf=256 guard=low\n"));
        CHECK(contains(text, "probe_ping: samples=4800\n"));
        CHECK(contains(text, "probe_log: blocks=1234 dropped=2 max_n=0 resync=0 trips=0\n"));
        CHECK(text.find(diag::buildDiagReport(base)) == 0);
        CHECK(text.back() == '\n');
    }

    SECTION("a custom folder shows only its last name")
    {
        const std::string text = buildDiagText(base, status, envDir("D:\\accounts\\alice\\runs\\ProbeRun7"), selection);
        CHECK(contains(text, "log_dir=ProbeRun7 "));
        for (const char* leak : {"alice", "accounts", "runs", "C:", "\\", "Documents", "Rinne Probe Logs"})
        {
            INFO("leaked " << leak);
            CHECK_FALSE(contains(text, leak));
        }
    }

    SECTION("the default folder shows as default, not as the documents path")
    {
        const std::string text = buildDiagText(base, status, envDir(""), selection);
        for (const char* leak : {"alice", "accounts", "C:", "\\", "Documents", "Rinne Probe Logs"})
        {
            INFO("leaked " << leak);
            CHECK_FALSE(contains(text, leak));
        }
    }

    SECTION("no ping yet reads as none")
    {
        status.lastPingSamples = -1;
        CHECK(contains(buildDiagText(base, status, envDir(""), selection), "probe_ping: samples=none\n"));
    }
}

TEST_CASE("status and counter text describe the host state", "[host][probe][text]")
{
    ProbeStatus status;
    status.paired = true;
    status.group = 2;
    status.transportPlaying = true;
    status.recording = true;
    status.recordRemainingSeconds = 42;
    status.lastPingSamples = 4800;
    status.counters.blocks = 99;
    status.feedbackPercent = 15.0f;

    const std::string text = statusText(status);
    CHECK(contains(text, "link: paired (group C)"));
    CHECK(contains(text, "transport: playing, dry: active"));
    CHECK(contains(text, "record: recording, 42 s left"));
    CHECK(contains(text, "ping: 4800 samples"));
    CHECK(contains(text, "safety: ok"));
    CHECK(contains(countersText(status), "blocks 99"));
    CHECK(contains(countersText(status), "feedback 15.0 %"));

    ProbeStatus idle;
    CHECK(contains(statusText(idle), "link: not paired (group -)"));
    CHECK(contains(statusText(idle), "ping: idle"));
    idle.pingRunning = true;
    idle.tripped = true;
    CHECK(contains(statusText(idle), "ping: running"));
    CHECK(contains(statusText(idle), "safety: tripped"));
}
