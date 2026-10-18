// SPDX-License-Identifier: GPL-3.0-or-later
//
// createEditor 的真实定义在各自的 *PluginEntry.cpp 里，那里会带进编辑器的依赖（WebView2）。宿主测试不编那两个文件，
// 本进程也从不开编辑器，补一个无头实现让虚函数表链接得上。

#include "InputProcessor.h"
#include "OutputProcessor.h"

juce::AudioProcessorEditor* rinne::InputProcessor::createEditor()
{
    return nullptr;
}

juce::AudioProcessorEditor* rinne::OutputProcessor::createEditor()
{
    return nullptr;
}
