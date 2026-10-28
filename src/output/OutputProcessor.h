// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rinne Output 的处理器：环路的发送端。宿主的音频永远逐位直通，Output 只把它写进环；配置的唯一真源在 Input，
// 这里只有组与配对。
// 正式构建：state 胶水（OutputStateGlue）→ 接线（InstanceHost，基类持有），按这个顺序建。
// 每块交给 InstanceHost 的引擎参数是默认值（Output 引擎不读参数）。
// 探针构建：不建胶水，state 与面板都在 probe::ProbeHost 里（见 RinneProcessorBase）。
// 界面动作的入口在下面，消息线程调用；哪些算用户动作（让实例定型）以 OutputStateGlue.h 文件头的清单为准，
// 这里不另列。
#pragma once

#include <optional>

#include "OutputStateGlue.h"
#include "RinneProcessorBase.h"

namespace rinne
{

class OutputProcessor final : public RinneProcessorBase
{
public:
    OutputProcessor();
    // 测试注入（命名作用域、后端、时钟、定时器）；role 与验证宿主由基类填
    explicit OutputProcessor(const InstanceHostConfig& config);

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    // 旁路：宿主的音频同样直通；写进环的数据 10 ms 淡到 0，写头照常推进
    void processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // 定义在 OutputPluginEntry.cpp
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    // ---- 界面动作（消息线程） ----
    // 手动选组（0..8，0 = 离组）：定型并标脏。越界返回 false
    bool selectGroup(int group);

    // ---- 读出（诊断与测试）。正式构建才有，探针构建里为空 ----
    output::OutputStateGlue* stateGlue() noexcept { return glue_.has_value() ? &*glue_ : nullptr; }

private:
    void process(juce::AudioBuffer<float>& buffer, bool bypassed) noexcept;

    std::optional<output::OutputStateGlue> glue_; // 只在正式构建里建

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OutputProcessor)
};

} // namespace rinne
