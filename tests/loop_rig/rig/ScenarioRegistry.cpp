// SPDX-License-Identifier: GPL-3.0-or-later
//
// 场景注册表的实现。表是定长的静态数组，不在运行时增长。
//
// 为什么定长：注册发生在静态初始化期，表要是可增长的容器，就得处理「静态初始化顺序」：
// 一个场景的静态对象先跑还是表先建，不确定。定长表没有这个问题——它有静态存储期，
// 零初始化一定先于任何动态初始化。

#include "rig/ScenarioRegistry.h"

#include <cstring>

namespace rinne::rig
{

namespace
{
// 注册表最多能装多少个场景。超出的登记被丢掉并计数（不会静默丢一个场景：计数能看出来）。
constexpr std::size_t kMaxScenarios = 64;

ScenarioEntry g_entries[kMaxScenarios]{};
std::size_t g_count = 0;
std::size_t g_overflow = 0;
} // namespace

Registrar::Registrar(const char* id, Scenario* (*factory)())
{
    if (g_count >= kMaxScenarios)
    {
        ++g_overflow;
        return;
    }
    g_entries[g_count].id = id;
    g_entries[g_count].factory = factory;
    ++g_count;
}

const ScenarioEntry* findScenario(const char* id) noexcept
{
    if (id == nullptr)
    {
        return nullptr;
    }
    for (std::size_t i = 0; i < g_count; ++i)
    {
        if (g_entries[i].id != nullptr && std::strcmp(g_entries[i].id, id) == 0)
        {
            return &g_entries[i];
        }
    }
    return nullptr;
}

std::size_t scenarioCount() noexcept
{
    return g_count;
}

const ScenarioEntry* scenarioAt(std::size_t index) noexcept
{
    return index < g_count ? &g_entries[index] : nullptr;
}

} // namespace rinne::rig
