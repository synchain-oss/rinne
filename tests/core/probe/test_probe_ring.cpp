// SPDX-License-Identifier: GPL-3.0-or-later
//
// 定长 SPSC 环：容量取 2 的幂、先进先出、满了拒收新来的而不覆盖旧的、下标回绕、双线程下一条不丢不乱序。
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <thread>

#include "diag/ProbeLog.h"

using rinne::diag::BlockSlot;
using rinne::diag::SpscRing;

TEST_CASE("the ring capacity is rounded up to a power of two", "[probe][ring]")
{
    CHECK(SpscRing<int>(0).capacity() == 2);
    CHECK(SpscRing<int>(2).capacity() == 2);
    CHECK(SpscRing<int>(5).capacity() == 8);
    CHECK(SpscRing<int>(8).capacity() == 8);
    CHECK(SpscRing<int>(1000).capacity() == 1024);
    CHECK(SpscRing<BlockSlot>(std::size_t{1} << 18).capacity() == (std::size_t{1} << 18));
}

TEST_CASE("the ring is first in first out and refuses new entries when full", "[probe][ring]")
{
    SpscRing<int> ring(4);
    int out = -1;
    CHECK_FALSE(ring.tryPop(out));
    for (int i = 0; i < 4; ++i)
    {
        REQUIRE(ring.tryPush(i));
    }
    CHECK(ring.size() == 4);
    // 满了：新来的被拒，已有的一条都不被覆盖
    CHECK_FALSE(ring.tryPush(100));
    CHECK_FALSE(ring.tryPush(101));
    for (int i = 0; i < 4; ++i)
    {
        REQUIRE(ring.tryPop(out));
        CHECK(out == i);
    }
    CHECK_FALSE(ring.tryPop(out));
    CHECK(ring.size() == 0);
}

TEST_CASE("the ring keeps order across index wrap-around", "[probe][ring]")
{
    SpscRing<std::uint64_t> ring(8);
    std::uint64_t nextPush = 0;
    std::uint64_t nextPop = 0;
    for (int round = 0; round < 1000; ++round)
    {
        for (int i = 0; i < 5; ++i)
        {
            REQUIRE(ring.tryPush(nextPush++));
        }
        for (int i = 0; i < 5; ++i)
        {
            std::uint64_t value = ~std::uint64_t{0};
            REQUIRE(ring.tryPop(value));
            REQUIRE(value == nextPop++);
        }
    }
}

TEST_CASE("two threads pass a million entries through a small ring without loss or reordering", "[probe][ring]")
{
    constexpr std::uint64_t kTotal = 1000000;
    SpscRing<std::uint64_t> ring(64); // 小环，让两边频繁碰到满和空
    std::atomic<bool> ordered{true};
    std::atomic<std::uint64_t> received{0};

    std::thread consumer([&] {
        std::uint64_t expected = 0;
        while (expected < kTotal)
        {
            std::uint64_t value = 0;
            if (ring.tryPop(value))
            {
                if (value != expected)
                {
                    ordered.store(false);
                    return;
                }
                ++expected;
                received.store(expected, std::memory_order_relaxed);
            }
            else
            {
                std::this_thread::yield();
            }
        }
    });
    std::uint64_t pushed = 0;
    while (pushed < kTotal && ordered.load())
    {
        if (ring.tryPush(pushed))
        {
            ++pushed;
        }
        else
        {
            std::this_thread::yield();
        }
    }
    consumer.join();
    CHECK(ordered.load());
    CHECK(received.load() == kTotal);
}

TEST_CASE("a ring slot hands over whole records", "[probe][ring]")
{
    // 槽里放满 64 字节：生产者写完一槽再发布，消费者读到的不会是写了一半的
    constexpr std::uint64_t kTotal = 200000;
    SpscRing<BlockSlot> ring(32);
    std::atomic<bool> torn{false};
    std::thread consumer([&] {
        std::uint64_t seen = 0;
        while (seen < kTotal)
        {
            BlockSlot slot;
            if (!ring.tryPop(slot))
            {
                std::this_thread::yield();
                continue;
            }
            const bool whole = slot.record.seq == seen && slot.record.callSeq == seen * 3 &&
                               slot.snapshot.word[0] == seen + 1 && slot.snapshot.word[3] == ~seen;
            if (!whole)
            {
                torn.store(true);
                return;
            }
            ++seen;
        }
    });
    for (std::uint64_t i = 0; i < kTotal && !torn.load();)
    {
        BlockSlot slot;
        slot.record.seq = i;
        slot.record.callSeq = i * 3;
        slot.snapshot.word[0] = i + 1;
        slot.snapshot.word[3] = ~i;
        if (ring.tryPush(slot))
        {
            ++i;
        }
        else
        {
            std::this_thread::yield();
        }
    }
    consumer.join();
    CHECK_FALSE(torn.load());
}
