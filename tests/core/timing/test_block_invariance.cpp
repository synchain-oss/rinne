// SPDX-License-Identifier: GPL-3.0-or-later
//
// 改 D 的交叉淡化与限速在块长上的不变性，以及它与重同步、算账块（不出样本）的相处：
//   - 参数恒定时，同一段输入怎么切块（整块、固定块、随机块、逐样本），输出逐位相同；限速窗口在块中间到点也一样；
//   - 算账块（不给回送缓冲，快速路径的口径）里计时照常前进：回到出样本的块时，交叉淡化的位置、限速窗口都在该在的地方；
//   - 交叉淡化进行中遇到重同步（跳变、换段），两个读头各自淡出，一个都不硬丢，回送没有阶跃；
//   - 读头的有效区间之前读成 0（交叉淡化里也一样）。

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "TimingKit.h"
#include "loop/LocalLine.h"
#include "loop/Taps.h"

namespace
{

using namespace rinne::loop;
using namespace rinne::timing_test;

constexpr std::int64_t kDeclared = 8192; // 声明块长：D 的下限就是它，所有切法共用
constexpr std::int64_t kD0 = 9600;
constexpr std::int64_t kDA = 12000;
constexpr std::int64_t kDB = 14400;
constexpr std::int64_t kDC = 19200;
constexpr std::int64_t kWarm = 25000;

// 环路层公开状态的全部量，摊成一串整数（浮点量取位型），用来整体比较「前后有没有变」
std::vector<std::int64_t> statusPrint(const LoopStatusFields& s)
{
    std::uint32_t userBits = 0;
    std::memcpy(&userBits, &s.delayUserMs, sizeof(userBits));
    std::vector<std::int64_t> v = {userBits,
                                   s.delayEffective,
                                   s.delayMin,
                                   s.delayMinReason,
                                   s.maxBlockSeen,
                                   s.delayMode,
                                   s.bpmCenti,
                                   s.bpmSource,
                                   s.delayTarget,
                                   s.delayHints,
                                   s.delayXfadeLeft,
                                   s.tapsActive,
                                   s.tapDrop,
                                   s.resyncCount,
                                   static_cast<std::int64_t>(s.overflow),
                                   static_cast<std::int64_t>(s.torn),
                                   static_cast<std::int64_t>(s.overrun),
                                   s.wCasFail,
                                   s.unstable,
                                   s.offsetDriftBlocks,
                                   s.frozenMax,
                                   s.stalled,
                                   s.tolerant,
                                   s.paired,
                                   s.drift,
                                   s.writeHead,
                                   s.offset,
                                   s.orderVerdict,
                                   s.orderVotes,
                                   s.hostNsValid};
    for (const std::uint32_t reason : s.resyncByReason)
    {
        v.push_back(reason);
    }
    return v;
}

// 确定的伪随机块长序列（1..700）
std::vector<int> randomSizes(std::uint32_t seed, int count, int maxSize)
{
    std::vector<int> sizes;
    std::uint32_t state = seed;
    for (int i = 0; i < count; ++i)
    {
        state = state * 1664525u + 1013904223u;
        sizes.push_back(1 + static_cast<int>((state >> 8) % static_cast<std::uint32_t>(maxSize)));
    }
    return sizes;
}

// 一次完整的运行：暖机 -> 改到 kDA（前沿，立即开始）-> 窗口里改到 kDB、kDC（合并）-> 窗口在块中间到点 -> 走完。
// 切法 sizes 在每一段里循环使用。
std::unique_ptr<Rig> runPlan(const std::vector<int>& sizes, bool accountingBetween = false,
                             std::int64_t accountFrom = 0, std::int64_t accountTo = 0)
{
    auto holder = std::make_unique<Rig>(48000.0, kDeclared);
    Rig& rig = *holder;
    const auto segment = [&](std::int64_t length, std::int64_t delay) {
        const EngineParams params = timeParams(delay);
        std::int64_t left = length;
        std::size_t k = 0;
        while (left > 0)
        {
            const int n = static_cast<int>(std::min<std::int64_t>(sizes[k % sizes.size()], left));
            const auto at = static_cast<std::int64_t>(rig.trace.size());
            // 算账块必须整块落在区间里：块的任何一个样本在区间外就照常出样本
            const bool account = accountingBetween && at >= accountFrom && at + n <= accountTo;
            rig.step(n, params, TransportInfo{}, !account);
            left -= n;
            ++k;
        }
    };
    segment(kWarm, kD0);
    segment(1000, kDA); // 25000：前沿，立即开始
    segment(3000, kDB); // 26000..29000：窗口里，挂着
    segment(20000, kDC); // 29000..：合并成最新的目标；窗口在 29800 到点（块中间）
    return holder;
}

} // namespace

TEST_CASE("块长不变性 改 D 的交叉淡化与限速：整块、固定块、随机块、逐样本的输出逐位相同，起点相同",
          "[timing][invariance]")
{
    // ⛔ 删除式验证点：限速窗口只在块边界到点（起点落在块首而不是块中间）→ 整块切法的起点是 37192 之类，
    //    与逐样本的 29800 不同，本用例变红；交叉淡化的步数按「块」而不是按样本推进 → 不同切法的增益错位，本用例变红。
    const auto wholeRun = runPlan({8192});
    const Rig& whole = *wholeRun;
    const std::vector<std::int64_t> wantStarts = {kWarm, kWarm + 4800};
    CHECK(whole.xfadeStarts() == wantStarts);
    CHECK(whole.status().delayEffective == kDC);
    CHECK(whole.status().tapDrop == 0u);
    CHECK(whole.status().delayXfadeLeft == 0);
    // 最终停在最新的目标上：读的正是 kDC 位置上的信号
    const auto last = static_cast<std::int64_t>(whole.trace.size()) - 1;
    CHECK(whole.trace.back() == noiseAt(last - kDC));

    const std::vector<std::vector<int>> splits = {
        {512}, {64}, {7, 1000, 3}, {1}, randomSizes(1u, 400, 700), randomSizes(99u, 400, 3000),
    };
    for (const auto& sizes : splits)
    {
        INFO("首块长 " << sizes[0] << "，共 " << sizes.size() << " 种块长");
        const auto otherRun = runPlan(sizes);
        const Rig& other = *otherRun;
        REQUIRE(other.trace.size() == whole.trace.size());
        CHECK(firstDiff(other.trace, whole.trace) == -1);
        CHECK(other.xfadeStarts() == wantStarts);
        CHECK(other.status().delayEffective == whole.status().delayEffective);
        CHECK(other.status().resyncCount == whole.status().resyncCount);
        CHECK(other.status().tapDrop == whole.status().tapDrop);
    }
}

TEST_CASE("块长不变性 算账块（不给回送缓冲）里计时照常前进：回到出样本的块时位置、限速窗口都不差一个样本",
          "[timing][invariance][accounting]")
{
    // ⛔ 删除式验证点：不出样本的块里不推进交叉淡化 / 限速窗口（早退）→ 回到出样本的块之后增益错位、第二个起点
    //    推迟，输出与全程出样本的运行不同，本用例变红。
    const auto referenceRun = runPlan({256});
    const Rig& reference = *referenceRun;
    const std::vector<std::int64_t> wantStarts = {kWarm, kWarm + 4800};
    REQUIRE(reference.xfadeStarts() == wantStarts);

    struct Stretch
    {
        std::int64_t from;
        std::int64_t to;
        const char* what;
    };
    const Stretch stretches[] = {
        {24000, 36000, "盖住两个起点与两段交叉淡化"},
        {24000, 26000, "盖住第一个起点，在第一段交叉淡化中间回到出样本"},
        {27000, 31000, "盖住限速窗口到点的那一刻（29800），在第二段交叉淡化中间回到出样本"},
        {26500, 29500, "整个挂起期间"},
    };
    for (const Stretch& stretch : stretches)
    {
        INFO(stretch.what);
        const auto variantRun = runPlan({256}, true, stretch.from, stretch.to);
        const Rig& variant = *variantRun;
        REQUIRE(variant.trace.size() == reference.trace.size());
        CHECK(variant.xfadeStarts() == wantStarts);
        // 算账的那一段没有产出；其余的样本逐位相同
        std::int64_t accounted = 0;
        std::int64_t wrong = 0;
        for (std::size_t t = 0; t < variant.trace.size(); ++t)
        {
            if (variant.produced[t] == 0u)
            {
                ++accounted;
                continue;
            }
            wrong += variant.trace[t] == reference.trace[t] ? 0 : 1;
        }
        CHECK(accounted > 0);
        CHECK(wrong == 0);
        CHECK(variant.status().delayEffective == reference.status().delayEffective);
        CHECK(variant.status().delayXfadeLeft == reference.status().delayXfadeLeft);
        CHECK(variant.processed() == reference.processed());
    }
}

TEST_CASE("0 样本的调用（直接打到环路层）：参数、速度全变了也不改任何状态、计数与事件，不推进限速窗口",
          "[timing][invariance][zero]")
{
    // ⛔ 删除式验证点：环路层在 0 样本时不立即返回（引擎层的早退挡不住直接调环路层的人）→ 目标 D 一变，
    //    零样本的调用就开始交叉淡化 / 刷新状态，状态指纹与起点和不插零样本的运行不同，本用例变红。
    struct Outcome
    {
        std::unique_ptr<Rig> rig;
        std::vector<std::int64_t> before;
        std::vector<std::int64_t> after;
        std::size_t eventsBefore = 0;
        std::size_t eventsAfter = 0;
        std::int64_t processedBefore = 0;
        std::int64_t processedAfter = 0;
    };
    const auto run = [](bool withZero) {
        Outcome out;
        out.rig = std::make_unique<Rig>(48000.0, kDeclared);
        Rig& rig = *out.rig;
        rig.run(kWarm, {256}, timeParams(kD0));
        const EngineParams next = timeParams(kDA);
        out.before = statusPrint(rig.status());
        out.eventsBefore = rig.events.size();
        out.processedBefore = rig.processed();
        if (withZero)
        {
            TransportInfo junk;
            junk.bpm = 777.0f;
            EngineParams other = next;
            other.delayMode = DelayMode::kSync;
            for (int i = 0; i < 60; ++i)
            {
                rig.feed(0, i % 2 == 0 ? next : other, junk);
            }
        }
        out.after = statusPrint(rig.status());
        out.eventsAfter = rig.events.size();
        out.processedAfter = rig.processed();
        rig.run(8000, {64}, next); // 参数这时才真的生效
        return out;
    };
    const Outcome zeroed = run(true);
    const Outcome plain = run(false);
    CHECK(zeroed.before == zeroed.after);
    CHECK(zeroed.eventsAfter == zeroed.eventsBefore);
    CHECK(zeroed.processedAfter == zeroed.processedBefore);
    CHECK(plain.before == plain.after);
    // 之后真正的块照常：起点就是第一个真块的第一个样本，输出与不插零样本的运行逐位相同
    REQUIRE(plain.rig->xfadeStarts().size() == 1u);
    CHECK(plain.rig->xfadeStarts()[0] == kWarm);
    CHECK(zeroed.rig->xfadeStarts() == plain.rig->xfadeStarts());
    CHECK(zeroed.rig->processed() == plain.rig->processed());
    CHECK(sameBits(zeroed.rig->trace, plain.rig->trace));
}

TEST_CASE("块长不变性 算账块里记账的量与出样本的运行一致：交叉淡化剩余、读头数、读位",
          "[timing][invariance][accounting]")
{
    // ⛔ 删除式验证点：算账块不推进交叉淡化 → 同一个样本位置上的剩余量不同，本用例变红。
    const auto probe = [](bool account) {
        Rig rig(48000.0, kDeclared);
        std::vector<std::int64_t> left;
        std::vector<std::uint32_t> taps;
        const auto push = [&](std::int64_t length, std::int64_t delay) {
            const EngineParams params = timeParams(delay);
            for (std::int64_t done = 0; done < length; done += 256)
            {
                const auto at = static_cast<std::int64_t>(rig.trace.size());
                const bool acc = account && at >= 24000 && at < 33000;
                rig.step(static_cast<int>(std::min<std::int64_t>(256, length - done)), params, TransportInfo{}, !acc);
                left.push_back(rig.status().delayXfadeLeft);
                taps.push_back(rig.status().tapsActive);
            }
        };
        push(kWarm, kD0);
        push(1000, kDA);
        push(3000, kDB);
        push(20000, kDC);
        return std::make_pair(left, taps);
    };
    const auto produced = probe(false);
    const auto accounted = probe(true);
    CHECK(produced.first == accounted.first);
    CHECK(produced.second == accounted.second);
    // 记账量里确实出现过交叉淡化（不是两边都恒为 0）
    std::int64_t peak = 0;
    for (const std::int64_t v : produced.first)
    {
        peak = std::max(peak, v);
    }
    CHECK(peak > 0);
}

TEST_CASE("交叉淡化遇到重同步（跳变）：两个读头各自 5 ms 淡出，不硬丢，回送没有阶跃，随后新读头 onset 接上",
          "[timing][resync]")
{
    // ⛔ 删除式验证点：重同步时只给「主读头」挂淡出（交叉淡化里的旧读头照旧）→ 旧读头的份额在重同步那一点硬掉，
    //    本用例变红；交叉淡化的读头被当成淡出中的读头跳过 → 同上。
    constexpr int kBlock = 256;
    Rig rig(48000.0, 1024);
    rig.setSignal([](std::int64_t) { return 0.5f; });
    EngineParams params = timeParams(kD0);
    rig.run(16384, {kBlock}, params); // 稳态
    REQUIRE(rig.trace.back() == 0.5f);
    params = timeParams(kDB);
    rig.run(3 * kBlock, {kBlock}, params); // 交叉淡化走到 40 %
    REQUIRE(rig.status().delayXfadeLeft > 0);
    const std::size_t mark = rig.trace.size();

    // 写方一下子多写了 9000 帧（超过跳变门限 8192）：下一块判跳变，重同步。
    rig.write(9000);
    rig.run(4 * kBlock, {kBlock}, params);
    CHECK(rig.status().resyncByReason[static_cast<std::size_t>(SyncReason::kJump)] == 1u);
    CHECK(rig.status().tapDrop == 0u);
    // 重同步之后到新数据到来之前只有淡出：两个读头的增益和（交叉淡化 out + in = 1）乘上淡出，一路线性降到 0。
    CHECK(maxStep(rig.trace, mark - 1) <= 0.5f / 240.0f + 1.0e-6f);
    // 随后新读头 D 之后 20 ms onset 接上，回到 0.5，全程仍没有阶跃
    rig.run(kDB + 4096, {kBlock}, params);
    CHECK(maxStep(rig.trace, mark - 1) <= 0.5f / 240.0f + 1.0e-6f);
    CHECK(rig.trace.back() == 0.5f);
    CHECK(rig.status().tapDrop == 0u);
    CHECK(rig.status().delayEffective == kDB);
    CHECK(rig.status().delayXfadeLeft == 0);
}

TEST_CASE("交叉淡化遇到换段：新旧两个读头读的位置相差一个 D，各占一格尾巴，都淡得完", "[timing][resync]")
{
    // ⛔ 删除式验证点：尾巴区只有一格（两个读头的区间取并集塞进去）→ 并集 5040 个样本超出尾巴容量，
    //    其中一个读头的淡出读不到数据、从非零硬掉到 0，本用例变红。
    constexpr int kBlock = 256;
    Rig rig(48000.0, 1024);
    rig.setSignal([](std::int64_t) { return 0.5f; });
    EngineParams params = timeParams(kD0);
    rig.run(16384, {kBlock}, params);
    REQUIRE(rig.trace.back() == 0.5f);
    params = timeParams(kDB);
    rig.run(3 * kBlock, {kBlock}, params);
    REQUIRE(rig.status().delayXfadeLeft > 0);
    const std::size_t mark = rig.trace.size();

    // 换到另一条环：它的写头比已拉到的位置更远（不触发写头倒退），里面一直是 0.5
    auto other = Rig::makeRing();
    rig.attach(*other, 2);
    rig.write(30000);
    rig.run(4 * kBlock, {kBlock}, params);
    CHECK(rig.status().resyncByReason[static_cast<std::size_t>(SyncReason::kBindSwitch)] == 1u);
    CHECK(rig.status().tapDrop == 0u);
    CHECK(maxStep(rig.trace, mark - 1) <= 0.5f / 240.0f + 1.0e-6f);
    // 旧段的读头只从尾巴里淡出，淡完之后到新段的数据到来之前是静音；新段 D 之后 onset 接上
    rig.run(kDB + 4096, {kBlock}, params);
    CHECK(maxStep(rig.trace, mark - 1) <= 0.5f / 240.0f + 1.0e-6f);
    CHECK(rig.trace.back() == 0.5f);
    CHECK(rig.status().tapDrop == 0u);
}

TEST_CASE("读头的有效区间：交叉淡化里新读头的 validFrom 之前读成 0，且切块不改变结果", "[timing][taps]")
{
    // ⛔ 删除式验证点：mixTaps 不看读头的 validFrom（只看延迟线的有效区间）→ validFrom 之前的位置读出旧数据，
    //    本用例变红；交叉淡化的步数不随推进累计 → 两半块的结果与整块不同，本用例变红。
    LocalLine line;
    REQUIRE(line.allocate(48000.0));
    line.resetTo(0);
    line.pull(0, 6000, [](i64 p, float& l, float& r) {
        l = noiseAt(p);
        r = noiseAt(p);
    });

    const auto build = [] {
        TapSet taps;
        const int a = taps.allocate();
        const int b = taps.allocate();
        Tap& oldTap = taps.at(a);
        oldTap.offset = 0;
        oldTap.delay = 1000;
        oldTap.validFrom = 0;
        oldTap.validTo = std::numeric_limits<i64>::max();
        oldTap.xfade = XfadeRole::kOutgoing;
        Tap& newTap = taps.at(b);
        newTap.offset = 0;
        newTap.delay = 2000;
        newTap.validFrom = 3000; // 位置 3000 之前没有数据
        newTap.validTo = std::numeric_limits<i64>::max();
        newTap.xfade = XfadeRole::kIncoming;
        taps.beginCrossfade(800, -100); // 起点在块内第 100 个样本
        return taps;
    };

    constexpr i64 kStart = 4000;
    constexpr std::int64_t kCount = 1200;
    std::vector<float> whole(static_cast<std::size_t>(kCount));
    std::vector<float> wholeRight(whole.size());
    {
        TapSet taps = build();
        mixTaps(taps, line, kStart, whole.data(), wholeRight.data(), kCount);
        advanceTaps(taps, kCount);
        CHECK_FALSE(taps.crossfadeActive()); // 起点 + 800 < 1200：走完，旧读头退场
        CHECK(taps.count() == 1);
    }
    // 逐样本的期望：新读头位置 t − 2000 在 3000 之前（t < 5000）读成 0，只有旧读头
    for (std::int64_t i = 0; i < kCount; ++i)
    {
        const i64 t = kStart + i;
        const std::int64_t step = i - 100;
        float in = 0.0f;
        float out = 1.0f;
        if (step >= 800)
        {
            in = 1.0f;
            out = 0.0f;
        }
        else if (step > 0)
        {
            in = static_cast<float>(step) / 800.0f;
            out = 1.0f - in;
        }
        float want = 0.0f;
        if (out > 0.0f)
        {
            want += noiseAt(t - 1000) * out;
        }
        if (in > 0.0f && t - 2000 >= 3000)
        {
            want += noiseAt(t - 2000) * in;
        }
        INFO("i = " << i);
        CHECK(whole[static_cast<std::size_t>(i)] == want);
    }
    // 切成 3 + 997 + 200：逐位相同
    std::vector<float> pieces(whole.size());
    std::vector<float> piecesRight(whole.size());
    {
        TapSet taps = build();
        std::int64_t done = 0;
        for (const std::int64_t n : {std::int64_t{3}, std::int64_t{997}, std::int64_t{200}})
        {
            mixTaps(taps, line, kStart + done, pieces.data() + done, piecesRight.data() + done, n);
            advanceTaps(taps, n);
            done += n;
        }
        CHECK_FALSE(taps.crossfadeActive());
    }
    CHECK(sameBits(pieces, whole));
}
