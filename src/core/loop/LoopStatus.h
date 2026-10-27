// SPDX-License-Identifier: GPL-3.0-or-later
//
// 环路层的状态：音频线程写，消息线程读。每个字段一个无锁原子量，音频线程只写、消息线程只读。
// 字段之间一般不保证出自同一块（显示允许前后差一个事件）；几何量 m / w / off 例外，三个一起经顺序锁发布。
//
// 存在的理由：诊断文本的环路段与顺序段（DiagLoop / DiagSync / DiagOrder）与界面的常驻显示都要这些量，
// 这个结构是它们唯一的落点：之后往里加字段只改这一个头与它的实现，引擎的代码不用改。
//
// 分两层：
//   LoopStatus        原子量容器，音频线程拥有并写。
//   LoopStatusFields  纯值的快照，消息线程读的就是它（可以整份拷贝、可以按字段显示）。
//
// 命名的口径：
//   m   漂移量 = W − (R + off)。正数 = 写头领先，负数 = 读位跑到写头前面。
//   W   写头（Output 已写总帧数）。R 读位（Input 本地已处理帧数）。
//   off 读头的同步偏移。d 实际环路延迟 D（样本）。rs 重同步次数。
//
// 这五个量（m / w / off / rs / d）逐块发布当前值：诊断与探针都要看「现在是多少」，
// 只有一个累计计数是不够的。d 与 rs 本来就在下面（delayEffective 与 resyncCount），
// 这里补的是 m / w / off 三个。
//
// **不在这个结构里的量**：安全段的六个（state / trips / tail_peak_max_db / limiter_gr_max_db /
// nonfinite_ring / clamp_ring）不是环路层的量，由引擎层的 engine::SafetyDiag 汇总
// （InputEngine::safetyDiag()：前四个来自安全链的测量量，后两个来自环头的清洗计数），不要把它们塞进这里。
//
// d_eff 的发布归属：会合段端点槽里的 d_eff 写在 **Input 槽**（RoleSlot::d_eff），由 InputEngine 每块发布；
// 环路层本身不碰会合段的写侧（只读对端槽）。
#pragma once

#include <atomic>
#include <cstdint>

#include "loop/LoopEvents.h"

namespace rinne::loop
{

// 漂移分类的判定结果（错放证据）。
enum class OrderVerdict : std::uint8_t
{
    kUnknown = 0, // 宿主没给时间戳，无法判定
    kCorrect = 1, // 顺序正确
    kOutputFirst = 2, // Output 在 Input 之前（琥珀提示）
};

// 速度的来源。
enum class BpmSource : std::uint8_t
{
    kNone = 0, // 从没拿到过宿主速度
    kHost = 1, // 宿主给的
    kFallback = 2, // 回退到 120
};

// D 的提示位（LoopStatusFields::delayHints）：界面据此给出提示。只在状态变化时发 kDelayHints 事件。
inline constexpr std::uint32_t kDelayHintNoTempo = 1u << 0; // 节拍模式下本会话还没拿到过有效的宿主速度，在用 120 BPM
inline constexpr std::uint32_t kDelayHintClipped = 1u << 1; // 用户要的 D 超过 4000 ms，被截到了上限

// 纯值快照。消息线程拿到的就是这一份。
struct LoopStatusFields
{
    // ---- 环路（D）----
    float delayUserMs = 0.0f; // 用户要的 D（毫秒）
    std::int64_t delayEffective = 0; // 实际用的 D（样本）：主读头正在读的 D；改 D 的过渡没走完时仍是旧的 D
    std::int64_t delayMin = 0; // 抬升后的下限（样本）
    std::uint8_t delayMinReason = 0; // 下限从哪来：loop::DelayMinReason（声明值 / 实测块长 / 宿主违约）
    std::int64_t maxBlockSeen = 0; // 本会话见过的最大块长（N_obs，每次 prepare 清零）
    std::uint8_t delayMode = 0; // 0 = 时间，1 = 节拍
    std::uint32_t bpmCenti = 0; // 速度（百分之一拍/分钟）
    std::uint8_t bpmSource = 0; // BpmSource
    std::int64_t delayTarget = 0; // 目标 D（样本）：交叉淡化走完之后读头会停在它
    std::uint32_t delayHints = 0; // kDelayHint* 的位或
    std::int64_t delayXfadeLeft = 0; // 改 D 的交叉淡化还要走多少个样本（0 = 没有在淡）

    // ---- 读头 ----
    std::uint32_t tapsActive = 0; // 当前活跃的读头数
    std::uint32_t tapDrop = 0; // 累计丢掉的读头数

    // ---- 同步 ----
    std::uint32_t resyncCount = 0; // 累计重同步次数
    std::uint32_t resyncByReason[9]{}; // 按原因分：索引是 loop::SyncReason（0..8）
    std::uint64_t overflow = 0; // 被环覆盖而丢掉的帧数
    std::uint64_t torn = 0; // 撕裂的帧数
    std::uint64_t overrun = 0; // 补 0 的样本数
    std::uint32_t wCasFail = 0; // 写头 CAS 失败次数（写侧统计，读侧从环头读）
    std::uint32_t unstable = 0; // 时序不稳定：0 或 1。进过一次容忍模式就是 1，之后不再清零（不是次数）
    std::uint32_t offsetDriftBlocks = 0; // 漂移被吸收的检查次数（m 不为 0、还没到重同步门槛）
    std::uint32_t frozenMax = 0; // 连续最多几块写头没动（块数）
    std::uint32_t stalled = 0; // 当前是否断档
    std::uint32_t tolerant = 0; // 当前是否容忍模式
    std::uint32_t paired = 0; // 当前是否有对端

    // ---- 当前的几何量（逐块刷新；探针与诊断要看「现在是多少」）----
    // 三个量出自同一块：音频线程按顺序锁（seqlock）一起发布，快照读到的三元组满足 m = w − R − off（R
    // 是那一块的块首读位）。
    std::int64_t drift = 0; // m = W − (R + off)。正数 = 写头领先
    std::int64_t writeHead = 0; // w = Output 已写总帧数
    std::int64_t offset = 0; // off = 读头的同步偏移
    // 1 = 上面三个量出自同一块；0 = 读的时候一直撞上音频线程正在写（重试够了次数仍没读到一份完整的），三个量不可信。
    std::uint32_t geometryConsistent = 1;

    // ---- 顺序 ----
    std::uint8_t orderVerdict = 0; // OrderVerdict
    std::uint32_t orderVotes = 0; // 判定的票数（连续 3 次即判定）
    std::uint32_t hostNsValid = 0; // 宿主时间戳是否有效
};

// 原子量容器。音频线程写，消息线程读。
class LoopStatus
{
public:
    // ---- 音频线程写 ----

    void setPaired(bool on) noexcept { paired_.store(on ? 1u : 0u, std::memory_order_relaxed); }
    void setStalled(bool on) noexcept { stalled_.store(on ? 1u : 0u, std::memory_order_relaxed); }
    void setTolerant(bool on) noexcept { tolerant_.store(on ? 1u : 0u, std::memory_order_relaxed); }
    void setUnstable(bool on) noexcept { unstable_.store(on ? 1u : 0u, std::memory_order_relaxed); }
    void setDelay(float userMs, std::int64_t effective, std::int64_t min, std::uint8_t mode) noexcept;
    // 界面常驻的「实际 / 最小 / 原因 / 本会话最大块长」里的后两项。
    void setDelayFloor(std::uint8_t reason, std::int64_t maxBlockSeen) noexcept;
    void setBpm(std::uint32_t centi, std::uint8_t source) noexcept;
    // 目标 D、提示位与交叉淡化的剩余样本数
    void setDelayTarget(std::int64_t target, std::uint32_t hints, std::int64_t xfadeLeft) noexcept;
    void setTaps(std::uint32_t active, std::uint32_t dropped) noexcept;
    // reason 是 loop::SyncReason 的值（0..8）；越界的值被忽略
    void addResync(std::uint8_t reason) noexcept;
    void addOverflow(std::uint64_t frames) noexcept;
    void addTorn(std::uint64_t frames) noexcept;
    void addOverrun(std::uint64_t samples) noexcept;
    void setWCasFail(std::uint32_t count) noexcept { wCasFail_.store(count, std::memory_order_relaxed); }
    // 漂移被吸收的块数（累计值，由判据那一半给）。漂移被吸收是真实事件，计数要看得见。
    void setOffsetDriftBlocks(std::uint32_t blocks) noexcept { offsetDrift_.store(blocks, std::memory_order_relaxed); }
    void setFrozenMax(std::uint32_t blocks) noexcept { frozenMax_.store(blocks, std::memory_order_relaxed); }
    // 当前的几何量：m（漂移量）、w（写头）、off（同步偏移）。每块写一次，三个量按顺序锁一起发布（只有音频线程写）。
    void setGeometry(std::int64_t driftValue, std::int64_t writeHead, std::int64_t offset) noexcept;
    void setOrder(std::uint8_t verdict, std::uint32_t votes, bool hostNsValid) noexcept;

    // ---- 消息线程读 ----

    LoopStatusFields snapshot() const noexcept;

private:
    // 按顺序锁读几何量（m / w / off 出自同一块）
    void readGeometry(LoopStatusFields& fields) const noexcept;

    std::atomic<bool> paired_{false};
    std::atomic<bool> stalled_{false};
    std::atomic<bool> tolerant_{false};
    std::atomic<bool> unstable_{false};

    std::atomic<std::uint32_t> delayUserCenti_{0}; // 毫秒的百分之一
    std::atomic<std::int64_t> delayEffective_{0};
    std::atomic<std::int64_t> delayMin_{0};
    std::atomic<std::uint8_t> delayMinReason_{0};
    std::atomic<std::int64_t> maxBlockSeen_{0};
    std::atomic<std::uint8_t> delayMode_{0};

    std::atomic<std::uint32_t> bpmCenti_{0};
    std::atomic<std::uint8_t> bpmSource_{0};
    std::atomic<std::int64_t> delayTarget_{0};
    std::atomic<std::uint32_t> delayHints_{0};
    std::atomic<std::int64_t> delayXfadeLeft_{0};

    std::atomic<std::uint32_t> tapsActive_{0};
    std::atomic<std::uint32_t> tapDrop_{0};

    std::atomic<std::uint32_t> resyncCount_{0};
    std::atomic<std::uint32_t> resyncByReason_[9]{};

    std::atomic<std::uint64_t> overflow_{0};
    std::atomic<std::uint64_t> torn_{0};
    std::atomic<std::uint64_t> overrun_{0};
    std::atomic<std::uint32_t> wCasFail_{0};
    std::atomic<std::uint32_t> offsetDrift_{0};
    std::atomic<std::uint32_t> frozenMax_{0};

    // 几何量的顺序锁：音频线程写之前把序号加到奇数、写完加到偶数；消息线程前后两次读到同一个偶数才算一份完整的。
    std::atomic<std::uint32_t> geometrySeq_{0};
    std::atomic<std::int64_t> drift_{0};
    std::atomic<std::int64_t> writeHead_{0};
    std::atomic<std::int64_t> offset_{0};

    std::atomic<std::uint8_t> orderVerdict_{0};
    std::atomic<std::uint32_t> orderVotes_{0};
    std::atomic<bool> hostNsValid_{false};
};

} // namespace rinne::loop
