// SPDX-License-Identifier: GPL-3.0-or-later
//
// OutputEngine：Rinne Output 的 core 侧引擎。宿主壳只做映射（参数 → EngineParams，宿主播放头 → TransportInfo），
// 然后调这里。引擎负责：清洗之后把宿主信号写进环、写头照常推进、发布状态。
//
// 三条硬规则：
//   1. 宿主的 buffer 逐位直通（不改动一个字节）。Output 只旁路地读它。
//   2. 写环只经 RingWriter（src/core/ipc/LoopRing.h）。直接调 LoopRing::write 的写方碰上写到一半死掉的
//      上一任，会永远拿到 kInFlight，这一组从此静音。RingWriter 负责卡住时的回收。
//   3. 每块先查 isWriter：写权不是自己的就不写（置冲突标志）。换写方、换段或采样率改变时 RingWriter::reset。
//
// 写环按 ≤ 8192 帧分段（kMaxWriteChunk）：**由引擎按 8192 帧切段**，每段各自公告与发布
// （W 连续前进 n 帧，读方看不出分段）。宿主给 32768 帧 → 4 段 8192。
// 块长超过 32768（kMaxSupportedBlock）时不写环、只出干声，并给红字提示码 kUnsupportedBlock。
//
// 声道：写环恒为立体声（两格）。mono 时引擎把左声道复制到两格（环的布局是 kRingChannels = 2，
// 写侧的声道数由 min(声道数, 2) 决定：只有 1 个声道时两格写同一个值）。
//
// 旁路语义在引擎里（不是壳里）：宿主旁路这一端时，引擎写零进环、写头照常推进；
// 壳只要把 bypassed 传进来，不必自己造零。Output 旁路时回送在 D 之后自然归零。
//
// **旁路的 10 ms 斜坡**：宿主旁路这一端时，写进环的数据在 10 ms 内线性淡到 0
// （不是阶跃：阶跃会在环里留下一处咔嗒）。写头照常推进（那 10 ms 里写的是正在淡出的值）。
// 解除旁路时同样 10 ms 淡回。
//
// 线程：
//   - prepare（非实时线程）：换采样率时重算常数。
//   - process（音频线程）：清洗并写环。不加锁、不分配、不做 I/O、不读墙钟、不抛异常。
//   - 0 样本调用：立刻返回，不推进任何计数。
//
// 分层：core 不 include JUCE。
#pragma once

#include <atomic>
#include <cstdint>
#include <utility>

#include "diag/EventQueue.h"
#include "dsp/LinearSmoother.h"
#include "engine/EngineEvents.h"
#include "engine/EngineParams.h"
#include "engine/TransportInfo.h"
#include "ipc/LoopRing.h"

namespace rinne::engine
{

// Output 的状态（写环状态，取值与 ipc::kWrite* 一致）。
enum class WriteState : std::uint8_t
{
    kIdle = 0,             // 没有环或没有写权
    kActive = 1,           // 正在写
    kBypassedZeros = 2,    // 旁路：写零，写头照常推进
    kConflict = 3,         // 冲突：写权不是自己的，不写
    kUnsupportedBlock = 4, // 块长超限：不写
};

// OutputEngine。
class OutputEngine
{
public:
    OutputEngine() = default;

    // 非实时线程：准备。sampleRate 是宿主声明的值。
    void prepare(double sampleRate, int maxBlock) noexcept;

    // 音频线程：处理一块。bypassed 为真表示宿主旁路了这一端：写零进环，写头照常推进。
    // seg 与 token 是调用方给的：段（经租约拿到）与自己的写权 token。
    // 返回 true 表示这一块写进了环；false 表示没写（没有段、没有写权、冲突或块长超限）。
    bool process(const AudioView& buffer, const EngineParams& params, const TransportInfo& transport, bool bypassed,
                 ipc::RingSegment* seg, std::uint64_t token) noexcept;

    // 消息线程：排空诊断事件
    template<typename Sink>
    int drainEvents(Sink&& sink) noexcept
    {
        return events_.drain(std::forward<Sink>(sink));
    }

    // 消息线程：读状态快照
    WriteState state() const noexcept { return static_cast<WriteState>(state_.load(std::memory_order_relaxed)); }
    std::uint64_t writtenFrames() const noexcept { return writtenFrames_.load(std::memory_order_relaxed); }
    std::uint32_t nonFiniteCount() const noexcept { return nonFinite_.load(std::memory_order_relaxed); }
    std::uint32_t clampCount() const noexcept { return clamps_.load(std::memory_order_relaxed); }
    std::uint32_t conflictCount() const noexcept { return conflicts_.load(std::memory_order_relaxed); }
    std::uint32_t reclaimCount() const noexcept { return reclaims_.load(std::memory_order_relaxed); }
    std::uint32_t unsupportedCount() const noexcept { return unsupported_.load(std::memory_order_relaxed); }
    // 写环头里的写头 CAS 失败次数（环自己的计数，写方在 CAS 失败时自增）。
    std::uint32_t wCasFailCount() const noexcept;

    // 采样率与最大块长（诊断用）
    double sampleRate() const noexcept { return sampleRate_; }
    int maxBlock() const noexcept { return maxBlock_; }

private:
    void publishState(WriteState state) noexcept;
    // 写一段（不超过 kMaxWriteChunk 帧）。返回这一段的结果。
    ipc::RingWriteResult writeChunk(ipc::RingSegment& seg, const AudioView& buffer, bool bypassed, std::uint64_t token,
                                     std::uint32_t offset, std::uint32_t frames) noexcept;

    double sampleRate_ = 0.0;
    int maxBlock_ = 0;
    bool prepared_ = false;

    ipc::RingWriter writer_;
    diag::EventQueue<256> events_;

    // 旁路的 10 ms 斜坡：写进环的数据在旁路时 10 ms 淡到 0，解除旁路时 10 ms 淡回。
    // 斜坡按样本推进，与块长无关。
    dsp::LinearSmoother bypassRamp_{1.0f};

    std::atomic<std::uint8_t> state_{static_cast<std::uint8_t>(WriteState::kIdle)};
    std::atomic<std::uint64_t> writtenFrames_{0};
    std::atomic<std::uint32_t> nonFinite_{0};
    std::atomic<std::uint32_t> clamps_{0};
    std::atomic<std::uint32_t> conflicts_{0};
    std::atomic<std::uint32_t> reclaims_{0};
    std::atomic<std::uint32_t> unsupported_{0};
    // 上一次写环时用的段与采样率：段换了或采样率变了要 reset 写方助手。
    std::atomic<const ipc::RingSegment*> lastSeg_{nullptr};
    std::atomic<std::uint32_t> lastSampleRate_{0};
};

} // namespace rinne::engine
