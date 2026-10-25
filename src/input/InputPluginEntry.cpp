// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rinne Input 的插件入口与编辑器工厂。单独成文件，宿主测试编处理器时可以不带它，也就不必链 WebView2。

#include "InputProcessor.h"

namespace rinne
{

// 界面接上之前先用 JUCE 的通用参数编辑器；探针构建里是探针面板
juce::AudioProcessorEditor* InputProcessor::createEditor()
{
    if (auto* editor = createProbeEditor())
        return editor;
    return new juce::GenericAudioProcessorEditor(*this);
}

} // namespace rinne

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new rinne::InputProcessor();
}
