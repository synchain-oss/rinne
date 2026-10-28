// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rinne Input 的处理器：环路的返回端，全部调节都在这里。
// 正式构建：参数层（InputParams）→ state 胶水（InputStateGlue，建在参数树上）→ 预设库（PresetStore）→
// 接线（InstanceHost，基类持有），按这个顺序建。每块：参数快照 → 胶水快照（stateSeq 与三个工程开关）→ 预设序号
// （读序见 snapshotBlock）→ InstanceHost（命令序号、走带、租约、引擎）。
// 探针构建：不建参数层与胶水，参数、state 与面板都在 probe::ProbeHost 里（见 RinneProcessorBase）。
// 界面动作的入口在下面，消息线程调用；哪些算用户动作（让实例定型）以 InputStateGlue.h 文件头的清单为准，这里不另列。
#pragma once

#include <atomic>
#include <optional>

#include "InputParams.h"
#include "InputStateGlue.h"
#include "PresetStore.h"
#include "RinneProcessorBase.h"

namespace rinne
{

class InputProcessor final : public RinneProcessorBase
{
public:
    InputProcessor();
    // 测试注入（命名作用域、后端、时钟、定时器）；role 与验证宿主由基类填
    explicit InputProcessor(const InstanceHostConfig& config);

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    // 旁路：干声逐位直通；引擎照常拉环、回送 10 ms 淡出，延迟线不清
    void processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // 定义在 InputPluginEntry.cpp
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    // ---- 界面动作（消息线程） ----
    // 紧急切断。不是设置，不定型；连点不会恢复
    void panic() noexcept;
    // 恢复回送。1.5 s 冷却由安全链判，冷却里的点击被丢掉，这里不排队；不定型
    void resumeLoop() noexcept;
    // 现在点恢复有没有用（跳闸了且不在冷却里）
    bool canResumeLoop() const noexcept;
    // 环路开关：按界面上的一次参数手势写 loop_on（begin / setValueNotifyingHost / end），定型
    void setLoopOnFromUi(bool on);
    bool isLoopOn() const noexcept;
    // 手动选组（0..8，0 = 离组）：定型并标脏。越界返回 false
    bool selectGroup(int group);
    // 界面上开始了一次参数手势（拖动或点击）：只定型，参数本身由手势照常写
    void userParamGestureBegan();

    // ---- 每块的读序（音频线程）。模板只为让测试用包了一层的三样直接调用、钉住调用的先后 ----
    // 先参数快照，再一道 acquire 栅栏，再读序号（胶水快照里的 stateSeq，最后是预设库的 presetSeq）。
    // 载入预设与 setState 都是先加序号、再写参数。参数快照读的原子量（APVTS 交给参数层的那个）由 JUCE 用默认的
    // seq_cst 写，带 release 语义；快照本身是 relaxed 读。读到任何一个新参数值之后，这道 acquire 栅栏就与那次写同步，
    // 写方在它之前加的序号对栅栏之后的读必定可见。没有栅栏，relaxed 的参数读可以被编译器或 CPU（ARM64）挪到序号读之后，
    // 拿到新参数、旧序号，这一块就没有淡入请求；presetSeq() 自己的 acquire 只和它自己的写配对，替代不了这道栅栏。
    // 内存序单测钉不住，源码扫描用例（tests/host/wiring/test_wiring_source_order.cpp）钉住栅栏的位置
    template<typename Params, typename Glue, typename Presets>
    static void snapshotBlock(const Params& params, const Glue& glue, const Presets& presets,
                              engine::EngineParams& out) noexcept
    {
        params.snapshot(out);
        std::atomic_thread_fence(std::memory_order_acquire);
        glue.snapshot(out);
        out.presetSeq = presets.presetSeq();
    }

    // ---- 读出（诊断与测试）。正式构建才有，探针构建里为空 ----
    input::InputParams* parameterLayer() noexcept { return wiring_.has_value() ? &wiring_->params : nullptr; }
    input::InputStateGlue* stateGlue() noexcept { return wiring_.has_value() ? &wiring_->glue : nullptr; }
    input::PresetStore* presetStore() noexcept { return wiring_.has_value() ? &wiring_->presets : nullptr; }

private:
    // 正式构建的参数层、胶水与预设库：先参数树、再建在它上面的胶水（析构反过来）。预设库只在载入时写参数树
    struct Wiring
    {
        explicit Wiring(juce::AudioProcessor& processor) : params(processor), glue(params.tree()) {}
        input::InputParams params;
        input::InputStateGlue glue;
        input::PresetStore presets;
    };

    void process(juce::AudioBuffer<float>& buffer, bool bypassed) noexcept;

    std::optional<Wiring> wiring_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InputProcessor)
};

} // namespace rinne
