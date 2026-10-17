// SPDX-License-Identifier: GPL-3.0-or-later
//
// 安全状态机对外的词汇：状态、跳闸原因、淡入原因，以及音频线程每块产生的安全事件。
// 界面、诊断与引擎都只认这里的枚举值；新值只能追加，已有的值不改不删（诊断日志与桥都按数值存）。
// 事件码占 0x0200–0x02FF 一段，别的模块的事件码各自在自己的头文件里分段，不建全局枚举。
#pragma once

#include <array>
#include <cstdint>

namespace rinne::engine
{

// 对外报告的安全状态。同时满足几种时按 Tripped > Unpaired > Stalled > FadeIn > Guard > Run 取第一个
enum class SafetyState : std::uint8_t
{
    Unpaired = 0, // 没有对端或读不了环：回送 10 ms 淡到 0
    FadeIn = 1, // 淡入中（从运行中进入时先 10 ms 降到 0，再从首个有效读样本起 500 ms 上行）
    Run = 2,
    Stalled = 3, // 写头不动（断档）；淡出由环路层负责，这里只报告
    Guard = 4, // 失控保护正在压低回送
    Tripped = 5, // 跳闸：淡出、清空延迟线、锁存，只有恢复或重载工程能清
};

enum class TripReason : std::uint8_t
{
    None = 0,
    Panic = 1, // 紧急切断（界面动作）
    Nan = 2, // Input 内部出现非有限值
    Huge = 3, // 超大值：回送电平 ≥ +12 dBFS 持续 100 ms
    Runaway = 4, // 失控保护压到 −24 dB 后仍在失控
};

enum class FadeReason : std::uint8_t
{
    Load = 0, // 实例准备好之后的第一次淡入
    FirstPair = 1, // 失去配对之后重新配上
    NewWriter = 2, // 换了写方
    Preset = 3, // 载入预设
    SetState = 4, // 宿主设置 state（预设、A/B、撤销）
    Recover = 5, // 跳闸后点了恢复
};

enum class SafetyEventCode : std::uint16_t
{
    StateChanged = 0x0200, // a = SafetyState，b = 跳闸原因（跳闸时）或淡入原因（淡入时），否则 0
    Tripped = 0x0201, // a = TripReason
    FlushRequested = 0x0202, // 跳闸淡出完成，引擎应清空本地延迟线
    Recovered = 0x0203, // a = 原来的 TripReason
    RecoverIgnored = 0x0204, // a = 1 冷却未到、a = 2 并未跳闸
    GuardEngaged = 0x0205,
    GuardReleased = 0x0206,
    TapNonFinite = 0x0207, // 环里读到非有限值并已置 0（环内插件的 NaN 应已由 Output 清洗，这是第二道）
};

struct SafetyEvent
{
    SafetyEventCode code = SafetyEventCode::StateChanged;
    std::uint8_t a = 0;
    std::uint8_t b = 0;
    std::int64_t sample = 0; // 事件发生在 prepare 之后的第几个样本
};

// 一块之内的事件，定长；装不下时只数丢了几条。引擎每块处理完后读走，下一块开始时清空
class SafetyEventLog
{
public:
    static constexpr int kCapacity = 16;

    void clear() noexcept
    {
        count_ = 0;
        dropped_ = 0;
    }

    void push(SafetyEventCode code, std::uint8_t a, std::uint8_t b, std::int64_t sample) noexcept
    {
        if (count_ < kCapacity)
            events_[static_cast<std::size_t>(count_++)] = SafetyEvent{code, a, b, sample};
        else
            ++dropped_;
    }

    int size() const noexcept { return count_; }
    int dropped() const noexcept { return dropped_; }
    const SafetyEvent& operator[](int i) const noexcept { return events_[static_cast<std::size_t>(i)]; }

private:
    std::array<SafetyEvent, kCapacity> events_{};
    int count_ = 0;
    int dropped_ = 0;
};

} // namespace rinne::engine
