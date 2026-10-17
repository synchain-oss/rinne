// SPDX-License-Identifier: GPL-3.0-or-later
//
// 冒烟：线程基本盘。一写一读两个线程经原子量交接，读方只在看到发布标志之后读数据。
// 没有 sanitizer 时它只是个正常的并发用例；在 tsan 配置下，内存序写错、或者测试夹具自己有竞争，都会在这里变红，
// 证明 tsan 这条路是通的，而不是一直在跑一个没有线程的程序。

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <thread>

TEST_CASE("release/acquire handoff between two threads", "[smoke][threads]")
{
    constexpr std::uint32_t rounds = 20000;
    std::array<std::uint32_t, 64> slot{};
    std::atomic<std::uint32_t> published{0}; // 已发布的轮数
    std::atomic<std::uint32_t> consumed{0}; // 已读走的轮数

    std::thread producer([&] {
        for (std::uint32_t i = 1; i <= rounds; ++i)
        {
            while (consumed.load(std::memory_order_acquire) + 1 < i)
                std::this_thread::yield();
            for (std::size_t k = 0; k < slot.size(); ++k)
                slot[k] = i * 31u + static_cast<std::uint32_t>(k);
            published.store(i, std::memory_order_release);
        }
    });

    bool intact = true;
    for (std::uint32_t i = 1; i <= rounds; ++i)
    {
        while (published.load(std::memory_order_acquire) < i)
            std::this_thread::yield();
        for (std::size_t k = 0; k < slot.size(); ++k)
            intact = intact && slot[k] == i * 31u + static_cast<std::uint32_t>(k);
        consumed.store(i, std::memory_order_release);
    }
    producer.join();

    CHECK(intact);
    CHECK(published.load() == rounds);
}
