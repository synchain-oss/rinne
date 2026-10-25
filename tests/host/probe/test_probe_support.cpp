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

TEST_CASE("the log directory has one source, the Rinne Probe Logs folder under the documents folder",
          "[host][probe][logdir]")
{
    SECTION("the folder is joined to the documents folder with the separator that folder uses")
    {
        CHECK(resolveLogDir("D:\\accounts\\someone\\Documents").path ==
              "D:\\accounts\\someone\\Documents\\Rinne Probe Logs");
        CHECK(resolveLogDir("D:\\accounts\\someone\\Documents\\").path ==
              "D:\\accounts\\someone\\Documents\\Rinne Probe Logs");
        CHECK(resolveLogDir("/srv/someone/Documents").path == "/srv/someone/Documents/Rinne Probe Logs");
        CHECK(resolveLogDir("/srv/someone/Documents//").path == "/srv/someone/Documents/Rinne Probe Logs");
        CHECK(std::string(kDefaultLogDirName) == "Rinne Probe Logs");
    }

    SECTION("no documents folder leaves the path empty and the label none")
    {
        const ProbeLogDir dir = resolveLogDir("");
        CHECK(dir.path.empty());
        CHECK(logDirLabel(dir) == "none");
    }

    SECTION("the label is default or none, never a piece of the path")
    {
        CHECK(logDirLabel(resolveLogDir("D:\\accounts\\someone\\Documents")) == "default");
        CHECK(logDirLabel(resolveLogDir("/srv/someone/Documents")) == "default");
        CHECK(logDirLabel(ProbeLogDir{}) == "none");
    }

    SECTION("the real documents folder is wired")
    {
        const ProbeLogDir real = resolveLogDir();
        const std::string name = kDefaultLogDirName;
        REQUIRE(real.path.size() > name.size());
        CHECK(real.path.substr(real.path.size() - name.size()) == name);
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
                for (const bool bypassed : {false, true})
                {
                    ProbeStatus status;
                    status.transportPlaying = playing;
                    status.drySilent = silent;
                    status.pingRunning = running;
                    status.bypassed = bypassed;
                    INFO("playing " << playing << " silent " << silent << " running " << running << " bypassed "
                                    << bypassed);
                    CHECK(pingAllowed(status) == (!running && !bypassed && (!playing || silent)));
                }
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
    status.logOpen = true;
    ProbeSelection good;
    CHECK(recordAllowed(status, good));

    ProbeSelection bad;
    bad.variant = "deactivate"; // CB-01 没有这个 variant
    CHECK_FALSE(recordAllowed(status, bad));

    status.recording = true;
    CHECK_FALSE(recordAllowed(status, good));
}

TEST_CASE("recording is off while the log file is not open", "[host][probe][buttons]")
{
    ProbeStatus status;
    CHECK_FALSE(status.logOpen); // 没填就当没开：数据会全丢，面板不能让人白录一场
    const ProbeSelection good;
    CHECK_FALSE(recordAllowed(status, good));
    status.logOpen = true;
    CHECK(recordAllowed(status, good));
    status.logOpen = false;
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

namespace
{

constexpr double kRate = 48000.0;
constexpr std::size_t kNoIndex = static_cast<std::size_t>(-1);

// 一次 Ping 的逐样本记录：注入、两个缩放系数，以及淡入请求在哪个样本之后被取走
struct PingTrace
{
    std::vector<float> injection;
    std::vector<float> dry;
    std::vector<float> ret;
    std::size_t requests = 0;
    std::size_t takenAfter = kNoIndex; // 取走请求时已经走过的样本数
};

// 按 blockSize 一块一块驱动 ping，回读全 0；每块之后取一次淡入请求。cancelAfter 不是 kNoIndex 时，走过这么多样本之后
// 的第一块开头请求取消。直到状态不再是 Armed / Running（Done 或取消后的 Idle）为止。
PingTrace drive(ProbePing& ping, std::size_t blockSize, std::size_t cancelAfter = kNoIndex)
{
    PingTrace trace;
    bool cancelled = false;
    std::vector<float> inject(blockSize);
    std::vector<float> dry(blockSize);
    std::vector<float> ret(blockSize);
    const std::vector<float> readback(blockSize, 0.0f);
    // 总样本数封顶：状态机被改坏而永远不结束时，这里按样本数截断，让断言去红，不去吃内存和时间
    // （正常的一次 Ping 连带静音段与取消都在 2 万个样本内走完）
    constexpr std::size_t kTraceCap = 60000;
    while ((ping.active() || trace.dry.empty()) && trace.dry.size() < kTraceCap)
    {
        if (!cancelled && cancelAfter != kNoIndex && trace.dry.size() >= cancelAfter)
        {
            ping.cancel();
            cancelled = true;
        }
        REQUIRE(ping.process(inject.data(), readback.data(), dry.data(), ret.data(), blockSize));
        trace.injection.insert(trace.injection.end(), inject.begin(), inject.end());
        trace.dry.insert(trace.dry.end(), dry.begin(), dry.end());
        trace.ret.insert(trace.ret.end(), ret.begin(), ret.end());
        if (ping.takeFadeInRequest())
        {
            ++trace.requests;
            trace.takenAfter = trace.dry.size();
        }
    }
    return trace;
}

// 相邻样本之间（含进入前的 1.0）变化量的最大值
float largestStep(const std::vector<float>& gain)
{
    float largest = 0.0f;
    float previous = 1.0f;
    for (const float g : gain)
    {
        largest = std::max(largest, std::fabs(g - previous));
        previous = g;
    }
    return largest;
}

} // namespace

TEST_CASE("ping finds the loop delay in the raw readback", "[host][probe][ping]")
{
    // 把 Ping 当作一个已知延迟的环：回读 = 注入序列延迟 delay 个样本再乘增益，加一点确定性的噪声。
    // 注入从两个增益降到 0、再静音 settle 个样本之后才开始（rampSamples + settle 个样本之后），回读由「应该注入什么」
    // 算出，不看 process 实际写了什么， 注入内容另行核对。
    const auto runOn = [](ProbePing& ping, std::size_t delay, float gain, float noise,
                          const std::vector<std::size_t>& blocks, std::vector<float>* injected,
                          std::size_t settle = 0) {
        const std::size_t lead = ping.rampSamples() + settle;
        REQUIRE(ping.start(settle));
        std::uint32_t lcg = 12345u;
        std::size_t position = 0;
        std::size_t blockIndex = 0;
        while (ping.state() != ProbePing::State::kDone)
        {
            REQUIRE(position < 60000); // 状态机被改坏而不结束时，按样本数截断，让断言去红
            const std::size_t n = blocks[blockIndex++ % blocks.size()];
            std::vector<float> inject(n);
            std::vector<float> readback(n);
            std::vector<float> dry(n);
            std::vector<float> ret(n);
            for (std::size_t i = 0; i < n; ++i)
            {
                const std::size_t p = position + i;
                lcg = lcg * 1664525u + 1013904223u;
                const float jitter = (static_cast<float>(lcg >> 8) / 8388608.0f - 1.0f) * noise;
                const bool inSequence = p >= lead + delay && p - lead - delay < kPingBuffer.size();
                readback[i] = (inSequence ? gain * kPingBuffer[p - lead - delay] : 0.0f) + jitter;
            }
            REQUIRE(ping.process(inject.data(), readback.data(), dry.data(), ret.data(), n));
            (void)ping.takeFadeInRequest();
            if (injected != nullptr)
            {
                injected->insert(injected->end(), inject.begin(), inject.end());
            }
            position += n;
        }
        return ping.finish();
    };
    const auto run = [&runOn](std::size_t delay, float gain, float noise, const std::vector<std::size_t>& blocks,
                              std::vector<float>* injected, std::size_t settle = 0) {
        ProbePing ping(kMaxDelay, kRate);
        return runOn(ping, delay, gain, noise, blocks, injected, settle);
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

    SECTION("every ping on the same state machine starts its window from the beginning")
    {
        ProbePing ping(kMaxDelay, kRate);
        const PingResult first = runOn(ping, 300, 1.0f, 0.0f, {64}, nullptr);
        const PingResult second = runOn(ping, 1200, 1.0f, 0.0f, {64}, nullptr);
        CHECK(first.ok);
        CHECK(first.delaySamples == 300);
        CHECK(second.ok);
        CHECK(second.delaySamples == 1200);

        // 取消在注入中途：残留的回读不能混进下一次
        std::vector<float> inject(64);
        std::vector<float> dry(64);
        std::vector<float> ret(64);
        const std::vector<float> loud(64, 0.5f);
        REQUIRE(ping.start());
        for (int block = 0; block < 20; ++block)
        {
            REQUIRE(ping.process(inject.data(), loud.data(), dry.data(), ret.data(), 64));
            (void)ping.takeFadeInRequest();
        }
        ping.cancel();
        for (int block = 0; block < 2000 && ping.active(); ++block)
        {
            REQUIRE(ping.process(inject.data(), loud.data(), dry.data(), ret.data(), 64));
            (void)ping.takeFadeInRequest();
        }
        REQUIRE(ping.state() == ProbePing::State::kIdle);
        const PingResult third = runOn(ping, 700, 1.0f, 0.0f, {64}, nullptr);
        CHECK(third.ok);
        CHECK(third.delaySamples == 700);
    }

    SECTION("with a settle the injection starts that much later and the delay still comes back exactly")
    {
        for (const std::size_t settle : {std::size_t{1}, std::size_t{480}, std::size_t{8160}})
        {
            for (const std::vector<std::size_t>& blocks :
                 {std::vector<std::size_t>{64}, std::vector<std::size_t>{7, 1000, 33}})
            {
                INFO("settle " << settle << " first block " << blocks.front());
                std::vector<float> injected;
                const PingResult r = run(4800, 1.0f, 0.0f, blocks, &injected, settle);
                CHECK(r.ok);
                CHECK(r.delaySamples == 4800);
                const std::size_t lead = ProbePing(kMaxDelay, kRate).rampSamples() + settle;
                REQUIRE(injected.size() > lead + kPingBuffer.size());
                CHECK(std::all_of(injected.begin(), injected.begin() + static_cast<std::ptrdiff_t>(lead),
                                  [](float v) { return v == 0.0f; }));
                CHECK(injected[lead] == kPingBuffer[0]);
                CHECK(injected[lead + kPingBuffer.size() - 1] == kPingBuffer.back());
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

    SECTION("the injection starts after the ramp, is the 127 point sequence at minus 20 dBFS, then silence")
    {
        std::vector<float> injected;
        (void)run(10, 1.0f, 0.0f, {50}, &injected);
        const std::size_t lead = ProbePing(kMaxDelay, kRate).rampSamples();
        REQUIRE(injected.size() >= lead + kPingBuffer.size() + 1);
        for (std::size_t i = 0; i < lead; ++i)
        {
            CHECK(injected[i] == 0.0f);
        }
        float peak = 0.0f;
        for (std::size_t i = 0; i < kPingBuffer.size(); ++i)
        {
            CHECK(injected[lead + i] == kPingBuffer[i]);
            peak = std::max(peak, std::fabs(injected[lead + i]));
        }
        CHECK(20.0 * std::log10(static_cast<double>(peak)) == Catch::Approx(-20.0).margin(1e-4));
        for (std::size_t i = lead + kPingBuffer.size(); i < injected.size(); ++i)
        {
            CHECK(injected[i] == 0.0f);
        }
    }
}

TEST_CASE("the ping state machine runs once at a time", "[host][probe][ping]")
{
    ProbePing ping(200, kRate);
    std::vector<float> inject(64, 5.0f);
    std::vector<float> dry(64, 7.0f);
    std::vector<float> ret(64, 9.0f);
    const std::vector<float> readback(64, 0.0f);

    CHECK(ping.state() == ProbePing::State::kIdle);
    CHECK_FALSE(ping.active());
    CHECK_FALSE(ping.process(inject.data(), readback.data(), dry.data(), ret.data(), 64));
    CHECK(inject[0] == 5.0f); // 没在跑，三个缓冲都不动：调用方当系数为 1
    CHECK(dry[0] == 7.0f);
    CHECK(ret[0] == 9.0f);
    CHECK_FALSE(ping.takeFadeInRequest());
    CHECK_FALSE(ping.finish().ok);

    SECTION("start is refused while a ping is under way, and the audio thread takes over on the next block")
    {
        REQUIRE(ping.start());
        CHECK_FALSE(ping.start());
        CHECK(ping.active());
        CHECK(ping.state() == ProbePing::State::kArmed);
        REQUIRE(ping.process(inject.data(), readback.data(), dry.data(), ret.data(), 64));
        CHECK(ping.state() == ProbePing::State::kRunning);
        CHECK_FALSE(ping.start());
        CHECK(dry[0] < 1.0f); // 第一个样本就已经在降
        CHECK(ret[0] < 1.0f);
    }

    SECTION("a cancel before the audio thread picked it up leaves the audio alone")
    {
        REQUIRE(ping.start());
        ping.cancel();
        CHECK_FALSE(ping.process(inject.data(), readback.data(), dry.data(), ret.data(), 64));
        CHECK(ping.state() == ProbePing::State::kIdle);
        CHECK(dry[0] == 7.0f);
        CHECK_FALSE(ping.takeFadeInRequest());
        REQUIRE(ping.start()); // 取消过的状态机可以重新开始，不带旧的取消
        const PingTrace trace = drive(ping, 64);
        CHECK(ping.state() == ProbePing::State::kDone);
        CHECK(trace.requests == 1);
    }

    SECTION("a run to the end ends in Done, hands out one fade-in request, and finish returns to Idle")
    {
        REQUIRE(ping.start());
        const PingTrace trace = drive(ping, 64);
        CHECK(ping.state() == ProbePing::State::kDone);
        CHECK(trace.requests == 1);
        CHECK_FALSE(ping.takeFadeInRequest()); // 请求只有一次
        CHECK_FALSE(ping.process(inject.data(), readback.data(), dry.data(), ret.data(), 64)); // 结束后不再碰音频
        CHECK_FALSE(ping.finish().ok); // 全 0 的回读
        CHECK(ping.state() == ProbePing::State::kIdle);

        REQUIRE(ping.start()); // 再来一次，窗口从头算，请求再出现一次
        CHECK(drive(ping, 64).requests == 1);
    }

    SECTION("cancel after the run is Done throws the result away")
    {
        REQUIRE(ping.start());
        (void)drive(ping, 64);
        REQUIRE(ping.state() == ProbePing::State::kDone);
        ping.cancel();
        CHECK(ping.state() == ProbePing::State::kIdle);
        CHECK_FALSE(ping.finish().ok);
    }

    SECTION("a refused start leaves the run under way alone, and it still hands out exactly one fade-in request")
    {
        // 还没接手时再点一次、跑到静音段中途再点一次：都被拒，原来那次照常跑完，请求只有一次
        REQUIRE(ping.start(300));
        CHECK_FALSE(ping.start(300));
        REQUIRE(ping.process(inject.data(), readback.data(), dry.data(), ret.data(), 64));
        for (int i = 0; i < 10; ++i)
        {
            REQUIRE(ping.process(inject.data(), readback.data(), dry.data(), ret.data(), 64));
            CHECK_FALSE(ping.takeFadeInRequest());
        }
        CHECK_FALSE(ping.start(300));
        const PingTrace trace = drive(ping, 64);
        CHECK(ping.state() == ProbePing::State::kDone);
        CHECK(trace.requests == 1);
    }

    SECTION("reset brings back an idle state machine whatever it was doing")
    {
        REQUIRE(ping.start());
        REQUIRE(ping.process(inject.data(), readback.data(), dry.data(), ret.data(), 64));
        ping.reset();
        CHECK(ping.state() == ProbePing::State::kIdle);
        REQUIRE(ping.start());
        CHECK(drive(ping, 64).requests == 1);
    }

    SECTION("the ramp is ten milliseconds of the sample rate, at least one sample")
    {
        CHECK(ProbePing(10, 48000.0).rampSamples() == 480);
        CHECK(ProbePing(10, 44100.0).rampSamples() == 441);
        CHECK(ProbePing(10, 96000.0).rampSamples() == 960);
        CHECK(ProbePing(10, 0.0).rampSamples() == 1);
        CHECK(ProbePing(10, std::nan("")).rampSamples() == 1);
        CHECK(ProbePing(10, 10.0).rampSamples() == 1);
    }
}

TEST_CASE("the dry and return gains never step while a ping starts, runs and ends", "[host][probe][ping][ramp]")
{
    const std::size_t ramp = ProbePing(kMaxDelay, kRate).rampSamples();
    const float bound = 1.0f / static_cast<float>(ramp) * 1.001f;

    SECTION("both gains ramp down over the ramp length, then the sequence is injected")
    {
        for (const std::size_t blockSize :
             {std::size_t{1}, std::size_t{7}, std::size_t{64}, std::size_t{480}, std::size_t{1000}})
        {
            INFO("block " << blockSize);
            ProbePing ping(kMaxDelay, kRate);
            REQUIRE(ping.start());
            const PingTrace trace = drive(ping, blockSize);
            REQUIRE(trace.dry.size() >= ramp + kPingBuffer.size());

            CHECK(largestStep(trace.dry) <= bound);
            CHECK(largestStep(trace.ret) <= bound);
            CHECK(trace.dry.front() == Catch::Approx(1.0f - 1.0f / static_cast<float>(ramp)).margin(1e-6));
            CHECK(trace.dry[ramp - 1] == 0.0f);
            CHECK(trace.ret[ramp - 1] == 0.0f);
            // 降的过程中不注入；注入只发生在两个增益都是 0 的时候
            for (std::size_t i = 0; i < trace.injection.size(); ++i)
            {
                if (trace.injection[i] != 0.0f)
                {
                    INFO("sample " << i);
                    CHECK(i >= ramp);
                    CHECK(trace.dry[i] == 0.0f);
                    CHECK(trace.ret[i] == 0.0f);
                }
            }
            // 注入的整个窗口里两个增益都保持 0
            for (std::size_t i = ramp - 1; i < ramp + kMaxDelay + kPingBuffer.size(); ++i)
            {
                CHECK(trace.dry[i] == 0.0f);
                CHECK(trace.ret[i] == 0.0f);
            }
        }
    }

    SECTION("at the end the dry gain climbs back over the ramp, the return waits for the fade-in")
    {
        for (const std::size_t blockSize :
             {std::size_t{1}, std::size_t{7}, std::size_t{64}, std::size_t{480}, std::size_t{1000}})
        {
            INFO("block " << blockSize);
            ProbePing ping(kMaxDelay, kRate);
            REQUIRE(ping.start());
            const PingTrace trace = drive(ping, blockSize);
            REQUIRE(ping.state() == ProbePing::State::kDone);
            REQUIRE(trace.requests == 1);
            REQUIRE(trace.takenAfter != kNoIndex);

            const std::size_t windowEnd = ramp + kMaxDelay + kPingBuffer.size(); // 最后一个注入窗口样本之后
            // 干声：窗口结束后用一个斜坡长度回到 1，中途不跳
            CHECK(trace.dry[windowEnd - 1] == 0.0f);
            CHECK(trace.dry[windowEnd + ramp - 1] == 1.0f);
            CHECK(trace.dry.back() == 1.0f);
            for (std::size_t i = windowEnd; i < trace.dry.size(); ++i)
            {
                CHECK(trace.dry[i] >= trace.dry[i - 1]);
            }
            // 回送：降到 0 之后，请求被取走、再过一个斜坡长度之前一直是 0，然后用一个斜坡长度回到 1
            for (std::size_t i = ramp - 1; i < trace.takenAfter + ramp && i < trace.ret.size(); ++i)
            {
                INFO("sample " << i);
                CHECK(trace.ret[i] == 0.0f);
            }
            CHECK(trace.ret.back() == 1.0f);
            CHECK(trace.ret[trace.takenAfter + ramp] > 0.0f);
            CHECK(trace.ret[trace.takenAfter + 2 * ramp - 1] == 1.0f);
            CHECK(largestStep(trace.dry) <= bound);
            CHECK(largestStep(trace.ret) <= bound);
        }
    }

    SECTION("a settle keeps both gains at 0 from the end of the ramp down to the end of the injection window")
    {
        constexpr std::size_t kSettle = 3000;
        for (const std::size_t blockSize : {std::size_t{1}, std::size_t{64}, std::size_t{1000}})
        {
            INFO("block " << blockSize);
            ProbePing ping(kMaxDelay, kRate);
            REQUIRE(ping.start(kSettle));
            const PingTrace trace = drive(ping, blockSize);
            REQUIRE(ping.state() == ProbePing::State::kDone);
            CHECK(trace.requests == 1);
            const std::size_t injectStart = ramp + kSettle;
            const std::size_t windowEnd = injectStart + kMaxDelay + kPingBuffer.size();
            REQUIRE(trace.dry.size() > windowEnd + ramp);
            for (std::size_t i = ramp - 1; i < windowEnd; ++i)
            {
                INFO("sample " << i);
                CHECK(trace.dry[i] == 0.0f);
                CHECK(trace.ret[i] == 0.0f);
            }
            for (std::size_t i = 0; i < trace.injection.size(); ++i)
            {
                if (trace.injection[i] != 0.0f)
                {
                    INFO("sample " << i);
                    CHECK(i >= injectStart);
                }
            }
            CHECK(trace.injection[injectStart] == kPingBuffer[0]);
            CHECK(trace.dry[windowEnd + ramp - 1] == 1.0f);
            CHECK(trace.takenAfter >= windowEnd);
            CHECK(largestStep(trace.dry) <= bound);
            CHECK(largestStep(trace.ret) <= bound);
        }
    }

    SECTION("a cancel during the settle restores over the ramps, injects nothing and leaves no result")
    {
        constexpr std::size_t kSettle = 3000;
        for (const std::size_t cancelAfter : {ramp + 10, ramp + kSettle / 2, ramp + kSettle - 200})
        {
            for (const std::size_t blockSize : {std::size_t{1}, std::size_t{64}, std::size_t{97}})
            {
                INFO("cancel after " << cancelAfter << " block " << blockSize);
                ProbePing ping(kMaxDelay, kRate);
                REQUIRE(ping.start(kSettle));
                const PingTrace trace = drive(ping, blockSize, cancelAfter);
                CHECK(ping.state() == ProbePing::State::kIdle);
                CHECK_FALSE(ping.finish().ok);
                CHECK(trace.requests == 1);
                CHECK(trace.dry.back() == 1.0f);
                CHECK(trace.ret.back() == 1.0f);
                CHECK(largestStep(trace.dry) <= bound);
                CHECK(largestStep(trace.ret) <= bound);
                CHECK(std::all_of(trace.injection.begin(), trace.injection.end(), [](float v) { return v == 0.0f; }));
                // 静音段没走完就开始恢复：整个过程比静音段加三个斜坡短
                CHECK(trace.dry.size() < ramp + kSettle + 3 * ramp + blockSize);
            }
        }
    }

    SECTION("the fade-in request appears once, in the callback that finished the capture")
    {
        ProbePing ping(kMaxDelay, kRate);
        REQUIRE(ping.start());
        const std::size_t windowEnd = ramp + kMaxDelay + kPingBuffer.size();
        const PingTrace trace = drive(ping, 64);
        CHECK(trace.requests == 1);
        CHECK(trace.takenAfter >= windowEnd);
        CHECK(trace.takenAfter <= windowEnd + 64);
    }

    SECTION("a cancel in the middle of the ramp down, the injection or the restore still never steps")
    {
        const std::size_t windowEnd = ramp + kMaxDelay + kPingBuffer.size();
        // 降的中途、降完刚开始注入、注入中途、注入刚结束（干声刚开始回升）、回升中途、回升刚结束（回送等待中）、
        // 回送放回的中途
        for (const std::size_t cancelAfter : {ramp / 3, ramp, ramp + 100, windowEnd - 1, windowEnd + ramp / 2,
                                              windowEnd + ramp + 10, windowEnd + ramp + ramp / 2})
        {
            for (const std::size_t blockSize : {std::size_t{1}, std::size_t{64}, std::size_t{97}})
            {
                INFO("cancel after " << cancelAfter << " block " << blockSize);
                ProbePing ping(kMaxDelay, kRate);
                REQUIRE(ping.start());
                const PingTrace trace = drive(ping, blockSize, cancelAfter);

                CHECK(largestStep(trace.dry) <= bound);
                CHECK(largestStep(trace.ret) <= bound);
                CHECK(trace.dry.back() == 1.0f);
                CHECK(trace.ret.back() == 1.0f);
                CHECK(trace.requests == 1);
                CHECK(ping.state() == ProbePing::State::kIdle); // 取消了，没有结果
                CHECK_FALSE(ping.finish().ok);
                if (cancelAfter < windowEnd - 1)
                {
                    // 取消得早，回读没攒满：窗口没跑完就开始恢复
                    CHECK(trace.dry.size() < windowEnd + 3 * ramp);
                }
                // 回送从来不在请求被取走之前回升，回升也不早于取走后的一个斜坡长度
                for (std::size_t i = 1; i < trace.ret.size() && i < trace.takenAfter + ramp; ++i)
                {
                    INFO("sample " << i);
                    CHECK(trace.ret[i] <= trace.ret[i - 1]);
                }
            }
        }
    }
}

TEST_CASE("abort ends a ping at once and hands the return over with one fade-in request", "[host][probe][ping]")
{
    const std::size_t ramp = ProbePing(kMaxDelay, kRate).rampSamples();
    constexpr std::size_t kSettle = 3000;
    constexpr std::size_t kBlockSize = 64;
    std::vector<float> inject(kBlockSize, 5.0f);
    std::vector<float> dry(kBlockSize, 7.0f);
    std::vector<float> ret(kBlockSize, 9.0f);
    const std::vector<float> readback(kBlockSize, 0.0f);

    SECTION("armed: nothing was touched, so no request, and a new start runs normally")
    {
        ProbePing ping(kMaxDelay, kRate);
        REQUIRE(ping.start(kSettle));
        ping.abort();
        CHECK(ping.state() == ProbePing::State::kIdle);
        CHECK_FALSE(ping.takeFadeInRequest());
        CHECK_FALSE(ping.process(inject.data(), readback.data(), dry.data(), ret.data(), kBlockSize));
        CHECK(dry[0] == 7.0f);
        REQUIRE(ping.start(kSettle));
        CHECK(drive(ping, kBlockSize).requests == 1);
    }

    SECTION("idle or done: nothing happens, a finished result stays for finish")
    {
        ProbePing ping(kMaxDelay, kRate);
        ping.abort();
        CHECK(ping.state() == ProbePing::State::kIdle);
        REQUIRE(ping.start());
        CHECK(drive(ping, kBlockSize).requests == 1);
        REQUIRE(ping.state() == ProbePing::State::kDone);
        ping.abort();
        CHECK(ping.state() == ProbePing::State::kDone);
        CHECK_FALSE(ping.takeFadeInRequest());
        (void)ping.finish();
        CHECK(ping.state() == ProbePing::State::kIdle);
    }

    SECTION("running, in any phase: Idle at once, no result, and over the whole run exactly one request")
    {
        const std::size_t windowEnd = ramp + kSettle + kMaxDelay + kPingBuffer.size();
        // 降的中途、静音段中途、注入中途（这三处请求还没交出，abort 之后交出），干声回升中途、回送等待中、回送放回的
        // 中途（请求已经交出过，abort 不再交）
        for (const std::size_t abortAfter : {ramp / 2, ramp + kSettle / 2, ramp + kSettle + 1000, windowEnd + ramp / 2,
                                             windowEnd + ramp + 10, windowEnd + ramp + ramp / 2})
        {
            INFO("abort after " << abortAfter);
            ProbePing ping(kMaxDelay, kRate);
            REQUIRE(ping.start(kSettle));
            std::size_t samples = 0;
            std::size_t requests = 0;
            while (samples < abortAfter && ping.active())
            {
                REQUIRE(ping.process(inject.data(), readback.data(), dry.data(), ret.data(), kBlockSize));
                samples += kBlockSize;
                if (ping.takeFadeInRequest())
                {
                    ++requests;
                }
            }
            REQUIRE(ping.active());
            ping.abort();
            CHECK(ping.state() == ProbePing::State::kIdle);
            if (ping.takeFadeInRequest())
            {
                ++requests;
            }
            CHECK(requests == 1);
            CHECK_FALSE(ping.takeFadeInRequest());
            CHECK_FALSE(ping.finish().ok);
            CHECK_FALSE(ping.process(inject.data(), readback.data(), dry.data(), ret.data(), kBlockSize));
            // 下一次从头来过，请求再出现一次
            REQUIRE(ping.start());
            CHECK(drive(ping, kBlockSize).requests == 1);
        }
    }
}

TEST_CASE("a zero-sample callback moves the ping state machine nowhere", "[host][probe][ping]")
{
    // 0 样本的回调：返回 false、状态不变，也不交出淡入请求
    const auto empty = [](ProbePing& ping) {
        const ProbePing::State before = ping.state();
        float sink = 3.0f;
        CHECK_FALSE(ping.process(&sink, &sink, &sink, &sink, 0));
        CHECK_FALSE(ping.takeFadeInRequest());
        CHECK(ping.state() == before);
    };

    SECTION("armed: the start is not taken over and a pending cancel stays pending")
    {
        ProbePing ping(200, kRate);
        REQUIRE(ping.start(100));
        empty(ping);
        CHECK(ping.state() == ProbePing::State::kArmed);
        ping.cancel();
        empty(ping);
        CHECK(ping.state() == ProbePing::State::kArmed);
        std::vector<float> buffer(64, 0.0f);
        CHECK_FALSE(ping.process(buffer.data(), buffer.data(), buffer.data(), buffer.data(), 64));
        CHECK(ping.state() == ProbePing::State::kIdle); // 下一个真块才取走取消
    }

    SECTION("running, settling or with a cancel pending: sample for sample the same run as without the empty calls")
    {
        const std::size_t ramp = ProbePing(kMaxDelay, kRate).rampSamples();
        constexpr std::size_t kSettle = 300;
        constexpr std::size_t kBlockSize = 64;
        // drive 的翻版：每个真块之前先来一次 0 样本的回调；请求取消之后紧跟一次
        const auto driveWithEmpty = [&](ProbePing& ping, std::size_t cancelAfter) {
            PingTrace trace;
            bool cancelled = false;
            std::vector<float> inject(kBlockSize);
            std::vector<float> dry(kBlockSize);
            std::vector<float> ret(kBlockSize);
            const std::vector<float> readback(kBlockSize, 0.0f);
            empty(ping);
            while ((ping.active() || trace.dry.empty()) && trace.dry.size() < 60000)
            {
                if (!cancelled && cancelAfter != kNoIndex && trace.dry.size() >= cancelAfter)
                {
                    ping.cancel();
                    cancelled = true;
                    empty(ping);
                }
                empty(ping);
                REQUIRE(ping.process(inject.data(), readback.data(), dry.data(), ret.data(), kBlockSize));
                trace.injection.insert(trace.injection.end(), inject.begin(), inject.end());
                trace.dry.insert(trace.dry.end(), dry.begin(), dry.end());
                trace.ret.insert(trace.ret.end(), ret.begin(), ret.end());
                if (ping.takeFadeInRequest())
                {
                    ++trace.requests;
                    trace.takenAfter = trace.dry.size();
                }
            }
            return trace;
        };
        // 不取消、降的中途、静音段中途、注入中途
        for (const std::size_t cancelAfter : {kNoIndex, ramp / 2, ramp + kSettle / 2, ramp + kSettle + 1000})
        {
            INFO("cancel after " << cancelAfter);
            ProbePing plain(kMaxDelay, kRate);
            ProbePing withEmpty(kMaxDelay, kRate);
            REQUIRE(plain.start(kSettle));
            REQUIRE(withEmpty.start(kSettle));
            const PingTrace expected = drive(plain, kBlockSize, cancelAfter);
            const PingTrace actual = driveWithEmpty(withEmpty, cancelAfter);
            CHECK(actual.injection == expected.injection);
            CHECK(actual.dry == expected.dry);
            CHECK(actual.ret == expected.ret);
            CHECK(actual.requests == expected.requests);
            CHECK(actual.takenAfter == expected.takenAfter);
            CHECK(withEmpty.state() == plain.state());
        }
    }
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
        REQUIRE(decodeState(bytes.data(), bytes.size(), state::Role::Input, back) == StateStatus::kOk);
        CHECK(back.group == 3);
        CHECK(back.feedbackPercent == 40.0f);
        CHECK(back.delayMs == 250.0f);
        CHECK(back.ceilingDb == -12.0f);

        ProbeStateData unset;
        unset.group = -1;
        const std::vector<std::uint8_t> unsetBytes = encodeState(unset, state::Role::Output);
        REQUIRE(decodeState(unsetBytes.data(), unsetBytes.size(), state::Role::Output, back) == StateStatus::kOk);
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
            CHECK(decodeState(bytes.data(), size, state::Role::Input, out) == StateStatus::kCorrupt);
            CHECK(out.group == 6);
            CHECK(out.feedbackPercent == 12.0f);
        }
        ProbeStateData out;
        CHECK(decodeState(bytes.data(), 0, state::Role::Input, out) == StateStatus::kEmpty);
        CHECK(decodeState(nullptr, 0, state::Role::Input, out) == StateStatus::kEmpty);

        std::vector<std::uint8_t> longer = bytes;
        longer.push_back(0);
        CHECK(decodeState(longer.data(), longer.size(), state::Role::Input, out) == StateStatus::kCorrupt);
    }

    SECTION("bytes a formal build wrote are not taken over")
    {
        state::Container container;
        container.role = state::Role::Input;
        std::vector<std::uint8_t> formal;
        REQUIRE(state::encodeContainer(container, formal));
        ProbeStateData out;
        CHECK(decodeState(formal.data(), formal.size(), state::Role::Input, out) == StateStatus::kNotProbe);
    }

    SECTION("bytes the other plugin saved are refused, probe bytes and formal bytes alike")
    {
        ProbeStateData untouched;
        untouched.group = 5;
        untouched.feedbackPercent = 33.0f;
        ProbeStateData out = untouched;

        CHECK(decodeState(bytes.data(), bytes.size(), state::Role::Output, out) == StateStatus::kOtherRole);

        state::Container container;
        container.role = state::Role::Output;
        std::vector<std::uint8_t> formalOutput;
        REQUIRE(state::encodeContainer(container, formalOutput));
        CHECK(decodeState(formalOutput.data(), formalOutput.size(), state::Role::Input, out) ==
              StateStatus::kOtherRole);

        std::vector<std::uint8_t> unknownRole = bytes;
        unknownRole[8] = 9; // 头里偏移 8 是 role（u32 小端）
        CHECK(decodeState(unknownRole.data(), unknownRole.size(), state::Role::Input, out) == StateStatus::kCorrupt);

        CHECK(out.group == 5);
        CHECK(out.feedbackPercent == 33.0f);
    }

    SECTION("damaged fields are refused, out of range numbers are pulled in")
    {
        ProbeStateData out;

        std::vector<std::uint8_t> badMagic = bytes;
        badMagic[0] = 'X';
        CHECK(decodeState(badMagic.data(), badMagic.size(), state::Role::Input, out) == StateStatus::kCorrupt);

        std::vector<std::uint8_t> badChunk = bytes;
        badChunk[16] = 'Q';
        CHECK(decodeState(badChunk.data(), badChunk.size(), state::Role::Input, out) == StateStatus::kCorrupt);

        std::vector<std::uint8_t> badGroup = bytes;
        badGroup[24] = 9;
        CHECK(decodeState(badGroup.data(), badGroup.size(), state::Role::Input, out) == StateStatus::kCorrupt);

        std::vector<std::uint8_t> nan = bytes;
        patchFloat(nan, 28, std::nanf(""));
        CHECK(decodeState(nan.data(), nan.size(), state::Role::Input, out) == StateStatus::kCorrupt);

        std::vector<std::uint8_t> loud = bytes;
        patchFloat(loud, 28, 120.0f);
        patchFloat(loud, 32, 90000.0f);
        patchFloat(loud, 36, 5.0f);
        REQUIRE(decodeState(loud.data(), loud.size(), state::Role::Input, out) == StateStatus::kOk);
        CHECK(out.feedbackPercent == 80.0f);
        CHECK(out.delayMs == 4000.0f);
        CHECK(out.ceilingDb == 0.0f);
    }
}

TEST_CASE("formal state bytes are kept as they came and written back unchanged", "[host][probe][state]")
{
    // 正式版存的字节：块的内容不重要，要紧的是整份逐字节不变
    state::Container container;
    container.role = state::Role::Input;
    container.set(state::makeFourCc('F', 'A', 'K', 'E'), {1, 2, 3, 4, 5, 6, 7});
    container.set(state::makeFourCc('M', 'O', 'R', 'E'), {9, 9});
    std::vector<std::uint8_t> formal;
    REQUIRE(state::encodeContainer(container, formal));

    ProbeStateData panel;
    panel.group = 4;
    panel.feedbackPercent = 25.0f;
    panel.delayMs = 120.0f;

    ProbeStateKeeper keeper(state::Role::Input);
    CHECK_FALSE(keeper.holding());
    CHECK(keeper.save(panel) == encodeState(panel, state::Role::Input)); // 什么都没读进来：存探针自己的

    SECTION("a formal state comes in, the panel settings stay put, the same bytes go out")
    {
        ProbeStateData loaded = panel;
        REQUIRE(keeper.load(formal.data(), formal.size(), loaded) == StateStatus::kNotProbe);
        CHECK(loaded.group == 4); // 探针不解析正式字节
        CHECK(loaded.feedbackPercent == 25.0f);
        CHECK(keeper.holding());
        CHECK(keeper.save(panel) == formal); // 逐字节相同
        panel.group = 1; // 留住期间面板上的改动不进 state
        CHECK(keeper.save(panel) == formal);
    }

    SECTION("a higher abi of this plugin's own role is kept the same way")
    {
        std::vector<std::uint8_t> newer = formal;
        newer[4] = 2; // 头里偏移 4 是 abi（u32 小端）
        ProbeStateData loaded = panel;
        REQUIRE(keeper.load(newer.data(), newer.size(), loaded) == StateStatus::kNotProbe);
        CHECK(loaded.group == 4);
        CHECK(keeper.holding());
        CHECK(keeper.save(panel) == newer);
    }

    SECTION("a later probe state of its own lets go of the formal bytes")
    {
        ProbeStateData loaded;
        REQUIRE(keeper.load(formal.data(), formal.size(), loaded) == StateStatus::kNotProbe);
        const std::vector<std::uint8_t> own = encodeState(panel, state::Role::Input);
        REQUIRE(keeper.load(own.data(), own.size(), loaded) == StateStatus::kOk);
        CHECK_FALSE(keeper.holding());
        CHECK(keeper.save(loaded) == own);
    }

    SECTION("an empty, damaged or other-role state changes nothing")
    {
        ProbeStateData loaded;
        REQUIRE(keeper.load(formal.data(), formal.size(), loaded) == StateStatus::kNotProbe);

        CHECK(keeper.load(nullptr, 0, loaded) == StateStatus::kEmpty);
        std::vector<std::uint8_t> damaged = formal;
        damaged[0] = 'X';
        CHECK(keeper.load(damaged.data(), damaged.size(), loaded) == StateStatus::kCorrupt);
        const std::vector<std::uint8_t> theirs = encodeState(panel, state::Role::Output);
        CHECK(keeper.load(theirs.data(), theirs.size(), loaded) == StateStatus::kOtherRole);
        CHECK(keeper.holding());
        CHECK(keeper.save(panel) == formal);

        ProbeStateKeeper fresh(state::Role::Input);
        CHECK(fresh.load(damaged.data(), damaged.size(), loaded) == StateStatus::kCorrupt);
        CHECK(fresh.load(theirs.data(), theirs.size(), loaded) == StateStatus::kOtherRole);
        CHECK_FALSE(fresh.holding());
    }

    SECTION("formal bytes of the other plugin are not kept")
    {
        state::Container other;
        other.role = state::Role::Output;
        std::vector<std::uint8_t> formalOutput;
        REQUIRE(state::encodeContainer(other, formalOutput));
        ProbeStateData loaded;
        CHECK(keeper.load(formalOutput.data(), formalOutput.size(), loaded) == StateStatus::kOtherRole);
        CHECK_FALSE(keeper.holding());
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
    status.logOpen = true;
    status.recording = true;
    status.lastPingSamples = 4800;
    status.counters.blocks = 1234;
    status.counters.dropped = 2;
    status.ceilingDb = -18.0f;

    ProbeSelection selection;
    selection.buf = 256;
    selection.guard = diag::ProbeGuard::kLow;

    const ProbeLogDir logDir = resolveLogDir("D:\\accounts\\alice\\Documents");

    SECTION("the base report comes first, then the probe lines")
    {
        const std::string text = buildDiagText(base, status, logDir, selection);
        CHECK(text.rfind("rinne-diag v1\n", 0) == 0);
        CHECK(contains(text, "probe: log_dir=default log_open=1 recording=1 feedback_cap=80 ceiling_db=-18.0\n"));
        CHECK(contains(text, "probe_case: case=CB-01 variant=steady buf=256 guard=low\n"));
        CHECK(contains(text, "probe_ping: samples=4800\n"));
        CHECK(contains(text, "probe_log: blocks=1234 dropped=2 max_n=0 resync=0 trips=0\n"));
        CHECK(text.find(diag::buildDiagReport(base)) == 0);
        CHECK(text.back() == '\n');
    }

    SECTION("the log folder shows as default, with no piece of its path in the text")
    {
        const std::string text = buildDiagText(base, status, logDir, selection);
        CHECK(contains(text, "probe: log_dir=default "));
        for (const char* leak : {"alice", "accounts", "D:", "\\", "Documents", "Rinne Probe Logs"})
        {
            INFO("leaked " << leak);
            CHECK_FALSE(contains(text, leak));
        }

        const std::string unixText = buildDiagText(base, status, resolveLogDir("/srv/bob/Documents"), selection);
        CHECK(contains(unixText, "probe: log_dir=default "));
        for (const char* leak : {"bob", "srv", "/", "Documents", "Rinne Probe Logs"})
        {
            INFO("leaked " << leak);
            CHECK_FALSE(contains(unixText, leak));
        }
    }

    SECTION("no documents folder reads as none")
    {
        CHECK(contains(buildDiagText(base, status, resolveLogDir(""), selection), "probe: log_dir=none "));
    }

    SECTION("a log that never opened is reported")
    {
        status.logOpen = false;
        status.recording = false;
        CHECK(
            contains(buildDiagText(base, status, logDir, selection), "probe: log_dir=default log_open=0 recording=0 "));
    }

    SECTION("no ping yet reads as none")
    {
        status.lastPingSamples = -1;
        CHECK(contains(buildDiagText(base, status, logDir, selection), "probe_ping: samples=none\n"));
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
    CHECK(contains(text, "state: saved with the project"));
    CHECK(contains(countersText(status), "blocks 99"));
    CHECK(contains(countersText(status), "feedback 15.0 %"));

    ProbeStatus idle;
    idle.logOpen = true;
    CHECK(contains(statusText(idle), "link: not paired (group -)"));
    CHECK(contains(statusText(idle), "record: idle\n"));
    CHECK(contains(statusText(idle), "ping: idle"));

    // 记录行的优先级：正在记录 > 日志没开 > 上次被拒 > 空闲
    CHECK(contains(statusText(idle, true), "record: idle, last start was refused (try again)\n"));
    ProbeStatus closed;
    CHECK(contains(statusText(closed), "record: unavailable, log file not open\n"));
    CHECK(contains(statusText(closed, true), "record: unavailable, log file not open\n"));
    ProbeStatus busy = idle;
    busy.recording = true;
    busy.recordRemainingSeconds = 7;
    CHECK(contains(statusText(busy, true), "record: recording, 7 s left\n"));
    busy.logOpen = false;
    CHECK(contains(statusText(busy), "record: recording, 7 s left\n"));
    idle.pingRunning = true;
    idle.tripped = true;
    CHECK(contains(statusText(idle), "ping: running"));
    CHECK(contains(statusText(idle), "safety: tripped"));

    // 正式版的字节留着：面板上的改动不会随工程保存，状态文本要明说
    ProbeStatus held;
    held.stateHeld = true;
    CHECK(contains(statusText(held), "\nstate: release-build state kept, panel changes are not saved"));
    CHECK_FALSE(contains(statusText(held), "saved with the project"));

    // 旁路着：走带行后面注明，Ping 为什么点不了一看就知道
    ProbeStatus bypassed;
    bypassed.bypassed = true;
    CHECK(contains(statusText(bypassed), "transport: stopped, dry: active, bypassed\n"));
    CHECK_FALSE(contains(statusText(idle), "bypassed"));
}
