// SPDX-License-Identifier: GPL-3.0-or-later
//
// 跨进程测试起子进程用的小工具：参数表、stdout+stderr 收进一个文件。Windows 走 CreateProcessW，显式传
// lpApplicationName 的绝对路径（绝不按名字搜索），只继承那一个输出文件句柄（PROC_THREAD_ATTRIBUTE_HANDLE_LIST），
// 子进程放在 KILL_ON_JOB_CLOSE 的作业对象里；POSIX 在 fork 之前算好 fd 上限与参数，子进程里只调用异步信号安全的
// 函数。wait 与 waitForLine 都有超时；output() 每次都重读整个文件，测试在失败信息里贴对端的原话。
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <filesystem>
#else
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace rinne::test
{

class Spawned
{
public:
    // 启动失败返回 nullptr（exe 打不开、句柄或进程创建失败）。args 不带 argv[0]，由 exe 自己补
    static std::unique_ptr<Spawned> start(const std::string& exe, const std::vector<std::string>& args,
                                          const std::string& outFile)
    {
#if defined(_WIN32)
        return startWindows(exe, args, outFile);
#else
        return startPosix(exe, args, outFile);
#endif
    }

    ~Spawned()
    {
        kill();
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
    Spawned(const Spawned&) = delete;
    Spawned& operator=(const Spawned&) = delete;

    std::uint32_t pid() const noexcept { return pid_; }

    bool running()
    {
        if (exited_)
        {
            return false;
        }
#if defined(_WIN32)
        if (process_ == nullptr)
        {
            exited_ = true;
            return false;
        }
        if (::WaitForSingleObject(process_, 0) == WAIT_TIMEOUT)
        {
            return true;
        }
        DWORD code = 0;
        ::GetExitCodeProcess(process_, &code);
        exitCode_ = static_cast<int>(code);
        exited_ = true;
        return false;
#else
        int status = 0;
        const pid_t reaped = ::waitpid(child_, &status, WNOHANG);
        if (reaped == child_)
        {
            exitCode_ = decodeStatus(status);
            exited_ = true;
            return false;
        }
        return true;
#endif
    }

    // 等退出。超时返回 false（子进程还活着，调用方自己决定 kill）；成功时 exitCode 是解码后的退出码
    bool wait(std::uint64_t timeoutMs, int& exitCode)
    {
        if (exited_)
        {
            exitCode = exitCode_;
            return true;
        }
#if defined(_WIN32)
        if (process_ == nullptr)
        {
            return false;
        }
        const DWORD timeout = timeoutMs > 0xffffffffull ? INFINITE : static_cast<DWORD>(timeoutMs);
        if (::WaitForSingleObject(process_, timeout) != WAIT_OBJECT_0)
        {
            return false;
        }
        DWORD code = 0;
        ::GetExitCodeProcess(process_, &code);
        exitCode_ = static_cast<int>(code);
        exited_ = true;
        exitCode = exitCode_;
        return true;
#else
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;)
        {
            int status = 0;
            const pid_t reaped = ::waitpid(child_, &status, WNOHANG);
            if (reaped == child_)
            {
                exitCode_ = decodeStatus(status);
                exited_ = true;
                exitCode = exitCode_;
                return true;
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
#endif
    }

    // 硬杀并收尸；已经退出时什么都不做
    void kill()
    {
        if (exited_)
        {
            return;
        }
#if defined(_WIN32)
        if (process_ != nullptr)
        {
            ::TerminateProcess(process_, 1);
            ::WaitForSingleObject(process_, INFINITE);
        }
        exited_ = true;
#else
        if (child_ > 0)
        {
            ::kill(child_, SIGKILL);
            int status = 0;
            while (::waitpid(child_, &status, 0) < 0 && errno == EINTR)
            {
            }
        }
        exited_ = true;
#endif
    }

    // 每次重读整个文件：子进程每行都 flush，测试轮询它
    std::string output() const
    {
#if defined(_WIN32)
        std::ifstream file(std::filesystem::path(widen(outFile_)));
#else
        std::ifstream file(outFile_);
#endif
        if (!file)
        {
            return {};
        }
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    // 每 20 ms 轮询一次，等某行出现（子串匹配）；超时返回 false
    bool waitForLine(const std::string& needle, std::uint64_t timeoutMs) const
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;)
        {
            if (output().find(needle) != std::string::npos)
            {
                return true;
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

private:
    Spawned() = default;

#if defined(_WIN32)
    static std::wstring widen(const std::string& text)
    {
        if (text.empty())
        {
            return {};
        }
        const int length = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (length <= 0)
        {
            return {};
        }
        std::wstring wide(static_cast<std::size_t>(length), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
        return wide;
    }

    // CommandLineToArgvW 的反向规则：参数用双引号包起来，内部的引号与结尾的反斜杠按规则加倍
    static std::wstring quote(const std::wstring& arg)
    {
        if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        {
            return arg;
        }
        std::wstring out = L"\"";
        for (auto it = arg.begin();; ++it)
        {
            std::size_t backslashes = 0;
            while (it != arg.end() && *it == L'\\')
            {
                ++it;
                ++backslashes;
            }
            if (it == arg.end())
            {
                out.append(backslashes * 2, L'\\');
                break;
            }
            if (*it == L'"')
            {
                out.append(backslashes * 2 + 1, L'\\');
                out.push_back(L'"');
            }
            else
            {
                out.append(backslashes, L'\\');
                out.push_back(*it);
            }
        }
        out.push_back(L'"');
        return out;
    }

    static std::unique_ptr<Spawned> startWindows(const std::string& exe, const std::vector<std::string>& args,
                                                 const std::string& outFile)
    {
        const std::wstring wideExe = widen(exe);
        if (wideExe.empty())
        {
            return nullptr;
        }
        std::wstring command = quote(wideExe);
        for (const std::string& arg : args)
        {
            command += L' ';
            command += quote(widen(arg));
        }

        SECURITY_ATTRIBUTES security{};
        security.nLength = sizeof(security);
        security.bInheritHandle = TRUE;
        const HANDLE file = ::CreateFileW(widen(outFile).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                          &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return nullptr;
        }

        SIZE_T attributeBytes = 0;
        ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
        if (attributeBytes == 0)
        {
            ::CloseHandle(file);
            return nullptr;
        }
        std::vector<unsigned char> attributeStorage(attributeBytes);
        auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
        if (!::InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes))
        {
            ::CloseHandle(file);
            return nullptr;
        }
        HANDLE inherited[] = {file};
        const BOOL listed = ::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
                                                        sizeof(inherited), nullptr, nullptr);

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = nullptr;
        startup.StartupInfo.hStdOutput = file;
        startup.StartupInfo.hStdError = file;
        startup.lpAttributeList = attributes;

        PROCESS_INFORMATION process{};
        const BOOL created =
            listed != FALSE && ::CreateProcessW(wideExe.c_str(), command.data(), nullptr, nullptr, TRUE,
                                                EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr,
                                                &startup.StartupInfo, &process) != FALSE;
        ::DeleteProcThreadAttributeList(attributes);
        ::CloseHandle(file);
        if (!created)
        {
            return nullptr;
        }

        auto spawned = std::unique_ptr<Spawned>(new Spawned());
        spawned->pid_ = static_cast<std::uint32_t>(process.dwProcessId);
        spawned->process_ = process.hProcess;
        spawned->outFile_ = outFile;
        spawned->job_ = ::CreateJobObjectW(nullptr, nullptr);
        if (spawned->job_ != nullptr)
        {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            ::SetInformationJobObject(spawned->job_, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
            ::AssignProcessToJobObject(spawned->job_, process.hProcess); // 尽力而为：失败只靠 TerminateProcess
        }
        ::CloseHandle(process.hThread);
        return spawned;
    }
#else
    static int decodeStatus(int status)
    {
        if (WIFEXITED(status))
        {
            return WEXITSTATUS(status);
        }
        if (WIFSIGNALED(status))
        {
            return 128 + WTERMSIG(status);
        }
        return 1;
    }

    static std::unique_ptr<Spawned> startPosix(const std::string& exe, const std::vector<std::string>& args,
                                               const std::string& outFile)
    {
        long maxFd = ::sysconf(_SC_OPEN_MAX);
        if (maxFd <= 0 || maxFd > 65536)
        {
            maxFd = 65536;
        }

        // fork 之前把 execv 需要的一切都备好：子进程里不再分配、不再碰非异步信号安全的调用
        std::vector<std::string> storage;
        storage.reserve(args.size() + 1);
        storage.push_back(exe);
        for (const std::string& arg : args)
        {
            storage.push_back(arg);
        }
        std::vector<char*> argv;
        argv.reserve(storage.size() + 1);
        for (std::string& item : storage)
        {
            argv.push_back(item.data());
        }
        argv.push_back(nullptr);
        const std::string outputPath = outFile;

        const pid_t child = ::fork();
        if (child < 0)
        {
            return nullptr;
        }
        if (child == 0)
        {
            const int fd = ::open(outputPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (fd >= 0)
            {
                ::dup2(fd, STDOUT_FILENO);
                ::dup2(fd, STDERR_FILENO);
                if (fd > STDERR_FILENO)
                {
                    ::close(fd);
                }
            }
            for (int other = 3; other < static_cast<int>(maxFd); ++other)
            {
                ::close(other);
            }
            ::execv(argv[0], argv.data());
            ::_exit(127);
        }

        auto spawned = std::unique_ptr<Spawned>(new Spawned());
        spawned->pid_ = static_cast<std::uint32_t>(child);
        spawned->child_ = child;
        spawned->outFile_ = outFile;
        return spawned;
    }
#endif

    std::uint32_t pid_ = 0;
    std::string outFile_;
    int exitCode_ = 0;
    bool exited_ = false;
#if defined(_WIN32)
    HANDLE process_ = nullptr;
    HANDLE job_ = nullptr;
#else
    pid_t child_ = -1;
#endif
};

} // namespace rinne::test
