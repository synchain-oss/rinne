// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rinne Input 的宿主参数层：把参数表（ParamTable.h，契约见 docs/contracts/PARAMETERS.md）展开成参数树，
// 并在音频线程的块首把 11 个参数的明文值搬进 EngineParams。
//
// 布局：makeInputLayout() 按表展开，树名 PARAMS，每个参数的 ParameterID 是 {id, kParamVersionHint}。
//   连续参数用 AudioParameterFloat，开关用 AudioParameterBool，档位用 AudioParameterChoice，
//   档位文字取表里的 RINNE_CHOICE 行。
// 刻度：线性参数用 JUCE 的标准线性范围。对数参数（delay_ms、low_cut、high_cut）用自定义的三个换算：
//   JUCE 的 skew 因子是幂函数，做不出契约要的纯对数 v = lo·(hi/lo)^p。
//   步进一律落在明文值上：宿主写来的值取整到 lo + k·step，再夹进范围。
// 明文值：APVTS 的 getRawParameterValue 给的已经是明文值（% / ms / Hz / dB，开关 0 或 1，档位下标），
//   不是 0…1 的归一化值。读到之后不能再 convertFrom0to1，否则换算了两次。
// 快照：消息线程上绑定时取好 11 个 std::atomic<float>*；音频线程每块调用一次 snapshot，
//   只做逐个 atomic load 和定长运算，不加锁、不分配、不碰 ValueTree。
//   快照只写 EngineParams 里属于参数的 11 个字段，命令序号与三个工程开关不归这里管，原样留着。
// 不夹值：映射层原样搬运明文值。限制器关闭时反馈的有效上限 100 % 由引擎里的安全状态机负责，
//   参数本身仍可以到 150 %。
//
// 线程：InputParams 在处理器的构造函数里建（消息线程），之后 snapshot 只在音频线程调用。
// bind 只能在音频线程启动之前调用。
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "engine/EngineParams.h"

namespace rinne::input
{

// 参数树的名字。参数树的 ValueTree 根节点取这个类型名，宿主工程里存的 state 也挂在它上面
inline constexpr const char* kParamsTreeName = "PARAMS";

// 所有参数的 versionHint。契约规定现有 11 个参数全是 1，以后追加的参数用更大的值
// （AU 宿主按 versionHint 再按宿主参数 ID 排参数列表，追加的参数因此排在原有参数之后）
inline constexpr int kParamVersionHint = 1;

// 参数个数，与参数表一致（InputParams.cpp 里有编译期核对）
inline constexpr std::size_t kParamCount = 11;

// 参数表第 index 行建成的 JUCE 参数对象；index 超出范围返回空指针。makeInputLayout 逐行调用它
std::unique_ptr<juce::RangedAudioParameter> makeInputParameter(std::size_t index);

// 按参数表展开的完整布局，11 个参数按表里的序号排列，不分组
juce::AudioProcessorValueTreeState::ParameterLayout makeInputLayout();

// 11 个参数的原子量句柄。消息线程上 bind，音频线程上 snapshot。
class InputParamHandles
{
public:
    // 按 ID 在参数树里取 11 个原子量。任何一个取不到就返回 false，并保持未绑定（句柄全部清空）；
    // 生产里的调用方（InputParams 的构造函数）对返回值断言，接线错误不会静默带过。
    // 只能在音频线程启动之前调用。
    bool bind(juce::AudioProcessorValueTreeState& tree);

    bool isBound() const noexcept { return bound_; }

    // 音频线程每块调用一次：把 11 个参数的明文值搬进 out 的对应字段，其余字段不动。
    // 未绑定时什么也不写，out 保持调用方给的值（EngineParams 的默认值就是安全默认）。
    // 开关与档位的取整规则：开关 ≥ 0.5 为真；档位四舍五入到下标、夹进 0…档数−1（NaN 取 0）。
    void snapshot(engine::EngineParams& out) const noexcept;

private:
    static_assert(std::atomic<float>::is_always_lock_free, "音频线程读参数要求 std::atomic<float> 无锁");

    std::array<std::atomic<float>*, kParamCount> raw_{};
    bool bound_ = false;
};

// 处理器持有的参数层：自己建参数树并绑定句柄。处理器在构造函数的初始化列表里构造它，之后每块调用 snapshot。
class InputParams
{
public:
    explicit InputParams(juce::AudioProcessor& processor);

    juce::AudioProcessorValueTreeState& tree() noexcept { return tree_; }
    const juce::AudioProcessorValueTreeState& tree() const noexcept { return tree_; }

    bool isBound() const noexcept { return handles_.isBound(); }

    // 音频线程，每块一次，见 InputParamHandles::snapshot
    void snapshot(engine::EngineParams& out) const noexcept { handles_.snapshot(out); }

private:
    juce::AudioProcessorValueTreeState tree_;
    InputParamHandles handles_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(InputParams)
};

// ---- 环路延迟 D 的毫秒与样本互换 ----
// 参数里存的是毫秒；界面可以按样本数显示与输入。样本数是毫秒按当前采样率换出来的，不是另一份参数：
// 采样率变了，参数里的毫秒值不动，样本数按新采样率重算（48 kHz 的 100 ms 是 4800 个样本，换到 96 kHz 是 9600 个）。
// 往返：样本数 → 毫秒 → 样本数，在同一个采样率下精确还原（double 运算，误差远小于半个样本）。
// 把毫秒写回参数时要再过一道步进（0.001 ms）和 float 归一化值：在 44.1 / 48 / 88.2 / 96 / 176.4 / 192 kHz 下，这两道
// 造成的误差都小于半个样本，所以按样本数设的值换成毫秒存进参数、再读出来换回样本，仍是同一个样本数（用例逐个样本数核对）。
// 余量在 4000 ms 附近最小：192 kHz 下最远偏出 0.48 个样本。限制精度的是 float 归一化值，不是步进，采样率再高就不够了。
// 毫秒值或采样率非正、非有限（NaN、±inf）一律返回 0；有限但大得离谱的值夹在 10^15，不溢出。
inline std::int64_t delayMsToSamples(double ms, double sampleRate) noexcept
{
    if (!(ms > 0.0) || !(sampleRate > 0.0) || !std::isfinite(ms) || !std::isfinite(sampleRate))
        return 0;
    const double samples = ms * sampleRate / 1000.0;
    // 远大于任何合法的 D（4000 ms × 192 kHz 不到 10^6），上限只为挡住 llround 的溢出
    return samples >= 1.0e15 ? static_cast<std::int64_t>(1.0e15) : static_cast<std::int64_t>(std::llround(samples));
}

inline double delaySamplesToMs(std::int64_t samples, double sampleRate) noexcept
{
    if (samples <= 0 || !(sampleRate > 0.0))
        return 0.0;
    return static_cast<double>(samples) * 1000.0 / sampleRate;
}

} // namespace rinne::input
