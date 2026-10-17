// SPDX-License-Identifier: GPL-3.0-or-later

#include "AllocationCounter.h"

#include <cstddef>
#include <cstdlib>
#include <new>

namespace
{

// 无构造函数的 thread_local：第一次访问不会触发分配
thread_local long gAllocations = 0;
thread_local int gScopeDepth = 0;

void* allocate(std::size_t size)
{
    if (gScopeDepth > 0)
        ++gAllocations;
    if (void* p = std::malloc(size != 0 ? size : 1))
        return p;
    throw std::bad_alloc();
}

} // namespace

namespace rinne::test
{

AllocationScope::AllocationScope() noexcept : before_(gAllocations)
{
    ++gScopeDepth;
}

AllocationScope::~AllocationScope()
{
    --gScopeDepth;
}

long AllocationScope::allocations() const noexcept
{
    return gAllocations - before_;
}

} // namespace rinne::test

void* operator new(std::size_t size)
{
    return allocate(size);
}

void* operator new[](std::size_t size)
{
    return allocate(size);
}

void operator delete(void* p) noexcept
{
    std::free(p);
}

void operator delete[](void* p) noexcept
{
    std::free(p);
}

void operator delete(void* p, std::size_t) noexcept
{
    std::free(p);
}

void operator delete[](void* p, std::size_t) noexcept
{
    std::free(p);
}
