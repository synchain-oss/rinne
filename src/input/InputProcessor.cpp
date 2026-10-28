// SPDX-License-Identifier: GPL-3.0-or-later

#include "InputProcessor.h"

#include "InputBridgeApi.h"
#include "util/RinneVersion.h"

namespace rinne
{

InputProcessor::InputProcessor() : InputProcessor(InstanceHostConfig{}) {}

InputProcessor::InputProcessor(const InstanceHostConfig& config)
{
    if constexpr (!build::kProbeBuild)
        wiring_.emplace(*this);
    attachHost(Side::Input, config);
    // 新实例的命令计数都从 0 起，基线就是当时的值
    if constexpr (!build::kProbeBuild)
        instanceHost().setCommandBaseline(wiring_->presets.presetSeq(), wiring_->glue.stateSeq());
}

// 输入与输出声道数相同（isBusesLayoutSupported 保证），宿主给的缓冲本身就是干声，引擎在上面就地加回送
void InputProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    if (!beginBlock(buffer.getNumSamples()))
        return;
    process(buffer, false);
}

void InputProcessor::processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    if (!beginBlock(buffer.getNumSamples()))
        return;
    process(buffer, true);
}

void InputProcessor::process(juce::AudioBuffer<float>& buffer, bool bypassed) noexcept
{
    if constexpr (build::kProbeBuild)
    {
        processProbe(buffer, bypassed);
    }
    else
    {
        engine::EngineParams params;
        // 先参数快照、再 acquire 栅栏、再读 stateSeq 与 presetSeq：读到新参数值时序号必定已经加过（见 snapshotBlock）
        snapshotBlock(wiring_->params, wiring_->glue, wiring_->presets, params);
        instanceHost().process(buffer, params, bypassed);
    }
}

void InputProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    if constexpr (build::kProbeBuild)
        RinneProcessorBase::getStateInformation(destData);
    else
        wiring_->glue.getState(destData);
}

void InputProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    if constexpr (build::kProbeBuild)
        RinneProcessorBase::setStateInformation(data, sizeInBytes);
    else
        loadHostState(wiring_->glue, data, sizeInBytes);
}

void InputProcessor::panic() noexcept
{
    instanceHost().panic();
}

void InputProcessor::resumeLoop() noexcept
{
    instanceHost().recover();
}

bool InputProcessor::canResumeLoop() const noexcept
{
    return instanceHost().canRecover();
}

void InputProcessor::setLoopOnFromUi(bool on)
{
    if constexpr (build::kProbeBuild)
    {
        juce::ignoreUnused(on);
    }
    else
    {
        auto* parameter = wiring_->params.tree().getParameter(input::bridge::kParamLoopOn);
        jassert(parameter != nullptr);
        if (parameter == nullptr)
            return;
        wiring_->glue.markUserAction();
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost(on ? 1.0f : 0.0f);
        parameter->endChangeGesture();
    }
}

bool InputProcessor::isLoopOn() const noexcept
{
    if constexpr (build::kProbeBuild)
    {
        return false;
    }
    else
    {
        const auto* value = wiring_->params.tree().getRawParameterValue(input::bridge::kParamLoopOn);
        return value != nullptr && value->load(std::memory_order_relaxed) >= 0.5f;
    }
}

bool InputProcessor::selectGroup(int group)
{
    if constexpr (build::kProbeBuild)
    {
        juce::ignoreUnused(group);
        return false;
    }
    else
    {
        return pickGroup(wiring_->glue, group);
    }
}

void InputProcessor::userParamGestureBegan()
{
    if constexpr (!build::kProbeBuild)
        wiring_->glue.markUserAction();
}

} // namespace rinne
