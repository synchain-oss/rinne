// SPDX-License-Identifier: GPL-3.0-or-later
//
// 零分配断言的底座自查：计数器必须数得到每一种分配与释放原语，否则「回调里没有分配」会在某一种原语上静默变绿。
// 每种原语一个必须数到的用例：它们是零分配断言的反例，计数器漏掉任何一种，对应的用例就变红。
// 用例都先把计数读进局部变量、作用域结束之后再 CHECK：断言宏自己记录结果时也会分配（junit、json 报告器下更明显），
// 在作用域里 CHECK 会把它们数进去，结果就取决于用哪个报告器。
// malloc 家族（HeapBlock、AudioBuffer、juce::Array、MemoryBlock、calloc、realloc）的计数目前只在 Windows 上实现，
// 见 AllocationCounter.h；其他平台上这几个用例会以 SKIP 标出来，等 mac CI 那一轮补上钩子再启用。

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
        long seen = 0;
        {
            const AllocationScope scope;
            auto* p = new int(1);
            touch(p);
            delete p;
            seen = scope.allocations();
        }
        CHECK(seen == 1);
    }

    SECTION("array new")
    {
        long seen = 0;
        {
            const AllocationScope scope;
            auto* p = new int[4];
            touch(p);
            delete[] p;
            seen = scope.allocations();
        }
        CHECK(seen == 1);
    }

    SECTION("nothrow new")
    {
        long seen = 0;
        {
            const AllocationScope scope;
            auto* p = new (std::nothrow) int[4];
            touch(p);
            delete[] p;
            seen = scope.allocations();
        }
        CHECK(seen == 1);
    }

    SECTION("over-aligned new")
    {
        long seen = 0;
        {
            const AllocationScope scope;
            auto* p = new Wide;
            touch(p);
            delete p;
            seen = scope.allocations();
        }
        CHECK(seen == 1);
    }

    SECTION("over-aligned array new")
    {
        long seen = 0;
        {
            const AllocationScope scope;
            auto* p = new Wide[2];
            touch(p);
            delete[] p;
            seen = scope.allocations();
        }
        CHECK(seen == 1);
    }
}

TEST_CASE("allocation counter sees releases as well", "[host][shell][allocation]")
{
    using rinne::test::AllocationScope;

    SECTION("delete of an object allocated before the scope")
    {
        auto* p = new int(1);
        touch(p);
        long allocs = -1;
        long frees = -1;
        {
            const AllocationScope scope;
            delete p;
            allocs = scope.allocations();
            frees = scope.frees();
        }
        CHECK(allocs == 0);
        CHECK(frees == 1);
    }

    SECTION("delete[] and over-aligned delete")
    {
        auto* a = new int[4];
        auto* w = new Wide;
        touch(a);
        touch(w);
        long frees = -1;
        {
            const AllocationScope scope;
            delete[] a;
            delete w;
            frees = scope.frees();
        }
        CHECK(frees == 2);
    }

    SECTION("an allocation inside the scope that is released inside it counts on both sides")
    {
        long allocs = -1;
        long frees = -1;
        {
            const AllocationScope scope;
            auto* p = new int(1);
            touch(p);
            delete p;
            allocs = scope.allocations();
            frees = scope.frees();
        }
        CHECK(allocs == 1);
        CHECK(frees == 1);
    }

    SECTION("nothing released, nothing counted")
    {
        long frees = -1;
        {
            const AllocationScope scope;
            int local[4] = {};
            touch(local);
            frees = scope.frees();
        }
        CHECK(frees == 0);
    }
}

TEST_CASE("allocation counter ignores what happens outside its scope", "[host][shell][allocation]")
{
    using rinne::test::AllocationScope;

    SECTION("stack work and allocations before the scope are not counted")
    {
        auto* before = new int(1);
        touch(before);
        long seen = -1;
        {
            const AllocationScope scope;
            int local[16] = {};
            touch(local);
            seen = scope.allocations();
        }
        delete before;
        CHECK(seen == 0);
    }

    SECTION("nested scopes each count their own part")
    {
        long inner = -1;
        long outer = -1;
        {
            const AllocationScope outerScope;
            auto* a = new int(1);
            touch(a);
            {
                const AllocationScope innerScope;
                auto* b = new int(2);
                touch(b);
                delete b;
                inner = innerScope.allocations();
            }
            delete a;
            outer = outerScope.allocations();
        }
        CHECK(inner == 1);
        CHECK(outer == 2);
    }
}

#if defined(_WIN32)

TEST_CASE("allocation counter sees the malloc family JUCE relies on", "[host][shell][allocation]")
{
    using rinne::test::AllocationScope;

    SECTION("malloc")
    {
        long seen = 0;
        {
            const AllocationScope scope;
            void* p = std::malloc(64);
            touch(p);
            std::free(p);
            seen = scope.allocations();
        }
        CHECK(seen >= 1);
    }

    SECTION("calloc")
    {
        long seen = 0;
        {
            const AllocationScope scope;
            void* p = std::calloc(4, 16);
            touch(p);
            std::free(p);
            seen = scope.allocations();
        }
        CHECK(seen >= 1);
    }

    SECTION("realloc")
    {
        void* p = std::malloc(16);
        touch(p);
        long seen = 0;
        {
            const AllocationScope scope;
            p = std::realloc(p, 4096);
            touch(p);
            seen = scope.allocations();
        }
        std::free(p);
        CHECK(seen >= 1);
    }

    SECTION("free")
    {
        void* p = std::malloc(32);
        touch(p);
        long seen = 0;
        {
            const AllocationScope scope;
            std::free(p);
            seen = scope.frees();
        }
        CHECK(seen >= 1);
    }

    SECTION("juce::HeapBlock release")
    {
        juce::HeapBlock<float> block;
        block.allocate(64, false);
        touch(block.get());
        long seen = 0;
        {
            const AllocationScope scope;
            block.free();
            seen = scope.frees();
        }
        CHECK(seen >= 1);
    }

    SECTION("juce::HeapBlock")
    {
        long seen = 0;
        {
            const AllocationScope scope;
            juce::HeapBlock<float> block;
            block.allocate(256, false);
            touch(block.get());
            seen = scope.allocations();
        }
        CHECK(seen >= 1);
    }

    SECTION("juce::AudioBuffer construction")
    {
        long seen = 0;
        {
            const AllocationScope scope;
            juce::AudioBuffer<float> buffer(2, 480);
            touch(buffer.getWritePointer(0));
            seen = scope.allocations();
        }
        CHECK(seen >= 1);
    }

    SECTION("juce::AudioBuffer::setSize")
    {
        juce::AudioBuffer<float> buffer(2, 16);
        long seen = 0;
        {
            const AllocationScope scope;
            buffer.setSize(2, 100000);
            touch(buffer.getWritePointer(0));
            seen = scope.allocations();
        }
        CHECK(seen >= 1);
    }

    SECTION("juce::Array growth")
    {
        juce::Array<int> array;
        long seen = 0;
        {
            const AllocationScope scope;
            for (int i = 0; i < 1000; ++i)
                array.add(i);
            seen = scope.allocations();
        }
        CHECK(seen >= 1);
    }

    SECTION("juce::MemoryBlock")
    {
        long seen = 0;
        {
            const AllocationScope scope;
            juce::MemoryBlock block(512);
            touch(block.getData());
            seen = scope.allocations();
        }
        CHECK(seen >= 1);
    }

    SECTION("juce::String")
    {
        long seen = 0;
        {
            const AllocationScope scope;
            juce::String text("a string that is long enough to need heap storage, not the small buffer");
            text += "more";
            seen = scope.allocations();
        }
        CHECK(seen >= 1);
    }
}

#else

TEST_CASE("allocation counter sees the malloc family JUCE relies on", "[host][shell][allocation]")
{
    SKIP("malloc family is not hooked on this platform yet; the zero-allocation assertions only cover operator new "
         "here");
}

#endif
