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
    if constexpr (!build::kProbeBuild)
        wirePairing(*glue_);
}

// 胶水先于 InstanceHost 析构：先撤下智能认领写胶水的回调
OutputProcessor::~OutputProcessor()
{
    if constexpr (!build::kProbeBuild)
        unwirePairing();
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

void OutputProcessor::updateTrackProperties(const TrackProperties& properties)
{
    // 名字与颜色各自独立：宿主这次没给的项沿用上次的（AU 只给名字），空名字不算给了
    if (properties.name.has_value() && properties.name->isNotEmpty())
        trackLabel_.name = properties.name;
    if (properties.colour.has_value())
        trackLabel_.colour = properties.colour;
    RinneProcessorBase::updateTrackProperties(properties);
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

RinneProcessorBase::RepairOutcome OutputProcessor::repairGroup(bool confirmTakeover)
{
    if constexpr (build::kProbeBuild)
    {
        juce::ignoreUnused(confirmTakeover);
        return {};
    }
    else
    {
        return repairWith(*glue_, confirmTakeover);
    }
}

bool OutputProcessor::takeOverGroup(int group)
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
        glue_->editConfigs([group](state::Configs& configs) {
            configs.group = static_cast<std::uint8_t>(group);
            configs.groupOrigin = state::GroupOrigin::User;
        });
        instanceHost().setGroup(static_cast<std::uint32_t>(group), ipc::ClaimIntent::kManualConfirmed);
        glue_->markUserAction();
        glue_->markStateDirty();
        return true;
    }
}

bool OutputProcessor::selectTailMode(state::TailMode mode)
{
    if constexpr (build::kProbeBuild)
    {
        juce::ignoreUnused(mode);
        return false;
    }
    else
    {
        glue_->editConfigs([mode](state::Configs& configs) { configs.tailMode = mode; });
        // 外壳的 TailMode 与 state::TailMode 逐值相同（RinneProcessorBase.cpp 里有逐值断言）
        setTailMode(static_cast<TailMode>(static_cast<int>(mode)));
        glue_->markUserAction();
        glue_->markStateDirty();
        return true;
    }
}

std::string OutputProcessor::diagnosticsText() const
{
    if constexpr (build::kProbeBuild)
    {
        return {};
    }
    else
    {
        diag::DiagState section;
        section.lastLoad = loadStatusOf(glue_->lastLoadStatus());
        section.encodeFailures = glue_->encodeFailures();
        section.holdingNewer = glue_->holdingNewerState();
        return diagnosticsWith(section);
    }
}

void OutputProcessor::copyDiagnosticsToClipboard() const
{
    if constexpr (!build::kProbeBuild)
        copyText(diagnosticsText());
}

} // namespace rinne
