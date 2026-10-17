// SPDX-License-Identifier: GPL-3.0-or-later
//
// 映射句柄的租约与释放：持租约时不解映射；释放请求之后不再发租约；租约归零后还要再等满宽限期才经后端解映射；
// 移动只转移所有权；析构或移动赋值覆盖时走不完宽限期，控制块与映射都留到进程退出，租约照样有效；
// 时钟读出 0 或倒退时宽限期从头算。时间用参数注入，不睡眠。

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>
#include <thread>

#include "ipc/ISegmentBackend.h"
#include "ipc/Naming.h"
#include "ipc/SegmentBackendInProcess.h"

namespace
{

using namespace rinne::ipc;

// 进程内后端外面包一层，数一数 unmap 调了几次
class CountingBackend final : public ISegmentBackend
{
public:
    InitResult createOrOpen(const std::string& name, std::size_t size, SegmentView& view) override
    {
        return inner.createOrOpen(name, size, view);
    }
    InitResult openExisting(const std::string& name, SegmentView& view) override
    {
        return inner.openExisting(name, view);
    }
    void unmap(SegmentView& view) override
    {
        ++unmaps;
        inner.unmap(view);
    }

    SegmentBackendInProcess inner;
    int unmaps = 0;
};

SegmentHandle openHandle(CountingBackend& backend)
{
    std::string name;
    REQUIRE(Naming(NameScope::randomForTesting()).name(SegmentId::groupData(1), name) == NameStatus::kOk);
    SegmentView view;
    REQUIRE(backend.createOrOpen(name, kRingBytes, view) == InitResult::kOk);
    return SegmentHandle(std::move(view), &backend);
}

} // namespace

TEST_CASE("a held lease blocks unmapping and the grace period follows", "[ipc][backend][handle]")
{
    SegmentBackendInProcess::resetAllForTesting();
    CountingBackend backend;
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
    SegmentBackendInProcess::resetAllForTesting();
    CountingBackend backend;
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

TEST_CASE("a lease that outlives its handle keeps the control block alive", "[ipc][backend][handle][lifetime]")
{
    // 插件卸载时音频线程还拿着租约：句柄析构走不完宽限期，控制块与映射都要留到进程退出，
    // 否则租约读 base、归还计数都落在已释放的堆上
    SegmentBackendInProcess::resetAllForTesting();
    CountingBackend backend;
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
    static_cast<unsigned char*>(lease.base())[kRingDataOffset] = 1;
    lease = SegmentHandle::Lease{}; // 归还：落在仍然活着的控制块上
    CHECK(backend.unmaps == 0);
}

TEST_CASE("move-assigning over a live handle keeps its outstanding leases valid", "[ipc][backend][handle][lifetime]")
{
    SegmentBackendInProcess::resetAllForTesting();
    CountingBackend backend;
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
    leaseA = SegmentHandle::Lease{};
    CHECK(backend.unmaps == 0);
    CHECK_FALSE(a.release(1000));
    CHECK(a.release(1000 + SegmentHandle::kReleaseGraceMs));
    CHECK(backend.unmaps == 1);
}

TEST_CASE("the grace period never ends early when the clock reads 0 or goes backwards", "[ipc][backend][handle][clock]")
{
    SegmentBackendInProcess::resetAllForTesting();
    CountingBackend backend;
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

TEST_CASE("destroying a handle before the grace period leaves the mapping alone", "[ipc][backend][handle]")
{
    SegmentBackendInProcess::resetAllForTesting();
    CountingBackend backend;
    {
        SegmentHandle handle = openHandle(backend);
        CHECK(handle.valid());
    }
    // 析构时宽限期才开始：不解映射，映射留到进程退出（音频线程可能仍持有的裸指针不会悬空）
    CHECK(backend.unmaps == 0);
}

TEST_CASE("leases taken concurrently with a release request stop once it is seen", "[ipc][backend][handle][threads]")
{
    SegmentBackendInProcess::resetAllForTesting();
    CountingBackend backend;
    SegmentHandle handle = openHandle(backend);
    auto* const samples = static_cast<std::atomic<float>*>(handle.base());

    std::atomic<bool> stop{false};
    std::atomic<bool> refused{false};
    std::atomic<long> granted{0};
    std::thread audio([&] {
        while (!stop.load(std::memory_order_acquire))
        {
            auto lease = handle.lease();
            if (!lease)
            {
                refused.store(true, std::memory_order_release);
                continue;
            }
            // 持租约期间访问映射：释放流程在这期间不得解映射
            static_cast<std::atomic<float>*>(lease.base())[kRingDataOffset / sizeof(float)].store(
                1.0f, std::memory_order_relaxed);
            granted.fetch_add(1, std::memory_order_relaxed);
        }
    });
    while (granted.load(std::memory_order_relaxed) < 1000)
    {
        std::this_thread::yield();
    }
    std::uint64_t now = 10;
    CHECK_FALSE(handle.release(now));
    while (!refused.load(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }
    // 音频线程已经看到释放请求；再停下它，宽限期按虚拟时间走完（真实宿主上这一段由 500 ms 真实时间兜住）
    stop.store(true, std::memory_order_release);
    audio.join();
    CHECK(samples[kRingDataOffset / sizeof(float)].load() == 1.0f);
    CHECK(backend.unmaps == 0);
    while (!handle.release(now))
    {
        now += 100;
    }
    CHECK(backend.unmaps == 1);
}
