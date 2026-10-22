// SPDX-License-Identifier: GPL-3.0-or-later
//
// OutputEngine 的实现。见 OutputEngine.h 的说明。
//
// 三件事，按顺序：
//   1. 查写权：不是自己的写权就不写（置冲突状态），一个字节都不碰环。
//   2. 清洗：非有限 → 0（计数）、|v| > 16 → ±16（计数）、|v| < 1e−30 → 0。宿主 buffer 一个字节都不改。
//   3. 写环：按 ≤ 8192 帧分段，每段各自公告与发布。写头连续前进 n 帧，读方看不出分段。
//
// 清洗在写侧做（不是读侧）：环里的数据从此就是有限且有界的，读侧不必再判，也省一次遍历。
// 宿主 buffer 是逐位直通的：清洗只作用在「写进环的那一份」上，宿主自己那份不动。
//
// 实时约定：不分配、不加锁、不读墙钟、不抛异常；0 样本的调用立刻返回，不推进任何计数。
#include "engine/OutputEngine.h"

#include <algorithm>

#include "dsp/Sanitize.h"

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
    prepared_ = true;

    if (changed)
    {
        // 换采样率：写方助手的回收门槛要重算（它按帧计 1 s）。
        writer_.reset(static_cast<std::uint32_t>(sr));
        lastSampleRate_.store(static_cast<std::uint32_t>(sr), std::memory_order_relaxed);
    }

    // 旁路斜坡的长度按采样率算（10 ms）。不旁路时增益是 1。
    bypassRamp_.reset(sampleRate_, kBypassRampSeconds);
}

void OutputEngine::publishState(WriteState state) noexcept
{
    state_.store(static_cast<std::uint8_t>(state), std::memory_order_relaxed);
}

std::uint32_t OutputEngine::wCasFailCount() const noexcept
{
    // 环头里的计数由写方写（LoopRing::write 在 CAS 失败时自增）。写方是唯一写它的人，所以读它是对的。
    const auto* seg = lastSeg_.load(std::memory_order_relaxed);
    return seg == nullptr ? 0u : seg->header.w_cas_fail.load(std::memory_order_relaxed);
}

ipc::RingWriteResult OutputEngine::writeChunk(ipc::RingSegment& seg, const AudioView& buffer, bool bypassed,
                                               std::uint64_t token, std::uint32_t offset,
                                               std::uint32_t frames) noexcept
{
    (void)token;
    const float* left = buffer.left();
    const float* right = buffer.right();

    return writer_.write(seg, frames, [&](std::uint32_t i, std::uint64_t, float& outLeft, float& outRight) noexcept {
        // 旁路的斜坡：写进环的数据在旁路时 10 ms 淡到 0，解除旁路时 10 ms 淡回。
        // 斜坡按样本推进，与块长无关：同一段输入怎么切块，写进环的值逐位相同。
        const float ramp = bypassRamp_.getNextValue();
        const float l = left != nullptr ? left[offset + i] : 0.0f;
        // 右声道：立体声用右边；mono 时左声道复制到两格（环恒为立体声）。
        const float r = right != nullptr ? right[offset + i] : l;

        // 清洗：非有限 → 0；|v| > 16 → ±16；|v| < 1e−30 → 0。
        bool bad = false;
        const float sl = dsp::sanitizeSample(l * ramp, bad);
        const float sr = dsp::sanitizeSample(r * ramp, bad);
        if (bad)
        {
            nonFinite_.fetch_add(1, std::memory_order_relaxed);
            seg.header.nonfinite_count.fetch_add(1, std::memory_order_relaxed);
        }
        // 钳位计数：|v| 超过 16 的样本数（非有限的不算，非有限已经变成 0）。
        if (sl == dsp::kSanitizeLimit || sl == -dsp::kSanitizeLimit || sr == dsp::kSanitizeLimit ||
            sr == -dsp::kSanitizeLimit)
        {
            clamps_.fetch_add(1, std::memory_order_relaxed);
            seg.header.clamp_count.fetch_add(1, std::memory_order_relaxed);
        }
        outLeft = sl;
        outRight = sr;
    });
}

bool OutputEngine::process(const AudioView& buffer, const EngineParams& params, const TransportInfo& transport,
                            bool bypassed, ipc::RingSegment* seg, std::uint64_t token) noexcept
{
    (void)params;
    (void)transport;

    // 0 样本的调用立刻返回，不推进任何计数。
    if (buffer.length <= 0)
    {
        return false;
    }

    const auto n = static_cast<std::uint32_t>(buffer.length);

    // 没有段或没有写权：什么都不做。
    if (seg == nullptr || token == 0)
    {
        publishState(WriteState::kIdle);
        return false;
    }

    // 块长超过支持的上限：不写环、只出干声，并给红字提示码。
    if (n > ipc::kMaxSupportedBlock)
    {
        unsupported_.fetch_add(1, std::memory_order_relaxed);
        publishState(WriteState::kUnsupportedBlock);
        events_.push(diag::Event{static_cast<std::uint16_t>(EngineEventCode::kUnsupportedBlock),
                                  static_cast<std::uint8_t>(std::min(n, 255u)),
                                  static_cast<std::uint8_t>(ipc::kMaxSupportedBlock), 0});
        return false;
    }

    // 写权不是自己的：不写，置冲突标志。写头照旧（不动别人的）。
    if (!ipc::LoopRing::isWriter(*seg, token))
    {
        conflicts_.fetch_add(1, std::memory_order_relaxed);
        publishState(WriteState::kConflict);
        events_.push(diag::Event{static_cast<std::uint16_t>(EngineEventCode::kNoPair), 1, 0, 0});
        return false;
    }

    // 换段或换采样率：写方助手要 reset（它按采样率算回收门槛）。
    const auto segPtr = static_cast<const ipc::RingSegment*>(seg);
    const auto sr = static_cast<std::uint32_t>(sampleRate_ > 0.0 ? sampleRate_ : 48000.0);
    if (lastSeg_.load(std::memory_order_relaxed) != segPtr || lastSampleRate_.load(std::memory_order_relaxed) != sr)
    {
        writer_.reset(sr);
        lastSeg_.store(segPtr, std::memory_order_relaxed);
        lastSampleRate_.store(sr, std::memory_order_relaxed);
    }

    // 旁路的斜坡：目标是 0（旁路）或 1（不旁路）。斜坡按样本推进，在写块里逐样本取。
    bypassRamp_.setTargetValue(bypassed ? 0.0f : 1.0f);

    // 分段写：每段不超过 kMaxWriteChunk 帧。宿主给 32768 帧 → 4 段 8192。
    // 每段各自公告与发布（写头 CAS 用的是这一段自己的起点，共用块首的快照会让第 2 段的 CAS 失败）。
    std::uint32_t written = 0;
    while (written < n)
    {
        const std::uint32_t chunk = std::min<std::uint32_t>(ipc::kMaxWriteChunk, n - written);
        const ipc::RingWriteResult result = writeChunk(*seg, buffer, bypassed, token, written, chunk);
        if (result != ipc::RingWriteResult::kOk)
        {
            // 这一段没写进去：写头没有推进（协议要求），本块到此为止。
            //   kInFlight 别人公告了一段还没发布：这一组暂时静默。回收由 RingWriter 按帧计 1 s 后做。
            //   kCasLost 写头 CAS 失败：这一段被回收推过了（环头里自增 w_cas_fail）。
            publishState(WriteState::kIdle);
            break;
        }
        written += chunk;
    }

    if (written > 0)
    {
        writtenFrames_.fetch_add(written, std::memory_order_relaxed);
        publishState(bypassed ? WriteState::kBypassedZeros : WriteState::kActive);
    }
    else if (state_.load(std::memory_order_relaxed) == static_cast<std::uint8_t>(WriteState::kActive))
    {
        publishState(WriteState::kIdle);
    }

    // 回收计数（写方助手自己记的：真正推过写头的回收次数）。
    const auto reclaims = writer_.reclaims();
    if (reclaims > 0)
    {
        reclaims_.store(reclaims, std::memory_order_relaxed);
    }

    return written > 0;
}

} // namespace rinne::engine
