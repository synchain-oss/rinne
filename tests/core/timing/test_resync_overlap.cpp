// SPDX-License-Identifier: GPL-3.0-or-later
//
// 重同步叠重同步：交叉淡化（或上一次重同步的 5 ms 淡出）还没走完，又来了第二次、第三次重同步。
//   - 仍在淡出的读头读的是自己的尾巴：后一次重同步的预拷贝不覆盖它们，淡出读到的永远是自己的快照；
//   - 读头槽用完（需要第 4 个读头）时收掉包络最小的那个，它在上一个样本上的值接成 5 ms 线性衰减的末值保持，
//     回送不阶跃；被收掉的读头增益有上界；
//   - 以上情形里，回送逐样本的跳变都不超过各路淡变斜率之和；重同步落在同一个块首时，切块不改变结果。
//
// 信号是常数 0.5：读头读到 0（尾巴被覆盖、读头凭空消失）都是一处明显的阶跃，淡变的斜率则是已知的小量。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include "TimingKit.h"

namespace
{

using namespace rinne::loop;
using namespace rinne::timing_test;

constexpr float kLevel = 0.5f;
constexpr int kBlock = 8;
// 交叉淡化前后的 D：都比 5 ms 短，淡出长度就是 D 本身（读头只有 D 个样本可淡）
constexpr std::int64_t kSlow = 160;
constexpr std::int64_t kFast = 96;

// 回送相邻样本之差的上界：同时淡出的读头各按「增益/淡出步数」下降，新读头按 20 ms onset 上升。
// 淡出步数至少 kFast，同时在场的读头至多 3 个。
constexpr float kSlope = kLevel / static_cast<float>(kFast) + 3.0f * kLevel / 960.0f;

// 一组环：每次换段用一条新环，写头比已拉到的位置更远
class Rings
{
public:
    void switchTo(Rig& rig, std::int64_t writeHead)
    {
        rings_.push_back(Rig::makeRing());
        rig.attach(*rings_.back(), static_cast<std::uint64_t>(rings_.size()) + 1u);
        rig.write(writeHead);
    }

private:
    std::vector<std::unique_ptr<rinne::ipc::RingSegment>> rings_;
};

// 暖机到 kSlow 稳态，再改到 kFast 走 xfadeBlocks 块交叉淡化（还没走完）
void warmIntoCrossfade(Rig& rig, int xfadeBlocks)
{
    rig.setSignal([](std::int64_t) { return kLevel; });
    rig.run(8192, {kBlock}, timeParams(kSlow));
    REQUIRE(rig.trace.back() == kLevel);
    rig.run(static_cast<std::int64_t>(xfadeBlocks) * kBlock, {kBlock}, timeParams(kFast));
    REQUIRE(rig.status().delayXfadeLeft > 0);
}

} // namespace

TEST_CASE("重同步叠重同步 交叉淡化中 5 ms 内连续两次换段：仍在淡出的读头读自己的尾巴，回送没有阶跃",
          "[timing][resync][overlap]")
{
    // ⛔ 删除式验证点：尾巴格不跟读头走（后一次重同步按「未淡出读头的顺序」占第 0 格）
    //    → 新读头的预拷贝盖掉旧读头的尾巴；旧读头在换段之后只能从尾巴读，读到 0，
    //    回送从大半幅度硬掉，本用例变红。
    Rig rig(48000.0, 32);
    Rings rings;
    warmIntoCrossfade(rig, 20);
    const EngineParams fast = timeParams(kFast);
    const std::size_t mark = rig.trace.size();

    // 第一次换段：旧读头（D = 160）与新读头（D = 96）各挂 5 ms 淡出，新环上建第三个读头
    rings.switchTo(rig, 30000);
    rig.run(6 * kBlock, {kBlock}, fast); // 48 个样本之后，两个读头都还在淡出
    // 第二次换段：新环上刚建的读头已有一点数据，它的预拷贝不能压到前两个读头的尾巴
    rings.switchTo(rig, 40000);
    rig.run(6 * kBlock, {kBlock}, fast);
    CHECK(rig.status().resyncByReason[static_cast<std::size_t>(SyncReason::kBindSwitch)] == 2u);

    // 一路淡完，再等新环上的读头 onset 走完
    rig.run(4096, {kBlock}, fast);
    CHECK(maxStep(rig.trace, mark - 1) <= kSlope + 1.0e-6f);
    CHECK(rig.trace.back() == kLevel);
    CHECK(rig.status().delayEffective == kFast);
    CHECK(rig.status().delayXfadeLeft == 0);
}

TEST_CASE("重同步叠重同步 5 ms 内连续三次换段：槽用完时收掉包络最小的读头，回送没有阶跃，被收掉的增益有上界",
          "[timing][resync][overlap]")
{
    // ⛔ 删除式验证点：收掉的不是包络最小的读头（改成收第一个）
    //    → 被收掉的是仍在淡出的大读头，增益超过上界，本用例变红。
    Rig rig(48000.0, 32);
    Rings rings;
    warmIntoCrossfade(rig, 20);
    const EngineParams fast = timeParams(kFast);
    const std::size_t mark = rig.trace.size();

    rings.switchTo(rig, 30000);
    rig.run(4 * kBlock, {kBlock}, fast);
    rings.switchTo(rig, 40000);
    rig.run(4 * kBlock, {kBlock}, fast);
    rings.switchTo(rig, 50000);
    rig.run(4 * kBlock, {kBlock}, fast);
    CHECK(rig.status().resyncByReason[static_cast<std::size_t>(SyncReason::kBindSwitch)] == 3u);

    rig.run(4096, {kBlock}, fast);
    CHECK(maxStep(rig.trace, mark - 1) <= kSlope + 1.0e-6f);
    CHECK(rig.trace.back() == kLevel);

    // 三次换段需要第 4、第 5 个读头：每次都收掉一个；被收掉的包络不超过 1/4（事件 b = 包络 × 255）
    CHECK(rig.status().tapDrop == 2u);
    int seen = 0;
    for (const LoggedEvent& e : rig.events)
    {
        if (e.code == static_cast<std::uint16_t>(LoopEventCode::kTapDropped))
        {
            ++seen;
            CHECK(e.b <= 64u);
        }
    }
    CHECK(seen == 2);
}

TEST_CASE("重同步叠重同步 交叉淡化后段两次跳变：槽用完时收掉的读头已有一点数据，它的末值接成衰减的保持，没有阶跃",
          "[timing][resync][overlap]")
{
    // ⛔ 删除式验证点：被收掉的读头在上一个样本上的值不接成末值保持（直接消失）
    //    → 它那份约 5 % 的增益在收掉的那一块首硬掉，回送出现一处比淡变斜率大得多的阶跃，本用例变红。
    // 几何：D 从 960 改到 160，交叉淡化走到约 61 %。跳变一（同时目标改到 96）让两个交叉淡化读头各挂淡出，
    // 建出 D = 96 的新读头。144 个样本之后跳变二：三个读头都在场，新读头已读了 48 个样本的数据
    // （包络约 5 %），是其中最小的，被收掉。
    Rig rig(48000.0, 32);
    rig.setSignal([](std::int64_t) { return kLevel; });
    rig.run(8192, {kBlock}, timeParams(960));
    rig.run(147 * kBlock, {kBlock}, timeParams(kSlow));
    REQUIRE(rig.status().delayXfadeLeft > 0);
    const std::size_t mark = rig.trace.size();

    rig.write(9000); // 跳变一：超过跳变门限 8192
    rig.run(18 * kBlock, {kBlock}, timeParams(kFast));
    CHECK(rig.status().tapDrop == 0u);
    rig.write(9000); // 跳变二：槽已满
    rig.run(24 * kBlock, {kBlock}, timeParams(kSlow));
    CHECK(rig.status().resyncByReason[static_cast<std::size_t>(SyncReason::kJump)] == 2u);
    CHECK(rig.status().tapDrop == 1u);

    rig.run(30000, {kBlock}, timeParams(kSlow));
    CHECK(maxStep(rig.trace, mark - 1) <= kSlope + 1.0e-6f);
    CHECK(rig.trace.back() == kLevel);
    CHECK(rig.status().delayEffective == kSlow);

    // 被收掉的读头：包络约 5 %，远小于 1/4
    unsigned dropped = 0;
    for (const LoggedEvent& e : rig.events)
    {
        if (e.code == static_cast<std::uint16_t>(LoopEventCode::kTapDropped))
        {
            dropped = e.b;
        }
    }
    CHECK(dropped >= 8u);
    CHECK(dropped <= 64u);
}

TEST_CASE("重同步叠重同步 算账块之后收掉读头：不把过时的末值接成保持，只读过的位置改了也不影响之后的输出",
          "[timing][resync][overlap]")
{
    // ⛔ 删除式验证点：收掉读头时不看「它的末值是不是上一个样本的」→ 算账块里没有更新过的旧末值被接成保持，
    //    之后的输出带上了那个读头早已读过的位置的数据，两次运行的输出不再逐位相同，本用例变红。
    // 几何同上一条：跳变一之后新读头（D = 96）读了 24 个样本的数据，其中位置 W + 20…30 在第二次运行里被改动；
    // 然后 3 个算账块（不出样本）；跳变二收掉这个读头。这些位置此后没有任何读头会再读到。
    const auto play = [](bool perturbed) {
        auto holder = std::make_unique<Rig>(48000.0, 32);
        Rig& rig = *holder;
        const auto lo = std::make_shared<std::int64_t>(0);
        rig.setSignal([perturbed, lo](std::int64_t p) {
            const bool inside = perturbed && *lo > 0 && p >= *lo + 20 && p < *lo + 30;
            return kLevel + (inside ? 0.4f : 0.0f);
        });
        rig.run(8192, {kBlock}, timeParams(960));
        rig.run(147 * kBlock, {kBlock}, timeParams(kSlow));
        *lo = static_cast<std::int64_t>(rig.trace.size()) + 9000; // 跳变一之后的写头
        rig.write(9000);
        rig.run(15 * kBlock, {kBlock}, timeParams(kFast)); // 新读头读了 24 个样本
        for (int i = 0; i < 3; ++i)
        {
            rig.step(kBlock, timeParams(kFast), TransportInfo{}, false); // 算账块
        }
        const std::size_t mark = rig.trace.size();
        rig.write(9000); // 跳变二：槽已满，收掉新读头
        rig.run(24 * kBlock, {kBlock}, timeParams(kSlow));
        rig.run(2000, {kBlock}, timeParams(kSlow));
        REQUIRE(rig.status().tapDrop == 1u);
        return std::make_pair(std::move(holder), mark);
    };
    const auto base = play(false);
    const auto changed = play(true);
    REQUIRE(base.second == changed.second);
    const std::vector<float> a(base.first->trace.begin() + static_cast<std::ptrdiff_t>(base.second),
                               base.first->trace.end());
    const std::vector<float> b(changed.first->trace.begin() + static_cast<std::ptrdiff_t>(changed.second),
                               changed.first->trace.end());
    CHECK(sameBits(a, b));
}

TEST_CASE("重同步叠重同步 切块不变性：重同步落在同一个块首时，整块、小块、逐样本的输出逐位相同，收掉的读头也相同",
          "[timing][resync][overlap][invariance]")
{
    // ⛔ 删除式验证点：收掉读头时的末值保持按「块」而不是按样本衰减，或读头选择依赖块长
    //    → 不同切法的输出不同，本用例变红。
    // 跳变发生在块与块之间；每一段的长度是 16 的倍数，各种切法在这些点上有共同的块边界，
    // 所以重同步落在同一个样本上（判据只在块首看一次，落在块中间的哪个样本是宿主切块决定的）。
    // 块长不超过声明值。换段不在此列：换段时旧读头只读到上一块拉到的位置（ADR 的既有口径），
    // 数据只够 D 个样本的短读头，淡出长度要看上一块切得多长。
    const std::vector<std::vector<int>> splits = {{16}, {8}, {5, 11, 3}, {1}, {32}, {7, 2, 31}};
    const auto play = [](const std::vector<int>& sizes) {
        auto holder = std::make_unique<Rig>(48000.0, 32);
        Rig& rig = *holder;
        rig.setSignal([](std::int64_t p) { return noiseAt(p); });
        rig.run(8192, sizes, timeParams(960));
        rig.run(1184, sizes, timeParams(kSlow)); // 交叉淡化走到约 61 %
        rig.write(9000); // 跳变一
        rig.run(144, sizes, timeParams(kFast));
        rig.write(9000); // 跳变二：槽用完
        rig.run(48, sizes, timeParams(kSlow));
        rig.write(9000); // 跳变三
        rig.run(32, sizes, timeParams(kSlow));
        rig.run(20000, sizes, timeParams(kSlow));
        return holder;
    };
    const auto whole = play(splits.front());
    CHECK(whole->status().tapDrop >= 1u);
    CHECK(whole->status().resyncCount >= 3u);
    for (const auto& sizes : splits)
    {
        const auto other = play(sizes);
        INFO("first block " << sizes.front() << " firstDiff " << firstDiff(other->trace, whole->trace));
        CHECK(sameBits(other->trace, whole->trace));
        CHECK(other->status().tapDrop == whole->status().tapDrop);
        CHECK(other->status().resyncCount == whole->status().resyncCount);
        CHECK(other->status().delayEffective == whole->status().delayEffective);
        CHECK(other->count(LoopEventCode::kTapDropped) == whole->count(LoopEventCode::kTapDropped));
    }
}

TEST_CASE(
    "重同步叠重同步 随机风暴（换段与跳变混着来，穿插改 D）：回送始终没有阶跃，被收掉的增益不超过 1/4，最终回到稳态",
    "[timing][resync][overlap][storm]")
{
    // ⛔ 删除式验证点：尾巴格不跟读头走，或被收掉的读头不接末值保持
    //    → 个别试验里回送出现比淡变斜率大得多的阶跃，本用例变红；
    //    收掉的不是包络最小的读头 → 被收掉的增益超过 1/4，本用例变红。
    // 每次试验：稳态 D 改到另一档（交叉淡化走到随机位置）→ 2 到 5 次重同步（换段或跳变，
    // 间隔 8 到 240 个样本），重同步之间可能又改 D。同时在场的读头至多 3 个，
    // 每个的斜率不超过 增益/淡出步数（淡出步数至少 min(D, 240)）。
    const std::int64_t ds[] = {96, 160, 480, 960};
    std::uint32_t state = 12345u;
    const auto rnd = [&](std::uint32_t n) {
        state = state * 1664525u + 1013904223u;
        return (state >> 8) % n;
    };
    int totalDrops = 0;
    for (int trial = 0; trial < 60; ++trial)
    {
        Rig rig(48000.0, 32);
        Rings rings;
        rig.setSignal([](std::int64_t) { return kLevel; });
        std::int64_t d = ds[rnd(4)];
        rig.run(8192, {kBlock}, timeParams(d));
        d = ds[rnd(4)];
        rig.run(static_cast<std::int64_t>(rnd(200)) * kBlock, {kBlock}, timeParams(d));
        const std::size_t mark = rig.trace.size();
        std::int64_t writeHead = 30000;
        std::int64_t minD = d;
        const int count = 2 + static_cast<int>(rnd(4));
        for (int k = 0; k < count; ++k)
        {
            if (rnd(2) == 0)
            {
                rings.switchTo(rig, writeHead);
                writeHead += 10000;
            }
            else
            {
                rig.write(9000);
            }
            if (rnd(3) == 0)
            {
                d = ds[rnd(4)];
                minD = std::min(minD, d);
            }
            rig.run(static_cast<std::int64_t>(1 + rnd(30)) * kBlock, {kBlock}, timeParams(d));
        }
        rig.run(30000, {kBlock}, timeParams(d));

        INFO("trial " << trial << " minD " << minD);
        const float bound =
            3.0f * kLevel / static_cast<float>(std::min<std::int64_t>(minD, 240)) + 3.0f * kLevel / 960.0f;
        CHECK(maxStep(rig.trace, mark - 1) <= bound + 1.0e-6f);
        CHECK(rig.trace.back() == kLevel);
        for (const LoggedEvent& e : rig.events)
        {
            if (e.code == static_cast<std::uint16_t>(LoopEventCode::kTapDropped))
            {
                ++totalDrops;
                CHECK(e.b <= 64u);
            }
        }
    }
    CHECK(totalDrops > 0); // 风暴确实挤满过读头槽
}
