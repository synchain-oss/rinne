// SPDX-License-Identifier: GPL-3.0-or-later
//
// 会合测试共用的小工具：一个把每次调用都记下来的后端包装（进程内后端是 final，只能包不能继承），以及直接建段、
// 初始化 rv / 数据段、按绝对偏移读写端点槽 version 的辅助函数。包装还允许按需强制返回 kFailedDenied，
// 用来构造「后端拒绝之后不再重试」。所有时间仍由用例自己给，这个文件不读时钟。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "ipc/LoopRing.h"
#include "ipc/Rendezvous.h"
#include "ipc/SegmentBackendInProcess.h"

namespace rinne::test
{

using namespace rinne::ipc;

// 一定已经不存在的 pid：POSIX 上超出 pid_t 的正数范围，Windows 上没有这么大的进程号
inline constexpr u32 kDeadPid = 0xfffffff0u;

// 转发给进程内后端并计数。forced 不是 kOk 时直接返回它（视图保持原样），用来模拟宿主拒绝
class CountingBackend final : public ISegmentBackend
{
public:
    ~CountingBackend() override { forgetRetired(); }

    InitResult createOrOpen(const std::string& name, std::size_t size, SegmentView& view) override
    {
        ++createOrOpenCalls;
        if (forced != InitResult::kOk)
        {
            return forced;
        }
        if (forcedSize != 0 && size == forcedSize)
        {
            return forcedForSize; // 只让某一种段打不开（例如数据段），会合段照常
        }
        return inner_.createOrOpen(name, size, view);
    }

    InitResult openExisting(const std::string& name, SegmentView& view) override
    {
        ++openExistingCalls;
        if (forced != InitResult::kOk)
        {
            return forced;
        }
        return inner_.openExisting(name, view);
    }

    InitResult openExistingReadOnly(const std::string& name, SegmentView& view) override
    {
        ++openExistingReadOnlyCalls;
        if (forced != InitResult::kOk)
        {
            return forced;
        }
        return inner_.openExistingReadOnly(name, view);
    }

    void unmap(SegmentView& view) noexcept override
    {
        ++unmapCalls;
        inner_.unmap(view);
    }

    OrphanUnmap orphanUnmapper() const noexcept override
    {
        ++orphanUnmapperCalls;
        return inner_.orphanUnmapper();
    }

    void tryLock(const SegmentView& view) override
    {
        ++tryLockCalls;
        inner_.tryLock(view);
    }

    void resetCounts() noexcept
    {
        createOrOpenCalls = 0;
        openExistingCalls = 0;
        openExistingReadOnlyCalls = 0;
        unmapCalls = 0;
        tryLockCalls = 0;
        orphanUnmapperCalls = 0;
    }

    InitResult forced = InitResult::kOk;
    std::size_t forcedSize = 0;
    InitResult forcedForSize = InitResult::kOk;
    std::size_t createOrOpenCalls = 0;
    std::size_t openExistingCalls = 0;
    std::size_t openExistingReadOnlyCalls = 0;
    std::size_t unmapCalls = 0;
    std::size_t tryLockCalls = 0;
    mutable std::size_t orphanUnmapperCalls = 0;

private:
    SegmentBackendInProcess inner_;
};

inline std::string segmentName(const Naming& naming, SegmentId id)
{
    std::string name;
    if (naming.name(id, name) != NameStatus::kOk)
    {
        return {};
    }
    return name;
}

// 只建（或打开）一个段，不初始化：给需要自己摆弄段头的用例
inline SegmentView createView(ISegmentBackend& backend, const Naming& naming, SegmentId id, std::size_t bytes)
{
    SegmentView view;
    REQUIRE(backend.createOrOpen(segmentName(naming, id), bytes, view) == InitResult::kOk);
    return view;
}

// 把 rv 初始化成属于自己的 abi 的已完成段（几何字段一起写）
inline void publishRv(ISegmentBackend& backend, SegmentView& view, u32 abi = kIpcAbi)
{
    auto* rv = static_cast<RvSegment*>(view.base);
    const SegmentHeaderSpec spec{kRvMagic, abi, kRvBytes, kRvHeaderBytes};
    REQUIRE(backend.initHeader(view, headerFields(rv->header), spec, [rv] {
        rv->header.layout_bytes.store(kRvBytes, std::memory_order_relaxed);
        rv->header.groups.store(kGroupCount, std::memory_order_relaxed);
        rv->header.group_stride.store(kGroupStride, std::memory_order_relaxed);
    }) == InitResult::kOk);
}

inline SegmentView openRv(ISegmentBackend& backend, const Naming& naming, u32 abi = kIpcAbi)
{
    SegmentView view = createView(backend, naming, SegmentId::rendezvous(), kRvBytes);
    publishRv(backend, view, abi);
    return view;
}

inline SegmentView openRing(ISegmentBackend& backend, const Naming& naming, u32 group)
{
    SegmentView view = createView(backend, naming, SegmentId::groupData(group), kRingBytes);
    auto* ring = static_cast<RingSegment*>(view.base);
    const SegmentHeaderSpec spec{kRingMagic, kIpcAbi, kRingBytes, 16};
    REQUIRE(backend.initHeader(view, headerFields(ring->header), spec, [ring] { LoopRing::initGeometry(*ring); }) ==
            InitResult::kOk);
    return view;
}

inline RvSegment& rvOf(SegmentView& view) noexcept
{
    return *static_cast<RvSegment*>(view.base);
}

inline const RvSegment& rvOf(const SegmentView& view) noexcept
{
    return *static_cast<const RvSegment*>(view.base);
}

inline RingSegment& ringOf(SegmentView& view) noexcept
{
    return *static_cast<RingSegment*>(view.base);
}

// 按绝对偏移读写对端槽的 version（只读方也这么读）
inline void storeSlotVersion(SegmentView& view, u32 group, bool output, u32 version)
{
    auto* bytes = static_cast<unsigned char*>(view.base);
    reinterpret_cast<std::atomic<u32>*>(bytes + slotVersionOffset(group, output))
        ->store(version, std::memory_order_relaxed);
}

inline u32 loadSlotVersion(const SegmentView& view, u32 group, bool output)
{
    const auto* bytes = static_cast<const unsigned char*>(view.base);
    return reinterpret_cast<const std::atomic<u32>*>(bytes + slotVersionOffset(group, output))
        ->load(std::memory_order_relaxed);
}

inline RendezvousConfig configFor(Role role, ISegmentBackend& backend, const NameScope& scope, u64 seed = 1)
{
    RendezvousConfig config;
    config.role = role;
    config.backend = &backend;
    config.scope = scope;
    config.version = packVersion(0, 9, 0);
    config.format = kFormatVst3;
    config.hostPpid = 4242;
    config.seed = seed;
    return config;
}

} // namespace rinne::test
