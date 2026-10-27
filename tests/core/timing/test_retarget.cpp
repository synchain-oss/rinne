// SPDX-License-Identifier: GPL-3.0-or-later
//
// 改 D 的 100 ms 前沿限速：
//   - 第一次改动立即开始交叉淡化（前沿）；
//   - 窗口里的改动合并，只认最新的目标；窗口到点时目标与当前不同就再淡一次，最终停在最新的目标上；
//   - 两次起点相隔恰好 100 ms 的样本数（按样本计，不读墙钟），起点可以落在块中间；
//   - 任何时刻至多一段交叉淡化；
//   - 0 样本的调用不推进窗口。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "TimingKit.h"
#include "loop/DelayRetarget.h"

namespace
{

using namespace rinne::loop;
using namespace rinne::timing_test;

constexpr std::int64_t kD0 = 9600; // 200 ms @ 48 kHz
constexpr std::int64_t kD1 = 12000;
constexpr std::int64_t kD2 = 14400;
constexpr std::int64_t kD3 = 7200;

// 暖机：D = kD0 稳态，回送已经出声。
void warm(Rig& rig, std::int64_t samples = kD2 + 2048) // 所有的 D 都有数据可读
{
    rig.run(samples, {256}, timeParams(kD0));
}

// 纯逻辑的块循环：want 为真时每块都问「最早能在哪个样本开始」，开始了就 noteStart。返回起点（绝对样本）。
std::vector<std::int64_t> simulateStarts(RetargetLimiter& limiter, const std::vector<int>& sizes, std::int64_t total,
                                         std::int64_t wantFrom)
{
    std::vector<std::int64_t> starts;
    std::int64_t t = 0;
    std::size_t k = 0;
    while (t < total)
    {
        const std::int64_t n = std::min<std::int64_t>(sizes[k % sizes.size()], total - t);
        ++k;
        // 在 wantFrom 之前目标没变；变了之后每一块都想开始，开始了就是新的一段
        if (t + n > wantFrom)
        {
            const std::int64_t off = limiter.startOffset(n);
            if (off >= 0)
            {
                const std::int64_t at = t + off;
                if (at >= wantFrom)
                {
                    starts.push_back(at);
                    limiter.noteStart(off);
                }
            }
        }
        limiter.endBlock(n);
        t += n;
    }
    return starts;
}

} // namespace

TEST_CASE("限速 窗口长度按样本换算：100 ms，各采样率", "[timing][retarget]")
{
    // ⛔ 删除式验证点：窗口写成固定样本数或读墙钟 → 非 48 kHz 下长度不对，本用例变红。
    const struct
    {
        double sr;
        std::int64_t window;
    } rows[] = {{44100.0, 4410}, {48000.0, 4800}, {88200.0, 8820}, {96000.0, 9600}, {192000.0, 19200}};
    for (const auto& row : rows)
    {
        RetargetLimiter limiter;
        limiter.prepare(row.sr);
        INFO("sr = " << row.sr);
        CHECK(limiter.windowSamples() == row.window);
        // 前沿：没有最近一次起点时立即开始
        CHECK(limiter.startOffset(64) == 0);
    }
}

TEST_CASE("限速 起点相隔恰好一个窗口，与块怎么切无关（起点可落在块中间）", "[timing][retarget]")
{
    // ⛔ 删除式验证点：窗口只在块边界判（到点那一块的块首才开始）→ 块长 1000 时第二个起点落在块首 5000 而不是 4800，
    //    本用例变红；窗口比 4800 长或短一个样本 → 本用例变红。
    const std::vector<std::vector<int>> splits = {{64}, {1000}, {97, 255, 4000, 3}, {1}, {4799, 1, 4800, 2}};
    for (const auto& sizes : splits)
    {
        RetargetLimiter limiter;
        limiter.prepare(48000.0);
        // 从第 0 个样本起一直想改 D（目标一直在变）：第一次立即，之后每个窗口一次
        const std::vector<std::int64_t> starts = simulateStarts(limiter, sizes, 20000, 0);
        INFO("首块长 " << sizes[0]);
        REQUIRE(starts.size() >= 4u);
        CHECK(starts[0] == 0);
        for (std::size_t i = 1; i < starts.size(); ++i)
        {
            CHECK(starts[i] - starts[i - 1] == 4800);
        }
    }
}

TEST_CASE("限速 前沿：第一次改动立即开始，当块的第一个样本就是起点", "[timing][retarget]")
{
    // ⛔ 删除式验证点：第一次也要等满一个窗口（把「没有最近一次起点」当成刚开始）→ 起点推迟 100 ms，本用例变红。
    Rig rig;
    warm(rig);
    const auto mark = static_cast<std::int64_t>(rig.trace.size());
    rig.run(4000, {128}, timeParams(kD1));
    REQUIRE(rig.xfadeStarts().size() == 1u);
    CHECK(rig.xfadeStarts()[0] == mark);
    CHECK(rig.count(LoopEventCode::kRetargetHeld) == 0u);
}

TEST_CASE("限速 窗口里的改动合并：只认最新的目标，起点相隔 100 ms，最终停在最新的 D 上", "[timing][retarget]")
{
    // ⛔ 删除式验证点：窗口到点时向第一次的目标而不是最新的目标淡 → 最终停在 kD1，本用例变红；
    //    窗口里的每次改动各起一段 → 起点不止两个，本用例变红。
    Rig rig;
    warm(rig);
    const auto mark = static_cast<std::int64_t>(rig.trace.size());
    rig.run(512, {64}, timeParams(kD1)); // 第一次：立即开始
    rig.run(1024, {64}, timeParams(kD2)); // 窗口里：先挂着
    rig.run(1024, {64}, timeParams(kD3)); // 再改：合并
    rig.run(1024, {64}, timeParams(kD2)); // 最新的目标
    rig.run(12000, {64}, timeParams(kD2));

    const std::vector<std::int64_t> starts = rig.xfadeStarts();
    REQUIRE(starts.size() == 2u);
    CHECK(starts[0] == mark);
    CHECK(starts[1] - starts[0] == 4800);
    CHECK(rig.count(LoopEventCode::kRetargetHeld) == 1u); // 一次挂起只发一条
    const LoopStatusFields fields = rig.status();
    CHECK(fields.delayEffective == kD2);
    CHECK(fields.delayTarget == kD2);
    CHECK(fields.delayXfadeLeft == 0);
    CHECK(fields.tapDrop == 0u);
    // 第二段淡的是最新的目标：回送最后读的是 kD2 位置上的信号
    const auto last = static_cast<std::int64_t>(rig.trace.size()) - 1;
    CHECK(rig.trace.back() == noiseAt(last - kD2));
}

TEST_CASE("限速 窗口到点时目标与当前一致就不再淡：改过去又改回来，只有第一段", "[timing][retarget]")
{
    // ⛔ 删除式验证点：窗口到点无条件再淡一次（不比较目标与当前）→ 起点有两个，本用例变红。
    Rig rig;
    warm(rig);
    rig.run(512, {64}, timeParams(kD1)); // 第一次：立即开始，目标 kD1
    rig.run(1024, {64}, timeParams(kD2)); // 窗口里改去 kD2
    rig.run(12000, {64}, timeParams(kD1)); // 又改回 kD1：窗口到点时目标就是当前
    CHECK(rig.xfadeStarts().size() == 1u);
    CHECK(rig.status().delayEffective == kD1);
    CHECK(rig.count(LoopEventCode::kRetargetHeld) == 1u);
    // 窗口过去之后，再改一次立即开始（前沿重新生效）
    const auto mark = static_cast<std::int64_t>(rig.trace.size());
    rig.run(2048, {64}, timeParams(kD2));
    REQUIRE(rig.xfadeStarts().size() == 2u);
    CHECK(rig.xfadeStarts()[1] == mark);
}

TEST_CASE("限速 窗口恰好到点那一刻改动立即开始，差一个样本就要等", "[timing][retarget]")
{
    // ⛔ 删除式验证点：窗口边界写成 >（多等一个样本）或 >= window − 1 → 起点错一个样本，本用例变红。
    for (const int early : {0, 1})
    {
        Rig rig;
        warm(rig);
        const auto first = static_cast<std::int64_t>(rig.trace.size());
        rig.run(64, {64}, timeParams(kD1)); // 起点 = first
        // 到第一个窗口到点的位置前 early 个样本的块边界处改 D：块长 1，精确落点
        rig.run(4800 - 64 - early, {64}, timeParams(kD1)); // 不改，推进
        const auto changeAt = static_cast<std::int64_t>(rig.trace.size());
        REQUIRE(changeAt == first + 4800 - early);
        rig.run(2048, {1}, timeParams(kD2)); // 逐样本推进，起点必然是第一个满足窗口的样本
        REQUIRE(rig.xfadeStarts().size() == 2u);
        CHECK(rig.xfadeStarts()[1] == first + 4800);
    }
}

TEST_CASE("限速 采样率不同窗口按样本换算：44.1 kHz 为 4410，96 kHz 为 9600", "[timing][retarget]")
{
    // ⛔ 删除式验证点：窗口固定 4800 个样本 → 非 48 kHz 下起点间隔不对，本用例变红。
    for (const double sr : {44100.0, 96000.0})
    {
        INFO("sr = " << sr);
        Rig rig(sr, 1024);
        const auto d0 = static_cast<std::int64_t>(0.200 * sr);
        EngineParams params;
        params.delayMs = 200.0f;
        rig.run(d0 + 4096, {256}, params);
        const auto mark = static_cast<std::int64_t>(rig.trace.size());
        params.delayMs = 250.0f;
        rig.run(1000, {64}, params);
        params.delayMs = 300.0f;
        rig.run(static_cast<std::int64_t>(0.5 * sr), {64}, params);
        const std::vector<std::int64_t> starts = rig.xfadeStarts();
        REQUIRE(starts.size() == 2u);
        CHECK(starts[0] == mark);
        CHECK(starts[1] - starts[0] == static_cast<std::int64_t>(std::llround(0.100 * sr)));
    }
}

TEST_CASE("限速 同一时刻至多一段交叉淡化：任何一块里读头集合的交叉淡化剩余不会超过一个总长", "[timing][retarget]")
{
    // ⛔ 删除式验证点：交叉淡化进行中又开一段（不看 crossfadeActive）→ 剩余量超过一个总长或读头槽被占满丢读头，
    //    本用例变红。
    Rig rig(48000.0, 1024);
    warm(rig);
    std::int64_t worst = 0;
    std::int64_t target = kD0;
    for (int block = 0; block < 600; ++block)
    {
        // 目标每 10 块换一次，窗口远大于换的间隔
        target = (block / 10 % 2 == 0) ? kD1 : kD2;
        rig.step(64, timeParams(target));
        worst = std::max(worst, rig.status().delayXfadeLeft);
    }
    CHECK(worst <= 1920);
    CHECK(rig.status().tapDrop == 0u);
    // 600 块 × 64 = 38400 个样本 = 8 个窗口：起点不超过 9 个，相邻起点至少相隔一个窗口
    const std::vector<std::int64_t> starts = rig.xfadeStarts();
    REQUIRE(starts.size() >= 2u);
    CHECK(starts.size() <= 9u);
    for (std::size_t i = 1; i < starts.size(); ++i)
    {
        CHECK(starts[i] - starts[i - 1] >= 4800);
    }
}

TEST_CASE("限速 新读头继承旧读头的同步偏移：写方在前（偏移 256）时改 D，淡完之后读的是 位置 + 偏移 − 新 D",
          "[timing][retarget]")
{
    // ⛔ 删除式验证点：新读头的同步偏移不继承旧读头（落成 0）→ 淡完之后读的位置差 256 个样本，本用例变红。
    //    Input 在前的驱动器偏移恒为 0，这条只有写方在前的顺序才能区分。
    Rig rig;
    const EngineParams initial = timeParams(kD0);
    for (int done = 0; done < kD2 + 2048; done += 256)
    {
        rig.stepOutputFirst(256, initial);
    }
    REQUIRE(rig.status().offset == 256);
    const auto mark = static_cast<std::int64_t>(rig.trace.size());
    const EngineParams next = timeParams(kD1);
    for (int done = 0; done < 4096; done += 256) // 块长不变：偏移 = 块长，换块长会让偏移重新锚定
    {
        rig.stepOutputFirst(256, next);
    }
    REQUIRE(rig.xfadeStarts().size() == 1u);
    CHECK(rig.xfadeStarts()[0] == mark);
    CHECK(rig.status().offset == 256);
    CHECK(rig.status().delayEffective == kD1);
    CHECK(rig.status().delayXfadeLeft == 0);
    // 淡完之后（40 ms = 1920 个样本）每个样本都读 位置 + 偏移 − 新 D，逐位相同
    const auto settled = mark + 1920;
    REQUIRE(static_cast<std::int64_t>(rig.trace.size()) > settled + 1024);
    for (auto t = settled; t < static_cast<std::int64_t>(rig.trace.size()); ++t)
    {
        INFO("t = " << t);
        REQUIRE(rig.trace[static_cast<std::size_t>(t)] == noiseAt(t + 256 - kD1));
    }
}

TEST_CASE("限速 0 样本的调用不推进窗口，也不改变任何输出", "[timing][retarget]")
{
    // ⛔ 删除式验证点：0 样本的调用也推进限速窗口 → 起点提前，输出与不插零样本的运行不同，本用例变红。
    const auto run = [](bool withZero) {
        auto holder = std::make_unique<Rig>();
        Rig& rig = *holder;
        warm(rig);
        EngineParams params = timeParams(kD1);
        for (int block = 0; block < 200; ++block)
        {
            rig.step(64, params);
            if (withZero && block % 3 == 0)
            {
                rig.feed(0, params);
            }
            if (block == 20)
            {
                params = timeParams(kD2);
            }
        }
        return holder;
    };
    const auto plainRun = run(false);
    const auto zeroRun = run(true);
    const Rig& plain = *plainRun;
    const Rig& zero = *zeroRun;
    // 0 样本的调用没有产出：把它们从轨迹里去掉再比
    CHECK(plain.xfadeStarts() == zero.xfadeStarts());
    CHECK(plain.processed() == zero.processed());
    CHECK(sameBits(plain.trace, zero.trace));
}
