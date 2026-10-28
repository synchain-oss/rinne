// SPDX-License-Identifier: GPL-3.0-or-later

#include "OutputProcessor.h"

#include "util/RinneVersion.h"

namespace rinne
{

OutputProcessor::OutputProcessor() : OutputProcessor(InstanceHostConfig{}) {}

OutputProcessor::OutputProcessor(const InstanceHostConfig& config)
{
    if constexpr (!build::kProbeBuild)
        glue_.emplace(*this);
    attachHost(Side::Output, config);
}

// Output 永远不改宿主的缓冲：输入与输出声道数相同，引擎只读缓冲、写环
void OutputProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    if (!beginBlock(buffer.getNumSamples()))
        return;
    process(buffer, false);
}

void OutputProcessor::processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    if (!beginBlock(buffer.getNumSamples()))
        return;
    process(buffer, true);
}

void OutputProcessor::process(juce::AudioBuffer<float>& buffer, bool bypassed) noexcept
{
    if constexpr (build::kProbeBuild)
        processProbe(buffer, bypassed);
    else
        instanceHost().process(buffer, engine::EngineParams{}, bypassed);
}

void OutputProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    if constexpr (build::kProbeBuild)
        RinneProcessorBase::getStateInformation(destData);
    else
        glue_->getState(destData);
}

void OutputProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if constexpr (build::kProbeBuild)
        RinneProcessorBase::setStateInformation(data, sizeInBytes);
    else
        loadHostState(*glue_, data, sizeInBytes);
}

bool OutputProcessor::selectGroup(int group)
{
    if constexpr (build::kProbeBuild)
    {
        juce::ignoreUnused(group);
        return false;
    }
    else
    {
        return pickGroup(*glue_, group);
    }
}

} // namespace rinne
