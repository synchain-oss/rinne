// SPDX-License-Identifier: GPL-3.0-or-later
//
// 诊断事件队列：音频线程往里推事件码，消息线程排空。定长、单生产者单消费者、无锁、不分配。
//
// 事件码按模块分段，各模块在自己的头文件里定义，不建全局枚举：
//   0x0200–0x02FF  安全链独占（src/core/engine/SafetyEvents.h）
//   0x0300–0x03FF  环路层独占（src/core/loop/LoopEvents.h）
//   0x0400–0x04FF  引擎独占（src/core/engine/EngineEvents.h）
//   0x0500–0x05FF  探针独占（src/core/diag/ProbeEvents.h）
// 四段是跨卡的数值契约：段归属定死，之后只能追加，不能把别的段的码挪进这一段。
// 引擎层与环路层定义的码一律落在 0x0300 与 0x04FF 两段里，绝不碰 0x0200–0x02FF。
//
// 实时约定：push 不加锁、不分配、不读时钟、不抛异常；队列满了就丢掉这一条并计数，绝不阻塞音频线程，
// 也绝不覆盖还没被读走的那一条（覆盖会让读者读到写了一半的条目）。消息线程每拍排空一次，
// 正常情况下不会满。
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace rinne::diag
{

// 一条事件：码 + 两个小参数 + 一个样本计数。样本计数让界面能把事件排在时间线上。
struct Event
{
    std::uint16_t code = 0;
    std::uint8_t a = 0;
    std::uint8_t b = 0;
    std::int64_t sample = 0;
};

// 定长的单生产者单消费者队列。
//
// 存储是「保留一格」的环形数组：生产者只写 head_，读者只写 tail_，两者各自单调递增，下标取模。
// 判满的条件是 head_ - tail_ == Capacity - 1（保留一格，所以能装 Capacity - 1 条）。
// 生产者写的格与读者读的格不重叠（判满就是为此），格与格之间用 release/acquire 配对，
// 读者拿到的一定是完整的一条。
template<int Capacity>
class EventQueue
{
    static_assert(Capacity >= 2, "至少要能存一条");

public:
    // 音频线程调用：满了就丢掉这一条并计数，不阻塞、不覆盖
    void push(const Event& event) noexcept
    {
        const std::uint64_t head = head_.load(std::memory_order_relaxed);
        const std::uint64_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= static_cast<std::uint64_t>(Capacity - 1))
        {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        entries_[static_cast<std::size_t>(head % static_cast<std::uint64_t>(Capacity))] = event;
        head_.store(head + 1, std::memory_order_release);
    }

    // 消息线程调用：把当前的全部事件交给 sink，然后清空队列（音频线程接着推）。
    // sink 收到的事件按推入顺序。返回值是这次交出去的条数。
    template<typename Sink>
    int drain(Sink&& sink) noexcept
    {
        std::uint64_t tail = tail_.load(std::memory_order_relaxed);
        const std::uint64_t head = head_.load(std::memory_order_acquire);
        int count = 0;
        while (tail != head)
        {
            sink(entries_[static_cast<std::size_t>(tail % static_cast<std::uint64_t>(Capacity))]);
            ++tail;
            ++count;
        }
        tail_.store(tail, std::memory_order_release);
        return count;
    }

    // 诊断用：队列里现在有几条、累计丢了几条
    std::size_t size() const noexcept
    {
        const std::uint64_t head = head_.load(std::memory_order_acquire);
        const std::uint64_t tail = tail_.load(std::memory_order_acquire);
        return static_cast<std::size_t>(head - tail);
    }

    std::uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }

    // 清空（重新绑定或实例析构时，由消费者一侧调用）
    void clear() noexcept
    {
        tail_.store(head_.load(std::memory_order_acquire), std::memory_order_release);
    }

private:
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "音频线程每块都碰这两个原子量");

    std::array<Event, static_cast<std::size_t>(Capacity)> entries_{};
    std::atomic<std::uint64_t> head_{0};
    std::atomic<std::uint64_t> tail_{0};
    std::atomic<std::uint64_t> dropped_{0};
};

} // namespace rinne::diag
