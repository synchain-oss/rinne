// SPDX-License-Identifier: GPL-3.0-or-later
//
// 传输环的协议：几何、块长校验、跨多圈的写入与读回、窗口两端的清零、拷贝途中被覆盖时 validFrom 的判定、
// intent 只进不退（两个写方交错时恰好一个拿到写头）、写方格式（epoch、writer_token、sample_rate）的发布与交还，
// 以及离 0 很远的位置上的读写。环用普通内存里的 RingSegment，不碰命名段：这些都是纯内存序与算术。
//
// 每个样本编码自己的绝对位置：L = p & 0xFFFFFF、R = (p >> 24) & 0xFFFFFF，读回来的值能直接对上是第几帧，
// 所以「读到了别的帧」不会被当成正确的数据。

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "ipc/LoopRing.h"

namespace
{

using namespace rinne::ipc;

// 位置 → 左右样本：24 位一截，float 能逐位精确表示
float encodeL(u64 p) noexcept
{
    return static_cast<float>(p & 0xFFFFFFu);
}

float encodeR(u64 p) noexcept
{
    return static_cast<float>((p >> 24) & 0xFFFFFFu);
}

// 读回来的值必须是这一帧的编码；解不出来（非整数、超出 24 位）返回 false
bool decode(u64 p, float l, float r) noexcept
{
    const auto el = static_cast<std::uint32_t>(l);
    const auto er = static_cast<std::uint32_t>(r);
    return static_cast<float>(el) == l && static_cast<float>(er) == r && el == (p & 0xFFFFFFu) &&
           er == ((p >> 24) & 0xFFFFFFu);
}

// 在写头处写 frames 帧，每帧编码自己的绝对位置；pos 是调用方以为的写头，必须对得上
void writeAt(RingSegment& seg, u64 pos, u32 frames)
{
    REQUIRE(LoopRing::writeHead(seg) == pos);
    const RingWriteResult result = LoopRing::write(seg, frames, [](u32, u64 p, float& l, float& r) {
        l = encodeL(p);
        r = encodeR(p);
    });
    REQUIRE(result == RingWriteResult::kOk);
}

std::unique_ptr<RingSegment> makeRing()
{
    auto seg = std::make_unique<RingSegment>(); // 值初始化 = 全零
    LoopRing::initGeometry(*seg);
    return seg;
}

} // namespace

TEST_CASE("geometry is published by initGeometry and checked by geometryOk", "[ipc][ring]")
{
    auto seg = std::make_unique<RingSegment>();
    CHECK_FALSE(LoopRing::geometryOk(*seg));
    LoopRing::initGeometry(*seg);
    CHECK(LoopRing::geometryOk(*seg));
    CHECK(seg->header.ring_frames.load() == kRingFrames);
    CHECK(seg->header.ring_channels.load() == kRingChannels);
}

TEST_CASE("a zero or oversized chunk is rejected without touching the segment", "[ipc][ring]")
{
    auto seg = makeRing();
    seg->header.write_head.store(1234);
    seg->header.write_intent.store(1234);
    seg->data[0].store(1.0f);
    seg->data[1].store(2.0f);

    for (const u32 frames : {0u, kMaxWriteChunk + 1})
    {
        const RingWriteResult result = LoopRing::write(*seg, frames, [](u32, u64, float& l, float& r) {
            l = 9.0f;
            r = 9.0f;
        });
        CHECK(result == RingWriteResult::kBadLength);
        CHECK(seg->header.write_head.load() == 1234u);
        CHECK(seg->header.write_intent.load() == 1234u);
        CHECK(seg->header.w_cas_fail.load() == 0u);
        CHECK(seg->data[0].load() == 1.0f);
        CHECK(seg->data[1].load() == 2.0f);
    }
}

TEST_CASE("chunks totalling more than three laps read back at their own positions", "[ipc][ring]")
{
    auto seg = makeRing();
    constexpr u32 kChunks[] = {1, 7, 8192, 333, 4096, 1, 8191};
    constexpr u64 kTotal = 2ull * kRingFrames + 4096;
    u64 pos = 0;
    std::unique_ptr<float[]> left(new float[kRingFrames]);
    std::unique_ptr<float[]> right(new float[kRingFrames]);

    for (std::size_t chunk = 0; pos < kTotal; ++chunk)
    {
        const u32 k = kChunks[chunk % (sizeof kChunks / sizeof kChunks[0])];
        writeAt(*seg, pos, k);
        pos += k;
        REQUIRE(LoopRing::writeHead(*seg) == pos);

        // 最近的一块与整整一圈都必须逐帧对上：最近的一块是刚发布的，整圈是这一圈里所有的历史。
        // 第一圈没写满时整圈窗口里有负位置，那种窗口由下一个用例专门核
        for (const u32 window : {k, kRingFrames})
        {
            if (window > pos)
            {
                continue;
            }
            INFO("pos " << pos << " window " << window);
            const RingRead r = LoopRing::read(*seg, static_cast<i64>(pos - window), window, left.get(), right.get());
            CHECK(r.writeHead == pos);
            CHECK(r.validFrom == static_cast<i64>(pos - window));
            CHECK(r.validTo == static_cast<i64>(pos));
            // 逐帧核对只计数、最后断言一次：每帧一条断言会让 sanitizer 构建慢上几个数量级
            u32 wrong = 0;
            for (u32 i = 0; i < window; ++i)
            {
                wrong += decode(pos - window + i, left[i], right[i]) ? 0u : 1u;
            }
            REQUIRE(wrong == 0u);
        }
    }
}

TEST_CASE("frames outside the ring window are zeroed and reported as invalid", "[ipc][ring]")
{
    auto seg = makeRing();
    const u64 w = 3ull * kRingFrames;
    // 写满三圈：窗口左端早于 W - kRingFrames 的那 10 帧早已被覆盖，必须按 0 交出
    for (u64 at = 0; at < w; at += 4096)
    {
        writeAt(*seg, at, 4096);
    }
    REQUIRE(LoopRing::writeHead(*seg) == w);

    const i64 from = static_cast<i64>(w) - static_cast<i64>(kRingFrames) - 10;
    const u32 frames = kRingFrames + 20;
    std::unique_ptr<float[]> left(new float[frames]);
    std::unique_ptr<float[]> right(new float[frames]);
    const RingRead r = LoopRing::read(*seg, from, frames, left.get(), right.get());

    CHECK(r.writeHead == w);
    CHECK(r.validFrom == static_cast<i64>(w) - static_cast<i64>(kRingFrames));
    CHECK(r.validTo == static_cast<i64>(w));
    for (u32 i = 0; i < frames; ++i)
    {
        const i64 p = from + static_cast<i64>(i);
        INFO("frame " << p);
        if (p >= static_cast<i64>(w) - static_cast<i64>(kRingFrames) && p < static_cast<i64>(w))
        {
            CHECK(decode(static_cast<u64>(p), left[i], right[i]));
        }
        else
        {
            CHECK(left[i] == 0.0f);
            CHECK(right[i] == 0.0f);
        }
    }

    // 整个窗口都早于 W - kRingFrames：有效区间为空（落在 to 上），输出全 0
    const RingRead gone = LoopRing::read(*seg, -10, 16, left.get(), right.get());
    CHECK(gone.validFrom == 6);
    CHECK(gone.validTo == 6);
    for (u32 i = 0; i < 16; ++i)
    {
        CHECK(left[i] == 0.0f);
        CHECK(right[i] == 0.0f);
    }

    // 负位置：第一圈里从 -10 起读，前 10 帧从没存在过，按 0 交出，其余是自己的值
    auto fresh = makeRing();
    writeAt(*fresh, 0, 4096);
    const RingRead neg = LoopRing::read(*fresh, -10, 16, left.get(), right.get());
    CHECK(neg.validFrom == 0);
    CHECK(neg.validTo == 6);
    for (u32 i = 0; i < 16; ++i)
    {
        const i64 p = -10 + static_cast<i64>(i);
        if (p < 0)
        {
            CHECK(left[i] == 0.0f);
            CHECK(right[i] == 0.0f);
        }
        else
        {
            CHECK(decode(static_cast<u64>(p), left[i], right[i]));
        }
    }
}

TEST_CASE("a copy raced by an overwriting write reports the overwritten frames through validFrom", "[ipc][ring]")
{
    auto seg = makeRing();
    const u64 w = 2ull * kRingFrames;
    for (u64 at = 0; at < w; at += 4096)
    {
        writeAt(*seg, at, 4096);
    }
    REQUIRE(LoopRing::writeHead(*seg) == w);

    const i64 from = static_cast<i64>(w) - static_cast<i64>(kRingFrames);
    const i64 to = static_cast<i64>(w);
    std::unique_ptr<float[]> left(new float[kRingFrames]);
    std::unique_ptr<float[]> right(new float[kRingFrames]);
    bool raced = false;
    const RingPull pulled = LoopRing::pull(*seg, from, to, [&](i64 p, float l, float r) {
        if (!raced)
        {
            // 第一次回调（第 from 帧已经读完）里同一线程再写 100 帧：它覆盖拷贝窗口最前面的 100 个槽，
            // 于是 from + 1 起的 99 帧拿到的是新一圈的值
            raced = true;
            writeAt(*seg, w, 100);
        }
        left[static_cast<std::size_t>(p - from)] = l;
        right[static_cast<std::size_t>(p - from)] = r;
    });
    REQUIRE(raced);
    CHECK(pulled.intent == static_cast<i64>(w) + 100);
    CHECK(pulled.validFrom == from + 100);
    // 判为有效的帧都还是自己的值；被排除的那一段里确实有新一圈的值，说明这一用例真的撞上了覆盖
    for (i64 p = pulled.validFrom; p < to; ++p)
    {
        INFO("frame " << p);
        REQUIRE(decode(static_cast<u64>(p), left[static_cast<std::size_t>(p - from)],
                       right[static_cast<std::size_t>(p - from)]));
    }
    CHECK_FALSE(decode(static_cast<u64>(from + 1), left[1], right[1]));
    CHECK(decode(static_cast<u64>(from + 1) + kRingFrames, left[1], right[1]));
}

TEST_CASE("a second writer stays out while a chunk is in flight", "[ipc][ring]")
{
    auto seg = makeRing();
    const u64 w0 = 777;
    seg->header.write_head.store(w0);
    seg->header.write_intent.store(w0);
    std::unique_ptr<float[]> left(new float[600]);
    std::unique_ptr<float[]> right(new float[600]);

    // 外层 500 帧公告之后、第一次填样本时，嵌套一个第二写方：它看见 intent 领先 W，什么都不写
    for (const u32 nestedFrames : {50u, 500u})
    {
        INFO("nested " << nestedFrames);
        const u64 start = LoopRing::writeHead(*seg);
        RingWriteResult nested = RingWriteResult::kOk;
        const RingWriteResult outer = LoopRing::write(*seg, 500, [&](u32 i, u64 p, float& l, float& r) {
            if (i == 0)
            {
                nested = LoopRing::write(*seg, nestedFrames, [](u32, u64, float& nl, float& nr) {
                    nl = 1.0f;
                    nr = 1.0f;
                });
            }
            l = encodeL(p);
            r = encodeR(p);
        });
        CHECK(nested == RingWriteResult::kInFlight);
        CHECK(outer == RingWriteResult::kOk);
        CHECK(seg->header.w_cas_fail.load() == 0u);
        CHECK(LoopRing::writeHead(*seg) == start + 500);
        CHECK(LoopRing::writeIntent(*seg) == start + 500);
        // 发布出去的每一帧都是外层自己的值：第二写方一个样本都没动
        const RingRead r = LoopRing::read(*seg, static_cast<i64>(start), 500, left.get(), right.get());
        CHECK(r.validFrom == static_cast<i64>(start));
        CHECK(r.validTo == static_cast<i64>(start + 500));
        u32 wrong = 0;
        for (u32 i = 0; i < 500; ++i)
        {
            wrong += decode(start + i, left[i], right[i]) ? 0u : 1u;
        }
        CHECK(wrong == 0u);
    }
}

TEST_CASE("a chunk abandoned after its announcement blocks writers until it is reclaimed", "[ipc][ring]")
{
    auto seg = makeRing();
    const u64 w0 = 5000;
    // 一个写方公告了 [w0, w0 + 300)、写了一半样本就死了：intent 停在 W 之前，槽里是半份垃圾
    seg->header.write_head.store(w0);
    seg->header.write_intent.store(w0 + 300);
    for (u64 p = w0; p < w0 + 150; ++p)
    {
        seg->data[2 * (p & (kRingFrames - 1))].store(-7.0f);
        seg->data[2 * (p & (kRingFrames - 1)) + 1].store(-7.0f);
    }
    const auto encode = [](u32, u64 p, float& l, float& r) {
        l = encodeL(p);
        r = encodeR(p);
    };

    // 采样率 1000 Hz：挡满 1000 帧才回收。前 9 次每次 100 帧都被挡住，什么都没写
    RingWriter writer;
    writer.reset(1000);
    for (int attempt = 0; attempt < 9; ++attempt)
    {
        CHECK(writer.write(*seg, 100, encode) == RingWriteResult::kInFlight);
    }
    CHECK(writer.stalledFrames() == 900u);
    CHECK(writer.reclaims() == 0u);
    CHECK(LoopRing::writeHead(*seg) == w0);
    CHECK(seg->data[2 * (w0 & (kRingFrames - 1))].load() == -7.0f);

    // 第 10 次挡满 1000 帧：卡住的那一段填零发布，写头推到 intent
    CHECK(writer.write(*seg, 100, encode) == RingWriteResult::kInFlight);
    CHECK(writer.reclaims() == 1u);
    CHECK(writer.stalledFrames() == 0u);
    CHECK(LoopRing::writeHead(*seg) == w0 + 300);
    CHECK(LoopRing::writeIntent(*seg) == w0 + 300);
    std::unique_ptr<float[]> left(new float[400]);
    std::unique_ptr<float[]> right(new float[400]);
    const RingRead zeros = LoopRing::read(*seg, static_cast<i64>(w0), 300, left.get(), right.get());
    CHECK(zeros.validFrom == static_cast<i64>(w0));
    CHECK(zeros.validTo == static_cast<i64>(w0 + 300));
    u32 nonZero = 0;
    for (u32 i = 0; i < 300; ++i)
    {
        nonZero += (left[i] != 0.0f || right[i] != 0.0f) ? 1u : 0u;
    }
    CHECK(nonZero == 0u);

    // 之后照常写；没有卡住的段时回收是空操作
    CHECK(writer.write(*seg, 100, encode) == RingWriteResult::kOk);
    CHECK(LoopRing::writeHead(*seg) == w0 + 400);
    CHECK_FALSE(LoopRing::reclaimStalled(*seg));
    CHECK(LoopRing::writeHead(*seg) == w0 + 400);
    const RingRead fresh = LoopRing::read(*seg, static_cast<i64>(w0 + 300), 100, left.get(), right.get());
    CHECK(fresh.validFrom == static_cast<i64>(w0 + 300));
    u32 wrong = 0;
    for (u32 i = 0; i < 100; ++i)
    {
        wrong += decode(w0 + 300 + i, left[i], right[i]) ? 0u : 1u;
    }
    CHECK(wrong == 0u);
}

TEST_CASE("reclaiming a stall more than a lap ahead only zeroes the last lap", "[ipc][ring]")
{
    auto seg = makeRing();
    const u64 w0 = 3ull * kRingFrames;
    const u64 intent = w0 + kRingFrames + 500;
    seg->header.write_head.store(w0);
    seg->header.write_intent.store(intent);
    for (u32 i = 0; i < 2 * kRingFrames; ++i)
    {
        seg->data[i].store(-3.0f);
    }
    REQUIRE(LoopRing::reclaimStalled(*seg));
    CHECK(LoopRing::writeHead(*seg) == intent);
    u32 nonZero = 0;
    for (u32 i = 0; i < 2 * kRingFrames; ++i)
    {
        nonZero += seg->data[i].load() != 0.0f ? 1u : 0u;
    }
    CHECK(nonZero == 0u);

    // RingWriter 在写成功之后清零计数；采样率 0 按 48 kHz 计
    RingWriter writer;
    writer.reset(0);
    CHECK(writer.write(*seg, 10, [](u32, u64, float& l, float& r) { l = r = 0.5f; }) == RingWriteResult::kOk);
    CHECK(writer.stalledFrames() == 0u);
    seg->header.write_intent.store(LoopRing::writeHead(*seg) + 10);
    for (int attempt = 0; attempt < 4799; ++attempt)
    {
        REQUIRE(writer.write(*seg, 10, [](u32, u64, float& l, float& r) { l = r = 0.5f; }) ==
                RingWriteResult::kInFlight);
    }
    CHECK(writer.reclaims() == 0u);
    CHECK(writer.write(*seg, 10, [](u32, u64, float& l, float& r) { l = r = 0.5f; }) == RingWriteResult::kInFlight);
    CHECK(writer.reclaims() == 1u);
}

TEST_CASE("the writer publishes its format with a rising epoch and retires only its own token", "[ipc][ring]")
{
    auto seg = makeRing();
    seg->header.write_head.store(4321);
    seg->header.write_intent.store(4321);

    // 从没发布过：epoch 0 → 1
    LoopRing::publishWriter(*seg, 1, 48000, 2);
    RingFormat f = LoopRing::readFormat(*seg);
    CHECK(f.epoch == 1u);
    CHECK(f.writerToken == 1u);
    CHECK(f.sampleRate == 48000u);
    CHECK(LoopRing::isWriter(*seg, 1));
    CHECK(seg->header.src_channels.load() == 2u);

    // 采样率没变：epoch 不动，只换写权与源声道数
    LoopRing::publishWriter(*seg, 2, 48000, 1);
    f = LoopRing::readFormat(*seg);
    CHECK(f.epoch == 1u);
    CHECK(f.writerToken == 2u);
    CHECK(f.sampleRate == 48000u);
    CHECK(seg->header.src_channels.load() == 1u);

    // 采样率变了：epoch + 1
    LoopRing::publishWriter(*seg, 2, 44100, 2);
    f = LoopRing::readFormat(*seg);
    CHECK(f.epoch == 2u);
    CHECK(f.writerToken == 2u);
    CHECK(f.sampleRate == 44100u);

    // token 0 什么都不做
    LoopRing::publishWriter(*seg, 0, 96000, 1);
    f = LoopRing::readFormat(*seg);
    CHECK(f.epoch == 2u);
    CHECK(f.writerToken == 2u);
    CHECK(f.sampleRate == 44100u);

    CHECK_FALSE(LoopRing::isWriter(*seg, 1));
    CHECK(LoopRing::isWriter(*seg, 2));
    CHECK_FALSE(LoopRing::isWriter(*seg, 0));
    CHECK_FALSE(LoopRing::retireWriter(*seg, 1));
    CHECK(seg->header.writer_token.load() == 2u);
    CHECK(LoopRing::retireWriter(*seg, 2));
    CHECK(seg->header.writer_token.load() == 0u);
    CHECK_FALSE(LoopRing::isWriter(*seg, 2));

    // 发布与交还从不碰写头与公告（「W 不清零」）
    CHECK(seg->header.write_head.load() == 4321u);
    CHECK(seg->header.write_intent.load() == 4321u);
}

TEST_CASE("positions far from zero survive the wrap arithmetic", "[ipc][ring]")
{
    auto seg = makeRing();
    const u64 start = (1ull << 40) - 10;
    seg->header.write_head.store(start);
    seg->header.write_intent.store(start);
    writeAt(*seg, start, 30);
    REQUIRE(LoopRing::writeHead(*seg) == start + 30);

    std::unique_ptr<float[]> left(new float[30]);
    std::unique_ptr<float[]> right(new float[30]);
    const RingRead r = LoopRing::read(*seg, static_cast<i64>(start), 30, left.get(), right.get());
    CHECK(r.writeHead == start + 30);
    CHECK(r.validFrom == static_cast<i64>(start));
    CHECK(r.validTo == static_cast<i64>(start + 30));
    for (u32 i = 0; i < 30; ++i)
    {
        REQUIRE(decode(start + i, left[i], right[i]));
    }

    CHECK(frameDiff(5, 7) == -2);
    CHECK(frameDiff(7, 5) == 2);
}
