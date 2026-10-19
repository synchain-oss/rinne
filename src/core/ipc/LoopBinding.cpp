// SPDX-License-Identifier: GPL-3.0-or-later
//
// 会合槽的定位、块首租约与绑定/句柄的生命周期。热路径（BoundBlock 的构造与析构、stillMine）只做原子的
// acquire 读、两次租约增减与指针运算：不分配、不加锁、不抛异常、不读时钟。分配与时钟都在消息线程一侧
// （BindingPublisher::publish、HandleKeeper::tick），时间由调用方注入。
#include "ipc/LoopBinding.h"

namespace rinne::ipc
{

RoleSlot& roleSlot(RvSegment& rv, u32 group, Role role) noexcept
{
    GroupBlock& block = rv.groups[group - 1];
    return role == Role::kInput ? block.input : block.output;
}

const RoleSlot& roleSlot(const RvSegment& rv, u32 group, Role role) noexcept
{
    const GroupBlock& block = rv.groups[group - 1];
    return role == Role::kInput ? block.input : block.output;
}

BoundBlock::BoundBlock(const LoopBinding* b) noexcept : b_(b), ok_(false)
{
    if (b == nullptr || b->rv == nullptr || b->ring == nullptr || b->group == 0 || b->group > kGroupCount)
    {
        return;
    }
    // 两块段各拿一次租约：租约在途期间映射绝不会被解映射，块内所有原子读写都落在活着的内存上
    rv_ = b->rv->lease();
    ring_ = b->ring->lease();
    // 段比结构体小说明句柄指的不是本 abi 的段（或是被截断的映射）：宁可当没有对端，也不越界读写
    if (!rv_ || !ring_ || rv_.size() < sizeof(RvSegment) || ring_.size() < sizeof(RingSegment))
    {
        return;
    }
    ok_ = true;
}

RvSegment& BoundBlock::rv() const noexcept
{
    return *static_cast<RvSegment*>(rv_.base());
}

RoleSlot& BoundBlock::mine() const noexcept
{
    return roleSlot(rv(), b_->group, b_->role);
}

RoleSlot& BoundBlock::peer() const noexcept
{
    return roleSlot(rv(), b_->group, peerOf(b_->role));
}

RingSegment& BoundBlock::ring() const noexcept
{
    return *static_cast<RingSegment*>(ring_.base());
}

bool BoundBlock::stillMine() const noexcept
{
    return mine().ctl.load(std::memory_order_acquire) == b_->myCtl;
}

const LoopBinding* BindingPublisher::publish(const LoopBinding& b)
{
    auto owned = std::make_unique<LoopBinding>(b);
    owned->seq = nextSeq_++;
    // 先留一份再发布：指针一旦以 release 出去，音频线程随时可能拿它，它必须已经归 keep_ 所有
    const LoopBinding* published = owned.get();
    keep_.push_back(std::move(owned));
    current_.store(published, std::memory_order_release);
    return published;
}

SegmentHandle* HandleKeeper::adopt(SegmentHandle&& h)
{
    Entry entry;
    entry.handle = std::make_unique<SegmentHandle>(std::move(h));
    SegmentHandle* const stable = entry.handle.get();
    // vector 扩容只搬 Entry，SegmentHandle 本体留在堆上，地址因此不变；音频线程手里的裸指针一直有效
    entries_.push_back(std::move(entry));
    return stable;
}

void HandleKeeper::retire(SegmentHandle* h, u64 nowMs) noexcept
{
    if (h == nullptr)
    {
        return;
    }
    for (Entry& e : entries_)
    {
        if (e.handle.get() == h)
        {
            if (!e.retiring)
            {
                e.retiring = true;
                e.retiredAtMs = nowMs;
            }
            return;
        }
    }
}

void HandleKeeper::tick(u64 nowMs) noexcept
{
    for (Entry& e : entries_)
    {
        if (!e.retiring || e.released)
        {
            continue;
        }
        if (nowMs == 0)
        {
            continue; // 读不出时钟：这一拍什么都不动
        }
        if (e.retiredAtMs == 0 || nowMs < e.retiredAtMs)
        {
            // 退休时读不出时钟、或者时钟倒退：从现在重新计，绝不因为无符号相减回绕而提前释放
            e.retiredAtMs = nowMs;
            continue;
        }
        if (nowMs - e.retiredAtMs < kRetireDelayMs)
        {
            continue;
        }
        // 到点了：release 在租约在途或宽限期没满时返回 false，之后每拍接着调，直到真的解映射
        if (e.handle->release(nowMs))
        {
            e.released = true;
        }
    }
}

void HandleKeeper::clear() noexcept
{
    // SegmentHandle 的析构停发租约、把没走完宽限期的映射登记给进程级的回收，不在这里等
    entries_.clear();
}

std::size_t HandleKeeper::liveCount() const noexcept
{
    std::size_t n = 0;
    for (const Entry& e : entries_)
    {
        if (!e.retiring)
        {
            ++n;
        }
    }
    return n;
}

std::size_t HandleKeeper::retiringCount() const noexcept
{
    std::size_t n = 0;
    for (const Entry& e : entries_)
    {
        if (e.retiring)
        {
            ++n;
        }
    }
    return n;
}

} // namespace rinne::ipc
