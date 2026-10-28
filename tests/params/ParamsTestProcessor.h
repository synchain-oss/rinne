// SPDX-License-Identifier: GPL-3.0-or-later
//
// 参数层用例共用的最小处理器：只为承载参数树，不处理音频、没有编辑器。test_input_params.cpp 与
// test_input_params_delay.cpp 都用它，放在这里免得 JUCE 的 AudioProcessor 接口一变要改两处。

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace rinne::test::params
{

struct ParamsTestProcessor final : juce::AudioProcessor
{
    ParamsTestProcessor()
        : juce::AudioProcessor(BusesProperties()
                                   .withInput("In", juce::AudioChannelSet::stereo(), true)
                                   .withOutput("Out", juce::AudioChannelSet::stereo(), true))
    {
    }

    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    const juce::String getName() const override { return "ParamsTest"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}
};

} // namespace rinne::test::params
