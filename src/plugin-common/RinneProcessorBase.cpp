// SPDX-License-Identifier: GPL-3.0-or-later

#include "RinneProcessorBase.h"

#include <limits>
#include <utility>

#include "ProbeHost.h"
#include "util/RinneVersion.h"

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

void RinneProcessorBase::prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock)
{
    if constexpr (build::kProbeBuild)
    {
        if (probe_ != nullptr)
            probe_->prepare(sampleRate, maximumExpectedSamplesPerBlock);
    }
    else
    {
        juce::ignoreUnused(sampleRate, maximumExpectedSamplesPerBlock);
    }
}

void RinneProcessorBase::releaseResources()
{
    if constexpr (build::kProbeBuild)
    {
        if (probe_ != nullptr)
            probe_->release();
    }
}

void RinneProcessorBase::reset()
{
    if constexpr (build::kProbeBuild)
    {
        if (probe_ != nullptr)
            probe_->reset();
    }
}

void RinneProcessorBase::updateTrackProperties(const TrackProperties& properties)
{
    if constexpr (build::kProbeBuild)
    {
        if (probe_ != nullptr)
            probe_->trackProperties(properties);
    }
    else
    {
        juce::ignoreUnused(properties);
    }
}

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

// 外壳阶段没有任何状态要存；state 容器接上之前，存出空块、读入时忽略（探针构建里存取的是探针 state）
void RinneProcessorBase::getStateInformation(juce::MemoryBlock& destData)
{
    if constexpr (build::kProbeBuild)
    {
        if (probe_ != nullptr)
        {
            probe_->getState(destData);
            return;
        }
    }
    destData.reset();
}

void RinneProcessorBase::setStateInformation(const void* data, int sizeInBytes)
{
    if constexpr (build::kProbeBuild)
    {
        if (probe_ != nullptr)
            probe_->setState(data, sizeInBytes);
    }
    else
    {
        juce::ignoreUnused(data, sizeInBytes);
    }
}

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

void RinneProcessorBase::attachProbe(Side side)
{
    if constexpr (build::kProbeBuild)
    {
        probe::ProbeHostConfig config;
        config.role = side == Side::Input ? ipc::Role::kInput : ipc::Role::kOutput;
        config.validationHost = validationHost_;
        probe_.reset(new probe::ProbeHost(*this, std::move(config)));
    }
    else
    {
        juce::ignoreUnused(side);
    }
}

void RinneProcessorBase::processProbe(juce::AudioBuffer<float>& buffer, bool bypassed) noexcept
{
    if constexpr (build::kProbeBuild)
    {
        if (probe_ != nullptr)
            probe_->process(buffer, bypassed);
    }
    else
    {
        juce::ignoreUnused(buffer, bypassed);
    }
}

juce::AudioProcessorEditor* RinneProcessorBase::createProbeEditor()
{
    if constexpr (build::kProbeBuild)
    {
        if (probe_ != nullptr)
            return probe_->createEditor();
    }
    return nullptr;
}

void RinneProcessorBase::destroyProbe(probe::ProbeHost* host) noexcept
{
    if constexpr (build::kProbeBuild)
        delete host;
    else
        juce::ignoreUnused(host);
}

} // namespace rinne
