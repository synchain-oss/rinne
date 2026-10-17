// SPDX-License-Identifier: GPL-3.0-or-later
//
// 冒烟：配置要求的 sanitizer 真的编进了测试。期望由 tests/smoke/suite.cmake 按 RINNE_SANITIZE 传入。
// 没有 sanitizer 的配置里这些宏都不定义，用例照常通过。

#include <catch2/catch_test_macros.hpp>

namespace
{

[[maybe_unused]] constexpr bool addressSanitizerActive()
{
#if defined(__SANITIZE_ADDRESS__)
    return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
    return true;
#else
    return false;
#endif
#else
    return false;
#endif
}

[[maybe_unused]] constexpr bool threadSanitizerActive()
{
#if defined(__SANITIZE_THREAD__)
    return true;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
    return true;
#else
    return false;
#endif
#else
    return false;
#endif
}

} // namespace

TEST_CASE("the requested sanitizer is compiled into the tests", "[smoke][sanitizer]")
{
#if defined(RINNE_EXPECT_ASAN)
    CHECK(addressSanitizerActive());
#endif
#if defined(RINNE_EXPECT_TSAN)
    CHECK(threadSanitizerActive());
#endif
#if !defined(RINNE_EXPECT_ASAN) && !defined(RINNE_EXPECT_TSAN)
    // 没有要求 sanitizer 的配置里也不应当有：混进来说明选项泄漏到了别的配置
    CHECK_FALSE(addressSanitizerActive());
    CHECK_FALSE(threadSanitizerActive());
#endif
}
