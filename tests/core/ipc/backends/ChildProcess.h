// SPDX-License-Identifier: GPL-3.0-or-later
//
// 测试用的外进程：起一个只会睡着的子进程，给「活着的外进程」一类用例提供一个真实的 pid。析构时一定杀掉并收尸，
// 用例中途失败也不会把它留下；测试进程自己被强杀时，Windows 上它在作业对象里随句柄关闭一起结束，POSIX 上它
// 30 秒后自己退出。
//
// Windows 上按 System32 的绝对路径起 ping：按相对名起会先搜 exe 所在目录与当前目录，那里放一个同名程序就会被
// 当成子进程跑起来。POSIX 上子进程是 fork 出来的、不 exec，一出生就关掉 3 以上的全部 fd：flock 锁跟着打开的文件
// 描述走，子进程留着继承来的 fd，父进程解锁、关闭之后锁仍然被它握着，会让 POSIX 后端的用例看到一把不该在的锁。
#pragma once

#include <cstdint>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>
#else
#include <cerrno>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace rinne::test
{

class ChildProcess
{
public:
    ChildProcess() noexcept { start(); }
    ~ChildProcess()
    {
        stop();
#if defined(_WIN32)
        if (process_ != nullptr)
        {
            ::CloseHandle(process_);
        }
        if (job_ != nullptr)
        {
            ::CloseHandle(job_);
        }
#endif
    }
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    bool started() const noexcept { return pid_ != 0; }
    std::uint32_t pid() const noexcept { return pid_; }

    // 杀掉并等它退出。之后 pid 指向一个已退出的进程：Windows 上进程句柄留到析构，进程对象还在，pid 不会被复用；
    // POSIX 上已收尸
    void stop() noexcept
    {
        if (!running_)
        {
            return;
        }
        running_ = false;
#if defined(_WIN32)
        ::TerminateProcess(process_, 0);
        ::WaitForSingleObject(process_, 10000);
#else
        ::kill(child_, SIGKILL);
        int status = 0;
        while (::waitpid(child_, &status, 0) < 0 && errno == EINTR)
        {
        }
#endif
    }

private:
    void start() noexcept
    {
#if defined(_WIN32)
        job_ = ::CreateJobObjectW(nullptr, nullptr);
        if (job_ != nullptr)
        {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            ::SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        }
        // 直接起 ping（不经 cmd）：杀掉的就是 pid 指的那个进程，不会留下孙进程。程序名给绝对路径，不走搜索
        wchar_t systemDir[MAX_PATH];
        const UINT length = ::GetSystemDirectoryW(systemDir, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
        {
            return;
        }
        const std::wstring application = std::wstring(systemDir, length) + L"\\PING.EXE";
        std::wstring command = L"\"" + application + L"\" -n 31 127.0.0.1";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (::CreateProcessW(application.c_str(), &command[0], nullptr, nullptr, FALSE,
                             CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi) == FALSE)
        {
            return;
        }
        if (job_ != nullptr)
        {
            ::AssignProcessToJobObject(job_, pi.hProcess); // 尽力而为：失败时只靠析构里的 TerminateProcess
        }
        ::ResumeThread(pi.hThread);
        ::CloseHandle(pi.hThread);
        process_ = pi.hProcess;
        pid_ = static_cast<std::uint32_t>(pi.dwProcessId);
#else
        // 要关到哪个 fd 在 fork 之前算好：子进程里只许异步信号安全的调用，sysconf 不在其列
        long maxFd = ::sysconf(_SC_OPEN_MAX);
        if (maxFd <= 0 || maxFd > 65536)
        {
            maxFd = 65536;
        }
        const pid_t child = ::fork();
        if (child < 0)
        {
            return;
        }
        if (child == 0)
        {
            // 子进程里只用异步信号安全的调用
            for (int fd = 3; fd < static_cast<int>(maxFd); ++fd)
            {
                ::close(fd);
            }
            ::alarm(30);
            for (;;)
            {
                ::pause();
            }
        }
        child_ = child;
        pid_ = static_cast<std::uint32_t>(child);
#endif
        running_ = true;
    }

    std::uint32_t pid_ = 0;
    bool running_ = false;
#if defined(_WIN32)
    HANDLE process_ = nullptr;
    HANDLE job_ = nullptr;
#else
    pid_t child_ = -1;
#endif
};

} // namespace rinne::test
