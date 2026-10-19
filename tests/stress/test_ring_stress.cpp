// SPDX-License-Identifier: GPL-3.0-or-later
//
// 传输环的内存序压测：arm64 上的判据门。一个写线程、一个读线程，读方专挑「写方正在覆盖的那一圈边缘」读，
// 逐帧比对读回来的位置编码。四条判据各抓一类缺陷：
//   - badNewer：接受了比 p 新的圈的数据。读到新圈本身不算错，错在 validFrom 没把它排除——这正是 no_intent 变异
//     （读方再也学不到窗口被覆盖过）与 no_read_fence 变异（读方看不见写方的 intent 公告）造成的后果；
//   - badOlder：在 W 以下读到了上一圈的旧值，说明 W 的 acquire / release 那一对没起作用；
//   - badOther：读到了非整数、超出 24 位或 NaN 的样本，多半是半个 float 被别的写方踩了；
//   - 非空转下限：写方、读方、接受帧与「撞上覆盖」各自的计数，证明这一轮真的跑满了，而不是什么都没做就绿了。
//
// 为什么专读边缘：帧 p 的槽在下一次写到 p + kRingFrames 时被覆盖。窗口取在 W - kRingFrames 附近（edge 模式），
// 就是写方此刻正在覆盖的那一段，validFrom 的判定在这里最吃紧；randomLag 模式补上任意滞后位置的读。
// two-writers 模式让两个写方同时写（接管瞬间新旧写方都活着）：样本按绝对位置编码，谁写都一样，只要一方把对方
// 已经发布的帧换成别的位置的值，badOlder / badNewer 就会变红；写方之间互相挡住的次数另有下限。
//
// x86 能证明什么、不能证明什么：x86 是 TSO，两道 fence 在那里都只编译成编译器屏障，样本的读写在硬件上本来就
// 不重排，所以 no_read_fence 在 x86 上仍然是绿的——它只能由原生 arm64 的 CI 变红。no_intent 在任何 CPU 上都会
// 变红：读方根本不知道窗口被覆盖过，validFrom 停在 from，新圈的值被当成有效数据。CI 的压测步骤跑这两个变异
// 构建，并要求它们以 Catch2 断言失败结束（不是崩溃、不是超时）。
// 工作线程只用普通计数，断言一律在主线程 join 之后做；每一轮都按时间封顶，不会挂死。
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>

#include "ipc/LoopRing.h"

namespace
{

using namespace rinne::ipc;
using Clock = std::chrono::steady_clock;

// 每一轮至少跑这么久；非空转下限没达到就继续，最多到上限
constexpr u32 kRoundMs = 300;
constexpr u32 kRoundCapMs = 3000;
// 一轮至少要有这么多写块、这么多正确帧、这么多次读
constexpr u64 kMinChunks = 1000;
constexpr u64 kMinAccepted = 20000;
constexpr u64 kMinReads = 1000;
// 边缘读必须真的撞上覆盖：没有这个下限，读方一次都没碰到正在被覆盖的窗口时压测也是绿的（什么都没测到）
constexpr u64 kMinRacedEdge = 50;
constexpr u64 kMinRacedRandomLag = 1;
// 两个写方的配置里，写方之间至少要互相挡住这么多次
constexpr u64 kMinInFlight = 100;
// 一次 pull 最多拷这么多帧：randomLag 的 m 上限是 2048，edge 的上限更小
constexpr std::size_t kMaxPull = 2048;

float encodeL(u64 p) noexcept
{
    return static_cast<float>(p & 0xFFFFFFu);
}

float encodeR(u64 p) noexcept
{
    return static_cast<float>((p >> 24) & 0xFFFFFFu);
}

// 样本对 → 位置。两个分量都必须落在 [0, 2^24) 且是精确的整数，否则说明读到了撕开的样本
bool decodeFrame(float l, float r, u64& q) noexcept
{
    if (!(l >= 0.0f && l < 16777216.0f) || !(r >= 0.0f && r < 16777216.0f))
    {
        return false;
    }
    const auto li = static_cast<u32>(l);
    const auto ri = static_cast<u32>(r);
    if (static_cast<float>(li) != l || static_cast<float>(ri) != r)
    {
        return false;
    }
    q = static_cast<u64>(li) | (static_cast<u64>(ri) << 24);
    return true;
}

// splitmix64：快、结果确定，每个线程用不同的种子
class Prng
{
public:
    explicit Prng(u64 seed) noexcept : state_(seed) {}

    u64 next() noexcept
    {
        u64 z = (state_ += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }

    u32 below(u32 n) noexcept { return static_cast<u32>(next() % n); }
    i64 range(i64 lo, i64 hi) noexcept { return lo + static_cast<i64>(next() % static_cast<u64>(hi - lo + 1)); }

private:
    u64 state_;
};

struct Config
{
    const char* name;
    bool logUniform; // 块长按 2 的指数均匀取，而不是区间均匀
    u32 chunkLo;
    u32 chunkHi;
    bool randomLag; // 读窗口取任意滞后，而不是贴着一圈边缘
    u32 edgeSpan; // edge：from = W - kRingFrames + d，d ∈ [0, edgeSpan)
    u32 edgeMaxM;
    u32 burst; // 每写这么多块就空转一阵（0 = 不空转）
    u32 pauseSpins;
    u32 writers; // 2 = 两个写方同时写（接管瞬间的重叠）
};

u32 pickChunk(const Config& c, Prng& rng) noexcept
{
    if (!c.logUniform)
    {
        return c.chunkLo + rng.below(c.chunkHi - c.chunkLo + 1);
    }
    u32 elo = 0;
    u32 ehi = 0;
    for (u32 v = c.chunkLo; v > 1; v >>= 1)
    {
        ++elo;
    }
    for (u32 v = c.chunkHi; v > 1; v >>= 1)
    {
        ++ehi;
    }
    const u32 v = 1u << (elo + rng.below(ehi - elo + 1));
    return std::min(std::max(v, c.chunkLo), c.chunkHi);
}

struct Result
{
    u64 chunks = 0;
    u64 frames = 0;
    u64 writeFails = 0;
    u64 inFlight = 0;
    u64 reads = 0;
    u64 acceptedFrames = 0;
    u64 rejectedFrames = 0;
    u64 badNewer = 0;
    u64 badOlder = 0;
    u64 badOther = 0;
    u64 raced = 0;
    u64 millis = 0;
};

Result runRound(const Config& cfg, u64 seed)
{
    auto seg = std::make_unique<RingSegment>();
    LoopRing::initGeometry(*seg);

    std::atomic<bool> stop{false};
    // 主线程只用来判断非空转下限，读到的是近似值；精确的计数在 join 之后从各线程自己的变量里取
    std::atomic<u64> publishedChunks{0};
    std::atomic<u64> publishedReads{0};
    std::atomic<u64> publishedAccepted{0};
    std::atomic<u64> publishedRaced{0};

    u64 rReads = 0;
    u64 rAccepted = 0;
    u64 rRejected = 0;
    u64 rNewer = 0;
    u64 rOlder = 0;
    u64 rOther = 0;
    u64 rRaced = 0;

    // 每个写方各记各的；两个写方的配置模拟接管瞬间新旧写方都活着：intent 让它们按段轮流写，被挡住的一段记 inFlight
    struct WriterCounts
    {
        u64 chunks = 0;
        u64 frames = 0;
        u64 fails = 0;
        u64 inFlight = 0;
    };
    WriterCounts counts[2];
    const auto writerLoop = [&](u64 salt, WriterCounts& c) {
        Prng rng(seed ^ salt);
        u64 sink = 0;
        while (!stop.load(std::memory_order_acquire))
        {
            const u32 k = pickChunk(cfg, rng);
            const RingWriteResult result = LoopRing::write(*seg, k, [](u32, u64 p, float& l, float& r) {
                l = encodeL(p);
                r = encodeR(p);
            });
            if (result == RingWriteResult::kOk)
            {
                ++c.chunks;
                c.frames += k;
                publishedChunks.fetch_add(1, std::memory_order_relaxed);
            }
            else if (result == RingWriteResult::kInFlight)
            {
                ++c.inFlight;
            }
            else
            {
                ++c.fails; // 没有回收，kCasLost 与 kBadLength 都是缺陷
            }
            if (cfg.burst != 0 && (c.chunks % cfg.burst) == 0)
            {
                // 空转一阵：让读方追上来，把窗口钉在「写方正要覆盖的那一段」上
                for (u32 s = 0; s < cfg.pauseSpins; ++s)
                {
                    sink += seg->header.write_head.load(std::memory_order_relaxed);
                }
            }
        }
        (void)sink;
    };
    std::thread writer([&] { writerLoop(0x9e3779b97f4a7c15ull, counts[0]); });
    std::thread second;
    if (cfg.writers > 1)
    {
        second = std::thread([&] { writerLoop(0xc2b2ae3d27d4eb4full, counts[1]); });
    }

    std::thread reader([&] {
        Prng rng(seed ^ 0x2545f4914f6cdd1dull);
        std::array<float, kMaxPull> left{};
        std::array<float, kMaxPull> right{};
        while (!stop.load(std::memory_order_acquire))
        {
            const u64 w = LoopRing::writeHead(*seg);
            if (w < static_cast<u64>(kRingFrames) + 2048u)
            {
                std::this_thread::yield(); // 第一圈还没写完：现在读什么都说明不了问题
                continue;
            }
            const i64 ww = static_cast<i64>(w);
            i64 from = 0;
            i64 m = 0;
            if (!cfg.randomLag)
            {
                const i64 d = rng.range(0, static_cast<i64>(cfg.edgeSpan) - 1);
                from = ww - static_cast<i64>(kRingFrames) + d;
                m = rng.range(1, static_cast<i64>(cfg.edgeMaxM));
            }
            else
            {
                const i64 lag = rng.range(1, static_cast<i64>(kRingFrames));
                from = ww - lag;
                m = rng.range(1, std::min<i64>(lag, static_cast<i64>(kMaxPull)));
            }
            const i64 to = std::min(from + m, ww);
            if (to <= from)
            {
                continue;
            }
            const RingPull pulled = LoopRing::pull(*seg, from, to, [&](i64 p, float l, float r) {
                const auto at = static_cast<std::size_t>(p - from);
                left[at] = l;
                right[at] = r;
            });
            ++rReads;
            // 只有 [max(from, validFrom), to) 里的帧可能是这一圈的完整数据，其余一律按 0 处理
            for (i64 p = std::max(from, pulled.validFrom); p < to; ++p)
            {
                const auto at = static_cast<std::size_t>(p - from);
                u64 q = 0;
                if (!decodeFrame(left[at], right[at], q))
                {
                    ++rOther;
                }
                else if (q > static_cast<u64>(p))
                {
                    ++rNewer;
                }
                else if (q < static_cast<u64>(p))
                {
                    ++rOlder;
                }
                else
                {
                    ++rAccepted;
                }
            }
            rRejected += static_cast<u64>(std::clamp<i64>(pulled.validFrom - from, 0, to - from));
            // 撞上覆盖：拷贝之后重读 W，W - kRingFrames 越过了 from，说明窗口左端的槽已经被新一圈写过。
            // 不拿 intent 判：no_intent 变异下 intent 恒为 0，下限会先于判据变红，变异的证据就不干净了
            if (static_cast<i64>(LoopRing::writeHead(*seg)) - static_cast<i64>(kRingFrames) > from)
            {
                ++rRaced;
            }
            publishedReads.store(rReads, std::memory_order_relaxed);
            publishedAccepted.store(rAccepted, std::memory_order_relaxed);
            publishedRaced.store(rRaced, std::memory_order_relaxed);
        }
    });

    const auto start = Clock::now();
    for (;;)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const auto elapsed =
            static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
        const bool minimums =
            publishedChunks.load(std::memory_order_relaxed) >= kMinChunks &&
            publishedAccepted.load(std::memory_order_relaxed) >= kMinAccepted &&
            publishedReads.load(std::memory_order_relaxed) >= kMinReads &&
            publishedRaced.load(std::memory_order_relaxed) >= (cfg.randomLag ? kMinRacedRandomLag : kMinRacedEdge);
        if (elapsed >= kRoundMs && minimums)
        {
            break;
        }
        if (elapsed >= kRoundCapMs)
        {
            break;
        }
    }
    stop.store(true, std::memory_order_release);
    writer.join();
    if (second.joinable())
    {
        second.join();
    }
    reader.join();

    Result r;
    for (const WriterCounts& c : counts)
    {
        r.chunks += c.chunks;
        r.frames += c.frames;
        r.writeFails += c.fails;
        r.inFlight += c.inFlight;
    }
    r.reads = rReads;
    r.acceptedFrames = rAccepted;
    r.rejectedFrames = rRejected;
    r.badNewer = rNewer;
    r.badOlder = rOlder;
    r.badOther = rOther;
    r.raced = rRaced;
    r.millis = static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
    return r;
}

} // namespace

TEST_CASE("the transfer ring never accepts a frame a newer lap has overwritten", "[stress][ring]")
{
    const Config configs[] = {
        {"edge-small-chunks", false, 1, 64, false, 1024, 64, 0, 0, 1},
        {"edge-loguniform", true, 1, 8192, false, 4096, 64, 0, 0, 1},
        {"randomlag-loguniform", true, 1, 8192, true, 0, 0, 0, 0, 1},
        {"edge-large-burst", false, 4096, 8192, false, 8192, 256, 4, 20000, 1},
        {"edge-two-writers", false, 1, 256, false, 2048, 64, 0, 0, 2},
    };

    u64 seed = 0x5eed1234u;
    for (const Config& cfg : configs)
    {
        INFO("config " << cfg.name);
        const Result r = runRound(cfg, seed);
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;

        // 四条判据各自独立：变异构建必须至少让 badNewer 变红，而正确的构建一条都不许红
        CHECK(r.badNewer == 0);
        CHECK(r.badOlder == 0);
        CHECK(r.badOther == 0);
        CHECK(r.writeFails == 0);
        // 单写方从不被挡；两个写方必须真的互相挡过，否则这一轮没测到接管重叠
        if (cfg.writers > 1)
        {
            CHECK(r.inFlight >= kMinInFlight);
        }
        else
        {
            CHECK(r.inFlight == 0);
        }
        // 非空转下限：这一轮真的写够、读够、并且真的撞上过覆盖
        CHECK(r.chunks >= kMinChunks);
        CHECK(r.acceptedFrames >= kMinAccepted);
        CHECK(r.reads >= kMinReads);
        CHECK(r.raced >= (cfg.randomLag ? kMinRacedRandomLag : kMinRacedEdge));

        std::printf("[stress][ring] %-21s chunks=%llu frames=%llu reads=%llu accepted=%llu rejected=%llu "
                    "badNewer=%llu badOlder=%llu badOther=%llu raced=%llu writeFails=%llu inFlight=%llu ms=%llu\n",
                    cfg.name, static_cast<unsigned long long>(r.chunks), static_cast<unsigned long long>(r.frames),
                    static_cast<unsigned long long>(r.reads), static_cast<unsigned long long>(r.acceptedFrames),
                    static_cast<unsigned long long>(r.rejectedFrames), static_cast<unsigned long long>(r.badNewer),
                    static_cast<unsigned long long>(r.badOlder), static_cast<unsigned long long>(r.badOther),
                    static_cast<unsigned long long>(r.raced), static_cast<unsigned long long>(r.writeFails),
                    static_cast<unsigned long long>(r.inFlight), static_cast<unsigned long long>(r.millis));
        std::fflush(stdout);
    }
}
