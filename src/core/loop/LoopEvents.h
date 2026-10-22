// SPDX-License-Identifier: GPL-3.0-or-later
//
// 环路层的事件码与事件出口。
//
// 事件码按模块分段，各模块在自己的头文件里定义，不建全局枚举：
//   0x0200–0x02FF  安全链独占（src/core/engine/SafetyEvents.h）
//   0x0300–0x03FF  环路层独占（本文件）
//   0x0400–0x04FF  引擎独占（src/core/engine/EngineEvents.h）
//   0x0500–0x05FF  探针独占（src/core/diag/ProbeEvents.h）
// 本段内的新码只能追加；已有的值不改不删（诊断日志与桥都按数值存）。
#pragma once

#include <cstdint>

namespace rinne::loop
{

// 重同步的原因：读头为什么重建。界面按这个给不同的提示。
enum class ResyncReason : std::uint8_t
{
    kBindSwitch = 0, // 换了绑定（换组、换对端）
    kWriter = 1,      // 换了写方
    kFormat = 2,      // 写方改了采样率（epoch 变了）
    kPlay = 3,        // 起播沿
    kStallResume = 4, // 断档后写头恢复
    kJump = 5,        // 漂移跳变（超过 N_cap）
    kShift = 6,       // 漂移持续同一个非零值
    kOffset = 7,      // 漂移被吸收（只记诊断）
    kDChange = 8,     // 改 D 引起的交叉淡化
};

// 事件码。a 放原因或计数，b 放次级信息。
enum class LoopEventCode : std::uint16_t
{
    kResync = 0x0300, // a = ResyncReason，b = 0
    kStalled = 0x0301, // a = 1，b = 0
    kStallCleared = 0x0302, // a = 1
    kTolerantMode = 0x0303, // a = 1（转入容忍模式：10 s 内 ≥3 次重同步）
    kUnstable = 0x0304, // a = 1（时序不稳定）
    kDMinRaised = 0x0305, // a = 新的 D_min（样本），b = 原因
    kHostViolation = 0x0306, // a = 宿主送来的块长 n，b = 声明值 N
    kUnsupportedBlock = 0x0307, // a = 块长 n，b = 支持的上限
    kLineCleared = 0x0308, // a = 1（本地延迟线被清空）
    kOverflow = 0x0309, // a = 被环覆盖而丢掉的帧数
    kTorn = 0x030A,    // a = 撕裂的帧数
    kOverrun = 0x030B, // a = 补 0 的样本数
    kTapDropped = 0x030C, // a = 1（读头槽满，丢掉了一个）
};

// 事件出口：环路层不直接依赖诊断队列，只经这一个函数指针往外出码。
// 出口可以是空（不装就什么都不发生），因此事件从来不是音频决策的依据。
//
// 实时约定：emit 只做一次函数调用与几个整数的拷贝，不加锁、不分配、不读时钟、不抛异常。
class LoopEventSink
{
public:
    using Fn = void (*)(void* context, std::uint16_t code, std::uint8_t a, std::uint8_t b,
                        std::int64_t sample) noexcept;

    constexpr LoopEventSink() noexcept = default;
    constexpr LoopEventSink(Fn fn, void* context) noexcept : fn_(fn), context_(context) {}

    void emit(std::uint16_t code, std::uint8_t a = 0, std::uint8_t b = 0, std::int64_t sample = 0) const noexcept
    {
        if (fn_ != nullptr)
        {
            fn_(context_, code, a, b, sample);
        }
    }

    void emit(LoopEventCode code, std::uint8_t a = 0, std::uint8_t b = 0, std::int64_t sample = 0) const noexcept
    {
        emit(static_cast<std::uint16_t>(code), a, b, sample);
    }

    bool enabled() const noexcept { return fn_ != nullptr; }

private:
    Fn fn_ = nullptr;
    void* context_ = nullptr;
};

} // namespace rinne::loop
