// SPDX-License-Identifier: GPL-3.0-or-later
//
// 数据段的传输环（g{G} 的 65536 帧立体声环）。Output 是唯一的写方（属主），按绝对帧位置写块；Input 按绝对位置读。
// W = write_head 是段寿命内单调递增的 u64 帧位置，帧 p 的槽是 p & (kRingFrames - 1)，样本是左右交错的
// std::atomic<float>，一律 relaxed 访问。整个协议只靠 fence 配对，不靠「写头至少领先一个块长」这类安全余量：
//
//   写方：把 write_intent 推到 W + k（relaxed 的 CAS-max）→ release fence → 逐帧写样本（relaxed）→ W 从旧值
//         CAS 到 W + k（release）。intent 先于样本、W 在样本之后；单写方时 intent <= W 恒成立，那一步 CAS 一次就成功。
//   读方：先 acquire 读 W（调用方保证 to 不超过它），再逐帧 relaxed 读样本，然后 acquire fence，最后 relaxed 读
//         intent，把 p < intent - kRingFrames 的帧全部丢弃。
//
// 为什么对：写方的 release CAS 与读方的 acquire 读 W 配对，单写方程序序里先写的每一帧 < W 都对读方可见，所以读到的
// 要么是自己的值、要么是之后某一圈的值。若拷贝里某帧的样本是之后的块 C 写的（落在 C 的 release fence 之后），读方的
// acquire fence 就与那道 release fence 配对，之后读 intent 一定看得见 intent >= C 的目标；C 覆盖过的帧位置都
// < C.target - kRingFrames <= intent - kRingFrames，正好被 validFrom 排除。于是「读到新一圈」只可能发生在被排除的
// 帧上，其余帧都还是自己的值：只靠 fence 配对，不需要任何余量。
//
// 变异构建：RINNE_STRESS_MUTATE_NO_READ_FENCE 去掉读方的 acquire fence，RINNE_STRESS_MUTATE_NO_INTENT 去掉写方的
// intent 公告；两个宏只在压测的变异构建里定义（CMake 只允许它们与 RINNE_CORE_ONLY 同用），发布构建永不定义。
// 变异构建的压测必须变红，用来证明压测判据抓得住这两类缺陷。x86 是 TSO：两道 fence 在那里都只是编译器屏障，
// no_read_fence 在 x86 上仍然是绿的，只有原生 arm64 才能把它变红；no_intent 在任何 CPU 上都会红，因为读方根本
// 没机会知道窗口被覆盖过。内联模板放在头文件里，调用方（尤其是压测）内联之后变异宏才在调用点生效。
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>

#include "ipc/SegmentLayout.h"

namespace rinne::ipc
{

using i64 = std::int64_t;

// 两个 u64 帧位置的有符号差：直接相减再转，回绕（W 越过 2^64）时仍然给出正确的差
constexpr i64 frameDiff(u64 a, u64 b) noexcept
{
    return static_cast<i64>(a - b);
}

// 写一块的结果。kOk = 已发布；kCasLost = 写头被别人推过（接管瞬间的双写），这一块丢弃，它已经写下的样本由
// write_intent 覆盖，读方会把它们当作被覆盖的帧排除；kBadLength = 块长为 0 或超过 kMaxWriteChunk，什么都没碰
enum class RingWriteResult : std::uint8_t
{
    kOk,
    kCasLost,
    kBadLength,
};

// 一次 pull 的结果：拷贝之后写方的公告，以及「从这一帧起才可能是完整数据」的下界。调用方必须把 p < validFrom 的帧
// 当作 0：它们的槽可能已经被写方的下一块覆盖
struct RingPull
{
    i64 intent = 0;
    i64 validFrom = 0;
};

// 写方的格式发布：epoch 只在采样率改变（或第一次发布）时 + 1；writer_token 是写权
struct RingFormat
{
    u64 epoch = 0;
    u64 writerToken = 0;
    u32 sampleRate = 0;
};

// 一次 read() 的结果：写头的快照与输出里有效帧的区间（validTo 已经按写头截断）
struct RingRead
{
    u64 writeHead = 0;
    i64 validFrom = 0;
    i64 validTo = 0;
};

class LoopRing
{
public:
    // 段头侧的几何：initGeometry 用作 initHeader 的 initData（认领之后、发布 magic 之前），所以写 relaxed
    static void initGeometry(RingSegment& seg) noexcept;
    static bool geometryOk(const RingSegment& seg) noexcept;

    // 写方在消息线程（或 prepare）发布自己：token 为 0 时什么都不做。采样率变了或从没发布过才写 sample_rate、
    // src_channels 并以 release 抬 epoch；否则只改 src_channels。最后以 release 写 writer_token。
    // 从不碰 write_head 与 write_intent：换写方时环里已有的数据不清零，读方靠 validFrom 自己排除
    static void publishWriter(RingSegment& seg, u64 writerToken, u32 sampleRate, u32 srcChannels) noexcept;
    // 只交出自己那一份写权；不是自己的返回 false。写权为 0 时没有属主，返回 false
    static bool retireWriter(RingSegment& seg, u64 writerToken) noexcept;
    // 先 acquire 读 epoch、writer_token，再 relaxed 读 sample_rate：写方的 release 保证采样率先于 epoch 可见
    static RingFormat readFormat(const RingSegment& seg) noexcept;
    static bool isWriter(const RingSegment& seg, u64 token) noexcept;
    static u64 writeHead(const RingSegment& seg) noexcept;
    // 诊断与测试用：写方在动样本之前公告的目标
    static u64 writeIntent(const RingSegment& seg) noexcept;

    // 写一块，帧数在 [1, kMaxWriteChunk]；更长的块由调用方切开。source(u32 i, float& l, float& r) 填第 i 帧。
    // 顺序不能改：见文件头。写头 CAS 失败只记一次 w_cas_fail 并丢弃这一块
    template<typename Source>
    static RingWriteResult write(RingSegment& seg, u32 frames, Source&& source) noexcept;

    // 读 [from, to)，逐帧交给 sink(p, l, r)。调用方保证 to 不超过自己之前用 writeHead()（acquire）读到的 W。
    // 比 to 早一圈的帧与负位置一定已经不存在，不读；读完之后用 acquire fence 与写方的 release fence 配对，
    // 再读 intent 定出 validFrom。sink 收到的 p < validFrom 的帧必须被丢弃
    template<typename Sink>
    static RingPull pull(const RingSegment& seg, i64 from, i64 to, Sink&& sink) noexcept;

    // 便利封装（测试与对端工具）：写头之后读 [from, from + frames) ∩ (-∞, W)，把每帧拷到 left/right[p - from]，
    // 并把 p < validFrom 或 p >= to 的输出帧清零。frames <= kRingFrames，left/right 非空
    static RingRead read(const RingSegment& seg, i64 from, u32 frames, float* left, float* right) noexcept;
};

template<typename Source>
RingWriteResult LoopRing::write(RingSegment& seg, u32 frames, Source&& source) noexcept
{
    if (frames == 0 || frames > kMaxWriteChunk)
    {
        return RingWriteResult::kBadLength;
    }
    u64 w0 = seg.header.write_head.load(std::memory_order_acquire);
    const u64 target = w0 + frames;
#if !defined(RINNE_STRESS_MUTATE_NO_INTENT)
    // intent 用 CAS-max 而不是普通 store：接管瞬间新旧两个写方可能同时在这里推进，普通 store 会让 intent 倒退，
    // 把「已经有一方覆盖过的帧」重新说成完整数据。单写方时 intent <= w0 恒成立，第一次 CAS 就成功，代价与 store 相同
    u64 intent = seg.header.write_intent.load(std::memory_order_relaxed);
    while (frameDiff(target, intent) > 0 && !seg.header.write_intent.compare_exchange_weak(
                                                intent, target, std::memory_order_relaxed, std::memory_order_relaxed))
    {
    }
#endif
    // 公告先于样本：读方在样本之后读 intent，只要它看见这一块写下的任何一帧，就必须看见至少 target
    std::atomic_thread_fence(std::memory_order_release);
    const u64 mask = static_cast<u64>(kRingFrames - 1);
    for (u32 i = 0; i < frames; ++i)
    {
        float l = 0.0f;
        float r = 0.0f;
        source(i, l, r);
        const u64 slot = (w0 + i) & mask;
        seg.data[2 * slot].store(l, std::memory_order_relaxed);
        seg.data[2 * slot + 1].store(r, std::memory_order_relaxed);
    }
    // 样本全部写完才推 W；输给另一个写方就丢弃这一块，它已经覆盖的帧由 intent 兜住
    if (!seg.header.write_head.compare_exchange_strong(w0, target, std::memory_order_release,
                                                       std::memory_order_relaxed))
    {
        seg.header.w_cas_fail.fetch_add(1, std::memory_order_relaxed);
        return RingWriteResult::kCasLost;
    }
    return RingWriteResult::kOk;
}

template<typename Sink>
RingPull LoopRing::pull(const RingSegment& seg, i64 from, i64 to, Sink&& sink) noexcept
{
    const i64 lap = static_cast<i64>(kRingFrames);
    // 比 to 早一圈的帧一定已经被覆盖（环只有一圈），负位置从没存在过：都不读，交给 validFrom 统一排除
    const i64 lo = std::max(std::max(from, to - lap), static_cast<i64>(0));
    const u64 mask = static_cast<u64>(kRingFrames - 1);
    for (i64 p = lo; p < to; ++p)
    {
        const u64 slot = static_cast<u64>(p) & mask;
        const float l = seg.data[2 * slot].load(std::memory_order_relaxed);
        const float r = seg.data[2 * slot + 1].load(std::memory_order_relaxed);
        sink(p, l, r);
    }
#if !defined(RINNE_STRESS_MUTATE_NO_READ_FENCE)
    // 与写方的 release fence 配对：拷贝里只要有一帧是之后的块写的，下面就一定看得见它的 intent
    std::atomic_thread_fence(std::memory_order_acquire);
#endif
    const i64 intent = static_cast<i64>(seg.header.write_intent.load(std::memory_order_relaxed));
    return {intent, std::max(lo, intent - lap)};
}

} // namespace rinne::ipc
