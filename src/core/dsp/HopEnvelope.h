// SPDX-License-Identifier: GPL-3.0-or-later
//
// 10 ms 格上的滑动统计，安全判据用的「最近一段时间」都在这里算：
//   HopPeakHistory   每格记一个峰值，查询最近 n 个整格的最大值（链增益估计用）；
//   HopCountWindow   每格记命中样本数，给出「最近 n 个整格 + 正在累计的这一格」里的命中总数（失控保护的 3 s 窗用）。
// 格长 round(0.01·sr)
// 个样本，从锚点（prepare、复位、起播沿复位）起逐样本计数，所以格界只由样本数决定，与宿主怎么切块无关。
// 容量是编译期常数，全部存储在对象里，音频线程上不分配。
#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace rinne::dsp
{

inline constexpr double kHopSeconds = 0.010;

inline std::int64_t hopSamplesFor(double sampleRate) noexcept
{
    const auto n = static_cast<std::int64_t>(std::llround(kHopSeconds * sampleRate));
    return n > 0 ? n : 1;
}

template<int Capacity>
class HopPeakHistory
{
    static_assert(Capacity > 1, "至少要能存两格");

public:
    void prepare(std::int64_t hopSamples) noexcept
    {
        hop_ = hopSamples > 0 ? hopSamples : 1;
        reset();
    }

    void reset() noexcept
    {
        peaks_.fill(0.0f);
        head_ = 0;
        filled_ = 0;
        pos_ = 0;
        partial_ = 0.0f;
    }

    // 记一个样本的幅度（≥ 0）。本样本正好收尾一格时返回 true，这时 maxOfLast 已包含刚收尾的格
    bool push(float magnitude) noexcept
    {
        if (magnitude > partial_)
            partial_ = magnitude;
        if (++pos_ < hop_)
            return false;
        peaks_[static_cast<std::size_t>(head_)] = partial_;
        head_ = (head_ + 1) % Capacity;
        if (filled_ < Capacity)
            ++filled_;
        pos_ = 0;
        partial_ = 0.0f;
        return true;
    }

    // 最近 hops 个整格的最大值；整格不够时只看已有的，一格都没有时为 0
    float maxOfLast(int hops) const noexcept
    {
        const int n = hops < filled_ ? hops : filled_;
        float m = 0.0f;
        for (int i = 1; i <= n; ++i)
        {
            const float v = peaks_[static_cast<std::size_t>((head_ - i + Capacity) % Capacity)];
            if (v > m)
                m = v;
        }
        return m;
    }

    int completedHops() const noexcept { return filled_; }
    std::int64_t hopSamples() const noexcept { return hop_; }

private:
    std::array<float, Capacity> peaks_{};
    std::int64_t hop_ = 1;
    std::int64_t pos_ = 0;
    int head_ = 0;
    int filled_ = 0;
    float partial_ = 0.0f;
};

template<int Capacity>
class HopCountWindow
{
    static_assert(Capacity > 1, "至少要能存两格");

public:
    // windowHops 个整格加上正在累计的一格；windowHops 不超过 Capacity
    void prepare(std::int64_t hopSamples, int windowHops) noexcept
    {
        hop_ = hopSamples > 0 ? hopSamples : 1;
        window_ = windowHops < 1 ? 1 : (windowHops > Capacity ? Capacity : windowHops);
        reset();
    }

    void reset() noexcept
    {
        counts_.fill(0);
        head_ = 0;
        filled_ = 0;
        pos_ = 0;
        partial_ = 0;
        sum_ = 0;
    }

    void push(bool hit) noexcept
    {
        if (hit)
        {
            ++partial_;
            ++sum_;
        }
        if (++pos_ < hop_)
            return;
        // 收尾一格：先让窗口最老的一格出窗，再把这一格存进去
        if (filled_ == window_)
            sum_ -= counts_[static_cast<std::size_t>((head_ - window_ + Capacity) % Capacity)];
        else
            ++filled_;
        counts_[static_cast<std::size_t>(head_)] = partial_;
        head_ = (head_ + 1) % Capacity;
        pos_ = 0;
        partial_ = 0;
    }

    // 窗口里的命中样本数
    std::int64_t sum() const noexcept { return sum_; }
    std::int64_t hopSamples() const noexcept { return hop_; }
    int windowHops() const noexcept { return window_; }

private:
    std::array<std::int64_t, Capacity> counts_{};
    std::int64_t hop_ = 1;
    std::int64_t pos_ = 0;
    std::int64_t partial_ = 0;
    std::int64_t sum_ = 0;
    int window_ = 1;
    int head_ = 0;
    int filled_ = 0;
};

} // namespace rinne::dsp
