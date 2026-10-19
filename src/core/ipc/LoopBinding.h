// SPDX-License-Identifier: GPL-3.0-or-later
//
// 音频线程怎么拿到段：一个不可变、只在消息线程构造一次的绑定快照（LoopBinding），加上块首/块末的按块租约
// （BoundBlock）。音频线程不查名字、不开段、不加锁：它只从 BindingPublisher 原子地读一个指针，用它拿一次租约，
// 之后整块都在映射上做原子读写。
//
// 寿命链条（每一环都留到实例析构，谁也不提前释放）：
//   - 段句柄由 HandleKeeper 独占（adopt 之后地址稳定），只在加载、改组时新建，绝不按拍新建；
//   - 绑定快照由 BindingPublisher 拥有，每次 publish 留一份旧的不动，音频线程可能还拿着它；
//   - 不再用的句柄先 retire，等 kRetireDelayMs（盖住音频线程 5 ms 的切换淡出，留足余量）再调 SegmentHandle::release，
//     而 release 之后映射表自己还要等 500 ms 宽限期才解映射。任何一环都不靠「大概已经不用了」的时间假设。
// 换写方、换组、重载工程都只换快照与句柄，不碰音频线程手里的那一份；旧的那一份到实例析构才真正消失。
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "ipc/ISegmentBackend.h"
#include "ipc/SegmentLayout.h"

namespace rinne::ipc
{

enum class Role : std::uint8_t
{
    kInput,
    kOutput,
};

constexpr Role peerOf(Role r) noexcept
{
    return r == Role::kInput ? Role::kOutput : Role::kInput;
}

// 组 G（1..8）的 Input 槽在组块 +0、Output 槽在 +256；组号是前提条件，越界不检查
RoleSlot& roleSlot(RvSegment& rv, u32 group, Role role) noexcept;
const RoleSlot& roleSlot(const RvSegment& rv, u32 group, Role role) noexcept;

// 音频线程用的绑定快照：构造之后一个字节都不再改，以 release 发布，音频线程以 acquire 读。
// 只借句柄（句柄由同一个实例的 HandleKeeper 拥有），自己不持有任何映射。
struct LoopBinding
{
    u64 seq = 0; // 发布序号，从 1 起，只供诊断与测试
    SegmentHandle* rv = nullptr;
    SegmentHandle* ring = nullptr;
    u32 group = 0;
    Role role = Role::kInput;
    u64 myCtl = 0; // 属主最后写下的 ctl 整字：整字变了就说明被人接管过
    u64 writerToken = 0;
};

// 音频线程每块一个：块首构造（拿两块段的租约），块末析构（归还）。不分配、不加锁、不抛异常。
// 绑定为空、组号越界、租约拿不到、段比结构体小，都只是 ok() 为假，调用方按「没有对端」出干声。
class BoundBlock
{
public:
    explicit BoundBlock(const LoopBinding* b) noexcept;
    ~BoundBlock() = default;

    bool ok() const noexcept { return ok_; }
    // 下面这些访问只许在 ok() 为真时调用：否则绑定或映射可能为空
    const LoopBinding& binding() const noexcept { return *b_; }
    RvSegment& rv() const noexcept;
    RoleSlot& mine() const noexcept;
    RoleSlot& peer() const noexcept;
    RingSegment& ring() const noexcept;
    // 这一块里属主身份有没有被别人动过：每块比对一次整字，杜绝双属主与 ABA（见 SegmentLayout 的 ctl 注释）
    bool stillMine() const noexcept;

    BoundBlock(const BoundBlock&) = delete;
    BoundBlock& operator=(const BoundBlock&) = delete;
    BoundBlock(BoundBlock&&) = delete;
    BoundBlock& operator=(BoundBlock&&) = delete;

private:
    const LoopBinding* b_;
    SegmentHandle::Lease rv_;
    SegmentHandle::Lease ring_;
    bool ok_;
};

// 消息线程发布、音频线程读取。发布要分配，只许非实时线程调用；音频线程只经 current() 拿指针。
// 旧快照一份都不释放（音频线程可能正拿着它），到析构才一起消失：实例析构时已经没有任何音频回调会进来。
class BindingPublisher
{
public:
    // 拷贝一份、编上新的序号（1、2、……）并留下，然后以 release 发布。返回留在表里的那一份
    const LoopBinding* publish(const LoopBinding& b);
    void clear() noexcept { current_.store(nullptr, std::memory_order_release); }
    const LoopBinding* current() const noexcept { return current_.load(std::memory_order_acquire); }
    // 诊断用：留着的快照个数
    std::size_t retained() const noexcept { return keep_.size(); }

private:
    static_assert(std::atomic<const LoopBinding*>::is_always_lock_free, "音频线程每块都读绑定快照");
    std::atomic<const LoopBinding*> current_{nullptr};
    std::vector<std::unique_ptr<LoopBinding>> keep_;
    u64 nextSeq_ = 1;
};

// 消息线程独占的段句柄表：adopt 之后句柄地址稳定，音频线程手里的裸指针一直有效。
// retire 只是打上时间戳，到点才真正释放；已释放的空壳留到析构（映射表自己的控制块也一样留到进程退出）。
class HandleKeeper
{
public:
    // 覆盖音频线程 5 ms 的切换淡出还留足余量；release() 之后映射表自己还要等 500 ms 宽限期
    static constexpr u64 kRetireDelayMs = 250;

    // 接管一个句柄：存成 unique_ptr，返回它稳定的地址。句柄只在这里进表，音频线程只读它的地址
    SegmentHandle* adopt(SegmentHandle&& h);
    // 标记一个句柄从 nowMs 起退休；不认识的指针、已经退休过的指针都是空操作（幂等）
    void retire(SegmentHandle* h, u64 nowMs) noexcept;
    // 推进退休时间：过了 kRetireDelayMs 就调 release(nowMs)，返回 false 时下一拍接着调，直到解映射为止。
    // 时间读出 0 就不推进；退休时的时间是 0 或时间倒退，都从这一拍重新计，绝不因为无符号相减回绕而提前释放
    void tick(u64 nowMs) noexcept;
    // 放下全部句柄（活的、退休中的、已释放的空壳）：宽限期没走完的映射交给进程级的回收。只在不会再有音频回调
    // 进来时调用（实例析构），绑定快照要先撤下
    void clear() noexcept;
    std::size_t liveCount() const noexcept;
    std::size_t retiringCount() const noexcept;

private:
    struct Entry
    {
        std::unique_ptr<SegmentHandle> handle;
        bool retiring = false;
        bool released = false;
        u64 retiredAtMs = 0;
    };

    std::vector<Entry> entries_;
};

} // namespace rinne::ipc
