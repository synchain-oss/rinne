// SPDX-License-Identifier: GPL-3.0-or-later

#include "RinneProcessorBase.h"

#include <limits>

namespace rinne
{

RinneProcessorBase::RinneProcessorBase()
    : juce::AudioProcessor(BusesProperties()
                               .withInput("Input", juce::AudioChannelSet::stereo(), true)
                               .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      validationHost_(detectValidationHost())
{
    setLatencySamples(0);
}

bool RinneProcessorBase::detectValidationHost()
{
    const juce::PluginHostType host;
    return host.isPluginval() || host.isAUVal() || host.isAppleInfoHelper() || host.isVBVSTScanner();
}

void RinneProcessorBase::prepareToPlay(double, int) {}

void RinneProcessorBase::releaseResources() {}

bool RinneProcessorBase::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto& in = layouts.getMainInputChannelSet();
    const auto& out = layouts.getMainOutputChannelSet();
    return in == out && (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo());
}

double RinneProcessorBase::getTailLengthSeconds() const
{
    switch (getTailMode())
    {
    case TailMode::Infinite:
        return std::numeric_limits<double>::infinity();
    case TailMode::ThirtySeconds:
        return 30.0;
    case TailMode::Auto:
        break;
    }
    // AU 宿主拿无限长的 tail 没有统一的处理，自动模式给 30 秒；VST3 有专门的无限 tail 取值
    const bool au = wrapperType == wrapperType_AudioUnit || wrapperType == wrapperType_AudioUnitv3;
    return au ? 30.0 : std::numeric_limits<double>::infinity();
}

void RinneProcessorBase::setTailMode(TailMode mode) noexcept
{
    tailMode_.store(static_cast<int>(mode), std::memory_order_relaxed);
}

RinneProcessorBase::TailMode RinneProcessorBase::getTailMode() const noexcept
{
    return static_cast<TailMode>(tailMode_.load(std::memory_order_relaxed));
}

const juce::String RinneProcessorBase::getName() const
{
    return JucePlugin_Name;
}

// 外壳阶段没有任何状态要存；state 容器接上之前，存出空块、读入时忽略
void RinneProcessorBase::getStateInformation(juce::MemoryBlock& destData)
{
    destData.reset();
}

void RinneProcessorBase::setStateInformation(const void*, int) {}

std::int64_t RinneProcessorBase::getProcessedSamples() const noexcept
{
    return processedSamples_.load(std::memory_order_relaxed);
}

bool RinneProcessorBase::beginBlock(int numSamples) noexcept
{
    if (numSamples <= 0)
        return false;
    // 只有音频线程写，读一次再写回就够了，不必用读改写
    processedSamples_.store(processedSamples_.load(std::memory_order_relaxed) + numSamples, std::memory_order_relaxed);
    return true;
}

} // namespace rinne
