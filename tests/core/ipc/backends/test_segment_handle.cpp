// SPDX-License-Identifier: GPL-3.0-or-later
//
// 映射句柄的租约与释放：持租约时不解映射；释放请求之后不再发租约；租约归零后还要再等满宽限期才经后端解映射；
// 移动只转移所有权；析构或移动赋值覆盖时走不完宽限期，映射交给回收表，之后走完宽限期再解映射，租约照样有效；
// 后端先于句柄没了（句柄还活着、或已交给回收表）时用收尾函数解映射，不碰已经析构的后端；时钟读出 0 或倒退时
// 宽限期从头算；租约握手在「读到裸指针」与「加计数」之间被挂起多久都拿不到已解映射的视图。
// 测试后端解映射时真的释放页面（VirtualFree / munmap）：解映射之后的任何访问都是访问违例，不会悄悄落在还活着的堆上。
// 时间用参数注入，不睡眠。

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "ipc/ISegmentBackend.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace rinne::ipc
{

// lease() 的两步拆开调用：先读裸指针，之后再加计数、查释放请求
struct LeaseSeamForTesting
{
    static detail::SegmentMapping* readPointer(const SegmentHandle& h)
    {
        return h.implPtr_.load(std::memory_order_acquire);
    }
    static SegmentHandle::Lease finish(detail::SegmentMapping* p) { return SegmentHandle::finishLease(p); }
};

} // namespace rinne::ipc

namespace
{

using namespace rinne::ipc;

// 后端先没了之后、由映射表用收尾函数解映射的次数
std::atomic<int> gOrphanUnmaps{0};

void* allocatePages(std::size_t size)
{
#if defined(_WIN32)
    return ::VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* p = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
#endif
}

void freePages(SegmentView& view) noexcept
{
    if (view.base != nullptr)
    {
#if defined(_WIN32)
        ::VirtualFree(view.base, 0, MEM_RELEASE);
#else
        ::munmap(view.base, view.size);
#endif
    }
    view.reset();
}

// 每次建段都映射一块新的匿名页面，解映射时真的还给系统；数一数经后端与经收尾函数各解映射了几次
class PageBackend final : public ISegmentBackend
{
public:
    ~PageBackend() override { forgetRetired(); }

    InitResult createOrOpen(const std::string& name, std::size_t size, SegmentView& view) override
    {
        view.reset();
        void* base = allocatePages(size);
        if (base == nullptr)
        {
            return InitResult::kFailed;
        }
        view.base = base;
        view.size = size;
        view.created = true;
        view.name = name;
        return InitResult::kOk;
    }
    InitResult openExisting(const std::string& name, SegmentView& view) override
    {
        (void)name;
        view.reset();
        return InitResult::kFailed;
    }
    void unmap(SegmentView& view) noexcept override
    {
        ++unmaps;
        freePages(view);
    }
    OrphanUnmap orphanUnmapper() const noexcept override
    {
        return [](SegmentView& view) noexcept {
            gOrphanUnmaps.fetch_add(1);
            freePages(view);
        };
    }

    int unmaps = 0;
};

// 先把别的用例留在回收表里的映射回收掉，下面按差值断言
void drainRetired()
{
    SegmentHandle::reapRetired(100);
    SegmentHandle::reapRetired(100 + SegmentHandle::kReleaseGraceMs);
}

SegmentHandle openHandle(PageBackend& backend)
{
    SegmentView view;
    REQUIRE(backend.createOrOpen("pages", kRingBytes, view) == InitResult::kOk);
    return SegmentHandle(std::move(view), &backend);
}

std::atomic<float>* firstSample(void* base)
{
    return static_cast<std::atomic<float>*>(base) + kRingDataOffset / sizeof(float);
}

} // namespace

TEST_CASE("a held lease blocks unmapping and the grace period follows", "[ipc][backend][handle]")
{
    PageBackend backend;
    SegmentHandle handle = openHandle(backend);
    REQUIRE(handle.valid());
    void* const base = handle.base();
    CHECK(handle.size() == kRingBytes);

    {
        auto lease = handle.lease();
        REQUIRE(lease);
        CHECK(lease.base() == base);
        CHECK(lease.size() == kRingBytes);
        CHECK_FALSE(handle.release(1000)); // 租约在途
        CHECK_FALSE(handle.valid());
        CHECK_FALSE(handle.lease()); // 请求释放之后不再发新租约
        CHECK(backend.unmaps == 0);
        firstSample(lease.base())->store(1.0f); // 持租约期间映射仍然有效
    }
    CHECK_FALSE(handle.release(1000)); // 租约归零：宽限期从这里开始
    CHECK_FALSE(handle.release(1000 + SegmentHandle::kReleaseGraceMs - 1));
    CHECK(backend.unmaps == 0);
    CHECK(handle.release(1000 + SegmentHandle::kReleaseGraceMs));
    CHECK(backend.unmaps == 1);
    CHECK(handle.release(5000)); // 幂等
    CHECK(backend.unmaps == 1);
}

TEST_CASE("moving a handle transfers ownership without releasing", "[ipc][backend][handle]")
{
    PageBackend backend;
    SegmentHandle a = openHandle(backend);
    void* const base = a.base();
    SegmentHandle b(std::move(a));
    CHECK_FALSE(a.valid());
    CHECK_FALSE(a.lease());
    REQUIRE(b.valid());
    CHECK(b.base() == base);
    SegmentHandle c;
    c = std::move(b);
    CHECK_FALSE(b.valid());
    CHECK(c.lease().base() == base);
    CHECK(backend.unmaps == 0);
    CHECK_FALSE(c.release(1000));
    CHECK(c.release(1000 + SegmentHandle::kReleaseGraceMs));
    CHECK(backend.unmaps == 1);
}

TEST_CASE("a lease suspended between reading the pointer and counting never gets an unmapped view",
          "[ipc][backend][handle][handshake]")
{
    // 音频线程在 lease() 里读到裸指针之后被挂起；这期间非实时线程请求释放、等满宽限期、解映射（页面真的还给系统）。
    // 音频线程醒来加计数：必须看到释放请求、退还计数、拿到空租约。少了 lease() 里的那次检查，或者 release() 不置
    // 释放请求，这里就拿到一个指着已释放页面（或空指针）的租约
    PageBackend backend;
    SegmentHandle handle = openHandle(backend);
    detail::SegmentMapping* const p = LeaseSeamForTesting::readPointer(handle);
    REQUIRE(p != nullptr);

    CHECK_FALSE(handle.release(1000));
    CHECK(handle.release(1000 + SegmentHandle::kReleaseGraceMs));
    REQUIRE(backend.unmaps == 1);

    const SegmentHandle::Lease lease = LeaseSeamForTesting::finish(p);
    CHECK_FALSE(lease);
    CHECK(lease.base() == nullptr);
    CHECK(p->leaseCount.load() == 0u);
}

TEST_CASE("a lease that outlives its handle keeps the control block alive", "[ipc][backend][handle][lifetime]")
{
    // 插件卸载时音频线程还拿着租约：句柄析构走不完宽限期，控制块与映射都要留着，否则租约读 base、归还计数都落在
    // 已释放的内存上
    PageBackend backend;
    SegmentHandle::Lease lease;
    void* base = nullptr;
    {
        SegmentHandle handle = openHandle(backend);
        base = handle.base();
        lease = handle.lease();
        REQUIRE(lease);
    }
    CHECK(lease.base() == base);
    CHECK(lease.size() == kRingBytes);
    firstSample(lease.base())->store(1.0f);
    // 租约还在：回收表不解映射
    SegmentHandle::reapRetired(1000);
    SegmentHandle::reapRetired(1000 + SegmentHandle::kReleaseGraceMs * 10);
    CHECK(backend.unmaps == 0);
    lease = SegmentHandle::Lease{}; // 归还：落在仍然活着的控制块上
    // 归还之后宽限期才开始，走完就解映射
    SegmentHandle::reapRetired(20000);
    CHECK(backend.unmaps == 0);
    SegmentHandle::reapRetired(20000 + SegmentHandle::kReleaseGraceMs);
    CHECK(backend.unmaps == 1);
}

TEST_CASE("move-assigning over a live handle keeps its outstanding leases valid", "[ipc][backend][handle][lifetime]")
{
    PageBackend backend;
    SegmentHandle a = openHandle(backend);
    void* const baseA = a.base();
    auto leaseA = a.lease();
    REQUIRE(leaseA);
    SegmentHandle b = openHandle(backend);
    void* const baseB = b.base();
    a = std::move(b); // a 原来那一块还有租约在途
    CHECK(a.base() == baseB);
    CHECK_FALSE(b.valid());
    CHECK(leaseA.base() == baseA);
    firstSample(leaseA.base())->store(1.0f);
    leaseA = SegmentHandle::Lease{};
    CHECK(backend.unmaps == 0);
    // a 的 release 顺手回收被覆盖的那一块：两块各走完自己的宽限期
    CHECK_FALSE(a.release(1000));
    CHECK(a.release(1000 + SegmentHandle::kReleaseGraceMs));
    CHECK(backend.unmaps == 2);
}

TEST_CASE("the grace period never ends early when the clock reads 0 or goes backwards", "[ipc][backend][handle][clock]")
{
    PageBackend backend;
    SegmentHandle handle = openHandle(backend);
    CHECK_FALSE(handle.release(0)); // 读不出时钟：宽限期不开始
    CHECK_FALSE(handle.release(0));
    CHECK_FALSE(handle.release(5000000)); // 从这里开始
    CHECK_FALSE(handle.release(0)); // 无符号相减回绕成极大值也不许提前解映射
    CHECK(backend.unmaps == 0);
    CHECK_FALSE(handle.release(10)); // 时间倒退：从 10 重新计
    CHECK_FALSE(handle.release(10 + SegmentHandle::kReleaseGraceMs - 1));
    CHECK(backend.unmaps == 0);
    CHECK(handle.release(10 + SegmentHandle::kReleaseGraceMs));
    CHECK(backend.unmaps == 1);
}

TEST_CASE("handles destroyed before the grace period are reclaimed once it passes", "[ipc][backend][handle][reap]")
{
    // 插件卸载、删轨、撤销反复建删实例：析构时宽限期才开始，映射交给回收表，之后由回收者解映射、关句柄，
    // 不在会话期间越积越多
    PageBackend backend;
    constexpr int kHandles = 5;
    drainRetired();
    const std::size_t pendingBefore = SegmentHandle::retiredPending();
    for (int i = 0; i < kHandles; ++i)
    {
        SegmentHandle handle = openHandle(backend);
        CHECK(handle.valid());
    }
    CHECK(backend.unmaps == 0);
    CHECK(SegmentHandle::retiredPending() == pendingBefore + kHandles);
    // 析构时的宽限期按真实时钟开始；这里换成注入的时间：时间倒退从头计，再走满 500 ms
    SegmentHandle::reapRetired(1000);
    CHECK(SegmentHandle::reapRetired(1000 + SegmentHandle::kReleaseGraceMs - 1) == 0);
    CHECK(backend.unmaps == 0);
    CHECK(SegmentHandle::reapRetired(1000 + SegmentHandle::kReleaseGraceMs) >= static_cast<std::size_t>(kHandles));
    CHECK(backend.unmaps == kHandles);
    CHECK(SegmentHandle::retiredPending() == pendingBefore);
}

TEST_CASE("mappings handed to the reaper are released after their backend is gone", "[ipc][backend][handle][reap]")
{
    // 句柄先析构交给回收表，后端紧接着析构。映射不许漏到进程退出：回收表不再经已经没了的后端，到时用后端事先给的
    // 收尾函数解映射、关平台句柄
    drainRetired();
    const std::size_t pendingBefore = SegmentHandle::retiredPending();
    const int orphanBefore = gOrphanUnmaps.load();
    int unmapsViaBackend = 0;
    {
        PageBackend backend;
        {
            SegmentHandle handle = openHandle(backend);
        }
        unmapsViaBackend = backend.unmaps;
    }
    CHECK(unmapsViaBackend == 0);
    CHECK(SegmentHandle::retiredPending() == pendingBefore + 1); // 还在等宽限期，没有被丢掉
    SegmentHandle::reapRetired(1000);
    CHECK(gOrphanUnmaps.load() == orphanBefore);
    CHECK(SegmentHandle::reapRetired(1000 + SegmentHandle::kReleaseGraceMs) >= 1);
    CHECK(gOrphanUnmaps.load() == orphanBefore + 1);
    CHECK(SegmentHandle::retiredPending() == pendingBefore);
}

TEST_CASE("a handle declared before its backend is destroyed after it without touching the dead backend",
          "[ipc][backend][handle][lifetime]")
{
    // 成员顺序 { SegmentHandle h; Backend b; }：析构时后端先没，句柄随后析构。后端析构时映射表必须连还活着的句柄
    // 一起忘掉它；否则句柄析构把映射挂进回收表时带着悬空的后端指针，下一次回收对已经析构的对象做虚调用
    drainRetired();
    const std::size_t pendingBefore = SegmentHandle::retiredPending();
    const int orphanBefore = gOrphanUnmaps.load();
    {
        struct Owner
        {
            SegmentHandle handle;
            PageBackend backend;
        };
        auto owner = std::make_unique<Owner>();
        owner->handle = openHandle(owner->backend);
        REQUIRE(owner->handle.valid());
        firstSample(owner->handle.base())->store(1.0f);
    }
    CHECK(SegmentHandle::retiredPending() == pendingBefore + 1);
    SegmentHandle::reapRetired(1000);
    CHECK(SegmentHandle::reapRetired(1000 + SegmentHandle::kReleaseGraceMs) >= 1);
    CHECK(gOrphanUnmaps.load() == orphanBefore + 1);
    CHECK(SegmentHandle::retiredPending() == pendingBefore);
}

TEST_CASE("a live handle releases through the orphan path after its backend is destroyed",
          "[ipc][backend][handle][lifetime]")
{
    // 后端先析构、句柄还独占着映射：之后的 release 不经后端，用收尾函数解映射
    const int orphanBefore = gOrphanUnmaps.load();
    auto backend = std::make_unique<PageBackend>();
    SegmentHandle handle = openHandle(*backend);
    {
        auto lease = handle.lease();
        REQUIRE(lease);
        backend.reset(); // 租约在途时后端没了：映射不受影响
        firstSample(lease.base())->store(1.0f);
    }
    CHECK_FALSE(handle.release(1000));
    CHECK(gOrphanUnmaps.load() == orphanBefore);
    CHECK(handle.release(1000 + SegmentHandle::kReleaseGraceMs));
    CHECK(gOrphanUnmaps.load() == orphanBefore + 1);
}

TEST_CASE("released control blocks are kept until exit and counted for diagnostics", "[ipc][backend][handle]")
{
    // 控制块永不释放：句柄只在加载、改组时新建。done 计数让按拍新建句柄这种误用在诊断里看得出来
    PageBackend backend;
    const std::size_t doneBefore = SegmentHandle::retiredDone();
    for (int i = 0; i < 3; ++i)
    {
        SegmentHandle handle = openHandle(backend);
        CHECK_FALSE(handle.release(1000));
        CHECK(handle.release(1000 + SegmentHandle::kReleaseGraceMs));
    }
    CHECK(SegmentHandle::retiredDone() == doneBefore + 3);
    CHECK(backend.unmaps == 3);
}

TEST_CASE("leases racing a release never touch an unmapped view", "[ipc][backend][handle][threads]")
{
    // 音频线程一直在拿租约、写映射；非实时线程请求释放、按虚拟时间走完宽限期、解映射（页面真的还给系统），
    // 之后音频线程还继续尝试一阵。解映射之后拿到任何租约、或者写到已释放的页面，都是缺陷
    PageBackend backend;
    constexpr int kRounds = 100;
    long grantedAfterUnmap = 0;
    for (int round = 0; round < kRounds; ++round)
    {
        SegmentHandle handle = openHandle(backend);
        std::atomic<bool> stop{false};
        std::atomic<bool> unmapped{false};
        std::atomic<long> granted{0};
        std::atomic<long> late{0};
        std::thread audio([&] {
            while (!stop.load(std::memory_order_acquire))
            {
                auto lease = handle.lease();
                if (!lease)
                {
                    continue;
                }
                if (unmapped.load(std::memory_order_acquire))
                {
                    late.fetch_add(1, std::memory_order_relaxed);
                }
                firstSample(lease.base())->store(1.0f, std::memory_order_relaxed);
                granted.fetch_add(1, std::memory_order_relaxed);
            }
        });
        while (granted.load(std::memory_order_relaxed) < 100)
        {
            std::this_thread::yield();
        }
        std::uint64_t now = 10;
        while (!handle.release(now))
        {
            now += 250;
        }
        unmapped.store(true, std::memory_order_release);
        for (int i = 0; i < 2000; ++i)
        {
            std::this_thread::yield();
        }
        stop.store(true, std::memory_order_release);
        audio.join();
        grantedAfterUnmap += late.load();
    }
    CHECK(grantedAfterUnmap == 0);
    CHECK(backend.unmaps == kRounds);
}
