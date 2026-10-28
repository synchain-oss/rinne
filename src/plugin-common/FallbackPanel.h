// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/plugin-common.json
#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>

namespace rinne::webview
{
// 兜底面板的底色：独立的常量，不与开窗占位同源。开窗占位是一组色标的渐变，而面板的底只能是一块纯色。
// 取值是占位渐变沿轴 50% 处的中点色（开窗切到兜底面板时与那块渐变的中间亮度不跳阶），
// 但那只是取值时的记录，不是跨文件真源：占位渐变日后改了，这里不会跟着变，也不会有判据判红。
// 面板可读性由宿主测试里的对比度断言独立把守，它读的就是本常量和各标签的实际颜色
inline constexpr juce::uint32 kFallbackPanelArgb = 0xffd9cadb;

// 面板上唯一的提示行。只用 ASCII：运行期字面量含非 ASCII 会在中文代码页的机器上触发编译器的字符集警告
inline constexpr const char* kFallbackHintLine = "Panic, Resume and Loop are native and work without the web UI.";

// 兜底面板：WebView 运行时缺失或过旧、看门狗超时、前端启动失败时的原生面板，平台无关。
// 网页界面起不来时，这是用户唯一能碰到的东西，所以三个安全相关的动作（急停、恢复、环路开关）放在这里，
// 它们不依赖网页。动作本身由别处实现，这里只提供按钮、禁用态与回调钩子：
// 恢复的冷却时间、命令序号都不归本面板管，面板只问「现在能不能点恢复」。
// 固定像素布局，不随缩放档位变化；切到兜底时宿主负责把窗口放大到不小于设计盒，
// 否则小档位（如 33%）会挤压或裁掉按钮。
// 只在消息线程使用
class FallbackPanel final : public juce::Component
{
public:
    // 三个原生动作的钩子。任何一个为空，对应按钮就禁用；钩子在消息线程被调用
    struct ControlHooks
    {
        std::function<void()> onPanic;
        std::function<void()> onResume;
        std::function<void(bool)> onLoopToggle; // 参数是点击后期望的新状态
        // 现在能不能点恢复（例如冷却中返回 false）。为空表示不能点
        std::function<bool()> resumeEnabled;
        // 环路开关的当前状态，用来画按钮的开关态。为空按关处理
        std::function<bool()> loopOn;
    };

    struct Options
    {
        juce::String title;
        juce::String message;
        // 诊断行（可空）：已等待时长、最后导航状态、运行时版本、前端错误摘要。小字等宽显示。
        // 同一份文本由宿主同步写本机日志，用户回报问题时二者取其一即可。
        // 面板上的字会被用户截图转发，所以调用方只放不带路径的内容
        juce::String details;
        bool showInstall = false; // 运行时缺失或过旧时显示「安装」按钮，其余不显示
        std::function<void()> onInstall;
        std::function<void()> onRetry;
        ControlHooks controls;
    };

    explicit FallbackPanel(Options options);
    ~FallbackPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    // 按钩子重新求一遍三个动作按钮的禁用态与开关态。宿主在定时器里调，点击后也会自己调一次
    void refreshControls();

private:
    Options options_;
    juce::Label title_;
    juce::Label message_;
    juce::Label details_;
    juce::Label hint_;
    juce::TextButton panic_;
    juce::TextButton resume_;
    juce::TextButton loop_;
    juce::TextButton install_;
    juce::TextButton retry_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FallbackPanel)
};

} // namespace rinne::webview
