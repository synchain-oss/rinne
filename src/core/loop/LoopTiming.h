// SPDX-License-Identifier: GPL-3.0-or-later
//
// 流相对寻址与环路延迟的几何：读位 R 与写头 W 只单调前进，与时间线无关。
//
// 符号（Input 本地全部用 int64；W 是 IPC 里的 u64，进本地后转 int64）：
//   n        本次回调的块长
//   N_prep   本端 prepare 声明的最大块长
//   N_obs    本会话实际观察到的最大块长（每次 prepare 清零）
//   W        Output 已写总帧数
//   R        Input 本地已处理帧数（跨 prepare 连续）
//   off      读头的同步偏移
//   D        环路延迟（样本）
//
// 差值一律走有符号运算：W 越过 2^64 时仍然给出正确的差；窗口下界允许为负（负的下界自然不起作用）。
//
// 线程：全部按样本计数，不读墙钟、不分配、不加锁。prepare 由非实时线程调，其余由音频线程每块调。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace rinne::loop
{

using i64 = std::int64_t;
using u64 = std::uint64_t;

// 两个 u64 帧位置的有符号差：直接相减再转，回绕时仍然正确
constexpr i64 frameDiff(u64 a, u64 b) noexcept
{
    return static_cast<i64>(a - b);
}

// 环路延迟的范围：1–4000 ms（计划口径），默认 100 ms。
inline constexpr double kDelayMinMs = 1.0;
inline constexpr double kDelayMaxMs = 4000.0;
inline constexpr double kDelayDefaultMs = 100.0;

// 改 D 的交叉淡化与去抖：契约的一部分，改动算兼容性事件（见 ADR-0002）。
//
// 交叉淡化 40 ms 线性：新旧两个读头同时在跑，增益是 (1−x) 与 x，两路增益之和恒为 1。
// 不用等功率交叉：两路信号相关时等功率会抬高约 3 dB，线性不过冲。
// 去抖 100 ms：重新定目标必须距上一次定目标 ≥ 100 ms（按样本计），窗口内的目标变化被丢掉。
//
// 这两个时长给改 D 的交叉淡化用（改 D 时开第二个读头并挂交叉淡化包络，读头槽已留够 3 个）；
// 这里先把常量与口径钉住，交叉淡化接上时照它做，避免两处各写一套。
inline constexpr double kDelayCrossfadeSeconds = 0.040;
inline constexpr double kDelayDebounceSeconds = 0.100;

// 本地延迟线的容量：nextPow2(ceil(4.0·sr) + 32768 + 64)。
// 4 s 是 D 的上限（D_cap = 4.0 s × sr），32768 是支持的最大块长，64 是余量。
// 44.1/48 kHz 下是 2^18，60/96 kHz 下是 2^19，192 kHz 下是 2^20。
inline std::int64_t lineFramesFor(double sampleRate) noexcept
{
    const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    const auto raw = static_cast<std::int64_t>(std::ceil(4.0 * sr)) + 32768 + 64;
    std::int64_t n = 1;
    while (n < raw)
    {
        n <<= 1;
    }
    return n;
}

// D 的上限：4.0 s × sr
inline std::int64_t delayCapFrames(double sampleRate) noexcept
{
    const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    return static_cast<std::int64_t>(std::ceil(4.0 * sr));
}

// D 的下限：max(N_prep, N_obs)。两个都是「本端声明的」与「本会话见过的」最大块长。
// v1 只做整数延迟，没有插值余量，所以下限就是块长。
inline std::int64_t delayMinFrames(std::int64_t nPrep, std::int64_t nObs) noexcept
{
    const std::int64_t a = nPrep > 0 ? nPrep : 0;
    const std::int64_t b = nObs > 0 ? nObs : 0;
    return std::max(a, b);
}

// 为什么 D 被抬升：界面常驻显示「实际 / 最小 / 原因 / 本会话最大块长」，原因就是这几项。
enum class DelayMinReason : std::uint8_t
{
    kNone = 0, // 没有被抬升
    kPrepare = 1, // 本端 prepare 声明的最大块长
    kObserved = 2, // 本会话观察到的最大块长
    kHostViolation = 3, // 宿主违约（送来的块长超过声明值）
};

// 一次 D 的计算结果。
struct DelayChoice
{
    std::int64_t effective = 0; // 实际用的 D（样本）
    std::int64_t minimum = 0; // 抬升后的下限
    std::int64_t requested = 0; // 用户要的 D（样本）
    DelayMinReason reason = DelayMinReason::kNone;
    bool raised = false; // 是否被抬升过
};

// 毫秒转样本：按 round(ms/1000 × sr)，至少 1 个样本
inline std::int64_t msToFrames(double ms, double sampleRate) noexcept
{
    const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    const auto n = static_cast<std::int64_t>(std::llround(ms * 0.001 * sr));
    return n > 0 ? n : 1;
}

// 样本转毫秒：给界面显示用
inline double framesToMs(std::int64_t frames, double sampleRate) noexcept
{
    const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    return static_cast<double>(frames) * 1000.0 / sr;
}

// 把用户要的 D 落到合法范围：D 用户 < D 下限时抬到下限，D 用户 > D 上限时截到上限。
// 下限随实测块长抬升，所以同一个 D 值在不同会话里可能落到不同的实际值。
inline DelayChoice chooseDelay(std::int64_t requestedFrames, std::int64_t nPrep, std::int64_t nObs,
                               double sampleRate) noexcept
{
    DelayChoice choice;
    choice.requested = requestedFrames;
    const std::int64_t min = delayMinFrames(nPrep, nObs);
    const std::int64_t cap = delayCapFrames(sampleRate);
    choice.minimum = std::min(min, cap);
    std::int64_t eff = requestedFrames;
    if (eff < choice.minimum)
    {
        eff = choice.minimum;
        choice.raised = true;
        choice.reason = nObs > nPrep ? DelayMinReason::kObserved : DelayMinReason::kPrepare;
    }
    if (eff > cap)
    {
        eff = cap;
        choice.raised = true;
        choice.reason = DelayMinReason::kPrepare;
    }
    choice.effective = eff;
    return choice;
}

// 写头与读位之间的漂移量：m = W − (R + off)。正数表示写头领先，负数表示读位跑到了写头前面。
// 全部走有符号运算：W ≈ 2^40 之类的位置上也正确。
constexpr i64 drift(i64 writeHead, i64 processed, i64 offset) noexcept
{
    return writeHead - (processed + offset);
}

// 漂移跳变的门限 N_cap = max(两端声明的最大块长, 8192)。超过它就是跳变（jump），立即重同步。
inline std::int64_t jumpCap(std::int64_t nPrep, std::int64_t peerMaxBlock) noexcept
{
    return std::max({nPrep > 0 ? nPrep : 0, peerMaxBlock > 0 ? peerMaxBlock : 0, static_cast<std::int64_t>(8192)});
}

// 断档判据：W 未动且 R 前进 ≥ 2·N_max → STALLED。
// N_max 取本端与对端声明的最大块长里较大的那个（对端未知时只用本端）。
// 判据按样本计：推进量不够就还不算断档。
inline std::int64_t stallThreshold(std::int64_t nPrep, std::int64_t peerMaxBlock) noexcept
{
    const std::int64_t nMax = std::max(nPrep > 0 ? nPrep : 0, peerMaxBlock > 0 ? peerMaxBlock : 0);
    return 2 * nMax;
}

// 严格模式的对齐判据：Δ 两次不为 0 就重同步。
// Δ 是本块读到的写头与上一块的写头之差（写头每块前进的帧数）。两次都是同一个非零值才算稳定。
// 返回 true 表示「Δ 变了」，需要重同步。
inline bool alignmentChanged(i64 delta, i64 previousDelta, bool havePrevious) noexcept
{
    if (!havePrevious)
    {
        return false;
    }
    return delta != previousDelta;
}

// 容忍模式：最近 window 帧内重同步 ≥ threshold 次就转容忍模式。
// 时刻按样本计数（不用墙钟）：resyncTimes 是最近几次重同步时的 R，环形记 threshold 个。
// 窗口以样本数给出（10 s × 采样率）。
//
// cursor 是「下一个要写的位置」：写满一圈之后，最老的那一次就在 cursor 指向的槽里
// （下一次写会把它覆盖掉）。所以最老的那次是 resyncTimes[cursor % threshold]。
inline bool shouldEnterTolerant(const i64* resyncTimes, int count, int cursor, int threshold, i64 windowSamples,
                                i64 now) noexcept
{
    if (count < threshold)
    {
        return false;
    }
    // 最老的那次在 cursor 指向的槽（下一个要写的位置所指向的那一格）
    const int oldest = cursor % threshold;
    const i64 oldestTime = resyncTimes[oldest];
    const i64 span = now - oldestTime;
    // 时间倒流（不该发生）时按「刚好在窗口内」处理：宁可早转容忍模式，不要漏掉
    return span >= 0 ? span <= windowSamples : true;
}

} // namespace rinne::loop
