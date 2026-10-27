// SPDX-License-Identifier: GPL-3.0-or-later
//
// 时序用例的小工具：一个包住 LoopSession 与一条环的驱动器，加一个「位置的确定性函数」当信号。
//
// 约定：Input 在前、写方在后。每一块先读（feed），再让写方写同样多的帧（write），所以块首的写头恰好等于读位 R，
// 同一串输入怎么切块，每一块看到的环都是同一份：块长不同的两次运行才有资格逐位比较。
//
// 信号：noiseAt(p) 是绝对位置 p 的哈希，落在 [-0.5, 0.5)，24 位整数除以 2^24，float 精确表示。
// 它没有任何斜率可言，所以读位置错一个样本、增益错一个比特，输出都会变。
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

#include "engine/EngineParams.h"
#include "engine/TransportInfo.h"
#include "ipc/LoopBinding.h"
#include "ipc/LoopRing.h"
#include "loop/LoopEvents.h"
#include "loop/LoopSession.h"
#include "loop/LoopStatus.h"

namespace rinne::timing_test
{

namespace ipc = rinne::ipc;
using rinne::engine::DelayMode;
using rinne::engine::EngineParams;
using rinne::engine::TransportInfo;
using rinne::loop::BlockContext;
using rinne::loop::BlockResult;
using rinne::loop::LoopEventCode;
using rinne::loop::LoopEventSink;
using rinne::loop::LoopSession;
using rinne::loop::LoopStatusFields;
using rinne::loop::ResyncReason;
using rinne::loop::SyncReason;

// 位置 p 上的信号值：确定、没有结构、float 精确。
inline float noiseAt(std::int64_t p) noexcept
{
    auto x = static_cast<std::uint64_t>(p) * 0x9E3779B97F4A7C15ull;
    x ^= x >> 29;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 32;
    return static_cast<float>(static_cast<std::int32_t>(x & 0xFFFFFFu) - 0x800000) / 16777216.0f;
}

// 两串 float 逐位相同
inline bool sameBits(const std::vector<float>& a, const std::vector<float>& b)
{
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
}

// 第一处位型不同的下标；全同是 -1
inline long long firstDiff(const std::vector<float>& a, const std::vector<float>& b)
{
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i)
    {
        if (std::memcmp(&a[i], &b[i], sizeof(float)) != 0)
        {
            return static_cast<long long>(i);
        }
    }
    return a.size() == b.size() ? -1 : static_cast<long long>(n);
}

// 相邻样本差的最大值（从 from 起）
inline float maxStep(const std::vector<float>& trace, std::size_t from = 0)
{
    float worst = 0.0f;
    for (std::size_t i = from + 1; i < trace.size(); ++i)
    {
        worst = std::max(worst, std::fabs(trace[i] - trace[i - 1]));
    }
    return worst;
}

struct LoggedEvent
{
    std::uint16_t code = 0;
    std::uint8_t a = 0;
    std::uint8_t b = 0;
    std::int64_t sample = 0;
};

// 一条环 + 一个 LoopSession。回送的左声道逐块追加到 trace，produced 记下哪些样本真的产出了（算账块不产出）。
class Rig
{
public:
    explicit Rig(double sampleRate = 48000.0, std::int64_t declaredBlock = 1024)
        : sampleRate_(sampleRate), declaredBlock_(declaredBlock)
    {
        left_.assign(static_cast<std::size_t>(ipc::kMaxSupportedBlock) + 8u, 0.0f);
        right_.assign(left_.size(), 0.0f);
        valid_.assign(left_.size(), 0u);
        REQUIRE(session_.prepare(sampleRate_));
        session_.setEventSink(LoopEventSink(&Rig::sink, this));
        ownRing_ = makeRing();
        attach(*ownRing_, 1);
    }

    // 事件出口里存着本对象的地址，所以不许拷贝与搬走
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    static std::unique_ptr<ipc::RingSegment> makeRing()
    {
        auto seg = std::make_unique<ipc::RingSegment>();
        ipc::LoopRing::initGeometry(*seg);
        return seg;
    }

    // 接上一条环、发布写权
    void attach(ipc::RingSegment& ring, std::uint64_t token)
    {
        ring_ = &ring;
        ipc::LoopRing::publishWriter(ring, token, static_cast<std::uint32_t>(sampleRate_), 2);
    }
    ipc::RingSegment& ring() { return *ring_; }

    void setSignal(std::function<float(std::int64_t)> signal) { signal_ = std::move(signal); }

    // 读一块。produce 为假时不给回送缓冲：环路层只算账、不出样本（快速路径的口径）。
    BlockResult feed(int n, const EngineParams& params, const TransportInfo& transport = TransportInfo{},
                     bool produce = true)
    {
        BlockContext ctx(params, transport);
        ctx.ring = ring_;
        ctx.declaredBlock = declaredBlock_;
        ctx.observedBlock = declaredBlock_;
        ctx.blockLength = n;
        if (produce)
        {
            ctx.returnLeft = left_.data();
            ctx.returnRight = right_.data();
            ctx.returnValid = valid_.data();
        }
        const BlockResult result = session_.process(ctx);
        for (int i = 0; i < n; ++i)
        {
            trace.push_back(produce ? left_[static_cast<std::size_t>(i)] : 0.0f);
            produced.push_back(produce ? 1u : 0u);
        }
        return result;
    }

    // 写方写 n 帧（按信号），一次不超过 8192
    void write(std::int64_t n)
    {
        while (n > 0)
        {
            const auto chunk = static_cast<std::uint32_t>(std::min<std::int64_t>(n, 8192));
            const ipc::RingWriteResult result =
                ipc::LoopRing::write(*ring_, chunk, [&](std::uint32_t, std::uint64_t p, float& l, float& r) {
                    l = signal_(static_cast<std::int64_t>(p));
                    r = l;
                });
            REQUIRE(result == ipc::RingWriteResult::kOk);
            n -= chunk;
        }
    }

    // 一块：先读、再写同样多的帧（Input 在前、写方在后）
    BlockResult step(int n, const EngineParams& params, const TransportInfo& transport = TransportInfo{},
                     bool produce = true)
    {
        const BlockResult result = feed(n, params, transport, produce);
        write(n);
        return result;
    }

    // 一块：先写、再读（写方在前）。块首的写头比读位多一块，读头锚定的同步偏移就是块长。
    BlockResult stepOutputFirst(int n, const EngineParams& params, const TransportInfo& transport = TransportInfo{},
                                bool produce = true)
    {
        write(n);
        return feed(n, params, transport, produce);
    }

    // 把 total 个样本按 sizes 给的块长循环切开跑（最后一块截短），参数不变
    void run(std::int64_t total, const std::vector<int>& sizes, const EngineParams& params,
             const TransportInfo& transport = TransportInfo{}, bool produce = true)
    {
        std::size_t k = 0;
        while (total > 0)
        {
            const int n = static_cast<int>(std::min<std::int64_t>(sizes[k % sizes.size()], total));
            step(n, params, transport, produce);
            total -= n;
            ++k;
        }
    }

    LoopSession& session() { return session_; }
    LoopStatusFields status() const { return session_.statusFields(); }
    std::int64_t processed() const { return session_.processed(); }

    // 交叉淡化的起点（改 D 事件里带绝对样本位置）
    std::vector<std::int64_t> xfadeStarts() const
    {
        std::vector<std::int64_t> out;
        for (const LoggedEvent& e : events)
        {
            if (e.code == static_cast<std::uint16_t>(LoopEventCode::kResync) &&
                e.a == static_cast<std::uint8_t>(ResyncReason::kDChange))
            {
                out.push_back(e.sample);
            }
        }
        return out;
    }
    std::size_t count(LoopEventCode code) const
    {
        std::size_t n = 0;
        for (const LoggedEvent& e : events)
        {
            n += e.code == static_cast<std::uint16_t>(code) ? 1u : 0u;
        }
        return n;
    }

    std::vector<float> trace;
    std::vector<std::uint8_t> produced;
    std::vector<LoggedEvent> events;

private:
    static void sink(void* context, std::uint16_t code, std::uint8_t a, std::uint8_t b, std::int64_t sample) noexcept
    {
        static_cast<Rig*>(context)->events.push_back(LoggedEvent{code, a, b, sample});
    }

    double sampleRate_;
    std::int64_t declaredBlock_;
    LoopSession session_;
    std::unique_ptr<ipc::RingSegment> ownRing_;
    ipc::RingSegment* ring_ = nullptr;
    std::function<float(std::int64_t)> signal_ = [](std::int64_t p) { return noiseAt(p); };
    std::vector<float> left_;
    std::vector<float> right_;
    std::vector<std::uint8_t> valid_;
};

// 时间模式、D = samples（48 kHz 下要取 48 的整数倍，毫秒才是整数）
inline EngineParams timeParams(std::int64_t samples, double sampleRate = 48000.0)
{
    EngineParams params;
    params.delayMs = static_cast<float>(static_cast<double>(samples) * 1000.0 / sampleRate);
    return params;
}

} // namespace rinne::timing_test
