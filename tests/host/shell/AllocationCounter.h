// SPDX-License-Identifier: GPL-3.0-or-later
//
// 数堆分配次数的小工具，用来断言音频回调里不分配。只统计当前线程在 AllocationScope 存活期间发生的分配。
// 要数到的不只是 operator new：JUCE 自己的分配原语（HeapBlock、AudioBuffer 的构造与 setSize、juce::Array
// 扩容、MemoryBlock） 走 malloc / calloc / realloc，那才是 JUCE 外壳里最常见的实时违例。
//   Windows（测试可执行文件静态链接 CRT）：malloc 家族最终都经本模块导入表里的 kernel32 的 HeapAlloc / HeapReAlloc
//     进系统堆，第一次建 AllocationScope 时把这两个导入项换成计数包装。只改测试可执行文件自己的导入表，不进插件。
//     operator new（含对齐版、nothrow）走 CRT 的 malloc，因此也在里面，不另替换。
//   其他平台：目前只替换了全局 operator new / delete（含对齐版），malloc 家族是盲区。mac / Linux 的 malloc 拦截留给
//     mac CI 那一轮（宿主测试在那些平台上还没有跑），在此之前，那里的「零分配」只等于「零 operator new」。
#pragma once

namespace rinne::test
{

class AllocationScope
{
public:
    // 第一次构造时安装计数钩子；装不上会抛 std::runtime_error，让测试变红，而不是悄悄什么都不数
    AllocationScope();
    ~AllocationScope();
    AllocationScope(const AllocationScope&) = delete;
    AllocationScope& operator=(const AllocationScope&) = delete;

    // 本作用域内、本线程已发生的分配次数
    long allocations() const noexcept;

private:
    long before_;
};

} // namespace rinne::test
