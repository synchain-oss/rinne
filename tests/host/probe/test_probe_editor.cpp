// SPDX-License-Identifier: GPL-3.0-or-later
//
// 探针面板：用假的处理器接口驱动，不开窗口、不起消息循环。按钮的可用规则、恢复冷却、选择器写进 C 行的字段、
// feedback 上限、复制诊断。

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "InputProcessor.h"
#include "ProbeEditor.h"
#include "ipc/ProcessInfo.h"

namespace
{

using namespace rinne::probe;
namespace diag = rinne::diag;
namespace fs = std::filesystem;

// 假处理器。log 不为空时，日志是否开着、是否在记录、开始记录能不能成，都由这个真的 ProbeLog 说了算
class FakeHost final : public ProbeEditorHost
{
public:
    FakeHost() { status.logOpen = true; }

    ProbeStatus status;
    diag::ProbeLog* log = nullptr;
    bool startResult = true;
    int panics = 0;
    int recovers = 0;
    int pings = 0;
    std::vector<ProbeSelection> recordings;
    std::vector<std::uint32_t> recordingSeconds;
    std::vector<int> groups;
    std::vector<float> feedbacks;
    std::vector<float> delays;
    std::string diagnostics = "diagnostic text";

    ProbeStatus probeStatus() const override
    {
        ProbeStatus out = status;
        if (log != nullptr)
        {
            out.logOpen = log->isOpen();
            out.recording = log->isRecording();
        }
        return out;
    }
    void probePanic() override { ++panics; }
    void probeRecover() override { ++recovers; }
    void probePing() override { ++pings; }
    bool probeStartRecording(const ProbeSelection& selection, std::uint32_t seconds) override
    {
        recordings.push_back(selection);
        recordingSeconds.push_back(seconds);
        return log != nullptr ? log->beginCase(toProbeCase(selection, 48000)) : startResult;
    }
    void probeSetGroup(int group) override
    {
        groups.push_back(group);
        status.group = group;
    }
    void probeSetFeedbackPercent(float percent) override
    {
        feedbacks.push_back(percent);
        status.feedbackPercent = percent;
    }
    void probeSetDelayMs(float ms) override
    {
        delays.push_back(ms);
        status.delayMs = ms;
    }
    std::string probeDiagnosticsText() override { return diagnostics; }
};

struct Panel
{
    rinne::InputProcessor processor;
    FakeHost host;
    std::uint64_t nowMs = 10000;
    std::string clipboard;
    int clipboardWrites = 0;
    ProbeEditor editor;

    Panel()
        : editor(processor, host,
                 ProbeEditorOptions{[this] { return nowMs; },
                                    [this](const std::string& text) {
                                        clipboard = text;
                                        ++clipboardWrites;
                                    }})
    {
    }

    ProbeEditor::Controls& c() { return editor.controls(); }
};

// 系统临时目录下本进程独占的一个目录；用完只删里面的普通文件和这个空目录，不做递归删除
class ScratchDir
{
public:
    ScratchDir()
    {
        static std::atomic<unsigned> counter{0};
        std::error_code ec;
        const fs::path base = fs::temp_directory_path(ec);
        for (int attempt = 0; attempt < 100 && path_.empty(); ++attempt)
        {
            const fs::path candidate =
                base / ("rinne-probe-panel-" + std::to_string(rinne::ipc::currentProcessId()) + "-" +
                        std::to_string(counter.fetch_add(1)) + "-" + std::to_string(diag::probeMonoNs() % 1000000007u));
            if (fs::create_directory(candidate, ec))
            {
                path_ = candidate;
            }
        }
    }
    ~ScratchDir()
    {
        if (path_.empty())
        {
            return;
        }
        std::error_code ec;
        std::vector<fs::path> files;
        for (fs::directory_iterator it(path_, ec), end; !ec && it != end; it.increment(ec))
        {
            files.push_back(it->path());
        }
        for (const fs::path& file : files)
        {
            fs::remove(file, ec);
        }
        fs::remove(path_, ec);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;

    std::string utf8() const { return path_.u8string(); }

private:
    fs::path path_;
};

// 手动 pump 的探针日志，directory 为空就是打不开的日志
diag::ProbeLogOptions manualLogOptions(const std::string& directory, std::size_t pendingLimit = 4096)
{
    diag::ProbeLogOptions options;
    options.directory = directory;
    options.header =
        diag::makeProbeHeader(diag::ProbeRole::kInput, diag::ProbeFormat::kVst3, diag::ProbeHost::kCubase, "89abcdef");
    options.blockRingSlots = 64;
    options.pendingLimit = pendingLimit;
    options.startThread = false;
    return options;
}

int idOf(juce::ComboBox& box, const juce::String& text)
{
    for (int i = 0; i < box.getNumItems(); ++i)
    {
        if (box.getItemText(i) == text)
        {
            return box.getItemId(i);
        }
    }
    return 0;
}

} // namespace

TEST_CASE("the probe panel has the five buttons and the four selectors", "[host][probe][editor]")
{
    Panel panel;
    auto& c = panel.c();

    CHECK(c.panic.getButtonText() == "Panic");
    CHECK(c.recover.getButtonText() == "Recover");
    CHECK(c.ping.getButtonText() == "Ping");
    CHECK(c.record.getButtonText() == "Record 60 s");
    CHECK(c.copyDiagnostics.getButtonText() == "Copy diagnostics");

    CHECK(c.group.getNumItems() == 9); // 未设加 A 到 H
    CHECK(c.caseId.getNumItems() == static_cast<int>(caseCount()));
    CHECK(c.buffer.getNumItems() == static_cast<int>(kBufferChoices.size()));
    CHECK(c.guard.getNumItems() == static_cast<int>(kGuardChoices.size()));

    // 开机的选择就是一个合规则的格
    CHECK(isValidSelection(panel.editor.selection()));
    CHECK(panel.editor.getWidth() > 0);
    CHECK_FALSE(panel.editor.isResizable());
}

TEST_CASE("ping can be clicked only with the transport stopped or the dry signal silent", "[host][probe][editor]")
{
    Panel panel;

    SECTION("playing with an audible dry signal")
    {
        panel.host.status.transportPlaying = true;
        panel.host.status.drySilent = false;
        panel.editor.refresh();
        CHECK_FALSE(panel.c().ping.isEnabled());
        panel.editor.doPing();
        panel.c().ping.onClick();
        CHECK(panel.host.pings == 0);
    }

    SECTION("playing but the dry signal is silent")
    {
        panel.host.status.transportPlaying = true;
        panel.host.status.drySilent = true;
        panel.editor.refresh();
        CHECK(panel.c().ping.isEnabled());
        panel.c().ping.onClick();
        CHECK(panel.host.pings == 1);
    }

    SECTION("stopped")
    {
        panel.host.status.transportPlaying = false;
        panel.editor.refresh();
        CHECK(panel.c().ping.isEnabled());
        panel.editor.doPing();
        CHECK(panel.host.pings == 1);
    }

    SECTION("a ping already running blocks another")
    {
        panel.host.status.pingRunning = true;
        panel.editor.refresh();
        CHECK_FALSE(panel.c().ping.isEnabled());
        panel.editor.doPing();
        CHECK(panel.host.pings == 0);
    }
}

TEST_CASE("recover is blocked for 1.5 s after panic", "[host][probe][editor]")
{
    Panel panel;
    panel.editor.refresh();
    CHECK(panel.c().panic.isEnabled());
    CHECK(panel.c().recover.isEnabled());

    panel.c().panic.onClick();
    CHECK(panel.host.panics == 1);
    CHECK(panel.c().panic.isEnabled()); // Panic 随时可点，连点也只是再切一次
    CHECK_FALSE(panel.c().recover.isEnabled());

    panel.nowMs += 1499;
    panel.editor.refresh();
    CHECK_FALSE(panel.c().recover.isEnabled());
    panel.c().recover.onClick(); // 按钮灰着也按不出去
    panel.editor.doRecover();
    CHECK(panel.host.recovers == 0);

    panel.nowMs += 1;
    panel.editor.refresh();
    CHECK(panel.c().recover.isEnabled());
    panel.c().recover.onClick();
    CHECK(panel.host.recovers == 1);

    // 再 Panic 一次，冷却重新计
    panel.c().panic.onClick();
    panel.nowMs += 100;
    panel.editor.refresh();
    CHECK_FALSE(panel.c().recover.isEnabled());
    CHECK(panel.host.panics == 2);
}

TEST_CASE("the four selectors go to the C row when recording starts", "[host][probe][editor]")
{
    Panel panel;
    auto& c = panel.c();

    SECTION("the chosen cell reaches the host with the 60 s window")
    {
        c.caseId.setSelectedId(idOf(c.caseId, "CB-01"), juce::sendNotificationSync);
        c.buffer.setSelectedId(idOf(c.buffer, "256"), juce::sendNotificationSync);
        c.guard.setSelectedId(idOf(c.guard, "low"), juce::sendNotificationSync);
        panel.editor.refresh();
        REQUIRE(c.record.isEnabled());
        c.record.onClick();

        REQUIRE(panel.host.recordings.size() == 1);
        CHECK(panel.host.recordingSeconds[0] == 60);
        std::string row;
        REQUIRE(diag::appendProbeCaseRow(row, 5, toProbeCase(panel.host.recordings[0], 48000)));
        CHECK(row == "C,5,CB-01,steady,256,low,48000\n");
    }

    SECTION("changing the case offers only the variants the rule table allows for it")
    {
        c.caseId.setSelectedId(idOf(c.caseId, "CB-04"), juce::sendNotificationSync);
        CHECK(c.variant.getNumItems() == static_cast<int>(variantsFor("CB-04").size()));
        c.variant.setSelectedId(idOf(c.variant, "rip"), juce::sendNotificationSync);
        c.buffer.setSelectedId(idOf(c.buffer, "-"), juce::sendNotificationSync);
        c.guard.setSelectedId(idOf(c.guard, "na"), juce::sendNotificationSync);
        panel.editor.doRecord();

        REQUIRE(panel.host.recordings.size() == 1);
        std::string row;
        REQUIRE(diag::appendProbeCaseRow(row, 9, toProbeCase(panel.host.recordings[0], 44100)));
        CHECK(row == "C,9,CB-04,rip,-,na,44100\n");

        c.caseId.setSelectedId(idOf(c.caseId, "LU-02"), juce::sendNotificationSync);
        CHECK(c.variant.getNumItems() == 3);
        CHECK(isValidSelection(panel.editor.selection()));
    }

    SECTION("while a window is open the record button is off")
    {
        panel.host.status.recording = true;
        panel.editor.refresh();
        CHECK_FALSE(c.record.isEnabled());
        panel.editor.doRecord();
        CHECK(panel.host.recordings.empty());
    }
}

TEST_CASE("feedback stops at 80 percent and starts at zero", "[host][probe][editor]")
{
    Panel panel;
    auto& c = panel.c();

    CHECK(c.feedback.getMinimum() == 0.0);
    CHECK(c.feedback.getMaximum() == 80.0);
    CHECK(c.feedback.getValue() == 0.0);

    c.feedback.setValue(120.0, juce::sendNotificationSync);
    REQUIRE_FALSE(panel.host.feedbacks.empty());
    CHECK(panel.host.feedbacks.back() == 80.0f);
    c.feedback.setValue(35.0, juce::sendNotificationSync);
    CHECK(panel.host.feedbacks.back() == 35.0f);

    // 处理器自己报出超限的值（不该发生），面板也不显示超过 80
    panel.host.status.feedbackPercent = 150.0f;
    panel.editor.refresh();
    CHECK(c.feedback.getValue() == 80.0);
}

TEST_CASE("the delay slider covers the engine range and reports milliseconds", "[host][probe][editor]")
{
    Panel panel;
    auto& c = panel.c();
    CHECK(c.delay.getMinimum() == 1.0);
    CHECK(c.delay.getMaximum() == 4000.0);
    c.delay.setValue(200.0, juce::sendNotificationSync);
    REQUIRE_FALSE(panel.host.delays.empty());
    CHECK(panel.host.delays.back() == 200.0f);
}

TEST_CASE("the group selector and the host stay in step", "[host][probe][editor]")
{
    Panel panel;
    auto& c = panel.c();

    c.group.setSelectedId(4, juce::sendNotificationSync); // A 是 2，C 是 4
    REQUIRE_FALSE(panel.host.groups.empty());
    CHECK(panel.host.groups.back() == 2);
    c.group.setSelectedId(1, juce::sendNotificationSync);
    CHECK(panel.host.groups.back() == -1);

    // 工程恢复改了组，面板跟着变，但不把值又发回去
    const std::size_t sent = panel.host.groups.size();
    panel.host.status.group = 5;
    panel.editor.refresh();
    CHECK(c.group.getSelectedId() == 7);
    CHECK(panel.host.groups.size() == sent);
}

TEST_CASE("copy diagnostics puts the host text on the clipboard", "[host][probe][editor]")
{
    Panel panel;
    panel.host.diagnostics = "rinne-diag v1\nprobe: log_dir=default\n";
    panel.c().copyDiagnostics.onClick();
    CHECK(panel.clipboardWrites == 1);
    CHECK(panel.clipboard == "rinne-diag v1\nprobe: log_dir=default\n");
}

TEST_CASE("the status and counter labels follow the host", "[host][probe][editor]")
{
    Panel panel;
    panel.host.status.paired = true;
    panel.host.status.group = 0;
    panel.host.status.counters.blocks = 777;
    panel.editor.refresh();
    CHECK(panel.c().status.getText().contains("link: paired (group A)"));
    CHECK(panel.c().counters.getText().contains("blocks 777"));
}

TEST_CASE("a refused start is written in the status text and forgotten once a window opens", "[host][probe][editor]")
{
    Panel panel;
    auto& c = panel.c();
    panel.host.startResult = false;
    panel.editor.refresh();
    REQUIRE(c.record.isEnabled());

    c.record.onClick();
    REQUIRE(panel.host.recordings.size() == 1);
    CHECK(c.status.getText().contains("record: idle, last start was refused (try again)"));
    CHECK(c.record.isEnabled()); // 稍后可以再点

    // 再点一次成功：提示立刻消失，不用等处理器报告「正在记录」
    panel.host.startResult = true;
    c.record.onClick();
    REQUIRE(panel.host.recordings.size() == 2);
    CHECK_FALSE(c.status.getText().contains("refused"));

    // 被拒之后窗口由别处开起来：记录行显示正在记录，窗口结束后旧的拒绝提示不会回来
    panel.host.startResult = false;
    c.record.onClick();
    CHECK(c.status.getText().contains("last start was refused"));
    panel.host.status.recording = true;
    panel.host.status.recordRemainingSeconds = 30;
    panel.editor.refresh();
    CHECK(c.status.getText().contains("record: recording, 30 s left"));
    panel.host.status.recording = false;
    panel.editor.refresh();
    CHECK(c.status.getText().contains("record: idle"));
    CHECK_FALSE(c.status.getText().contains("refused"));
}

TEST_CASE("without an open log file the record button is off and the panel says why", "[host][probe][editor]")
{
    Panel panel;
    auto& c = panel.c();
    panel.host.status.logOpen = false;
    panel.editor.refresh();

    CHECK_FALSE(c.record.isEnabled());
    CHECK(c.status.getText().contains("record: unavailable, log file not open"));
    c.record.onClick(); // 按钮灰着也不会把请求交给处理器
    panel.editor.doRecord();
    CHECK(panel.host.recordings.empty());
    CHECK(c.status.getText().contains("record: unavailable, log file not open"));

    // 其余按钮不受影响
    CHECK(c.panic.isEnabled());
    CHECK(c.copyDiagnostics.isEnabled());
    c.panic.onClick();
    CHECK(panel.host.panics == 1);

    panel.host.status.logOpen = true;
    panel.editor.refresh();
    CHECK(c.record.isEnabled());
    CHECK(c.status.getText().contains("record: idle"));
}

TEST_CASE("with a real log: one that never opened keeps the panel from recording", "[host][probe][editor]")
{
    diag::ProbeLog log(manualLogOptions(""));
    REQUIRE_FALSE(log.isOpen());
    Panel panel;
    panel.host.log = &log;
    panel.editor.refresh();

    CHECK_FALSE(panel.c().record.isEnabled());
    CHECK(panel.c().status.getText().contains("record: unavailable, log file not open"));
    panel.c().record.onClick();
    panel.editor.doRecord();
    CHECK(panel.host.recordings.empty());
    CHECK_FALSE(log.isRecording());
    CHECK(log.counters().blocks == 0);
}

TEST_CASE("with a real log: a full queue refuses the start, the panel says so, and a retry works",
          "[host][probe][editor]")
{
    ScratchDir dir;
    diag::ProbeLog log(manualLogOptions(dir.utf8(), 3));
    REQUIRE(log.isOpen());
    Panel panel;
    panel.host.log = &log;
    panel.editor.refresh();
    REQUIRE(panel.c().record.isEnabled());

    // 排队 2 行，C 行加 track_props 快照还要 2 行，超过上限 3
    log.logEvent(diag::ProbeEventCode::kReset);
    log.logEvent(diag::ProbeEventCode::kReset);
    panel.c().record.onClick();
    REQUIRE(panel.host.recordings.size() == 1);
    CHECK_FALSE(log.isRecording());
    CHECK(panel.c().status.getText().contains("record: idle, last start was refused (try again)"));
    CHECK(panel.c().record.isEnabled());

    log.pump(); // 腾空队列后再点
    panel.c().record.onClick();
    REQUIRE(panel.host.recordings.size() == 2);
    CHECK(log.isRecording());
    CHECK_FALSE(panel.c().status.getText().contains("refused"));
    panel.editor.refresh();
    CHECK_FALSE(panel.c().record.isEnabled()); // 窗口开着，记录按钮灰掉
    log.close();
}
