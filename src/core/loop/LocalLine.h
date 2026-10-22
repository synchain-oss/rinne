// SPDX-License-Identifier: GPL-3.0-or-later
//
// 本地延迟线：Input 把从环里拉到的帧搬进来，读头再从这里按绝对位置读出去。
//
// 几何：立体声，长度是 2 的幂（lineFramesFor），用 p & mask 取下标，所以任何绝对位置都能直接寻址。
// 拉进来的帧按绝对位置 p 存放；读头读的位置 s = R + off + i − D。
// 有效区间 [lo, hi)：lo = max(读头 validFrom, lineValidFrom, pulled − LL_frames)，hi = min(pulled, 读头 validTo)。
// 落在区间外的位置一律读成 0：绝不读旧数据或别名数据。
//
// 尾巴快照：重同步时旧读头要用「预拷贝的 5 ms 尾巴」淡出。预拷贝是为了防这一段在淡出期间被环覆盖：
// 快照把淡出要用的那一段先拷到线内的定长尾巴区，此后读旧位置一律从快照读，不再依赖线的主区。
//
// 清空：两端停用超过 250 ms 时清空（防停用止啸后原样复燃）、跳闸淡出后清空、起播复位时清空。
// 清空只改两个整数（lineValidFrom 与 pulled 的基准），不动缓冲区里的样本：清空之后这些位置落在有效区间外，
// 读出来是 0。清空不需要把几兆字节清零，因此可以在音频线程上做。
//
// 线程：全部按样本计数，不分配、不加锁、不读时钟。prepare（采样率变化时重分配）由非实时线程调；
// 拉取与读取在音频线程上。
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "loop/LoopTiming.h"

namespace rinne::loop
{

// 一个采样点的左右两声道。
struct StereoSample
{
    float left = 0.0f;
    float right = 0.0f;
};

// 本地延迟线。
class LocalLine
{
public:
    // 尾巴区能放多少帧。取 20 ms：重同步的淡出是 5 ms，onset 是 20 ms，取宽一点够用。
    static constexpr std::int64_t kTailFrames = 1024;

    // 采样率变化时才重新分配（allocate 返回 false 表示没准备好，调用方按「没有对端」处理）。
    // 非实时线程调用。
    bool allocate(double sampleRate) noexcept
    {
        const std::int64_t frames = lineFramesFor(sampleRate);
        if (frames <= 0)
        {
            return false;
        }
        if (frames == frames_ && sampleRate == sampleRate_)
        {
            return true; // 采样率没变：什么都不动，流保持连续
        }
        sampleRate_ = sampleRate;
        frames_ = frames;
        mask_ = frames - 1;
        left_.assign(static_cast<std::size_t>(frames), 0.0f);
        right_.assign(static_cast<std::size_t>(frames), 0.0f);
        tailLeft_.assign(static_cast<std::size_t>(kTailFrames), 0.0f);
        tailRight_.assign(static_cast<std::size_t>(kTailFrames), 0.0f);
        reset();
        return true;
    }

    bool ready() const noexcept { return frames_ > 0; }
    std::int64_t frames() const noexcept { return frames_; }
    double sampleRate() const noexcept { return sampleRate_; }

    // 清空：有效区间从下一个位置起算，已有的样本读不出来。
    // pulled_ 归零、lineValidFrom_ 归零：之后拉进来的帧从 0 开始算起。尾巴快照也作废。
    void reset() noexcept
    {
        pulled_ = 0;
        lineValidFrom_ = 0;
        tailStart_ = 0;
        tailCount_ = 0;
    }

    // 已经拉进来的帧数（绝对位置）
    i64 pulled() const noexcept { return pulled_; }
    // 有效区间的下界：这个位置之前的帧一律读成 0
    i64 lineValidFrom() const noexcept { return lineValidFrom_; }

    // 把 [from, to) 的帧搬进来。from 不小于 pulled_，to 不超过 pulled_ + 一圈（环只有一圈）。
    // sink 的签名是 sink(绝对位置 p, 左, 右)：调用方把环里的帧按位置交出来。
    // 拉进来的帧已经过清洗（非有限 → 0，钳 ±16）：调用方负责，line 只搬。
    template<typename Sink>
    void pull(i64 from, i64 to, Sink&& sink) noexcept
    {
        if (to <= from || frames_ == 0)
        {
            return;
        }
        const i64 lap = frames_;
        const i64 lo = std::max(from, to - lap);
        for (i64 p = lo; p < to; ++p)
        {
            float l = 0.0f;
            float r = 0.0f;
            sink(p, l, r);
            const auto at = static_cast<std::size_t>(p & mask_);
            left_[at] = l;
            right_[at] = r;
        }
        pulled_ = to;
    }

    // 有效区间的下界被抬高：环里比 intent − 一圈更早的帧可能被覆盖了，从这个位置起算
    void raiseValidFrom(i64 from) noexcept
    {
        lineValidFrom_ = std::max(lineValidFrom_, from);
    }

    // 读一个绝对位置。返回 false 表示这个位置不在有效区间里（调用方按 0 处理）。
    bool read(i64 position, float& left, float& right) const noexcept
    {
        if (frames_ == 0 || position < 0 || position >= pulled_ || position < lineValidFrom_)
        {
            return false;
        }
        const auto at = static_cast<std::size_t>(position & mask_);
        left = left_[at];
        right = right_[at];
        return true;
    }

    // 尾巴快照：把主区最后 count 帧拷到尾巴区。返回 false 表示没有可拷的帧。
    // 快照之后，那一段的位置一律经 readTail 读，不再依赖主区（主区可能已被环覆盖）。
    bool snapshotTail(i64 count) noexcept
    {
        if (frames_ == 0 || pulled_ <= 0)
        {
            return false;
        }
        const i64 n = std::min({count, kTailFrames, pulled_});
        if (n <= 0)
        {
            return false;
        }
        const i64 start = pulled_ - n;
        for (i64 i = 0; i < n; ++i)
        {
            const auto src = static_cast<std::size_t>((start + i) & mask_);
            const auto dst = static_cast<std::size_t>(i);
            tailLeft_[dst] = left_[src];
            tailRight_[dst] = right_[src];
        }
        tailStart_ = start;
        tailCount_ = n;
        return true;
    }

    // 从尾巴快照里读。位置不在快照里返回 false。
    bool readTail(i64 position, float& left, float& right) const noexcept
    {
        if (tailCount_ <= 0 || position < tailStart_ || position >= tailStart_ + tailCount_)
        {
            return false;
        }
        const auto at = static_cast<std::size_t>(position - tailStart_);
        left = tailLeft_[at];
        right = tailRight_[at];
        return true;
    }

    // 尾巴快照（指定区间）：把 [from, from + count) 拷进尾巴区。
    // 与 snapshotTail 的区别：snapshotTail 拷的是主区的最后 count 帧（[pulled - count, pulled)），
    // 这一份拷的是调用方指定的区间。重同步时旧读头要淡出的正是它自己即将读的那一段，
    // 那段不一定在主区的末尾（读头落后写头 D 个样本），所以要按区间拷。
    //
    // 区间会被夹到主区的可用范围里：[from, from + count) 与 [lineValidFrom, pulled) 的交集。
    // 夹不到任何帧时返回 false（没有可拷的帧）。
    bool snapshotRange(i64 from, i64 count) noexcept
    {
        if (frames_ == 0 || count <= 0)
        {
            return false;
        }
        const i64 n = std::min(count, kTailFrames);
        const i64 lo = std::max(from, lineValidFrom_);
        const i64 hi = std::min(from + n, pulled_);
        if (hi <= lo)
        {
            return false;
        }
        const std::int64_t m = hi - lo;
        for (i64 i = 0; i < m; ++i)
        {
            const auto src = static_cast<std::size_t>((lo + i) & mask_);
            const auto dst = static_cast<std::size_t>(i);
            tailLeft_[dst] = left_[src];
            tailRight_[dst] = right_[src];
        }
        tailStart_ = lo;
        tailCount_ = m;
        return true;
    }

    // 尾巴快照覆盖的区间 [tailStart, tailStart + tailCount)
    i64 tailStart() const noexcept { return tailStart_; }
    i64 tailCount() const noexcept { return tailCount_; }

private:
    std::vector<float> left_;
    std::vector<float> right_;
    std::vector<float> tailLeft_;
    std::vector<float> tailRight_;
    std::int64_t frames_ = 0;
    std::int64_t mask_ = 0;
    double sampleRate_ = 0.0;
    i64 pulled_ = 0;
    i64 lineValidFrom_ = 0;
    i64 tailStart_ = 0;
    i64 tailCount_ = 0;
};

} // namespace rinne::loop
