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

#include <cstdint>
#include <memory>
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
        ctx.declaredBlock = 1024;
        ctx.observedBlock = 1024;
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

    const float* left() const { return mixLeft_.data(); }
    const std::uint8_t* valid() const { return mixValid_.data(); }
    LoopStatusFields status() const { return session_.statusFields(); }
    LoopSession& session() { return session_; }

private:
    LoopSession session_;
    ipc::RingSegment* ring_ = nullptr;
    ipc::RvSegment* rv_ = nullptr;
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
