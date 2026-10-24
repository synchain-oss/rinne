// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rinne Input 与 Rinne Output 共用的处理器外壳：两端对宿主的约定都在这里，各自只写音频回调与编辑器。
//   - 延迟恒报 0，永不改变：报了延迟，宿主的延迟补偿会把环路延迟抵掉，或在限制补偿时把插件整个旁路。
//   - tail 让宿主在静音时也不挂起插件，否则环路会在安静段断开：自动模式下 VST3 报无限长，AU 报 30 秒。
//   - 总线：单声道或立体声，输入与输出声道数相同。
//   - 0 样本的回调立即返回，不推进任何计数（宿主用它来刷参数，不代表时间前进）。
//   - 验证宿主（pluginval、auval、AppleInfoHelper、VBVSTScanner）在构造时识别一次；在它们下面只出干声，不开共享内存段。
// 不自建旁路参数，旁路由包装层的 bypass 参数驱动，两端各自重写 processBlockBypassed。
// 探针构建里每个实例另持有一个 probe::ProbeHost（引擎、会合、探针日志与面板的接线），这里只转发；
// 正式构建里转发的那一支是 `if constexpr` 的弃置分支，不引用 ProbeHost 的定义，行为与外壳阶段相同。
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <cstdint>
#include <memory>

namespace rinne
{

namespace probe
{
class ProbeHost;
}

class RinneProcessorBase : public juce::AudioProcessor
{
public:
    // 与 state 里 tail_mode 的取值一一对应，顺序不能改
    enum class TailMode : int
    {
        Auto = 0,
        Infinite = 1,
        ThirtySeconds = 2,
    };

    ~RinneProcessorBase() override = default;

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
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    // 本实例是否跑在验证宿主里；构造后不变
    bool isValidationHost() const noexcept { return validationHost_; }
    // 实例建立以来音频回调处理过的样本数（旁路也算，0 样本的回调不算）
    std::int64_t getProcessedSamples() const noexcept;

protected:
    RinneProcessorBase();

    // 每个音频回调最先调用。0 样本返回 false，调用方立即返回；否则推进样本计数、返回 true。只在音频线程上调用。
    bool beginBlock(int numSamples) noexcept;

    // ---- 探针构建的接线（正式构建里都是空操作） ----
    enum class Side
    {
        Input,
        Output,
    };
    // 子类构造时调一次
    void attachProbe(Side side);
    // 音频回调在 beginBlock 之后调
    void processProbe(juce::AudioBuffer<float>& buffer, bool bypassed) noexcept;
    // 探针面板；没有探针时返回空，调用方用自己的编辑器
    juce::AudioProcessorEditor* createProbeEditor();

private:
    static bool detectValidationHost();
    static void destroyProbe(probe::ProbeHost* host) noexcept;

    static_assert(std::atomic<int>::is_always_lock_free);
    static_assert(std::atomic<std::int64_t>::is_always_lock_free);

    std::atomic<int> tailMode_{static_cast<int>(TailMode::Auto)};
    std::atomic<std::int64_t> processedSamples_{0};
    const bool validationHost_;
    std::unique_ptr<probe::ProbeHost, void (*)(probe::ProbeHost*) noexcept> probe_{nullptr, &destroyProbe};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RinneProcessorBase)
};

} // namespace rinne
