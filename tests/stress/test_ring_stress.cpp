// SPDX-License-Identifier: GPL-3.0-or-later
//
// 传输环的内存序压测：arm64 上的判据门。一个写线程、一个读线程，读方专挑「写方正在覆盖的那一圈边缘」读，
// 逐帧比对读回来的位置编码。样本编码：L = p 的低 24 位，R = p 的 24–39 位，外加 p 的 16–23 位（圈号的低 8 位）
// 放在 R 的 16–23 位，于是一帧的两个声道各带一份圈号（压测里的位置远小于 2^40）。五条判据各抓一类缺陷：
//   - badNewer：接受了比 p 新的圈的整帧数据。读到新圈本身不算错，错在 validFrom 没把它排除——这正是 no_intent 变异
//     （读方再也学不到窗口被覆盖过）与 no_read_fence 变异（读方看不见写方的 intent 公告）造成的后果；
//   - badTorn：同一帧的左右声道来自不同的圈（拷贝途中被覆盖，两半分两次读），后果与 badNewer 相同，只是撕开了；
//   - badOlder：在 W 以下读到了上一圈的整帧旧值，说明 W 的 acquire / release 那一对没起作用；
//   - badOther：读到了非整数、超出 24 位或 NaN 的样本，多半是半个 float 被别的写方踩了；
//   - 非空转下限：写方、读方、接受帧与「撞上覆盖」各自的计数，证明这一轮真的跑满了，而不是什么都没做就绿了。
//
// 为什么专读边缘：帧 p 的槽在下一次写到 p + kRingFrames 时被覆盖。窗口取在 W - kRingFrames 附近（edge 模式），
// 就是写方此刻正在覆盖的那一段，validFrom 的判定在这里最吃紧；randomLag 模式补上任意滞后位置的读。
// two-writers 模式让两个写方同时写（接管瞬间新旧写方都活着）：样本按绝对位置编码，谁写都一样，只要一方把对方
// 已经发布的帧换成别的位置的值，badOlder / badNewer 就会变红；写方之间互相挡住的次数另有下限。两个写方往同一个位置
// 写的值相同，这里看不出「输的一方也写了样本」：互斥的判据是 writeFails == 0，加上单测里被挡住的写方一个样本都
// 不写（它的 source 一次都不会被调用）。
//
// x86 能证明什么、不能证明什么：x86 是 TSO，两道 fence 在那里都只编译成编译器屏障，样本的读写在硬件上本来就
// 不重排，所以 no_read_fence 在 x86 上抓不到：跑满 25 s 后以「证据不成立」结束（退出码 70），只有原生 arm64 的
// CI 能抓到。no_intent 在任何 CPU 上都会
// 变红：读方根本不知道窗口被覆盖过，validFrom 停在 from，新圈的值被当成有效数据。CI 的压测步骤跑这两个变异
// 构建，并要求它们以 Catch2 断言失败结束（不是崩溃、不是超时）。
// 变异构建里另有正向取证（见 StressMutation.h）：所有配置一遍遍地跑，直到 badNewer + badTorn 攒够 20 次（封顶
// 25 s），不够就不算抓住。no_intent 下两个写方本来就没有互斥，那一组 intent 相关的两条检查从构造上必然失败，
// 所以跳过它们，它的帧也不计入命中。edge-tiny-chunks 一组（块长 1–16、窗口 32 帧）专门把读方压在覆盖的边缘上，
// 给读侧 fence 的变异多一些命中。读侧 fence 的变异证据来自 edge-small-chunks、edge-tiny-chunks、edge-loguniform
// 与 edge-two-writers；randomlag-loguniform 与 edge-large-burst 从构造上对这种重排不敏感（读窗口离正被覆盖的边缘远，
// 或块大、竞争窗口很少重合），它们在常规构建里的绿不构成读侧 fence 的证据，前四组不是冗余的。
// 文件末尾另有一个用例补三种形状（4 倍速率的写方、按宿主块写并切段、60k
// 帧滞后加读方停顿），见那里的注释；它在变异构建里 不跑。环境变量 RINNE_STRESS_SCALE
// 把每一轮的时长放大（StressScale.h），本机 Full 档与手动长跑用。 工作线程只用普通计数，断言一律在主线程 join
// 之后做；每一轮都按时间封顶，不会挂死。
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>

#include "StressMutation.h"
#include "StressScale.h"
#include "ipc/LoopRing.h"

namespace
{

using namespace rinne::ipc;
using namespace rinne::stress;
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
    return static_cast<float>(((p >> 24) & 0xFFFFu) | (((p >> 16) & 0xFFu) << 16));
}

enum class Decoded : std::uint8_t
{
    kFrame, // 两半同一圈，q 是它编码的位置
    kTorn, // 两半的圈号对不上
    kGarbage, // 不是这套编码写得出来的值
};

// 样本对 → 位置。两个分量都必须落在 [0, 2^24) 且是精确的整数，两份圈号还必须相同
Decoded decodeFrame(float l, float r, u64& q) noexcept
{
    if (!(l >= 0.0f && l < 16777216.0f) || !(r >= 0.0f && r < 16777216.0f))
    {
        return Decoded::kGarbage;
    }
    const auto li = static_cast<u32>(l);
    const auto ri = static_cast<u32>(r);
    if (static_cast<float>(li) != l || static_cast<float>(ri) != r)
    {
        return Decoded::kGarbage;
    }
    if (((li >> 16) & 0xFFu) != (ri >> 16))
    {
        return Decoded::kTorn;
    }
    q = static_cast<u64>(li) | (static_cast<u64>(ri & 0xFFFFu) << 24);
    return Decoded::kFrame;
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

// 变速、宿主块与大滞后的扩展。全部取默认值（0）时与原来的行为完全一样，原有六组配置不带它。
//   paceFramesPerSec  写方按这个帧速率配速（写完一块就等到「已写帧数 / 速率」那一刻再写下一块）；0 = 全速。48 kHz 的 4
//   倍是 192000 hostBlockMax      > 0：写方按「宿主块」写：块长在 [1, hostBlockMax] 对数均匀，经 RingWriter 按
//   kMaxWriteChunk 切段，
//                     与 OutputEngine 的分段循环同形（某一段被挡住也接着写后面的段，写头 CAS 失败就结束这一块）
//   lagLo / lagHi     > 0：随机滞后读的滞后取 [lagLo, lagHi]，而不是 [1, kRingFrames]
//   stallMaxUs        > 0：读方每 4 次里有 1 次，在取到 W 之后先停最多这么久再拷贝（模拟读线程被调度走）：配上快写方，
//                     窗口左端会被新一圈覆盖，validFrom 必须把这些帧排除
//   roundMs / capMs   这一组每轮的最短与封顶时长（0 = kRoundMs / kRoundCapMs）；配速的写方出块慢，轮要更长
//   min*              这一组的非空转下限（0 = 用全局默认）
struct Extras
{
    u64 paceFramesPerSec = 0;
    u32 hostBlockMax = 0;
    u32 lagLo = 0;
    u32 lagHi = 0;
    u32 stallMaxUs = 0;
    u32 roundMs = 0;
    u32 capMs = 0;
    u64 minChunks = 0;
    u64 minReads = 0;
    u64 minAccepted = 0;
    u64 minRaced = 0;
    u64 minStalledRaced = 0; // stallMaxUs > 0 时：被停顿过、且停顿期间窗口左端被新一圈覆盖的读至少这么多
};

// 对数均匀的块长：先在 [0, log2(hi)] 里均匀取一个指数 e，再在 [2^e, 2^(e+1)) 里均匀取值，最后夹在 hi 以内。
// 与 pickChunk 不同，结果不只是 2 的幂，所以宿主块的切段边界（8192 的整数倍）会遇到各种余数
u32 pickLogRange(u32 hi, Prng& rng) noexcept
{
    u32 top = 0;
    for (u32 v = hi; v > 1; v >>= 1)
    {
        ++top;
    }
    const u32 base = 1u << rng.below(top + 1);
    return std::min(base + rng.below(base), hi);
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
    u64 badTorn = 0;
    u64 badOlder = 0;
    u64 badOther = 0;
    u64 raced = 0;
    u64 stalledRaced = 0; // 先停顿、再拷贝，拷贝之后发现窗口左端已被覆盖的读
    u64 lagMin = ~0ull; // 随机滞后读实际用到的最小与最大滞后（W - from）；没有随机滞后读时 lagMin 保持 ~0、lagMax 为 0
    u64 lagMax = 0;
    u64 millis = 0;
    // 宿主块模式（hostBlockMax > 0）
    u64 hostBlocks = 0;
    u64 splitBlocks = 0; // 长度超过 kMaxWriteChunk、被切成几段的宿主块
    u64 maxHostBlock = 0;
    u64 head = 0; // 收尾时的写头；单写方、没有失败时必须等于 frames
};

// 两个写方都在 intent 下互斥时才要求它们真的互相挡过；no_intent 下根本不会挡
bool needsInFlight(const Config& cfg) noexcept
{
    return cfg.writers > 1 && !kNoIntent;
}

u64 minRaced(const Config& cfg) noexcept
{
    return cfg.randomLag ? kMinRacedRandomLag : kMinRacedEdge;
}

bool nonVacuous(const Config& cfg, const Result& r) noexcept
{
    return r.chunks >= kMinChunks && r.acceptedFrames >= kMinAccepted && r.reads >= kMinReads &&
           r.raced >= minRaced(cfg) && (!needsInFlight(cfg) || r.inFlight >= kMinInFlight);
}

// 每个写方各自发布自己被挡住的次数，主线程只拿它判断能不能提前收尾。手工填到 128 字节（alignas 在 MSVC 上触发
// C4324）：两个计数相隔 128 字节，缓存行不论 64 还是 128 字节（Apple M），两个写方都不会争同一条线。只影响节奏，
// 不影响判据
struct PaddedCounter
{
    std::atomic<u64> value{0};
    unsigned char pad[128 - sizeof(std::atomic<u64>)] = {};
};

Result runRound(const Config& cfg, u64 seed, const Extras& extra = {})
{
    const u64 wantChunks = extra.minChunks != 0 ? extra.minChunks : kMinChunks;
    const u64 wantReads = extra.minReads != 0 ? extra.minReads : kMinReads;
    const u64 wantAccepted = extra.minAccepted != 0 ? extra.minAccepted : kMinAccepted;
    const u64 wantRaced = extra.minRaced != 0 ? extra.minRaced : minRaced(cfg);
    const u64 roundMs = scaledMs(extra.roundMs != 0 ? extra.roundMs : kRoundMs);
    const u64 capMs = scaledMs(extra.capMs != 0 ? extra.capMs : kRoundCapMs);
    auto seg = std::make_unique<RingSegment>();
    LoopRing::initGeometry(*seg);

    std::atomic<bool> stop{false};
    // 主线程只用来判断非空转下限，读到的是近似值；精确的计数在 join 之后从各线程自己的变量里取
    std::atomic<u64> publishedChunks{0};
    std::atomic<u64> publishedReads{0};
    std::atomic<u64> publishedAccepted{0};
    std::atomic<u64> publishedRaced{0};
    std::atomic<u64> publishedStalledRaced{0};
    PaddedCounter publishedInFlight[2];

    u64 rReads = 0;
    u64 rAccepted = 0;
    u64 rRejected = 0;
    u64 rNewer = 0;
    u64 rTorn = 0;
    u64 rOlder = 0;
    u64 rOther = 0;
    u64 rRaced = 0;
    u64 rStalledRaced = 0;
    u64 rLagMin = ~0ull;
    u64 rLagMax = 0;

    // 每个写方各记各的；两个写方的配置模拟接管瞬间新旧写方都活着：intent 让它们按段轮流写，被挡住的一段记 inFlight
    struct WriterCounts
    {
        u64 chunks = 0;
        u64 frames = 0;
        u64 fails = 0;
        u64 inFlight = 0;
        u64 hostBlocks = 0;
        u64 splitBlocks = 0;
        u64 maxHostBlock = 0;
    };
    WriterCounts counts[2];
    const auto writerLoop = [&](u64 salt, WriterCounts& c, PaddedCounter& inFlightOut) {
        Prng rng(seed ^ salt);
        u64 sink = 0;
        RingWriter ringWriter; // 宿主块模式经它写，与 OutputEngine 一样
        ringWriter.reset(48000);
        const auto paceStart = Clock::now();
        const auto fillSample = [](u32, u64 p, float& l, float& r) {
            l = encodeL(p);
            r = encodeR(p);
        };
        const auto count = [&](RingWriteResult result, u32 k) {
            if (result == RingWriteResult::kOk)
            {
                ++c.chunks;
                c.frames += k;
                publishedChunks.fetch_add(1, std::memory_order_relaxed);
            }
            else if (result == RingWriteResult::kInFlight)
            {
                ++c.inFlight;
                inFlightOut.value.store(c.inFlight, std::memory_order_relaxed);
            }
            else
            {
                ++c.fails; // 没有回收，kCasLost 与 kBadLength 都是缺陷
            }
        };
        while (!stop.load(std::memory_order_acquire))
        {
            if (extra.hostBlockMax == 0)
            {
                const u32 k = pickChunk(cfg, rng);
                count(LoopRing::write(*seg, k, fillSample), k);
            }
            else
            {
                // 一个宿主块：按 kMaxWriteChunk 切段，每段各自公告与发布；挡住了接着写后面的段，别的失败结束这一块
                const u32 n = pickLogRange(extra.hostBlockMax, rng);
                ++c.hostBlocks;
                c.splitBlocks += n > kMaxWriteChunk ? 1u : 0u;
                c.maxHostBlock = std::max<u64>(c.maxHostBlock, n);
                for (u32 offset = 0; offset < n;)
                {
                    const u32 k = std::min<u32>(kMaxWriteChunk, n - offset);
                    const RingWriteResult result = ringWriter.write(*seg, k, fillSample);
                    count(result, k);
                    offset += k;
                    if (result != RingWriteResult::kOk && result != RingWriteResult::kInFlight)
                    {
                        break;
                    }
                }
            }
            if (cfg.burst != 0 && (c.chunks % cfg.burst) == 0)
            {
                // 空转一阵：让读方追上来，把窗口钉在「写方正要覆盖的那一段」上
                for (u32 s = 0; s < cfg.pauseSpins; ++s)
                {
                    sink += seg->header.write_head.load(std::memory_order_relaxed);
                }
            }
            if (extra.paceFramesPerSec != 0)
            {
                // 配速：到「已写帧数 / 速率」那一刻才写下一块；落后了就不等，追上去（平均速率就是 paceFramesPerSec）
                const auto due = paceStart + std::chrono::microseconds(c.frames * 1000000ull / extra.paceFramesPerSec);
                while (!stop.load(std::memory_order_acquire) && Clock::now() < due)
                {
                    std::this_thread::yield();
                }
            }
        }
        (void)sink;
    };
    std::thread writer([&] { writerLoop(0x9e3779b97f4a7c15ull, counts[0], publishedInFlight[0]); });
    std::thread second;
    if (cfg.writers > 1)
    {
        second = std::thread([&] { writerLoop(0xc2b2ae3d27d4eb4full, counts[1], publishedInFlight[1]); });
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
                const i64 lag = extra.lagHi != 0
                                    ? rng.range(static_cast<i64>(extra.lagLo), static_cast<i64>(extra.lagHi))
                                    : rng.range(1, static_cast<i64>(kRingFrames));
                from = ww - lag;
                m = rng.range(1, std::min<i64>(lag, static_cast<i64>(kMaxPull)));
                rLagMin = std::min(rLagMin, static_cast<u64>(lag));
                rLagMax = std::max(rLagMax, static_cast<u64>(lag));
            }
            const i64 to = std::min(from + m, ww);
            if (to <= from)
            {
                continue;
            }
            bool stalled = false;
            if (extra.stallMaxUs != 0 && rng.below(4) == 0)
            {
                // 取到 W 之后被调度走一会儿：to 仍不超过自己读到的 W，写方这段时间里已经往前跑了
                stalled = true;
                std::this_thread::sleep_for(std::chrono::microseconds(rng.below(extra.stallMaxUs)));
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
                const Decoded decoded = decodeFrame(left[at], right[at], q);
                if (decoded == Decoded::kGarbage)
                {
                    ++rOther;
                }
                else if (decoded == Decoded::kTorn)
                {
                    ++rTorn;
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
                rStalledRaced += stalled ? 1u : 0u;
            }
            publishedReads.store(rReads, std::memory_order_relaxed);
            publishedAccepted.store(rAccepted, std::memory_order_relaxed);
            publishedRaced.store(rRaced, std::memory_order_relaxed);
            publishedStalledRaced.store(rStalledRaced, std::memory_order_relaxed);
        }
    });

    const auto start = Clock::now();
    for (;;)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const auto elapsed =
            static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
        const u64 inFlight = publishedInFlight[0].value.load(std::memory_order_relaxed) +
                             publishedInFlight[1].value.load(std::memory_order_relaxed);
        const bool minimums = publishedChunks.load(std::memory_order_relaxed) >= wantChunks &&
                              publishedAccepted.load(std::memory_order_relaxed) >= wantAccepted &&
                              publishedReads.load(std::memory_order_relaxed) >= wantReads &&
                              publishedRaced.load(std::memory_order_relaxed) >= wantRaced &&
                              publishedStalledRaced.load(std::memory_order_relaxed) >= extra.minStalledRaced &&
                              (!needsInFlight(cfg) || inFlight >= kMinInFlight);
        if (elapsed >= roundMs && minimums)
        {
            break;
        }
        if (elapsed >= capMs)
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
        r.hostBlocks += c.hostBlocks;
        r.splitBlocks += c.splitBlocks;
        r.maxHostBlock = std::max(r.maxHostBlock, c.maxHostBlock);
    }
    r.head = LoopRing::writeHead(*seg);
    r.reads = rReads;
    r.acceptedFrames = rAccepted;
    r.rejectedFrames = rRejected;
    r.badNewer = rNewer;
    r.badTorn = rTorn;
    r.badOlder = rOlder;
    r.badOther = rOther;
    r.raced = rRaced;
    r.stalledRaced = rStalledRaced;
    r.lagMin = rLagMin;
    r.lagMax = rLagMax;
    r.millis = static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
    return r;
}

} // namespace

TEST_CASE("the transfer ring never accepts a frame a newer lap has overwritten", "[stress][ring]")
{
    const Config configs[] = {
        {"edge-small-chunks", false, 1, 64, false, 1024, 64, 0, 0, 1},
        {"edge-tiny-chunks", false, 1, 16, false, 32, 16, 0, 0, 1},
        {"edge-loguniform", true, 1, 8192, false, 4096, 64, 0, 0, 1},
        {"randomlag-loguniform", true, 1, 8192, true, 0, 0, 0, 0, 1},
        {"edge-large-burst", false, 4096, 8192, false, 8192, 256, 4, 20000, 1},
        {"edge-two-writers", false, 1, 256, false, 2048, 64, 0, 0, 2},
    };

    u64 seed = 0x5eed1234u;
    // 变异构建：接受了新一圈数据的帧（整帧或撕开的一半）攒了多少次；两个变异都作用在环上
    u64 hits = 0;
    const auto start = Clock::now();
    for (u32 pass = 1;; ++pass)
    {
        for (const Config& cfg : configs)
        {
            INFO("config " << cfg.name << " pass " << pass);
            const Result r = runRound(cfg, seed);
            seed = seed * 6364136223846793005ull + 1442695040888963407ull;
            // no_intent 下两个写方没有互斥：writeFails 与 inFlight 两条从构造上必然失败，跳过，它的帧也不计入命中
            const bool intentChecks = !(kNoIntent && cfg.writers > 1);

            // 判据各自独立：变异构建必须让 badNewer 或 badTorn 变红，而正确的构建一条都不许红
            CHECK(r.badNewer == 0);
            CHECK(r.badTorn == 0);
            CHECK(r.badOlder == 0);
            CHECK(r.badOther == 0);
            if (intentChecks)
            {
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
            }
            // 非空转下限：这一轮真的写够、读够、并且真的撞上过覆盖
            CHECK(r.chunks >= kMinChunks);
            CHECK(r.acceptedFrames >= kMinAccepted);
            CHECK(r.reads >= kMinReads);
            CHECK(r.raced >= minRaced(cfg));

            std::printf("[stress][ring] %-21s chunks=%llu frames=%llu reads=%llu accepted=%llu rejected=%llu "
                        "badNewer=%llu badTorn=%llu badOlder=%llu badOther=%llu raced=%llu writeFails=%llu "
                        "inFlight=%llu ms=%llu\n",
                        cfg.name, static_cast<unsigned long long>(r.chunks), static_cast<unsigned long long>(r.frames),
                        static_cast<unsigned long long>(r.reads), static_cast<unsigned long long>(r.acceptedFrames),
                        static_cast<unsigned long long>(r.rejectedFrames), static_cast<unsigned long long>(r.badNewer),
                        static_cast<unsigned long long>(r.badTorn), static_cast<unsigned long long>(r.badOlder),
                        static_cast<unsigned long long>(r.badOther), static_cast<unsigned long long>(r.raced),
                        static_cast<unsigned long long>(r.writeFails), static_cast<unsigned long long>(r.inFlight),
                        static_cast<unsigned long long>(r.millis));
            std::fflush(stdout);

            requireNonVacuous(nonVacuous(cfg, r), "环的一轮没有跑满非空转下限");
            if (intentChecks)
            {
                hits += r.badNewer + r.badTorn;
            }
        }
        const auto elapsed =
            static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
        if (!anotherPass(kMutated, hits, elapsed))
        {
            break;
        }
    }
    requireHits(kMutated, hits, "环接受了新一圈的帧（badNewer + badTorn）");
}

// 写方变速、宿主块分段与大滞后：产品里读写两端的速率与块划分从来不一样（离线导出时 Output 比实时快好几倍，宿主块最大
// 32768 帧、写环按 8192 帧切段，Input 的读线程被调度走之后滞后接近一整圈）。上面那组配置全速写、读方只贴边缘读，这里补
// 这三种形状，判据与上面完全一样：读出的每一帧要么是自己的位置、要么被 validFrom 排除。
//   paced4x-lag60k
//     写方 4 倍实时速率（192 kHz 帧速率）写 1..8192 的块；读方滞后 56000..65536，其中 1/4 的读在取到 W 之后
//     停最多 40 ms，写方这段时间里跑 7680 帧，窗口左端落进被覆盖的区域
//   paced4x-edge
//     同样的写方，读方贴着覆盖边缘读，停顿最多 5 ms
//   hostblock-32768
//     全速的写方按宿主块写，块长 1..32768（对数均匀，余数各异），经 RingWriter 按 8192 切段；读方贴边缘读。
//     另核：W 恰好前进了写下的全部帧数（切段之后读方看不出分段），切过段的块数与最大块长够了
//   hostblock-paced-4x
//     宿主块写方配 4 倍速率，读方 60k 滞后加停顿
// 变异构建里不跑：那里的取证规则只认上面那个用例的命中数（StressMutation.h），多出来的组只会拖长时间。
TEST_CASE("the transfer ring holds under a 4x writer, split host blocks and a 60k lag", "[stress][ring][rates]")
{
    if constexpr (kMutated)
    {
        SKIP("变异构建只跑环的主用例，取证规则见 StressMutation.h");
    }
    constexpr u64 k4xRate = 4ull * 48000ull;
    struct Scenario
    {
        Config cfg;
        Extras extra;
        u64 minSplitBlocks; // 切过段的宿主块至少这么多
        u64 minMaxBlock; // 最大的宿主块至少这么长（32768 的宿主块是恰好 4 段）
    };
    // Config：name, logUniform, chunkLo, chunkHi, randomLag, edgeSpan, edgeMaxM, burst, pauseSpins, writers
    // Extras 按字段名赋值（C++17 没有指派初始化），先造默认值再改
    const auto make = [](Config cfg, auto&& tweak, u64 splits, u64 maxBlock) {
        Extras extra;
        tweak(extra);
        return Scenario{cfg, extra, splits, maxBlock};
    };
    const Scenario scenarios[] = {
        make(
            {"paced4x-lag60k", true, 1, 8192, true, 0, 0, 0, 0, 1},
            [&](Extras& x) {
                x.paceFramesPerSec = k4xRate;
                x.lagLo = 56000;
                x.lagHi = kRingFrames;
                x.stallMaxUs = 40000;
                x.roundMs = 1500;
                x.capMs = 6000;
                x.minChunks = 150;
                x.minReads = 50;
                x.minRaced = 5;
                x.minStalledRaced = 2;
            },
            0, 0),
        make(
            {"paced4x-edge", true, 1, 8192, false, 4096, 64, 0, 0, 1},
            [&](Extras& x) {
                x.paceFramesPerSec = k4xRate;
                x.stallMaxUs = 5000;
                x.roundMs = 1500;
                x.capMs = 6000;
                x.minChunks = 150;
                x.minReads = 300;
                x.minAccepted = 5000;
                x.minRaced = 10;
                x.minStalledRaced = 2;
            },
            0, 0),
        make(
            {"hostblock-32768", true, 1, 8192, false, 4096, 64, 0, 0, 1},
            [&](Extras& x) {
                x.hostBlockMax = kMaxSupportedBlock;
                x.minChunks = 300;
                x.minRaced = 20;
            },
            20, 3ull * kMaxWriteChunk + 1),
        make(
            {"hostblock-paced-4x", true, 1, 8192, true, 0, 0, 0, 0, 1},
            [&](Extras& x) {
                x.hostBlockMax = kMaxSupportedBlock;
                x.paceFramesPerSec = k4xRate;
                x.lagLo = 56000;
                x.lagHi = kRingFrames;
                x.stallMaxUs = 40000;
                x.roundMs = 2000;
                x.capMs = 8000;
                x.minChunks = 100;
                x.minReads = 60;
                x.minRaced = 5;
                x.minStalledRaced = 2;
            },
            5, 0),
    };

    u64 seed = 0x4a7e5eedu;
    for (const Scenario& sc : scenarios)
    {
        INFO("scenario " << sc.cfg.name);
        const Result r = runRound(sc.cfg, seed, sc.extra);
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;

        CHECK(r.badNewer == 0);
        CHECK(r.badTorn == 0);
        CHECK(r.badOlder == 0);
        CHECK(r.badOther == 0);
        // 单写方：从不被挡、从不失败，写头恰好是写下的全部帧数
        CHECK(r.writeFails == 0);
        CHECK(r.inFlight == 0);
        CHECK(r.head == r.frames);
        const Extras& x = sc.extra;
        if (x.paceFramesPerSec != 0)
        {
            // 配速真的在起作用：写下的帧数不超过「速率 × 时长」加一点余量（写方落后时追赶，所以平均速率贴着标称值，
            // 不会超过它），也不低于它的四分之一（再慢就是写方被饿死，不是 4 倍速率的写方了）
            const u64 nominal = x.paceFramesPerSec * r.millis / 1000;
            CHECK(r.frames <= nominal + nominal / 4 + 2ull * kMaxSupportedBlock);
            CHECK(r.frames >= nominal / 4);
        }
        // 非空转下限
        CHECK(r.chunks >= (x.minChunks != 0 ? x.minChunks : kMinChunks));
        CHECK(r.reads >= (x.minReads != 0 ? x.minReads : kMinReads));
        CHECK(r.acceptedFrames >= (x.minAccepted != 0 ? x.minAccepted : kMinAccepted));
        CHECK(r.raced >= (x.minRaced != 0 ? x.minRaced : minRaced(sc.cfg)));
        CHECK(r.stalledRaced >= x.minStalledRaced);
        if (x.lagHi != 0)
        {
            // 随机滞后真的落在配置的范围里，并且摸到了接近一整圈的滞后（约 60k 帧）
            CHECK(r.lagMin >= x.lagLo);
            CHECK(r.lagMax >= 60000);
        }
        if (x.hostBlockMax != 0)
        {
            CHECK(r.splitBlocks >= sc.minSplitBlocks);
            CHECK(r.maxHostBlock >= sc.minMaxBlock);
        }

        std::printf("[stress][rates] %-19s chunks=%llu frames=%llu head=%llu reads=%llu accepted=%llu rejected=%llu "
                    "badNewer=%llu badTorn=%llu badOlder=%llu badOther=%llu raced=%llu stalledRaced=%llu "
                    "hostBlocks=%llu split=%llu "
                    "maxBlock=%llu ms=%llu\n",
                    sc.cfg.name, static_cast<unsigned long long>(r.chunks), static_cast<unsigned long long>(r.frames),
                    static_cast<unsigned long long>(r.head), static_cast<unsigned long long>(r.reads),
                    static_cast<unsigned long long>(r.acceptedFrames),
                    static_cast<unsigned long long>(r.rejectedFrames), static_cast<unsigned long long>(r.badNewer),
                    static_cast<unsigned long long>(r.badTorn), static_cast<unsigned long long>(r.badOlder),
                    static_cast<unsigned long long>(r.badOther), static_cast<unsigned long long>(r.raced),
                    static_cast<unsigned long long>(r.stalledRaced), static_cast<unsigned long long>(r.hostBlocks),
                    static_cast<unsigned long long>(r.splitBlocks), static_cast<unsigned long long>(r.maxHostBlock),
                    static_cast<unsigned long long>(r.millis));
        std::fflush(stdout);
    }
}
