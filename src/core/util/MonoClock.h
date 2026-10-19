// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/ipc.json
//
// 跨进程可比的单调毫秒时钟。心跳、认领时刻、休眠时刻写进共享内存，由另一个进程拿自己的「现在」去减，
// 所以同一台机器上的所有进程必须读同一个全系统时钟：
//   Windows       GetTickCount64（开机以来的毫秒，全系统一个）
//   有 CLOCK_MONOTONIC_RAW 的 POSIX（macOS、Linux）  clock_gettime(CLOCK_MONOTONIC_RAW)
// 不经 std::chrono::steady_clock：它在 Apple 上落到哪个时钟由 libc++ 的版本决定，不能当跨进程契约。
// 只在非实时线程调用。音频线程的判断只看数据面的原子量与样本计数，不读任何时钟。
#pragma once

#include <cstdint>

namespace rinne::util
{

// 单调毫秒。系统调用失败时返回 0；读到 0 的一方一律按「陈旧」处理，不会因此误判对方还活着。
std::uint64_t monoNowMs() noexcept;

} // namespace rinne::util
