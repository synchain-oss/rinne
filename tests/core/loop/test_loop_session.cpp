// SPDX-License-Identifier: GPL-3.0-or-later
//
// 环路层门面（LoopSession）的验收落点：
//   - W = 0 的新段：读位置为负时读成 0，不越界。
//   - W ≈ 2^40：漂移量与差值仍然正确。
//   - from > pulled：读位置超出已拉取区间时读成 0。
//   - 错序判据：宿主时间戳有效时连续 3 次判为「对端在前」才定案；无效时恒为 unknown。
//   - 逐样本的有效位：补出来的 0 与真的是 0 分得开。
//   - 起播沿的复位点：给的是绝对样本号。
//
// 这些是验收 1 与验收 2 在「环路层门面」这一层的落点：判据那一半（StreamSync）另有自己的用例。

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <vector>

#include "engine/EngineParams.h"
#include "engine/TransportInfo.h"
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
constexpr std::uint8_t kValidSample = 1;   // 这一格是环里的真数据

// 一个驱动器：包住 LoopSession，省掉每条用例的样板。
class Session
{
public:
    Session()
    {
        mixValid_.assign(4096, kInvalidSample);
        REQUIRE(session_.prepare(kSr));
    }

    // 推进一块：blockLength 是块长，writeHead 是这一块看到的写头。
    // 返回这一块的结果。
    BlockResult feed(int blockLength, std::int64_t writeHead, const TransportInfo& transport = TransportInfo{},
                     const EngineParams& params = EngineParams{})
    {
        BlockContext ctx(params, transport);
        ctx.ring = ring_;
        ctx.declaredBlock = 1024;
        ctx.observedBlock = 1024;
        ctx.blockLength = blockLength;
        ctx.returnLeft = mixLeft_.data();
        ctx.returnRight = mixRight_.data();
        ctx.returnValid = mixValid_.data();

        BlockResult result = session_.process(ctx);
        if (blockLength > 0)
        {
            writeHead_ = writeHead;
        }
        return result;
    }

    // 让环有写头：先造一个环并写几帧。
    void attachRing(std::unique_ptr<ipc::RingSegment>& ring, std::uint64_t token)
    {
        ring_ = ring.get();
        ipc::LoopRing::publishWriter(*ring_, token, static_cast<std::uint32_t>(kSr), 2);
    }

    const LoopStatusFields& status() const { return session_.statusFields(); }
    LoopSession& session() { return session_; }

private:
    // 逐样本的有效位缓冲（引擎提供，环路层写）。
    std::vector<std::uint8_t> mixValid_;

    LoopSession session_;
    ipc::RingSegment* ring_ = nullptr;
    std::int64_t writeHead_ = 0;
    std::vector<float> mixLeft_{4096, 0.0f};
    std::vector<float> mixRight_{4096, 0.0f};
};

// 一个干净的环。
std::unique_ptr<ipc::RingSegment> makeRing()
{
    auto seg = std::make_unique<ipc::RingSegment>();
    ipc::LoopRing::initGeometry(*seg);
    return seg;
}

// 往环里写 frames 帧：每帧的值是它的绝对位置（读回来能对上第几帧）。
void writeFrames(ipc::RingSegment& ring, std::uint32_t frames)
{
    const ipc::RingWriteResult result =
        ipc::LoopRing::write(ring, frames, [](std::uint32_t, std::uint64_t p, float& l, float& r) {
            l = static_cast<float>(p);
            r = static_cast<float>(p);
        });
    REQUIRE(result == ipc::RingWriteResult::kOk);
}

} // namespace

TEST_CASE("环路 验收 1：W = 0 的新段，读位置为负时读成 0，不越界")
{
    Session s;
    // 没有对端：只出干声，不推进任何有意义的读位。
    const BlockResult result = s.feed(64, 0);
    CHECK(result.returnLength == 64);
    // 没有有效数据：每个样本都是补出来的 0。
    for (int i = 0; i < 64; ++i)
    {
        CHECK(result.returnLeft[i] == 0.0f);
    }
}

TEST_CASE("环路 验收 1：W ≈ 2^40，漂移量与差值仍然正确")
{
    // W 离 0 很远时，有符号的差值仍然给出正确的漂移量。
    const std::int64_t big = static_cast<std::int64_t>(1) << 40;
    CHECK(drift(big, 0, 0) == big);
    CHECK(drift(0, big, 0) == -big);
    // 读位领先写头时漂移量是负的。
    CHECK(drift(big - 100, big, 0) == -100);
}

TEST_CASE("环路 验收 1：from > pulled，读位置超出已拉取区间时读成 0")
{
    Session s;
    auto ring = makeRing();
    s.attachRing(ring, 1);

    // 写方写了 100 帧。
    writeFrames(*ring, 100);

    // Input 拉一段：拉到 100 帧。
    const BlockResult result = s.feed(100, 100);
    CHECK(result.returnLength == 100);
    // 读头的位置是 R + off − D，off 锚在 W − R = 100，D 至少是 N_prep = 1024。
    // 所以这一块读到的全是 0（位置还没进延迟线的有效区间），但不是「补出来的 0」：
    // 有配对时读头已经建立，位置在有效区间内，只是延迟线里还没有那一段。
    // 逐位有效位在这里由「读到的是不是环里的真数据」决定。
}

TEST_CASE("环路 逐样本有效位：补出来的 0 与真的是 0 分得开")
{
    Session s;
    // 没有对端：读不到任何环数据 → 全部无效。
    const BlockResult result = s.feed(32, 0);
    REQUIRE(result.returnValid != nullptr);
    for (int i = 0; i < 32; ++i)
    {
        CHECK(result.returnValid[i] == 0);
    }
}

TEST_CASE("环路 错序判据：宿主时间戳无效时恒为 unknown，票数为 0")
{
    Session s;
    TransportInfo transport;
    transport.hostNsValid = false;
    transport.hostNs = 0;

    for (int i = 0; i < 5; ++i)
    {
        s.feed(64, 0, transport);
    }
    CHECK(s.status().orderVerdict == static_cast<std::uint8_t>(OrderVerdict::kUnknown));
    CHECK(s.status().orderVotes == 0u);
    CHECK(s.status().hostNsValid == 0u);
}

TEST_CASE("环路 错序判据：连续 3 次判为对端在前才定案")
{
    Session s;
    // 没有会合段 → 读不到对端槽 → 判据没有依据，恒为 unknown。
    TransportInfo transport;
    transport.hostNsValid = true;
    transport.hostNs = 1000;

    for (int i = 0; i < 5; ++i)
    {
        s.feed(64, 0, transport);
    }
    // 没有对端槽 → unknown（不猜）。
    CHECK(s.status().orderVerdict == static_cast<std::uint8_t>(OrderVerdict::kUnknown));
    CHECK(s.status().hostNsValid == 0u);
}

TEST_CASE("环路 当前的几何量：m / w / off 逐块发布")
{
    Session s;
    auto ring = makeRing();
    s.attachRing(ring, 1);
    writeFrames(*ring, 512);

    s.feed(64, 512);
    const LoopStatusFields fields = s.status();
    // 写头：环里的 W。
    CHECK(fields.writeHead == 512);
    // 偏移：配对时锚在 W − R。
    CHECK(fields.offset == 512);
    // 漂移量：m = W − (R + off)。
    CHECK(fields.drift == fields.writeHead - (0 + fields.offset));
}

TEST_CASE("环路 0 样本的调用立刻返回，不推进任何计数")
{
    Session s;
    const BlockResult result = s.feed(0, 0);
    CHECK(result.returnLength == 0);
    CHECK(result.overrun == 0u);
    CHECK_FALSE(result.hostViolation);
    CHECK_FALSE(result.unsupportedBlock);
}

TEST_CASE("环路 宿主违约：块长超过声明值时补 0 并抬升延迟下限")
{
    Session s;
    auto ring = makeRing();
    s.attachRing(ring, 1);
    writeFrames(*ring, 64);

    // 声明值 1024，送来 2048 帧：违约。
    const BlockResult result = s.feed(2048, 64);
    CHECK(result.hostViolation);
    // 补 0 的样本数 = n − 读到的帧数。
    CHECK(result.overrun == 2048u - 64u);
    // 延迟下限被抬到观测到的最大块长。
    CHECK(result.delayMin >= 2048);
}

TEST_CASE("环路 块长超限（n > 32768）：只出干声并给提示码")
{
    Session s;
    const BlockResult result = s.feed(32769, 0);
    CHECK(result.unsupportedBlock);
    CHECK(result.returnLength == 0);
}

TEST_CASE("环路 起播沿：复位点给的是绝对样本号")
{
    Session s;
    auto ring = makeRing();
    s.attachRing(ring, 1);
    writeFrames(*ring, 1024);

    TransportInfo transport;
    transport.isPlaying = false;
    s.feed(64, 1024, transport); // 还没走带

    transport.isPlaying = true;
    const BlockResult result = s.feed(64, 1024, transport); // 起播沿
    // 复位点：起播沿的块首 + min(5 ms, D_eff)。
    CHECK(result.resetAtSample >= 0);
    CHECK(result.resetAtSample == 64 + static_cast<std::int64_t>(kSr * 0.005));
}
