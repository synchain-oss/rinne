// SPDX-License-Identifier: GPL-3.0-or-later
//
// 环路层门面（LoopSession）的验收落点：
//   - W = 0 的新段：读位置为负时读成 0，不越界。
//   - W ≈ 2^40：配对时锚在写头，读头读到的正是 D 帧之前写下的那一帧，漂移量为 0。
//   - from > pulled：读位置超出已拉取区间时读成 0，按补 0 计数，逐样本有效位为 0。
//   - 宿主违约：块长超过声明值时 D 下限立即抬到块长，原因记成宿主违约。
//   - 错序判据：宿主时间戳有效时连续 3 次判为「Output 在前」才定案；无效时恒为 unknown。
//   - 对端没了：读头 10 ms 淡出，读位照常前进，之后回送为 0。
//   - 起播沿的复位点：给的是绝对样本号。
//
// 这些是验收 1 与验收 2 在「环路层门面」这一层的落点：判据那一半（StreamSync）另有自己的用例。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "engine/EngineParams.h"
#include "engine/TransportInfo.h"
#include "ipc/LoopBinding.h"
#include "ipc/LoopRing.h"
#include "loop/LoopSession.h"
#include "loop/LoopStatus.h"

namespace
{

using namespace rinne::loop;
using namespace rinne::engine;
namespace ipc = rinne::ipc;

constexpr double kSr = 48000.0;

// 逐样本有效位的两个取值。
constexpr std::uint8_t kInvalidSample = 0; // 这一格是补出来的 0
constexpr std::uint8_t kValidSample = 1; // 这一格是环里的真数据

// 混音缓冲的容量：容得下支持的最大块长（环路层按块长写满）。
constexpr std::size_t kMixCapacity = static_cast<std::size_t>(ipc::kMaxSupportedBlock) + 8u;

// 环里每一帧的值：按位置编码，读回来能对上第几帧（值都在 [0, 1) 里，float 精确表示）。
float encode(std::int64_t p) noexcept
{
    return static_cast<float>(static_cast<std::uint64_t>(p) & 0xFFFFu) / 65536.0f;
}

// 一个驱动器：包住 LoopSession，省掉每条用例的样板。
class Session
{
public:
    Session()
    {
        // 缓冲按支持的最大块长给：环路层混音时按块长写满，不能给小了。
        mixLeft_.assign(kMixCapacity, 0.0f);
        mixRight_.assign(kMixCapacity, 0.0f);
        mixValid_.assign(kMixCapacity, kInvalidSample);
        REQUIRE(session_.prepare(kSr));
    }

    // 推进一块。返回这一块的结果（回送在 left() / valid() 里）。
    BlockResult feed(int blockLength, const TransportInfo& transport = TransportInfo{},
                     const EngineParams& params = EngineParams{})
    {
        BlockContext ctx(params, transport);
        ctx.ring = ring_;
        ctx.rendezvous = rv_;
        ctx.group = rv_ != nullptr ? 1u : 0u;
        ctx.role = ipc::Role::kInput;
        ctx.declaredBlock = declaredBlock_;
        ctx.observedBlock = declaredBlock_;
        ctx.blockLength = blockLength;
        ctx.returnLeft = mixLeft_.data();
        ctx.returnRight = mixRight_.data();
        ctx.returnValid = mixValid_.data();
        return session_.process(ctx);
    }

    // 接上一个环：发布写权。
    void attachRing(ipc::RingSegment& ring, std::uint64_t token)
    {
        ring_ = &ring;
        ipc::LoopRing::publishWriter(*ring_, token, static_cast<std::uint32_t>(kSr), 2);
    }
    void attachRendezvous(ipc::RvSegment& rv) { rv_ = &rv; }
    // 段没了（换组、断开）：之后的块不给环。
    void detachRing() { ring_ = nullptr; }
    // 本端声明的最大块长（N_prep）。默认 1024。
    void setDeclaredBlock(std::int64_t n) { declaredBlock_ = n; }

    const float* left() const { return mixLeft_.data(); }
    const std::uint8_t* valid() const { return mixValid_.data(); }
    LoopStatusFields status() const { return session_.statusFields(); }
    LoopSession& session() { return session_; }

private:
    LoopSession session_;
    ipc::RingSegment* ring_ = nullptr;
    ipc::RvSegment* rv_ = nullptr;
    std::int64_t declaredBlock_ = 1024;
    std::vector<float> mixLeft_;
    std::vector<float> mixRight_;
    std::vector<std::uint8_t> mixValid_;
};

// 一个干净的环。
std::unique_ptr<ipc::RingSegment> makeRing()
{
    auto seg = std::make_unique<ipc::RingSegment>();
    ipc::LoopRing::initGeometry(*seg);
    return seg;
}

// 往环里写 frames 帧（≤ 8192）：每帧的值按它的绝对位置编码。
void writeFrames(ipc::RingSegment& ring, std::uint32_t frames)
{
    const ipc::RingWriteResult result =
        ipc::LoopRing::write(ring, frames, [](std::uint32_t, std::uint64_t p, float& l, float& r) {
            l = encode(static_cast<std::int64_t>(p));
            r = encode(static_cast<std::int64_t>(p));
        });
    REQUIRE(result == ipc::RingWriteResult::kOk);
}

// D 取最小（1 ms 会被抬到声明值 1024）：几何最好算。
EngineParams minimalDelay()
{
    EngineParams params;
    params.delayMs = 1.0f;
    return params;
}
} // namespace

TEST_CASE("环路 验收 1：W = 0 的新段，读位置为负时读成 0，不越界")
{
    Session s;
    auto ring = makeRing();
    s.attachRing(*ring, 1);

    // W = 0：读头读的位置是 R + off − D = −1024 起，全部为负 → 全是 0，全部无效。
    const BlockResult result = s.feed(64);
    CHECK(result.returnLength == 64);
    for (int i = 0; i < 64; ++i)
    {
        CHECK(result.returnLeft[i] == 0.0f);
        CHECK(result.returnValid[i] == kInvalidSample);
    }
    CHECK(s.status().paired == 1u);
}

TEST_CASE("环路 验收 1：W ≈ 2^40，配对时锚在写头，读到的正是 D 帧之前写下的那一帧")
{
    // ⛔ 删除式验证点：配对时不把本地延迟线锚在写头（从 0 起拉）→ 回送永远是 0 → 本用例变红。
    const std::int64_t base = (static_cast<std::int64_t>(1) << 40) - 300; // 跨过 2^40
    auto ring = makeRing();
    ring->header.write_head.store(static_cast<std::uint64_t>(base));
    ring->header.write_intent.store(static_cast<std::uint64_t>(base));

    Session s;
    s.attachRing(*ring, 1);
    const EngineParams params = minimalDelay(); // D = 1024

    // Input 在前、Output 在后：每块 Input 先读，Output 再写 256 帧。
    std::vector<float> out;
    std::vector<std::uint8_t> valid;
    for (int block = 0; block < 12; ++block)
    {
        const BlockResult result = s.feed(256, TransportInfo{}, params);
        out.insert(out.end(), s.left(), s.left() + 256);
        valid.insert(valid.end(), s.valid(), s.valid() + 256);
        CHECK(result.overrun == 0u);
        writeFrames(*ring, 256);
    }
    // 输出下标 t 读环位置 base + t − D：t < D 时位置在配对之前，读成 0 且无效；之后逐位对得上。
    for (int t = 0; t < 12 * 256; ++t)
    {
        if (t < 1024)
        {
            CHECK(out[static_cast<std::size_t>(t)] == 0.0f);
            CHECK(valid[static_cast<std::size_t>(t)] == kInvalidSample);
        }
        else
        {
            // 新读头的 onset（20 ms = 960 帧）从第一帧有效数据起淡入，之后增益恰为 1。
            const std::int64_t p = base + t - 1024;
            CHECK(valid[static_cast<std::size_t>(t)] == kValidSample);
            if (t >= 1024 + 960)
            {
                CHECK(out[static_cast<std::size_t>(t)] == encode(p));
            }
        }
    }
    const LoopStatusFields fields = s.status();
    CHECK(fields.drift == 0);
    CHECK(fields.offset == base);
    CHECK(fields.writeHead == base + 11 * 256);
    CHECK(fields.overflow == 0u);
    // 漂移量的有符号运算在这么远的位置上也对。
    CHECK(drift(base, 0, 0) == base);
    CHECK(drift(0, base, 0) == -base);
    CHECK(drift(base - 100, base, 0) == -100);
}

TEST_CASE("环路 验收 1：from > pulled，读位置超出已拉取区间时读成 0，计补 0，有效位为 0")
{
    // 写方这一块只写了一半：主读头读到的后一半位置还没写到 → 补 0。
    auto ring = makeRing();
    Session s;
    s.attachRing(*ring, 1);
    const EngineParams params = minimalDelay(); // D = 1024

    writeFrames(*ring, 1024);
    s.feed(1024, TransportInfo{}, params); // 配对：锚在 W = 1024，R 走到 1024
    writeFrames(*ring, 512); // 这一周期写方只推进了 512 帧（W = 1536）

    // 这一块读的位置是 1024 + i（i = 0..1023）；已拉到 1536，所以后 512 个读不到。
    const BlockResult result = s.feed(1024, TransportInfo{}, params);
    CHECK_FALSE(result.resynced); // 第二块没有对齐基准，小漂移先吸收
    CHECK(result.overrun == 512u);
    for (int i = 0; i < 512; i += 37)
    {
        CHECK(result.returnValid[i] == kValidSample);
    }
    for (int i = 512; i < 1024; i += 37)
    {
        CHECK(result.returnLeft[i] == 0.0f);
        CHECK(result.returnValid[i] == kInvalidSample);
    }
    CHECK(s.status().overrun == 512u);
    CHECK(s.status().drift == -512);
}

TEST_CASE("环路 错序判据：宿主时间戳无效时恒为 unknown，票数为 0")
{
    Session s;
    TransportInfo transport;
    transport.hostNsValid = false;
    transport.hostNs = 0;

    for (int i = 0; i < 5; ++i)
    {
        s.feed(64, transport);
    }
    CHECK(s.status().orderVerdict == static_cast<std::uint8_t>(OrderVerdict::kUnknown));
    CHECK(s.status().orderVotes == 0u);
    CHECK(s.status().hostNsValid == 0u);
}

TEST_CASE("环路 错序判据：读不到对端槽时不猜，恒为 unknown")
{
    Session s;
    // 没有会合段 → 读不到对端槽 → 判据没有依据。
    TransportInfo transport;
    transport.hostNsValid = true;
    transport.hostNs = 1000;

    for (int i = 0; i < 5; ++i)
    {
        s.feed(64, transport);
    }
    CHECK(s.status().orderVerdict == static_cast<std::uint8_t>(OrderVerdict::kUnknown));
    CHECK(s.status().orderVotes == 0u);
    CHECK(s.status().hostNsValid == 1u); // 宿主给了时间戳，只是对端槽读不到
}

TEST_CASE("环路 错序判据：对端时间戳不小于本块的连续 3 次才定案，判对一次就清零")
{
    // ⛔ 删除式验证点：把判据写反（对端时间戳更小就记一票）→ 本用例变红。
    auto rv = std::make_unique<ipc::RvSegment>();
    Session s;
    s.attachRendezvous(*rv);
    ipc::RoleSlot& output = ipc::roleSlot(*rv, 1, ipc::Role::kOutput);

    TransportInfo transport;
    transport.hostNsValid = true;
    std::uint64_t ns = 5000;

    // Output 已经处理过本块（它的时间戳等于本块的）：一票、两票、三票才定案。
    for (int vote = 1; vote <= 3; ++vote)
    {
        transport.hostNs = ns;
        output.last_host_ns.store(ns);
        s.feed(64, transport);
        CHECK(s.status().orderVotes == static_cast<std::uint32_t>(vote));
        CHECK(s.status().orderVerdict == (vote < 3 ? static_cast<std::uint8_t>(OrderVerdict::kUnknown)
                                                   : static_cast<std::uint8_t>(OrderVerdict::kOutputFirst)));
        ns += 1000;
    }

    // Output 还停在上一块（它的时间戳更小）：本块里 Input 先被处理 → 判为正确，票数清零。
    transport.hostNs = ns;
    output.last_host_ns.store(ns - 1000);
    s.feed(64, transport);
    CHECK(s.status().orderVerdict == static_cast<std::uint8_t>(OrderVerdict::kCorrect));
    CHECK(s.status().orderVotes == 0u);

    // 对端还没发布过时间戳（0）：不猜。
    output.last_host_ns.store(0);
    s.feed(64, transport);
    CHECK(s.status().orderVerdict == static_cast<std::uint8_t>(OrderVerdict::kUnknown));
}

TEST_CASE("环路 当前的几何量：m / w / off 逐块发布，用的是块首的读位")
{
    Session s;
    auto ring = makeRing();
    s.attachRing(*ring, 1);
    writeFrames(*ring, 512);

    s.feed(64);
    const LoopStatusFields fields = s.status();
    // 写头：环里的 W。
    CHECK(fields.writeHead == 512);
    // 偏移：配对时锚在 W − R（R = 0）。
    CHECK(fields.offset == 512);
    // 漂移量：m = W − (R + off)，R 是本块的读位起点 0。
    CHECK(fields.drift == fields.writeHead - (0 + fields.offset));
    CHECK(fields.drift == 0);
    CHECK(s.session().processed() == 64);
}

TEST_CASE("环路 0 样本的调用立刻返回，不推进任何计数")
{
    Session s;
    const BlockResult result = s.feed(0);
    CHECK(result.returnLength == 0);
    CHECK(result.overrun == 0u);
    CHECK_FALSE(result.hostViolation);
    CHECK_FALSE(result.unsupportedBlock);
    CHECK(s.session().processed() == 0);
}

TEST_CASE("环路 宿主违约：块长超过声明值时 D 下限立即抬到块长，原因记成宿主违约")
{
    // ⛔ 删除式验证点：违约那一块不把块长算进 N_obs（等引擎下一块再报）→ 本用例变红。
    Session s;
    auto ring = makeRing();
    s.attachRing(*ring, 1);
    writeFrames(*ring, 64);

    // 声明值 1024，送来 2048 帧：违约。引擎报的 N_obs 还是 1024。
    const BlockResult result = s.feed(2048, TransportInfo{}, minimalDelay());
    CHECK(result.hostViolation);
    CHECK(result.delayMin == 2048);
    CHECK(result.delayEffective == 2048);
    const LoopStatusFields fields = s.status();
    CHECK(fields.delayMin == 2048);
    CHECK(fields.maxBlockSeen == 2048);
    CHECK(fields.delayMinReason == static_cast<std::uint8_t>(DelayMinReason::kHostViolation));
}

TEST_CASE("环路 块长超限（n > 32768）：只出干声并给提示码")
{
    Session s;
    const BlockResult result = s.feed(32769);
    CHECK(result.unsupportedBlock);
    CHECK(result.returnLength == 0);
}

TEST_CASE("环路 对端没了：读头 10 ms 淡出，读位照常前进，之后回送为 0")
{
    auto ring = makeRing();
    Session s;
    s.attachRing(*ring, 1);
    const EngineParams params = minimalDelay();

    for (int block = 0; block < 12; ++block)
    {
        s.feed(256, TransportInfo{}, params);
        writeFrames(*ring, 256);
    }
    REQUIRE(s.status().tapsActive == 1u);
    const std::int64_t before = s.session().processed();

    // 写方退出（写权交出去）：没有对端了。
    REQUIRE(ipc::LoopRing::retireWriter(*ring, 1));
    s.feed(256, TransportInfo{}, params);
    CHECK(s.status().paired == 0u);
    CHECK(s.session().processed() == before + 256); // 读位照常前进
    s.feed(256, TransportInfo{}, params); // 480 帧之内淡完
    const BlockResult quiet = s.feed(256, TransportInfo{}, params);
    for (int i = 0; i < 256; ++i)
    {
        CHECK(quiet.returnLeft[i] == 0.0f);
    }
    CHECK(s.status().tapsActive == 0u);
}

TEST_CASE("环路 起播沿：复位点给的是绝对样本号")
{
    Session s;
    auto ring = makeRing();
    s.attachRing(*ring, 1);
    writeFrames(*ring, 1024);

    TransportInfo transport;
    transport.isPlaying = false;
    s.feed(64, transport); // 还没走带

    transport.isPlaying = true;
    const BlockResult edge = s.feed(64, transport); // 起播沿：块首 R = 64
    // 复位点：起播沿的块首 + min(5 ms, D_eff)。沿上就给出（预告）。
    CHECK(edge.resynced);
    CHECK(edge.blockStart == 64);
    CHECK(edge.resetAtSample == 64 + static_cast<std::int64_t>(kSr * 0.005));
    CHECK_FALSE(edge.resetDsp); // 复位点不在这一块里

    // 复位点所在的那一块再给一次并置 resetDsp（与宿主怎么切块无关：给的是同一个绝对样本号）。
    bool seen = false;
    for (int block = 0; block < 8 && !seen; ++block)
    {
        const BlockResult r = s.feed(64, transport);
        if (r.resetDsp)
        {
            seen = true;
            CHECK(r.resetAtSample == 64 + static_cast<std::int64_t>(kSr * 0.005));
            CHECK(r.resetAtSample >= r.blockStart);
            CHECK(r.resetAtSample < r.blockStart + 64);
        }
    }
    CHECK(seen);
}

TEST_CASE("环路 块长超限的提示码只在进入超限时发一条，载荷是块长与上限（÷256）")
{
    // ⛔ 删除式验证点：超限每块都发一条 → 诊断队列被灌满，本用例变红。
    struct Log
    {
        std::vector<std::uint16_t> codes;
        std::vector<std::uint8_t> a;
        std::vector<std::uint8_t> b;
        static void sink(void* context, std::uint16_t code, std::uint8_t a, std::uint8_t b, std::int64_t) noexcept
        {
            auto* self = static_cast<Log*>(context);
            self->codes.push_back(code);
            self->a.push_back(a);
            self->b.push_back(b);
        }
    };
    Log log;
    Session s;
    s.session().setEventSink(LoopEventSink(&Log::sink, &log));
    for (int i = 0; i < 50; ++i)
    {
        CHECK(s.feed(40000).unsupportedBlock);
    }
    REQUIRE(log.codes.size() == 1u);
    CHECK(log.codes[0] == static_cast<std::uint16_t>(LoopEventCode::kUnsupportedBlock));
    CHECK(log.a[0] == eventUnits256(40000)); // 157
    CHECK(log.b[0] == 128u); // 32768 ÷ 256
    // 回到支持的块长、再超限：再发一条。
    s.feed(64);
    s.feed(40000);
    std::size_t unsupported = 0;
    for (const auto code : log.codes)
    {
        unsupported += code == static_cast<std::uint16_t>(LoopEventCode::kUnsupportedBlock) ? 1u : 0u;
    }
    CHECK(unsupported == 2u);
}

namespace
{
// 环里写 frames 帧常数（≤ 8192）：每帧左右都是 value。
void writeConstant(ipc::RingSegment& ring, std::uint32_t frames, float value)
{
    const ipc::RingWriteResult result =
        ipc::LoopRing::write(ring, frames, [=](std::uint32_t, std::uint64_t, float& l, float& r) {
            l = value;
            r = value;
        });
    REQUIRE(result == ipc::RingWriteResult::kOk);
}

// D 只比块长多一点（不到一个 5 ms 淡出）：块长 512、声明 512、D = 600 个样本（12.5 ms @ 48 kHz）。
// 上一块拉到的位置离旧读头只剩 D − n = 88 个样本：判据若拿它当淡出的读上界，淡出读到第 88 个样本就撞上去。
constexpr int kNearBlock = 512;
EngineParams nearBlockDelay()
{
    EngineParams params;
    params.delayMs = 12.5f;
    return params;
}

// 相邻两个回送样本之差的最大值（从 from 起）：淡出与 onset 都是线性斜坡，阶跃会让它一下子变大。
float maxStep(const std::vector<float>& trace, std::size_t from)
{
    float worst = 0.0f;
    for (std::size_t i = from + 1; i < trace.size(); ++i)
    {
        worst = std::max(worst, std::fabs(trace[i] - trace[i - 1]));
    }
    return worst;
}

// 读一块，把混好的回送接在轨迹后面。
void feedInto(Session& s, const EngineParams& params, std::vector<float>& trace)
{
    s.feed(kNearBlock, TransportInfo{}, params);
    trace.insert(trace.end(), s.left(), s.left() + kNearBlock);
}

// 稳态：Input 在前、写方在后，每块写 512 帧常数（默认 0.5）。跑够 D + onset（600 + 960 个样本）之后回送恒为这个值。
void runSteady(Session& s, ipc::RingSegment& ring, const EngineParams& params, std::vector<float>& trace, int blocks,
               float value = 0.5f)
{
    for (int block = 0; block < blocks; ++block)
    {
        feedInto(s, params, trace);
        writeConstant(ring, kNearBlock, value);
    }
}
} // namespace

TEST_CASE("环路 淡出读到的实际样本：D 只比上一块长多不到 5 ms 时，重同步（换写方）的淡出走满 5 ms、没有阶跃")
{
    // ⛔ 删除式验证点：重同步的淡出读上界改回「上一块拉到的位置」→ 淡出在第 88 个样本撞上去，本用例变红。
    auto ring = makeRing();
    Session s;
    s.setDeclaredBlock(kNearBlock);
    s.attachRing(*ring, 1);
    const EngineParams params = nearBlockDelay();
    std::vector<float> trace;
    runSteady(s, *ring, params, trace, 12);
    REQUIRE(s.status().delayEffective == 600);
    REQUIRE(trace.back() == 0.5f);
    const std::size_t mark = trace.size();

    // 换写方（同一个环上的接力）：新的写方接着写。
    ipc::LoopRing::publishWriter(*ring, 2, static_cast<std::uint32_t>(kSr), 2);
    runSteady(s, *ring, params, trace, 8);
    CHECK(s.status().resyncByReason[static_cast<std::size_t>(SyncReason::kWriter)] == 1u);

    // 旧读头从换写方那一块的第一个样本起线性淡出 240 个样本（5 ms），读到的是环里的真数据：0.5 × (240 − i) / 240。
    for (std::size_t i = 0; i < 240; i += 7)
    {
        CHECK(std::fabs(trace[mark + i] - 0.5f * static_cast<float>(240 - i) / 240.0f) < 1.0e-6f);
    }
    CHECK(trace[mark + 238] > 0.0f);
    CHECK(trace[mark + 240] == 0.0f);
    // 整段（淡出、空档、新数据 20 ms onset）没有阶跃：最大步长就是 5 ms 淡出的那一步。
    CHECK(maxStep(trace, mark - 1) <= 0.5f / 240.0f + 1.0e-6f);
    // 新数据接上了。
    CHECK(trace.back() == 0.5f);
}

TEST_CASE("环路 淡出读到的实际样本：对端走了、环还在时，10 ms 淡出读得到它最后写下的那一段，没有阶跃")
{
    // ⛔ 删除式验证点：对端走的那一块不再拉环（淡出读上界只剩上一块拉到的位置）→ 淡出只剩 88 个样本，本用例变红。
    auto ring = makeRing();
    Session s;
    s.setDeclaredBlock(kNearBlock);
    s.attachRing(*ring, 1);
    const EngineParams params = nearBlockDelay();
    std::vector<float> trace;
    runSteady(s, *ring, params, trace, 12);
    REQUIRE(trace.back() == 0.5f);

    // 写方写完最后一块之后交出写权：环还在，环里的帧都是它写完了的。
    REQUIRE(ipc::LoopRing::retireWriter(*ring, 1));
    const std::size_t mark = trace.size();
    for (int block = 0; block < 3; ++block)
    {
        feedInto(s, params, trace);
    }
    CHECK(s.status().paired == 0u);

    // 10 ms = 480 个样本的线性淡出，读到的是环里的真数据。
    CHECK(trace[mark] == 0.5f);
    CHECK(trace[mark + 478] > 0.0f);
    CHECK(trace[mark + 480] == 0.0f);
    CHECK(maxStep(trace, mark - 1) <= 0.5f / 480.0f + 1.0e-6f);
    CHECK(trace.back() == 0.0f);
}

TEST_CASE("环路 淡出读到的实际样本：段一起没了（只剩已拉进来的 88 个样本）时，淡出缩短到恰好用完，不以阶跃收尾")
{
    // ⛔ 删除式验证点：去掉「按剩下的样本缩短淡出」→ 10 ms 的淡出读到第 88 个样本撞上去，从 0.41 直接掉到
    // 0，本用例变红。
    auto ring = makeRing();
    Session s;
    s.setDeclaredBlock(kNearBlock);
    s.attachRing(*ring, 1);
    const EngineParams params = nearBlockDelay();
    std::vector<float> trace;
    runSteady(s, *ring, params, trace, 12);
    REQUIRE(trace.back() == 0.5f);

    s.detachRing(); // 换组或断开：这一块起不给环
    const std::size_t mark = trace.size();
    for (int block = 0; block < 2; ++block)
    {
        feedInto(s, params, trace);
    }
    CHECK(s.status().paired == 0u);
    // 读头从 R + off − D 起读，已拉进来的只剩 88 个样本：淡出 88 个样本，在第 88 个样本恰好到 0。
    CHECK(trace[mark] == 0.5f);
    CHECK(trace[mark + 87] > 0.0f);
    CHECK(trace[mark + 88] == 0.0f);
    CHECK(maxStep(trace, mark - 1) <= 0.5f / 88.0f + 1.0e-6f);
}

TEST_CASE("环路 换段（换组、重新映射）：旧读头只读旧段已经拉进来的数据淡出，新段从它自己的写头起重建读头")
{
    // ⛔ 删除式验证点：换了段照常把新段拉进旧的延迟线、旧读头淡出读到新段的帧 →
    // 淡出里混进新段的值（负数），本用例变红。
    auto a = makeRing();
    auto b = makeRing();
    Session s;
    s.setDeclaredBlock(kNearBlock);
    s.attachRing(*a, 1);
    const EngineParams params = nearBlockDelay();
    std::vector<float> trace;
    runSteady(s, *a, params, trace, 20); // 段 A：0.5
    REQUIRE(trace.back() == 0.5f);

    // 段 B 的写方早就在写 −0.5，写头比 A 已拉到的位置远一点（两条流的位置编号恰好相接，但那是两条流）。
    for (int block = 0; block < 25; ++block)
    {
        writeConstant(*b, kNearBlock, -0.5f);
    }
    s.attachRing(*b, 2); // 换组：这一块起给的是段 B
    const std::size_t mark = trace.size();
    runSteady(s, *b, params, trace, 12, -0.5f);
    CHECK(s.status().resyncByReason[static_cast<std::size_t>(SyncReason::kBindSwitch)] == 1u);

    // 旧读头只剩段 A 已拉进来的 88 个样本：淡出缩短到 88，全程不出现段 B 的值（负数）。
    CHECK(trace[mark] == 0.5f);
    CHECK(trace[mark + 87] > 0.0f);
    CHECK(trace[mark + 88] == 0.0f);
    for (std::size_t i = mark; i < mark + 600; ++i)
    {
        CHECK(trace[i] >= 0.0f);
    }
    // 新段从它自己的写头起：D 之后 20 ms onset 接上 −0.5，全程没有阶跃。
    CHECK(trace.back() == -0.5f);
    CHECK(maxStep(trace, mark - 1) <= 0.5f / 88.0f + 1.0e-6f);
}

TEST_CASE("环路 淡出读到的实际样本：写方停了、D 只比块长多一点时，数据断开之前先淡到 0，不以阶跃收尾")
{
    // ⛔ 删除式验证点：去掉写头落后时的淡出（取不到的样本直接补 0）→ 回送从 0.5 直接掉到 0，本用例变红。
    // 断档判据（W 不动且 R 前进 2·N_max）还没到，数据就先断了：写头第一块不动时，读头只剩 88 个样本。
    auto ring = makeRing();
    Session s;
    s.setDeclaredBlock(kNearBlock);
    s.attachRing(*ring, 1);
    const EngineParams params = nearBlockDelay();
    std::vector<float> trace;
    runSteady(s, *ring, params, trace, 12);
    feedInto(s, params, trace); // 这一块读到的是写方最后一次写下的 W
    REQUIRE(trace.back() == 0.5f);

    // 写方从此不写：第一块写头不动（还不算断档）。
    const std::size_t mark = trace.size();
    for (int block = 0; block < 3; ++block)
    {
        feedInto(s, params, trace);
    }
    CHECK(s.status().stalled == 1u);
    CHECK(s.status().overrun > 0u); // 取不到的样本照样计补 0
    CHECK(trace[mark] == 0.5f);
    CHECK(trace[mark + 87] > 0.0f);
    CHECK(trace[mark + 88] == 0.0f);
    CHECK(maxStep(trace, mark - 1) <= 0.5f / 88.0f + 1.0e-6f);

    // 写方恢复：断档解除，新数据 20 ms onset 接上，全程没有阶跃。
    runSteady(s, *ring, params, trace, 10);
    CHECK(s.status().stalled == 0u);
    CHECK(trace.back() == 0.5f);
    CHECK(maxStep(trace, mark - 1) <= 0.5f / 88.0f + 1.0e-6f);
}

TEST_CASE("环路 淡出读到的实际样本：D 足够长时断档的 5 ms 淡出读得到数据、走满，没有阶跃")
{
    auto ring = makeRing();
    Session s;
    s.setDeclaredBlock(kNearBlock);
    s.attachRing(*ring, 1);
    const EngineParams params; // D = 100 ms = 4800：断档在数据断开之前就判出来
    std::vector<float> trace;
    runSteady(s, *ring, params, trace, 20);
    feedInto(s, params, trace);
    REQUIRE(trace.back() == 0.5f);

    // 写方停了：第一块写头不动，第二块判断档（R 前进 2·N_max），从那一块的第一个样本起 5 ms 淡出。
    feedInto(s, params, trace);
    const std::size_t mark = trace.size();
    feedInto(s, params, trace);
    CHECK(s.status().stalled == 1u);
    CHECK(s.status().overrun == 0u);
    CHECK(trace[mark] == 0.5f);
    CHECK(trace[mark + 238] > 0.0f);
    CHECK(trace[mark + 240] == 0.0f);
    CHECK(maxStep(trace, 0) <= 0.5f / 240.0f + 1.0e-6f);
}

TEST_CASE("环路 拉环每块追到写头：写头一块里跳了 20000 帧，读头照样读到环里的真数据")
{
    // ⛔ 删除式验证点：每块只拉 n 帧（追不上写头）→ 跳变之后读头一直读到还没拉进来的位置，回送是 0，本用例变红。
    auto ring = makeRing();
    Session s;
    s.setDeclaredBlock(256);
    s.attachRing(*ring, 1);
    EngineParams params;
    params.delayMs = 10.0f; // D = 480

    for (int block = 0; block < 8; ++block)
    {
        s.feed(256, TransportInfo{}, params);
        writeFrames(*ring, 256);
    }
    // 写方一下子写了 20000 帧（它那一端的宿主一次给了很大的块）：|m| 超过跳变门限，重同步。
    writeFrames(*ring, 8192);
    writeFrames(*ring, 8192);
    writeFrames(*ring, 3616);
    std::vector<float> out;
    for (int block = 0; block < 16; ++block)
    {
        const BlockResult result = s.feed(256, TransportInfo{}, params);
        CHECK(result.overrun == 0u);
        out.insert(out.end(), s.left(), s.left() + 256);
        writeFrames(*ring, 256);
    }
    CHECK(s.status().resyncByReason[static_cast<std::size_t>(SyncReason::kJump)] == 1u);
    // 跳变之后新读头从 W 起读：D + onset（480 + 960）之后回送逐位就是环里那一帧的值。
    const LoopStatusFields fields = s.status();
    REQUIRE(fields.geometryConsistent == 1u);
    REQUIRE(fields.drift == 0);
    const std::int64_t lastStart = fields.writeHead - fields.offset; // 最后一块的块首读位 R
    const std::int64_t first = lastStart - 15 * 256; // 跳变那一块的块首读位
    for (int t = 480 + 960 + 16; t < 16 * 256; t += 13)
    {
        const std::int64_t p = first + t + fields.offset - 480;
        CHECK(out[static_cast<std::size_t>(t)] == encode(p));
    }
}

TEST_CASE("环路 状态快照：m / w / off 三元组出自同一块（顺序锁），并发读也拿不到拼起来的值")
{
    // ⛔ 删除式验证点：三个量逐个发布、不加顺序锁 → 并发读会拿到相邻两块拼起来的三元组，本用例大概率变红。
    LoopStatus status;
    std::atomic<bool> stop{false};
    std::thread writer([&] {
        for (std::int64_t k = 1; !stop.load(std::memory_order_relaxed); ++k)
        {
            status.setGeometry(k, 2 * k, 3 * k);
        }
    });
    std::uint64_t consistent = 0;
    std::uint64_t mixed = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(150);
    while (std::chrono::steady_clock::now() < until)
    {
        const LoopStatusFields fields = status.snapshot();
        if (fields.geometryConsistent == 0u)
        {
            continue;
        }
        if (fields.writeHead == 2 * fields.drift && fields.offset == 3 * fields.drift)
        {
            ++consistent;
        }
        else
        {
            ++mixed;
        }
    }
    stop.store(true, std::memory_order_relaxed);
    writer.join();
    CHECK(mixed == 0u);
    CHECK(consistent > 0u);
}
