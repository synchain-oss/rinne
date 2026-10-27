// SPDX-License-Identifier: GPL-3.0-or-later
//
// 读头：每个读头在自己的绝对位置上读本地延迟线，并带一个自己的包络。
// 最多 3 个读头：改 D 的交叉淡化要两个（旧的与新的），交叉淡化进行中又遇重同步时，旧 A、旧 B 与新读头要同时在场，
// 3 个才够。
//
// 一个读头的字段：
//   off     同步偏移。读的位置 = R + off + i − D。
//   delay   这个读头用的 D（样本）。
//   validFrom / validTo  有效区间：区间外读成 0。validFrom 是「从哪个绝对位置起才有数据」，
//           validTo 是「到哪个绝对位置为止还有数据」（+∞ 表示没有上界）。
//   env     包络：0..1 的线性淡变，用来淡入（新读头）与淡出（旧读头）。
//
// 改 D 的交叉淡化不是读头自己的包络，而是整个读头集合的一段状态（同一时刻至多一段）：旧读头（outgoing）与新读头
// （incoming）的增益逐样本出自同一个函数，两路之和恒为 1（见 DelayRetarget.h）。它与下面的淡出、淡入相乘、互相独立：
// 交叉淡化进行中遇到一次重同步，两路读头各自再挂上 5 ms 淡出，一起淡进各自的尾巴，一个读头都不丢；
// 5 ms 之内连遇第二次、槽不够第四个读头时，收掉包络最小的那个（见 LoopSession::newTap）。
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

#include <algorithm>
#include <cstdint>

#include "loop/DelayRetarget.h"
#include "loop/LocalLine.h"
#include "loop/LoopTiming.h"

namespace rinne::loop
{

// 读头槽数：改 D 的交叉淡化 + 重同步同时发生时，旧 A、旧 B 与新读头要同时在场
inline constexpr int kMaxTaps = 3;
// 尾巴区的格号就是读头的槽号：每个读头一格，互不覆盖
static_assert(LocalLine::kTailSlots == kMaxTaps, "尾巴区每个读头槽一格");

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

// 读头在改 D 的交叉淡化里的角色。
enum class XfadeRole : std::uint8_t
{
    kNone = 0, // 不在交叉淡化里
    kOutgoing = 1, // 旧读头：增益 1 − x，走完退场
    kIncoming = 2, // 新读头：增益 x，走完成为普通读头
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
    XfadeRole xfade = XfadeRole::kNone;
    // 最近一次混音里，这个读头在最后一个样本上的贡献（已乘包络）与那个样本的输出下标（R 坐标；-1 = 没有）。
    // 写头恰好在块边界落后时，环路层靠它「保持末值」：回送不从上一块的末值直接落到 0。
    float lastLeft = 0.0f;
    float lastRight = 0.0f;
    i64 lastAt = -1;
};

// 末值保持：回送要落到 0、而本块一个可读的样本都没有时（写头恰好在块边界落后、两端停用后清空），
// 不在块首从上一个样本的值直接落到 0，而是从那个值起线性衰减（名义 5 ms，跨块延续）。
// 从开始保持的那个样本起数，第 i 个样本是 value·(L − i)/L：第一个样本等于上一个样本的值（相邻差为 0），
// 之后每个样本降 value/L，第 L 个样本起为 0。只保持一个值、不读延迟线：清空之后不会再回放任何旧数据。
// 按样本推进，与块长无关。
struct ReturnHold
{
    float left = 0.0f;
    float right = 0.0f;
    std::int64_t steps = 0; // 0 = 不在保持
    std::int64_t step = 0;

    bool active() const noexcept { return steps > 0; }
    // 还要出多少个样本才走完
    std::int64_t remaining() const noexcept { return active() ? steps - step : 0; }
    // 从 (l, r) 起保持 length 个样本。正在保持时，先把它在下一个样本上的余值并进来：两段叠在一起也不阶跃。
    // (l, r) 是 0 时什么都不做（正在进行的保持照旧走完）。
    void start(float l, float r, std::int64_t length) noexcept;
    // 叠加进 out（count 个样本）并推进；out 为空时只推进
    void addInto(float* outLeft, float* outRight, std::int64_t count) noexcept;
};

// 读头集合：最多 kMaxTaps 个。
class TapSet
{
public:
    // 找一个空槽并占住它：槽里的读头被清成初始值，active 置真（调用方接着填 offset / delay / 有效区间）。
    // 占了之后这个槽就不再是空槽，满了返回 -1（调用方按「收掉一个包络最小的读头」处理，见 smallestEnvelopeSlot）。
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

    // 包络最小（最不重要）的活跃读头的槽号（块首的包络：淡出、淡入与交叉淡化相乘）；并列时取槽号小的。
    // 没有活跃读头时返回 -1。不改任何东西：调用方要先看一眼它的末值再收掉它。
    int smallestEnvelopeSlot() const noexcept
    {
        int best = -1;
        float bestEnv = 0.0f;
        for (int i = 0; i < kMaxTaps; ++i)
        {
            if (!taps_[i].active)
            {
                continue;
            }
            const float e = envelopeNow(taps_[i]);
            if (best < 0 || e < bestEnv)
            {
                best = i;
                bestEnv = e;
            }
        }
        return best;
    }

    void clear() noexcept
    {
        for (int i = 0; i < kMaxTaps; ++i)
        {
            taps_[i] = Tap{};
        }
        xfadeLength_ = 0;
        xfadeStep_ = 0;
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

    // 主读头：第一个既没在淡出、也不是交叉淡化里的旧读头的活跃读头。没有时返回 -1。
    // 交叉淡化一开始，新读头就是主读头（发布的 D、拉环不足的统计都认它），旧读头照自己的增益走完。
    int mainSlot() const noexcept
    {
        for (int i = 0; i < kMaxTaps; ++i)
        {
            const Tap& tap = taps_[i];
            if (tap.active && !tap.env.fadingOut() && tap.xfade != XfadeRole::kOutgoing)
            {
                return i;
            }
        }
        return -1;
    }

    // ---- 改 D 的线性交叉淡化（同一时刻至多一段）----

    // 开始一段交叉淡化：总长 length 个样本；stepAtBlockStart 是本块第一个样本上的步数，可以为负
    // （交叉淡化在本块中途才开始：开始之前旧读头满增益、新读头 0）。角色由调用方标在读头上。
    void beginCrossfade(std::int64_t length, std::int64_t stepAtBlockStart) noexcept
    {
        xfadeLength_ = length > 0 ? length : 1;
        xfadeStep_ = stepAtBlockStart;
    }
    bool crossfadeActive() const noexcept { return xfadeLength_ > 0; }
    std::int64_t crossfadeLength() const noexcept { return xfadeLength_; }
    // 本块第一个样本上的步数（块末推进之后就是下一块第一个样本上的步数）
    std::int64_t crossfadeStep() const noexcept { return xfadeStep_; }
    // 还要多少个样本走完；没有交叉淡化时是 0
    std::int64_t crossfadeRemaining() const noexcept
    {
        return crossfadeActive() ? xfadeLength_ - std::max<std::int64_t>(xfadeStep_, 0) : 0;
    }
    // 本块第 i 个样本上的一对增益
    XfadeGains crossfadeGainsAt(std::int64_t i) const noexcept
    {
        return linearXfadeGains(xfadeStep_ + i, xfadeLength_);
    }
    // 推进 n 个样本。走完时旧读头退场、新读头成为普通读头；两路读头都没了（被重同步的淡出走完）也收尾。
    void advanceCrossfade(std::int64_t n) noexcept;

    // 读头此刻（块首）的增益：淡出、淡入与交叉淡化相乘。
    float envelopeNow(const Tap& tap) const noexcept
    {
        float env = currentEnvelope(tap);
        if (crossfadeActive() && tap.xfade != XfadeRole::kNone)
        {
            const XfadeGains g = crossfadeGainsAt(0);
            env *= tap.xfade == XfadeRole::kOutgoing ? g.out : g.in;
        }
        return env;
    }

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
    // 交叉淡化的进度：总长为 0 表示没有。
    std::int64_t xfadeLength_ = 0;
    std::int64_t xfadeStep_ = 0;
};

// 下面这几个是读头混音的落点，实现见 Taps.cpp。它们按样本推进包络，与块长无关：
// 同一段输入怎么切块，输出逐位相同。
//
// mixTaps：把全部活跃读头按各自的包络混进 outLeft / outRight。position 是本块的读位起点（Input 已处理的帧数 R）。
//   每个读头读自己的位置 position + i + tap.offset − tap.delay，乘自己第 i 个样本处的包络，加进总和。
//   valid（可空，长度不小于 count）：逐样本写「这个样本是不是环里的真数据」。
//   1 = 至少有一个活跃读头读到了这一帧（且它在有效区间内）；0 = 这一格是补出来的 0。
//   引擎据此区分「补出来的 0」与「真的是 0」——两者的听感完全不同。
//   每个活跃读头记下自己在最后一个样本上的贡献（Tap::lastLeft / lastRight / lastAt），包络与读位都不动。
//   返回本块读到的非零样本个数。
std::uint64_t mixTaps(TapSet& taps, const LocalLine& line, i64 position, float* outLeft, float* outRight,
                      std::int64_t count, std::uint8_t* valid = nullptr) noexcept;
// countNonZero：count 个样本里左右任一声道不为 0（高于 denormal 下限）的个数。
std::uint64_t countNonZero(const float* left, const float* right, std::int64_t count) noexcept;
// advanceTaps：推进全部读头的淡出 n 个样本。淡出走完的读头被释放。
void advanceTaps(TapSet& taps, std::int64_t n) noexcept;
// settleOnsets：记下每个淡入中的读头在本块末走到了第几步（按位置：本块最后一个读位置 − validFrom + 1）。
//   走完的读头淡入结束（包络恒为 1）。position 是本块的读位起点 R，n 是块长。
void settleOnsets(TapSet& taps, i64 position, std::int64_t n) noexcept;
// largestTapDelay：全部活跃读头里最大的 D（估计窗口用的那个）。没有活跃读头时是 0。
std::int64_t largestTapDelay(const TapSet& taps) noexcept;
// mainTapDelay：主读头（见 TapSet::mainSlot）正在读的 D。没有主读头时返回 fallback。
std::int64_t mainTapDelay(const TapSet& taps, std::int64_t fallback) noexcept;
// tapsSettledSilent：全部读头都已经彻底安静（没有活跃读头，或每个活跃读头的包络都是 0）。
bool tapsSettledSilent(const TapSet& taps) noexcept;

} // namespace rinne::loop
