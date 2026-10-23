// SPDX-License-Identifier: GPL-3.0-or-later
//
// 127 点最大长度序列（MLS）和 Ping 用的相关求延迟。探针往环里注入一串 MLS，再对读头的原始回读做互相关，
// 相关峰的位置就是环路延迟（样本数）。MLS 的循环自相关只在零移位处有峰（127），其余移位处恒为 -1，
// 所以峰是唯一的，抗噪也比脉冲好；峰与次高值的比就是可信度。
//
// 序列：特征多项式 x^7 + x^6 + 1（本原），递推 a[n+7] = a[n+6] ^ a[n]，种子全 1；位 1 取 +1、位 0 取 -1。
// 全部是定长循环：不分配、不加锁、不抛异常，序列与自相关在编译期算出并用 static_assert 核对。
//
// 延迟的约定：接收到的信号 x 是序列延迟 L 个样本之后的样子（x[k] = mls[k - L]），求出来的就是 L。
//   mlsCircularDelay  x 是周期注入时的任意一个整周期（127 点），只能得到 L 对 127 取模的值
//   mlsLinearDelay    x 是一次性注入的单个序列所在的一段回读（长度 n >= 127），得到 L 在 [0, n - 127] 内的绝对值
// 两者都在非实时线程调用（Ping 的结果由消息线程算），代价 O(127 * 候选延迟数)。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace rinne::util
{

inline constexpr int kMlsDegree = 7;
inline constexpr int kMlsLength = (1 << kMlsDegree) - 1; // 127

constexpr std::array<std::int8_t, kMlsLength> makeMls() noexcept
{
    std::array<std::int8_t, kMlsLength> out{};
    unsigned reg = 0x7Fu; // 第 k 位 = a[n + k]
    for (int i = 0; i < kMlsLength; ++i)
    {
        out[static_cast<std::size_t>(i)] = (reg & 1u) != 0 ? std::int8_t{1} : std::int8_t{-1};
        const unsigned next = ((reg >> 6) ^ reg) & 1u; // a[n + 7] = a[n + 6] ^ a[n]
        reg = (reg >> 1) | (next << 6);
    }
    return out;
}

inline constexpr std::array<std::int8_t, kMlsLength> kMls = makeMls();

// 循环自相关：sum_i mls[i] * mls[(i + lag) mod 127]。零移位 127，其余恒为 -1
constexpr int mlsAutocorrelation(int lag) noexcept
{
    int sum = 0;
    for (int i = 0; i < kMlsLength; ++i)
    {
        sum += kMls[static_cast<std::size_t>(i)] * kMls[static_cast<std::size_t>((i + lag) % kMlsLength)];
    }
    return sum;
}

constexpr bool mlsIsMaximal() noexcept
{
    if (mlsAutocorrelation(0) != kMlsLength)
    {
        return false;
    }
    for (int lag = 1; lag < kMlsLength; ++lag)
    {
        if (mlsAutocorrelation(lag) != -1)
        {
            return false;
        }
    }
    return true;
}

static_assert(mlsIsMaximal(), "x^7 + x^6 + 1 必须产生周期 127 的最大长度序列");

// 相关结果。lag < 0 表示没有可算的候选（输入太短或指针为空）。
// peak = 峰处的相关和（干净的单位幅度信号为 127，幅度 A 的信号为 127 * A）；
// runnerUp = 其余候选里绝对值最大的相关和；调用方用 peak 与 runnerUp 的比判断可信度
struct MlsPeak
{
    int lag = -1;
    double peak = 0.0;
    double runnerUp = 0.0;
};

namespace detail
{

constexpr double absd(double v) noexcept
{
    return v < 0.0 ? -v : v;
}

// 在一串候选相关和里取峰：只认正相关的最大值（反相的信号不是我们注入的那串），并列时取最小的延迟
struct PeakTracker
{
    int lag = -1;
    double peak = 0.0;
    double runnerUp = 0.0;

    constexpr void offer(int candidate, double sum) noexcept
    {
        if (lag < 0 || sum > peak)
        {
            if (lag >= 0)
            {
                runnerUp = absd(peak) > runnerUp ? absd(peak) : runnerUp;
            }
            lag = candidate;
            peak = sum;
        }
        else
        {
            runnerUp = absd(sum) > runnerUp ? absd(sum) : runnerUp;
        }
    }

    constexpr MlsPeak result() const noexcept { return MlsPeak{lag, peak, runnerUp}; }
};

} // namespace detail

// x 指向恰好一个周期（127 点）。返回 L in [0, 127)：x[k] = mls[(k - L) mod 127] 时相关最大
constexpr MlsPeak mlsCircularDelay(const float* x) noexcept
{
    if (x == nullptr)
    {
        return MlsPeak{};
    }
    detail::PeakTracker tracker;
    for (int lag = 0; lag < kMlsLength; ++lag)
    {
        double sum = 0.0;
        for (int i = 0; i < kMlsLength; ++i)
        {
            sum += static_cast<double>(x[(i + lag) % kMlsLength]) * kMls[static_cast<std::size_t>(i)];
        }
        tracker.offer(lag, sum);
    }
    return tracker.result();
}

// x 指向 n 个样本的回读，其中某处开始是一次性注入的单个序列（之后是静默或噪声）。
// 返回 L in [0, n - 127]：x[L + i] = mls[i] 时相关最大。线性相关，不回卷。
constexpr MlsPeak mlsLinearDelay(const float* x, std::size_t n) noexcept
{
    if (x == nullptr || n < static_cast<std::size_t>(kMlsLength))
    {
        return MlsPeak{};
    }
    detail::PeakTracker tracker;
    const std::size_t candidates = n - static_cast<std::size_t>(kMlsLength) + 1;
    for (std::size_t lag = 0; lag < candidates; ++lag)
    {
        double sum = 0.0;
        for (std::size_t i = 0; i < static_cast<std::size_t>(kMlsLength); ++i)
        {
            sum += static_cast<double>(x[lag + i]) * kMls[i];
        }
        tracker.offer(static_cast<int>(lag), sum);
    }
    return tracker.result();
}

} // namespace rinne::util
