// SPDX-License-Identifier: GPL-3.0-or-later

#include "ProbeEditor.h"

#include <algorithm>
#include <initializer_list>
#include <vector>

#include "util/MonoClock.h"

namespace rinne::probe
{

namespace
{

constexpr int kWidth = 520;
constexpr int kHeight = 420;
constexpr int kRefreshMs = 100;

juce::String text(const std::string& s)
{
    return juce::String::fromUTF8(s.data(), static_cast<int>(s.size()));
}

// 把一行切成 n 列，列间留 8 像素
std::vector<juce::Rectangle<int>> splitColumns(juce::Rectangle<int> row, int n)
{
    const int width = (row.getWidth() - (n - 1) * 8) / n;
    std::vector<juce::Rectangle<int>> columns;
    for (int i = 0; i < n; ++i)
    {
        columns.push_back(row.removeFromLeft(width));
        row.removeFromLeft(8);
    }
    return columns;
}

} // namespace

ProbeEditor::ProbeEditor(juce::AudioProcessor& processor, ProbeEditorHost& host, ProbeEditorOptions options)
    : juce::AudioProcessorEditor(processor), host_(host), options_(std::move(options))
{
    if (!options_.nowMs)
    {
        options_.nowMs = [] { return util::monoNowMs(); };
    }
    if (!options_.copyToClipboard)
    {
        options_.copyToClipboard = [](const std::string& s) { juce::SystemClipboard::copyTextToClipboard(text(s)); };
    }

    // 选择器：id 都是下标加一
    c_.group.addItem("-", 1);
    for (int g = 0; g < 8; ++g)
    {
        c_.group.addItem(juce::String::charToString(static_cast<juce::juce_wchar>('A' + g)), g + 2);
    }
    for (std::size_t i = 0; i < caseCount(); ++i)
    {
        c_.caseId.addItem(text(caseAt(i)), static_cast<int>(i) + 1);
    }
    for (std::size_t i = 0; i < kBufferChoices.size(); ++i)
    {
        c_.buffer.addItem(kBufferChoices[i] == 0 ? juce::String("-") : juce::String(kBufferChoices[i]),
                          static_cast<int>(i) + 1);
    }
    for (std::size_t i = 0; i < kGuardChoices.size(); ++i)
    {
        c_.guard.addItem(guardName(kGuardChoices[i]), static_cast<int>(i) + 1);
    }
    c_.caseId.onChange = [this] { fillVariants(); };
    c_.group.onChange = [this] { host_.probeSetGroup(std::max(c_.group.getSelectedId() - 2, -1)); };

    const ProbeSelection initial;
    c_.caseId.setText(text(initial.caseId), juce::dontSendNotification);
    fillVariants();
    c_.variant.setText(text(initial.variant), juce::dontSendNotification);
    c_.buffer.setSelectedId(1, juce::dontSendNotification);
    c_.guard.setSelectedId(static_cast<int>(kGuardChoices.size()), juce::dontSendNotification);

    const ProbeStatus status = host_.probeStatus();
    c_.group.setSelectedId(status.group + 2, juce::dontSendNotification);

    // 滑杆：范围就是规则里的上限，处理器收到的值再钳一次
    c_.feedback.setSliderStyle(juce::Slider::LinearHorizontal);
    c_.feedback.setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 22);
    c_.feedback.setRange(0.0, static_cast<double>(kFeedbackMaxPercent), 1.0);
    c_.feedback.setTextValueSuffix(" %");
    c_.feedback.setValue(static_cast<double>(clampFeedbackPercent(status.feedbackPercent)), juce::dontSendNotification);
    c_.feedback.onValueChange = [this] {
        host_.probeSetFeedbackPercent(clampFeedbackPercent(static_cast<float>(c_.feedback.getValue())));
    };
    c_.delay.setSliderStyle(juce::Slider::LinearHorizontal);
    c_.delay.setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 22);
    c_.delay.setRange(static_cast<double>(engine::kMinDelayMs), static_cast<double>(engine::kMaxDelayMs), 1.0);
    c_.delay.setSkewFactorFromMidPoint(250.0);
    c_.delay.setTextValueSuffix(" ms");
    c_.delay.setValue(static_cast<double>(clampDelayMs(status.delayMs)), juce::dontSendNotification);
    c_.delay.onValueChange = [this] { host_.probeSetDelayMs(clampDelayMs(static_cast<float>(c_.delay.getValue()))); };

    c_.panic.setButtonText("Panic");
    c_.recover.setButtonText("Recover");
    c_.ping.setButtonText("Ping");
    c_.record.setButtonText("Record " + juce::String(kRecordSeconds) + " s");
    c_.copyDiagnostics.setButtonText("Copy diagnostics");
    c_.panic.onClick = [this] { doPanic(); };
    c_.recover.onClick = [this] { doRecover(); };
    c_.ping.onClick = [this] { doPing(); };
    c_.record.onClick = [this] { doRecord(); };
    c_.copyDiagnostics.onClick = [this] { doCopyDiagnostics(); };

    c_.status.setJustificationType(juce::Justification::topLeft);
    c_.counters.setJustificationType(juce::Justification::topLeft);

    addCaption("Group", c_.group);
    addCaption("Case", c_.caseId);
    addCaption("Variant", c_.variant);
    addCaption("Buffer (samples)", c_.buffer);
    addCaption("ASIO-Guard", c_.guard);
    addCaption("Feedback", c_.feedback);
    addCaption("Delay D", c_.delay);
    for (juce::Component* child : std::initializer_list<juce::Component*>{
             &c_.group, &c_.caseId, &c_.variant, &c_.buffer, &c_.guard, &c_.feedback, &c_.delay, &c_.panic, &c_.recover,
             &c_.ping, &c_.record, &c_.copyDiagnostics, &c_.status, &c_.counters})
    {
        addAndMakeVisible(child);
    }

    setResizable(false, false);
    setSize(kWidth, kHeight);
    refresh();
    startTimer(kRefreshMs);
}

ProbeEditor::~ProbeEditor()
{
    stopTimer();
}

void ProbeEditor::addCaption(const juce::String& caption, juce::Component& target)
{
    auto* label = captions_.add(new juce::Label({}, caption));
    label->attachToComponent(&target, false);
}

void ProbeEditor::fillVariants()
{
    c_.variant.clear(juce::dontSendNotification);
    int id = 1;
    for (const std::string& variant : variantsFor(c_.caseId.getText().toStdString()))
    {
        c_.variant.addItem(text(variant), id++);
    }
    c_.variant.setSelectedId(1, juce::dontSendNotification);
}

ProbeSelection ProbeEditor::selection() const
{
    ProbeSelection sel;
    sel.caseId = c_.caseId.getText().toStdString();
    sel.variant = c_.variant.getText().toStdString();
    const auto bufferIndex = static_cast<std::size_t>(std::max(c_.buffer.getSelectedId(), 1) - 1);
    const auto guardIndex = static_cast<std::size_t>(std::max(c_.guard.getSelectedId(), 1) - 1);
    sel.buf = kBufferChoices[std::min(bufferIndex, kBufferChoices.size() - 1)];
    sel.guard = kGuardChoices[std::min(guardIndex, kGuardChoices.size() - 1)];
    return sel;
}

void ProbeEditor::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
}

void ProbeEditor::resized()
{
    auto area = getLocalBounds().reduced(12);
    const auto row = [&area](int captionHeight, int height) {
        area.removeFromTop(captionHeight);
        const auto r = area.removeFromTop(height);
        area.removeFromTop(6);
        return r;
    };
    const auto first = splitColumns(row(20, 26), 3);
    c_.group.setBounds(first[0]);
    c_.caseId.setBounds(first[1]);
    c_.variant.setBounds(first[2]);
    const auto second = splitColumns(row(20, 26), 2);
    c_.buffer.setBounds(second[0]);
    c_.guard.setBounds(second[1]);
    c_.feedback.setBounds(row(20, 24));
    c_.delay.setBounds(row(20, 24));
    const auto buttons = splitColumns(row(0, 32), 5);
    c_.panic.setBounds(buttons[0]);
    c_.recover.setBounds(buttons[1]);
    c_.ping.setBounds(buttons[2]);
    c_.record.setBounds(buttons[3]);
    c_.copyDiagnostics.setBounds(buttons[4]);
    c_.status.setBounds(area.removeFromTop(104));
    c_.counters.setBounds(area);
}

void ProbeEditor::refresh()
{
    const ProbeStatus status = host_.probeStatus();
    if (status.recording)
    {
        recordRejected_ = false;
    }
    c_.ping.setEnabled(pingAllowed(status));
    c_.record.setEnabled(recordAllowed(status, selection()));
    c_.recover.setEnabled(cooldown_.ready(options_.nowMs()));

    if (c_.group.getSelectedId() != status.group + 2)
    {
        c_.group.setSelectedId(status.group + 2, juce::dontSendNotification);
    }
    if (!c_.feedback.isMouseButtonDown())
    {
        c_.feedback.setValue(static_cast<double>(clampFeedbackPercent(status.feedbackPercent)),
                             juce::dontSendNotification);
    }
    if (!c_.delay.isMouseButtonDown())
    {
        c_.delay.setValue(static_cast<double>(clampDelayMs(status.delayMs)), juce::dontSendNotification);
    }

    // 文本没变就不动：重设同样的文本也会触发重绘
    const std::string statusNow = statusText(status, recordRejected_);
    if (statusNow != statusShown_)
    {
        statusShown_ = statusNow;
        c_.status.setText(text(statusNow), juce::dontSendNotification);
    }
    const std::string countersNow = countersText(status);
    if (countersNow != countersShown_)
    {
        countersShown_ = countersNow;
        c_.counters.setText(text(countersNow), juce::dontSendNotification);
    }
}

void ProbeEditor::doPanic()
{
    cooldown_.onPanic(options_.nowMs());
    host_.probePanic();
    refresh();
}

void ProbeEditor::doRecover()
{
    if (!cooldown_.ready(options_.nowMs()))
    {
        return;
    }
    host_.probeRecover();
}

void ProbeEditor::doPing()
{
    if (pingAllowed(host_.probeStatus()))
    {
        host_.probePing();
    }
}

void ProbeEditor::doRecord()
{
    const ProbeSelection sel = selection();
    if (recordAllowed(host_.probeStatus(), sel))
    {
        recordRejected_ = !host_.probeStartRecording(sel, kRecordSeconds);
        refresh();
    }
}

void ProbeEditor::doCopyDiagnostics()
{
    options_.copyToClipboard(host_.probeDiagnosticsText());
}

} // namespace rinne::probe
