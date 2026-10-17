// SPDX-License-Identifier: GPL-3.0-or-later

#include "AllocationCounter.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace
{

// 无构造函数的 thread_local：第一次访问不会触发分配
thread_local long gAllocations = 0;
thread_local long gFrees = 0;
thread_local int gScopeDepth = 0;

void count() noexcept
{
    if (gScopeDepth > 0)
        ++gAllocations;
}

void countFree() noexcept
{
    if (gScopeDepth > 0)
        ++gFrees;
}

#if defined(_WIN32)

using HeapAllocFn = LPVOID(WINAPI*)(HANDLE, DWORD, SIZE_T);
using HeapReAllocFn = LPVOID(WINAPI*)(HANDLE, DWORD, LPVOID, SIZE_T);
using HeapFreeFn = BOOL(WINAPI*)(HANDLE, DWORD, LPVOID);

HeapAllocFn gRealHeapAlloc = nullptr;
HeapReAllocFn gRealHeapReAlloc = nullptr;
HeapFreeFn gRealHeapFree = nullptr;

LPVOID WINAPI countedHeapAlloc(HANDLE heap, DWORD flags, SIZE_T bytes)
{
    count();
    return gRealHeapAlloc(heap, flags, bytes);
}

LPVOID WINAPI countedHeapReAlloc(HANDLE heap, DWORD flags, LPVOID block, SIZE_T bytes)
{
    count();
    return gRealHeapReAlloc(heap, flags, block, bytes);
}

BOOL WINAPI countedHeapFree(HANDLE heap, DWORD flags, LPVOID block)
{
    if (block != nullptr)
        countFree();
    return gRealHeapFree(heap, flags, block);
}

// 把本模块导入表里名叫 name 的导入项（不论来自哪个 DLL）换成 replacement；返回换了几处
int patchImports(const char* name, void* replacement)
{
    auto* base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0)
        return 0;

    int patched = 0;
    for (const auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
         desc->Name != 0; ++desc)
    {
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA*>(
            base + (desc->OriginalFirstThunk != 0 ? desc->OriginalFirstThunk : desc->FirstThunk));
        for (; names->u1.AddressOfData != 0; ++names, ++iat)
        {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
                continue;
            const auto* entry = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(entry->Name), name) != 0)
                continue;
            DWORD old = 0;
            if (!VirtualProtect(&iat->u1.Function, sizeof(iat->u1.Function), PAGE_READWRITE, &old))
                continue;
            iat->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
            VirtualProtect(&iat->u1.Function, sizeof(iat->u1.Function), old, &old);
            ++patched;
        }
    }
    return patched;
}

bool installHooks()
{
    const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    if (kernel == nullptr)
        return false;
    gRealHeapAlloc = reinterpret_cast<HeapAllocFn>(GetProcAddress(kernel, "HeapAlloc"));
    gRealHeapReAlloc = reinterpret_cast<HeapReAllocFn>(GetProcAddress(kernel, "HeapReAlloc"));
    gRealHeapFree = reinterpret_cast<HeapFreeFn>(GetProcAddress(kernel, "HeapFree"));
    if (gRealHeapAlloc == nullptr || gRealHeapReAlloc == nullptr || gRealHeapFree == nullptr)
        return false;
    // 三个都要换到：只换其一，realloc、malloc 或释放会漏
    return patchImports("HeapAlloc", reinterpret_cast<void*>(&countedHeapAlloc)) > 0 &&
           patchImports("HeapReAlloc", reinterpret_cast<void*>(&countedHeapReAlloc)) > 0 &&
           patchImports("HeapFree", reinterpret_cast<void*>(&countedHeapFree)) > 0;
}

#else

void* allocate(std::size_t size)
{
    count();
    if (void* p = std::malloc(size != 0 ? size : 1))
        return p;
    throw std::bad_alloc();
}

void* allocateAligned(std::size_t size, std::align_val_t alignment)
{
    count();
    const auto align = static_cast<std::size_t>(alignment);
    // aligned_alloc 要求大小是对齐的整数倍
    const std::size_t rounded = ((size != 0 ? size : 1) + align - 1) / align * align;
    if (void* p = std::aligned_alloc(align, rounded))
        return p;
    throw std::bad_alloc();
}

#endif

} // namespace

namespace rinne::test
{

AllocationScope::AllocationScope()
{
#if defined(_WIN32)
    static const bool installed = installHooks();
    if (!installed)
        throw std::runtime_error("AllocationCounter: could not hook HeapAlloc / HeapReAlloc in the import table");
#endif
    before_ = gAllocations;
    beforeFrees_ = gFrees;
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

long AllocationScope::frees() const noexcept
{
    return gFrees - beforeFrees_;
}

} // namespace rinne::test

#if !defined(_WIN32)

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
    countFree();
    std::free(p);
}

void operator delete[](void* p) noexcept
{
    countFree();
    std::free(p);
}

void operator delete(void* p, std::size_t) noexcept
{
    countFree();
    std::free(p);
}

void operator delete[](void* p, std::size_t) noexcept
{
    countFree();
    std::free(p);
}

void* operator new(std::size_t size, std::align_val_t alignment)
{
    return allocateAligned(size, alignment);
}

void* operator new[](std::size_t size, std::align_val_t alignment)
{
    return allocateAligned(size, alignment);
}

void operator delete(void* p, std::align_val_t) noexcept
{
    countFree();
    std::free(p);
}

void operator delete[](void* p, std::align_val_t) noexcept
{
    countFree();
    std::free(p);
}

void operator delete(void* p, std::size_t, std::align_val_t) noexcept
{
    countFree();
    std::free(p);
}

void operator delete[](void* p, std::size_t, std::align_val_t) noexcept
{
    countFree();
    std::free(p);
}

#endif
