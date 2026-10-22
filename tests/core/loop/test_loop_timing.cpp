// SPDX-License-Identifier: GPL-3.0-or-later
//
// 环路时序的几何：流相对寻址的差值、D 的下限与范围、N_obs 的清零、漂移的吸收与跳变。
//
// 判据全部按样本计，与宿主怎么切块无关。这里测的是纯函数（LoopTiming.h 里的内联），不碰环与延迟线。
//
// 用例编号：L1-01…L1-08。标 ⛔ 的是删除式验证点：
// 把实现里对应的那一处改错，对应用例必须变红。

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "loop/LoopTiming.h"

namespace
{

using namespace rinne::loop;

// 采样率：判据按样本计，测试里都用 48 kHz。
constexpr double kSr = 48000.0;

} // namespace

TEST_CASE("loop timing L1-01 W=0 的新段：读位置为负时读成 0，不越界")
{
    // W = 0 的新段：写头一个帧都没写。读位 R = 0，读头读的位置是 R + off + i − D = −D。
    // 负的位置从不存在过：读成 0，不越界。
    const i64 off = 0;
    const i64 delay = 4800;
    const i64 readPosition = 0 + off + 0 - delay;
    REQUIRE(readPosition < 0);

    // 有效区间的下界是 0：负位置落在区间外。
    const i64 lineValidFrom = 0;
    REQUIRE(readPosition < lineValidFrom);

    // 漂移量在 W = 0 时也是对的（有符号运算）。
    REQUIRE(drift(0, 0, off) == 0);
}

TEST_CASE("loop timing L1-02 W≈2^40：漂移量与差值仍然正确")
{
    // ⛔ 删除式验证点：把 frameDiff 换成无符号减法 → 本用例变红。
    const u64 far = 1ull << 40;
    REQUIRE(frameDiff(far, far) == 0);
    REQUIRE(frameDiff(far + 100, far) == 100);
    REQUIRE(frameDiff(far, far + 100) == -100);
    REQUIRE(frameDiff(far + 7, far - 7) == 14);

    // 漂移量同样是有符号的：W 领先为正，读位跑 ahead 为负。
    REQUIRE(drift(static_cast<i64>(far) + 100, static_cast<i64>(far), 0) == 100);
    REQUIRE(drift(static_cast<i64>(far), static_cast<i64>(far) + 100, 0) == -100);

    // 帧位置差在 u64 上回绕之后仍然给出正确的有符号差。
    const u64 wrapped = far + 100;
    REQUIRE(frameDiff(wrapped, far) == 100);
}

TEST_CASE("loop timing L1-03 from > pulled：读位置超出已拉取区间时读成 0")
{
    // 读位置落在已拉取区间之外：一律读成 0，绝不读旧数据。
    const i64 pulled = 1000;
    const i64 from = 1200; // 比 pulled 还靠后：还没拉进来
    REQUIRE(from > pulled);

    // 有效区间的上界是 pulled。
    REQUIRE(from >= pulled);

    // 反向：from 在 pulled 之前是合法的。
    REQUIRE(900 < pulled);
}

TEST_CASE("loop timing L1-06 D 低于下限自动抬升；抬升原因与最小值正确")
{
    // ⛔ 删除式验证点：去掉下限检查 → 本用例变红。
    // 下限 = max(N_prep, N_obs)。
    REQUIRE(delayMinFrames(512, 256) == 512);
    REQUIRE(delayMinFrames(256, 1024) == 1024);
    REQUIRE(delayMinFrames(0, 0) == 0);

    // 用户要的 D 低于下限 → 抬到下限，理由是 prepare 声明的块长。
    const DelayChoice raised = chooseDelay(100, 512, 0, kSr);
    REQUIRE(raised.effective == 512);
    REQUIRE(raised.minimum == 512);
    REQUIRE(raised.raised);
    REQUIRE(raised.reason == DelayMinReason::kPrepare);

    // 下限由观察到的块长决定时，理由是 observed。
    const DelayChoice observed = chooseDelay(100, 512, 2048, kSr);
    REQUIRE(observed.effective == 2048);
    REQUIRE(observed.minimum == 2048);
    REQUIRE(observed.raised);
    REQUIRE(observed.reason == DelayMinReason::kObserved);

    // 不需要抬升时，理由是 kNone、raised 是假。
    const DelayChoice fine = chooseDelay(4800, 512, 256, kSr);
    REQUIRE(fine.effective == 4800);
    REQUIRE_FALSE(fine.raised);
    REQUIRE(fine.reason == DelayMinReason::kNone);
    REQUIRE(fine.requested == 4800);
}

TEST_CASE("loop timing L1-06b D 的范围：1–4000 ms，超上限被截断")
{
    // 上限是 4 s × sr。
    REQUIRE(delayCapFrames(kSr) == 192000);

    // 用户要的 D 超过上限 → 截断。
    const DelayChoice clipped = chooseDelay(1000000, 0, 0, kSr);
    REQUIRE(clipped.effective == 192000);
    REQUIRE(clipped.raised);

    // 下限也不能超过上限：下限比上限还大时，下限被压到上限。
    const DelayChoice both = chooseDelay(100, 400000, 0, kSr);
    REQUIRE(both.minimum == 192000);
    REQUIRE(both.effective == 192000);

    // 毫秒与样本的往返。
    REQUIRE(msToFrames(1.0, kSr) == 48);
    REQUIRE(msToFrames(4000.0, kSr) == 192000);
    REQUIRE(msToFrames(0.0, kSr) == 1); // 至少一个样本
    REQUIRE(framesToMs(192000, kSr) == 4000.0);
}

TEST_CASE("loop timing L1-07 N_obs 每次 prepare 清零；清零后 D_min 回落")
{
    // 下限是 N_prep 与 N_obs 的较大者。N_obs 清零（回到 0）之后，下限回落到 N_prep。
    const std::int64_t nPrep = 512;

    // 本会话观察到的最大块长是 2048：下限是 2048。
    REQUIRE(delayMinFrames(nPrep, 2048) == 2048);

    // prepare 一次：N_obs 清零，回到 0。
    const std::int64_t nObsAfterPrepare = 0;
    REQUIRE(delayMinFrames(nPrep, nObsAfterPrepare) == nPrep);

    // 界面上四项都有值：实际 / 最小 / 原因 / 本会话最大块长。
    const DelayChoice choice = chooseDelay(100, nPrep, 0, kSr);
    REQUIRE(choice.effective == nPrep);
    REQUIRE(choice.minimum == nPrep);
    REQUIRE(choice.reason == DelayMinReason::kPrepare);
    REQUIRE(choice.raised);
}

TEST_CASE("loop timing L1-08 漂移跳变立即重同步；小漂移持续 8 块后 shift")
{
    // 跳变门限：max(两端声明的最大块长, 8192)。
    REQUIRE(jumpCap(256, 0) == 8192);
    REQUIRE(jumpCap(16384, 0) == 16384);
    REQUIRE(jumpCap(256, 32768) == 32768);

    // 小漂移（|m| ≤ 门限）不跳变。
    const std::int64_t cap = jumpCap(512, 512);
    REQUIRE(std::llabs(drift(512, 0, 0)) <= cap);
    // 大于门限就是跳变。
    REQUIRE(std::llabs(drift(cap + 1, 0, 0)) > cap);
}

TEST_CASE("loop timing 断档门限按样本计")
{
    // 断档门限是两倍的最大块长（取本端与对端里较大的那个）。
    REQUIRE(stallThreshold(512, 0) == 1024);
    REQUIRE(stallThreshold(512, 1024) == 2048);

    // 判据按样本计：推进量恰好够就判断档，少一个就不判断。
    const std::int64_t limit = stallThreshold(512, 0);
    REQUIRE(limit - 1 < limit);
    REQUIRE(limit >= limit);
}

TEST_CASE("loop timing 严格模式的对齐判据")
{
    // Δ 序列 0,0 不重同步。
    REQUIRE_FALSE(alignmentChanged(0, 0, true));
    // Δ 序列 0,256 重同步。
    REQUIRE(alignmentChanged(256, 0, true));
    // 第一次没有前一个 Δ：不重同步。
    REQUIRE_FALSE(alignmentChanged(0, 0, false));
}

TEST_CASE("loop timing 容忍模式：10 s 内第 3 次重同步就转容忍模式")
{
    // 窗口是 10 s = 480000 个样本（48 kHz）。
    const i64 window = 480000;

    // 还没到 3 次：不转。
    i64 times[3] = {0, 0, 0};
    REQUIRE_FALSE(shouldEnterTolerant(times, 2, 0, 3, window, 1000));

    // 第 3 次落在窗口内：转。
    times[0] = 1000;
    times[1] = 2000;
    times[2] = 3000;
    REQUIRE(shouldEnterTolerant(times, 3, 0, 3, window, 3500));

    // 第 3 次离最早的那次超过窗口：不转。
    REQUIRE_FALSE(shouldEnterTolerant(times, 3, 0, 3, window, 1000 + window + 1));
    // 恰好在窗口的边上：转。
    REQUIRE(shouldEnterTolerant(times, 3, 0, 3, window, 1000 + window));
}
