// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/ipc.json

#include "ipc/ProcessInfo.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#else
#include <cerrno>
#include <climits>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace rinne::ipc
{

std::uint32_t currentProcessId() noexcept
{
#if defined(_WIN32)
    return static_cast<std::uint32_t>(::GetCurrentProcessId());
#else
    return static_cast<std::uint32_t>(::getpid());
#endif
}

std::uint32_t parentProcessId() noexcept
{
#if defined(_WIN32)
    const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        return 0;
    }
    const DWORD self = ::GetCurrentProcessId();
    std::uint32_t parent = 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = ::Process32FirstW(snapshot, &entry); ok != FALSE; ok = ::Process32NextW(snapshot, &entry))
    {
        if (entry.th32ProcessID == self)
        {
            parent = static_cast<std::uint32_t>(entry.th32ParentProcessID);
            break;
        }
    }
    ::CloseHandle(snapshot);
    return parent;
#else
    return static_cast<std::uint32_t>(::getppid());
#endif
}

std::uint32_t currentUserId() noexcept
{
#if defined(_WIN32)
    return 0;
#else
    return static_cast<std::uint32_t>(::geteuid());
#endif
}

bool isProcessAlive(std::uint32_t pid) noexcept
{
    if (pid == 0)
    {
        return false;
    }
#if defined(_WIN32)
    const HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (h == nullptr)
    {
        // 没有这个 pid 时 OpenProcess 报 ERROR_INVALID_PARAMETER；别的失败（多半是拒绝访问）说明进程在
        return ::GetLastError() != ERROR_INVALID_PARAMETER;
    }
    DWORD code = 0;
    const bool queried = ::GetExitCodeProcess(h, &code) != FALSE;
    ::CloseHandle(h);
    // 查不到退出码时同样按活处理；拿到了就看它是否还在运行
    return !queried || code == STILL_ACTIVE;
#else
    if (pid > static_cast<std::uint32_t>(INT_MAX))
    {
        return false;
    }
    // 信号 0 只做存在性与权限检查，不真的发信号
    if (::kill(static_cast<pid_t>(pid), 0) == 0)
    {
        return true;
    }
    return errno == EPERM;
#endif
}

} // namespace rinne::ipc
