// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rinne Input 的网页编辑器（胶水），见 InputEditor.h；桥的逻辑在 InputBridge.cpp。

#include "InputEditor.h"

#include <utility>

#include "InputBridgeApi.h"
#include "PlatformWebView.h"

namespace rinne::input
{

// 角色函数开头的守卫逐字写成 complete(bridge::respondNotReady())（宿主测试的源码卫生用例按这个写法核对），
// 而在本命名空间里 bridge 指的是名表那一份（InputBridgeApi.h），这里把共用的回执引进来。
// 名表头文件只放桥名，不在那里加
namespace bridge
{
using rinne::bridge::respondNotReady;
} // namespace bridge

namespace
{
namespace api = rinne::input::bridge;

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

InputEditor::InputEditor(InputProcessor& processor, webview::ResourceProvider::Source resources,
                         webview::FallbackPanel::ControlHooks controls)
    : webview::WebViewHost(processor,
                           [self = this, resources, controls = std::move(controls)] {
                               webview::WebViewHost::Config config;
                               config.role = "input";
                               config.resourceSource = resources;
                               // 网页起不来时这几个按钮是全部的入口，与网页走同一条路径（处理器的界面入口）
                               config.controls = controls;
                               config.augmentOptions = [self](juce::WebBrowserComponent::Options& options) {
                                   registerFunctions(self, options);
                               };
                               return config;
                           }()),
      processor_(processor), resources_(resources),
      bridge_(processor,
              InputBridge::Hooks{{}, &writeSystemClipboard, &launchInBrowser, [this] { return openLicense(); }, {}})
{
}

InputEditor::~InputEditor() = default;

void InputEditor::registerFunctions(InputEditor* self, juce::WebBrowserComponent::Options& options)
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
            .withNativeFunction(juce::Identifier(api::kFnBeginParamGesture),
                                [self](const Args& a, Completion c) { self->handleBeginParamGesture(a, std::move(c)); })
            .withNativeFunction(juce::Identifier(api::kFnSetParam),
                                [self](const Args& a, Completion c) { self->handleSetParam(a, std::move(c)); })
            .withNativeFunction(juce::Identifier(api::kFnEndParamGesture),
                                [self](const Args& a, Completion c) { self->handleEndParamGesture(a, std::move(c)); })
            .withNativeFunction(juce::Identifier(api::kFnSetLimiter),
                                [self](const Args& a, Completion c) { self->handleSetLimiter(a, std::move(c)); })
            .withNativeFunction(juce::Identifier(api::kFnPanic),
                                [self](const Args& a, Completion c) { self->handlePanic(a, std::move(c)); })
            .withNativeFunction(juce::Identifier(api::kFnResumeLoop),
                                [self](const Args& a, Completion c) { self->handleResumeLoop(a, std::move(c)); })
            .withNativeFunction(juce::Identifier(api::kFnDismissHint),
                                [self](const Args& a, Completion c) { self->handleDismissHint(a, std::move(c)); })
            .withNativeFunction(juce::Identifier(api::kFnLoadPreset),
                                [self](const Args& a, Completion c) { self->handleLoadPreset(a, std::move(c)); })
            .withNativeFunction(juce::Identifier(api::kFnSavePreset),
                                [self](const Args& a, Completion c) { self->handleSavePreset(a, std::move(c)); })
            .withNativeFunction(juce::Identifier(api::kFnDeletePreset),
                                [self](const Args& a, Completion c) { self->handleDeletePreset(a, std::move(c)); })
            .withNativeFunction(juce::Identifier(api::kFnSetGuardOn),
                                [self](const Args& a, Completion c) { self->handleSetGuardOn(a, std::move(c)); })
            .withNativeFunction(juce::Identifier(api::kFnSetResetOnPlay),
                                [self](const Args& a, Completion c) { self->handleSetResetOnPlay(a, std::move(c)); })
            .withNativeFunction(juce::Identifier(api::kFnSetTailMode),
                                [self](const Args& a, Completion c) { self->handleSetTailMode(a, std::move(c)); })
            .withNativeFunction(juce::Identifier(api::kFnSetFbUnit),
                                [self](const Args& a, Completion c) { self->handleSetFbUnit(a, std::move(c)); })
            .withNativeFunction(juce::Identifier(api::kFnSetDelayUnit),
                                [self](const Args& a, Completion c) { self->handleSetDelayUnit(a, std::move(c)); })
            .withNativeFunction(juce::Identifier(api::kFnMoveToFreeGroup),
                                [self](const Args& a, Completion c) { self->handleMoveToFreeGroup(a, std::move(c)); });
}

InputBridge::UiContext InputEditor::uiContext() const
{
    InputBridge::UiContext ui;
    ui.lang = lang();
    ui.uiScale = uiScale();
    return ui;
}

void InputEditor::sendFrames(const InputBridge::Frames& frames)
{
    if (frames.state)
        emitState();
    if (!frames.pair.isVoid())
        emitEvent(api::kEvPair, frames.pair);
    if (!frames.params.isVoid())
        emitEvent(api::kEvParams, frames.params);
    if (!frames.meters.isVoid())
        emitEvent(api::kEvMeters, frames.meters);
    if (!frames.presets.isVoid())
        emitEvent(api::kEvPresets, frames.presets);
    for (const auto& error : frames.errors)
        emitEvent(api::kEvError, error);
}

juce::var InputEditor::buildSnapshot()
{
    auto snapshot = webview::WebViewHost::buildSnapshot();
    if (auto* obj = snapshot.getDynamicObject())
        bridge_.addStateFields(*obj);
    return snapshot;
}

void InputEditor::emitTick()
{
    sendFrames(bridge_.tick(uiContext()));
}

void InputEditor::emitFirstFrames()
{
    sendFrames(bridge_.firstFrames(uiContext()));
}

void InputEditor::persistUiScale(float scale)
{
    webview::WebViewHost::persistUiScale(scale);
    if (auto* glue = processor_.stateGlue())
        glue->markUserAction();
}

void InputEditor::onFallbackPanelShown()
{
    bridge_.endAllGestures();
}

bool InputEditor::openLicense() const
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

void InputEditor::handleSetRulesAck(const Args& args, Completion complete)
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

void InputEditor::handleSetGuideSeen(const Args& args, Completion complete)
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

void InputEditor::handleSetGroup(const Args& args, Completion complete)
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

void InputEditor::handleCopyDiagnostics(const Args& args, Completion complete)
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

void InputEditor::handleOpenExternal(const Args& args, Completion complete)
{
    if (panelShown())
    {
        logDiag("bridge request ignored while the fallback panel is shown (openExternal)");
        complete(bridge::respondNotReady());
        return;
    }
    complete(bridge_.openExternal(args));
}

void InputEditor::handleSetDiagLog(const Args& args, Completion complete)
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

void InputEditor::handleResolveTakeover(const Args& args, Completion complete)
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

void InputEditor::handleShowLicense(const Args&, Completion complete)
{
    if (panelShown())
    {
        logDiag("bridge request ignored while the fallback panel is shown (showLicense)");
        complete(bridge::respondNotReady());
        return;
    }
    complete(bridge_.showLicense());
}

void InputEditor::handleBeginParamGesture(const Args& args, Completion complete)
{
    if (panelShown())
    {
        logDiag("bridge request ignored while the fallback panel is shown (beginParamGesture)");
        complete(bridge::respondNotReady());
        return;
    }
    complete(bridge_.beginParamGesture(args));
}

void InputEditor::handleSetParam(const Args& args, Completion complete)
{
    if (panelShown())
    {
        logDiag("bridge request ignored while the fallback panel is shown (setParam)");
        complete(bridge::respondNotReady());
        return;
    }
    complete(bridge_.setParam(args));
    sendFrames(bridge_.poll(uiContext()));
}

void InputEditor::handleEndParamGesture(const Args& args, Completion complete)
{
    if (panelShown())
    {
        logDiag("bridge request ignored while the fallback panel is shown (endParamGesture)");
        complete(bridge::respondNotReady());
        return;
    }
    complete(bridge_.endParamGesture(args));
}

void InputEditor::handleSetLimiter(const Args& args, Completion complete)
{
    if (panelShown())
    {
        logDiag("bridge request ignored while the fallback panel is shown (setLimiter)");
        complete(bridge::respondNotReady());
        return;
    }
    complete(bridge_.setLimiter(args));
    sendFrames(bridge_.poll(uiContext()));
}

void InputEditor::handlePanic(const Args&, Completion complete)
{
    if (panelShown())
    {
        // 页面点下去之后，请求要经浏览器进程才到这里，看门狗可能抢先切了面板。这一下切断不能丢：页面已经卸载，不会再发。
        // 回执照通则，反正也没人收
        processor_.panic();
        logDiag("bridge request ignored while the fallback panel is shown (panic), the cut itself still applied");
        complete(bridge::respondNotReady());
        return;
    }
    complete(bridge_.panic());
    sendFrames(bridge_.poll(uiContext()));
}

void InputEditor::handleResumeLoop(const Args&, Completion complete)
{
    if (panelShown())
    {
        logDiag("bridge request ignored while the fallback panel is shown (resumeLoop)");
        complete(bridge::respondNotReady());
        return;
    }
    complete(bridge_.resumeLoop());
    sendFrames(bridge_.poll(uiContext()));
}

void InputEditor::handleDismissHint(const Args& args, Completion complete)
{
    if (panelShown())
    {
        logDiag("bridge request ignored while the fallback panel is shown (dismissHint)");
        complete(bridge::respondNotReady());
        return;
    }
    complete(bridge_.dismissHint(args));
    sendFrames(bridge_.poll(uiContext()));
}

void InputEditor::handleLoadPreset(const Args& args, Completion complete)
{
    if (panelShown())
    {
        logDiag("bridge request ignored while the fallback panel is shown (loadPreset)");
        complete(bridge::respondNotReady());
        return;
    }
    complete(bridge_.loadPreset(args));
    sendFrames(bridge_.poll(uiContext()));
}

void InputEditor::handleSavePreset(const Args& args, Completion complete)
{
    if (panelShown())
    {
        logDiag("bridge request ignored while the fallback panel is shown (savePreset)");
        complete(bridge::respondNotReady());
        return;
    }
    complete(bridge_.savePreset(args));
    sendFrames(bridge_.poll(uiContext()));
}

void InputEditor::handleDeletePreset(const Args& args, Completion complete)
{
    if (panelShown())
    {
        logDiag("bridge request ignored while the fallback panel is shown (deletePreset)");
        complete(bridge::respondNotReady());
        return;
    }
    complete(bridge_.deletePreset(args));
    sendFrames(bridge_.poll(uiContext()));
}

void InputEditor::handleSetGuardOn(const Args& args, Completion complete)
{
    if (panelShown())
    {
        logDiag("bridge request ignored while the fallback panel is shown (setGuardOn)");
        complete(bridge::respondNotReady());
        return;
    }
    complete(bridge_.setGuardOn(args));
    sendFrames(bridge_.poll(uiContext()));
}

void InputEditor::handleSetResetOnPlay(const Args& args, Completion complete)
{
    if (panelShown())
    {
        logDiag("bridge request ignored while the fallback panel is shown (setResetOnPlay)");
        complete(bridge::respondNotReady());
        return;
    }
    complete(bridge_.setResetOnPlay(args));
    sendFrames(bridge_.poll(uiContext()));
}

void InputEditor::handleSetTailMode(const Args& args, Completion complete)
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

void InputEditor::handleSetFbUnit(const Args& args, Completion complete)
{
    if (panelShown())
    {
        logDiag("bridge request ignored while the fallback panel is shown (setFbUnit)");
        complete(bridge::respondNotReady());
        return;
    }
    complete(bridge_.setFbUnit(args));
    sendFrames(bridge_.poll(uiContext()));
}

void InputEditor::handleSetDelayUnit(const Args& args, Completion complete)
{
    if (panelShown())
    {
        logDiag("bridge request ignored while the fallback panel is shown (setDelayUnit)");
        complete(bridge::respondNotReady());
        return;
    }
    complete(bridge_.setDelayUnit(args));
    sendFrames(bridge_.poll(uiContext()));
}

void InputEditor::handleMoveToFreeGroup(const Args&, Completion complete)
{
    if (panelShown())
    {
        logDiag("bridge request ignored while the fallback panel is shown (moveToFreeGroup)");
        complete(bridge::respondNotReady());
        return;
    }
    complete(bridge_.moveToFreeGroup());
    sendFrames(bridge_.poll(uiContext()));
}

} // namespace rinne::input
