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
// 尾巴区有两格：改 D 的交叉淡化进行中遇到重同步时，新旧两个读头读的位置相差一个 D，一段区间装不下，
// 各占一格。快照按绝对位置存放，过时的快照只会读出它当时那个位置上的数据，不会读错位置。
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
    // 尾巴区的格数（见文件头）
    static constexpr int kTailSlots = 2;

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
        for (Tail& tail : tails_)
        {
            tail.left.assign(static_cast<std::size_t>(kTailFrames), 0.0f);
            tail.right.assign(static_cast<std::size_t>(kTailFrames), 0.0f);
        }
        reset();
        return true;
    }

    bool ready() const noexcept { return frames_ > 0; }
    std::int64_t frames() const noexcept { return frames_; }
    double sampleRate() const noexcept { return sampleRate_; }

    // 清空：有效区间从下一个位置起算，已有的样本读不出来。
    // pulled_ 归零、lineValidFrom_ 归零：之后拉进来的帧从 0 开始算起。尾巴快照也作废。
    void reset() noexcept { resetTo(0); }

    // 清空并把起点锚在 position（通常是当前的写头 W）：pulled_ 与 lineValidFrom_ 都落在 position，
    // 比它早的位置一律读成 0，之后从 position 起往后拉。尾巴快照也作废。
    //
    // 为什么要锚在 W 而不是 0：写头是段寿命内的绝对帧位置，配对或清空的那一刻 W 可能早已很大。
    // 从 0 起拉的话，拉取的位置永远追不上写头，读头读的位置 R + off − D 永远落在已拉取区间之外，回送从此是 0。
    void resetTo(i64 position) noexcept
    {
        pulled_ = position;
        lineValidFrom_ = position;
        for (Tail& tail : tails_)
        {
            tail.start = 0;
            tail.count = 0;
        }
    }

    // 换段：主区从 position 起重来（比它早的位置一律读不出来），但尾巴快照保留 —— 旧读头的淡出正在读它。
    // 只许 position 不小于已拉到的位置时用：旧读头读的位置都在 position 之前，从此只可能从尾巴里读到，
    // 新段的帧（落在 position 及以后）进不了旧读头。
    void rebaseKeepingTail(i64 position) noexcept
    {
        pulled_ = position;
        lineValidFrom_ = position;
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
    void raiseValidFrom(i64 from) noexcept { lineValidFrom_ = std::max(lineValidFrom_, from); }

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
        Tail& tail = tails_[0];
        for (i64 i = 0; i < n; ++i)
        {
            const auto src = static_cast<std::size_t>((start + i) & mask_);
            const auto dst = static_cast<std::size_t>(i);
            tail.left[dst] = left_[src];
            tail.right[dst] = right_[src];
        }
        tail.start = start;
        tail.count = n;
        return true;
    }

    // 从尾巴快照里读（两格都查，先第 0 格）。位置不在任何一格里返回 false。
    bool readTail(i64 position, float& left, float& right) const noexcept
    {
        for (const Tail& tail : tails_)
        {
            if (tail.count <= 0 || position < tail.start || position >= tail.start + tail.count)
            {
                continue;
            }
            const auto at = static_cast<std::size_t>(position - tail.start);
            left = tail.left[at];
            right = tail.right[at];
            return true;
        }
        return false;
    }

    // 尾巴快照（指定区间）：把 [from, from + count) 拷进尾巴区。
    // 与 snapshotTail 的区别：snapshotTail 拷的是主区的最后 count 帧（[pulled - count, pulled)），
    // 这一份拷的是调用方指定的区间。重同步时旧读头要淡出的正是它自己即将读的那一段，
    // 那段不一定在主区的末尾（读头落后写头 D 个样本），所以要按区间拷。
    //
    // 区间会被夹到主区的可用范围里：[from, from + count) 与 [lineValidFrom, pulled) 的交集。
    // 夹不到任何帧时返回 false（没有可拷的帧，这一格原样不动）。slot 是尾巴区的格号（0..kTailSlots-1，越界当 0）。
    bool snapshotRange(i64 from, i64 count, int slot = 0) noexcept
    {
        if (frames_ == 0 || count <= 0)
        {
            return false;
        }
        Tail& tail = tails_[slot >= 0 && slot < kTailSlots ? slot : 0];
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
            tail.left[dst] = left_[src];
            tail.right[dst] = right_[src];
        }
        tail.start = lo;
        tail.count = m;
        return true;
    }

    // 尾巴快照覆盖的区间 [tailStart, tailStart + tailCount)（指定一格，缺省第 0 格）
    i64 tailStart(int slot = 0) const noexcept { return tails_[slot >= 0 && slot < kTailSlots ? slot : 0].start; }
    i64 tailCount(int slot = 0) const noexcept { return tails_[slot >= 0 && slot < kTailSlots ? slot : 0].count; }

private:
    // 尾巴区的一格
    struct Tail
    {
        std::vector<float> left;
        std::vector<float> right;
        i64 start = 0;
        i64 count = 0;
    };

    std::vector<float> left_;
    std::vector<float> right_;
    Tail tails_[kTailSlots];
    std::int64_t frames_ = 0;
    std::int64_t mask_ = 0;
    double sampleRate_ = 0.0;
    i64 pulled_ = 0;
    i64 lineValidFrom_ = 0;
};

} // namespace rinne::loop
