// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rinne Output 的网页编辑器：WebViewHost 的子类，只做胶水，桥的逻辑全在 OutputBridge（见 OutputBridge.h）。
//   - 经 augmentOptions 注册十个角色函数，每个开头一道兜底面板守卫（宿主测试的源码卫生用例逐个核对），之后交给
//     OutputBridge，再当拍推一次事件；
//   - 覆写 buildSnapshot 补 Output 字段，覆写 emitTick 按 OutputBridge 的结论推事件，覆写 emitFirstFrames 补发首帧
//     rinne.pair；
//   - 落到系统上的动作（剪贴板写后回读、系统浏览器、随包许可证）在这里实现，经 OutputBridge::Hooks 交进去。
// 需要 JUCE_WEB_BROWSER=1（同 WebViewHost.h）：宿主测试目标不编本文件。全部在消息线程。
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "OutputBridge.h"
#include "OutputProcessor.h"
#include "WebViewHost.h"

namespace rinne::output
{

class OutputEditor final : public webview::WebViewHost
{
public:
    // resources 是插件自己嵌入的页面资源（含随包的 LICENSE），由插件入口填
    OutputEditor(OutputProcessor& processor, webview::ResourceProvider::Source resources);
    ~OutputEditor() override;

protected:
    juce::var buildSnapshot() override;
    void emitTick() override;
    void emitFirstFrames() override;
    // 确认缩放：照基类落盘，再按胶水的定型清单让实例定型（commitUiScale 算用户动作）
    void persistUiScale(float scale) override;

private:
    using Args = juce::Array<juce::var>;
    using Completion = juce::WebBrowserComponent::NativeFunctionCompletion;

    // 在 WebViewHost 的构造里调用（经 augmentOptions），那时本类的成员还没建好：这里只登记 lambda，
    // 不碰任何成员，lambda 只在页面调用时才经 self 转到处理函数
    static void registerFunctions(OutputEditor* self, juce::WebBrowserComponent::Options& options);

    // 角色函数。开头一律是兜底面板守卫（宿主测试的源码卫生用例逐个核对），之后交给 OutputBridge，再当拍推一次事件
    void handleSetRulesAck(const Args& args, Completion complete);
    void handleSetGuideSeen(const Args& args, Completion complete);
    void handleSetGroup(const Args& args, Completion complete);
    void handleCopyDiagnostics(const Args& args, Completion complete);
    void handleOpenExternal(const Args& args, Completion complete);
    void handleSetDiagLog(const Args& args, Completion complete);
    void handleResolveTakeover(const Args& args, Completion complete);
    void handleShowLicense(const Args& args, Completion complete);
    void handleRepairPair(const Args& args, Completion complete);
    void handleSetTailMode(const Args& args, Completion complete);

    OutputBridge::UiContext uiContext() const;
    void sendFrames(const OutputBridge::Frames& frames);
    bool openLicense() const;

    OutputProcessor& processor_;
    webview::ResourceProvider resources_; // 只为「查看许可证」取随包的 LICENSE
    OutputBridge bridge_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OutputEditor)
};

} // namespace rinne::output
