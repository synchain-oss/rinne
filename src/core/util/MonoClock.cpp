// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/ipc.json

#include "util/MonoClock.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <time.h>
#if defined(__APPLE__) && !defined(CLOCK_MONOTONIC_RAW)
// Apple 上必须是这个时钟；宏缺失时宁可编不过，也不静默换成零点不同的另一个时钟
#error "CLOCK_MONOTONIC_RAW is required on Apple platforms"
#endif
#if !defined(CLOCK_MONOTONIC_RAW)
#include <chrono>
#endif
#endif

namespace rinne::util
{

std::uint64_t monoNowMs() noexcept
{
#if defined(_WIN32)
    return static_cast<std::uint64_t>(::GetTickCount64());
#elif defined(CLOCK_MONOTONIC_RAW)
    // 不设回落：同一进程里混进第二个零点不同的时钟，心跳差值就没有意义了。clock id 编译期已知且受支持，
    // 指针有效，EINVAL / EFAULT 都不会出现；万一失败就返回 0
    timespec ts{};
    if (::clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0)
    {
        return 0;
    }
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000u + static_cast<std::uint64_t>(ts.tv_nsec) / 1000000u;
#else
    // 只给没有 CLOCK_MONOTONIC_RAW 的 POSIX 编得过；Rinne 发布的平台都不会走到这里
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
#endif
}

} // namespace rinne::util
