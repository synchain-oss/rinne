// SPDX-License-Identifier: GPL-3.0-or-later
//
// 冒烟：构建信息与 CMake 配置一致。这些常量由根 CMakeLists.txt 生成进 RinneVersion.h，段名前缀决定本机测试碰不碰得到
// DAW 里装的正式版，所以三个允许值和探针开关的配对在这里再钉一次。

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>

#include "util/RinneVersion.h"

namespace
{

bool endsWith(const std::string& text, const std::string& tail)
{
    return text.size() >= tail.size() && text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
}

} // namespace

TEST_CASE("version constants agree with each other", "[smoke][build-info]")
{
    using namespace rinne::build;
    const std::string joined =
        std::to_string(kVersionMajor) + "." + std::to_string(kVersionMinor) + "." + std::to_string(kVersionPatch);
    CHECK(joined == kVersion);
    CHECK(kVersionMajor >= 0);
}

TEST_CASE("shared-memory prefix is one of the three allowed values", "[smoke][build-info]")
{
    using namespace rinne::build;
    const std::string prefix = kShmPrefix;
    const std::string formal = "SynchainRinne";
    const bool allowed = prefix == formal || prefix == formal + "D" || prefix == formal + "P";
    CHECK(allowed);
    // 探针前缀与探针构建必须同时出现，否则探针会和正式版在同一个宿主里互相配对
    CHECK(endsWith(prefix, "P") == kProbeBuild);
}

TEST_CASE("build label is a well-formed string", "[smoke][build-info]")
{
    const char* label = rinne::build::buildLabel();
    REQUIRE(label != nullptr);
    for (const char* c = label; *c != '\0'; ++c)
    {
        const bool ok = (*c >= '0' && *c <= '9') || (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || *c == '.' ||
                        *c == '+' || *c == '-';
        CHECK(ok);
    }
}
