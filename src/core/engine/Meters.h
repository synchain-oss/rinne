// SPDX-License-Identifier: GPL-3.0-or-later
//
// 音频线程发布、界面线程读取的测量量。每个字段一个无锁原子量（float 按位存），音频线程每块写一次。
// 两种字段：
//   值（store / load）：当前值，读多少次都一样；
//   峰值（publishMax / take）：音频线程只往大里写，界面以 25 Hz 左右 take 走并清零，两次读之间的最大值不会丢。
// 这里的安全测量量（回送 DC 均值、贴近满幅的持续时长、限制器增益缩减）只供安全灯与失控保护判据使用，
// 不参与任何别的音频决策。电平只供界面显示（rinne.meters 的 in / ret / out 与 Mix 推子的两路分量），
// 不参与任何音频决策；取点见 InputEngine.h 的文件头。
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

#include "engine/SafetyEvents.h"

namespace rinne::engine
{

class AtomicFloat
{
public:
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "测量量必须无锁");

    void store(float v) noexcept { bits_.store(toBits(v), std::memory_order_relaxed); }
    float load() const noexcept { return fromBits(bits_.load(std::memory_order_relaxed)); }

    // 只往大里写。读方只会用 take 把它清成 0，所以这个循环最多多转一圈
    void publishMax(float v) noexcept
    {
        std::uint32_t cur = bits_.load(std::memory_order_relaxed);
        while (v > fromBits(cur) && !bits_.compare_exchange_weak(cur, toBits(v), std::memory_order_relaxed))
        {
        }
    }

    float take() noexcept { return fromBits(bits_.exchange(0u, std::memory_order_relaxed)); }

private:
    static std::uint32_t toBits(float v) noexcept
    {
        std::uint32_t b = 0;
        std::memcpy(&b, &v, sizeof b);
        return b;
    }

    static float fromBits(std::uint32_t b) noexcept
    {
        float v = 0.0f;
        std::memcpy(&v, &b, sizeof v);
        return v;
    }

    std::atomic<std::uint32_t> bits_{0u};
};

// 安全状态打包成一个 32 位字：低 8 位状态，再 8 位跳闸原因，再 8 位淡入原因，最高位 = 恢复冷却中
struct PackedSafety
{
    static std::uint32_t pack(SafetyState s, TripReason t, FadeReason f, bool coolingDown) noexcept
    {
        return static_cast<std::uint32_t>(s) | (static_cast<std::uint32_t>(t) << 8) |
               (static_cast<std::uint32_t>(f) << 16) | (coolingDown ? 0x80000000u : 0u);
    }
    static SafetyState state(std::uint32_t w) noexcept { return static_cast<SafetyState>(w & 0xffu); }
    static TripReason trip(std::uint32_t w) noexcept { return static_cast<TripReason>((w >> 8) & 0xffu); }
    static FadeReason fade(std::uint32_t w) noexcept { return static_cast<FadeReason>((w >> 16) & 0xffu); }
    static bool coolingDown(std::uint32_t w) noexcept { return (w & 0x80000000u) != 0; }
};

struct Meters
{
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "测量量必须无锁");
    static_assert(std::atomic<bool>::is_always_lock_free, "测量量必须无锁");

    // 值：回送（清洗后、DC 阻断前）的直流均值，取左右声道绝对值较大者，线性幅度；时间常数 300 ms
    AtomicFloat returnDcMean;
    // 值：回送（×g 之后、限制器之前）连续贴近满幅（|x| ≥ 0.7，约 −3.1 dBFS）的时长，秒；不贴近时为 0
    AtomicFloat nearFullSeconds;
    // 峰值：限制器增益缩减，dB（≥ 0）；限制器关闭时不写
    AtomicFloat limiterReductionDb;
    // 值：失控保护当前把回送压低了多少，dB（≤ 0）
    AtomicFloat guardGainDb;
    // 值：PackedSafety
    std::atomic<std::uint32_t> safety{0u};
    // 累计：环里读到并已置 0 的非有限样本数（只增不减，回绕无妨）
    std::atomic<std::uint32_t> returnNonFinite{0u};

    // 电平：五个都是峰值，线性幅度，只收有限样本（NaN、±Inf 不计），两次 take 之间一直静音就是 0（换成 null 归桥）。
    // 峰值：in，宿主给的干声（g_dry 之前）
    AtomicFloat inputPeak;
    // 峰值：ret，⑥ ×g 之前的回送（③④⑤ 之后，mono 时是两格的平均）
    AtomicFloat returnPeak;
    // 峰值：out，Input 的最终输出
    AtomicFloat outputPeak;
    // 峰值：Mix 推子的干声分量 g_dry·干声
    AtomicFloat dryPartPeak;
    // 峰值：Mix 推子的回送分量，就是加到干声上的那一项（⑨ 之后）
    AtomicFloat returnPartPeak;
    // 值：高切被采样率截断在 0.45·sr（参数在它之上而又不是关），界面据此给中性提示
    std::atomic<bool> highCutLimited{false};
};

} // namespace rinne::engine
