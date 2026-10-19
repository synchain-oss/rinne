// SPDX-License-Identifier: GPL-3.0-or-later
//
// ctl 整字的转移：CAS 每次让 gen + 1，失败返回当前值，gen 回绕回 0 之后同一个整字还要 2^32 次才会再现；
// 两个线程拿着同一个旧整字同时接管，恰好一个赢；释放的 TOCTOU 里释放方输给接管方就什么都不改。
// 接管档位的判定是一张表：每一档的正例，以及每个条件各自的边界（幽灵 999/1000 ms、死进程 4999/5000 ms、
// 同进程孤儿 29999/30000 ms、休眠 1999/2000 ms、活着的外进程、未来时间戳、心跳 0、不稳定样本）。
// 探活函数是注入的，所以这一组用例不碰任何进程。
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "ipc/Rendezvous.h"

namespace
{

using namespace rinne::ipc;

// 接管表里的「现在」与两个 pid
constexpr u64 kNow = 100000;
constexpr u32 kSelf = 1000;
constexpr u32 kForeign = 2000;
// 比 now 还大的「年龄」：对应的时间戳写成 0（从没写过）
constexpr u64 kNever = 1000000;

bool gPidAlive = false;

bool pidAlive(u32) noexcept
{
    return gPidAlive;
}

// 时间戳 = now - age；age 比 now 大就写成 0
u64 stamp(u64 age) noexcept
{
    return age >= kNow ? 0 : kNow - age;
}

SlotSample sampleFor(u64 ctl, u32 pid, u64 heartbeatAge, u64 ctlAge, u64 seqAge, u64 claimAge = kNever,
                     u64 dormantAge = kNever) noexcept
{
    SlotSample s;
    s.ctl = ctl;
    s.pid = pid;
    s.heartbeatMs = stamp(heartbeatAge);
    s.claimMs = stamp(claimAge);
    s.dormantMs = stamp(dormantAge);
    s.ctlSinceMs = stamp(ctlAge);
    s.seqSinceMs = stamp(seqAge);
    s.stable = true;
    return s;
}

// 两个线程每轮共用同一个同步点；n 固定为 2
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

TEST_CASE("casCtl writes gen + 1 and reports the word it saw when it loses", "[ipc][ctl]")
{
    std::atomic<u64> ctl{makeCtl(0, kCtlPhaseFree)};
    const CtlCas first = casCtl(ctl, makeCtl(0, kCtlPhaseFree), kCtlPhaseClaiming);
    CHECK(first.ok);
    CHECK(first.word == makeCtl(1, kCtlPhaseClaiming));
    CHECK(ctl.load() == first.word);

    const CtlCas wrong = casCtl(ctl, makeCtl(99, kCtlPhaseFree), kCtlPhaseActive);
    CHECK_FALSE(wrong.ok);
    CHECK(wrong.word == first.word); // 失败时带回 CAS 看到的当前值

    // gen 从 0xffffffff 回绕到 0：同一个整字要 2^32 次转移之后才会再现
    ctl.store(makeCtl(0xffffffffu, kCtlPhaseActive));
    const CtlCas wrapped = casCtl(ctl, ctl.load(), kCtlPhaseDormant);
    CHECK(wrapped.ok);
    CHECK(wrapped.word == makeCtl(0, kCtlPhaseDormant));
    CHECK(ctl.load() == wrapped.word);
}

TEST_CASE("two holders of the same stale ctl word cannot both take over", "[ipc][ctl]")
{
    const u64 stale = makeCtl(7, kCtlPhaseActive);
    std::atomic<u64> ctl{stale};
    const CtlCas first = casCtl(ctl, stale, kCtlPhaseClaiming);
    const CtlCas second = casCtl(ctl, stale, kCtlPhaseClaiming);
    CHECK(first.ok);
    CHECK_FALSE(second.ok);

    // 一轮 free -> claiming -> active 之后状态和 stale 一样，但 gen 不同：旧整字仍然赢不了
    CHECK(casCtl(ctl, ctl.load(), kCtlPhaseFree).ok);
    CHECK(casCtl(ctl, ctl.load(), kCtlPhaseClaiming).ok);
    CHECK(casCtl(ctl, ctl.load(), kCtlPhaseActive).ok);
    CHECK(ctlPhase(ctl.load()) == kCtlPhaseActive);
    CHECK(ctlGen(ctl.load()) != ctlGen(stale));
    CHECK_FALSE(casCtl(ctl, stale, kCtlPhaseClaiming).ok);
}

TEST_CASE("two threads racing the same observed word elect exactly one winner per round", "[ipc][ctl]")
{
    constexpr int kRounds = 20000;
    std::atomic<u64> ctl{makeCtl(0, kCtlPhaseFree)};
    std::atomic<u64> startWord{0};
    std::vector<std::array<bool, 2>> results(static_cast<std::size_t>(kRounds));
    SpinBarrier start(2);
    SpinBarrier done(2);
    const auto worker = [&](int who) {
        for (int round = 0; round < kRounds; ++round)
        {
            // 0 号线程每轮先把当前整字公告出来；屏障保证两个线程拿到的是同一个 observed
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
}

TEST_CASE("a release that loses the race to a takeover changes nothing", "[ipc][ctl]")
{
    const u64 mine = makeCtl(12, kCtlPhaseActive);
    std::atomic<u64> ctl{mine};
    CHECK(ctl.load() == mine); // 属主在这一拍的自检

    const CtlCas thief = casCtl(ctl, mine, kCtlPhaseClaiming);
    REQUIRE(thief.ok); // 自检与释放之间被抢走
    const CtlCas release = casCtl(ctl, mine, kCtlPhaseFree);
    CHECK_FALSE(release.ok);
    CHECK(ctl.load() == thief.word); // 槽保持接管者的整字，释放方什么都不修
}

TEST_CASE("the takeover tier follows the evidence table and every boundary", "[ipc][ctl]")
{
    const u64 free = makeCtl(3, kCtlPhaseFree);
    const u64 claiming = makeCtl(3, kCtlPhaseClaiming);
    const u64 active = makeCtl(4, kCtlPhaseActive);
    const u64 provisional = makeCtl(4, kCtlPhaseActive | kCtlProvisional);
    const u64 dormant = makeCtl(4, kCtlPhaseDormant);

    struct Row
    {
        const char* name;
        SlotSample sample;
        ClaimIntent intent;
        bool alive;
        TakeoverTier want;
    };
    std::vector<Row> rows;
    const auto add = [&rows](const char* name, const SlotSample& s, ClaimIntent intent, bool alive, TakeoverTier want) {
        rows.push_back(Row{name, s, intent, alive, want});
    };

    SlotSample unstable = sampleFor(active, kForeign, 0, 9000, 9000);
    unstable.stable = false;
    add("an unstable sample is never acted on", unstable, ClaimIntent::kManual, true, TakeoverTier::kNone);

    add("free", sampleFor(free, 0, kNever, kNever, kNever), ClaimIntent::kManual, false, TakeoverTier::kFree);
    add("free with stale fields", sampleFor(free, kForeign, 9000, 9000, 9000), ClaimIntent::kManual, true,
        TakeoverTier::kFree);

    add("ghost at 999 ms", sampleFor(claiming, kForeign, 9000, 999, 9000, 9000), ClaimIntent::kManual, false,
        TakeoverTier::kNone);
    add("ghost at 1000 ms", sampleFor(claiming, kForeign, 9000, 1000, 9000, 9000), ClaimIntent::kManual, false,
        TakeoverTier::kGhost);
    add("ghost with a fresh claim_ms", sampleFor(claiming, kForeign, 9000, 1000, 9000, 999), ClaimIntent::kManual,
        false, TakeoverTier::kNone);
    add("ghost with claim_ms never written", sampleFor(claiming, kForeign, 9000, 1000, 9000, kNever),
        ClaimIntent::kManual, false, TakeoverTier::kGhost);
    add("ghost with claim_ms at exactly 1000 ms", sampleFor(claiming, kForeign, 9000, 9000, 9000, 1000),
        ClaimIntent::kManual, false, TakeoverTier::kGhost);

    add("provisional slot with kRestore", sampleFor(provisional, kForeign, 0, 9000, 9000), ClaimIntent::kRestore, true,
        TakeoverTier::kPreemptProvisional);
    add("provisional slot with kManual", sampleFor(provisional, kForeign, 0, 9000, 9000), ClaimIntent::kManual, true,
        TakeoverTier::kNone);
    add("provisional slot with kSmart", sampleFor(provisional, kForeign, 0, 9000, 9000), ClaimIntent::kSmart, true,
        TakeoverTier::kNone);
    add("provisional slot with unstable sample", unstable, ClaimIntent::kRestore, true, TakeoverTier::kNone);

    add("dead process at 4999 ms", sampleFor(active, kForeign, 4999, 9000, 9000), ClaimIntent::kManual, false,
        TakeoverTier::kNone);
    add("dead process at 5000 ms", sampleFor(active, kForeign, 5000, 5000, 5000), ClaimIntent::kManual, false,
        TakeoverTier::kDeadProcess);
    add("dead process with a moving block_seq", sampleFor(active, kForeign, 6000, 6000, 4999), ClaimIntent::kManual,
        false, TakeoverTier::kNone);
    add("dead process with a changing ctl", sampleFor(active, kForeign, 6000, 4999, 6000), ClaimIntent::kManual, false,
        TakeoverTier::kNone);
    add("live foreign pid stays", sampleFor(active, kForeign, 3600000, 3600000, 3600000), ClaimIntent::kManual, true,
        TakeoverTier::kNone);
    add("dead process with heartbeat never written", sampleFor(active, kForeign, kNever, 6000, 6000),
        ClaimIntent::kManual, false, TakeoverTier::kDeadProcess);
    SlotSample futureHeartbeat = sampleFor(active, kForeign, 0, 6000, 6000);
    futureHeartbeat.heartbeatMs = kNow + 1000;
    add("dead process with a future heartbeat", futureHeartbeat, ClaimIntent::kManual, false, TakeoverTier::kNone);
    add("dormant dead process is a dead process", sampleFor(dormant, kForeign, 6000, 6000, 6000, kNever, 6000),
        ClaimIntent::kManual, false, TakeoverTier::kDeadProcess);

    add("orphan at 29999 ms", sampleFor(active, kSelf, 29999, 29999, 29999), ClaimIntent::kManual, true,
        TakeoverTier::kNone);
    add("orphan at 30000 ms", sampleFor(active, kSelf, 30000, 30000, 30000), ClaimIntent::kManual, true,
        TakeoverTier::kOrphan);
    add("orphan with a moving block_seq", sampleFor(active, kSelf, 30000, 30000, 29999), ClaimIntent::kManual, true,
        TakeoverTier::kNone);
    add("orphan with a fresher heartbeat", sampleFor(active, kSelf, 29999, 30000, 30000), ClaimIntent::kManual, true,
        TakeoverTier::kNone);
    add("orphan with a changing ctl", sampleFor(active, kSelf, 30000, 29999, 30000), ClaimIntent::kManual, true,
        TakeoverTier::kNone);
    add("own pid is never a dead process", sampleFor(active, kSelf, 6000, 6000, 6000), ClaimIntent::kManual, false,
        TakeoverTier::kNone);
    add("dormant orphan is an orphan", sampleFor(dormant, kSelf, 30000, 30000, 30000), ClaimIntent::kManual, true,
        TakeoverTier::kOrphan);

    add("dormant at 1999 ms", sampleFor(dormant, kForeign, 0, 1999, 9000, kNever, 1999), ClaimIntent::kRestore, true,
        TakeoverTier::kNone);
    add("dormant at 2000 ms with kRestore", sampleFor(dormant, kForeign, 0, 2000, 9000, kNever, 2000),
        ClaimIntent::kRestore, true, TakeoverTier::kDormant);
    add("dormant at 2000 ms with kManualConfirmed", sampleFor(dormant, kForeign, 0, 2000, 9000, kNever, 2000),
        ClaimIntent::kManualConfirmed, true, TakeoverTier::kDormant);
    add("dormant at 2000 ms with kManual", sampleFor(dormant, kForeign, 0, 2000, 9000, kNever, 2000),
        ClaimIntent::kManual, true, TakeoverTier::kNone);
    add("dormant at 2000 ms with kConflictRetry", sampleFor(dormant, kForeign, 0, 2000, 9000, kNever, 2000),
        ClaimIntent::kConflictRetry, true, TakeoverTier::kNone);
    add("dormant with a ctl seen for 1999 ms", sampleFor(dormant, kForeign, 0, 1999, 9000, kNever, 9000),
        ClaimIntent::kRestore, true, TakeoverTier::kNone);
    add("dormant with dormant_ms at 1999 ms", sampleFor(dormant, kForeign, 0, 9000, 9000, kNever, 1999),
        ClaimIntent::kRestore, true, TakeoverTier::kNone);

    REQUIRE(rows.size() >= 25);
    for (const Row& row : rows)
    {
        INFO(row.name);
        gPidAlive = row.alive;
        CHECK(takeoverTier(row.sample, kNow, kSelf, row.intent, &pidAlive) == row.want);
    }
}
