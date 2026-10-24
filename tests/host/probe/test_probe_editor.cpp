// SPDX-License-Identifier: GPL-3.0-or-later
//
// 探针面板：用假的处理器接口驱动，不开窗口、不起消息循环。按钮的可用规则、恢复冷却、选择器写进 C 行的字段、
// feedback 上限、复制诊断。

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "InputProcessor.h"
#include "ProbeEditor.h"

namespace
{

using namespace rinne::probe;
namespace diag = rinne::diag;

class FakeHost final : public ProbeEditorHost
{
public:
    ProbeStatus status;
    int panics = 0;
    int recovers = 0;
    int pings = 0;
    std::vector<ProbeSelection> recordings;
    std::vector<std::uint32_t> recordingSeconds;
    std::vector<int> groups;
    std::vector<float> feedbacks;
    std::vector<float> delays;
    std::string diagnostics = "diagnostic text";

    ProbeStatus probeStatus() const override { return status; }
    void probePanic() override { ++panics; }
    void probeRecover() override { ++recovers; }
    void probePing() override { ++pings; }
    bool probeStartRecording(const ProbeSelection& selection, std::uint32_t seconds) override
    {
        recordings.push_back(selection);
        recordingSeconds.push_back(seconds);
        return true;
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
