// SPDX-License-Identifier: GPL-3.0-or-later
//
// 起播沿的去抖：宿主走带中间短暂报停不算停走带，也就不会在恢复时触发起播复位（口径见 ADR-0010）。
//
// 背景：Cubase 在实时与预取之间切换时，会在走带中间连续几块把 isPlaying 报成 false 再恢复（实测 3 块，每块 256 帧，
// 共 768 个样本）。起播沿若按「上一块没走、这一块走了」直接判，这几块就会触发起播复位：回送 5 ms 淡出、本地延迟线复位，
// 回声尾巴被切掉。
//
// 做法：停走带要「持续够久」才算。连续报停的样本数累计到阈值才确认停走带，确认之后第一个在走的块才是起播沿；
// 没到阈值就恢复，整段当作没有发生。阈值 = max(100 ms, 4 × 最大块长)，都按样本计，引擎不读墙钟：
//   - 100 ms 是下限：真停走带至少持续到用户再按一次播放，实测最短的一次也有 5 s；而实测的短暂报停只有 16 ms。
//   - 4 × 最大块长随块长放宽：块很大时（例如 2048 帧，约 43 ms）3 块就超过 100 ms，固定 100 ms 会把它当成停走带。
//     最大块长取「声明值与本会话见过的最大块长」里较大的，与 D 的下限同一个口径。
//   阈值随最大块长走，所以停顿落在 100 ms 与 4 个最大块之间时，结论可能随宿主给的块长而变；除此之外与切块无关。
//
// 宿主停走带时可能一块 isPlaying=false 都没送过，或只送了一块就不再调用我们（挂起）。样本数在这段时间里不走，
// 单靠累计分不出「短暂报停」和「停走带」，所以还要两个已有的信号：
//   - suspended()：两端一起挂起超过 250 ms（壳请求清空本地延迟线，LoopSession::flushAfterSuspend）。挂起期间走带状态
//     不可知，恢复后第一个在走的块算起播沿；
//   - prepared()：重新 prepare（停用再激活）。报停已经开始但还没到阈值时，视为确认；一直在走时不动，
//     同采样率重新 prepare 因此仍保持流连续。
//
// 实时约定：纯整数运算，不分配、不加锁、不读墙钟、不抛异常。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace rinne::loop
{

namespace timing
{
inline constexpr double kPlayDebounceSeconds = 0.100; // 报停短于它（且短于 4 个最大块）不算停走带
inline constexpr int kPlayDebounceBlocks = 4;
} // namespace timing

class PlayDebounce
{
public:
    // 非实时线程：按采样率换算 100 ms 的下限。不动走带状态（同采样率重新 prepare 保持流连续）。
    void prepare(double sampleRate) noexcept
    {
        const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
        floor_ = std::max<std::int64_t>(1, static_cast<std::int64_t>(std::llround(timing::kPlayDebounceSeconds * sr)));
    }

    // 停走带的阈值（样本）。largestBlock 是声明值与本会话见过的最大块长里较大的。
    std::int64_t threshold(std::int64_t largestBlock) const noexcept
    {
        return std::max<std::int64_t>(floor_, timing::kPlayDebounceBlocks * std::max<std::int64_t>(largestBlock, 0));
    }

    // 每个 n > 0 的支持块调一次，返回这一块是不是起播沿。
    bool observe(bool playing, std::int64_t n, std::int64_t largestBlock) noexcept
    {
        if (playing)
        {
            const bool edge = !running_;
            running_ = true;
            stopped_ = 0;
            return edge;
        }
        if (running_)
        {
            stopped_ += n;
            if (stopped_ >= threshold(largestBlock))
            {
                running_ = false;
                stopped_ = 0;
            }
        }
        return false;
    }

    // 两端一起挂起超过 250 ms：走带状态不可知，恢复后第一个在走的块算起播沿。
    void suspended() noexcept
    {
        running_ = false;
        stopped_ = 0;
    }

    // 重新 prepare：报停已经开始、还没到阈值时视为确认；一直在走时不动。
    void prepared() noexcept
    {
        if (stopped_ > 0)
        {
            running_ = false;
            stopped_ = 0;
        }
    }

    // 诊断与测试：已确认在走带；已经连续报停的样本数（还没到阈值）。
    bool running() const noexcept { return running_; }
    std::int64_t stoppedSamples() const noexcept { return stopped_; }

private:
    std::int64_t floor_ = 4800; // 100 ms @ 48 kHz，prepare 里按采样率重算
    bool running_ = false;
    std::int64_t stopped_ = 0;
};

} // namespace rinne::loop
