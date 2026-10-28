// SPDX-License-Identifier: GPL-3.0-or-later
//
// 起播沿的去抖：宿主走带中间短暂报停（Cubase 在实时与预取之间切换时实测连续 3 块、每块 256 帧）不算停走带，
// 恢复时不触发起播复位，回声尾巴连续；真的停走带之后再起播，复位照常。
//
// 判据（口径见 PlayDebounce.h 与 ADR-0010）：连续报停的样本数累计到阈值才确认停走带，阈值 = max(100 ms, 4 × 最大块长)，
// 按样本计。样本数在宿主不调用我们的时候不走，所以「只送了一块 isPlaying=false 就挂起」「停走带时一块都没送过」要靠
// 已有的两个信号区分：两端挂起超过 250 ms（flushAfterSuspend）与重新 prepare。
//
// 标 ⛔ 的是删除式验证点：把对应的实现改坏，本文件必须变红。
//   ⛔ 不去抖（报停一块就算停走带）→ 短暂报停那几例都变红；
//   ⛔ 阈值不随块长放宽（只用 100 ms）→ 大块长的那一例变红；
//   ⛔ 阈值写成 > 而不是 >=、或下限改了数 → 边界那一例变红；
//   ⛔ 挂起后不确认（flushAfterSuspend 不通知去抖）→ 两个反例变红；
//   ⛔ 重新 prepare 不确认 → prepare 那一例变红；一直在走时的重新 prepare 也确认 → 流连续那一例变红。

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "TimingKit.h"
#include "loop/PlayDebounce.h"

namespace
{

using namespace rinne::loop;
using namespace rinne::timing_test;

constexpr int kBlock = 256;
constexpr std::int64_t kSr = 48000;
constexpr std::int64_t kFloor = 4800; // 100 ms @ 48 kHz

// ---- 去抖本身（纯函数级）----

// 连续报停 blocks 块（每块 n 帧）后恢复，返回恢复的那一块是不是起播沿
bool resumeAfter(PlayDebounce& d, int blocks, std::int64_t n, std::int64_t largest)
{
    for (int i = 0; i < blocks; ++i)
    {
        REQUIRE_FALSE(d.observe(false, n, largest));
    }
    return d.observe(true, n, largest);
}

PlayDebounce running(std::int64_t n = kBlock, std::int64_t largest = kBlock)
{
    PlayDebounce d;
    d.prepare(static_cast<double>(kSr));
    REQUIRE(d.observe(true, n, largest)); // 第一个在走的块是起播沿
    for (int i = 0; i < 10; ++i)
    {
        REQUIRE_FALSE(d.observe(true, n, largest));
    }
    return d;
}

} // namespace

TEST_CASE("起播去抖 第一个在走的块是起播沿，一直在走不再有")
{
    PlayDebounce d;
    d.prepare(static_cast<double>(kSr));
    CHECK_FALSE(d.observe(false, kBlock, kBlock)); // 一直没走：没有沿
    CHECK_FALSE(d.observe(false, kBlock, kBlock));
    CHECK(d.observe(true, kBlock, kBlock));
    CHECK_FALSE(d.observe(true, kBlock, kBlock));
}

TEST_CASE("起播去抖 短暂报停：实测的 3 块 × 256 帧不算停走带，恢复时没有沿")
{
    // ⛔ 删除式验证点：不去抖 → 本用例变红。
    PlayDebounce d = running();
    CHECK_FALSE(resumeAfter(d, 3, kBlock, kBlock));
    CHECK(d.running());
    CHECK(d.stoppedSamples() == 0); // 恢复之后账清零，下一次报停重新数
    CHECK_FALSE(resumeAfter(d, 3, kBlock, kBlock)); // 连着来几次也一样
}

TEST_CASE("起播去抖 阈值的边界：100 ms 的下限按样本计，差一个样本不确认，够了就确认")
{
    // ⛔ 删除式验证点：>= 写成 >、或下限改了数 → 本用例变红。
    CHECK(PlayDebounce{}.threshold(0) == kFloor); // 没 prepare 时按 48 kHz 的默认
    {
        PlayDebounce d = running();
        CHECK(d.threshold(kBlock) == kFloor); // 4 × 256 = 1024 比 100 ms 小，下限说了算
        // 18 块 × 256 = 4608 < 4800：不确认
        CHECK_FALSE(resumeAfter(d, 18, kBlock, kBlock));
    }
    {
        PlayDebounce d = running();
        // 19 块 × 256 = 4864 ≥ 4800：确认，恢复时是起播沿
        CHECK(resumeAfter(d, 19, kBlock, kBlock));
    }
    {
        // 阈值本身：差一个样本不确认，恰好等于就确认
        PlayDebounce d = running(1, kBlock);
        REQUIRE_FALSE(d.observe(false, kFloor - 1, kBlock));
        CHECK_FALSE(d.observe(true, 1, kBlock)); // 4799 < 4800：没有沿
        REQUIRE_FALSE(d.observe(false, kFloor, kBlock));
        CHECK(d.observe(true, 1, kBlock)); // 恰好 4800：确认
    }
    {
        // 采样率换算：96 kHz 下限是 9600
        PlayDebounce d;
        d.prepare(96000.0);
        CHECK(d.threshold(0) == 9600);
    }
}

TEST_CASE("起播去抖 阈值随块长放宽：大缓冲下 3 块已经超过 100 ms，仍是短暂报停")
{
    // ⛔ 删除式验证点：阈值只用 100 ms → 本用例变红。
    constexpr std::int64_t kBig = 2048; // 约 43 ms 一块
    PlayDebounce d = running(kBig, kBig);
    REQUIRE(d.threshold(kBig) == 4 * kBig);
    static_assert(3 * kBig > kFloor, "3 块已经超过 100 ms 的下限，否则本例没有意义");
    CHECK_FALSE(resumeAfter(d, 3, kBig, kBig)); // 6144 < 8192
    CHECK(resumeAfter(d, 4, kBig, kBig)); // 8192 ≥ 8192：确认
    // 块长的口径是声明值与见过的最大块长里较大的：小块的报停也按大块算
    PlayDebounce e = running(kBlock, kBig);
    CHECK_FALSE(resumeAfter(e, 30, kBlock, kBig)); // 7680 < 8192
    CHECK(resumeAfter(e, 34, kBlock, kBig)); // 8704 ≥ 8192
}

TEST_CASE("起播去抖 挂起：只送了一块 isPlaying=false 就不再调用，恢复时起播复位不能被吞掉")
{
    // ⛔ 删除式验证点：挂起后不确认 → 本用例变红。
    {
        // 对照：同样的一块报停，没有挂起的证据，当作短暂报停
        PlayDebounce d = running();
        CHECK_FALSE(resumeAfter(d, 1, kBlock, kBlock));
    }
    {
        PlayDebounce d = running();
        REQUIRE_FALSE(d.observe(false, kBlock, kBlock));
        d.suspended();
        CHECK(d.observe(true, kBlock, kBlock));
    }
    {
        // 宿主停走带时一块 isPlaying=false 都没送过：挂起之后第一个在走的块照样是沿
        PlayDebounce d = running();
        d.suspended();
        CHECK(d.observe(true, kBlock, kBlock));
        CHECK_FALSE(d.observe(true, kBlock, kBlock));
    }
}

TEST_CASE("起播去抖 重新 prepare：报停已经开始就确认，一直在走则不动")
{
    // ⛔ 删除式验证点：prepare 不确认 → 第一段变红；一直在走时也确认 → 第二段变红。
    {
        PlayDebounce d = running();
        REQUIRE_FALSE(d.observe(false, kBlock, kBlock));
        d.prepared();
        CHECK(d.observe(true, kBlock, kBlock));
    }
    {
        PlayDebounce d = running();
        d.prepared(); // 同采样率重新 prepare：流保持连续，不当作新的起播
        CHECK_FALSE(d.observe(true, kBlock, kBlock));
    }
}

// ---- 环路层门面（LoopSession）----

namespace
{

constexpr std::int64_t kDelay = 4800; // D = 100 ms
constexpr int kSteps = 220;
constexpr int kPauseAt = 120; // 回声早已建立（D = 100 ms，已经跑了 30 多个 D 的一半以上）

// 一台 Rig 加一份走带：每一步给一块，记下每个起播复位点
struct Run
{
    explicit Run(std::int64_t declared = 1024) : rig(48000.0, declared), params(timeParams(kDelay)) {}

    void step(bool playing, int n = kBlock)
    {
        TransportInfo transport;
        transport.isPlaying = playing;
        const BlockResult r = rig.step(n, params, transport);
        if (r.resetDsp)
        {
            resets.push_back(r.resetAtSample);
        }
    }

    std::uint32_t playResyncs() const
    {
        return rig.status().resyncByReason[static_cast<std::size_t>(SyncReason::kPlay)];
    }

    // 全程在走：对照
    void playAll(int steps = kSteps)
    {
        for (int i = 0; i < steps; ++i)
        {
            step(true);
        }
    }

    Rig rig;
    EngineParams params;
    std::vector<std::int64_t> resets;
};

// 回送里有多少样本不是 0（回声确实在响，逐位相同才有意义）
std::size_t audible(const std::vector<float>& trace)
{
    std::size_t count = 0;
    for (const float v : trace)
    {
        count += v != 0.0f ? 1u : 0u;
    }
    return count;
}

} // namespace

TEST_CASE("起播去抖 环路层：走带中间 3 块报停不复位，回声尾巴与全程在走的对照逐位相同")
{
    // ⛔ 删除式验证点：不去抖 → 多一次起播重同步，恢复处回送 5 ms 淡出、读头重建，本用例变红。
    Run control;
    control.playAll();
    REQUIRE(control.playResyncs() == 1u);
    REQUIRE(control.resets.size() == 1u);
    REQUIRE(audible(control.rig.trace) > 20000u);

    Run paused;
    for (int i = 0; i < kSteps; ++i)
    {
        paused.step(!(i >= kPauseAt && i < kPauseAt + 3));
    }
    CHECK(paused.playResyncs() == 1u);
    CHECK(paused.rig.status().resyncCount == 1u);
    CHECK(paused.resets.size() == 1u);
    CHECK(paused.rig.status().tapsActive == 1u);
    // 回声尾巴连续：整条回送与对照逐位相同（报停的那几块照样出回声，恢复处不淡出、不重建读头）
    CHECK(firstDiff(paused.rig.trace, control.rig.trace) == -1);
}

TEST_CASE("起播去抖 环路层：真停走带（够长的报停）之后再起播，复位照常；阈值前一块不确认，够了就确认")
{
    Run control;
    control.playAll();

    // 19 块 × 256 = 4864 ≥ 4800：确认
    Run stopped;
    for (int i = 0; i < kSteps; ++i)
    {
        stopped.step(!(i >= kPauseAt && i < kPauseAt + 19));
    }
    CHECK(stopped.playResyncs() == 2u);
    REQUIRE(stopped.resets.size() == 2u);
    // 第二次复位点：恢复的那一块的块首读位 + T（T = 5 ms = 240）
    CHECK(stopped.resets[1] == static_cast<std::int64_t>(kPauseAt + 19) * kBlock + 240);
    CHECK(firstDiff(stopped.rig.trace, control.rig.trace) != -1); // 旧回送 5 ms 淡出，读头重建

    // 18 块 × 256 = 4608 < 4800：不确认
    Run shorter;
    for (int i = 0; i < kSteps; ++i)
    {
        shorter.step(!(i >= kPauseAt && i < kPauseAt + 18));
    }
    CHECK(shorter.playResyncs() == 1u);
    CHECK(firstDiff(shorter.rig.trace, control.rig.trace) == -1);
}

TEST_CASE("起播去抖 环路层：大缓冲下 3 块报停也不复位；同样长的报停在小块下则是真停走带")
{
    // ⛔ 删除式验证点：阈值只用 100 ms → 第一段变红。
    constexpr int kBig = 2048;
    Run control(kBig);
    for (int i = 0; i < 40; ++i)
    {
        control.step(true, kBig);
    }
    Run big(kBig);
    for (int i = 0; i < 40; ++i)
    {
        big.step(!(i >= 25 && i < 28), kBig); // 3 × 2048 = 6144 样本，比 100 ms 的 4800 长
    }
    CHECK(big.playResyncs() == 1u);
    CHECK(big.resets.size() == 1u);
    CHECK(firstDiff(big.rig.trace, control.rig.trace) == -1);

    // 同样的 6144 样本，块长 256（最大块长 1024 的 4 倍 = 4096 < 4800，下限说了算）：24 块 ≥ 4800，是真停走带
    Run small;
    for (int i = 0; i < 480; ++i)
    {
        small.step(!(i >= 300 && i < 324));
    }
    CHECK(small.playResyncs() == 2u);
}

TEST_CASE("起播去抖 环路层：0 样本调用不累计报停")
{
    // 0 样本的调用立刻返回、不推进任何计数：它带的 isPlaying=false 不能让报停累计出去
    Run control;
    control.playAll();
    Run run;
    for (int i = 0; i < kSteps; ++i)
    {
        if (i == kPauseAt)
        {
            TransportInfo off;
            off.isPlaying = false;
            for (int k = 0; k < 500; ++k)
            {
                run.rig.feed(0, run.params, off);
            }
        }
        run.step(true);
    }
    CHECK(run.playResyncs() == 1u);
    CHECK(firstDiff(run.rig.trace, control.rig.trace) == -1);
}

TEST_CASE("起播去抖 环路层反例：只送一块 isPlaying=false 就挂起，之后真起播，起播复位不被吞掉")
{
    // ⛔ 删除式验证点：flushAfterSuspend 不通知去抖 → 本用例变红。
    // 对照：同样只有一块报停、没有挂起的证据，是短暂报停，不复位。
    Run noEvidence;
    for (int i = 0; i < kSteps; ++i)
    {
        noEvidence.step(i != kPauseAt);
    }
    CHECK(noEvidence.playResyncs() == 1u);

    // 挂起：壳判定两端一起停了 250 ms 以上，请求清空，下一块（恢复）块首 flushAfterSuspend
    Run run;
    for (int i = 0; i < kSteps; ++i)
    {
        if (i == kPauseAt + 1)
        {
            run.rig.session().flushAfterSuspend();
        }
        run.step(i != kPauseAt);
    }
    CHECK(run.playResyncs() == 2u);
    REQUIRE(run.resets.size() == 2u);
    CHECK(run.resets[1] == static_cast<std::int64_t>(kPauseAt + 1) * kBlock + 240);
}

TEST_CASE("起播去抖 环路层反例：停走带时一块 isPlaying=false 都没送过，挂起之后真起播，起播复位不被吞掉")
{
    // ⛔ 删除式验证点：同上。
    // 宿主停走带时不调用我们：样本数不走，累计不出报停。没有挂起的证据（间隔不到 250 ms）就接着走，没有沿；
    Run quick;
    for (int i = 0; i < kSteps; ++i)
    {
        quick.step(true);
    }
    CHECK(quick.playResyncs() == 1u);

    // 间隔超过 250 ms、壳请求清空：恢复后第一个在走的块是沿
    Run run;
    for (int i = 0; i < kSteps; ++i)
    {
        if (i == kPauseAt)
        {
            run.rig.session().flushAfterSuspend();
        }
        run.step(true);
    }
    CHECK(run.playResyncs() == 2u);
    CHECK(run.resets.size() == 2u);
}

TEST_CASE("起播去抖 环路层：重新 prepare——报停开始后确认；一直在走时同采样率重新 prepare 保持流连续")
{
    // ⛔ 删除式验证点：prepare 不确认 → 第一段变红；一直在走时也确认 → 第二段变红（流被打断）。
    {
        Run run;
        for (int i = 0; i < kSteps; ++i)
        {
            if (i == kPauseAt + 1)
            {
                REQUIRE(run.rig.session().prepare(48000.0)); // 停用再激活
            }
            run.step(i != kPauseAt);
        }
        CHECK(run.playResyncs() == 2u);
    }
    {
        Run control;
        control.playAll();
        Run run;
        for (int i = 0; i < kSteps; ++i)
        {
            if (i == kPauseAt)
            {
                REQUIRE(run.rig.session().prepare(48000.0));
            }
            run.step(true);
        }
        CHECK(run.playResyncs() == 1u);
        CHECK(firstDiff(run.rig.trace, control.rig.trace) == -1);
    }
}

TEST_CASE("起播去抖 环路层：一直没走带的预滚之后第一次起播照常复位")
{
    Run run;
    for (int i = 0; i < 50; ++i)
    {
        run.step(false);
    }
    CHECK(run.playResyncs() == 0u);
    run.step(true);
    CHECK(run.playResyncs() == 1u);
    REQUIRE(run.resets.size() == 1u); // 复位点在沿后 240 个样本，落在这一块里
    CHECK(run.resets[0] == 50 * kBlock + 240);
}
