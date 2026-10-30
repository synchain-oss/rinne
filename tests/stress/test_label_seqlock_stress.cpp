// SPDX-License-Identifier: GPL-3.0-or-later
//
// 标签 seqlock 的压测：一个写线程不停地发布 k = 2, 3, 4, ……（主线程先发布 k = 1，所以起始序号是 2），
// 一个读线程不停地读，读成功就按三条判据核对：
//   - 自洽：读到的四个字段必须全部是同一个 k 的函数（k 取 colour_argb），少一道读方 fence 或写方发布次序被
//     打乱时，读方会拿着新旧混在一起的一版当稳定版；
//   - 区间：k 必须落在 [ceil(s0/2), floor(s1/2)] 里。单写方时第 k 版完整发布的序号正好是 2k，读方前后两次
//     序号读数给出了这一版可能的时间窗，落在窗外说明它读到的字段与它以为的那一版对不上；
//   - 单调：k 不小于上一次成功读到的 k，读到倒退的版本说明序号比对没起作用。
// 两种摆放各跑一轮：标签在 256 字节对齐缓冲的偏移 0 与偏移 64。Apple M 系列一条缓存行 128 字节，偏移 0 时整个
// 128 字节的标签落在一条线上，偏移 64 时跨两条线——撕开的读在跨线时更容易被看到。
// x86 是 TSO，读方 fence 在那里只是编译器屏障，no_read_fence 变异抓不到：跑满 25 s 后以「证据不成立」结束（退出码
// 70），只有原生 arm64 能抓到。
// no_read_fence 的变异构建里两种摆放一遍遍地跑，直到三条判据的命中合计攒够 20 次（封顶 25 s），不够就不算抓住
// （见 StressMutation.h）；no_intent 不碰标签，这里照常跑一遍、照常该绿。两个变异构建里每一轮的非空转下限都必须达标。
// 工作线程只用计数，断言在主线程 join 之后做；每一轮按时间封顶，不会挂死。
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <thread>

#include "StressMutation.h"
#include "StressScale.h"
#include "ipc/LabelSeqlock.h"
#include "ipc/SegmentLayout.h"

namespace
{

using namespace rinne::ipc;
using namespace rinne::stress;
using Clock = std::chrono::steady_clock;

constexpr u32 kRoundMs = 100;
constexpr u32 kRoundCapMs = 2000;
constexpr u64 kMinAccepted = 1000;
constexpr u64 kMinPublished = 1000;

// 第 k 版的全部字段都是 k 的确定函数：读方拿到任意一个字段就能认出它属于哪一版
LabelPayload makeLabel(u32 k) noexcept
{
    LabelPayload p;
    p.colourArgb = k;
    p.nameLen = k * 7u + 3u;
    p.nameHash = k * 2654435761u;
    for (u32 i = 0; i < kLabelNameWords; ++i)
    {
        p.words[i] = k * (2u * i + 1u) + i * 0x9E3779B9u;
    }
    return p;
}

struct Result
{
    u64 published = 0;
    u64 accepted = 0;
    u64 rejected = 0;
    u64 badSelf = 0;
    u64 badBracket = 0;
    u64 badMonotonic = 0;
    u64 millis = 0;
};

// 一块 256 字节对齐的清零缓冲，标签按 placement new 摆在 offset 处
struct SlotStorage
{
    static constexpr std::size_t kBytes = 512;

    alignas(256) unsigned char bytes[kBytes] = {};

    RoleSlot& slotAt(std::size_t offset) noexcept { return *new (bytes + offset) RoleSlot(); }
};

Result runRound(std::size_t offset)
{
    SlotStorage storage;
    RoleSlot& slot = storage.slotAt(offset);
    // 主线程先发布 k = 1：序号 0 → 2，读线程一开始就能读到完整的一版
    REQUIRE(writeLabel(slot, makeLabel(1)));
    REQUIRE(slot.label_seq.load() == 2u);

    std::atomic<bool> stop{false};
    std::atomic<u64> published{0};
    std::atomic<u64> accepted{0};

    u64 wPublished = 0;
    u64 rAccepted = 0;
    u64 rRejected = 0;
    u64 rBadSelf = 0;
    u64 rBadBracket = 0;
    u64 rBadMonotonic = 0;

    std::thread writer([&] {
        u32 k = 2;
        while (!stop.load(std::memory_order_acquire))
        {
            if (writeLabel(slot, makeLabel(k)))
            {
                ++k;
                ++wPublished;
                published.store(wPublished, std::memory_order_relaxed);
            }
        }
    });

    std::thread reader([&] {
        u32 previous = 0;
        LabelPayload out;
        while (!stop.load(std::memory_order_acquire))
        {
            const u32 s0 = slot.label_seq.load(std::memory_order_acquire);
            const bool ok = readLabel(slot, out);
            const u32 s1 = slot.label_seq.load(std::memory_order_acquire);
            if (!ok)
            {
                ++rRejected;
                continue;
            }
            ++rAccepted;
            const u32 k = out.colourArgb;
            // 自洽：四个字段必须全部来自第 k 版
            if (out != makeLabel(k))
            {
                ++rBadSelf;
            }
            // 区间：这一版完整发布时序号是 2k，它必须落在两次序号读数之间
            const u32 lo = (s0 + 1u) / 2u;
            const u32 hi = s1 / 2u;
            if (k < lo || k > hi)
            {
                ++rBadBracket;
            }
            // 单调：同一轮里版本号只许往前走
            if (k < previous)
            {
                ++rBadMonotonic;
            }
            previous = k;
            accepted.store(rAccepted, std::memory_order_relaxed);
        }
    });

    const auto start = Clock::now();
    for (;;)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const auto elapsed =
            static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
        if (elapsed >= rinne::stress::scaledMs(kRoundMs) && accepted.load(std::memory_order_relaxed) >= kMinAccepted &&
            published.load(std::memory_order_relaxed) >= kMinPublished)
        {
            break;
        }
        if (elapsed >= rinne::stress::scaledMs(kRoundCapMs))
        {
            break;
        }
    }
    stop.store(true, std::memory_order_release);
    writer.join();
    reader.join();

    Result r;
    r.published = wPublished;
    r.accepted = rAccepted;
    r.rejected = rRejected;
    r.badSelf = rBadSelf;
    r.badBracket = rBadBracket;
    r.badMonotonic = rBadMonotonic;
    r.millis = static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
    return r;
}

} // namespace

TEST_CASE("a torn label read is always rejected or self-consistent", "[stress][seqlock]")
{
    const std::size_t offsets[] = {0, 64};
    // no_read_fence 的变异构建：三条判据合计命中了多少次
    u64 hits = 0;
    const auto start = Clock::now();
    for (u32 pass = 1;; ++pass)
    {
        for (const std::size_t offset : offsets)
        {
            INFO("label at offset " << offset << " pass " << pass);
            const Result r = runRound(offset);

            // 三条判据各自独立：变异构建必须让至少一条变红
            CHECK(r.badSelf == 0);
            CHECK(r.badBracket == 0);
            CHECK(r.badMonotonic == 0);
            // 非空转下限：读方真的读到了足够多完整的版本，写方也真的发布了足够多
            CHECK(r.accepted >= kMinAccepted);
            CHECK(r.published >= kMinPublished);

            std::printf("[stress][seqlock] offset=%llu published=%llu accepted=%llu rejected=%llu badSelf=%llu "
                        "badBracket=%llu badMonotonic=%llu ms=%llu\n",
                        static_cast<unsigned long long>(offset), static_cast<unsigned long long>(r.published),
                        static_cast<unsigned long long>(r.accepted), static_cast<unsigned long long>(r.rejected),
                        static_cast<unsigned long long>(r.badSelf), static_cast<unsigned long long>(r.badBracket),
                        static_cast<unsigned long long>(r.badMonotonic), static_cast<unsigned long long>(r.millis));
            std::fflush(stdout);

            requireNonVacuous(r.accepted >= kMinAccepted && r.published >= kMinPublished,
                              "标签的一轮没有跑满非空转下限");
            hits += r.badSelf + r.badBracket + r.badMonotonic;
        }
        const auto elapsed =
            static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
        if (!anotherPass(kNoReadFence, hits, elapsed))
        {
            break;
        }
    }
    requireHits(kNoReadFence, hits, "标签读到了不自洽或越界的一版（badSelf + badBracket + badMonotonic）");
}
