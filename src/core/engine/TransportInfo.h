// SPDX-License-Identifier: GPL-3.0-or-later
//
// 走带信息：外壳与 core 之间唯一的走带通道。壳把宿主的播放头映射成 TransportInfo，引擎只认这个结构。
// 这个头文件一次定全：之后的卡只读它，要改字段或语义一律返回 needs-user，由统筹裁。
//
// 三件事：
//   - 宿主时间戳 hostNs 与它的有效位：两端上报延迟恒为 0，宿主给不给时间戳只影响「谁在谁前面」的证据。
//     宿主没给就置有效位为假，引擎记成 unknown，绝不猜。
//   - 走带状态 isPlaying：起播沿（上一块没走、这一块走了）触发起播复位。
//   - 离线标记 nonrealtime：只用于诊断与「上次导出可能与播放不同」的提示，不改变 DSP。
//   - 速度 bpm：节拍同步用；宿主没给或给的值超出 20..999 就是无效，引擎回退到 120 并给提示码。
//
// 线程：消息线程每块读一次宿主播放头，填成一份快照交给引擎；引擎在音频线程只读这份快照。
// 引擎不读墙钟：起播沿靠 isPlaying 的跳变判定，不靠时间。
#pragma once

#include <cstdint>

namespace rinne::engine
{

// 宿主速度的有效范围（计划口径）：超出就是没给
inline constexpr float kMinHostBpm = 20.0f;
inline constexpr float kMaxHostBpm = 999.0f;
// 宿主没给速度时的回退值
inline constexpr float kFallbackBpm = 120.0f;

// 走带信息的一份快照。
struct TransportInfo
{
    // 宿主时间戳（纳秒）与它的有效位。宿主没给时 ns 为 0、valid 为假。
    std::uint64_t hostNs = 0;
    bool hostNsValid = false;

    // 宿主正在走带（含预滚与循环）
    bool isPlaying = false;

    // 宿主在离线渲染（导出）。只用于诊断与提示，不改变 DSP。
    bool nonrealtime = false;

    // 宿主速度（拍/分钟）。0 或超出有效范围 = 没给，引擎回退到 kFallbackBpm。
    float bpm = 0.0f;
};

} // namespace rinne::engine
