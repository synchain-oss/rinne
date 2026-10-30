// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rinne Output 的插件入口与编辑器工厂。单独成文件，宿主测试编处理器时可以不带它，也就不必链 WebView2。

#include <RinneOutputWebData.h> // juce_add_binary_data 生成：嵌入的 web/output 页面资源与许可证文本

#include "OutputEditor.h"
#include "OutputProcessor.h"
#include "RinneEditorFactory.h"

namespace rinne
{

// 正式构建：加载页面的网页编辑器（OutputEditor，带 Output 的原生桥）；探针构建：探针面板（经注入点，由处理器基类造）。
// WebView 运行时缺失或页面起不来时，编辑器自己换上兜底面板；工厂造不出时（例如没有浏览器组件）退回 JUCE 的
// 通用参数编辑器，窗口至少是开得了的。
juce::AudioProcessorEditor* OutputProcessor::createEditor()
{
    editor::Options options;
    options.role = "output";
    // 页面资源必须交给工厂：Source 为空时资源提供者恒返回空，窗口会空白到看门狗超时才落到兜底面板。
    // 探针构建也嵌了这份资源（入口代码两种身份同一份、不为身份另起编译定义），但探针路径从不读它
    options.resources = {RinneOutputWebData::namedResourceListSize, RinneOutputWebData::originalFilenames,
                         RinneOutputWebData::namedResourceList, &RinneOutputWebData::getNamedResource};
    // 兜底面板的急停、恢复、环路开关钩子留空，面板上这三个按钮是禁用的：这是设计。回送、安全链与环路开关都在 Input 端，
    // Output 没有可以急停或恢复的东西
    options.controls = {};
    options.probeEditorFactory = [this] { return createProbeEditor(); };
    // 网页编辑器经工厂的构造缝造：工厂只认识通用的 WebViewHost，Output 的桥函数与事件在 OutputEditor 里
    options.webViewEditorMaker = [this,
                                  resources = options.resources](juce::AudioProcessor&) -> juce::AudioProcessorEditor* {
        return new output::OutputEditor(*this, resources);
    };
    return editor::createEditorOrGeneric(*this, options);
}

} // namespace rinne

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new rinne::OutputProcessor();
}
