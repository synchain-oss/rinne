// SPDX-License-Identifier: GPL-3.0-or-later
//
// 数据段头侧的非模板部分：几何的初始化与校验、写权（writer_token 与 epoch）的发布与交还，以及把 pull 包成
// 「拷进左右声道缓冲、顺手清零无效帧」的便利读取。协议与内存序的说明在 LoopRing.h 的文件头；这里只做不涉及
// 模板的那一半，写环与读环的热路径都在头文件里内联到调用点。
#include "ipc/LoopRing.h"

namespace rinne::ipc
{

void LoopRing::initGeometry(RingSegment& seg) noexcept
{
    seg.header.ring_frames.store(kRingFrames, std::memory_order_relaxed);
    seg.header.ring_channels.store(kRingChannels, std::memory_order_relaxed);
}

bool LoopRing::geometryOk(const RingSegment& seg) noexcept
{
    return seg.header.ring_frames.load(std::memory_order_relaxed) == kRingFrames &&
           seg.header.ring_channels.load(std::memory_order_relaxed) == kRingChannels;
}

void LoopRing::publishWriter(RingSegment& seg, u64 writerToken, u32 sampleRate, u32 srcChannels) noexcept
{
    if (writerToken == 0)
    {
        return;
    }
    // 采样率变了、或者这一段从没发布过写方：先写采样率与源声道数，再以 release 抬 epoch。读方先 acquire 读 epoch，
    // 才能保证它随后读到的 sample_rate 是新的。epoch 只在采样率改变时 + 1，读方靠它判断要不要重新同步
    if (seg.header.sample_rate.load(std::memory_order_relaxed) != sampleRate ||
        seg.header.epoch.load(std::memory_order_relaxed) == 0)
    {
        seg.header.sample_rate.store(sampleRate, std::memory_order_relaxed);
        seg.header.src_channels.store(srcChannels, std::memory_order_relaxed);
        seg.header.epoch.fetch_add(1, std::memory_order_release);
    }
    else
    {
        seg.header.src_channels.store(srcChannels, std::memory_order_relaxed);
    }
    // 写权最后发布：读方看到新 token 时，它需要的格式字段都已经写好。W 与 intent 一概不碰（换写方不清零）
    seg.header.writer_token.store(writerToken, std::memory_order_release);
}

bool LoopRing::retireWriter(RingSegment& seg, u64 writerToken) noexcept
{
    if (writerToken == 0)
    {
        return false;
    }
    u64 expected = writerToken;
    return seg.header.writer_token.compare_exchange_strong(expected, 0, std::memory_order_acq_rel,
                                                           std::memory_order_relaxed);
}

RingFormat LoopRing::readFormat(const RingSegment& seg) noexcept
{
    // 次序是契约：先 acquire 读 epoch、writer_token（与写方的 release 配对），再 relaxed 读 sample_rate
    RingFormat format;
    format.epoch = seg.header.epoch.load(std::memory_order_acquire);
    format.writerToken = seg.header.writer_token.load(std::memory_order_acquire);
    format.sampleRate = seg.header.sample_rate.load(std::memory_order_relaxed);
    return format;
}

bool LoopRing::isWriter(const RingSegment& seg, u64 token) noexcept
{
    return token != 0 && seg.header.writer_token.load(std::memory_order_acquire) == token;
}

u64 LoopRing::writeHead(const RingSegment& seg) noexcept
{
    return seg.header.write_head.load(std::memory_order_acquire);
}

u64 LoopRing::writeIntent(const RingSegment& seg) noexcept
{
    return seg.header.write_intent.load(std::memory_order_relaxed);
}

RingRead LoopRing::read(const RingSegment& seg, i64 from, u32 frames, float* left, float* right) noexcept
{
    const u64 w = writeHead(seg);
    // to 不超过读到的 W：只有已经发布的帧才算数。from 已经在 W 之后时没有可读的帧（差按有符号算，不会回绕）
    const i64 available = frameDiff(w, static_cast<u64>(from));
    const i64 to = from + std::min<i64>(static_cast<i64>(frames), std::max<i64>(available, 0));
    const RingPull pulled = pull(seg, from, to, [&](i64 p, float l, float r) {
        const auto at = static_cast<std::size_t>(p - from);
        left[at] = l;
        right[at] = r;
    });
    // 有效区间 [validFrom, validTo) 总落在 [from, to) 里：整段都被覆盖过时它是空的 [to, to)
    const i64 validFrom = std::clamp(pulled.validFrom, from, to);
    const i64 validTo = to;
    // 没读过的帧（比 to 早一圈的、负位置的、超过 W 的）与可能被覆盖的帧一律按 0：读方永远不拿旧数据当有效数据
    for (u32 i = 0; i < frames; ++i)
    {
        const i64 p = from + static_cast<i64>(i);
        if (p < validFrom || p >= to)
        {
            left[i] = 0.0f;
            right[i] = 0.0f;
        }
    }
    return {w, validFrom, validTo};
}

} // namespace rinne::ipc
