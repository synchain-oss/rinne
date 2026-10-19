// SPDX-License-Identifier: GPL-3.0-or-later
//
// 内存不足：把全局 operator new/delete 换成一个受开关控制的版本，从第 N 次分配起抛 std::bad_alloc，逐个 N 扫
// loadState 与 encodeDocument。判据：不崩（异常被公开入口的 catch 接住）、失败时出参保持调用前的样子、成功时结果
// 完整。开关默认关，关闭时直接转发 malloc/free，行为与默认分配器相同；只在下面的用例里短暂打开，不影响其他用例。
// 全局替换只能有一处：同一个测试可执行文件里再有别的翻译单元或库替换分配器，会链接冲突（重复定义），到时要改成单独的\n//
// 测试目标。new/delete 成对替换；对齐版本在 malloc 出来的块里自己算对齐，ASan 构建下也留在 malloc/free 这一套里。

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <utility>

#include "StateTestKit.h"
#include "state/StateCodec.h"
#include "state/StateDocument.h"
#include "state/StateMigration.h"

// TSan 的运行库（libclang_rt.tsan_cxx）自己定义了全局 operator new/delete，再替换会重复定义、链接失败；这组用例在 TSan
// 构建下跳过（ASan 构建允许用户替换，照常运行）。
#if defined(__SANITIZE_THREAD__)
#define RINNE_STATE_ALLOC_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define RINNE_STATE_ALLOC_TSAN 1
#endif
#endif

#ifndef RINNE_STATE_ALLOC_TSAN

namespace
{

std::atomic<bool> g_armed{false};
std::atomic<std::size_t> g_allocations{0};
std::atomic<long long> g_failFrom{0}; // 0 = 不注入；否则第 N 次分配起失败
thread_local bool t_constructingFailure = false; // 抛 bad_alloc 期间不再注入，免得异常对象自己的分配也被打断

bool allocationShouldFail() noexcept
{
    if (t_constructingFailure || !g_armed.load(std::memory_order_relaxed))
        return false;
    const std::size_t index = g_allocations.fetch_add(1u, std::memory_order_relaxed) + 1u;
    const long long failFrom = g_failFrom.load(std::memory_order_relaxed);
    return failFrom > 0 && index >= static_cast<std::size_t>(failFrom);
}

[[noreturn]] void throwAllocationFailure()
{
    t_constructingFailure = true;
    struct Reset
    {
        ~Reset() { t_constructingFailure = false; }
    };
    [[maybe_unused]] Reset reset;
    throw std::bad_alloc{};
}

void* allocateBytes(std::size_t size)
{
    if (allocationShouldFail())
        throwAllocationFailure();
    void* memory = std::malloc(size == 0u ? 1u : size);
    if (memory == nullptr)
        throw std::bad_alloc{};
    return memory;
}

void* allocateAlignedBytes(std::size_t size, std::size_t alignment)
{
    if (allocationShouldFail())
        throwAllocationFailure();
    if (alignment < sizeof(void*))
        alignment = sizeof(void*);
    const std::size_t bytes = size == 0u ? 1u : size;
    void* raw = std::malloc(bytes + alignment + sizeof(void*));
    if (raw == nullptr)
        throw std::bad_alloc{};
    const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(raw) + sizeof(void*);
    const std::uintptr_t aligned = (start + alignment - 1u) & ~static_cast<std::uintptr_t>(alignment - 1u);
    reinterpret_cast<void**>(aligned)[-1] = raw;
    return reinterpret_cast<void*>(aligned);
}

void releaseAlignedBytes(void* memory) noexcept
{
    if (memory != nullptr)
        std::free(reinterpret_cast<void**>(memory)[-1]);
}

void* allocateBytesNoThrow(std::size_t size) noexcept
{
    try
    {
        return allocateBytes(size);
    }
    catch (...)
    {
        return nullptr;
    }
}

void* allocateAlignedBytesNoThrow(std::size_t size, std::size_t alignment) noexcept
{
    try
    {
        return allocateAlignedBytes(size, alignment);
    }
    catch (...)
    {
        return nullptr;
    }
}

// 打开开关并清零计数；析构时关掉。读结果与断言都在开关关掉之后做，免得测试框架自己的分配也被算进去。
class AllocationGate
{
public:
    explicit AllocationGate(std::size_t failFrom) noexcept
    {
        g_allocations.store(0u, std::memory_order_relaxed);
        g_failFrom.store(static_cast<long long>(failFrom), std::memory_order_relaxed);
        g_armed.store(true, std::memory_order_relaxed);
    }

    ~AllocationGate() { disarm(); }

    AllocationGate(const AllocationGate&) = delete;
    AllocationGate& operator=(const AllocationGate&) = delete;

    static void disarm() noexcept
    {
        g_armed.store(false, std::memory_order_relaxed);
        g_failFrom.store(0, std::memory_order_relaxed);
    }
};

std::size_t allocationsOfGate() noexcept
{
    return g_allocations.load(std::memory_order_relaxed);
}

} // namespace

void* operator new(std::size_t size)
{
    return allocateBytes(size);
}

void* operator new[](std::size_t size)
{
    return allocateBytes(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    return allocateBytesNoThrow(size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    return allocateBytesNoThrow(size);
}

void* operator new(std::size_t size, std::align_val_t alignment)
{
    return allocateAlignedBytes(size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment)
{
    return allocateAlignedBytes(size, static_cast<std::size_t>(alignment));
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    return allocateAlignedBytesNoThrow(size, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    return allocateAlignedBytesNoThrow(size, static_cast<std::size_t>(alignment));
}

void operator delete(void* memory) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::align_val_t) noexcept
{
    releaseAlignedBytes(memory);
}

void operator delete[](void* memory, std::align_val_t) noexcept
{
    releaseAlignedBytes(memory);
}

void operator delete(void* memory, std::size_t, std::align_val_t) noexcept
{
    releaseAlignedBytes(memory);
}

void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept
{
    releaseAlignedBytes(memory);
}

namespace
{

constexpr std::size_t kAllocationScanLimit = 100000u;

std::size_t countLoadAllocations(const rinne::test::Bytes& bytes, rinne::state::Role role,
                                 const rinne::state::StateDocument& expected)
{
    rinne::state::StateDocument out = rinne::test::sentinelDocument(role);
    rinne::state::StateLoadStatus status = rinne::state::StateLoadStatus::Corrupt;
    std::size_t allocations = 0;
    {
        AllocationGate gate(kAllocationScanLimit);
        status = rinne::state::loadState(bytes.data(), bytes.size(), role, out).status;
        allocations = allocationsOfGate();
    }
    REQUIRE(status == rinne::state::StateLoadStatus::Ok);
    REQUIRE(rinne::test::sameDocument(out, expected));
    REQUIRE(allocations > 0u);
    return allocations;
}

} // namespace

TEST_CASE("allocation: loadState 在任意一次分配失败时返回 Corrupt，不碰出参", "[state][allocation]")
{
    const rinne::state::StateDocument expected = rinne::test::sampleInputDocument();
    rinne::test::Bytes bytes;
    REQUIRE(rinne::state::encodeDocument(expected, bytes));

    const std::size_t allocations = countLoadAllocations(bytes, rinne::state::Role::Input, expected);
    std::size_t failures = 0;
    for (std::size_t failFrom = 1u; failFrom <= allocations + 1u; ++failFrom)
    {
        rinne::state::StateDocument out = rinne::test::sentinelDocument(rinne::state::Role::Input);
        rinne::state::StateLoadStatus status = rinne::state::StateLoadStatus::Ok;
        {
            AllocationGate gate(failFrom);
            status = rinne::state::loadState(bytes.data(), bytes.size(), rinne::state::Role::Input, out).status;
        }

        INFO("第 " << failFrom << " 次分配起失败");
        if (status == rinne::state::StateLoadStatus::Ok)
        {
            CHECK(failFrom == allocations + 1u);
            CHECK(rinne::test::sameDocument(out, expected));
        }
        else
        {
            ++failures;
            CHECK(status == rinne::state::StateLoadStatus::Corrupt);
            CHECK(rinne::test::sameDocument(out, rinne::test::sentinelDocument(rinne::state::Role::Input)));
        }
    }
    CHECK(failures == allocations);
}

TEST_CASE("allocation: encodeDocument 在任意一次分配失败时返回失败，不碰出参", "[state][allocation]")
{
    const rinne::state::StateDocument document = rinne::test::sampleInputDocument();
    rinne::test::Bytes expected;
    REQUIRE(rinne::state::encodeDocument(document, expected));

    std::size_t allocations = 0;
    {
        rinne::test::Bytes out = rinne::test::byteList({0xff});
        AllocationGate gate(kAllocationScanLimit);
        const bool ok = rinne::state::encodeDocument(document, out);
        allocations = allocationsOfGate();
        AllocationGate::disarm();
        REQUIRE(ok);
        REQUIRE(out == expected);
    }
    REQUIRE(allocations > 0u);

    std::size_t failures = 0;
    for (std::size_t failFrom = 1u; failFrom <= allocations + 1u; ++failFrom)
    {
        const rinne::test::Bytes sentinel = rinne::test::byteList({0xff});
        rinne::test::Bytes out = sentinel;
        bool ok = true;
        {
            AllocationGate gate(failFrom);
            ok = rinne::state::encodeDocument(document, out);
        }

        INFO("第 " << failFrom << " 次分配起失败");
        if (ok)
        {
            CHECK(failFrom == allocations + 1u);
            CHECK(out == expected);
        }
        else
        {
            ++failures;
            CHECK(out == sentinel);
        }
    }
    CHECK(failures == allocations);
}

TEST_CASE("allocation: 复制高 abi 原字节失败时只返回 Corrupt", "[state][allocation]")
{
    const rinne::state::StateDocument source = rinne::test::sampleInputDocument();
    rinne::test::Bytes bytes;
    REQUIRE(rinne::state::encodeDocument(source, bytes));
    // 把 abi 改成 9：加载走 RejectedNewer 分支，要复制整份原字节。
    for (int i = 0; i < 4; ++i)
        bytes[4u + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((9u >> (8 * i)) & 0xffu);

    std::size_t allocations = 0;
    {
        rinne::state::StateDocument out = rinne::test::sentinelDocument(rinne::state::Role::Input);
        AllocationGate gate(kAllocationScanLimit);
        const auto result = rinne::state::loadState(bytes.data(), bytes.size(), rinne::state::Role::Input, out);
        allocations = allocationsOfGate();
        AllocationGate::disarm();
        REQUIRE(result.status == rinne::state::StateLoadStatus::RejectedNewer);
        REQUIRE(result.preservedOriginal == bytes);
        REQUIRE(rinne::test::sameDocument(out, rinne::test::sentinelDocument(rinne::state::Role::Input)));
    }
    REQUIRE(allocations > 0u);

    std::size_t failures = 0;
    for (std::size_t failFrom = 1u; failFrom <= allocations + 1u; ++failFrom)
    {
        rinne::state::StateDocument out = rinne::test::sentinelDocument(rinne::state::Role::Input);
        rinne::state::StateLoadStatus status = rinne::state::StateLoadStatus::Ok;
        rinne::test::Bytes preserved;
        {
            AllocationGate gate(failFrom);
            auto result = rinne::state::loadState(bytes.data(), bytes.size(), rinne::state::Role::Input, out);
            status = result.status;
            preserved = std::move(result.preservedOriginal);
        }

        INFO("第 " << failFrom << " 次分配起失败");
        CHECK(rinne::test::sameDocument(out, rinne::test::sentinelDocument(rinne::state::Role::Input)));
        if (status == rinne::state::StateLoadStatus::RejectedNewer)
        {
            CHECK(failFrom == allocations + 1u);
            CHECK(preserved == bytes);
        }
        else
        {
            ++failures;
            CHECK(status == rinne::state::StateLoadStatus::Corrupt);
            CHECK(preserved.empty());
        }
    }
    CHECK(failures == allocations);
}

#else

TEST_CASE("allocation: 分配失败注入在 TSan 构建下跳过", "[state][allocation]")
{
    SKIP("TSan 运行库已定义全局 operator new/delete，替换会重复定义；分配失败注入由其他构建覆盖");
}

#endif
