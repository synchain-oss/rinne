// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rinne Input 的处理器。外壳阶段：干声逐位直通，还没有回送。探针构建里由 ProbeHost 接上引擎（见 RinneProcessorBase）。
#pragma once

#include "RinneProcessorBase.h"

namespace rinne
{

class InputProcessor final : public RinneProcessorBase
{
public:
    InputProcessor();

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    // 旁路时干声照样逐位直通（回送接上之后在这里淡出）
    void processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

    // 定义在 InputPluginEntry.cpp
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InputProcessor)
};

} // namespace rinne
