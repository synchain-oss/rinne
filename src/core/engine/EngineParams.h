// SPDX-License-Identifier: GPL-3.0-or-later
//
// 引擎的输入面：外壳与 core 之间的唯一通道。壳只做映射（宿主参数 → EngineParams，宿主播放头 → TransportInfo，
// 宿主缓冲 → AudioView），core 只认这里定义的类型。这个头文件一次定全：之后的卡只读它，
// 要改字段或语义一律返回 needs-user，由统筹裁。
//
// 宿主音频缓冲的视图在 AudioView.h（它不是参数：参数面只有这一份）。
//
// 本文件里有三样东西：
//   1. EngineParams：11 个参数 + 3 个命令序号 + 3 个工程设置。
//   2. CommandBaseline：命令序号的基线。
//   3. DelayMode 与 TailMode 两个枚举（它们是参数的取值，不进参数表）。
//
// 「本块是不是有界」不在参数面：违约与超长是块级事实，由环路层按块长 n 判定
// （n 大于声明值补 0 并抬升延迟下限；n 大于 32768 判超长），结果由 BlockResult 的标志给出。
// 参数面不装块级事实：宿主违约一次，参数就脏一次。
//
// 11 个参数（都带单位；ID、范围与可自动化与否在工厂参数表里，这里不重复）：
//   feedback   反馈量，0..150 %，默认 0（默认安全：反馈为 0）
//   loop_on    环路开关，默认开
//   mix        混合，0..100 %，默认 25。线性律：m = mixPercent / 100（0..1 的比值），
//               g_dry = min(1, 2(1-m))、g_ret = min(1, 2m)；
//               m ≤ 50 % 时 g_dry 恰为 1，mix 为 0 时干声逐位直通。
//               四个点：0 % → 1/0；25 % → 1/0.5；50 % → 1/1；100 % → 0/1
//   delay_mode 时间 / 节拍，默认时间
//   delay_ms   环路延迟，1..4000 ms，默认 100
//   delay_note 时值，16 档的下标（0..15），默认 10（1/4）
//   low_cut    低切，20(=关)..2000 Hz，默认关
//   high_cut   高切，1000..20000(=关) Hz，默认关
//   polarity   极性反转，默认关
//   limiter    限制器，默认开
//   ceiling    限制器上限，-24..0 dBFS，默认 -6
//
// 三个命令序号：界面动作不是参数，改一下加一，引擎在音频线程比对。实例第一块只记下序号、不执行，
// 所以重载工程不会重放旧命令（紧急切断尤其不能重放）。
//
// presetSeq 与 stateSeq 分开的原因：安全状态机有两种不同的淡入原因
// （FadeReason::Preset = 载入工厂预设；FadeReason::SetState = 宿主设置工程状态，含宿主预设、A/B、撤销）。
// 合成一个序号的话，引擎拿到「序号变了」也分不出该报哪一个原因，两处口径就对不上。
//   presetSeq  载入工厂预设
//   stateSeq   宿主设置工程状态（宿主预设、A/B、撤销都走这个）
// 两者都在块首比对「与上一块不同就执行一次」，都不重放旧命令。
//
// 三个来自工程设置的开关：失控保护、起播复位、尾长模式。它们不可自动化，但引擎要读。
//
// 线程：消息线程在每块开头读一遍宿主参数，填成一份快照交给引擎；引擎在音频线程只读这份快照，
// 不持有宿主的原子指针（宿主怎么切块因此不会影响结论）。
#pragma once

#include <cstdint>

#include "engine/AudioView.h"

namespace rinne::engine
{

// 延迟的时间基准：按毫秒还是按节拍。数值只在本结构内部有意义，不进工程 state。
enum class DelayMode : std::uint8_t
{
    kTime = 0,
    kSync = 1,
};

// 尾长模式：宿主静音时插件还挂多久，防止宿主把环路挂起。
//
// 数值与工程 state 里的同名枚举、与外壳里的 rinne::TailMode 逐个相同（state 是线格式，只能追加；
// 外壳接线时用 static_assert 对齐）。core 里的这一份是准绳，两边不许再各写一份。
enum class TailMode : std::uint8_t
{
    kAuto = 0, // VST3 报无限长，AU 报 30 秒
    kInfinite = 1,
    kThirtySeconds = 2,
};

// 时值的档数与拍数。顺序冻结：只能追加新档，不能改已有档位的含义或次序。
// 拍数以 1/4 拍为 1。三连 = 直值 × 2/3，附点 = 直值 × 3/2。
inline constexpr int kDelayNoteCount = 16;
inline constexpr double kDelayNoteBeats[kDelayNoteCount] = {
    1.0 / 16.0, // 0  1/64
    1.0 / 12.0, // 1  1/32T
    1.0 / 8.0,  // 2  1/32
    1.0 / 6.0,  // 3  1/16T
    1.0 / 4.0,  // 4  1/16
    1.0 / 3.0,  // 5  1/8T
    3.0 / 8.0,  // 6  1/16D
    1.0 / 2.0,  // 7  1/8
    2.0 / 3.0,  // 8  1/4T
    3.0 / 4.0,  // 9  1/8D
    1.0,        // 10 1/4（默认）
    4.0 / 3.0,  // 11 1/2T
    3.0 / 2.0,  // 12 1/4D
    2.0,        // 13 1/2
    3.0,        // 14 1/2D
    4.0,        // 15 1/1
};
inline constexpr int kDefaultDelayNote = 10; // 1/4

// 参数与设置的默认值。默认值本身就是契约：反馈为 0、限制器开着、上限 -6 dBFS、失控保护开着、
// 起播复位开着、尾长自动、混合 25 %。
inline constexpr float kDefaultFeedbackPercent = 0.0f;
inline constexpr float kDefaultMixPercent = 25.0f;
inline constexpr float kDefaultDelayMs = 100.0f;
inline constexpr float kDefaultLowCutHz = 20.0f;   // 20 = 关
inline constexpr float kDefaultHighCutHz = 20000.0f; // 20000 = 关
inline constexpr float kDefaultCeilingDb = -6.0f;
inline constexpr float kMinFeedbackPercent = 0.0f;
inline constexpr float kMaxFeedbackPercent = 150.0f;
inline constexpr float kMinMixPercent = 0.0f;
inline constexpr float kMaxMixPercent = 100.0f;
inline constexpr float kMinDelayMs = 1.0f;
inline constexpr float kMaxDelayMs = 4000.0f;
inline constexpr float kMinLowCutHz = 20.0f;
inline constexpr float kMaxLowCutHz = 2000.0f;
inline constexpr float kMinHighCutHz = 1000.0f;
inline constexpr float kMaxHighCutHz = 20000.0f;
inline constexpr float kMinCeilingDb = -24.0f;
inline constexpr float kMaxCeilingDb = 0.0f;

// 引擎参数的一份快照。壳在块首填好它，引擎在音频线程只读。
struct EngineParams
{
    // ---- 11 个参数 ----
    float feedbackPercent = kDefaultFeedbackPercent; // 0..150
    bool loopOn = true;
    float mixPercent = kDefaultMixPercent; // 0..100
    DelayMode delayMode = DelayMode::kTime;
    float delayMs = kDefaultDelayMs;         // 1..4000
    std::uint8_t delayNote = kDefaultDelayNote; // 0..15
    float lowCutHz = kDefaultLowCutHz;         // 20 = 关
    float highCutHz = kDefaultHighCutHz;       // 20000 = 关
    bool polarityInvert = false;
    bool limiterOn = true;
    float ceilingDb = kDefaultCeilingDb; // -24..0

    // ---- 命令序号：变了就执行一次 ----
    std::uint32_t presetSeq = 0; // 载入工厂预设
    std::uint32_t stateSeq = 0;  // 宿主设置工程状态（宿主预设、A/B、撤销）
    std::uint32_t panicSeq = 0;  // 紧急切断
    std::uint32_t recoverSeq = 0; // 恢复回送

    // ---- 工程设置里的三个开关 ----
    bool guardOn = true;     // 失控保护
    bool resetOnPlay = true; // 起播复位
    TailMode tailMode = TailMode::kAuto;
};

// 命令序号的基线：新引擎按当前序号建基线，之后的变化才执行，重建后不重放旧命令。
// 基线不能只靠 EngineParams 每块传递：构造引擎时还没有第一块，首块之前的命令必须照常生效
// （紧急切断尤其不能被吞掉）。
struct CommandBaseline
{
    std::uint32_t presetSeq = 0;
    std::uint32_t stateSeq = 0;
    std::uint32_t panicSeq = 0;
    std::uint32_t recoverSeq = 0;
};

} // namespace rinne::engine
