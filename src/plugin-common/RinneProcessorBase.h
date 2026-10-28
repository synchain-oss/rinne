// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rinne Input 与 Rinne Output 共用的处理器外壳：两端对宿主的约定都在这里，各自只写音频回调、state 与编辑器。
//   - 延迟恒报 0，永不改变：报了延迟，宿主的延迟补偿会把环路延迟抵掉，或在限制补偿时把插件整个旁路。
//   - tail 让宿主在静音时也不挂起插件，否则环路会在安静段断开：自动模式下 VST3 报无限长，AU 报 30 秒。
//   - 总线：单声道或立体声，输入与输出声道数相同。
//   - 0 样本的回调立即返回，不推进任何计数（宿主用它来刷参数，不代表时间前进）。
//   - 验证宿主（pluginval、auval、AppleInfoHelper、VBVSTScanner）在构造时识别一次；在它们下面只出干声，不开共享内存段。
//   - program 只有一个，名字固定为插件名：界面改设置后标脏带 programChanged，AU 宿主会因此重读 program 名。
// 不自建旁路参数，旁路由包装层的 bypass 参数驱动，两端各自重写 processBlockBypassed。
// 每个实例持有一个 InstanceHost（后端、会合、引擎、tick、命令序号）：正式构建里就是 InstanceHost 本身，
// 探针构建里是建在它之上的 probe::ProbeHost（探针日志、面板与探针 state）。prepare、release、reset 两种构建都转给它；
// 探针构建另把 state、轨道信息、音频与编辑器转给 ProbeHost，正式构建里那一支是 `if constexpr` 的弃置分支，
// 不引用 ProbeHost 的定义。正式构建的参数层与 state 胶水由两个子类持有。
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>

#include "InstanceHost.h"
#include "ipc/Rendezvous.h"
#include "ipc/SegmentLayout.h"
#include "state/StateCodec.h"
#include "state/StateGlueCommon.h"

namespace rinne
{

class RinneProcessorBase : public juce::AudioProcessor
{
public:
    // 与 state 里 tail_mode 的取值一一对应，顺序不能改（RinneProcessorBase.cpp 里与 state::TailMode 逐值断言）
    enum class TailMode : int
    {
        Auto = 0,
        Infinite = 1,
        ThirtySeconds = 2,
    };

    ~RinneProcessorBase() override;

    void prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    void reset() override;
    void updateTrackProperties(const TrackProperties& properties) override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

    double getTailLengthSeconds() const override;
    void setTailMode(TailMode mode) noexcept;
    TailMode getTailMode() const noexcept;

    const juce::String getName() const override;
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return getName(); }
    void changeProgramName(int, const juce::String&) override {}

    // 探针构建：探针 state；正式构建由子类经胶水重写
    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // 本实例是否跑在验证宿主里；构造后不变
    bool isValidationHost() const noexcept { return validationHost_; }
    // 实例建立以来音频回调处理过的样本数（旁路也算，0 样本的回调不算）
    std::int64_t getProcessedSamples() const noexcept;
    // 本实例的接线；子类构造完之后一直在。外部只为诊断与测试读它、调它的 tick（宿主里由它自己的定时器调）
    InstanceHost& instanceHost() noexcept { return *host_; }
    const InstanceHost& instanceHost() const noexcept { return *host_; }

protected:
    RinneProcessorBase();

    // 每个音频回调最先调用。0 样本返回 false，调用方立即返回；否则推进样本计数、返回 true。只在音频线程上调用。
    bool beginBlock(int numSamples) noexcept;

    enum class Side
    {
        Input,
        Output,
    };
    // 子类构造时调一次（正式构建先建好参数层与胶水再调）。config 里只有测试注入项算数：
    // role 与 validationHost 由这里填，注入覆盖不了；探针构建按同样的注入项建 ProbeHost
    void attachHost(Side side, const InstanceHostConfig& config);

    // ---- 探针构建的转发（正式构建里都是空操作） ----
    // 音频回调在 beginBlock 之后调
    void processProbe(juce::AudioBuffer<float>& buffer, bool bypassed) noexcept;
    // 探针面板；没有探针时返回空，调用方用自己的编辑器
    juce::AudioProcessorEditor* createProbeEditor();

    // ---- 正式构建：两端胶水共用的两段接线（Glue 是 InputStateGlue 或 OutputStateGlue，两者的这几个接口同名） ----
    // 宿主 setState：胶水按定型规则应用。这一次是全量应用（定型前、成功读入）时，设置里的组号交给会合（意图是工程恢复，
    // 不是智能默认），tail_mode 交给 setTailMode。是不是全量应用用胶水自己的那个判定（applyModeFor），不另写一份；
    // 读定型状态与 setState 之间若另一个线程恰好定型，按读到的算（宿主与界面都在消息线程上时不会发生）。
    // 写回保留、Corrupt 保持当前、编码失败交空块都在胶水里，这里不另做
    template<typename Glue>
    void loadHostState(Glue& glue, const void* data, int sizeInBytes)
    {
        const bool settled = glue.isSettled();
        glue.setState(data, sizeInBytes);
        const std::optional<state::StateLoadStatus> status = glue.lastLoadStatus();
        if (status.has_value() && state::glue::applyModeFor(*status, settled) == state::glue::ApplyMode::Full)
            restoreConfigs(glue.configs());
    }

    // 界面上手动选组（组号 0..8，0 = 离组）：写回设置（来源记为用户选的）、交给会合、定型、告诉宿主工程有改动。
    // 越界返回 false，什么都不动
    template<typename Glue>
    bool pickGroup(Glue& glue, int group)
    {
        if (group < 0 || group > static_cast<int>(ipc::kGroupCount))
            return false;
        glue.editConfigs([group](state::Configs& configs) {
            configs.group = static_cast<std::uint8_t>(group);
            configs.groupOrigin = state::GroupOrigin::User;
        });
        host_->setGroup(static_cast<std::uint32_t>(group), ipc::ClaimIntent::kManual);
        glue.markUserAction();
        glue.markStateDirty();
        return true;
    }

private:
    static bool detectValidationHost();
    // 全量应用之后：组号交给会合（工程恢复），tail_mode 交给 setTailMode
    void restoreConfigs(const state::Configs& configs);

    static_assert(std::atomic<int>::is_always_lock_free);
    static_assert(std::atomic<std::int64_t>::is_always_lock_free);

    std::atomic<int> tailMode_{static_cast<int>(TailMode::Auto)};
    std::atomic<std::int64_t> processedSamples_{0};
    const bool validationHost_;
    std::unique_ptr<InstanceHost> host_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RinneProcessorBase)
};

} // namespace rinne
