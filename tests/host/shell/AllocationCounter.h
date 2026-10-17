// SPDX-License-Identifier: GPL-3.0-or-later
//
// 数堆分配次数的小工具，用来断言音频回调里不分配。AllocationCounter.cpp 替换了全局的 operator new / delete，
// 只统计当前线程在 AllocationScope 存活期间发生的分配；其余时候照常转给 malloc / free。
#pragma once

namespace rinne::test
{

class AllocationScope
{
public:
    AllocationScope() noexcept;
    ~AllocationScope();
    AllocationScope(const AllocationScope&) = delete;
    AllocationScope& operator=(const AllocationScope&) = delete;

    // 本作用域内、本线程已发生的 operator new 次数
    long allocations() const noexcept;

private:
    long before_;
};

} // namespace rinne::test
