// SPDX-License-Identifier: GPL-3.0-or-later
//
// S14 与 S15 的 core 级环路：一台 core 测试台，干声是 s14_tap_trace.h 的两个余弦，逐块记下读头、读头槽与事件，
// 外加一次 D 变化之后的共同断言。
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "diag/EventQueue.h"
#include "loop/LoopStatus.h"
#include "loop/StreamSync.h"
#include "s01_rig_kit.h"
#include "s14_tap_trace.h"

namespace rinne::rig::scn
{

inline std::size_t reasonIndex(loop::SyncReason r)
{
    return static_cast<std::size_t>(r);
}

// 一条环路，逐块记下 Input 这一块的读头、读头槽与事件（只记 Input 被调用的块：读头轨迹按 Input 自己的样本坐标）
class TapLeg
{
public:
    TapLeg(int block, const engine::EngineParams& params)
    {
        host = makeHost(kSr, block);
        REQUIRE(host != nullptr);
        host->setBlockScripts(fixedBlocks(block), fixedBlocks(block));
        host->setSource([](std::uint64_t first, float* left, float* right, int n) {
            for (int i = 0; i < n; ++i)
            {
                const auto t = static_cast<std::int64_t>(first) + i;
                left[i] = s14::dryLeft(t);
                right[i] = s14::dryRight(t);
            }
        });
        setParams(params);
    }

    void setParams(const engine::EngineParams& params)
    {
        ParamsScript script;
        script.snapshots = {params};
        host->setParamsScript(script);
    }

    // 宿主速度（拍/分钟），此后每个周期都一样；0 = 宿主没给
    void setBpm(float bpm)
    {
        TransportScript script;
        script.bpm = {bpm};
        host->setTransportScript(script);
    }

    // 改两端的块长（Input 跳过哪些周期由 inputSkips 给，下标是周期号）
    void setBlocks(int block, std::vector<bool> inputSkips = {})
    {
        BlockScript input = fixedBlocks(block);
        input.skips = std::move(inputSkips);
        host->setBlockScripts(input, fixedBlocks(block));
    }

    std::size_t cycle() const { return cycle_; }

    // 跑一个周期；返回 Input 这一周期的样本数（被跳过时为 0）
    int step()
    {
        const int n = host->runCycle(cycle_++);
        host->drainEvents([this](const diag::Event& e) { events.push_back(e); });
        if (n <= 0)
        {
            return n;
        }
        const loop::BlockResult& r = host->inputLastBlock();
        const int length = r.returnLeft != nullptr && r.returnRight != nullptr ? std::min(r.returnLength, n) : 0;
        for (int i = 0; i < n; ++i)
        {
            tap.left.push_back(i < length ? r.returnLeft[i] : 0.0f);
            tap.right.push_back(i < length ? r.returnRight[i] : 0.0f);
        }
        const loop::LoopStatusFields& s = host->inputStatus();
        maxTapsActive = std::max(maxTapsActive, s.tapsActive);
        tapsWithinSlots = tapsWithinSlots && s.tapsActive <= s14::kMaxTaps;
        if (r.resynced && r.resyncReason == loop::SyncReason::kShift && shiftAt < 0)
        {
            shiftAt = static_cast<std::int64_t>(tap.size()) - n;
            xfadeLeftBeforeShift = lastXfadeLeft_;
        }
        lastXfadeLeft_ = s.delayXfadeLeft;
        return n;
    }

    // Input 跑满 samples 个样本（每个周期都必须有样本）
    void run(std::int64_t samples)
    {
        for (std::int64_t done = 0; done < samples;)
        {
            const int n = step();
            if (n <= 0)
            {
                reportStall(cycle_ - 1, n);
                return;
            }
            done += n;
        }
    }

    // 记到的某个事件码的条数
    std::size_t eventCount(std::uint16_t code) const
    {
        return static_cast<std::size_t>(
            std::count_if(events.begin(), events.end(), [code](const diag::Event& e) { return e.code == code; }));
    }

    std::size_t now() const { return tap.size(); }
    const loop::LoopStatusFields& status() const { return host->inputStatus(); }

    std::unique_ptr<EngineHost> host;
    s14::TapTrace tap;
    std::vector<diag::Event> events;
    std::uint32_t maxTapsActive = 0;
    bool tapsWithinSlots = true;
    std::int64_t shiftAt = -1; // 位移重同步那一块的起点（Input 的样本坐标）
    std::int64_t xfadeLeftBeforeShift = -1; // 那一块之前改 D 的交叉淡化还剩多少个样本

private:
    std::size_t cycle_ = 0;
    std::int64_t lastXfadeLeft_ = 0;
};

// 一次 D 变化之后的共同断言：mark 之前 100 ms 读的是旧 D；[mark − 1, 末尾) 步长不超过 B，过渡窗口里直接换到新 D 的反例
// 超过 B；交叉淡化走完之后读的正是新 D（逐位）；读头槽不溢出。
// peakTaps 是块末快照里能看到的最多读头数：交叉淡化跨块时是 2；整段过渡落在一块之内时块末只剩 1。
// dChanges 是到目前为止「改 D」重同步的累计次数。
inline void checkTransition(Verdict& v, const TapLeg& leg, std::size_t mark, std::int64_t oldDelay,
                            std::int64_t newDelay, const s14::Bound& bound, std::uint32_t peakTaps = 2,
                            std::uint32_t dChanges = 1)
{
    const std::size_t end = leg.now();
    RIG_CHECK(v, s14::settledOn(leg.tap, mark - 4800, mark, oldDelay));
    const float stepLeft = s14::maxStep(leg.tap.left, mark - 1, end);
    const float stepRight = s14::maxStep(leg.tap.right, mark - 1, end);
    INFO("max step " << stepLeft << " / " << stepRight << ", bound " << bound.left << " / " << bound.right);
    RIG_CHECK(v, stepLeft <= bound.left);
    RIG_CHECK(v, stepRight <= bound.right);
    RIG_CHECK(v, s14::settledOn(leg.tap, mark + static_cast<std::size_t>(s14::kXfade), end, newDelay));
    const std::size_t window = mark + static_cast<std::size_t>(s14::kXfade);
    RIG_CHECK(v, s14::switchJump(oldDelay, newDelay, mark, window, true) > bound.left);
    RIG_CHECK(v, s14::switchJump(oldDelay, newDelay, mark, window, false) > bound.right);
    const loop::LoopStatusFields& s = leg.status();
    RIG_EQ(v, s.delayEffective, newDelay);
    RIG_EQ(v, s.resyncByReason[reasonIndex(loop::SyncReason::kDChange)], dChanges);
    RIG_EQ(v, s.tapDrop, 0u);
    RIG_CHECK(v, leg.tapsWithinSlots);
    RIG_EQ(v, leg.maxTapsActive, peakTaps);
}

} // namespace rinne::rig::scn
