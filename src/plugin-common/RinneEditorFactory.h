// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>

#include "FallbackPanel.h"
#include "ResourceProvider.h"

namespace rinne::editor
{
// 编辑器工厂：两个插件的 createEditor 都经这里造编辑器，插件壳不直接认识 WebView 宿主。
//
// 身份只有两种：
//   正式身份：造网页编辑器（WebViewHost），加载占位页或正式界面；
//   探针身份：造探针编辑器。探针构建是借来的机器上做实测用的，界面是一块只读面板，不加载网页。
// 判定取构建时的探针标志，与处理器基类里「探针构建才跑探针逻辑」是同一个真源，不再另起一个编译定义。
// 探针编辑器的构造不在本工厂里：它由插件壳注入（处理器基类的受保护方法造它），
// 这样本工厂不必包含探针编辑器的头文件，正式构建里也不会把探针代码拖进来。
enum class Identity
{
    formal,
    probe
};

// 本次构建的身份
Identity currentIdentity() noexcept;

struct Options
{
    juce::String role; // "input" 或 "output"，其余值造不出编辑器

    // 页面资源（插件自己的二进制数据，由插件壳填）。暂无可嵌资源则留空
    webview::ResourceProvider::Source resources;

    // 兜底面板上急停、恢复、环路开关的钩子，原样交给网页编辑器
    webview::FallbackPanel::ControlHooks controls;

    // 探针身份才用，由插件壳注入。返回新建的探针编辑器（调用方接管所有权）。身份是探针而这里为空时造不出编辑器
    std::function<juce::AudioProcessorEditor*()> probeEditorFactory;

    // 网页编辑器的构造缝，单测注入替身，避开真实的浏览器组件。为空时造真正的网页编辑器
    // （只有开了浏览器组件的构建里才有，宿主测试目标里没有）
    std::function<juce::AudioProcessorEditor*(juce::AudioProcessor&)> webViewEditorMaker;
};

// 造编辑器。返回新建的对象，调用方接管所有权；造不出（角色不对、探针注入缺失、构建里没有浏览器组件）返回空指针，
// 插件壳应当回退到通用编辑器，而不是崩溃。identity 默认取本次构建的身份，单测显式传另一个值
juce::AudioProcessorEditor* createEditor(juce::AudioProcessor& processor, const Options& options,
                                         Identity identity = currentIdentity());

// 插件壳的入口：createEditor 造不出就退回 JUCE 的通用参数编辑器，始终返回新建的对象（调用方接管所有权）。
// 两个插件壳的 createEditor 都只调这一个函数，回退规则因此只写一处、单测守得到
juce::AudioProcessorEditor* createEditorOrGeneric(juce::AudioProcessor& processor, const Options& options,
                                                  Identity identity = currentIdentity());

} // namespace rinne::editor
