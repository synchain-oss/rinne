// SPDX-License-Identifier: GPL-3.0-or-later
//
// 压测时长的放大系数。ctest 里的默认规模只有几秒（每条 CI 都要跑，TSan 与 ASan 下还要再慢十几倍）；本机 Full 档与
// 手动长跑把环境变量 RINNE_STRESS_SCALE 设成 1..600 的整数，把每一轮的最短时长与封顶时长乘上它，同一批用例跑得更久、
// 撞上更多交错。非整数、越界或没设都按 1。只有测试读环境变量。
// 变异构建（StressMutation.h）一律按 1：它的取证靠命中数与自己的封顶，不能被放大系数改变时长。
#pragma once

#include <cstdint>
#include <cstdlib>

#include "StressMutation.h"

namespace rinne::stress
{

inline constexpr unsigned kMaxScale = 600;

// 把 RINNE_STRESS_SCALE 的文本解析成放大系数；空、非整数、0 或超过 kMaxScale 都是 1
inline unsigned parseScale(const char* text) noexcept
{
    if (text == nullptr || text[0] == '\0')
    {
        return 1;
    }
    unsigned long long value = 0;
    for (const char* p = text; *p != '\0'; ++p)
    {
        if (*p < '0' || *p > '9')
        {
            return 1;
        }
        value = value * 10 + static_cast<unsigned long long>(*p - '0');
        if (value > kMaxScale)
        {
            return 1;
        }
    }
    return value == 0 ? 1u : static_cast<unsigned>(value);
}

inline unsigned scale() noexcept
{
    static const unsigned value = [] {
#ifdef _MSC_VER
        // MSVC 把 getenv 标成不安全（C4996，告警即错误），用它的替代品
        char* text = nullptr;
        std::size_t length = 0;
        const unsigned parsed = _dupenv_s(&text, &length, "RINNE_STRESS_SCALE") == 0 ? parseScale(text) : 1u;
        std::free(text);
        return parsed;
#else
        return parseScale(std::getenv("RINNE_STRESS_SCALE"));
#endif
    }();
    return kMutated ? 1u : value;
}

// 毫秒数乘放大系数
inline std::uint64_t scaledMs(std::uint64_t ms) noexcept
{
    return ms * scale();
}

} // namespace rinne::stress
