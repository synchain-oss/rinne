// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rinne Output 的处理器。宿主的音频永远逐位直通，Output 只把它写进环（外壳阶段还没有环；探针构建里由 ProbeHost 接上）。
#pragma once

#include "RinneProcessorBase.h"

namespace rinne
{

class OutputProcessor final : public RinneProcessorBase
{
public:
    OutputProcessor();

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    // 旁路时宿主的音频同样直通，写进环的是零，写头照常推进（环接上之后在这里写零）
    void processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

    // 定义在 OutputPluginEntry.cpp
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OutputProcessor)
};

} // namespace rinne
