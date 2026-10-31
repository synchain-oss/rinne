// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rinne Output 的网页编辑器（胶水），见 OutputEditor.h；桥的逻辑在 OutputBridge.cpp。

#include "OutputEditor.h"

#include <utility>

#include "OutputBridgeApi.h"
#include "PlatformWebView.h"

namespace rinne::output
{

// 角色函数开头的守卫逐字写成 complete(bridge::respondNotReady())（宿主测试的源码卫生用例按这个写法核对），
// 而在本命名空间里 bridge 指的是名表那一份（OutputBridgeApi.h），这里把共用的回执引进来。
// 名表头文件只放桥名，不在那里加
namespace bridge
{
using rinne::bridge::respondNotReady;
} // namespace bridge

namespace
{
namespace api = rinne::output::bridge;

// 写系统剪贴板，再读回来核对。换行按 \n 比：个别剪贴板工具会把换行改成 \r\n
bool writeSystemClipboard(const juce::String& text)
{
    juce::SystemClipboard::copyTextToClipboard(text);
    return juce::SystemClipboard::getTextFromClipboard().removeCharacters("\r") == text.removeCharacters("\r");
}

bool launchInBrowser(const juce::String& url)
{
    return juce::URL(url).launchInDefaultBrowser();
}
} // namespace

OutputEditor::OutputEditor(OutputProcessor& processor, webview::ResourceProvider::Source resources)
    : webview::WebViewHost(processor,
                           [self = this, resources] {
                               webview::WebViewHost::Config config;
                               config.role = "output";
                               config.resourceSource = resources;
                               // 回送、安全链与环路开关都在 Input：兜底面板上的三个原生动作留空（按钮禁用）
                               config.controls = {};
                               config.augmentOptions = [self](juce::WebBrowserComponent::Options& options) {
                                   registerFunctions(self, options);
                               };
                               return config;
                           }()),
      processor_(processor), resources_(resources),
      bridge_(processor,
              OutputBridge::Hooks{{}, &writeSystemClipboard, &launchInBrowser, [this] { return openLicense(); }, {}})
{
}

OutputEditor::~OutputEditor() = default;

void OutputEditor::registerFunctions(OutputEditor* self, juce::WebBrowserComponent::Options& options)
{
    options =
        options
            .withNativeFunction(juce::Identifier(api::kFnSetRulesAck),
                                [self](const Args& a, Completion c) { self->handleSetRulesAck(a, std::move(c)); })
            .withNativeFunction(juce::Identifier(api::kFnSetGuideSeen),
                                [self](const Args& a, Completion c) { self->handleSetGuideSeen(a, std::move(c)); })
            .withNativeFunction(juce::Identifier(api::kFnSetGroup),
                                [self](const Args& a, Completion c) { self->handleSetGroup(a, std::move(c)); })
            .withNativeFunction(juce::Identifier(api::kFnCopyDiagnostics),
                                [self](const Args& a, Completion c) { self->handleCopyDiagnostics(a, std::move(c)); })
            .withNativeFunction(juce::Identifier(api::kFnOpenExternal),
                                [self](const Args& a, Completion c) { self->handleOpenExternal(a, std::move(c)); })
            .withNativeFunction(juce::Identifier(api::kFnSetDiagLog),
                                [self](const Args& a, Completion c) { self->handleSetDiagLog(a, std::move(c)); })
            .withNativeFunction(juce::Identifier(api::kFnResolveTakeover),
                                [self](const Args& a, Completion c) { self->handleResolveTakeover(a, std::move(c)); })
            .withNativeFunction(juce::Identifier(api::kFnShowLicense),
                                [self](const Args& a, Completion c) { self->handleShowLicense(a, std::move(c)); })
            .withNativeFunction(juce::Identifier(api::kFnRepairPair),
                                [self](const Args& a, Completion c) { self->handleRepairPair(a, std::move(c)); })
            .withNativeFunction(juce::Identifier(api::kFnSetTailMode),
                                [self](const Args& a, Completion c) { self->handleSetTailMode(a, std::move(c)); });
}

OutputBridge::UiContext OutputEditor::uiContext() const
{
    OutputBridge::UiContext ui;
    ui.lang = lang();
    ui.uiScale = uiScale();
    return ui;
}

void OutputEditor::sendFrames(const OutputBridge::Frames& frames)
{
    if (frames.state)
        emitState();
    if (!frames.pair.isVoid())
        emitEvent(api::kEvPair, frames.pair);
    for (const auto& error : frames.errors)
        emitEvent(api::kEvError, error);
}

juce::var OutputEditor::buildSnapshot()
{
    auto snapshot = webview::WebViewHost::buildSnapshot();
    if (auto* obj = snapshot.getDynamicObject())
        bridge_.addStateFields(*obj);
    return snapshot;
}

void OutputEditor::emitTick()
{
    sendFrames(bridge_.poll(uiContext()));
}

void OutputEditor::emitFirstFrames()
{
    emitEvent(api::kEvPair, bridge_.firstFrames(uiContext()));
}

void OutputEditor::persistUiScale(float scale)
{
    webview::WebViewHost::persistUiScale(scale);
    if (auto* glue = processor_.stateGlue())
        glue->markUserAction();
}

bool OutputEditor::openLicense() const
{
    // 随包的许可证（构建时由 rinne_add_web_assets 的 LICENSE_FILE 嵌入）写到临时目录的固定文件名，再交给系统打开。
    // 每次覆盖同一个文件，不删：系统的查看器可能还开着它
    const auto hit = resources_.lookup("/LICENSE");
    if (!hit.has_value() || hit->data == nullptr || hit->size <= 0)
        return false;
    const juce::File file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("Rinne-LICENSE.txt");
    if (!file.replaceWithData(hit->data, static_cast<std::size_t>(hit->size)))
        return false;
    return file.startAsProcess();
}

void OutputEditor::handleSetRulesAck(const Args& args, Completion complete)
{
    if (panelShown())
    {
        logDiag("bridge request ignored while the fallback panel is shown (setRulesAck)");
        complete(bridge::respondNotReady());
        return;
    }
    complete(bridge_.setRulesAck(args));
    sendFrames(bridge_.poll(uiContext()));
}

void OutputEditor::handleSetGuideSeen(const Args& args, Completion complete)
{
    if (panelShown())
    {
        logDiag("bridge request ignored while the fallback panel is shown (setGuideSeen)");
        complete(bridge::respondNotReady());
        return;
    }
    complete(bridge_.setGuideSeen(args));
    sendFrames(bridge_.poll(uiContext()));
}

void OutputEditor::handleSetGroup(const Args& args, Completion complete)
{
    if (panelShown())
    {
        logDiag("bridge request ignored while the fallback panel is shown (setGroup)");
        complete(bridge::respondNotReady());
        return;
    }
    complete(bridge_.setGroup(args));
    sendFrames(bridge_.poll(uiContext()));
}

void OutputEditor::handleCopyDiagnostics(const Args& args, Completion complete)
{
    if (panelShown())
    {
        logDiag("bridge request ignored while the fallback panel is shown (copyDiagnostics)");
        complete(bridge::respondNotReady());
        return;
    }
    auto ui = uiContext();
    ui.webview = webview::PlatformWebView::runtimeDiagnosticsField(webview::PlatformWebView::runtimeInfo());
    complete(bridge_.copyDiagnostics(args, ui));
}

void OutputEditor::handleOpenExternal(const Args& args, Completion complete)
{
    if (panelShown())
    {
        logDiag("bridge request ignored while the fallback panel is shown (openExternal)");
        complete(bridge::respondNotReady());
        return;
    }
    complete(bridge_.openExternal(args));
}

void OutputEditor::handleSetDiagLog(const Args& args, Completion complete)
{
    if (panelShown())
    {
        logDiag("bridge request ignored while the fallback panel is shown (setDiagLog)");
        complete(bridge::respondNotReady());
        return;
    }
    complete(bridge_.setDiagLog(args));
    sendFrames(bridge_.poll(uiContext()));
}

void OutputEditor::handleResolveTakeover(const Args& args, Completion complete)
{
    if (panelShown())
    {
        logDiag("bridge request ignored while the fallback panel is shown (resolveTakeover)");
        complete(bridge::respondNotReady());
        return;
    }
    complete(bridge_.resolveTakeover(args));
    sendFrames(bridge_.poll(uiContext()));
}

void OutputEditor::handleShowLicense(const Args&, Completion complete)
{
    if (panelShown())
    {
        logDiag("bridge request ignored while the fallback panel is shown (showLicense)");
        complete(bridge::respondNotReady());
        return;
    }
    complete(bridge_.showLicense());
}

void OutputEditor::handleRepairPair(const Args&, Completion complete)
{
    if (panelShown())
    {
        logDiag("bridge request ignored while the fallback panel is shown (repairPair)");
        complete(bridge::respondNotReady());
        return;
    }
    complete(bridge_.repairPair());
    sendFrames(bridge_.poll(uiContext()));
}

void OutputEditor::handleSetTailMode(const Args& args, Completion complete)
{
    if (panelShown())
    {
        logDiag("bridge request ignored while the fallback panel is shown (setTailMode)");
        complete(bridge::respondNotReady());
        return;
    }
    complete(bridge_.setTailMode(args));
    sendFrames(bridge_.poll(uiContext()));
}

} // namespace rinne::output
