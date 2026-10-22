// SPDX-License-Identifier: GPL-3.0-or-later
//
// 读头：每个读头在自己的绝对位置上读本地延迟线，并带一个自己的包络。
// 最多 3 个读头：改 D 的交叉淡化要两个（旧的与新的），重同步时新旧要同时存在，3 个才够。
//
// 一个读头的字段：
//   off     同步偏移。读的位置 = R + off + i − D。
//   delay   这个读头用的 D（样本）。
//   validFrom / validTo  有效区间：区间外读成 0。validFrom 是「从哪个绝对位置起才有数据」，
//           validTo 是「到哪个绝对位置为止还有数据」（+∞ 表示没有上界）。
//   env     包络：0..1 的线性淡变，用来淡入（新读头）与淡出（旧读头）。
//
// 包络按样本推进，与块长无关：同一段输入怎么切块，输出逐位相同。
//   淡出按时间走：从挂上淡出的那个样本起，每个输出样本走一步。
//   淡入（onset）按位置走：从读头读到的第一个有效位置（validFrom）起，每读一个位置走一步。
//   新读头建好之后要过 D 个样本才读到第一帧新数据；onset 若从建好的那一刻起算，D 大于 20 ms 时
//   还没读到数据 onset 就走完了，新数据会以满增益突然出现。按位置起算，新数据总是从 0 起淡入。
//
// 一个读头可以同时在淡入与淡出（重同步时旧读头淡出、新读头淡入，但它们是不同读头；
// 同一个读头在「预拷贝尾巴」的情形下要先把旧数据淡出再把新数据接上，用 tail 段表达）。
//
// 线程：不分配、不加锁、不读时钟、不抛异常。
#pragma once

#include <cstdint>

#include "loop/LocalLine.h"
#include "loop/LoopTiming.h"

namespace rinne::loop
{

// 读头槽数：改 D 的交叉淡化 + 重同步同时发生时，旧 A、旧 B 与新读头要同时在场
inline constexpr int kMaxTaps = 3;

// 淡变的时长（样本，按采样率换算后注入）：
//   旧读头淡出 5 ms（重同步、换组）；新数据 onset 20 ms（重同步之后的新数据）
//   起播淡出 5 ms；断档淡出 5 ms
// 这几个时长在 StreamSync 里按采样率算好，注入这里。
struct TapEnvelope
{
    // 淡出：步数为 0 表示不在淡出。走完之后本读头释放。
    std::int64_t fadeOutSteps = 0;
    std::int64_t fadeOutStep = 0;

    // 淡入（onset）：从 validFrom 起按位置走 fadeInSteps 步；fadeInStep 是块末已走到第几步（诊断与选读头用）。
    // 走完之后 fadeInSteps 归零（包络恒为 1）。
    std::int64_t fadeInSteps = 0;
    std::int64_t fadeInStep = 0;

    bool fadingOut() const noexcept { return fadeOutSteps > 0; }
    bool fadingIn() const noexcept { return fadeInSteps > 0; }
    bool finished() const noexcept { return !fadingOut() && !fadingIn(); }
};

// 一个读头。
struct Tap
{
    bool active = false;
    i64 offset = 0; // 同步偏移
    i64 delay = 0; // 这个读头用的 D
    i64 validFrom = 0;
    i64 validTo = 0; // +∞ = 没有上界
    TapEnvelope env;
};

// 读头集合：最多 kMaxTaps 个。
class TapSet
{
public:
    // 找一个空槽并占住它：槽里的读头被清成初始值，active 置真（调用方接着填 offset / delay / 有效区间）。
    // 占了之后这个槽就不再是空槽，满了返回 -1（调用方按「丢一个包络最小的旧读头」处理，见 dropSmallestEnvelope）。
    int allocate() noexcept
    {
        for (int i = 0; i < kMaxTaps; ++i)
        {
            if (!taps_[i].active)
            {
                taps_[i] = Tap{};
                taps_[i].active = true;
                return i;
            }
        }
        return -1;
    }

    // 丢掉包络最小（最不重要）的读头，返回被丢的下标；没有读头时返回 -1
    int dropSmallestEnvelope() noexcept
    {
        int best = -1;
        float bestEnv = 0.0f;
        for (int i = 0; i < kMaxTaps; ++i)
        {
            if (!taps_[i].active)
            {
                continue;
            }
            const float e = currentEnvelope(taps_[i]);
            if (best < 0 || e < bestEnv)
            {
                best = i;
                bestEnv = e;
            }
        }
        if (best >= 0)
        {
            taps_[best] = Tap{};
        }
        return best;
    }

    void clear() noexcept
    {
        for (int i = 0; i < kMaxTaps; ++i)
        {
            taps_[i] = Tap{};
        }
    }

    int count() const noexcept
    {
        int n = 0;
        for (int i = 0; i < kMaxTaps; ++i)
        {
            if (taps_[i].active)
            {
                ++n;
            }
        }
        return n;
    }

    Tap& at(int index) noexcept { return taps_[index]; }
    const Tap& at(int index) const noexcept { return taps_[index]; }

    // 读头当前的包络值（块边界上的值）：淡出与淡入相乘。两者都不在进行时是 1。
    // 淡出的起点是 1（读头刚建立时满增益），线性降到 0；淡入从 0 线性升到 1。
    // 还没读到第一帧有效数据的新读头，淡入是 0：它现在还不出声。
    static float currentEnvelope(const Tap& tap) noexcept
    {
        float env = 1.0f;
        if (tap.env.fadingOut())
        {
            const auto remaining = tap.env.fadeOutSteps - tap.env.fadeOutStep;
            env *= static_cast<float>(remaining) / static_cast<float>(tap.env.fadeOutSteps);
        }
        if (tap.env.fadingIn())
        {
            env *= static_cast<float>(tap.env.fadeInStep) / static_cast<float>(tap.env.fadeInSteps);
        }
        return env;
    }

private:
    // 读头槽：定长 3 个。不用容器：读头集合的大小是编译期常量，音频线程上不分配。
    Tap taps_[kMaxTaps]{};
};

// 下面五个是读头混音的落点，实现见 Taps.cpp。它们按样本推进包络，与块长无关：
// 同一段输入怎么切块，输出逐位相同。
//
// mixTaps：把全部活跃读头按各自的包络混进 outLeft / outRight。position 是本块的读位起点（Input 已处理的帧数 R）。
//   每个读头读自己的位置 position + i + tap.offset − tap.delay，乘自己第 i 个样本处的包络，加进总和。
//   valid（可空，长度不小于 count）：逐样本写「这个样本是不是环里的真数据」。
//   1 = 至少有一个活跃读头读到了这一帧（且它在有效区间内）；0 = 这一格是补出来的 0。
//   引擎据此区分「补出来的 0」与「真的是 0」——两者的听感完全不同。
//   返回本块读到的非零样本个数。
std::uint64_t mixTaps(const TapSet& taps, const LocalLine& line, i64 position, float* outLeft, float* outRight,
                      std::int64_t count, std::uint8_t* valid = nullptr) noexcept;
// advanceTaps：推进全部读头的淡出 n 个样本。淡出走完的读头被释放。
void advanceTaps(TapSet& taps, std::int64_t n) noexcept;
// settleOnsets：记下每个淡入中的读头在本块末走到了第几步（按位置：本块最后一个读位置 − validFrom + 1）。
//   走完的读头淡入结束（包络恒为 1）。position 是本块的读位起点 R，n 是块长。
void settleOnsets(TapSet& taps, i64 position, std::int64_t n) noexcept;
// largestTapDelay：全部活跃读头里最大的 D（估计窗口用的那个）。没有活跃读头时是 0。
std::int64_t largestTapDelay(const TapSet& taps) noexcept;
// tapsSettledSilent：全部读头都已经彻底安静（没有活跃读头，或每个活跃读头的包络都是 0）。
bool tapsSettledSilent(const TapSet& taps) noexcept;

} // namespace rinne::loop
