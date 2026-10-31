// SPDX-License-Identifier: GPL-3.0-or-later

#include "InputProcessor.h"

#include <utility>

#include "InputBridgeApi.h"
#include "util/RinneVersion.h"

namespace rinne
{

InputProcessor::InputProcessor() : InputProcessor(InstanceHostConfig{}) {}

InputProcessor::InputProcessor(const InstanceHostConfig& config, const juce::File& presetDirectory)
{
    if constexpr (!build::kProbeBuild)
        wiring_.emplace(*this, presetDirectory);
    attachHost(Side::Input, config);
    // 新实例的命令计数都从 0 起，基线就是当时的值
    if constexpr (!build::kProbeBuild)
    {
        instanceHost().setCommandBaseline(wiring_->presets.presetSeq(), wiring_->glue.stateSeq());
        wirePairing(wiring_->glue);
    }
}

// 胶水先于 InstanceHost 析构：先撤下智能认领写胶水的回调
InputProcessor::~InputProcessor()
{
    if constexpr (!build::kProbeBuild)
        unwirePairing();
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
    hostBypassed_.store(bypassed, std::memory_order_relaxed);
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

void InputProcessor::updateTrackProperties(const TrackProperties& properties)
{
    // 名字与颜色各自独立：宿主这次没给的项沿用上次的（AU 只给名字），空名字不算给了
    if (properties.name.has_value() && properties.name->isNotEmpty())
        trackLabel_.name = properties.name;
    if (properties.colour.has_value())
        trackLabel_.colour = properties.colour;
    RinneProcessorBase::updateTrackProperties(properties);
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

RinneProcessorBase::RepairOutcome InputProcessor::repairGroup(bool confirmTakeover)
{
    if constexpr (build::kProbeBuild)
    {
        juce::ignoreUnused(confirmTakeover);
        return {};
    }
    else
    {
        return repairWith(wiring_->glue, confirmTakeover);
    }
}

bool InputProcessor::takeOverGroup(int group)
{
    if constexpr (build::kProbeBuild)
    {
        juce::ignoreUnused(group);
        return false;
    }
    else
    {
        if (group < 1 || group > static_cast<int>(ipc::kGroupCount))
            return false;
        wiring_->glue.editConfigs([group](state::Configs& configs) {
            configs.group = static_cast<std::uint8_t>(group);
            configs.groupOrigin = state::GroupOrigin::User;
        });
        instanceHost().setGroup(static_cast<std::uint32_t>(group), ipc::ClaimIntent::kManualConfirmed);
        wiring_->glue.markUserAction();
        wiring_->glue.markStateDirty();
        return true;
    }
}

template<typename Edit>
bool InputProcessor::applyUserConfig(Edit&& edit)
{
    if constexpr (build::kProbeBuild)
    {
        juce::ignoreUnused(edit);
        return false;
    }
    else
    {
        wiring_->glue.editConfigs(std::forward<Edit>(edit));
        wiring_->glue.markUserAction();
        wiring_->glue.markStateDirty();
        return true;
    }
}

bool InputProcessor::selectTailMode(state::TailMode mode)
{
    if (!applyUserConfig([mode](state::Configs& configs) { configs.tailMode = mode; }))
        return false;
    // 外壳的 TailMode 与 state::TailMode 逐值相同（RinneProcessorBase.cpp 里有逐值断言）
    setTailMode(static_cast<TailMode>(static_cast<int>(mode)));
    return true;
}

bool InputProcessor::selectGuardOn(bool on)
{
    return applyUserConfig([on](state::Configs& configs) { configs.guardOn = on; });
}

bool InputProcessor::selectResetOnPlay(bool on)
{
    return applyUserConfig([on](state::Configs& configs) { configs.resetOnPlay = on; });
}

bool InputProcessor::selectFbUnit(state::FbUnit unit)
{
    return applyUserConfig([unit](state::Configs& configs) { configs.fbUnit = unit; });
}

bool InputProcessor::selectDelayUnit(state::DelayUnit unit)
{
    return applyUserConfig([unit](state::Configs& configs) { configs.delayUnit = unit; });
}

std::string InputProcessor::diagnosticsText() const
{
    if constexpr (build::kProbeBuild)
    {
        return {};
    }
    else
    {
        const input::InputStateGlue& glue = wiring_->glue;
        diag::DiagState section;
        section.lastLoad = loadStatusOf(glue.lastLoadStatus());
        section.droppedUnknownParams = glue.droppedUnknownParams();
        section.encodeFailures = glue.encodeFailures();
        section.holdingNewer = glue.holdingNewerState();
        return diagnosticsWith(section);
    }
}

void InputProcessor::copyDiagnosticsToClipboard() const
{
    if constexpr (!build::kProbeBuild)
        copyText(diagnosticsText());
}

} // namespace rinne
