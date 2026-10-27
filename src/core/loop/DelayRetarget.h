// SPDX-License-Identifier: GPL-3.0-or-later
//
// 改 D 的 40 ms 线性交叉淡化与 100 ms 前沿限速（纯逻辑，只有头文件，不分配、不读墙钟）。
//
// 交叉淡化：新旧两个读头读同一信号的不同时刻，同时在跑 40 ms。第 k 个样本（从 0 起数）上，新读头的增益是
// in = k / L，旧读头的增益是 out = 1 − in，两路之和恒为 1：相关信号上不过冲。等功率（sin / cos）交叉在相关信号上
// 会抬高约 3 dB，所以不用。out 必须由同一个 in 做浮点减法得到，不能另算 (L − k) / L：in 在 [0.5, 1] 时 1 − in
// 是精确减法，在 [0, 0.5) 时 1 − in 的舍入误差不超过 2^-25，两者相加再舍入回 1.0f，所以逐样本之和恰为 1.0f，
// 单测可以逐位断言。core 带 -ffp-contract=off，编译器不会把这一步合并成别的算法。
//
// 前沿限速（100 ms）：第一次改动立即开始交叉淡化；此后每 100 ms 至多开始一次。窗口内的改动合并：不记中间值，
// 每一块都拿当时最新的目标去比，窗口到点那一刻目标与当前不同，就向最新目标再淡一次。窗口比交叉淡化长（100 > 40），
// 任何时刻至多一段交叉淡化在进行。计时一律按样本：窗口到点的那个样本就是下一段交叉淡化的第一个样本，
// 可以落在块中间，所以结果与宿主怎么切块无关。
//
// 线程：prepare 由非实时线程调；其余在音频线程上，只做整数与浮点运算。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "loop/LoopTiming.h"

namespace rinne::loop
{

// 一个样本上的一对增益。
struct XfadeGains
{
    float in = 0.0f; // 新读头
    float out = 1.0f; // 旧读头
};

// 第 step 个样本上的交叉淡化增益，总长 length 个样本。step 从 0 起数：
//   step ≤ 0        还没开始（负数）或正好是第一个样本：新读头 0、旧读头 1；
//   0 < step < L    in = step / L，out = 1 − in；
//   step ≥ L        走完：新读头 1、旧读头 0（旧读头从这一个样本起退场）。
// length ≤ 0 当作「已经走完」。
inline XfadeGains linearXfadeGains(std::int64_t step, std::int64_t length) noexcept
{
    XfadeGains g;
    if (length <= 0 || step >= length)
    {
        g.in = 1.0f;
        g.out = 0.0f;
        return g;
    }
    if (step <= 0)
    {
        return g;
    }
    g.in = static_cast<float>(step) / static_cast<float>(length);
    g.out = 1.0f - g.in;
    return g;
}

// 时长（秒）→ 样本数，至少一个样本
inline std::int64_t secondsToSamples(double seconds, double sampleRate) noexcept
{
    const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    const auto n = static_cast<std::int64_t>(std::llround(seconds * sr));
    return n > 0 ? n : 1;
}

// 交叉淡化的总长：40 ms
inline std::int64_t xfadeSamplesFor(double sampleRate) noexcept
{
    return secondsToSamples(kDelayCrossfadeSeconds, sampleRate);
}

// 前沿限速：两次交叉淡化的起点至少相隔 100 ms。
class RetargetLimiter
{
public:
    // 按采样率换算窗口长度，并回到「没有最近一次起点」（第一次改动立即开始）。
    void prepare(double sampleRate) noexcept
    {
        window_ = secondsToSamples(kDelayDebounceSeconds, sampleRate);
        reset();
    }

    // 回到「没有最近一次起点」。
    void reset() noexcept
    {
        since_ = window_;
        startedAt_ = -1;
    }

    std::int64_t windowSamples() const noexcept { return window_; }

    // 本块（n 个样本）里最早可以开始交叉淡化的样本偏移；整块都在窗口里时返回 -1（改动先挂着）。
    // 窗口早就过了就是 0：改动在块首立即开始。
    std::int64_t startOffset(std::int64_t n) const noexcept
    {
        const std::int64_t wait = window_ - since_;
        const std::int64_t offset = wait > 0 ? wait : 0;
        return offset < n ? offset : -1;
    }

    // 本块在 offset 这个样本上开始了一段交叉淡化。块末的 endBlock 据此重算「距上一个起点多少样本」。
    void noteStart(std::int64_t offset) noexcept { startedAt_ = offset; }

    // 块末，每块恰好调一次：n 是本块的样本数。
    void endBlock(std::int64_t n) noexcept
    {
        if (startedAt_ >= 0)
        {
            since_ = std::min(n - startedAt_, window_);
            startedAt_ = -1;
            return;
        }
        since_ = std::min(since_ + n, window_);
    }

    // 距上一个起点已过的样本数（封顶在窗口长度；测试与诊断用）
    std::int64_t sinceStart() const noexcept { return since_; }

private:
    std::int64_t window_ = 1;
    std::int64_t since_ = 1; // 距上一个起点已过的样本数，封顶在 window_（封顶 = 窗口早就过了）
    std::int64_t startedAt_ = -1; // 本块里起点的偏移，-1 = 本块没有起点
};

} // namespace rinne::loop
