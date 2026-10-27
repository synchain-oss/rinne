// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rinne Input 的插件入口与编辑器工厂。单独成文件，宿主测试编处理器时可以不带它，也就不必链 WebView2。

#include <RinneInputWebData.h> // juce_add_binary_data 生成：嵌入的 web/input 页面资源与许可证文本

#include "InputProcessor.h"
#include "RinneEditorFactory.h"

namespace rinne
{

// 正式构建：加载页面的网页编辑器；探针构建：探针面板（经注入点，由处理器基类造）。
// 工厂造不出时（例如没有浏览器组件）退回 JUCE 的通用参数编辑器，窗口至少是开得了的。
juce::AudioProcessorEditor* InputProcessor::createEditor()
{
    editor::Options options;
    options.role = "input";
    // 页面资源必须交给工厂：Source 为空时资源提供者恒返回空，窗口会空白到看门狗超时才落到兜底面板。
    // 探针构建也嵌了这份资源（入口代码两种身份同一份、不为身份另起编译定义），但探针路径从不读它
    options.resources = {RinneInputWebData::namedResourceListSize, RinneInputWebData::originalFilenames,
                         RinneInputWebData::namedResourceList, &RinneInputWebData::getNamedResource};
    // 兜底面板的急停、恢复、环路开关钩子暂时留空，面板上这三个按钮是禁用的。现在没有可以急停的回送；
    // 命令序号与恢复冷却接上之后，钩子在这里填
    options.controls = {};
    options.probeEditorFactory = [this] { return createProbeEditor(); };
    return editor::createEditorOrGeneric(*this, options);
}

} // namespace rinne

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new rinne::InputProcessor();
}
