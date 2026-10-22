// SPDX-License-Identifier: GPL-3.0-or-later
//
// 中间链：环内插件的替身。它的增益与延迟就是「环内链增益」与「环内延迟」。
//
// 七种形态（场景可逐块切换）：
//   恒等、增益、一阶低通、k 样本延迟、NaN 注入、反相、低架提升。
//
// 实时约定：音频线程每块调 process。不分配、不加锁、不读墙钟、不抛异常。
// 滤波器的状态在延迟线里，容量在构造时定好（prepare 可以在非实时线程调）。

#pragma once

#include <cstdint>
#include <vector>

namespace rinne::rig
{

// 中间链的形态。
enum class ChainKind : std::uint8_t
{
    kIdentity = 0, // 恒等
    kGain = 1, // 增益
    kOnePoleLowpass = 2, // 一阶低通
    kDelay = 3, // k 样本延迟
    kNanInject = 4, // NaN 注入
    kInvert = 5, // 反相
    kLowShelfBoost = 6, // 低架提升
};

inline constexpr int kChainKindCount = 7;

// 一阶低通与低架提升的系数：一阶低通用双二阶不行（一阶极点用一阶就够了），低架提升用一阶低通的极点和增益。
//
// 一阶低通：y[n] = y[n-1] + a·(x[n] - y[n-1])，a = 1 - exp(-2π·fc/sr)。
// 低架提升：先做一阶低通取 low，再输出 low·(1+g) + (x - low)·(1 - g·k)，
// g 是提升量（线性，0..1），k 让高频的增益往 1 靠（0 = 高频完全不动，1 = 高频也被提升）。
// 两种形态的系数都在 prepare 时按采样率算好，音频线程上只做乘加。
class MiddleChain
{
public:
    // 非实时线程：按采样率算好系数与延迟线长度。可以分配。
    void prepare(double sampleRate, int maxBlock) noexcept;

    // 切形态。增益、截止频率、延迟样本数、注入位置在这一层给。
    void setKind(ChainKind kind) noexcept { kind_ = kind; }
    void setGain(float gain) noexcept { gain_ = gain; }
    // 改截止频率：系数要重算，否则低通形态还是旧的。
    void setLowpassHz(float hz) noexcept;
    // 改低架提升：低频与高频的增益要重算。
    void setLowShelfBoost(float boost, float hfMix) noexcept;
    // 改延迟样本数：延迟线要重开（长度变了）。
    void setDelaySamples(int samples) noexcept;
    // NaN 注入：从第几个样本开始注入（-1 = 不注入）。注入的是 NaN，用来验 Output 侧的清洗。
    void setNanInjectAt(int sample) noexcept { nanAt_ = sample; }

    ChainKind kind() const noexcept { return kind_; }
    int delaySamples() const noexcept { return delaySamples_; }
    int64_t totalLatency() const noexcept { return latency_; }
    double sampleRate() const noexcept { return sampleRate_; }

    // 音频线程：处理一块。in 与 out 都是左右各一串，长度都是 n。
    // 延迟形态会引入 latency_ 个样本的延迟（读侧要按这个对齐）。
    void process(const float* inLeft, const float* inRight, float* outLeft, float* outRight, int n) noexcept;

    // 复位：清掉滤波器的状态与延迟线里的内容（起播沿时用）。只改状态，不分配。
    void reset() noexcept;

private:
    // 一阶低通的系数：改截止频率或采样率时重算。
    void updateLowpass() noexcept;
    // 低架提升的增益：改提升量或混合比时重算。
    void updateShelf() noexcept;
    // 延迟线：长度恰好是 k（k = 0 时不延迟）。改 k 时重开。
    void openDelayLine(int samples) noexcept;

    ChainKind kind_ = ChainKind::kIdentity;

    float gain_ = 1.0f;
    float lowpassHz_ = 1000.0f;
    float shelfBoost_ = 0.25f;
    float shelfHfMix_ = 0.5f;
    int delaySamples_ = 0;
    int nanAt_ = -1;

    // 一阶低通与低架提升的系数（prepare 时算好）
    float lpA_ = 0.0f;
    float shelfLowGain_ = 1.0f;
    float shelfHighGain_ = 1.0f;

    // 滤波器的状态
    float lpLeft_ = 0.0f;
    float lpRight_ = 0.0f;
    float shelfLeft_ = 0.0f;
    float shelfRight_ = 0.0f;

    // 延迟线：左右各一条
    std::vector<float> delayLeft_;
    std::vector<float> delayRight_;
    std::size_t delayCursor_ = 0;

    // 已经过的样本数（NaN 注入的位置按块内绝对计数给）
    std::int64_t sampleCounter_ = 0;

    std::int64_t latency_ = 0;
    double sampleRate_ = 0.0;
};

} // namespace rinne::rig
