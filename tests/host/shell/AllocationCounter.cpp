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
#elif defined(__APPLE__)
#include <malloc/malloc.h>
#include <pthread.h>

#include <algorithm>
#endif

namespace
{

#if !defined(__APPLE__)

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

long threadAllocations() noexcept
{
    return gAllocations;
}

long threadFrees() noexcept
{
    return gFrees;
}

void enterScope() noexcept
{
    ++gScopeDepth;
}

void leaveScope() noexcept
{
    --gScopeDepth;
}

#endif

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

#elif defined(__APPLE__)

// 每个线程的计数放在 pthread_getspecific 取到的小结构里，而不用 thread_local：mac 上 thread_local
// 的存储在线程第一次访问时由 malloc 分配，而这里的访问发生在 malloc 钩子里，别的线程第一次进钩子就会递归。结构由
// AllocationScope 的构造函数在本线程
// 里建（那时本线程还没有结构，钩子不计数）；没建过结构的线程不在任何作用域里，钩子什么也不数。
struct ThreadState
{
    long allocations;
    long frees;
    int depth;
};

pthread_key_t gKey;
malloc_zone_t* gOriginal = nullptr;

// 致盲开关：环境变量 RINNE_TEST_ALLOC_BLIND 指定一种原语，计数器对它视而不见。CI 靠它证明每一种原语的用例真的会变红
// （计数器漏掉任何一种，对应用例必须失败），见 scripts/ci/mac/alloc-canary.sh。只有这个测试辅助读它，不进插件。
enum class Primitive
{
    mallocCall,
    callocCall,
    reallocCall,
    freeCall,
    none
};
Primitive gBlind = Primitive::none;

ThreadState* threadState() noexcept
{
    return static_cast<ThreadState*>(pthread_getspecific(gKey));
}

void count(Primitive primitive, long n = 1) noexcept
{
    if (primitive == gBlind)
        return;
    ThreadState* s = threadState();
    if (s != nullptr && s->depth > 0)
        s->allocations += n;
}

void countFree(long n = 1) noexcept
{
    if (gBlind == Primitive::freeCall)
        return;
    ThreadState* s = threadState();
    if (s != nullptr && s->depth > 0)
        s->frees += n;
}

long threadAllocations() noexcept
{
    const ThreadState* s = threadState();
    return s != nullptr ? s->allocations : 0;
}

long threadFrees() noexcept
{
    const ThreadState* s = threadState();
    return s != nullptr ? s->frees : 0;
}

void enterScope() noexcept
{
    ++threadState()->depth;
}

void leaveScope() noexcept
{
    --threadState()->depth;
}

// 包在系统默认 malloc zone 外面的计数 zone：malloc / calloc / realloc / free（以及 valloc、memalign、批量接口和带大小的
// free） 全部先数一次再转给原来的 zone。注册后把它推到 zone 列表最前面，malloc() 走的「默认 zone」就是它；它的 size
// 回调照原 zone 回答，所以 free() 按「谁认领这个指针」找到的也是它，安装前分配的块在作用域里释放同样数得到。 原来的
// zone 仍然注册着，加锁、统计、枚举都由它自己负责，计数 zone 的这几项因此什么也不做（做了会重复加锁）。
size_t zoneSize(malloc_zone_t*, const void* ptr)
{
    return gOriginal->size(gOriginal, ptr);
}

void* zoneMalloc(malloc_zone_t*, size_t size)
{
    count(Primitive::mallocCall);
    return gOriginal->malloc(gOriginal, size);
}

void* zoneCalloc(malloc_zone_t*, size_t items, size_t size)
{
    count(Primitive::callocCall);
    return gOriginal->calloc(gOriginal, items, size);
}

void* zoneValloc(malloc_zone_t*, size_t size)
{
    count(Primitive::mallocCall);
    return gOriginal->valloc(gOriginal, size);
}

void zoneFree(malloc_zone_t*, void* ptr)
{
    if (ptr != nullptr)
        countFree();
    gOriginal->free(gOriginal, ptr);
}

void* zoneRealloc(malloc_zone_t*, void* ptr, size_t size)
{
    count(Primitive::reallocCall);
    return gOriginal->realloc(gOriginal, ptr, size);
}

void zoneDestroy(malloc_zone_t*) {}

unsigned zoneBatchMalloc(malloc_zone_t*, size_t size, void** results, unsigned requested)
{
    count(Primitive::mallocCall, static_cast<long>(requested));
    return gOriginal->batch_malloc(gOriginal, size, results, requested);
}

void zoneBatchFree(malloc_zone_t*, void** blocks, unsigned n)
{
    long live = 0;
    for (unsigned i = 0; i < n; ++i)
        if (blocks[i] != nullptr)
            ++live;
    countFree(live);
    gOriginal->batch_free(gOriginal, blocks, n);
}

void* zoneMemalign(malloc_zone_t*, size_t alignment, size_t size)
{
    count(Primitive::mallocCall);
    return gOriginal->memalign(gOriginal, alignment, size);
}

void zoneFreeDefiniteSize(malloc_zone_t*, void* ptr, size_t size)
{
    if (ptr != nullptr)
        countFree();
    gOriginal->free_definite_size(gOriginal, ptr, size);
}

size_t zonePressureRelief(malloc_zone_t*, size_t goal)
{
    return gOriginal->pressure_relief(gOriginal, goal);
}

boolean_t zoneClaimedAddress(malloc_zone_t*, void* ptr)
{
    return gOriginal->claimed_address(gOriginal, ptr);
}

kern_return_t zoneEnumerator(task_t, void*, unsigned, vm_address_t, memory_reader_t, vm_range_recorder_t)
{
    return KERN_SUCCESS;
}

size_t zoneGoodSize(malloc_zone_t*, size_t size)
{
    return gOriginal->introspect->good_size(gOriginal, size);
}

boolean_t zoneCheck(malloc_zone_t*)
{
    return 1;
}

void zonePrint(malloc_zone_t*, boolean_t) {}

void zoneLog(malloc_zone_t*, void*) {}

void zoneLock(malloc_zone_t*) {}

boolean_t zoneLocked(malloc_zone_t*)
{
    return 0;
}

void zoneStatistics(malloc_zone_t*, malloc_statistics_t* stats)
{
    stats->blocks_in_use = 0;
    stats->size_in_use = 0;
    stats->max_size_in_use = 0;
    stats->size_allocated = 0;
}

void releaseThreadState(void* state)
{
    std::free(state);
}

malloc_zone_t gZone;
malloc_introspection_t gIntrospection;

bool installHooks()
{
    const char* blind = std::getenv("RINNE_TEST_ALLOC_BLIND");
    if (blind != nullptr && *blind != '\0')
    {
        if (std::strcmp(blind, "malloc") == 0)
            gBlind = Primitive::mallocCall;
        else if (std::strcmp(blind, "calloc") == 0)
            gBlind = Primitive::callocCall;
        else if (std::strcmp(blind, "realloc") == 0)
            gBlind = Primitive::reallocCall;
        else if (std::strcmp(blind, "free") == 0)
            gBlind = Primitive::freeCall;
        else
            return false; // 拼错的名字不能悄悄当成「没有致盲」
    }

    if (pthread_key_create(&gKey, &releaseThreadState) != 0)
        return false;

    gOriginal = malloc_default_zone();
    if (gOriginal == nullptr || gOriginal->introspect == nullptr || gOriginal->introspect->good_size == nullptr)
        return false;

    std::memset(&gIntrospection, 0, sizeof gIntrospection);
    gIntrospection.enumerator = &zoneEnumerator;
    gIntrospection.good_size = &zoneGoodSize;
    gIntrospection.check = &zoneCheck;
    gIntrospection.print = &zonePrint;
    gIntrospection.log = &zoneLog;
    gIntrospection.force_lock = &zoneLock;
    gIntrospection.force_unlock = &zoneLock;
    gIntrospection.statistics = &zoneStatistics;
    gIntrospection.zone_locked = &zoneLocked;
    gIntrospection.reinit_lock = &zoneLock;

    std::memset(&gZone, 0, sizeof gZone);
    gZone.size = &zoneSize;
    gZone.malloc = &zoneMalloc;
    gZone.calloc = &zoneCalloc;
    gZone.valloc = &zoneValloc;
    gZone.free = &zoneFree;
    gZone.realloc = &zoneRealloc;
    gZone.destroy = &zoneDestroy;
    gZone.zone_name = "RinneAllocationCounter";
    gZone.introspect = &gIntrospection;
    // 版本号最多声明到 10：再往上的槽（try_free_default 等）留空，libmalloc 按版本号决定用不用，空着就走通用路径
    gZone.version = std::min(gOriginal->version, 10u);
    if (gOriginal->batch_malloc != nullptr && gOriginal->batch_free != nullptr)
    {
        gZone.batch_malloc = &zoneBatchMalloc;
        gZone.batch_free = &zoneBatchFree;
    }
    if (gZone.version >= 5 && gOriginal->memalign != nullptr)
        gZone.memalign = &zoneMemalign;
    if (gZone.version >= 6 && gOriginal->free_definite_size != nullptr)
        gZone.free_definite_size = &zoneFreeDefiniteSize;
    if (gZone.version >= 8 && gOriginal->pressure_relief != nullptr)
        gZone.pressure_relief = &zonePressureRelief;
    if (gZone.version >= 10 && gOriginal->claimed_address != nullptr)
        gZone.claimed_address = &zoneClaimedAddress;

    // 先注册；再把排在前面的 zone 依次注销后重新注册（它们排到末尾），直到计数 zone 成为列表第一个，也就是 malloc()
    // 的默认 zone。 其余 zone 重新注册后仍然在册，free() 找得到它们。
    malloc_zone_register(&gZone);
    for (int i = 0; i < 8 && malloc_default_zone() != &gZone; ++i)
    {
        malloc_zone_t* front = malloc_default_zone();
        malloc_zone_unregister(front);
        malloc_zone_register(front);
    }
    return malloc_default_zone() == &gZone;
}

// 本线程第一次进作用域之前先建好计数结构；建的时候本线程还没有结构，钩子不计数
void ensureThreadState()
{
    if (threadState() != nullptr)
        return;
    void* state = std::calloc(1, sizeof(ThreadState));
    if (state == nullptr || pthread_setspecific(gKey, state) != 0)
        throw std::runtime_error("AllocationCounter: could not create the per-thread counter");
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
#elif defined(__APPLE__)
    static const bool installed = installHooks();
    if (!installed)
        throw std::runtime_error("AllocationCounter: could not install the counting malloc zone (or "
                                 "RINNE_TEST_ALLOC_BLIND names an unknown primitive)");
    ensureThreadState();
#endif
    before_ = threadAllocations();
    beforeFrees_ = threadFrees();
    enterScope();
}

AllocationScope::~AllocationScope()
{
    leaveScope();
}

long AllocationScope::allocations() const noexcept
{
    return threadAllocations() - before_;
}

long AllocationScope::frees() const noexcept
{
    return threadFrees() - beforeFrees_;
}

} // namespace rinne::test

#if !defined(_WIN32) && !defined(__APPLE__)

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
