// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rinne Input 的网页编辑器：WebViewHost 的子类，只做胶水，桥的逻辑全在 InputBridge（见 InputBridge.h）。
//   - 经 augmentOptions 注册二十四个角色函数，每个开头一道兜底面板守卫（宿主测试的源码卫生用例逐个核对），之后交给
//     InputBridge，再当拍推一次事件；
//   - 覆写 buildSnapshot 补 Input 字段，覆写 emitTick 按 InputBridge 的结论推事件，覆写 emitFirstFrames 补发其余首帧；
//   - 落到系统上的动作（剪贴板写后回读、系统浏览器、随包许可证）在这里实现，经 InputBridge::Hooks 交进去。
// 需要 JUCE_WEB_BROWSER=1（同 WebViewHost.h）：宿主测试目标不编本文件。全部在消息线程。
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "InputBridge.h"
#include "InputProcessor.h"
#include "WebViewHost.h"

namespace rinne::input
{

class InputEditor final : public webview::WebViewHost
{
public:
    // resources 是插件自己嵌入的页面资源（含随包的 LICENSE），controls
    // 是兜底面板上的急停、恢复与环路开关，都由插件入口填
    InputEditor(InputProcessor& processor, webview::ResourceProvider::Source resources,
                webview::FallbackPanel::ControlHooks controls);
    ~InputEditor() override;

protected:
    juce::var buildSnapshot() override;
    void emitTick() override;
    void emitFirstFrames() override;
    // 确认缩放：照基类落盘，再按胶水的定型清单让实例定型（commitUiScale 算用户动作）
    void persistUiScale(float scale) override;
    // 切到兜底面板：网页没了，还开着的参数手势等不到它自己结束
    void onFallbackPanelShown() override;

private:
    using Args = juce::Array<juce::var>;
    using Completion = juce::WebBrowserComponent::NativeFunctionCompletion;

    // 在 WebViewHost 的构造里调用（经 augmentOptions），那时本类的成员还没建好：这里只登记 lambda，
    // 不碰任何成员，lambda 只在页面调用时才经 self 转到处理函数
    static void registerFunctions(InputEditor* self, juce::WebBrowserComponent::Options& options);

    // 角色函数。开头一律是兜底面板守卫（宿主测试的源码卫生用例逐个核对），之后交给 InputBridge，再当拍推一次事件
    void handleSetRulesAck(const Args& args, Completion complete);
    void handleSetGuideSeen(const Args& args, Completion complete);
    void handleSetGroup(const Args& args, Completion complete);
    void handleCopyDiagnostics(const Args& args, Completion complete);
    void handleOpenExternal(const Args& args, Completion complete);
    void handleSetDiagLog(const Args& args, Completion complete);
    void handleResolveTakeover(const Args& args, Completion complete);
    void handleShowLicense(const Args& args, Completion complete);
    void handleBeginParamGesture(const Args& args, Completion complete);
    void handleSetParam(const Args& args, Completion complete);
    void handleEndParamGesture(const Args& args, Completion complete);
    void handleSetLimiter(const Args& args, Completion complete);
    void handlePanic(const Args& args, Completion complete);
    void handleResumeLoop(const Args& args, Completion complete);
    void handleDismissHint(const Args& args, Completion complete);
    void handleLoadPreset(const Args& args, Completion complete);
    void handleSavePreset(const Args& args, Completion complete);
    void handleDeletePreset(const Args& args, Completion complete);
    void handleSetGuardOn(const Args& args, Completion complete);
    void handleSetResetOnPlay(const Args& args, Completion complete);
    void handleSetTailMode(const Args& args, Completion complete);
    void handleSetFbUnit(const Args& args, Completion complete);
    void handleSetDelayUnit(const Args& args, Completion complete);
    void handleMoveToFreeGroup(const Args& args, Completion complete);

    InputBridge::UiContext uiContext() const;
    void sendFrames(const InputBridge::Frames& frames);
    bool openLicense() const;

    InputProcessor& processor_;
    webview::ResourceProvider resources_; // 只为「查看许可证」取随包的 LICENSE
    InputBridge bridge_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InputEditor)
};

} // namespace rinne::input
