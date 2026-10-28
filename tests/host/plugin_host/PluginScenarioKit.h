// SPDX-License-Identifier: GPL-3.0-or-later
//
// 插件级场景复验共用的小工具：经参数树写参数、两个余弦的信号源、逐块记下读头（Input 引擎的 lastBlockResult 里的回送，
// 即安全链的输入，与 core 级同一个观测点）与读头槽。判据与阈值和 core 级共用 tests/loop_rig/scenarios 下的头文件。
#pragma once

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "host/plugin_host/PluginHost.h"
#include "host/wiring/WiringTestKit.h"
#include "loop/LoopSession.h"
#include "loop/LoopStatus.h"
#include "loop/StreamSync.h"
#include "loop_rig/scenarios/s14_tap_trace.h"

namespace rinne::plugin_host
{

// 按参数表 ID 写一个参数的明文值（宿主自动化的写法，不经界面手势），返回读回的值
inline float setParam(PluginHost& host, const char* id, float plain)
{
    wiring_test::hostSetParam(host.input(), id, plain);
    return wiring_test::paramValue(host.input(), id);
}

// 同上，并要求读回的正是写的值（判据逐位依赖它的参数用这个）
inline void setParamExact(PluginHost& host, const char* id, float plain)
{
    INFO("parameter " << id << " = " << plain);
    REQUIRE(setParam(host, id, plain) == plain);
}

// 两个声道各一个低频余弦，按轨道上的样本号（s14_tap_trace.h 的信号）
inline void useTones(PluginHost& host)
{
    host.source = [](std::int64_t index, float& left, float& right) {
        left = s14::dryLeft(index);
        right = s14::dryRight(index);
    };
}

inline std::size_t reasonIndex(loop::SyncReason r)
{
    return static_cast<std::size_t>(r);
}

// 逐个 Input 块记下读头与读头槽（读头轨迹按 Input 自己的样本号排）
class TapRecorder
{
public:
    explicit TapRecorder(PluginHost& host) : host_(host)
    {
        host.onInputBlock = [this](const InputBlock& block) { record(block); };
    }

    std::size_t now() const { return tap.size(); }

    s14::TapTrace tap;
    std::uint32_t maxTapsActive = 0;
    bool tapsWithinSlots = true;
    std::int64_t shiftAt = -1; // 位移重同步那一块的起点（Input 的样本号）
    std::int64_t xfadeLeftBeforeShift = -1; // 那一块之前改 D 的交叉淡化还剩多少个样本

private:
    void record(const InputBlock& block)
    {
        const loop::BlockResult& r = host_.inputEngine().lastBlockResult();
        const int n = block.n;
        const int length = r.returnLeft != nullptr && r.returnRight != nullptr ? std::min(r.returnLength, n) : 0;
        for (int i = 0; i < n; ++i)
        {
            tap.left.push_back(i < length ? r.returnLeft[i] : 0.0f);
            tap.right.push_back(i < length ? r.returnRight[i] : 0.0f);
        }
        const loop::LoopStatusFields s = host_.inputEngine().loopStatus();
        maxTapsActive = std::max(maxTapsActive, s.tapsActive);
        tapsWithinSlots = tapsWithinSlots && s.tapsActive <= s14::kMaxTaps;
        if (r.resynced && r.resyncReason == loop::SyncReason::kShift && shiftAt < 0)
        {
            shiftAt = block.first;
            xfadeLeftBeforeShift = lastXfadeLeft_;
        }
        lastXfadeLeft_ = s.delayXfadeLeft;
    }

    PluginHost& host_;
    std::int64_t lastXfadeLeft_ = 0;
};

} // namespace rinne::plugin_host
