// SPDX-License-Identifier: GPL-3.0-or-later
//
// 错序证据（OrderEvidence）的验收落点：
//   - 每块判一次；连续 3 次「Output 在前」定为 kOutputFirst，连续 3 次「Input 在前」清除。
//   - 判不了的块（没有时间戳、对端没有新块、时间戳回退 / 原地不动 / 跳变）清零连续计数，结论不动。
//   - 1 s（按样本计）没有有效检查，结论回到 unknown；失效时间与块长无关。
//   - Input 与 Output 两端对同一个现场给出同一个结论（镜像）。
//   - 用真 Cubase 的一段「宿主时间戳回退数秒」的记录回放：两种调用次序下都不误判、不翻转。
//   - 0 样本的调用不推进任何计数。
//
// 用例里的「两端」由 OrderPair 模拟：每个宿主周期两端各被调用一次（次序可选），各自在块首读对端槽的
// 块序号与时间戳、块末发布自己的槽，和真引擎的发布次序一致。

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "loop/OrderEvidence.h"

namespace
{

using namespace rinne::loop;

struct Slot
{
    std::uint64_t seq = 0;
    std::uint64_t ns = 0;
};

class OrderPair
{
public:
    explicit OrderPair(double sampleRate = 48000.0)
    {
        input.prepare(sampleRate);
        output.prepare(sampleRate);
    }

    // 一个宿主周期：两端各被调用一次，次序由 outputFirst 定；hostNs 两端相同（同一个周期的时间戳）。
    void cycle(std::uint64_t hostNs, std::int64_t n, bool outputFirst, bool valid = true)
    {
        if (outputFirst)
        {
            runOutput(hostNs, n, valid);
            runInput(hostNs, n, valid);
        }
        else
        {
            runInput(hostNs, n, valid);
            runOutput(hostNs, n, valid);
        }
    }

    // 只有 Input 被调用（Output 这个周期没来）
    void inputOnly(std::uint64_t hostNs, std::int64_t n) { runInput(hostNs, n, true); }

    void runInput(std::uint64_t hostNs, std::int64_t n, bool valid)
    {
        input.observe(OrderSide::kInput, n, valid, hostNs, true, outputSlot.seq, outputSlot.ns);
        inputSlot.ns = valid ? hostNs : 0u;
        ++inputSlot.seq;
    }
    void runOutput(std::uint64_t hostNs, std::int64_t n, bool valid)
    {
        output.observe(OrderSide::kOutput, n, valid, hostNs, true, inputSlot.seq, inputSlot.ns);
        outputSlot.ns = valid ? hostNs : 0u;
        ++outputSlot.seq;
    }

    OrderEvidence input;
    OrderEvidence output;
    Slot inputSlot;
    Slot outputSlot;
};

constexpr std::int64_t kBlock = 256;
constexpr std::uint64_t kStepNs = 5333333; // 256 样本 @ 48 kHz
constexpr std::uint64_t kBase = 1000000000000ull;

// 从 start 起连续跑 count 个周期，每周期前进一个块长的时间；返回下一个周期的时间戳。
std::uint64_t run(OrderPair& pair, std::uint64_t start, int count, bool outputFirst)
{
    std::uint64_t ns = start;
    for (int i = 0; i < count; ++i)
    {
        pair.cycle(ns, kBlock, outputFirst);
        ns += kStepNs;
    }
    return ns;
}

OrderVerdict verdictOf(const OrderEvidence& e)
{
    return e.snapshot().verdict;
}

} // namespace

TEST_CASE("错序证据 连续 3 次 Output 在前才定案，两端一致")
{
    OrderPair pair;
    std::uint64_t ns = kBase;
    // Output 先跑：每端第 1 个周期读到的对端槽只记块序号，不当新块（可能是上一个会话留下的旧值），没有票；
    // 第 2 个周期起每个周期一票，两端在第 4 个周期同时定案。
    ns = run(pair, ns, 1, true);
    CHECK(pair.input.snapshot().votes == 0u);
    CHECK(pair.input.snapshot().checks == 0u);
    CHECK(pair.output.snapshot().votes == 0u);
    CHECK(pair.input.snapshot().glitches == 0u);
    ns = run(pair, ns, 2, true);
    CHECK(pair.input.snapshot().votes == 2u);
    CHECK(pair.output.snapshot().votes == 2u);
    CHECK(verdictOf(pair.input) == OrderVerdict::kUnknown);
    CHECK(verdictOf(pair.output) == OrderVerdict::kUnknown);
    ns = run(pair, ns, 1, true);
    CHECK(verdictOf(pair.input) == OrderVerdict::kOutputFirst);
    CHECK(verdictOf(pair.output) == OrderVerdict::kOutputFirst);
    CHECK(pair.input.snapshot().direction == OrderDirection::kOutputFirst);
    CHECK(pair.input.snapshot().votes == OrderEvidence::kVotesToJudge);
    CHECK(pair.input.snapshot().hostNsValid);
    CHECK(pair.input.snapshot().glitches == 0u);
    CHECK(pair.output.snapshot().glitches == 0u);
}

TEST_CASE("错序证据 票数一票一票地加，第三票才改结论")
{
    OrderPair pair;
    // 预热：Input 在前跑几个周期，两端都已经判成正确
    std::uint64_t ns = run(pair, kBase, 6, false);
    REQUIRE(verdictOf(pair.input) == OrderVerdict::kCorrect);

    // 翻成 Output 在前：Input 端同周期就读到 Output 刚发布的值，一个周期一票
    for (std::uint32_t vote = 1; vote <= 2; ++vote)
    {
        ns = run(pair, ns, 1, true);
        CHECK(pair.input.snapshot().votes == vote);
        CHECK(pair.input.snapshot().direction == OrderDirection::kOutputFirst);
        CHECK(verdictOf(pair.input) == OrderVerdict::kCorrect); // 票不够：结论不动
    }
    ns = run(pair, ns, 1, true);
    CHECK(pair.input.snapshot().votes == 3u);
    CHECK(verdictOf(pair.input) == OrderVerdict::kOutputFirst);
    // 票数封顶在 3
    ns = run(pair, ns, 5, true);
    CHECK(pair.input.snapshot().votes == 3u);
}

TEST_CASE("错序证据 清除错序也要连续 3 次 Input 在前，中间夹一票反的重新数")
{
    OrderPair pair;
    std::uint64_t ns = run(pair, kBase, 8, true);
    REQUIRE(verdictOf(pair.input) == OrderVerdict::kOutputFirst);
    REQUIRE(verdictOf(pair.output) == OrderVerdict::kOutputFirst);

    // 翻成 Input 在前之后，一票、两票都不够。翻转后的第 1 个周期 Input 端读到的 Output 槽还是上一个周期读过的
    // （块序号没动），判不了；第 2 个周期起才一周期一票。
    ns = run(pair, ns, 1, false);
    CHECK(pair.input.snapshot().votes == 0u);
    ns = run(pair, ns, 1, false); // 第 1 票
    ns = run(pair, ns, 1, false); // 第 2 票
    CHECK(verdictOf(pair.input) == OrderVerdict::kOutputFirst);
    CHECK(pair.input.snapshot().direction == OrderDirection::kInputFirst);
    CHECK(pair.input.snapshot().votes == 2u);

    // 夹一个 Output 在前的周期：Input 端这个周期读到的是 Output 刚发布的值（同周期）→ 一票反的，连续计数从头来
    ns = run(pair, ns, 1, true);
    CHECK(verdictOf(pair.input) == OrderVerdict::kOutputFirst);
    CHECK(pair.input.snapshot().direction != OrderDirection::kInputFirst);

    // 再连续 Input 在前：先有一个周期是翻转后读旧值的（判不了），然后 3 票才清除
    ns = run(pair, ns, 1, false);
    ns = run(pair, ns, 2, false);
    CHECK(pair.input.snapshot().votes == 2u);
    CHECK(verdictOf(pair.input) == OrderVerdict::kOutputFirst);
    run(pair, ns, 1, false);
    CHECK(verdictOf(pair.input) == OrderVerdict::kCorrect);
    CHECK(verdictOf(pair.output) == OrderVerdict::kCorrect);
}

TEST_CASE("错序证据 判不了的块清零连续计数、结论不动：没有时间戳")
{
    OrderPair pair;
    std::uint64_t ns = run(pair, kBase, 8, true);
    REQUIRE(verdictOf(pair.input) == OrderVerdict::kOutputFirst);
    REQUIRE(pair.input.snapshot().votes == 3u);

    pair.cycle(ns, kBlock, true, false); // 宿主这个周期没给时间戳
    ns += kStepNs;
    const OrderSnapshot s = pair.input.snapshot();
    CHECK(s.verdict == OrderVerdict::kOutputFirst); // 结论还在
    CHECK(s.votes == 0u); // 连续计数清零
    CHECK(s.direction == OrderDirection::kNone);
    CHECK_FALSE(s.hostNsValid);

    // 时间戳回来：重新数票，结论一直是 Output 在前
    ns = run(pair, ns, 1, true);
    CHECK(verdictOf(pair.input) == OrderVerdict::kOutputFirst);
    CHECK(pair.input.snapshot().votes == 1u);
    run(pair, ns, 3, true);
    CHECK(pair.input.snapshot().votes == 3u);
    CHECK(verdictOf(pair.input) == OrderVerdict::kOutputFirst);
}

TEST_CASE("错序证据 没有对端：不猜，结论不动，超过 1 s 才回到 unknown")
{
    OrderEvidence e;
    e.prepare(48000.0);
    // 没有对端槽：从头到尾 unknown
    for (int i = 0; i < 10; ++i)
    {
        e.observe(OrderSide::kInput, kBlock, true, kBase + static_cast<std::uint64_t>(i) * kStepNs, false, 0, 0);
    }
    CHECK(e.snapshot().verdict == OrderVerdict::kUnknown);
    CHECK(e.snapshot().checks == 0u);

    // 对端的时间戳是 0（对端宿主没给）：同样判不了
    for (int i = 0; i < 10; ++i)
    {
        e.observe(OrderSide::kInput, kBlock, true, kBase + static_cast<std::uint64_t>(i + 10) * kStepNs, true,
                  static_cast<std::uint64_t>(i + 1), 0);
    }
    CHECK(e.snapshot().verdict == OrderVerdict::kUnknown);
    CHECK(e.snapshot().checks == 0u);
}

TEST_CASE("错序证据 失效时间按样本计，与块长无关")
{
    // 验证点：把失效改成按块数计，或改成别的时长 → 本用例变红。
    for (const std::int64_t n : {1, 64, 480, 1024, 4800, 48000})
    {
        OrderPair pair;
        run(pair, kBase, 8, true);
        REQUIRE(verdictOf(pair.input) == OrderVerdict::kOutputFirst);

        // 此后 Input 一端的每一块都没有有效检查（宿主不给时间戳）：累计样本数到 48000 之前结论不动，之后回到 unknown
        std::int64_t elapsed = 0;
        bool expired = false;
        while (!expired && elapsed < 200000)
        {
            pair.input.observe(OrderSide::kInput, n, false, 0, true, pair.outputSlot.seq, pair.outputSlot.ns);
            elapsed += n;
            expired = verdictOf(pair.input) == OrderVerdict::kUnknown;
            if (!expired)
            {
                CHECK(elapsed < 48000);
            }
        }
        INFO("n = " << n);
        CHECK(expired);
        CHECK(elapsed >= 48000);
        CHECK(elapsed - n < 48000); // 差一块就不到 1 s
    }
}

TEST_CASE("错序证据 失效时间随采样率走：96 kHz 下是 96000 样本")
{
    OrderPair pair(96000.0);
    run(pair, kBase, 8, true);
    REQUIRE(verdictOf(pair.input) == OrderVerdict::kOutputFirst);
    std::int64_t elapsed = 0;
    while (verdictOf(pair.input) != OrderVerdict::kUnknown && elapsed < 400000)
    {
        pair.input.observe(OrderSide::kInput, 1000, false, 0, true, pair.outputSlot.seq, pair.outputSlot.ns);
        elapsed += 1000;
    }
    CHECK(elapsed == 96000);
}

TEST_CASE("错序证据 有效检查把失效计时清零")
{
    OrderPair pair;
    std::uint64_t ns = run(pair, kBase, 8, true);
    REQUIRE(verdictOf(pair.input) == OrderVerdict::kOutputFirst);
    // 每 40000 样本才有一次有效检查：永远等不到 1 s，结论一直在
    for (int round = 0; round < 6; ++round)
    {
        pair.input.observe(OrderSide::kInput, 40000, false, 0, true, pair.outputSlot.seq, pair.outputSlot.ns);
        CHECK(verdictOf(pair.input) == OrderVerdict::kOutputFirst);
        ns = run(pair, ns, 1, true);
    }
    CHECK(verdictOf(pair.input) == OrderVerdict::kOutputFirst);
}

TEST_CASE("错序证据 对端自上次检查起没有处理新块：槽里是旧值，不是证据")
{
    // 验证点：不看块序号、把旧值当证据 → 本用例变红（Output 停了之后 Input 把旧值当成「Input 在前」清掉错序）。
    OrderPair pair;
    std::uint64_t ns = run(pair, kBase, 8, true);
    REQUIRE(verdictOf(pair.input) == OrderVerdict::kOutputFirst);

    // Output 停了：Input 一端的时间戳继续往前走，Output 槽的块序号和时间戳都不再变
    for (int i = 0; i < 20; ++i)
    {
        pair.inputOnly(ns, kBlock);
        ns += kStepNs;
    }
    CHECK(verdictOf(pair.input) == OrderVerdict::kOutputFirst); // 没被旧值清掉
    CHECK(pair.input.snapshot().votes == 0u);
    CHECK(pair.input.snapshot().checks > 0u);
}

TEST_CASE("错序证据 本端时间戳回退：这一块不投票、结论不重置，下一块和回退后的值比")
{
    OrderPair pair;
    const std::uint64_t ns = run(pair, kBase, 8, false);
    REQUIRE(verdictOf(pair.input) == OrderVerdict::kCorrect);
    const std::uint32_t checksBefore = pair.input.snapshot().checks;

    // 回退 7 秒（和真宿主一样两端拿到同一个值）
    const std::uint64_t regressed = ns - 7000000000ull;
    pair.cycle(regressed, kBlock, false);
    CHECK(verdictOf(pair.input) == OrderVerdict::kCorrect); // 结论不被回退冲掉
    CHECK(pair.input.snapshot().votes == 0u);
    CHECK(pair.input.snapshot().checks == checksBefore); // 没有计票
    CHECK(pair.input.snapshot().glitches == 1u);
    CHECK(pair.output.snapshot().glitches == 1u);

    // 回退之后正常前进：新的基准是回退后的值，不是回退前的。
    // Input 在前时，Input 端要到下一个周期才从 Output 槽里读到那个回退的值（对端回退，又一个判不了的块），
    // Output 端读到的是 Input 刚发布的，没有这一块；此后两端都不再出毛刺。
    run(pair, regressed + kStepNs, 4, false);
    CHECK(verdictOf(pair.input) == OrderVerdict::kCorrect);
    CHECK(pair.input.snapshot().votes == OrderEvidence::kVotesToJudge);
    CHECK(pair.input.snapshot().glitches == 2u);
    CHECK(pair.output.snapshot().glitches == 1u);
}

TEST_CASE("错序证据 连续三块时间戳单调下降：每块都判不了，结论不变，不会崩")
{
    OrderPair pair;
    std::uint64_t ns = run(pair, kBase, 8, true);
    REQUIRE(verdictOf(pair.input) == OrderVerdict::kOutputFirst);
    const std::uint32_t glitchesBefore = pair.input.snapshot().glitches;

    for (int i = 0; i < 3; ++i)
    {
        ns -= 3 * kStepNs; // 每块比上一块更早
        pair.cycle(ns, kBlock, true);
        CHECK(verdictOf(pair.input) == OrderVerdict::kOutputFirst);
        CHECK(pair.input.snapshot().votes == 0u);
    }
    CHECK(pair.input.snapshot().glitches == glitchesBefore + 3u);
    CHECK(pair.output.snapshot().glitches >= 3u);
}

TEST_CASE("错序证据 本端时间戳原地不动：不投票")
{
    OrderPair pair;
    std::uint64_t ns = run(pair, kBase, 8, true);
    REQUIRE(verdictOf(pair.input) == OrderVerdict::kOutputFirst);
    // 同一个时间戳连续给了两块
    pair.cycle(ns, kBlock, true);
    const std::uint32_t glitches = pair.input.snapshot().glitches;
    pair.cycle(ns, kBlock, true);
    CHECK(pair.input.snapshot().glitches > glitches);
    CHECK(pair.input.snapshot().votes == 0u);
    CHECK(verdictOf(pair.input) == OrderVerdict::kOutputFirst);
}

TEST_CASE("错序证据 本端时间戳原地不动：对端是新块、在前进时也不投票（只因本端）")
{
    OrderEvidence e;
    e.prepare(48000.0);
    std::uint64_t seq = 0;
    std::uint64_t ns = kBase;
    // 6 块正常：Output 的槽落后一块（Input 在前）
    for (int i = 0; i < 6; ++i)
    {
        e.observe(OrderSide::kInput, kBlock, true, ns, true, ++seq, ns - kStepNs);
        ns += kStepNs;
    }
    REQUIRE(e.snapshot().verdict == OrderVerdict::kCorrect);
    const std::uint32_t checks = e.snapshot().checks;
    const std::uint32_t glitches = e.snapshot().glitches;

    // 本端给的时间戳和上一块相同；对端是新块、时间戳往前走了一步（单看对端没有任何问题，
    // 要是照常比较，这一块会记成一票「Output 在前」）
    const std::uint64_t same = ns - kStepNs;
    e.observe(OrderSide::kInput, kBlock, true, same, true, ++seq, same);
    CHECK(e.snapshot().checks == checks);
    CHECK(e.snapshot().glitches == glitches + 1u);
    CHECK(e.snapshot().votes == 0u);
    CHECK(e.snapshot().direction == OrderDirection::kNone);
    CHECK(e.snapshot().verdict == OrderVerdict::kCorrect);
}

TEST_CASE("错序证据 跳变：比按样本推进的预期多出 0.25 s 以上不投票，之内照常投票")
{
    OrderPair pair;
    std::uint64_t ns = run(pair, kBase, 8, false);
    REQUIRE(verdictOf(pair.input) == OrderVerdict::kCorrect);

    // 前进量 = 预期 + 0.2 s（在松弛之内）：照常投票
    ns += 200000000ull;
    pair.cycle(ns, kBlock, false);
    CHECK(pair.input.snapshot().glitches == 0u);
    CHECK(pair.input.snapshot().votes > 0u);
    ns += kStepNs;

    // 前进量 = 预期 + 0.3 s：跳变，不投票
    ns += 300000000ull;
    pair.cycle(ns, kBlock, false);
    CHECK(pair.input.snapshot().glitches == 1u);
    CHECK(pair.input.snapshot().votes == 0u);
    CHECK(verdictOf(pair.input) == OrderVerdict::kCorrect);
}

TEST_CASE("错序证据 对端时间戳回退：不投票")
{
    OrderEvidence e;
    e.prepare(48000.0);
    std::uint64_t seq = 0;
    std::uint64_t ns = kBase;
    // 6 块正常：Output 的槽落后一块（Input 在前）
    for (int i = 0; i < 6; ++i)
    {
        e.observe(OrderSide::kInput, kBlock, true, ns, true, ++seq, ns - kStepNs);
        ns += kStepNs;
    }
    REQUIRE(e.snapshot().verdict == OrderVerdict::kCorrect);
    const std::uint32_t checks = e.snapshot().checks;

    // 对端槽里的时间戳比它上一块还小（对端回退），块序号是新的
    e.observe(OrderSide::kInput, kBlock, true, ns, true, ++seq, kBase);
    CHECK(e.snapshot().checks == checks);
    CHECK(e.snapshot().glitches == 1u);
    CHECK(e.snapshot().verdict == OrderVerdict::kCorrect);
}

TEST_CASE("错序证据 一端从 Input 在前翻成 Output 在前再翻回：两端的结论始终一致（稳定之后）")
{
    // 随机的次序串（每段长度 ≥ 5）：每段末尾两端的结论必须一致，并且等于这一段的次序。
    std::uint64_t rng = 0x243F6A8885A308D3ull;
    auto next = [&rng]() {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return rng;
    };
    OrderPair pair;
    std::uint64_t ns = kBase;
    bool outputFirst = false;
    for (int segment = 0; segment < 60; ++segment)
    {
        outputFirst = !outputFirst;
        const int length = 5 + static_cast<int>(next() % 7);
        ns = run(pair, ns, length, outputFirst);
        const OrderVerdict want = outputFirst ? OrderVerdict::kOutputFirst : OrderVerdict::kCorrect;
        INFO("segment " << segment << " length " << length);
        CHECK(verdictOf(pair.input) == want);
        CHECK(verdictOf(pair.output) == want);
    }
}

TEST_CASE("错序证据 块长不影响判断：每个周期的块长随机，结论一样")
{
    std::uint64_t rng = 0x9E3779B97F4A7C15ull;
    auto next = [&rng]() {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return rng;
    };
    for (const bool outputFirst : {false, true})
    {
        OrderPair pair;
        std::uint64_t ns = kBase;
        for (int i = 0; i < 40; ++i)
        {
            const std::int64_t n = 16 + static_cast<std::int64_t>(next() % 2000);
            pair.cycle(ns, n, outputFirst);
            // 前进量不超过按样本推进的预期（小一点，模拟比实时快一点的宿主）
            ns += static_cast<std::uint64_t>(static_cast<double>(n) * 1.0e9 / 48000.0 * 0.5) + 1;
        }
        const OrderVerdict want = outputFirst ? OrderVerdict::kOutputFirst : OrderVerdict::kCorrect;
        CHECK(verdictOf(pair.input) == want);
        CHECK(verdictOf(pair.output) == want);
        CHECK(pair.input.snapshot().glitches == 0u);
    }
}

TEST_CASE("错序证据 0 样本的调用不推进任何计数")
{
    OrderPair pair;
    std::uint64_t ns = run(pair, kBase, 8, true);
    REQUIRE(verdictOf(pair.input) == OrderVerdict::kOutputFirst);
    const OrderSnapshot before = pair.input.snapshot();

    // 0 样本调用任意多次（含非法时间戳与新的块序号）：状态一个字节都不变
    for (int i = 0; i < 100000; ++i)
    {
        pair.input.observe(OrderSide::kInput, 0, true, ns + static_cast<std::uint64_t>(i), true,
                           pair.outputSlot.seq + static_cast<std::uint64_t>(i), 1);
    }
    const OrderSnapshot after = pair.input.snapshot();
    CHECK(after.pack() == before.pack());
    CHECK(after.glitches == before.glitches);
    // 而且失效计时没被它推进：之后一块正常的检查仍能延续
    pair.cycle(ns, kBlock, true);
    CHECK(verdictOf(pair.input) == OrderVerdict::kOutputFirst);
}

TEST_CASE("错序证据 prepare：换采样率才清证据，同采样率保持连续")
{
    OrderPair pair;
    run(pair, kBase, 8, true);
    REQUIRE(verdictOf(pair.input) == OrderVerdict::kOutputFirst);
    const std::uint32_t checks = pair.input.snapshot().checks;

    pair.input.prepare(48000.0);
    CHECK(verdictOf(pair.input) == OrderVerdict::kOutputFirst);
    CHECK(pair.input.snapshot().checks == checks);

    pair.input.prepare(44100.0);
    CHECK(verdictOf(pair.input) == OrderVerdict::kUnknown);
    CHECK(pair.input.snapshot().checks == 0u);
    CHECK(pair.input.snapshot().glitches == 0u);
}

TEST_CASE("错序证据 快照打包与解包逐字段相同")
{
    OrderSnapshot s;
    s.verdict = OrderVerdict::kOutputFirst;
    s.direction = OrderDirection::kInputFirst;
    s.votes = 2;
    s.checks = 0xDEADBEEFu;
    s.glitches = 77;
    s.hostNsValid = true;
    const OrderSnapshot t = OrderSnapshot::unpack(s.pack(), s.glitches);
    CHECK(t.verdict == s.verdict);
    CHECK(t.direction == s.direction);
    CHECK(t.votes == s.votes);
    CHECK(t.checks == s.checks);
    CHECK(t.glitches == s.glitches);
    CHECK(t.hostNsValid == s.hostNsValid);

    // 全 0 就是默认值
    const OrderSnapshot z = OrderSnapshot::unpack(0, 0);
    CHECK(z.verdict == OrderVerdict::kUnknown);
    CHECK(z.direction == OrderDirection::kNone);
    CHECK(z.votes == 0u);
    CHECK_FALSE(z.hostNsValid);
}

namespace
{

// 真 Cubase 记录里的一段宿主时间戳（纳秒）：开关切换时回退约 7.3 s、再回到原来的轴，
// 前后的块长是 256 样本；两端取到的是同一串值。块长 / 间隔取自同一份记录。
struct CubaseRow
{
    std::int64_t n;
    std::uint64_t ns;
};

const std::vector<CubaseRow>& cubaseRows()
{
    static const std::vector<CubaseRow> rows = {
        {256, 1133500788600ull},
        {256, 1133505779000ull},
        {256, 1133510781300ull},
        {256, 1133516780600ull},
        {256, 1133521787600ull},
        {256, 1133526785900ull},
        {256, 1133532781400ull},
        // 回退到另一条时间轴
        {256, 1126188753233ull},
        {256, 1126194086566ull},
        {256, 1126188753233ull},
        // 向前跳回原轴，之后几块的步长只有 0.1 ~ 0.2 ms
        {256, 1133575649766ull},
        {256, 1133575869866ull},
        {256, 1133575951466ull},
        {256, 1133576066366ull},
        {256, 1133576200866ull},
        // 块长改成 512，又回退了 11 ms，之后恢复正常步长
        {512, 1133565000700ull},
        {512, 1133575079600ull},
        {512, 1133585946900ull},
        {512, 1133596970600ull},
        {512, 1133606985200ull},
        {512, 1133618003100ull},
        {512, 1133629050100ull},
        {512, 1133639000300ull},
        {512, 1133650008700ull},
        {512, 1133661017400ull},
        {512, 1133671068700ull},
        {512, 1133682003700ull},
    };
    return rows;
}

} // namespace

TEST_CASE("错序证据 真 Cubase 记录回放：Input 在前时回退段不误判成 Output 在前")
{
    // 验证点：回退 / 跳变的块也投票 → 本用例变红（回退段的比较结果是反的）。
    OrderPair pair;
    // 先在前面铺一段稳定的 Input 在前
    run(pair, 1133400000000ull, 10, false);
    REQUIRE(verdictOf(pair.input) == OrderVerdict::kCorrect);
    REQUIRE(verdictOf(pair.output) == OrderVerdict::kCorrect);

    for (const CubaseRow& row : cubaseRows())
    {
        pair.cycle(row.ns, row.n, false);
        CHECK(verdictOf(pair.input) != OrderVerdict::kOutputFirst);
        CHECK(verdictOf(pair.output) != OrderVerdict::kOutputFirst);
    }
    CHECK(verdictOf(pair.input) == OrderVerdict::kCorrect);
    CHECK(verdictOf(pair.output) == OrderVerdict::kCorrect);
    // 回退、跳变都被看见了
    CHECK(pair.input.snapshot().glitches >= 4u);
    CHECK(pair.output.snapshot().glitches >= 3u);
}

TEST_CASE("错序证据 真 Cubase 记录回放：真的 Output 在前时回退段不把错序冲掉")
{
    OrderPair pair;
    run(pair, 1133400000000ull, 10, true);
    REQUIRE(verdictOf(pair.input) == OrderVerdict::kOutputFirst);
    REQUIRE(verdictOf(pair.output) == OrderVerdict::kOutputFirst);

    for (const CubaseRow& row : cubaseRows())
    {
        pair.cycle(row.ns, row.n, true);
        CHECK(verdictOf(pair.input) != OrderVerdict::kCorrect);
        CHECK(verdictOf(pair.output) != OrderVerdict::kCorrect);
    }
    CHECK(verdictOf(pair.input) == OrderVerdict::kOutputFirst);
    CHECK(verdictOf(pair.output) == OrderVerdict::kOutputFirst);
}

// ---- 第一次读到的槽、超长块 ----

namespace
{

// n 个样本 @ 48 kHz 对应的纳秒数（取整）。
std::uint64_t samplesToNs(std::int64_t n)
{
    return static_cast<std::uint64_t>(n) * 1000000000ull / 48000ull;
}

// 手动喂的 Input 端：每个正常块对端都是新块（块序号 +1），对端时间戳与本块相同（Output 在前，每块一票）。
// huge 是超长块：环路不走，只走 advance；对端这个周期照样发布了一个块。
struct OutputFirstFeed
{
    OrderEvidence e;
    std::uint64_t ns = kBase;
    std::uint64_t seq = 100;

    OutputFirstFeed() { e.prepare(48000.0); }

    void block(std::int64_t n = kBlock)
    {
        e.observe(OrderSide::kInput, n, true, ns, true, ++seq, ns);
        ns += samplesToNs(n);
    }
    void huge(std::int64_t n, bool withAdvance = true)
    {
        if (withAdvance)
        {
            e.advance(n, true, ns);
        }
        ++seq;
        ns += samplesToNs(n);
    }
};

} // namespace

TEST_CASE("错序证据 第一次读到的槽、对端消失后重新出现的槽、换组后的槽：只记块序号，不当新块")
{
    // ⛔ 删除式验证点：第一次读到就当新块 → 旧值投了一票，本用例变红；
    //    第一次读到的旧值记成对端的时间戳基线 → 下一个真新块被当成对端回退，本用例变红；
    //    对端消失时不清对端记账 → 重新出现的旧值被当成新块，本用例变红；
    //    forgetPeer 不清对端记账 → 换组后的旧值被当成新块，本用例变红。
    OrderEvidence e;
    e.prepare(48000.0);
    std::uint64_t ns = kBase;
    constexpr std::uint64_t kStale = 10ull * 1000000000ull; // 旧会话留下的时间戳比本块大 10 s

    const auto step = [&](bool present, std::uint64_t seq, std::uint64_t peerNs) {
        e.observe(OrderSide::kInput, kBlock, true, ns, present, seq, peerNs);
        ns += kStepNs;
    };

    // 第一次读到：槽里是旧值（块序号 7，时间戳远大于本块）。不当新块：没有票，也不记异常
    step(true, 7, ns + kStale);
    CHECK(e.snapshot().votes == 0u);
    CHECK(e.snapshot().checks == 0u);
    CHECK(e.snapshot().glitches == 0u);
    // 对端的第一个真新块（时间戳与本块相同）：旧值没有被记成基线，所以不是回退；这一票才是第一票
    step(true, 8, ns);
    CHECK(e.snapshot().votes == 1u);
    CHECK(e.snapshot().checks == 1u);
    CHECK(e.snapshot().glitches == 0u);

    // 对端消失（读不到槽）：连续票清零，对端记账清掉
    step(false, 0, 0);
    CHECK(e.snapshot().votes == 0u);
    // 重新出现：槽里是别的会话留下的旧值（块序号 50）：只记块序号
    step(true, 50, ns + kStale);
    CHECK(e.snapshot().votes == 0u);
    CHECK(e.snapshot().checks == 1u);
    CHECK(e.snapshot().glitches == 0u);
    step(true, 51, ns);
    CHECK(e.snapshot().votes == 1u);
    CHECK(e.snapshot().checks == 2u);
    CHECK(e.snapshot().glitches == 0u);

    // 旧组的槽里时间戳很靠前（比本块大 10 s）：它是旧组真发布的新块，照常一票
    step(true, 52, ns + kStale);
    CHECK(e.snapshot().votes == 2u);
    CHECK(e.snapshot().checks == 3u);
    CHECK(e.snapshot().glitches == 0u);

    // 换组（对端的槽直接换成另一个，中间没有读不到的块）：forgetPeer 之后同样只记块序号
    e.forgetPeer();
    step(true, 900, ns + kStale);
    CHECK(e.snapshot().votes == 0u);
    CHECK(e.snapshot().checks == 3u);
    // 新组的第一个真新块：时间戳比旧组最后一个值小得多，但旧组的对端基线已经作废，不是对端回退
    step(true, 901, ns);
    CHECK(e.snapshot().votes == 1u);
    CHECK(e.snapshot().checks == 4u);
    CHECK(e.snapshot().glitches == 0u);
}

TEST_CASE("错序证据 超长块只推进本端的时间戳基线：下一个正常块不记异常、不清连续票")
{
    // ⛔ 删除式验证点：advance 不推进时间戳基线 → 下一个正常块被记成跳变，glitches 加 1、连续票清零，本用例变红；
    //    advance 清掉连续票 → 第三票接不上，本用例变红；advance 自己投票或计检查 → checks 对不上，本用例变红。
    constexpr std::int64_t kHuge = 40000; // 约 0.83 s，超过支持上限 32768
    OutputFirstFeed f;
    f.block(); // 第一次读到：只记块序号
    f.block();
    f.block();
    REQUIRE(f.e.snapshot().votes == 2u);
    REQUIRE(f.e.snapshot().checks == 2u);

    f.huge(kHuge);
    CHECK(f.e.snapshot().votes == 2u); // 不打断连续票
    CHECK(f.e.snapshot().checks == 2u); // 自己不投票
    CHECK(f.e.snapshot().glitches == 0u);
    CHECK(verdictOf(f.e) == OrderVerdict::kUnknown);

    f.block(); // 约 40000 帧的超长块之后接 256 帧的正常块
    CHECK(f.e.snapshot().glitches == 0u);
    CHECK(f.e.snapshot().votes == 3u);
    CHECK(f.e.snapshot().checks == 3u);
    CHECK(verdictOf(f.e) == OrderVerdict::kOutputFirst);

    // 对照：超长块完全不经证据（没有 advance），下一个正常块的时间戳比基线多出整块超长块的时长，
    // 被记成跳变，连续票清零——这正是 advance 要避免的。
    OutputFirstFeed g;
    g.block();
    g.block();
    g.block();
    g.huge(kHuge, false);
    g.block();
    CHECK(g.e.snapshot().glitches == 1u);
    CHECK(g.e.snapshot().votes == 0u);
    CHECK(verdictOf(g.e) == OrderVerdict::kUnknown);
}

TEST_CASE("错序证据 超长块的样本数计入失效计时，到 1 s 结论回到 unknown；0 样本与负数的 advance 什么都不做")
{
    // ⛔ 删除式验证点：advance 不累加失效计时 → 结论永远不失效，本用例变红；
    //    失效门槛差一个样本（> 换成 >=）→ 恰好 48000 个样本那一档变红；
    //    advance 不挡 n <= 0 → 0 样本的调用推动基线，下一个正常块被记成回退，本用例变红。
    OutputFirstFeed f;
    for (int i = 0; i < 4; ++i)
    {
        f.block();
    }
    REQUIRE(verdictOf(f.e) == OrderVerdict::kOutputFirst);
    REQUIRE(f.e.snapshot().votes == 3u);

    // 47999 个样本：还没到 1 s；再多 1 个样本，恰好 48000：回到 unknown，连续票一并作废
    f.e.advance(47999, true, f.ns);
    CHECK(verdictOf(f.e) == OrderVerdict::kOutputFirst);
    CHECK(f.e.snapshot().votes == 3u);
    f.e.advance(1, true, f.ns);
    CHECK(verdictOf(f.e) == OrderVerdict::kUnknown);
    CHECK(f.e.snapshot().votes == 0u);
    CHECK(f.e.snapshot().direction == OrderDirection::kNone);

    // 分几块超长块累加，与一块超长块一样
    OutputFirstFeed g;
    for (int i = 0; i < 4; ++i)
    {
        g.block();
    }
    g.huge(30000);
    CHECK(verdictOf(g.e) == OrderVerdict::kOutputFirst);
    g.huge(30000);
    CHECK(verdictOf(g.e) == OrderVerdict::kUnknown);

    // 0 样本与负数：状态一个字节都不变，基线也不动（之后的正常块不记回退）
    OutputFirstFeed h;
    for (int i = 0; i < 3; ++i)
    {
        h.block();
    }
    const OrderSnapshot before = h.e.snapshot();
    for (int i = 0; i < 1000; ++i)
    {
        h.e.advance(0, true, h.ns + 1000000000000ull);
        h.e.advance(-5, true, 1);
    }
    CHECK(h.e.snapshot().pack() == before.pack());
    CHECK(h.e.snapshot().glitches == before.glitches);
    h.block();
    CHECK(h.e.snapshot().glitches == 0u);
    CHECK(h.e.snapshot().votes == 3u);
}

TEST_CASE("错序证据 超长块自己的时间戳回退或原地不动：不计异常，下一块和它比")
{
    // ⛔ 删除式验证点：advance 也做回退 / 跳变检查并计异常 → glitches 对不上，本用例变红。
    OutputFirstFeed f;
    f.block();
    f.block();
    f.block();
    REQUIRE(f.e.snapshot().votes == 2u);

    const std::uint64_t lastBlockNs = f.ns - samplesToNs(kBlock);
    // 时间戳原地不动（等于上一块的）：普通块会被记成异常，超长块不计
    f.e.advance(1000, true, lastBlockNs);
    CHECK(f.e.snapshot().glitches == 0u);
    // 时间戳回退了 5 s：同样不计异常，连续票不动
    f.e.advance(1000, true, lastBlockNs - 5ull * 1000000000ull);
    CHECK(f.e.snapshot().glitches == 0u);
    CHECK(f.e.snapshot().votes == 2u);
    CHECK(f.e.snapshot().checks == 2u);
}

TEST_CASE("错序证据 没有时间戳的超长块：本端基线作废（和普通块一样），下一块照常，不记异常")
{
    // ⛔ 删除式验证点：advance 在没有时间戳时仍保留旧基线 → 下一块和很早以前的基线比，被记成跳变，本用例变红。
    OutputFirstFeed f;
    f.block();
    f.block();
    f.block();
    REQUIRE(f.e.snapshot().votes == 2u);
    f.e.advance(40000, false, 0);
    CHECK_FALSE(f.e.snapshot().hostNsValid);
    CHECK(f.e.snapshot().votes == 2u);
    ++f.seq;
    f.ns += 100ull * 1000000000ull; // 宿主在这期间时间戳跳了很远：没有基线可比，不算跳变
    f.block();
    CHECK(f.e.snapshot().glitches == 0u);
    CHECK(f.e.snapshot().votes == 3u);
    CHECK(f.e.snapshot().hostNsValid);
}
