// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/plugin-common.json
#include "FallbackPanel.h"

#include <utility>

namespace rinne::webview
{
namespace
{
// 面板底是浅色，所以文字必须是深墨，否则这块「网页起不来时唯一能告诉用户发生了什么」的面板会白字浅底、一个字看不见。
// 这两个值不是跨文件真源，样式表改了这里不会红；唯一的硬约束是与面板底的对比度不低于 4.5，由宿主测试守。
// 正文、提示与诊断行同色，是对比度下限逼出来的：再淡一档只有约 3.4，过不了断言；层级改由字号与等宽字面承担
constexpr juce::uint32 kTitleTextArgb = 0xff21212a;
constexpr juce::uint32 kBodyTextArgb = 0xff52525c;

// 布局常量（像素）。窗口最小的是 Output 的 460 x 364 设计盒，全部元素同时出现时要放得下
constexpr int kPad = 16;
constexpr int kTitleH = 30;
constexpr int kMessageH = 64;
constexpr int kDetailsH = 56;
constexpr int kControlRowH = 30;
constexpr int kControlGap = 8;
constexpr int kHintH = 16;
constexpr int kButtonH = 32;
} // namespace

FallbackPanel::FallbackPanel(Options options) : options_(std::move(options))
{
    title_.setComponentID("fallback.title");
    title_.setText(options_.title, juce::dontSendNotification);
    title_.setJustificationType(juce::Justification::centred);
    title_.setFont(juce::Font(juce::FontOptions(20.0f, juce::Font::bold)));
    title_.setColour(juce::Label::textColourId, juce::Colour(kTitleTextArgb));
    addAndMakeVisible(title_);

    message_.setComponentID("fallback.message");
    message_.setText(options_.message, juce::dontSendNotification);
    message_.setJustificationType(juce::Justification::centredTop);
    message_.setColour(juce::Label::textColourId, juce::Colour(kBodyTextArgb));
    addAndMakeVisible(message_);

    details_.setComponentID("fallback.details");
    details_.setText(options_.details, juce::dontSendNotification);
    details_.setJustificationType(juce::Justification::centredTop);
    // 11 像素等宽；Output 的窄盒里长诊断串要折三四行
    details_.setFont(
        juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 11.0f, juce::Font::plain)));
    details_.setColour(juce::Label::textColourId, juce::Colour(kBodyTextArgb));
    // 1.0 表示不许横向压扁，长诊断串按行宽自动折行
    details_.setMinimumHorizontalScale(1.0f);
    addChildComponent(details_);
    details_.setVisible(options_.details.isNotEmpty());

    hint_.setComponentID("fallback.hint");
    hint_.setText(kFallbackHintLine, juce::dontSendNotification);
    hint_.setJustificationType(juce::Justification::centred);
    hint_.setFont(juce::Font(juce::FontOptions(11.0f)));
    hint_.setColour(juce::Label::textColourId, juce::Colour(kBodyTextArgb));
    addAndMakeVisible(hint_);

    panic_.setComponentID("fallback.panic");
    panic_.setButtonText("Panic");
    panic_.onClick = [this] {
        if (options_.controls.onPanic)
            options_.controls.onPanic();
        refreshControls();
    };
    addAndMakeVisible(panic_);

    resume_.setComponentID("fallback.resume");
    resume_.setButtonText("Resume");
    resume_.onClick = [this] {
        // 点击时再问一次：按钮的禁用态是定时刷新的，点击与刷新之间冷却可能刚好重新开始
        if (options_.controls.onResume && options_.controls.resumeEnabled && options_.controls.resumeEnabled())
            options_.controls.onResume();
        refreshControls();
    };
    addAndMakeVisible(resume_);

    loop_.setComponentID("fallback.loop");
    loop_.setButtonText("Loop");
    loop_.onClick = [this] {
        if (options_.controls.onLoopToggle)
        {
            const bool now = options_.controls.loopOn ? options_.controls.loopOn() : false;
            options_.controls.onLoopToggle(!now);
        }
        refreshControls();
    };
    addAndMakeVisible(loop_);

    if (options_.showInstall)
    {
        install_.setComponentID("fallback.install");
        install_.setButtonText("Install WebView2 Runtime");
        install_.onClick = [this] {
            if (options_.onInstall)
                options_.onInstall();
        };
        addAndMakeVisible(install_);
    }

    retry_.setComponentID("fallback.retry");
    retry_.setButtonText("Retry");
    retry_.onClick = [this] {
        if (options_.onRetry)
            options_.onRetry();
    };
    addAndMakeVisible(retry_);

    refreshControls();
}

FallbackPanel::~FallbackPanel() = default;

void FallbackPanel::refreshControls()
{
    const auto& c = options_.controls;
    panic_.setEnabled(static_cast<bool>(c.onPanic));
    resume_.setEnabled(static_cast<bool>(c.onResume) && static_cast<bool>(c.resumeEnabled) && c.resumeEnabled());
    loop_.setEnabled(static_cast<bool>(c.onLoopToggle));
    loop_.setToggleState(c.loopOn ? c.loopOn() : false, juce::dontSendNotification);
}

void FallbackPanel::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(kFallbackPanelArgb));
}

void FallbackPanel::resized()
{
    auto b = getLocalBounds().reduced(kPad);
    title_.setBounds(b.removeFromTop(kTitleH));
    message_.setBounds(b.removeFromTop(kMessageH));
    if (details_.isVisible())
    {
        details_.setBounds(b.removeFromTop(kDetailsH));
        b.removeFromTop(4);
    }
    b.removeFromTop(8);

    auto row = b.removeFromTop(kControlRowH);
    const int w = (row.getWidth() - 2 * kControlGap) / 3;
    panic_.setBounds(row.removeFromLeft(w));
    row.removeFromLeft(kControlGap);
    resume_.setBounds(row.removeFromLeft(w));
    row.removeFromLeft(kControlGap);
    loop_.setBounds(row);

    b.removeFromTop(6);
    hint_.setBounds(b.removeFromTop(kHintH));
    b.removeFromTop(8);

    if (options_.showInstall)
    {
        install_.setBounds(b.removeFromTop(kButtonH).reduced(24, 0));
        b.removeFromTop(8);
    }
    retry_.setBounds(b.removeFromTop(kButtonH).reduced(96, 0));
}

} // namespace rinne::webview
