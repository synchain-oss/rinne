// SPDX-License-Identifier: GPL-3.0-or-later
//
// 场景自注册：每个场景一个 .cpp，用宏把自己挂进注册表。
//
// 注册表是「函数指针 + id」的定长表，不进全局可变状态，避免静态初始化顺序问题。
// 场景文件放在 tests/loop_rig/scenarios/**（各自的归属另行登记），本模块只提供框架与一个占位场景。

#pragma once

#include <cstddef>
#include <cstdint>

#include "rig/EngineHost.h"

namespace rinne::rig
{

// 一个场景要跑的东西：主机、断言助手、场景自己的状态。
struct ScenarioContext
{
    EngineHost* host = nullptr;
    // 场景自己放数据的地方（用 void*，场景自己转型）
    void* state = nullptr;
};

// 场景的基类。
class Scenario
{
public:
    virtual ~Scenario() = default;
    // 场景的 id（例如 "s01"）
    virtual const char* id() const noexcept = 0;
    // 跑完并自检。返回 false 表示场景失败。
    virtual bool run(ScenarioContext& context) = 0;
};

// 注册表的一项。
struct ScenarioEntry
{
    const char* id = nullptr;
    Scenario* (*factory)() = nullptr;
};

// 自注册宏：静态对象的构造把自己挂进注册表。
//
// 用法：
//   class MyScenario final : public Scenario { ... };
//   RINNE_RIG_SCENARIO("s01")
//   {
//       return new MyScenario();
//   }
//
// 参数是场景 id 的字符串字面量。宏造出唯一的符号名：行号先展开再拼接（两层宏，直接 ## __LINE__ 拼出来的是
// 字面的 __LINE__，每个场景都会撞名），结构体放在无名命名空间里（每个翻译单元一份，多个场景文件链接时不撞符号）。
// 一个文件里每个场景占一行。
// 展开成一个结构体（声明工厂函数与注册器）+ 注册器的定义 + 工厂函数的类外定义头：
// 用户的那个块正好是工厂函数体，宏在它之前已经把结构体闭合了，所以拼起来是完整的定义。
#define RINNE_RIG_CONCAT_INNER(a, b) a##b
#define RINNE_RIG_CONCAT(a, b) RINNE_RIG_CONCAT_INNER(a, b)
#define RINNE_RIG_SCENARIO(idText) RINNE_RIG_SCENARIO_NAMED(idText, RINNE_RIG_CONCAT(rinneRigScenario_, __LINE__))
#define RINNE_RIG_SCENARIO_NAMED(idText, name)                          \
    namespace                                                           \
    {                                                                   \
    struct name                                                         \
    {                                                                   \
        static ::rinne::rig::Scenario* make();                          \
        static const ::rinne::rig::Registrar registrar;                 \
    };                                                                  \
    const ::rinne::rig::Registrar name::registrar(idText, &name::make); \
    }                                                                   \
    ::rinne::rig::Scenario* name::make()
// 注册器：构造时把自己挂进注册表。注册表是静态的，进程内一直活着（不释放，场景跑完就删）。
class Registrar
{
public:
    Registrar(const char* id, Scenario* (*factory)());
};

// 注册表：定长，满了就不加（并计数）。查不到就是 nullptr。
const ScenarioEntry* findScenario(const char* id) noexcept;
// 注册表里的场景数
std::size_t scenarioCount() noexcept;
// 遍历：按注册顺序给出第 index 项（越界返回 nullptr）
const ScenarioEntry* scenarioAt(std::size_t index) noexcept;

} // namespace rinne::rig
