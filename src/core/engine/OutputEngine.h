// SPDX-License-Identifier: GPL-3.0-or-later
//
// OutputEngine：Rinne Output 的 core 侧引擎。宿主壳只做映射（参数 → EngineParams，宿主播放头 → TransportInfo），
// 然后调这里。引擎负责：清洗之后把宿主信号写进环、写头照常推进、发布状态。
//
// 三条硬规则：
//   1. 宿主的 buffer 逐位直通（不改动一个字节）。Output 只旁路地读它。
//   2. 写环只经 RingWriter（src/core/ipc/LoopRing.h）。直接调 LoopRing::write 的写方碰上写到一半死掉的
//      上一任，会永远拿到 kInFlight，这一组从此静音。RingWriter 负责卡住时的回收。
//   3. 每块先查 isWriter：写权不是自己的就不写（置冲突标志）。换写方（token 变了）、换段（映射代号变了）
//      或采样率改变时 RingWriter::reset，上一个任期留下的「被挡住的帧数」不带进新的任期。
//
// 写环按 ≤ 8192 帧分段（kMaxWriteChunk）：**由引擎按 8192 帧切段**，每段各自公告与发布
// （W 连续前进 n 帧，读方看不出分段）。宿主给 32768 帧 → 4 段 8192。
// 某一段被 kInFlight 挡住时，后面的段照样交给 RingWriter：它按「被挡住的帧数」计回收门槛，一块里每一段都算，
// 门槛按实际经过的音频帧数走满 1 s（宿主块长 32768 时也是约 1 s，不会因为只数了第一段而拖到约 4 s）。
// 回收在某一段里发生之后，本块余下的段照常写进去。kCasLost（这一段被回收推过了）时本块到此为止。
// 块长超过 32768（kMaxSupportedBlock）时不写环、只出干声，并给红字提示码 kUnsupportedBlock。
//
// **回收为什么留在音频线程、按帧计 1 s**（不挪到消息线程，也不改按墙钟计）：
//   - 卡住只有现任写方在写环时才看得见（拿到 kInFlight），回收又要把写头推过那一段：写头只由写方自己推，
//     回收与写方自己的写放在同一个线程上，写头就始终只有一个线程在动，不会出现消息线程的回收与音频线程的
//     下一段写互相 CAS 的竞争（安全不变量 9：环头只由属主写）。
//   - 音频线程不读墙钟（安全不变量 5），帧数是它唯一的时钟。门槛是「1 s 的音频」：比实时快的离线渲染里
//     墙钟时间会缩短，但卡住的上一任已经死了（活着的写方不会公告了一段却一直不发布），多等墙钟时间换不来任何东西；
//     按音频帧计，渲染结果只取决于音频本身，同一个工程每次导出在同一个样本上恢复出声。
//   - 宿主不调我们时没有东西要写，也就不需要回收；离线渲染被宿主暂停时同理。
//   - 剩下的风险是 LoopRing.h 文件头写明的那一个窗口（挂起的旧写方醒来写进新一圈），与回收在哪个线程上做无关。
//
// 声道：写环恒为立体声（两格）。mono 时引擎把左声道复制到两格（环的布局是 kRingChannels = 2，
// 写侧的声道数由 min(声道数, 2) 决定：只有 1 个声道时两格写同一个值）。
//
// 旁路语义在引擎里（不是壳里）：宿主旁路这一端时，引擎写零进环、写头照常推进；
// 壳只要把 bypassed 传进来，不必自己造零。Output 旁路时回送在 D 之后自然归零。
//
// **旁路的 10 ms 斜坡**：宿主旁路这一端时，写进环的数据在 10 ms 内线性淡到 0
// （不是阶跃：阶跃会在环里留下一处咔嗒）。写头照常推进（那 10 ms 里写的是正在淡出的值）。
// 解除旁路时同样 10 ms 淡回。斜坡按发布进环的帧逐个推进，与宿主怎么切块无关；某一段写头 CAS 失败（被回收推过、
// 没有发布）时斜坡不前进（见 writeChunk）。
//
// 端点槽：给了会合段时，每块发布本端（Output）槽的块序号、宿主时间戳与实时标志。
// Input 靠 Output 槽的宿主时间戳判「Output 在不在 Input 之前」（错序判据），靠块序号判对端是否还活着。
//
// 线程：
//   - prepare（非实时线程）：换采样率时重算常数。
//   - process（音频线程）：清洗并写环。不加锁、不分配、不做 I/O、不读墙钟、不抛异常。
//   - 状态读出（消息线程）：只读本引擎自己的原子量。段只在音频线程的一块之内有效（块首租约、块末归还，
//     之后可能被解映射），消息线程从不碰段：要从环头转出来的计数（写头 CAS 失败次数）由音频线程每块拷进本引擎。
//   - 0 样本调用：立刻返回，不推进任何计数。
//
// 分层：core 不 include JUCE。
#pragma once

#include <atomic>
#include <cstdint>
#include <utility>

#include "diag/EventQueue.h"
#include "dsp/LinearSmoother.h"
#include "engine/AudioView.h"
#include "engine/EngineContext.h"
#include "engine/EngineEvents.h"
#include "engine/EngineParams.h"
#include "engine/TransportInfo.h"
#include "ipc/LoopRing.h"

namespace rinne::engine
{

// 状态读出用的原子量：音频线程写、消息线程读，必须无锁。
static_assert(std::atomic<std::uint8_t>::is_always_lock_free, "写环状态必须是无锁的原子量");
static_assert(std::atomic<std::uint32_t>::is_always_lock_free, "写环计数必须是无锁的原子量");
static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "写环计数必须是无锁的原子量");

// Output 的状态（写环状态，取值与 ipc::kWrite* 一致）。
enum class WriteState : std::uint8_t
{
    kIdle = 0, // 没有环或没有写权
    kActive = 1, // 正在写
    kBypassedZeros = 2, // 旁路：写零，写头照常推进
    kConflict = 3, // 冲突：写权不是自己的，不写
    kUnsupportedBlock = 4, // 块长超限：不写
};

// OutputEngine。
class OutputEngine
{
public:
    OutputEngine() = default;

    // 非实时线程：准备。sampleRate 是宿主声明的值。
    void prepare(double sampleRate, int maxBlock) noexcept;

    // 音频线程：处理一块。bypassed 为真表示宿主旁路了这一端：写零进环（10 ms 斜坡），写头照常推进。
    // context.ring 与 context.token 是调用方给的段（经租约拿到）与自己的写权 token，context.ringGeneration 是
    // 段的映射代号（判「换段」用）；context.rendezvous 与 context.group 给了时发布本端槽。宿主 buffer 一个字节都不改。
    // 返回 true 表示这一块写进了环；false 表示没写（0 样本、没有段、没有写权、冲突或块长超限）。
    bool process(const AudioView& buffer, const EngineParams& params, const TransportInfo& transport, bool bypassed,
                 const EngineContext& context) noexcept;

    // 消息线程：排空诊断事件
    template<typename Sink>
    int drainEvents(Sink&& sink) noexcept
    {
        return events_.drain(std::forward<Sink>(sink));
    }

    // 消息线程：读状态快照（只读本引擎自己的原子量，从不碰段）
    WriteState state() const noexcept { return static_cast<WriteState>(state_.load(std::memory_order_relaxed)); }
    std::uint64_t writtenFrames() const noexcept { return writtenFrames_.load(std::memory_order_relaxed); }
    std::uint32_t nonFiniteCount() const noexcept { return nonFinite_.load(std::memory_order_relaxed); }
    std::uint32_t clampCount() const noexcept { return clamps_.load(std::memory_order_relaxed); }
    std::uint32_t conflictCount() const noexcept { return conflicts_.load(std::memory_order_relaxed); }
    std::uint32_t reclaimCount() const noexcept { return reclaims_.load(std::memory_order_relaxed); }
    std::uint32_t unsupportedCount() const noexcept { return unsupported_.load(std::memory_order_relaxed); }
    // 环头里的写头 CAS 失败次数（环自己的计数，写方在 CAS 失败时自增）：音频线程每块在租约之内把它拷进
    // 本引擎，这里读的是那份拷贝（最近一块看到的值）。没有段之后保持最后的值，不再去读段。
    std::uint32_t wCasFailCount() const noexcept { return wCasFail_.load(std::memory_order_relaxed); }

    // 采样率与最大块长（诊断用）
    double sampleRate() const noexcept { return sampleRate_; }
    int maxBlock() const noexcept { return maxBlock_; }

private:
    // 写环状态：状态变了才推一条事件（冲突、块长超限一类的状态会连续很多块，不能每块推一条把队列灌满）。
    // n 是本块的块长（块长超限的提示码要带它）。
    void publishState(WriteState state, std::uint32_t n) noexcept;
    // 写方助手的任期：写权 token、段的映射代号或采样率任一变了，就复位 RingWriter（上一个任期的「被挡住的帧数」
    // 不带进新的任期，否则回收会提前）。只在音频线程上调用。
    void armWriter(const EngineContext& context, std::uint32_t sampleRate) noexcept;
    // 发布本端（Output）的会合槽：块序号、宿主时间戳、实时标志、本会话最大块长。
    void publishToSlot(const EngineContext& context, const TransportInfo& transport, bool bypassed, WriteState state,
                       std::uint32_t n) noexcept;
    // 写一段（不超过 kMaxWriteChunk 帧）。返回这一段的结果。
    ipc::RingWriteResult writeChunk(ipc::RingSegment& seg, const AudioView& buffer, std::uint32_t offset,
                                    std::uint32_t frames) noexcept;

    double sampleRate_ = 0.0;
    int maxBlock_ = 0;
    std::uint32_t maxBlockSeen_ = 0; // 本会话见过的最大块长（每次 prepare 清零）
    bool prepared_ = false;

    ipc::RingWriter writer_;
    diag::EventQueue<256> events_;

    // 旁路的 10 ms 斜坡：写进环的数据在旁路时 10 ms 淡到 0，解除旁路时 10 ms 淡回。
    // 斜坡按发布进环的帧推进，与块长无关。
    dsp::LinearSmoother bypassRamp_{1.0f};

    std::atomic<std::uint8_t> state_{static_cast<std::uint8_t>(WriteState::kIdle)};
    std::atomic<std::uint64_t> writtenFrames_{0};
    std::atomic<std::uint32_t> nonFinite_{0};
    std::atomic<std::uint32_t> clamps_{0};
    std::atomic<std::uint32_t> conflicts_{0};
    std::atomic<std::uint32_t> reclaims_{0};
    std::atomic<std::uint32_t> unsupported_{0};
    // 环头里的写头 CAS 失败次数的拷贝：音频线程每块在租约之内拷进来，消息线程只读这一份。
    std::atomic<std::uint32_t> wCasFail_{0};

    // 写方助手的任期（只有音频线程读写，不是原子量；prepare 在宿主保证不与 process 并发时调用）：
    // 上一次写环时的写权 token、段的映射代号与采样率。任一变了就 reset 写方助手。
    // 不记段的地址：同一个地址上可能是一块新的映射，地址判不出换段；映射代号判得出。
    bool writerArmed_ = false;
    std::uint64_t writerToken_ = 0;
    std::uint64_t writerGeneration_ = 0;
    std::uint32_t writerRate_ = 0;
};

} // namespace rinne::engine
