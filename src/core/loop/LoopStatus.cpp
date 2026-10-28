// SPDX-License-Identifier: GPL-3.0-or-later
//
// 环路层状态的原子量容器：音频线程写、消息线程读。
//
// 这里是「写」的那一半（LoopStatus.h 里的 setter）与「读」的那一半（snapshot）。
// 全部用 relaxed：这些量是诊断与显示用的快照，不是同步原语。唯一需要保证的是「一个字段不会读到
// 半个值」——每个字段本身就是一个原子量，读到的是它的某一个值，这就够了。跨字段的组合
// （例如 resyncCount 与 resyncByReason）不保证是同一瞬间的快照：那是显示，本来就允许前后差一个事件。
// 例外是几何量 m / w / off：探针要拿它们验 m = w − R − off，三个量必须出自同一块，经顺序锁一起发布。
//
// 实时约定：写侧的每个函数都只做一次或几次整数存储，不加锁、不分配、不读时钟、不抛异常。
#include "loop/LoopStatus.h"

#include <algorithm>
#include <cmath>

namespace rinne::loop
{

namespace
{
// 重同步原因的下标上限：SyncReason 的取值是 0..8（kReasonCount = 9），越界的值被忽略。
constexpr std::uint32_t kReasonCount = 9;
} // namespace

// 状态对象由音频线程写、消息线程读：原子量必须 lock-free，否则音频线程上会挂锁。
static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "状态字段必须是 lock-free 的原子量");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "状态字段必须是 lock-free 的原子量");
static_assert(std::atomic<std::int64_t>::is_always_lock_free, "状态字段必须是 lock-free 的原子量");
static_assert(std::atomic<std::uint8_t>::is_always_lock_free, "状态字段必须是 lock-free 的原子量");
static_assert(std::atomic<bool>::is_always_lock_free, "状态字段必须是 lock-free 的原子量");

void LoopStatus::setDelay(float userMs, std::int64_t effective, std::int64_t min, std::uint8_t mode) noexcept
{
    // 毫秒存百分之一：浮点原子量不保证 lock-free，也不保证逐位一致
    const auto centi = static_cast<std::uint32_t>(std::lround(static_cast<double>(userMs) * 100.0));
    delayUserCenti_.store(centi, std::memory_order_relaxed);
    delayEffective_.store(effective, std::memory_order_relaxed);
    delayMin_.store(min, std::memory_order_relaxed);
    delayMode_.store(mode, std::memory_order_relaxed);
}

void LoopStatus::setDelayFloor(std::uint8_t reason, std::int64_t maxBlockSeen) noexcept
{
    delayMinReason_.store(reason, std::memory_order_relaxed);
    maxBlockSeen_.store(maxBlockSeen, std::memory_order_relaxed);
}

void LoopStatus::setBpm(std::uint32_t centi, std::uint8_t source) noexcept
{
    bpmCenti_.store(centi, std::memory_order_relaxed);
    bpmSource_.store(source, std::memory_order_relaxed);
}

void LoopStatus::setDelayTarget(std::int64_t target, std::uint32_t hints, std::int64_t xfadeLeft) noexcept
{
    delayTarget_.store(target, std::memory_order_relaxed);
    delayHints_.store(hints, std::memory_order_relaxed);
    delayXfadeLeft_.store(xfadeLeft, std::memory_order_relaxed);
}

void LoopStatus::setTaps(std::uint32_t active, std::uint32_t dropped) noexcept
{
    tapsActive_.store(active, std::memory_order_relaxed);
    tapDrop_.store(dropped, std::memory_order_relaxed);
}

void LoopStatus::addResync(std::uint8_t reason) noexcept
{
    if (reason >= kReasonCount)
    {
        return; // 越界的值被忽略，不去踩数组外面
    }
    resyncCount_.fetch_add(1, std::memory_order_relaxed);
    resyncByReason_[reason].fetch_add(1, std::memory_order_relaxed);
}

void LoopStatus::addOverflow(std::uint64_t frames) noexcept
{
    overflow_.fetch_add(frames, std::memory_order_relaxed);
}

void LoopStatus::addTorn(std::uint64_t frames) noexcept
{
    torn_.fetch_add(frames, std::memory_order_relaxed);
}

void LoopStatus::addOverrun(std::uint64_t samples) noexcept
{
    overrun_.fetch_add(samples, std::memory_order_relaxed);
}

// 几何量的顺序锁（只有音频线程写）：序号先加到奇数，release fence 之后写三个量，最后以 release 把序号加到偶数。
// 读方见到偶数序号、读完三个量、acquire fence 之后再读到同一个序号，三个量就出自同一次写。
void LoopStatus::setGeometry(std::int64_t driftValue, std::int64_t writeHead, std::int64_t offset) noexcept
{
    const std::uint32_t seq = geometrySeq_.load(std::memory_order_relaxed);
    geometrySeq_.store(seq + 1u, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    drift_.store(driftValue, std::memory_order_relaxed);
    writeHead_.store(writeHead, std::memory_order_relaxed);
    offset_.store(offset, std::memory_order_relaxed);
    geometrySeq_.store(seq + 2u, std::memory_order_release);
}

// 读几何量：最多重试 kGeometryReadAttempts 次。音频线程每块只写一次、一次只有三次存储，正常情况下第一次就读得到；
// 一直撞上（音频线程恰好挂在两次存储之间）时不在消息线程上无限自旋，给出最后读到的值并标成不一致。
void LoopStatus::readGeometry(LoopStatusFields& fields) const noexcept
{
    constexpr int kGeometryReadAttempts = 64;
    for (int attempt = 0; attempt < kGeometryReadAttempts; ++attempt)
    {
        const std::uint32_t before = geometrySeq_.load(std::memory_order_acquire);
        fields.drift = drift_.load(std::memory_order_relaxed);
        fields.writeHead = writeHead_.load(std::memory_order_relaxed);
        fields.offset = offset_.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        const std::uint32_t after = geometrySeq_.load(std::memory_order_relaxed);
        if ((before & 1u) == 0u && before == after)
        {
            fields.geometryConsistent = 1;
            return;
        }
    }
    fields.geometryConsistent = 0;
}

void LoopStatus::setOrder(const OrderSnapshot& snapshot) noexcept
{
    orderPacked_.store(snapshot.pack(), std::memory_order_relaxed);
    orderGlitches_.store(snapshot.glitches, std::memory_order_relaxed);
}

void LoopStatus::noteExport(std::uint32_t causes) noexcept
{
    if (causes == 0)
    {
        return;
    }
    exportCauses_.store(causes, std::memory_order_relaxed);
    exportEvents_.fetch_add(1, std::memory_order_relaxed);
}

LoopStatusFields LoopStatus::snapshot() const noexcept
{
    LoopStatusFields fields;
    fields.delayUserMs = static_cast<float>(delayUserCenti_.load(std::memory_order_relaxed)) * 0.01f;
    fields.delayEffective = delayEffective_.load(std::memory_order_relaxed);
    fields.delayMin = delayMin_.load(std::memory_order_relaxed);
    fields.delayMinReason = delayMinReason_.load(std::memory_order_relaxed);
    fields.maxBlockSeen = maxBlockSeen_.load(std::memory_order_relaxed);
    fields.delayMode = delayMode_.load(std::memory_order_relaxed);
    fields.bpmCenti = bpmCenti_.load(std::memory_order_relaxed);
    fields.bpmSource = bpmSource_.load(std::memory_order_relaxed);
    fields.delayTarget = delayTarget_.load(std::memory_order_relaxed);
    fields.delayHints = delayHints_.load(std::memory_order_relaxed);
    fields.delayXfadeLeft = delayXfadeLeft_.load(std::memory_order_relaxed);

    fields.tapsActive = tapsActive_.load(std::memory_order_relaxed);
    fields.tapDrop = tapDrop_.load(std::memory_order_relaxed);

    fields.resyncCount = resyncCount_.load(std::memory_order_relaxed);
    for (std::uint32_t i = 0; i < kReasonCount; ++i)
    {
        fields.resyncByReason[i] = resyncByReason_[i].load(std::memory_order_relaxed);
    }
    fields.overflow = overflow_.load(std::memory_order_relaxed);
    fields.torn = torn_.load(std::memory_order_relaxed);
    fields.overrun = overrun_.load(std::memory_order_relaxed);
    fields.wCasFail = wCasFail_.load(std::memory_order_relaxed);
    fields.unstable = unstable_.load(std::memory_order_relaxed) ? 1u : 0u;
    fields.offsetDriftBlocks = offsetDrift_.load(std::memory_order_relaxed);
    fields.frozenMax = frozenMax_.load(std::memory_order_relaxed);
    fields.stalled = stalled_.load(std::memory_order_relaxed) ? 1u : 0u;
    fields.tolerant = tolerant_.load(std::memory_order_relaxed) ? 1u : 0u;
    fields.paired = paired_.load(std::memory_order_relaxed) ? 1u : 0u;

    readGeometry(fields);

    const OrderSnapshot order = OrderSnapshot::unpack(orderPacked_.load(std::memory_order_relaxed),
                                                      orderGlitches_.load(std::memory_order_relaxed));
    fields.orderVerdict = static_cast<std::uint8_t>(order.verdict);
    fields.orderDirection = static_cast<std::uint8_t>(order.direction);
    fields.orderVotes = order.votes;
    fields.hostNsValid = order.hostNsValid ? 1u : 0u;
    fields.orderChecks = order.checks;
    fields.orderGlitches = order.glitches;

    // 原因位与计数各是一个原子量，不保证出自同一次事件（显示允许前后差一个事件）。
    fields.exportCauses = exportCauses_.load(std::memory_order_relaxed);
    fields.exportEvents = exportEvents_.load(std::memory_order_relaxed);
    return fields;
}

} // namespace rinne::loop
