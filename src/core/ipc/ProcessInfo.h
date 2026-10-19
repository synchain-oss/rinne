// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/ipc.json
//
// 本进程的标识与对端进程的存活探测。端点槽里记着属主的 pid；跨进程接管的条件之一是「对端 pid 已死」，
// 探活判错的代价不对称：把活进程判成死，会让两个属主同时写一个环；把死进程判成活，只是这个槽不会被自动接管。
// 所以凡是拿不准的情形（无权限打开、pid 越界以外的错误）一律判「活」。
// 已知限制：死进程的 pid 被别的进程复用之后，探活一直返回「活」，那个活跃槽就一直被占着。用户确认只对休眠槽生效，
// 目前没有拿走这种活跃槽的手动出口；要换到另一个组才能配对。
// 全部是系统调用：只在非实时线程调用，音频线程从不经过这里。
#pragma once

#include <cstdint>

namespace rinne::ipc
{

// 本进程 pid。Windows = GetCurrentProcessId()，POSIX = getpid()；写进端点槽与探活用的是同一个口径。
std::uint32_t currentProcessId() noexcept;

// 父进程 pid，取不到时为 0。POSIX = getppid()；Windows 在进程快照里找本进程那一项。只供诊断与预留字段，
// 配对规则不看它。Windows 上要遍历进程快照，调用方取一次就缓存起来。
std::uint32_t parentProcessId() noexcept;

// 有效用户 id：POSIX 段名里带它（8 位小写十六进制），同一台机器上不同用户的段互不相见。Windows 恒为 0
// （Local\ 命名空间本来就按登录会话隔开）。
std::uint32_t currentUserId() noexcept;

// pid 是否还活着。pid == 0 恒判死。
//   Windows  OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION) 后看退出码是否 STILL_ACTIVE；打不开时只有
//            ERROR_INVALID_PARAMETER（没有这个 pid）判死，拒绝访问之类的判活：进程在，只是不让我们看。
//   POSIX    kill(pid, 0) 成功，或失败但 errno == EPERM（进程在，属于别的用户）判活；ESRCH 判死。
//            pid 超出 pid_t 的正数范围直接判死：转成负数交给 kill 会变成按进程组探测，(u32)-1 → kill(-1, 0)
//            对「所有能发信号的进程」恒成功。
bool isProcessAlive(std::uint32_t pid) noexcept;

} // namespace rinne::ipc
