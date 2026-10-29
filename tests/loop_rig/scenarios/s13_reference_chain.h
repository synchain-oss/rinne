// SPDX-License-Identifier: GPL-3.0-or-later
//
// S13 的参照安全链：core 级与插件级共用。同一个 SafetyChain 类，控制量与淡入请求的接法照抄 EngineTestKit 的
// InputReference，逐样本喂引擎这一块的读头（Input 引擎的 lastBlockResult）与干声，给出：
//   - 回送（逐位就是引擎的回送：引擎的 Input 输出与 want* 逐位相同时接线没走样）；
//   - 引擎写回宿主缓冲的值 want*；
//   - 超大值判据逐样本看的「超阈」：×g 之后 max(|tap|, |x|) ≥ +12 dBFS，|tap| 只在判为放大时计入、|x| 在判为放大
//     或 g > 1 时计入，回送不活动（g = 0）时不计。
// 只依赖 core 与 core 的测试工具，不依赖 JUCE，也不依赖 core 测试台。
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "core/engine/EngineTestKit.h"
#include "dsp/Sanitize.h"
#include "engine/EngineParams.h"
#include "engine/Meters.h"
#include "engine/SafetyMachine.h"
#include "loop/LoopSession.h"
#include "loop/StreamSync.h"
#include "s13_safety_cells.h"

namespace rinne::s13
{

// 参照安全链：控制量与淡入请求的接法照抄 EngineTestKit 的 InputReference，另外逐样本取出超大值判据看的量
class ReferenceChain
{
public:
    explicit ReferenceChain(const engine::EngineParams& params) : params_(params)
    {
        chain_.setCommandBaseline(0, 0);
        chain_.prepare(kSampleRate);
        dryGain_ = test::referenceDryGain(params.mixPercent);
    }

    // 一块：r 是引擎这一块的 lastBlockResult()，dry 是 Input 之前的干声。结果在 retLeft / retRight / want* / hot 里
    void block(const loop::BlockResult& r, const float* dryLeft, const float* dryRight, int n)
    {
        engine::SafetyControls c;
        c.feedback =
            std::clamp(params_.feedbackPercent, engine::kMinFeedbackPercent, engine::kMaxFeedbackPercent) / 100.0f;
        c.returnGain = test::referenceReturnGain(params_.mixPercent);
        c.loopOn = params_.loopOn;
        c.bypassed = r.unsupportedBlock;
        c.limiterOn = params_.limiterOn;
        c.ceilingDb = params_.ceilingDb;
        c.guardOn = params_.guardOn;
        c.paired = r.paired;
        c.stalled = r.stalledState;
        c.loopDelaySamples = std::max(r.estimatorDelay, r.delayEffective);
        chain_.beginBlock(c);
        if (r.resynced && r.resyncReason == loop::SyncReason::kWriter)
        {
            chain_.requestFadeIn(engine::FadeReason::NewWriter);
        }
        else if (r.resynced && r.resyncReason == loop::SyncReason::kBindSwitch)
        {
            chain_.requestFadeIn(engine::FadeReason::FirstPair);
        }
        const int retLength = r.returnLeft != nullptr && r.returnRight != nullptr ? r.returnLength : 0;
        retLeft.resize(static_cast<std::size_t>(n));
        retRight.resize(static_cast<std::size_t>(n));
        wantLeft.assign(dryLeft, dryLeft + n);
        wantRight.assign(dryRight, dryRight + n);
        hot.resize(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i)
        {
            const auto k = static_cast<std::size_t>(i);
            float l = i < retLength ? r.returnLeft[i] : 0.0f;
            float rr = i < retLength ? r.returnRight[i] : 0.0f;
            const bool valid = i < retLength && r.returnValid != nullptr && r.returnValid[i] != 0;
            const float tapPeak = std::max(std::fabs(dsp::sanitizeSample(l)), std::fabs(dsp::sanitizeSample(rr)));
            chain_.processFront(l, rr, valid);
            const float blockedLeft = l;
            const float blockedRight = rr;
            const float dl = dryLeft[i] * dryGain_;
            const float dr = dryRight[i] * dryGain_;
            chain_.processBack(l, rr, dl, dr);
            // 与 SafetyChain::processBack 同一口径：|x| = |g · 读头（DC 阻断之后）|
            const float g = chain_.lastGain();
            const float xPeak = std::max(std::fabs(blockedLeft * g), std::fabs(blockedRight * g));
            const bool amplifies = chain_.chainAmplifiesNow();
            const float tapTerm = amplifies ? tapPeak : 0.0f;
            const float gainTerm = amplifies || g > 1.0f ? xPeak : 0.0f;
            hot[k] = g > 0.0f && std::max(tapTerm, gainTerm) >= engine::safety::kHugeLevel;
            retLeft[k] = l;
            retRight[k] = rr;
            // 引擎的写法（InputEngine.h）：回送不为 ±0 时写干声 + 回送；为 ±0 且 g_dry 恰为 1 时一位不碰
            if (l != 0.0f)
            {
                wantLeft[k] = dl + l;
            }
            else if (dryGain_ != 1.0f)
            {
                wantLeft[k] = dl;
            }
            if (rr != 0.0f)
            {
                wantRight[k] = dr + rr;
            }
            else if (dryGain_ != 1.0f)
            {
                wantRight[k] = dr;
            }
        }
        chain_.publish(meters_);
        chain_.takeFlushRequest();
    }

    std::vector<float> retLeft;
    std::vector<float> retRight;
    std::vector<float> wantLeft;
    std::vector<float> wantRight;
    std::vector<bool> hot;

private:
    engine::EngineParams params_;
    engine::SafetyChain chain_;
    engine::Meters meters_;
    float dryGain_ = 1.0f;
};

} // namespace rinne::s13
