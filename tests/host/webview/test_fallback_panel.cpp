// SPDX-License-Identifier: GPL-3.0-or-later
//
// 兜底面板：三个原生动作（急停、恢复、环路开关）的钩子、禁用态、点击时的再确认，
// 唯一的提示行是纯 ASCII，所有元素在最小窗口里放得下且互不重叠，文字与底色的对比度够读。
// 点击用直接调用按钮的 onClick 来模拟，不起消息循环、不开窗口。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

#include <juce_gui_basics/juce_gui_basics.h>

#include "FallbackPanel.h"

namespace
{

using rinne::webview::FallbackPanel;
using rinne::webview::kFallbackHintLine;
using rinne::webview::kFallbackPanelArgb;

// 记下被调用的情况；开关状态由「环路」这个假的原生侧持有
struct Recorder
{
    int panics = 0;
    int resumes = 0;
    std::vector<bool> loopRequests;
    bool resumeAllowed = true;
    bool loop = false;
    int installs = 0;
    int retries = 0;

    FallbackPanel::ControlHooks hooks()
    {
        FallbackPanel::ControlHooks h;
        h.onPanic = [this] { ++panics; };
        h.onResume = [this] { ++resumes; };
        h.onLoopToggle = [this](bool wanted) {
            loopRequests.push_back(wanted);
            loop = wanted;
        };
        h.resumeEnabled = [this] { return resumeAllowed; };
        h.loopOn = [this] { return loop; };
        return h;
    }
};

juce::TextButton* button(FallbackPanel& panel, const char* id)
{
    return dynamic_cast<juce::TextButton*>(panel.findChildWithID(id));
}

juce::Label* label(FallbackPanel& panel, const char* id)
{
    return dynamic_cast<juce::Label*>(panel.findChildWithID(id));
}

// WCAG 相对亮度与对比度
double channel(juce::uint8 v)
{
    const double c = static_cast<double>(v) / 255.0;
    return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double luminance(juce::Colour c)
{
    return 0.2126 * channel(c.getRed()) + 0.7152 * channel(c.getGreen()) + 0.0722 * channel(c.getBlue());
}

double contrast(juce::Colour a, juce::Colour b)
{
    const double la = luminance(a);
    const double lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

FallbackPanel::Options baseOptions()
{
    FallbackPanel::Options o;
    o.title = "Title";
    o.message = "Message text";
    return o;
}

} // namespace

TEST_CASE("fallback panel: shows the title, message and the single ASCII hint line", "[webview][fallback]")
{
    auto o = baseOptions();
    FallbackPanel panel(o);

    REQUIRE(label(panel, "fallback.title") != nullptr);
    REQUIRE(label(panel, "fallback.message") != nullptr);
    REQUIRE(label(panel, "fallback.hint") != nullptr);
    CHECK(label(panel, "fallback.title")->getText() == "Title");
    CHECK(label(panel, "fallback.message")->getText() == "Message text");
    CHECK(label(panel, "fallback.hint")->getText() == juce::String(kFallbackHintLine));

    // 提示行只用 ASCII：运行期字面量含非 ASCII 会在中文代码页的机器上触发字符集警告
    for (const char* p = kFallbackHintLine; *p != '\0'; ++p)
        CHECK(static_cast<unsigned char>(*p) < 0x80);
    CHECK(std::string(kFallbackHintLine).find('\n') == std::string::npos);
}

TEST_CASE("fallback panel: button captions are the documented ASCII words", "[webview][fallback]")
{
    auto o = baseOptions();
    o.showInstall = true;
    FallbackPanel panel(o);

    CHECK(button(panel, "fallback.panic")->getButtonText() == "Panic");
    CHECK(button(panel, "fallback.resume")->getButtonText() == "Resume");
    CHECK(button(panel, "fallback.loop")->getButtonText() == "Loop");
    CHECK(button(panel, "fallback.install")->getButtonText() == "Install WebView2 Runtime");
    CHECK(button(panel, "fallback.retry")->getButtonText() == "Retry");
}

TEST_CASE("fallback panel: without hooks all three native actions are disabled", "[webview][fallback]")
{
    auto o = baseOptions();
    FallbackPanel panel(o);
    CHECK_FALSE(button(panel, "fallback.panic")->isEnabled());
    CHECK_FALSE(button(panel, "fallback.resume")->isEnabled());
    CHECK_FALSE(button(panel, "fallback.loop")->isEnabled());
    // 重试不属于三个原生动作，随时可点
    CHECK(button(panel, "fallback.retry")->isEnabled());
}

TEST_CASE("fallback panel: each missing hook disables only its own button", "[webview][fallback]")
{
    Recorder rec;

    SECTION("no panic hook")
    {
        auto o = baseOptions();
        o.controls = rec.hooks();
        o.controls.onPanic = nullptr;
        FallbackPanel panel(o);
        CHECK_FALSE(button(panel, "fallback.panic")->isEnabled());
        CHECK(button(panel, "fallback.resume")->isEnabled());
        CHECK(button(panel, "fallback.loop")->isEnabled());
    }

    SECTION("no resume hook")
    {
        auto o = baseOptions();
        o.controls = rec.hooks();
        o.controls.onResume = nullptr;
        FallbackPanel panel(o);
        CHECK(button(panel, "fallback.panic")->isEnabled());
        CHECK_FALSE(button(panel, "fallback.resume")->isEnabled());
        CHECK(button(panel, "fallback.loop")->isEnabled());
    }

    SECTION("no resume gate means resume cannot be clicked")
    {
        auto o = baseOptions();
        o.controls = rec.hooks();
        o.controls.resumeEnabled = nullptr;
        FallbackPanel panel(o);
        CHECK_FALSE(button(panel, "fallback.resume")->isEnabled());
        CHECK(button(panel, "fallback.panic")->isEnabled());
    }

    SECTION("no loop hook")
    {
        auto o = baseOptions();
        o.controls = rec.hooks();
        o.controls.onLoopToggle = nullptr;
        FallbackPanel panel(o);
        CHECK(button(panel, "fallback.panic")->isEnabled());
        CHECK(button(panel, "fallback.resume")->isEnabled());
        CHECK_FALSE(button(panel, "fallback.loop")->isEnabled());
    }
}

TEST_CASE("fallback panel: panic calls its hook once per click", "[webview][fallback]")
{
    Recorder rec;
    auto o = baseOptions();
    o.controls = rec.hooks();
    FallbackPanel panel(o);

    button(panel, "fallback.panic")->onClick();
    CHECK(rec.panics == 1);
    button(panel, "fallback.panic")->onClick();
    CHECK(rec.panics == 2);
    CHECK(rec.resumes == 0);
    CHECK(rec.loopRequests.empty());
}

TEST_CASE("fallback panel: resume follows the gate, both on screen and at click time", "[webview][fallback]")
{
    Recorder rec;
    rec.resumeAllowed = false; // 例如切断后的冷却期
    auto o = baseOptions();
    o.controls = rec.hooks();
    FallbackPanel panel(o);

    CHECK_FALSE(button(panel, "fallback.resume")->isEnabled());

    // 冷却结束，下一次刷新按钮可点
    rec.resumeAllowed = true;
    panel.refreshControls();
    CHECK(button(panel, "fallback.resume")->isEnabled());

    button(panel, "fallback.resume")->onClick();
    CHECK(rec.resumes == 1);

    // 点击与定时刷新之间冷却重新开始：按钮的禁用态是旧的，点击时必须再问一次，不能放行
    rec.resumeAllowed = false;
    button(panel, "fallback.resume")->onClick();
    CHECK(rec.resumes == 1);
    CHECK_FALSE(button(panel, "fallback.resume")->isEnabled()); // 点完立刻刷新
}

TEST_CASE("fallback panel: loop asks for the opposite of the current state", "[webview][fallback]")
{
    Recorder rec;
    auto o = baseOptions();
    o.controls = rec.hooks();
    FallbackPanel panel(o);

    CHECK_FALSE(button(panel, "fallback.loop")->getToggleState());

    button(panel, "fallback.loop")->onClick();
    REQUIRE(rec.loopRequests.size() == 1);
    CHECK(rec.loopRequests[0] == true);
    CHECK(button(panel, "fallback.loop")->getToggleState()); // 点完按新状态画

    button(panel, "fallback.loop")->onClick();
    REQUIRE(rec.loopRequests.size() == 2);
    CHECK(rec.loopRequests[1] == false);
    CHECK_FALSE(button(panel, "fallback.loop")->getToggleState());
}

TEST_CASE("fallback panel: loop state changed elsewhere shows up on refresh", "[webview][fallback]")
{
    Recorder rec;
    auto o = baseOptions();
    o.controls = rec.hooks();
    FallbackPanel panel(o);

    rec.loop = true; // 比如宿主或探针里关了又开了
    panel.refreshControls();
    CHECK(button(panel, "fallback.loop")->getToggleState());

    // 此时点击是「关」，而不是按面板上次记住的状态再「开」一次
    button(panel, "fallback.loop")->onClick();
    REQUIRE(rec.loopRequests.size() == 1);
    CHECK(rec.loopRequests[0] == false);
}

TEST_CASE("fallback panel: loop without a state reader is treated as off", "[webview][fallback]")
{
    Recorder rec;
    auto o = baseOptions();
    o.controls = rec.hooks();
    o.controls.loopOn = nullptr;
    FallbackPanel panel(o);

    CHECK_FALSE(button(panel, "fallback.loop")->getToggleState());
    button(panel, "fallback.loop")->onClick();
    button(panel, "fallback.loop")->onClick();
    REQUIRE(rec.loopRequests.size() == 2);
    CHECK(rec.loopRequests[0] == true);
    CHECK(rec.loopRequests[1] == true); // 读不到当前状态，就一直请求「开」
}

TEST_CASE("fallback panel: install button exists only when asked for", "[webview][fallback]")
{
    {
        auto o = baseOptions();
        FallbackPanel panel(o);
        CHECK(panel.findChildWithID("fallback.install") == nullptr);
    }

    Recorder rec;
    auto o = baseOptions();
    o.showInstall = true;
    o.onInstall = [&rec] { ++rec.installs; };
    o.onRetry = [&rec] { ++rec.retries; };
    FallbackPanel panel(o);

    REQUIRE(button(panel, "fallback.install") != nullptr);
    button(panel, "fallback.install")->onClick();
    CHECK(rec.installs == 1);
    CHECK(rec.retries == 0);
    button(panel, "fallback.retry")->onClick();
    CHECK(rec.retries == 1);
}

TEST_CASE("fallback panel: install and retry tolerate missing callbacks", "[webview][fallback]")
{
    auto o = baseOptions();
    o.showInstall = true;
    FallbackPanel panel(o);
    button(panel, "fallback.install")->onClick(); // 没有回调时什么也不发生，不崩
    button(panel, "fallback.retry")->onClick();
    SUCCEED();
}

TEST_CASE("fallback panel: the diagnostics line is shown only when there is text", "[webview][fallback]")
{
    {
        auto o = baseOptions();
        FallbackPanel panel(o);
        panel.setSize(460, 364);
        CHECK_FALSE(panel.findChildWithID("fallback.details")->isVisible());
    }
    {
        auto o = baseOptions();
        o.details = "waited 5000 ms, runtime 120.0.1";
        FallbackPanel panel(o);
        panel.setSize(460, 364);
        auto* details = label(panel, "fallback.details");
        REQUIRE(details != nullptr);
        CHECK(details->isVisible());
        CHECK(details->getText() == "waited 5000 ms, runtime 120.0.1");
    }
}

TEST_CASE("fallback panel: everything fits in the smallest window and nothing overlaps", "[webview][fallback]")
{
    // 最小的窗口是 Output 的 460 x 364 设计盒；全部元素同时出现（含诊断行与安装按钮）也要放得下
    Recorder rec;
    auto o = baseOptions();
    o.details = "waited 5000 ms, last navigation: started, runtime: 120.0.2210.91, error: none reported";
    o.showInstall = true;
    o.controls = rec.hooks();
    FallbackPanel panel(o);
    panel.setSize(460, 364);

    const juce::Rectangle<int> whole(0, 0, 460, 364);
    std::vector<juce::Component*> shown;
    for (const char* id : {"fallback.title", "fallback.message", "fallback.details", "fallback.panic",
                           "fallback.resume", "fallback.loop", "fallback.hint", "fallback.install", "fallback.retry"})
    {
        auto* c = panel.findChildWithID(id);
        REQUIRE(c != nullptr);
        REQUIRE(c->isVisible());
        INFO(id << " bounds " << c->getBounds().toString());
        CHECK(c->getWidth() > 0);
        CHECK(c->getHeight() > 0);
        CHECK(whole.contains(c->getBounds()));
        shown.push_back(c);
    }

    for (std::size_t i = 0; i < shown.size(); ++i)
        for (std::size_t j = i + 1; j < shown.size(); ++j)
        {
            INFO(shown[i]->getComponentID() << " vs " << shown[j]->getComponentID());
            CHECK_FALSE(shown[i]->getBounds().intersects(shown[j]->getBounds()));
        }
}

TEST_CASE("fallback panel: the three native action buttons share one row and keep a usable size", "[webview][fallback]")
{
    Recorder rec;
    auto o = baseOptions();
    o.controls = rec.hooks();
    FallbackPanel panel(o);
    panel.setSize(460, 364);

    const auto p = button(panel, "fallback.panic")->getBounds();
    const auto r = button(panel, "fallback.resume")->getBounds();
    const auto l = button(panel, "fallback.loop")->getBounds();
    CHECK(p.getY() == r.getY());
    CHECK(r.getY() == l.getY());
    CHECK(p.getRight() < r.getX());
    CHECK(r.getRight() < l.getX());
    for (const auto& b : {p, r, l})
    {
        CHECK(b.getWidth() >= 80);
        CHECK(b.getHeight() >= 24);
    }
}

TEST_CASE("fallback panel: text has enough contrast against the panel colour", "[webview][fallback]")
{
    Recorder rec;
    auto o = baseOptions();
    o.details = "diagnostic";
    o.controls = rec.hooks();
    FallbackPanel panel(o);

    const juce::Colour background(kFallbackPanelArgb);
    for (const char* id : {"fallback.title", "fallback.message", "fallback.details", "fallback.hint"})
    {
        auto* l = label(panel, id);
        REQUIRE(l != nullptr);
        const auto text = l->findColour(juce::Label::textColourId);
        INFO(id << " contrast " << contrast(text, background));
        CHECK(contrast(text, background) >= 4.5);
    }
}

TEST_CASE("fallback panel: the panel colour is the documented opaque mid tone", "[webview][fallback]")
{
    const juce::Colour c(kFallbackPanelArgb);
    CHECK(c.getAlpha() == 255);
    CHECK(c.getRed() == 0xd9);
    CHECK(c.getGreen() == 0xca);
    CHECK(c.getBlue() == 0xdb);
}
