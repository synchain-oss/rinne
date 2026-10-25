// SPDX-License-Identifier: GPL-3.0-or-later

#include "OutputProcessor.h"

namespace rinne
{

OutputProcessor::OutputProcessor()
{
    attachProbe(Side::Output);
}

// Output 永远不改宿主的缓冲：输入与输出声道数相同，一个样本都不写就是逐位直通。
void OutputProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    if (!beginBlock(buffer.getNumSamples()))
        return;
    processProbe(buffer, false);
}

void OutputProcessor::processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    if (!beginBlock(buffer.getNumSamples()))
        return;
    processProbe(buffer, true);
}

} // namespace rinne
