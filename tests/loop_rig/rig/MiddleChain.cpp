// SPDX-License-Identifier: GPL-3.0-or-later
//
// 中间链的实现。见 MiddleChain.h 的说明。

#include "rig/MiddleChain.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace rinne::rig
{

void MiddleChain::prepare(double sampleRate, int maxBlock) noexcept
{
    (void)maxBlock;
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;

    // 一阶低通的系数：a = 1 - exp(-2π·fc/sr)。截止频率非法时取 1000 Hz。
    updateLowpass();
    updateShelf();

    // 延迟线：长度恰好是 k（k = 0 时不延迟）。
    openDelayLine(delaySamples_);

    latency_ = delaySamples_;
}

// 一阶低通的系数。改截止频率时也要重算：系数不重算，低通形态就是旧的。
void MiddleChain::updateLowpass() noexcept
{
    const double fc = lowpassHz_ > 0.0 ? static_cast<double>(lowpassHz_) : 1000.0;
    lpA_ = static_cast<float>(1.0 - std::exp(-2.0 * 3.14159265358979323846 * fc / sampleRate_));
}

// 低架提升的增益：低频 1 + boost，高频 1 − boost·hfMix。boost 夹在 0..1。
void MiddleChain::updateShelf() noexcept
{
    const float boost = std::clamp(shelfBoost_, 0.0f, 1.0f);
    shelfLowGain_ = 1.0f + boost;
    shelfHighGain_ = 1.0f - boost * std::clamp(shelfHfMix_, 0.0f, 1.0f);
}

// 延迟线：长度恰好是 k。用「先读后写」的环形缓冲：第 i 格存的正是 k 个样本之前的输入。
void MiddleChain::openDelayLine(int samples) noexcept
{
    const int k = samples > 0 ? samples : 0;
    if (k == 0)
    {
        delayLeft_.clear();
        delayRight_.clear();
        delayCursor_ = 0;
        return;
    }
    const auto want = static_cast<std::size_t>(k);
    if (delayLeft_.size() != want)
    {
        delayLeft_.assign(want, 0.0f);
        delayRight_.assign(want, 0.0f);
        delayCursor_ = 0;
    }
}

void MiddleChain::setLowpassHz(float hz) noexcept
{
    lowpassHz_ = hz;
    updateLowpass();
}

void MiddleChain::setLowShelfBoost(float boost, float hfMix) noexcept
{
    shelfBoost_ = boost;
    shelfHfMix_ = hfMix;
    updateShelf();
}

void MiddleChain::setDelaySamples(int samples) noexcept
{
    delaySamples_ = samples > 0 ? samples : 0;
    openDelayLine(delaySamples_);
    latency_ = delaySamples_;
}

void MiddleChain::reset() noexcept
{
    lpLeft_ = 0.0f;
    lpRight_ = 0.0f;
    shelfLeft_ = 0.0f;
    shelfRight_ = 0.0f;
    std::fill(delayLeft_.begin(), delayLeft_.end(), 0.0f);
    std::fill(delayRight_.begin(), delayRight_.end(), 0.0f);
    delayCursor_ = 0;
    sampleCounter_ = 0;
}

void MiddleChain::process(const float* inLeft, const float* inRight, float* outLeft, float* outRight, int n) noexcept
{
    if (n <= 0 || inLeft == nullptr || inRight == nullptr || outLeft == nullptr || outRight == nullptr)
    {
        return;
    }

    for (int i = 0; i < n; ++i)
    {
        float l = inLeft[i];
        float r = inRight[i];

        switch (kind_)
        {
        case ChainKind::kIdentity:
            break;

        case ChainKind::kGain:
            l *= gain_;
            r *= gain_;
            break;

        case ChainKind::kOnePoleLowpass: {
            lpLeft_ += lpA_ * (l - lpLeft_);
            lpRight_ += lpA_ * (r - lpRight_);
            l = lpLeft_;
            r = lpRight_;
            break;
        }

        case ChainKind::kDelay: {
            // 延迟 k 个样本：输出是 k 个样本之前的输入。
            // 延迟线用「先读后写」的环形缓冲：读到的那一格恰好是 k 个样本前写进去的值。
            // 长度恰好是 k，不一定是 2 的幂：游标按长度回绕（不能用按位与）。
            if (!delayLeft_.empty())
            {
                const std::size_t slot = delayCursor_;
                const float dl = delayLeft_[slot];
                const float dr = delayRight_[slot];
                delayLeft_[slot] = l;
                delayRight_[slot] = r;
                delayCursor_ = slot + 1u < delayLeft_.size() ? slot + 1u : 0u;
                l = dl;
                r = dr;
            }
            break;
        }

        case ChainKind::kNanInject: {
            // 从 nanAt_ 个样本起往后都是 NaN：用来验 Output 侧的清洗（NaN → 0）与计数。
            if (nanAt_ >= 0 && sampleCounter_ >= static_cast<std::int64_t>(nanAt_))
            {
                l = std::numeric_limits<float>::quiet_NaN();
                r = l;
            }
            break;
        }

        case ChainKind::kInvert:
            l = -l;
            r = -r;
            break;

        case ChainKind::kLowShelfBoost: {
            shelfLeft_ += lpA_ * (l - shelfLeft_);
            shelfRight_ += lpA_ * (r - shelfRight_);
            l = shelfLowGain_ * shelfLeft_ + (l - shelfLeft_) * shelfHighGain_;
            r = shelfLowGain_ * shelfRight_ + (r - shelfRight_) * shelfHighGain_;
            break;
        }
        }

        outLeft[i] = l;
        outRight[i] = r;
        ++sampleCounter_;
    }
}

} // namespace rinne::rig
