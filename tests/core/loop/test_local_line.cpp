// SPDX-License-Identifier: GPL-3.0-or-later
//
// 本地延迟线：搬进与读出、有效区间、清空、尾巴快照。
//
// 这里测的是 LocalLine 的几何与「绝不读旧数据」这条安全不变量：
//   - 落在有效区间之外的位置一律读成 0；
//   - 清空之后旧样本读不出来（清空 = 有效区间归零，不是 memset，也不是只清 pulled）；
//   - 尾巴快照是预拷贝：拷完之后那一段不再依赖主区。
//
// 用例编号：L1-04（清空）与它周边的几何。

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "loop/LocalLine.h"

namespace
{

using namespace rinne::loop;

constexpr double kSr = 48000.0;

// 把一帧按位置写进延迟线。位置用编码过的浮点，读回来能对上第几帧。
float encode(std::int64_t p) noexcept
{
    return static_cast<float>(p & 0xFFFFFF);
}

// 往延迟线里搬 [from, from + count) 帧，每帧的值是它的位置。
void fill(LocalLine& line, std::int64_t from, std::int64_t count)
{
    line.pull(from, from + count, [&](std::int64_t p, float& l, float& r) {
        l = encode(p);
        r = -encode(p);
    });
}

} // namespace

TEST_CASE("loop line 容量是 2 的幂，且容得下 D 的上限加一个最大块长")
{
    REQUIRE(lineFramesFor(44100.0) == (1 << 18));
    REQUIRE(lineFramesFor(48000.0) == (1 << 18));
    REQUIRE(lineFramesFor(96000.0) == (1 << 19));
    REQUIRE(lineFramesFor(192000.0) == (1 << 20));

    // 容得下 4 s（D 的上限）加一个最大块长（32768）。
    REQUIRE(lineFramesFor(48000.0) >= 4 * 48000 + 32768);
}

TEST_CASE("loop line L1-01 负位置与未拉取的位置一律读成 0")
{
    LocalLine line;
    REQUIRE(line.allocate(kSr));
    REQUIRE(line.ready());

    fill(line, 0, 100);

    float l = 1.0f;
    float r = 1.0f;
    // 负位置：读不成。
    REQUIRE_FALSE(line.read(-1, l, r));
    // 超过 pulled：读不成。
    REQUIRE_FALSE(line.read(100, l, r));
    // 有效区间里：读得到，且值对得上位置。
    REQUIRE(line.read(42, l, r));
    REQUIRE(l == encode(42));
    REQUIRE(r == -encode(42));
}

TEST_CASE("loop line L1-03 from > pulled：还没拉进来的区间读成 0")
{
    LocalLine line;
    REQUIRE(line.allocate(kSr));

    fill(line, 0, 100);

    // 从 150 开始拉：100..149 从不存在过。
    float l = 1.0f;
    float r = 1.0f;
    REQUIRE_FALSE(line.read(120, l, r));
    REQUIRE(line.pulled() == 100);
}

TEST_CASE("loop line L1-04 清空后旧样本读不出来")
{
    // ⛔ 删除式验证点：把清空改成「只清 pulled 不清 lineValidFrom」→ 本用例变红。
    LocalLine line;
    REQUIRE(line.allocate(kSr));

    fill(line, 0, 100);
    float l = 0.0f;
    float r = 0.0f;
    REQUIRE(line.read(42, l, r));
    REQUIRE(l == encode(42));

    // 清空。
    line.reset();

    // 清空之后：pulled 归零、lineValidFrom 归零。
    REQUIRE(line.pulled() == 0);
    REQUIRE(line.lineValidFrom() == 0);

    // 旧样本读不出来：位置 42 落在有效区间外（>= pulled）。
    REQUIRE_FALSE(line.read(42, l, r));
    // 负位置也读不出来。
    REQUIRE_FALSE(line.read(-1, l, r));

    // 清空不是 memset：缓冲区里的样本还在，只是读不出来。
    // 所以清空之后重新拉同一段，读到的是新拉进来的值，不是旧值。
    fill(line, 0, 100);
    REQUIRE(line.read(42, l, r));
    REQUIRE(l == encode(42));
}

TEST_CASE("loop line 清空之后重新拉取的位置从 0 开始算")
{
    LocalLine line;
    REQUIRE(line.allocate(kSr));

    fill(line, 100, 50);
    REQUIRE(line.pulled() == 150);

    line.reset();
    REQUIRE(line.pulled() == 0);

    // 清空之后拉进来的帧从 0 开始算起。
    fill(line, 0, 10);
    REQUIRE(line.pulled() == 10);

    float l = 0.0f;
    float r = 0.0f;
    REQUIRE(line.read(5, l, r));
    REQUIRE(l == encode(5));
}

TEST_CASE("loop line 尾巴快照是预拷贝：拷完之后不再依赖主区")
{
    LocalLine line;
    REQUIRE(line.allocate(kSr));

    fill(line, 0, 200);

    // 快照主区最后 100 帧。
    REQUIRE(line.snapshotTail(100));
    REQUIRE(line.tailStart() == 100);
    REQUIRE(line.tailCount() == 100);

    // 从快照里读得到，而且值对得上位置。
    float l = 0.0f;
    float r = 0.0f;
    REQUIRE(line.readTail(150, l, r));
    REQUIRE(l == encode(150));
    REQUIRE(r == -encode(150));

    // 快照范围之外读不到。
    REQUIRE_FALSE(line.readTail(50, l, r));
    REQUIRE_FALSE(line.readTail(250, l, r));
}

TEST_CASE("loop line 尾巴快照可以按区间拷（重同步时的预拷贝尾巴）")
{
    LocalLine line;
    REQUIRE(line.allocate(kSr));

    fill(line, 0, 200);

    // 拷 [50, 100)：这一段不在主区的末尾（读头落后写头 D 个样本时就是这样）。
    REQUIRE(line.snapshotRange(50, 50));
    REQUIRE(line.tailStart() == 50);
    REQUIRE(line.tailCount() == 50);

    float l = 0.0f;
    float r = 0.0f;
    REQUIRE(line.readTail(75, l, r));
    REQUIRE(l == encode(75));

    // 区间太早（落在有效区间之外）时没有可拷的帧。
    line.reset();
    REQUIRE_FALSE(line.snapshotRange(50, 50));
}

TEST_CASE("loop line 两格尾巴互不覆盖：改 D 的交叉淡化里两个读头同时重同步，各淡各的")
{
    // ⛔ 删除式验证点：第二格拷贝写进第 0 格（或 readTail 只查第 0 格）→ 另一个读头的尾巴被覆盖，本用例变红。
    LocalLine line;
    REQUIRE(line.allocate(kSr));
    fill(line, 0, 400);

    REQUIRE(line.snapshotRange(50, 50, 0));
    REQUIRE(line.snapshotRange(250, 60, 1));
    REQUIRE(line.tailStart(0) == 50);
    REQUIRE(line.tailCount(0) == 50);
    REQUIRE(line.tailStart(1) == 250);
    REQUIRE(line.tailCount(1) == 60);

    float l = 0.0f;
    float r = 0.0f;
    REQUIRE(line.readTail(75, l, r));
    REQUIRE(l == encode(75));
    REQUIRE(line.readTail(300, l, r));
    REQUIRE(l == encode(300));
    REQUIRE(r == -encode(300));
    // 两格之间与之外读不到
    REQUIRE_FALSE(line.readTail(150, l, r));
    REQUIRE_FALSE(line.readTail(310, l, r));

    // 越界的格号当第 0 格；清空之后两格都读不出来
    REQUIRE(line.snapshotRange(100, 20, 7));
    REQUIRE(line.tailStart(0) == 100);
    REQUIRE(line.tailStart(1) == 250);
    line.reset();
    REQUIRE_FALSE(line.readTail(105, l, r));
    REQUIRE_FALSE(line.readTail(300, l, r));
}

TEST_CASE("loop line 同采样率重新 allocate 什么都不动，流保持连续")
{
    LocalLine line;
    REQUIRE(line.allocate(kSr));

    fill(line, 0, 100);
    const std::int64_t pulledBefore = line.pulled();

    // 同采样率：什么都不动。
    REQUIRE(line.allocate(kSr));
    REQUIRE(line.pulled() == pulledBefore);

    float l = 0.0f;
    float r = 0.0f;
    REQUIRE(line.read(42, l, r));
    REQUIRE(l == encode(42));
}

TEST_CASE("loop line 有效区间的下界可以抬高（环覆盖了更早的帧）")
{
    LocalLine line;
    REQUIRE(line.allocate(kSr));

    fill(line, 0, 100);

    // 抬高到 50：50 之前的帧可能被环覆盖了，一律读成 0。
    line.raiseValidFrom(50);

    float l = 1.0f;
    float r = 1.0f;
    REQUIRE_FALSE(line.read(49, l, r));
    REQUIRE(line.read(50, l, r));
    REQUIRE(l == encode(50));
}

TEST_CASE("loop line 清空并锚在写头：比锚点早的帧读不出来，之后从锚点起往后拉")
{
    // ⛔ 删除式验证点：清空之后把起点留在 0（不锚在写头）→ 本用例变红。
    // 写头是段寿命内的绝对位置，配对或清空的那一刻可能早已很大：从 0 起拉永远追不上它。
    LocalLine line;
    REQUIRE(line.allocate(kSr));

    const std::int64_t far = (static_cast<std::int64_t>(1) << 40) + 12345;
    fill(line, 0, 100);
    line.resetTo(far);
    REQUIRE(line.pulled() == far);
    REQUIRE(line.lineValidFrom() == far);

    float l = 1.0f;
    float r = 1.0f;
    // 清空前的帧读不出来（同一个下标在新位置上也不算数）。
    REQUIRE_FALSE(line.read(42, l, r));
    REQUIRE_FALSE(line.read(far - 1, l, r));
    // 锚点本身还没拉进来。
    REQUIRE_FALSE(line.read(far, l, r));

    // 从锚点起往后拉一段：读得到，值对得上位置。
    fill(line, far, 64);
    REQUIRE(line.pulled() == far + 64);
    REQUIRE(line.read(far, l, r));
    REQUIRE(l == encode(far));
    REQUIRE(line.read(far + 63, l, r));
    REQUIRE(l == encode(far + 63));
}
