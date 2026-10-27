// SPDX-License-Identifier: GPL-3.0-or-later
//
// OutputEngine 的实现。见 OutputEngine.h 的说明。
//
// 四件事，按顺序：
//   1. 查写权：不是自己的写权就不写（置冲突状态），一个字节都不碰环。
//   2. 清洗：非有限 → 0（计数）、|v| > 16 → ±16（计数）、|v| < 1e−30 → 0。宿主 buffer 一个字节都不改。
//   3. 写环：按 ≤ 8192 帧分段，每段各自公告与发布。写头连续前进 n 帧，读方看不出分段。
//   4. 发布本端的会合槽（给了会合段时）。
//
// 清洗在写侧做（不是读侧）：环里的数据从此就是有限且有界的，读侧不必再判，也省一次遍历。
// 宿主 buffer 是逐位直通的：清洗只作用在「写进环的那一份」上，宿主自己那份不动。
//
// 实时约定：不分配、不加锁、不读墙钟、不抛异常；0 样本的调用立刻返回，不推进任何计数。
#include "engine/OutputEngine.h"

#include <algorithm>
#include <cmath>

#include "dsp/Sanitize.h"
#include "ipc/LoopBinding.h"
#include "loop/LoopEvents.h"

namespace rinne::engine
{

namespace
{
// 旁路斜坡的长度（秒）：10 ms。写进环的数据在旁路时 10 ms 淡到 0，解除旁路时 10 ms 淡回。
inline constexpr double kBypassRampSeconds = 0.010;
} // namespace

void OutputEngine::prepare(double sampleRate, int maxBlock) noexcept
{
    const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    const bool changed = !prepared_ || sampleRate_ != sr;
    sampleRate_ = sr;
    maxBlock_ = maxBlock > 0 ? maxBlock : 0;
    maxBlockSeen_ = 0;
    prepared_ = true;

    if (changed)
    {
        // 换采样率：写方助手的回收门槛要重算（它按帧计 1 s）。下一块写环之前按新的采样率重新起一个任期。
        writer_.reset(static_cast<std::uint32_t>(sr));
        writerArmed_ = false;
    }

    // 旁路斜坡的长度按采样率算（10 ms）。不旁路时增益是 1；当前值不动（同采样率重新 prepare 不跳）。
    bypassRamp_.reset(sampleRate_, kBypassRampSeconds);

    // 错序证据：换采样率才清，同采样率重新 prepare 证据保持连续
    order_.prepare(sampleRate_);
}

void OutputEngine::publishState(WriteState state, std::uint32_t n) noexcept
{
    const auto previous = state_.exchange(static_cast<std::uint8_t>(state), std::memory_order_relaxed);
    if (previous == static_cast<std::uint8_t>(state))
    {
        return;
    }
    // 状态变了才记一条：冲突与块长超限给界面提示码（引擎段 0x04xx）。这两种状态会连续很多块，每块一条会把队列灌满。
    if (state == WriteState::kConflict)
    {
        events_.push(diag::Event{static_cast<std::uint16_t>(EngineEventCode::kWriteConflict), 1, 0, 0});
    }
    else if (state == WriteState::kUnsupportedBlock)
    {
        events_.push(diag::Event{static_cast<std::uint16_t>(EngineEventCode::kUnsupportedBlock),
                                 loop::eventUnits256(static_cast<std::int64_t>(n)),
                                 loop::eventUnits256(static_cast<std::int64_t>(ipc::kMaxSupportedBlock)), 0});
    }
}

void OutputEngine::armWriter(const EngineContext& context, std::uint32_t sampleRate) noexcept
{
    if (writerArmed_ && writerToken_ == context.token && writerGeneration_ == context.ringGeneration &&
        writerRate_ == sampleRate)
    {
        return;
    }
    writer_.reset(sampleRate);
    writerArmed_ = true;
    writerToken_ = context.token;
    writerGeneration_ = context.ringGeneration;
    writerRate_ = sampleRate;
}

ipc::RingWriteResult OutputEngine::writeChunk(ipc::RingSegment& seg, const AudioView& buffer, std::uint32_t offset,
                                              std::uint32_t frames) noexcept
{
    const float* left = buffer.left();
    const float* right = buffer.right(); // mono 时与 left 是同一串：两格写同一个值（环恒为立体声）

    // 旁路的斜坡按「真正发布进环的帧」推进。RingWriter::write 在公告了这一段（write_intent 推到 W + k）之后、
    // 推写头之前逐帧调回调；写头的 CAS 输了（kCasLost：这一段在写的时候被回收推过，没有发布）时回调已经全部跑过。
    // 所以回调里推进的是斜坡的一份拷贝，这一段发布了（kOk）才交回去；kInFlight 时回调一次都没调，拷贝也没动。
    // 否则丢掉的那一段照样把斜坡往前推，之后真正写进环的数据就和斜坡错开了。
    dsp::LinearSmoother ramp = bypassRamp_;
    const ipc::RingWriteResult result =
        writer_.write(seg, frames, [&](std::uint32_t i, std::uint64_t, float& outLeft, float& outRight) noexcept {
            // 旁路的斜坡：写进环的数据在旁路时 10 ms 淡到 0，解除旁路时 10 ms 淡回。
            // 斜坡按写进环的帧推进，与块长无关：同一段输入怎么切块，写进环的值逐位相同。
            const float gain = ramp.getNextValue();
            const float l = left != nullptr ? left[offset + i] : 0.0f;
            const float r = right != nullptr ? right[offset + i] : l;

            // 清洗：非有限 → 0；|v| > 16 → ±16；|v| < 1e−30 → 0。先清洗再乘斜坡（非有限 × 0 仍是非有限）。
            bool bad = false;
            float sl = dsp::sanitizeSample(l, bad);
            float sr = dsp::sanitizeSample(r, bad);
            if (bad)
            {
                nonFinite_.fetch_add(1, std::memory_order_relaxed);
                seg.header.nonfinite_count.fetch_add(1, std::memory_order_relaxed);
            }
            // 钳位计数：|v| 超过 16 的帧数（非有限的不算，非有限已经变成 0）。
            if (std::fabs(l) > dsp::kSanitizeLimit || std::fabs(r) > dsp::kSanitizeLimit)
            {
                if (dsp::isFiniteSample(l) && dsp::isFiniteSample(r))
                {
                    clamps_.fetch_add(1, std::memory_order_relaxed);
                    seg.header.clamp_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
            if (gain != 1.0f)
            {
                sl = dsp::flushTiny(sl * gain);
                sr = dsp::flushTiny(sr * gain);
            }
            outLeft = sl;
            outRight = sr;
        });
    if (result == ipc::RingWriteResult::kOk)
    {
        bypassRamp_ = ramp;
    }
    return result;
}

// 错序证据的 Output 一侧：块首读 Input 槽（块序号 acquire，再读时间戳），与本块的宿主时间戳比。
// 只读对端的槽，不写；会合段没给或组号越界时当没有对端槽（读会越界）。
void OutputEngine::judgeOrder(const EngineContext& context, const TransportInfo& transport, std::uint32_t n) noexcept
{
    bool present = false;
    std::uint64_t peerSeq = 0;
    std::uint64_t peerNs = 0;
    if (context.rendezvous != nullptr && context.group != 0 && context.group <= ipc::kGroupCount)
    {
        const ipc::RoleSlot& peer = ipc::roleSlot(*context.rendezvous, context.group, ipc::Role::kInput);
        peerSeq = peer.block_seq.load(std::memory_order_acquire);
        peerNs = peer.last_host_ns.load(std::memory_order_acquire);
        present = true;
    }
    order_.observe(loop::OrderSide::kOutput, static_cast<std::int64_t>(n), transport.hostNsValid, transport.hostNs,
                   present, peerSeq, peerNs);
    const loop::OrderSnapshot snapshot = order_.snapshot();
    orderPacked_.store(snapshot.pack(), std::memory_order_relaxed);
    orderGlitches_.store(snapshot.glitches, std::memory_order_relaxed);
}

// 发布本端（Output）的会合槽。写的是自己的槽（安全不变量 9），角色固定为 Output。
void OutputEngine::publishToSlot(const EngineContext& context, const TransportInfo& transport, bool bypassed,
                                 WriteState state, std::uint32_t n) noexcept
{
    if (context.rendezvous == nullptr || context.group == 0 || context.group > ipc::kGroupCount)
    {
        return;
    }
    ipc::RoleSlot& mine = ipc::roleSlot(*context.rendezvous, context.group, ipc::Role::kOutput);

    mine.last_host_ns.store(transport.hostNsValid ? transport.hostNs : 0u, std::memory_order_relaxed);
    mine.max_n_seen.store(maxBlockSeen_, std::memory_order_relaxed);

    std::uint32_t flags = 0;
    if (bypassed)
    {
        flags |= ipc::kRtBypassed;
    }
    if (transport.nonrealtime)
    {
        flags |= ipc::kRtNonrealtime;
    }
    if (maxBlock_ > 0 && n > static_cast<std::uint32_t>(maxBlock_))
    {
        flags |= ipc::kRtOverMax;
    }
    if (state == WriteState::kConflict)
    {
        flags |= ipc::kRtConflict;
    }
    if (state == WriteState::kUnsupportedBlock)
    {
        flags |= ipc::kRtUnsupportedBlock;
    }
    mine.rt_flags.store(flags, std::memory_order_relaxed);

    // 块序号最后 release：对端先看见序号就看得见这一块的时间戳与标志。
    mine.block_seq.fetch_add(1, std::memory_order_release);
}

bool OutputEngine::process(const AudioView& buffer, const EngineParams& params, const TransportInfo& transport,
                           bool bypassed, const EngineContext& context) noexcept
{
    (void)params;

    // 0 样本的调用立刻返回，不推进任何计数。
    if (buffer.length <= 0)
    {
        return false;
    }

    const auto n = static_cast<std::uint32_t>(buffer.length);
    maxBlockSeen_ = std::max(maxBlockSeen_, n);

    // 错序证据：每个支持块判一次，在发布本端的槽之前读 Input 槽（没有段、没有写权的块也判：错序与写环无关）。
    if (n <= ipc::kMaxSupportedBlock)
    {
        judgeOrder(context, transport, n);
    }

    ipc::RingSegment* seg = context.ring;
    const std::uint64_t token = context.token;

    // 没有段或没有写权：什么都不写。
    if (seg == nullptr || token == 0)
    {
        publishState(WriteState::kIdle, n);
        publishToSlot(context, transport, bypassed, WriteState::kIdle, n);
        return false;
    }

    // 段在这一块之内有效（块首租约）：环头里要转给消息线程的计数就在这里拷进本引擎，消息线程从不碰段。
    wCasFail_.store(seg->header.w_cas_fail.load(std::memory_order_relaxed), std::memory_order_relaxed);

    // 块长超过支持的上限：不写环、只出干声，并给红字提示码（只在进入超限时一条）。写头不动，Input 侧按断档处理。
    if (n > ipc::kMaxSupportedBlock)
    {
        unsupported_.fetch_add(1, std::memory_order_relaxed);
        publishState(WriteState::kUnsupportedBlock, n);
        publishToSlot(context, transport, bypassed, WriteState::kUnsupportedBlock, n);
        return false;
    }

    // 写权不是自己的：不写，置冲突标志。写头照旧（不动别人的）。
    if (!ipc::LoopRing::isWriter(*seg, token))
    {
        conflicts_.fetch_add(1, std::memory_order_relaxed);
        publishState(WriteState::kConflict, n);
        publishToSlot(context, transport, bypassed, WriteState::kConflict, n);
        return false;
    }

    // 换写方、换段或换采样率：写方助手起一个新任期（reset）。它按采样率算回收门槛，按任期数「被挡住的帧数」。
    armWriter(context, static_cast<std::uint32_t>(sampleRate_ > 0.0 ? sampleRate_ : 48000.0));

    // 旁路的斜坡：目标是 0（旁路）或 1（不旁路）。斜坡按样本推进，在写块里逐样本取。
    bypassRamp_.setTargetValue(bypassed ? 0.0f : 1.0f);

    // 分段写：每段不超过 kMaxWriteChunk 帧。宿主给 32768 帧 → 4 段 8192。
    // 每段各自公告与发布（写头 CAS 用的是这一段自己的起点，共用块首的快照会让第 2 段的 CAS 失败）。
    //   kInFlight 别人公告了一段还没发布：这一段没写（写头没有推进），下一段照样交给 RingWriter ——
    //             它按被挡住的帧数计回收门槛，一块里的每一段都要算进去，门槛才按实际经过的音频走满 1 s。
    //             回收在某一段里发生之后，余下的段照常写进去。
    //   kCasLost  写头 CAS 失败：这一段被回收推过了（环头里自增 w_cas_fail），本块到此为止。
    std::uint32_t written = 0;
    std::uint32_t offset = 0;
    while (offset < n)
    {
        const std::uint32_t chunk = std::min<std::uint32_t>(ipc::kMaxWriteChunk, n - offset);
        const ipc::RingWriteResult result = writeChunk(*seg, buffer, offset, chunk);
        offset += chunk;
        if (result == ipc::RingWriteResult::kOk)
        {
            written += chunk;
            continue;
        }
        if (result != ipc::RingWriteResult::kInFlight)
        {
            break;
        }
    }

    WriteState state = WriteState::kIdle;
    if (written > 0)
    {
        writtenFrames_.fetch_add(written, std::memory_order_relaxed);
        state = bypassed ? WriteState::kBypassedZeros : WriteState::kActive;
    }
    publishState(state, n);
    publishToSlot(context, transport, bypassed, state, n);

    // 回收计数（写方助手自己记的：真正推过写头的回收次数）与写头 CAS 失败次数（本块写完之后的值）。
    reclaims_.store(writer_.reclaims(), std::memory_order_relaxed);
    wCasFail_.store(seg->header.w_cas_fail.load(std::memory_order_relaxed), std::memory_order_relaxed);

    return written > 0;
}

} // namespace rinne::engine
