// SPDX-License-Identifier: GPL-3.0-or-later
//
// 脚本：每周期给什么块长、什么走带、什么参数、按什么顺序驱动两端。
//
// 能力（对应下游场景要的脚本清单）：
//   1. 每周期的调用顺序（Input 先 / Output 先 / 同周期）。
//   2. 两端各自的块长脚本：恒定、随机、交替、0 样本穿插、违约块（n 大于声明值）、超长块（n 大于 32768）。
//   3. 单端跳过 k 块：这一端这一周期不被调用。
//   4. 每周期的 TransportInfo 脚本：走带状态、速度、时间戳及其有效位。同一周期两端拿到同一个时间戳
//      （错序判定靠它；两端拿不同的值是另一回事，场景要自己造）。
//   5. 逐块的 EngineParams 脚本。
//
// 脚本是纯值：宿主每周期填一份新的（按值，不复用同一份）。

#pragma once

#include <cstdint>
#include <vector>

#include "engine/EngineParams.h"
#include "engine/TransportInfo.h"

namespace rinne::rig
{

// 一端的块长脚本。
struct BlockScript
{
    // 每一项是一端的块长。0 = 0 样本调用（引擎必须立刻返回、不推进计数）。
    // 大于声明值 = 违约块；大于 32768 = 超长块（引擎只出干声并给提示码）。
    std::vector<int> lengths;
    // 每一项是「这一端这一周期跳过」（不被调用）。长度与 lengths 一样；短的就按 false 补。
    std::vector<bool> skips;
    // 每一项是「这一端这一周期用第几个块长项」。默认逐项对应。
    // 用它可以两位共用一份长度表但错开推进。
    std::vector<int> cursors;

    // 取这一端的第 index 个块长（循环取用）。skip 单独看 skips。
    int lengthAt(std::size_t index) const noexcept
    {
        if (lengths.empty())
        {
            return 0;
        }
        const std::size_t at = cursors.size() > index ? static_cast<std::size_t>(cursors[index]) : index;
        return lengths[at % lengths.size()];
    }

    bool skipAt(std::size_t index) const noexcept { return skips.size() > index && skips[index]; }
};

// 每周期给两端的走带脚本。
struct TransportScript
{
    // 每一项：宿主时间戳（纳秒）。0 = 宿主没给。
    std::vector<std::uint64_t> hostNs;
    // 每一项：时间戳的有效位。
    std::vector<bool> hostNsValid;
    // 每一项：宿主在走带。
    std::vector<bool> playing;
    // 每一项：宿主在离线渲染。
    std::vector<bool> nonrealtime;
    // 每一项：宿主速度（拍/分钟）。0 = 没给。
    std::vector<float> bpm;

    engine::TransportInfo at(std::size_t index) const noexcept
    {
        engine::TransportInfo info;
        if (!hostNs.empty())
        {
            info.hostNs = hostNs[index % hostNs.size()];
        }
        if (!hostNsValid.empty())
        {
            info.hostNsValid = hostNsValid[index % hostNsValid.size()];
        }
        if (!playing.empty())
        {
            info.isPlaying = playing[index % playing.size()];
        }
        if (!nonrealtime.empty())
        {
            info.nonrealtime = nonrealtime[index % nonrealtime.size()];
        }
        if (!bpm.empty())
        {
            info.bpm = bpm[index % bpm.size()];
        }
        return info;
    }
};

// 逐块的参数脚本。每一项是一份完整的参数快照。
struct ParamsScript
{
    std::vector<engine::EngineParams> snapshots;

    engine::EngineParams at(std::size_t index) const noexcept
    {
        if (snapshots.empty())
        {
            return engine::EngineParams{};
        }
        return snapshots[index % snapshots.size()];
    }
};

// 每周期的调用顺序。
enum class CallOrder : std::uint8_t
{
    kInputFirst = 0, // 先驱动 Input，后驱动 Output
    kOutputFirst = 1, // 先驱动 Output，后驱动 Input
    kOutputOnly = 2, // 只驱动 Output（Input 这一周期不调）
    kInputOnly = 3, // 只驱动 Input
};

} // namespace rinne::rig
