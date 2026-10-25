// SPDX-License-Identifier: GPL-3.0-or-later
//
// rig 用的段夹具：建好并初始化会合段与数据段，给出裸指针（模拟音频线程手里的租约）。
//
// 走真实共享内存后端：Windows 上是 Local\ 命名段，mac 上是 POSIX 段加锁文件。Linux 上没有真后端，
// 退回进程内后端（同名段在同一进程内共享，跨进程不可见）——环路的语义在单进程内照样成立，
// 跨进程那一半由 ipc 套件的用例覆盖。
//
// 作用域随机：每个 rig 一份自己的段名，两个 rig 互不撞；发布版不读环境变量，作用域只经 API 注入。
//
// 线程：只在非实时线程用（建段要分配、要碰系统调用）。音频线程只拿这里给出的裸指针。

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ipc/ISegmentBackend.h"
#include "ipc/LoopBinding.h"
#include "ipc/LoopRing.h"
#include "ipc/Naming.h"
#include "ipc/SegmentLayout.h"

#if defined(_WIN32)
#include "ipc/SegmentBackendWin32.h"
#elif defined(__APPLE__)
#include "ipc/SegmentBackendPosix.h"
#else
#include "ipc/SegmentBackendInProcess.h"
#endif

namespace rinne::rig
{

using namespace rinne::ipc;

// 会合段与数据段的段头规格：段头之后清零（数据段从 16 字节起，那 16 字节是永久冻结的段头）。
inline constexpr SegmentHeaderSpec kRvSpec{kRvMagic, kIpcAbi, kRvBytes, kRvHeaderBytes};
inline constexpr SegmentHeaderSpec kRingSpec{kRingMagic, kIpcAbi, kRingBytes, 16};

// 真后端不可用时的说明（Linux 的 core-only 构建）。场景用它决定跳过还是失败。
inline const char* backendNote() noexcept
{
#if defined(_WIN32) || defined(__APPLE__)
    return "真实共享内存后端";
#else
    return "进程内段后端（这个平台没有真共享内存）";
#endif
}

// 一组段：会合段 + 若干组的数据段。
//
// 建段是「建或开」：同名段已存在就附着上去（不重新初始化）。rig 每次用随机的测试作用域，
// 所以段名一定是新的，不存在撞上别人的情况。
class Segments
{
public:
    Segments() : scope_(NameScope::randomForTesting()), naming_(scope_)
    {
#if defined(_WIN32)
        backend_ = std::make_unique<SegmentBackendWin32>();
#elif defined(__APPLE__)
        backend_ = std::make_unique<SegmentBackendPosix>();
#else
        backend_ = std::make_unique<SegmentBackendInProcess>();
#endif
    }

    // 解映射全部段：rig 用完不留残段（真实后端在最后一个映射解除时回收段）。
    ~Segments()
    {
        for (SegmentView& view : views_)
        {
            backend_->unmap(view);
        }
    }
    Segments(const Segments&) = delete;
    Segments& operator=(const Segments&) = delete;

    // 建好一组段并初始化。group 是组号（1..8）。返回 false 表示建段失败（宿主沙盒一类）。
    bool open(std::uint32_t group)
    {
        if (group == 0 || group > kGroupCount)
        {
            return false;
        }
        if (!openOne(SegmentId::rendezvous(), kRvSpec))
        {
            return false;
        }
        if (!openOne(SegmentId::groupData(group), kRingSpec))
        {
            return false;
        }
        return true;
    }

    // 会合段与数据段的裸指针（模拟音频线程手里的租约）。没有建过就是空。
    RvSegment* rv() const noexcept { return rv_; }
    RingSegment* ring(std::uint32_t group) const noexcept
    {
        return group >= 1 && group <= kGroupCount ? rings_[group] : nullptr;
    }

    const Naming& naming() const noexcept { return naming_; }
    const NameScope& scope() const noexcept { return scope_; }

private:
    // 建一段。会合段与数据段的头不同，所以分开写；返回值表示建段成功与否。
    bool openOne(SegmentId id, const SegmentHeaderSpec& spec)
    {
        std::string name;
        if (naming_.name(id, name) != NameStatus::kOk)
        {
            return false;
        }
        SegmentView view;
        if (backend_->createOrOpen(name, spec.layoutBytes, view) != InitResult::kOk)
        {
            return false;
        }
        if (id.isRendezvous())
        {
            auto* rv = static_cast<RvSegment*>(view.base);
            if (backend_->initHeader(view, headerFields(rv->header), spec, [&] {
                    rv->header.layout_bytes.store(kRvBytes, std::memory_order_relaxed);
                    rv->header.groups.store(kGroupCount, std::memory_order_relaxed);
                    rv->header.group_stride.store(kGroupStride, std::memory_order_relaxed);
                }) != InitResult::kOk)
            {
                backend_->unmap(view);
                return false;
            }
            rv_ = rv;
            views_.push_back(std::move(view));
            return true;
        }

        auto* ring = static_cast<RingSegment*>(view.base);
        if (backend_->initHeader(view, headerFields(ring->header), spec, [&] { LoopRing::initGeometry(*ring); }) !=
            InitResult::kOk)
        {
            backend_->unmap(view);
            return false;
        }
        rings_[id.group] = ring;
        views_.push_back(std::move(view));
        return true;
    }

    NameScope scope_;
    Naming naming_;
    std::unique_ptr<ISegmentBackend> backend_;
    std::vector<SegmentView> views_;
    RvSegment* rv_ = nullptr;
    RingSegment* rings_[kGroupCount + 1]{};
};

} // namespace rinne::rig
