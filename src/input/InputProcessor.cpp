// SPDX-License-Identifier: GPL-3.0-or-later

#include "InputProcessor.h"

namespace rinne
{

// 输入与输出声道数相同（isBusesLayoutSupported 保证），宿主给的缓冲本身就是干声：一个样本都不写就是逐位直通。
void InputProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    if (!beginBlock(buffer.getNumSamples()))
        return;
}

void InputProcessor::processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    if (!beginBlock(buffer.getNumSamples()))
        return;
}

} // namespace rinne
