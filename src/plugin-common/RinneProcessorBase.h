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
// 正式构建的配对层（智能默认、转正、标签、配对状态、诊断）在 InstanceHost 里，这里只做接线：胶水的转正回调与智能认领写
// 设置的回调、工程恢复把组号交给会合、一键修复、诊断文本与复制诊断。诊断日志是本机全局设置（UiDefaultsStore）：
// 每个实例第一次 prepareToPlay 时开关开着、又不在验证宿主里才装上。不在构造时装：扫描插件、生成 moduleinfo 这类只构造、
// 不处理音频的宿主不该写日志；代价是 prepare 之前的事件不进文件日志。装上之后把 IPC 诊断的转发出口放回最上面。
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "InstanceHost.h"
#include "diag/DiagReport.h"
#include "ipc/PairingPolicy.h"
#include "ipc/Rendezvous.h"
#include "ipc/SegmentLayout.h"
#include "pairing/PairStateModel.h"
#include "state/StateCodec.h"
#include "state/StateGlueCommon.h"
#include "state/StateMigration.h"

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

    // ---- 配对与诊断（消息线程；桥不接，界面卡接）。探针构建里是默认值或空操作 ----
    // 上一拍折出的配对状态（一份拷贝）
    pairing::PairModel pairState() const;
    // 诊断日志开关（本机全局设置，不进工程，不定型）：写设置；打开而且不在验证宿主里就当场装上（不等 prepare）。
    // 关掉之后本进程继续写到退出：PlatformLog 没有生产可用的停写
    void setDiagLogEnabled(bool enabled);

    // 一键修复的结果。group 为 0 = 现在不在冲突里或没有可去的组；needsConfirm 而没确认时什么都不动（applied 为假），
    // 界面问过用户再带着确认调一次
    struct RepairOutcome
    {
        std::uint32_t group = 0;
        bool needsConfirm = false;
        bool applied = false;
    };

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
    // 不是智能默认），tail_mode 交给 setTailMode。是不是全量应用、换上的是哪份设置，都用胶水在锁里定下交回的这一次的
    // 结果：不另读定型状态、不另判，也不在返回之后再读 configs()，别的线程在两步之间定型或改设置都不会让两边结论不一。
    // 写回保留、Corrupt 保持当前、编码失败交空块都在胶水里，这里不另做。
    // 整个过程包在 StateLoadScope 里：期间不做智能认领，胶水应用与交给会合之间不会被改成智能组
    // （见 InstanceHost::beginStateLoad）
    template<typename Glue>
    void loadHostState(Glue& glue, const void* data, int sizeInBytes)
    {
        const StateLoadScope scope(*host_);
        const auto result = glue.setState(data, sizeInBytes);
        if (result.mode == state::glue::ApplyMode::Full && result.applied.has_value())
            restoreConfigs(*result.applied);
    }

    // 配对层的两个回调：胶水的转正回调转给会合（每次 getState、每次 setState 处理完、定型那一刻）；智能认领把组号
    // 写进胶水的设置（来源 Smart，不定型、不标脏）。子类建好胶水与 InstanceHost 之后、宿主第一次调 state 之前调一次；
    // 子类析构时先调 unwirePairing（胶水先于 InstanceHost 析构）
    template<typename Glue>
    void wirePairing(Glue& glue)
    {
        glue.setConfirmClaimHook([this] { host_->confirmClaim(); });
        host_->setSmartGroupSink([&glue](std::uint32_t group) {
            glue.editConfigs([group](state::Configs& configs) {
                configs.group = static_cast<std::uint8_t>(group);
                configs.groupOrigin = state::GroupOrigin::Smart;
            });
        });
    }
    void unwirePairing();

    // 冲突的一键修复（消息线程）：Input 移到空闲组，Output 重新配对（按智能默认重选）。属于界面上的设置改动：
    // 写回设置（来源记为用户选的）、交给会合（目标是休眠槽时要用户确认，意图是 kManualConfirmed）、定型、标脏
    template<typename Glue>
    RepairOutcome repairWith(Glue& glue, bool confirmTakeover)
    {
        const ipc::RepairPlan plan = host_->planRepair();
        RepairOutcome outcome;
        outcome.group = plan.group;
        outcome.needsConfirm = plan.needsConfirm;
        if (plan.group == 0 || (plan.needsConfirm && !confirmTakeover))
            return outcome;
        glue.editConfigs([&plan](state::Configs& configs) {
            configs.group = static_cast<std::uint8_t>(plan.group);
            configs.groupOrigin = state::GroupOrigin::User;
        });
        host_->setGroup(plan.group, plan.needsConfirm ? ipc::ClaimIntent::kManualConfirmed : ipc::ClaimIntent::kManual);
        glue.markUserAction();
        glue.markStateDirty();
        outcome.applied = true;
        return outcome;
    }

    // 诊断文本：InstanceHost 填各项，state 一段由子类从胶水填。消息线程
    std::string diagnosticsWith(const diag::DiagState& stateSection) const;
    // 复制诊断：同一份文本交给系统剪贴板（测试注入替身）
    void copyText(const std::string& text) const;
    static diag::DiagLoadStatus loadStatusOf(const std::optional<state::StateLoadStatus>& status) noexcept;

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
    // beginStateLoad 与 endStateLoad 成对（胶水抛出时也一样）
    class StateLoadScope
    {
    public:
        explicit StateLoadScope(InstanceHost& host) : host_(host) { host_.beginStateLoad(); }
        ~StateLoadScope() { host_.endStateLoad(); }
        StateLoadScope(const StateLoadScope&) = delete;
        StateLoadScope& operator=(const StateLoadScope&) = delete;

    private:
        InstanceHost& host_;
    };

    static bool detectValidationHost();
    // 全量应用之后：组号交给会合（工程恢复），tail_mode 交给 setTailMode
    void restoreConfigs(const state::Configs& configs);
    // 装诊断日志（正式构建、不在验证宿主里才调）。这次真的装上了就把 IPC 诊断的转发出口放回最上面
    void installDiagLog();
    // 第一次 prepare：开关开着、不在验证宿主里、本插件二进制还没装过日志就装（见文件头）
    void installDiagLogOnFirstPrepare();
    const char* roleName() const noexcept;

    static_assert(std::atomic<bool>::is_always_lock_free);
    static_assert(std::atomic<int>::is_always_lock_free);
    static_assert(std::atomic<std::int64_t>::is_always_lock_free);

    std::atomic<int> tailMode_{static_cast<int>(TailMode::Auto)};
    std::atomic<std::int64_t> processedSamples_{0};
    // 第一次 prepare 已经看过诊断日志开关：之后的 prepare 不再读设置文件。宿主可能在不同线程上先后 prepare
    std::atomic<bool> diagLogChecked_{false};
    const bool validationHost_;
    ipc::Role role_ = ipc::Role::kInput;
    bool (*installLog_)(const juce::String& role) = nullptr; // 空 = platformlog::install
    void (*copyToClipboard_)(const juce::String& text) = nullptr; // 空 = juce::SystemClipboard
    std::unique_ptr<InstanceHost> host_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RinneProcessorBase)
};

} // namespace rinne
