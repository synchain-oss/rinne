// SPDX-License-Identifier: GPL-3.0-or-later
//
// 零分配断言的底座自查：计数器必须数得到每一种分配原语，否则「回调里没有分配」会在某一种分配上静默变绿。
// 每种原语一个必须数到的用例：它们是零分配断言的反例，计数器漏掉任何一种，对应的用例就变红。
// malloc 家族（HeapBlock、AudioBuffer、juce::Array、MemoryBlock、calloc、realloc）的计数目前只在 Windows 上实现，
// 见 AllocationCounter.h；其他平台等 mac CI 那一轮补上钩子再启用这几个用例。

#include <catch2/catch_test_macros.hpp>

#include <juce_audio_processors/juce_audio_processors.h>

#include <cstdlib>

#include "AllocationCounter.h"

namespace
{

struct alignas(64) Wide
{
    char bytes[64];
};

// 写一次 volatile，免得编译器把成对的分配与释放整个消掉
template<typename T>
void touch(T* p)
{
    *reinterpret_cast<volatile char*>(p) = 1;
}

} // namespace

TEST_CASE("allocation counter sees operator new in all its forms", "[host][shell][allocation]")
{
    using rinne::test::AllocationScope;

    SECTION("plain new")
    {
        const AllocationScope scope;
        auto* p = new int(1);
        touch(p);
        delete p;
        CHECK(scope.allocations() == 1);
    }

    SECTION("array new")
    {
        const AllocationScope scope;
        auto* p = new int[4];
        touch(p);
        delete[] p;
        CHECK(scope.allocations() == 1);
    }

    SECTION("nothrow new")
    {
        const AllocationScope scope;
        auto* p = new (std::nothrow) int[4];
        touch(p);
        delete[] p;
        CHECK(scope.allocations() == 1);
    }

    SECTION("over-aligned new")
    {
        const AllocationScope scope;
        auto* p = new Wide;
        touch(p);
        delete p;
        CHECK(scope.allocations() == 1);
    }

    SECTION("over-aligned array new")
    {
        const AllocationScope scope;
        auto* p = new Wide[2];
        touch(p);
        delete[] p;
        CHECK(scope.allocations() == 1);
    }
}

TEST_CASE("allocation counter ignores what happens outside its scope", "[host][shell][allocation]")
{
    using rinne::test::AllocationScope;

    SECTION("stack work and allocations before the scope are not counted")
    {
        auto* before = new int(1);
        touch(before);
        const AllocationScope scope;
        int local[16] = {};
        touch(local);
        delete before;
        CHECK(scope.allocations() == 0);
    }

    SECTION("nested scopes each count their own part")
    {
        const AllocationScope outer;
        auto* a = new int(1);
        touch(a);
        {
            const AllocationScope inner;
            auto* b = new int(2);
            touch(b);
            delete b;
            CHECK(inner.allocations() == 1);
        }
        delete a;
        CHECK(outer.allocations() == 2);
    }
}

#if defined(_WIN32)

TEST_CASE("allocation counter sees the malloc family JUCE relies on", "[host][shell][allocation]")
{
    using rinne::test::AllocationScope;

    SECTION("malloc")
    {
        const AllocationScope scope;
        void* p = std::malloc(64);
        touch(p);
        std::free(p);
        CHECK(scope.allocations() >= 1);
    }

    SECTION("calloc")
    {
        const AllocationScope scope;
        void* p = std::calloc(4, 16);
        touch(p);
        std::free(p);
        CHECK(scope.allocations() >= 1);
    }

    SECTION("realloc")
    {
        void* p = std::malloc(16);
        touch(p);
        const AllocationScope scope;
        p = std::realloc(p, 4096);
        touch(p);
        const long seen = scope.allocations();
        std::free(p);
        CHECK(seen >= 1);
    }

    SECTION("juce::HeapBlock")
    {
        const AllocationScope scope;
        juce::HeapBlock<float> block;
        block.allocate(256, false);
        touch(block.get());
        CHECK(scope.allocations() >= 1);
    }

    SECTION("juce::AudioBuffer construction")
    {
        const AllocationScope scope;
        juce::AudioBuffer<float> buffer(2, 480);
        touch(buffer.getWritePointer(0));
        CHECK(scope.allocations() >= 1);
    }

    SECTION("juce::AudioBuffer::setSize")
    {
        juce::AudioBuffer<float> buffer(2, 16);
        const AllocationScope scope;
        buffer.setSize(2, 100000);
        touch(buffer.getWritePointer(0));
        CHECK(scope.allocations() >= 1);
    }

    SECTION("juce::Array growth")
    {
        juce::Array<int> array;
        const AllocationScope scope;
        for (int i = 0; i < 1000; ++i)
            array.add(i);
        CHECK(scope.allocations() >= 1);
    }

    SECTION("juce::MemoryBlock")
    {
        const AllocationScope scope;
        juce::MemoryBlock block(512);
        touch(block.getData());
        CHECK(scope.allocations() >= 1);
    }

    SECTION("juce::String")
    {
        const AllocationScope scope;
        juce::String text("a string that is long enough to need heap storage, not the small buffer");
        text += "more";
        CHECK(scope.allocations() >= 1);
    }
}

#endif
