// SPDX-License-Identifier: GPL-3.0-or-later
//
// ctl 整字的压测：8 个线程在 4 个槽上抢着「认领 -> 激活 -> 保持一会儿 -> 释放」，每个槽同时只许有一个持有者；
// 每次成功认领恰好写 3 次 gen（free -> claiming -> active -> free），所以 join 之后每个槽的 gen 必须等于
// 3 x 该槽的成功认领次数。外加一次 2 线程 20000 轮的同字竞争：每轮恰好一个赢家。
// 工作线程不碰 Catch2，只计数；断言全在主线程 join 之后做。时间有上下界（先跑 1 s，样本不够就延到 3 s）。
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "ipc/Rendezvous.h"

namespace
{

using namespace rinne::ipc;
using Clock = std::chrono::steady_clock;

constexpr int kSlots = 4;
constexpr u32 kMinClaims = 10000;
constexpr u32 kMinContention = 100;
constexpr int kRaceRounds = 20000;

class Rng
{
public:
    explicit Rng(u64 seed) noexcept : state_(seed) {}

    u64 next() noexcept
    {
        u64 z = (state_ += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }

    int below(int n) noexcept { return static_cast<int>(next() % static_cast<u64>(n)); }

private:
    u64 state_;
};

class SpinBarrier
{
public:
    explicit SpinBarrier(int n) noexcept : n_(n) {}

    void arriveAndWait() noexcept
    {
        const int generation = generation_.load(std::memory_order_acquire);
        if (arrived_.fetch_add(1, std::memory_order_acq_rel) + 1 == n_)
        {
            arrived_.store(0, std::memory_order_relaxed);
            generation_.fetch_add(1, std::memory_order_release);
            return;
        }
        while (generation_.load(std::memory_order_acquire) == generation)
        {
            std::this_thread::yield();
        }
    }

private:
    int n_;
    std::atomic<int> arrived_{0};
    std::atomic<int> generation_{0};
};

} // namespace

TEST_CASE("ctl stays exclusive under an eight-thread claim storm", "[stress][ctl]")
{
    auto rv = std::make_unique<RvSegment>(); // 值初始化 = 全零
    std::array<std::atomic<long long>, kSlots> holders{};
    std::array<std::atomic<long long>, kSlots> claimsOnSlot{};
    std::atomic<long long> claims{0};
    std::atomic<long long> releases{0};
    std::atomic<long long> contention{0};
    std::atomic<long long> violations{0};
    std::atomic<bool> stop{false};

    const auto worker = [&](u64 seed) {
        Rng rng(seed);
        while (!stop.load(std::memory_order_acquire))
        {
            const int s = rng.below(kSlots);
            const u32 group = static_cast<u32>(1 + s / 2);
            const Role role = (s % 2) != 0 ? Role::kOutput : Role::kInput;
            std::atomic<u64>& ctl = roleSlot(*rv, group, role).ctl;

            const u64 observed = ctl.load(std::memory_order_acquire);
            if (ctlPhase(observed) != kCtlPhaseFree)
            {
                continue; // 别人占着：不算一次失败的 CAS
            }
            const CtlCas claimed = casCtl(ctl, observed, kCtlPhaseClaiming);
            if (!claimed.ok)
            {
                contention.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            const CtlCas activated = casCtl(ctl, claimed.word, kCtlPhaseActive);
            if (!activated.ok)
            {
                violations.fetch_add(1, std::memory_order_relaxed); // 认领之后没人能抢走
                continue;
            }
            claimsOnSlot[static_cast<std::size_t>(s)].fetch_add(1, std::memory_order_relaxed);
            if (holders[static_cast<std::size_t>(s)].fetch_add(1, std::memory_order_acq_rel) != 0)
            {
                violations.fetch_add(1, std::memory_order_relaxed); // 同一个槽出现两个持有者
            }
            for (int spin = 0; spin < 200; ++spin)
            {
                (void)holders[static_cast<std::size_t>(s)].load(std::memory_order_relaxed);
            }
            holders[static_cast<std::size_t>(s)].fetch_sub(1, std::memory_order_acq_rel);
            const CtlCas released = casCtl(ctl, activated.word, kCtlPhaseFree);
            if (released.ok)
            {
                releases.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                violations.fetch_add(1, std::memory_order_relaxed); // 持有期间没人能动这个整字
            }
            claims.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(8);
    for (u64 i = 0; i < 8; ++i)
    {
        threads.emplace_back(worker, 0x5eed0000ull + i * 0x9e3779b97f4a7c15ull);
    }

    const auto start = Clock::now();
    for (;;)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const auto elapsed =
            static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
        const bool minimums = claims.load(std::memory_order_relaxed) >= kMinClaims &&
                              contention.load(std::memory_order_relaxed) >= kMinContention;
        if (elapsed >= 1000 && minimums)
        {
            break;
        }
        if (elapsed >= 3000)
        {
            break;
        }
    }
    stop.store(true, std::memory_order_release);
    for (std::thread& t : threads)
    {
        t.join();
    }
    const auto millis =
        static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());

    CHECK(violations.load() == 0);
    CHECK(claims.load() == releases.load());
    CHECK(claims.load() >= static_cast<long long>(kMinClaims));
    CHECK(contention.load() >= static_cast<long long>(kMinContention));
    for (int s = 0; s < kSlots; ++s)
    {
        const u32 group = static_cast<u32>(1 + s / 2);
        const Role role = (s % 2) != 0 ? Role::kOutput : Role::kInput;
        const u64 word = roleSlot(*rv, group, role).ctl.load(std::memory_order_acquire);
        INFO("slot " << s << " ctl " << word);
        CHECK(ctlPhase(word) == kCtlPhaseFree);
        const auto expected =
            static_cast<u32>(3ull * static_cast<u64>(claimsOnSlot[static_cast<std::size_t>(s)].load()));
        CHECK(ctlGen(word) == expected);
    }

    std::printf("[stress][ctl] claims=%lld releases=%lld contention=%lld violations=%lld ms=%llu\n",
                static_cast<long long>(claims.load()), static_cast<long long>(releases.load()),
                static_cast<long long>(contention.load()), static_cast<long long>(violations.load()),
                static_cast<unsigned long long>(millis));
    std::fflush(stdout);
}

TEST_CASE("two threads racing one ctl word elect exactly one winner per round", "[stress][ctl]")
{
    std::atomic<u64> ctl{makeCtl(0, kCtlPhaseFree)};
    std::atomic<u64> startWord{0};
    std::vector<std::array<bool, 2>> results(static_cast<std::size_t>(kRaceRounds));
    SpinBarrier start(2);
    SpinBarrier done(2);
    const auto worker = [&](int who) {
        for (int round = 0; round < kRaceRounds; ++round)
        {
            if (who == 0)
            {
                startWord.store(ctl.load(std::memory_order_relaxed), std::memory_order_release);
            }
            start.arriveAndWait();
            const u64 observed = startWord.load(std::memory_order_acquire);
            const CtlCas cas = casCtl(ctl, observed, kCtlPhaseClaiming);
            results[static_cast<std::size_t>(round)][static_cast<std::size_t>(who)] = cas.ok;
            done.arriveAndWait();
        }
    };
    std::thread a(worker, 0);
    std::thread b(worker, 1);
    a.join();
    b.join();

    int doubleWins = 0;
    int noWins = 0;
    for (const auto& round : results)
    {
        const int winners = (round[0] ? 1 : 0) + (round[1] ? 1 : 0);
        doubleWins += winners > 1 ? 1 : 0;
        noWins += winners == 0 ? 1 : 0;
    }
    CHECK(doubleWins == 0);
    CHECK(noWins == 0);
    std::printf("[stress][ctl] takeover race rounds=%d doubleWins=%d noWins=%d\n", kRaceRounds, doubleWins, noWins);
    std::fflush(stdout);
}
