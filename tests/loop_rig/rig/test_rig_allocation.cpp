// SPDX-License-Identifier: GPL-3.0-or-later
//
// 音频线程不分配：试验台的一个周期（两端引擎的 process 加中间链）在稳态、违约块、超长块、0 样本、
// 起播沿、旁路、换写方时都不碰堆。
//
// 做法：本测试可执行文件里把全局 operator new / delete 换成「数一下再转给 malloc / free」的版本，
// 只在计数开着的线程上计数。全局替换只能有一处：同一个可执行文件里再有别的翻译单元替换分配器会链接冲突。
// 对齐版本不替换（它们走各自的默认实现，与这里的 malloc / free 不混用）；core 的音频路径不用对齐分配。
//
// TSan 的运行库自己定义了全局 operator new / delete，再替换会重复定义：TSan 构建下这组用例跳过。

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

#include "engine/EngineParams.h"
#include "rig/EngineHost.h"
#include "rig/Schedule.h"

#if defined(__SANITIZE_THREAD__)
#define RINNE_RIG_ALLOC_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define RINNE_RIG_ALLOC_TSAN 1
#endif
#endif

#ifndef RINNE_RIG_ALLOC_TSAN

namespace
{
thread_local bool t_counting = false;
thread_local long t_allocations = 0;
thread_local long t_frees = 0;

// 计数的作用域：构造时开、析构时关。只数本线程。
class AllocationCount
{
public:
    AllocationCount() noexcept
    {
        t_allocations = 0;
        t_frees = 0;
        t_counting = true;
    }
    ~AllocationCount() { t_counting = false; }
    AllocationCount(const AllocationCount&) = delete;
    AllocationCount& operator=(const AllocationCount&) = delete;

    long allocations() const noexcept { return t_allocations; }
    long frees() const noexcept { return t_frees; }
};
} // namespace

void* operator new(std::size_t size)
{
    if (t_counting)
    {
        ++t_allocations;
    }
    void* memory = std::malloc(size == 0u ? 1u : size);
    if (memory == nullptr)
    {
        throw std::bad_alloc{};
    }
    return memory;
}

void operator delete(void* memory) noexcept
{
    if (t_counting && memory != nullptr)
    {
        ++t_frees;
    }
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    ::operator delete(memory);
}

using namespace rinne::rig;

TEST_CASE("rig L3-03 音频线程不分配：一个周期的两端 process 与中间链在各种块上都不碰堆", "[rig][alloc]")
{
    // ⛔ 删除式验证点：在 InputEngine::process 或 OutputEngine::process 的热路径上 new 一次 → 本用例变红。
    EngineHost host;
    REQUIRE(host.open(1));
    host.prepare(48000.0, 512);

    // 稳态、违约块（1024 > 512）、超长块（40000 > 32768）、0 样本，两端各自的脚本。
    BlockScript input;
    input.lengths = {256, 256, 1024, 40000, 0, 256, 256, 256};
    BlockScript output;
    output.lengths = {256, 256, 256, 256, 0, 256, 256, 256};
    host.setBlockScripts(input, output); // 按脚本给缓冲：会分配，在计数之外

    TransportScript transport;
    transport.playing = {false, false, false, false, false, true, true, true}; // 第 6 周期起播
    transport.hostNs = {1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000};
    transport.hostNsValid = {true, true, true, true, true, true, true, true};
    transport.nonrealtime = {false, false, false, false, false, false, true, true};
    host.setTransportScript(transport);

    rinne::engine::EngineParams params;
    params.feedbackPercent = 80.0f;
    params.mixPercent = 50.0f;
    params.delayMs = 20.0f;
    ParamsScript script;
    script.snapshots = {params};
    host.setParamsScript(script);
    host.setChain(ChainKind::kOnePoleLowpass);

    REQUIRE(host.addSecondOutput()); // 起第二台在计数之外（prepare 会分配）
    host.setSecondOutputActive(true);

    host.runCycle(0); // 预热一周期（不计）

    // 计数只包住周期本身：读数先存进局部量，计数关了再断言（断言宏自己可能分配）。
    long allocations = -1;
    long frees = -1;
    {
        AllocationCount count;
        for (std::size_t i = 1; i < 8; ++i)
        {
            host.runCycle(i);
        }
        allocations = count.allocations();
        frees = count.frees();
    }
    CHECK(allocations == 0);
    CHECK(frees == 0);

    // 旁路与换写方（写权交接在消息线程做，不计；交接之后的周期计）。
    host.switchWriter(true);
    host.setOutputBypassed(true);
    host.setInputBypassed(true);
    {
        AllocationCount count;
        for (std::size_t i = 8; i < 16; ++i)
        {
            host.runCycle(i);
        }
        allocations = count.allocations();
        frees = count.frees();
    }
    CHECK(allocations == 0);
    CHECK(frees == 0);
}

TEST_CASE("rig 分配计数器自检：计数开着时 new 一次就数得到", "[rig][alloc]")
{
    // 证明计数器真的在数：否则上面那条用例永远是绿的。直接调分配函数（new 表达式可以被编译器整对省掉）。
    long allocations = -1;
    long frees = -1;
    {
        AllocationCount count;
        void* p = ::operator new(16);
        ::operator delete(p);
        allocations = count.allocations();
        frees = count.frees();
    }
    CHECK(allocations == 1);
    CHECK(frees == 1);
}

#endif // RINNE_RIG_ALLOC_TSAN
