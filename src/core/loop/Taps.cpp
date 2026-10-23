// SPDX-License-Identifier: GPL-3.0-or-later
//
// 读头：读头的增删在 Taps.h 里内联，包络的推进与读头的混音在这一份。
//
// 几何：读头的读位置是 p = 位置 + tap.offset − tap.delay。
//   位置    本块的读位起点（Input 已处理的帧数 R）
//   offset  同步偏移 off：环的写位置与 R 的差（配对或重同步时锚定，见 StreamSync）
//   delay   这个读头用的 D（样本）
// 于是稳态下，输出索引 R+i 上播的是环位置 R+off+i−D，也就是写方在输出索引 R+i−D 上写下的那一帧：
// 环路延迟恰为 D。
//
// 包络按样本推进，与块长无关：同一段输入怎么切块，输出逐位相同（安全不变量 7）。
// 混音按样本算包络（本块内的第 i 个样本用「本块起点 + i」处的包络），块末再把淡出推进 n 个样本。
// 淡入按位置算：读位置 p 处的淡入是 (p − validFrom + 1) / onset（截在 [0, 1]），从第一帧有效数据起淡入。
//
// 实时约定：不分配、不加锁、不读时钟、不抛异常。
#include <algorithm>
#include <cmath>

#include "loop/LocalLine.h"
#include "loop/Taps.h"

namespace rinne::loop
{

namespace
{
// 混音时的浮点下限：低于它的值当成 0，避免 denormal 拖慢音频线程。
constexpr float kDenormalFloor = 1.0e-30f;

// 第 i 个样本处的淡出：起点是 1（读头刚挂上淡出时满增益），线性降到 0。不在淡出时是 1。
float fadeOutAt(const Tap& tap, std::int64_t i) noexcept
{
    if (!tap.env.fadingOut())
    {
        return 1.0f;
    }
    const auto remaining = tap.env.fadeOutSteps - (tap.env.fadeOutStep + i);
    return remaining > 0 ? static_cast<float>(remaining) / static_cast<float>(tap.env.fadeOutSteps) : 0.0f;
}

// 读位置 p 处的淡入：从 validFrom 起按位置线性升到 1（第一帧有效数据是 1/onset）。不在淡入时是 1。
float fadeInAt(const Tap& tap, i64 p) noexcept
{
    if (!tap.env.fadingIn())
    {
        return 1.0f;
    }
    const i64 step = p - tap.validFrom + 1;
    if (step <= 0)
    {
        return 0.0f;
    }
    if (step >= tap.env.fadeInSteps)
    {
        return 1.0f;
    }
    return static_cast<float>(static_cast<double>(step) / static_cast<double>(tap.env.fadeInSteps));
}
} // namespace

// 读头槽数是 3：交叉淡化要两个，重同步时新旧要同时存在。少了就不够。
static_assert(kMaxTaps >= 3, "读头槽至少要有 3 个：交叉淡化两个、重同步的尾巴一个");
static_assert(kMaxTaps == 3, "读头槽数是 3，改它是接口事件");

// 把全部活跃读头按各自的包络混进 outLeft / outRight。
//
// 混音规则：每个读头读自己的位置（position + i + tap.offset − tap.delay），乘自己第 i 个样本处的包络，
// 加进总和。读不到的位置（不在读头的有效区间里、或不在延迟线的有效区间里）贡献 0。
//
// outLeft / outRight 必须至少有 count 个元素；调用方（LoopSession）在 prepare 里准备好。
//
// valid（可空，长度不小于 count）：逐样本写「这个样本是不是环里的真数据」。
// 1 = 至少有一个活跃读头读到了这一帧；0 = 这一格是补出来的 0。引擎据此区分两者。
//
// 返回值：本块里读到的非零样本个数（快速路径的判据之一）。
std::uint64_t mixTaps(const TapSet& taps, const LocalLine& line, i64 position, float* outLeft, float* outRight,
                      std::int64_t count, std::uint8_t* valid) noexcept
{
    if (outLeft == nullptr || outRight == nullptr || count <= 0)
    {
        return 0;
    }
    for (std::int64_t i = 0; i < count; ++i)
    {
        outLeft[i] = 0.0f;
        outRight[i] = 0.0f;
        if (valid != nullptr)
        {
            valid[i] = 0;
        }
    }
    std::uint64_t nonZero = 0;
    for (int slot = 0; slot < kMaxTaps; ++slot)
    {
        const Tap& tap = taps.at(slot);
        if (!tap.active)
        {
            continue;
        }
        if (fadeOutAt(tap, 0) <= 0.0f)
        {
            continue; // 淡出已经走完：这个读头不贡献任何东西
        }
        for (std::int64_t i = 0; i < count; ++i)
        {
            const i64 p = position + i + tap.offset - tap.delay;
            // 读头的有效区间是半开的 [validFrom, validTo)：区间外一律读成 0，绝不读旧数据
            if (p < tap.validFrom || p >= tap.validTo)
            {
                continue;
            }
            const float env = fadeOutAt(tap, i) * fadeInAt(tap, p);
            if (env <= 0.0f)
            {
                continue;
            }
            float l = 0.0f;
            float r = 0.0f;
            // 先查尾巴快照（预拷贝的那一段），再查延迟线主区
            if (!line.readTail(p, l, r) && !line.read(p, l, r))
            {
                continue; // 位置不在有效区间里：这一格贡献 0
            }
            outLeft[i] += l * env;
            outRight[i] += r * env;
            if (valid != nullptr)
            {
                valid[i] = 1; // 这一格是环里的真数据（不是补出来的 0）
            }
        }
    }
    for (std::int64_t i = 0; i < count; ++i)
    {
        if (std::fabs(outLeft[i]) > kDenormalFloor || std::fabs(outRight[i]) > kDenormalFloor)
        {
            ++nonZero;
        }
    }
    return nonZero;
}

// 推进全部读头的淡出 n 个样本。淡出走完的读头被释放。淡入按位置算，不在这里推进（见 settleOnsets）。
void advanceTaps(TapSet& taps, std::int64_t n) noexcept
{
    if (n <= 0)
    {
        return;
    }
    for (int i = 0; i < kMaxTaps; ++i)
    {
        Tap& tap = taps.at(i);
        if (!tap.active)
        {
            continue;
        }
        if (tap.env.fadingOut())
        {
            tap.env.fadeOutStep += n;
            if (tap.env.fadeOutStep >= tap.env.fadeOutSteps)
            {
                tap = Tap{};
            }
        }
    }
}

// 记下每个淡入中的读头在本块末走到了第几步。本块最后一个读位置是 position + n − 1 + off − D，
// 走到的步数 = 它 − validFrom + 1（还没读到有效数据时是 0）。走满 onset 的读头淡入结束。
void settleOnsets(TapSet& taps, i64 position, std::int64_t n) noexcept
{
    if (n <= 0)
    {
        return;
    }
    for (int i = 0; i < kMaxTaps; ++i)
    {
        Tap& tap = taps.at(i);
        if (!tap.active || !tap.env.fadingIn())
        {
            continue;
        }
        const i64 last = position + n - 1 + tap.offset - tap.delay;
        const i64 step = last - tap.validFrom + 1;
        if (step >= tap.env.fadeInSteps)
        {
            tap.env.fadeInSteps = 0;
            tap.env.fadeInStep = 0;
        }
        else
        {
            tap.env.fadeInStep = step > 0 ? step : 0;
        }
    }
}

// 全部活跃读头里最大的 D：估计窗口用的那个（安全链的链增益估计窗口要盖住它）。
// 没有活跃读头时返回 0。
std::int64_t largestTapDelay(const TapSet& taps) noexcept
{
    std::int64_t largest = 0;
    for (int slot = 0; slot < kMaxTaps; ++slot)
    {
        const Tap& tap = taps.at(slot);
        if (tap.active && tap.delay > largest)
        {
            largest = tap.delay;
        }
    }
    return largest;
}

// 主读头 = 第一个没在淡出的活跃读头（改 D 的过渡、拉环不足时的淡出都认这一个）。没有时返回 fallback。
std::int64_t mainTapDelay(const TapSet& taps, std::int64_t fallback) noexcept
{
    for (int slot = 0; slot < kMaxTaps; ++slot)
    {
        const Tap& tap = taps.at(slot);
        if (tap.active && !tap.env.fadingOut())
        {
            return tap.delay;
        }
    }
    return fallback;
}

// 全部读头都已经彻底安静：没有活跃读头，或者每个活跃读头的包络都是 0。
// 快速路径（逐位直通的干声）据此决定要不要走完整路径。
bool tapsSettledSilent(const TapSet& taps) noexcept
{
    for (int slot = 0; slot < kMaxTaps; ++slot)
    {
        const Tap& tap = taps.at(slot);
        if (!tap.active)
        {
            continue;
        }
        // 淡入中的读头（含还没读到第一帧有效数据的新读头）马上要出声：不算安静。
        if (tap.env.fadingIn() || TapSet::currentEnvelope(tap) > 0.0f)
        {
            return false;
        }
    }
    return true;
}

} // namespace rinne::loop
