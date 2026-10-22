// SPDX-License-Identifier: GPL-3.0-or-later
//
// 音频线程的诊断计数。只有音频线程写（单写者），消息线程随时读：全是 relaxed 的整字原子量，不加锁、不分配、
// 不读时钟。读到的是各字段各自的近似值，彼此之间不保证同一时刻，诊断文本不需要更多。
// 快照里的直方图合计与总块数的关系不保证：总块数先加，直方图桶后加，两次读之间可能只看到一半，所以
// 「合计 + 0 样本 <= 总块数」不是恒等式，读到时以各字段自己的值为准。
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace rinne::diag
{

static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "诊断计数必须 lock-free");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "诊断计数必须 lock-free");

// 块长直方图的桶数：<=64、<=128、<=256、<=512、<=1024、<=2048、<=4096、更大
inline constexpr std::size_t kNsamplesBuckets = 8;

// 一次读出的计数，纯数据
struct DiagCountersSnapshot
{
    std::uint64_t blocksTotal = 0;
    std::uint64_t zeroLen = 0; // 0 样本的调用
    std::uint64_t overMax = 0; // 块长超过 prepare 声明的值
    std::uint64_t bypassed = 0; // 宿主旁路的块
    std::uint32_t nMin = 0; // 最小块长；还没见过非零块，或读到时 nMax 仍是 0，报告 0
    std::uint32_t nMax = 0; // 终身最大；「本次 prepare 以来」的最大块长不在这里，由调用方从共享段的 max_n_seen 填
    std::uint64_t hist[kNsamplesBuckets] = {};
};

class DiagCounters
{
public:
    // 音频线程：每个 processBlock 调一次。n = 本块样本数，declaredMax = prepare 声明的最大块长
    void onBlock(std::uint32_t n, std::uint32_t declaredMax, bool bypassed) noexcept
    {
        blocksTotal_.fetch_add(1, std::memory_order_relaxed);
        if (bypassed)
        {
            bypassed_.fetch_add(1, std::memory_order_relaxed);
        }
        if (n == 0)
        {
            zeroLen_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (declaredMax != 0 && n > declaredMax)
        {
            overMax_.fetch_add(1, std::memory_order_relaxed);
        }
        // 单写者：读后写不会丢更新
        if (n > nMax_.load(std::memory_order_relaxed))
        {
            nMax_.store(n, std::memory_order_relaxed);
        }
        if (n < nMin_.load(std::memory_order_relaxed))
        {
            nMin_.store(n, std::memory_order_relaxed);
        }
        hist_[bucketOf(n)].fetch_add(1, std::memory_order_relaxed);
    }

    // 消息线程：读出一份
    DiagCountersSnapshot snapshot() const noexcept
    {
        DiagCountersSnapshot s;
        s.blocksTotal = blocksTotal_.load(std::memory_order_relaxed);
        s.zeroLen = zeroLen_.load(std::memory_order_relaxed);
        s.overMax = overMax_.load(std::memory_order_relaxed);
        s.bypassed = bypassed_.load(std::memory_order_relaxed);
        s.nMax = nMax_.load(std::memory_order_relaxed);
        const std::uint32_t lo = nMin_.load(std::memory_order_relaxed);
        // 第一块的两次写落在两次读之间时，nMax 还是 0 而 nMin 已有值；两者一起报 0，不报出 min > max
        s.nMin = (lo == kNoMin || s.nMax == 0) ? 0 : lo;
        for (std::size_t i = 0; i < kNsamplesBuckets; ++i)
        {
            s.hist[i] = hist_[i].load(std::memory_order_relaxed);
        }
        return s;
    }

    static constexpr std::size_t bucketOf(std::uint32_t n) noexcept
    {
        std::size_t b = 0;
        std::uint32_t limit = 64;
        while (b + 1 < kNsamplesBuckets && n > limit)
        {
            ++b;
            limit <<= 1;
        }
        return b;
    }

private:
    static constexpr std::uint32_t kNoMin = 0xffffffffu;
    std::atomic<std::uint64_t> blocksTotal_{0};
    std::atomic<std::uint64_t> zeroLen_{0};
    std::atomic<std::uint64_t> overMax_{0};
    std::atomic<std::uint64_t> bypassed_{0};
    std::atomic<std::uint32_t> nMax_{0};
    std::atomic<std::uint32_t> nMin_{kNoMin};
    std::atomic<std::uint64_t> hist_[kNsamplesBuckets] = {};
};

} // namespace rinne::diag
