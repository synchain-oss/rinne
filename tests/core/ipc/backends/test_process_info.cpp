// SPDX-License-Identifier: GPL-3.0-or-later
//
// 进程标识、存活探测、跨进程时钟与四字符码。存活探测的代价不对称（把活进程判死会造成双属主），
// 所以既要验「死了就判死」，也要验越界的 pid 不会被当成「所有进程」。

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstring>
#include <thread>

#include "ipc/ProcessInfo.h"
#include "util/Fourcc.h"
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
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace rinne;

TEST_CASE("this process is alive and pid 0 or an out-of-range pid is not", "[ipc][process]")
{
    const std::uint32_t self = ipc::currentProcessId();
    CHECK(self != 0u);
    CHECK(ipc::isProcessAlive(self));
    CHECK_FALSE(ipc::isProcessAlive(0));
    CHECK_FALSE(ipc::isProcessAlive(0xffffffffu));
    CHECK(ipc::parentProcessId() != 0u);
    CHECK(ipc::parentProcessId() != self);
}

TEST_CASE("a process that has exited is reported dead", "[ipc][process]")
{
#if defined(_WIN32)
    // 句柄还开着时进程对象还在：OpenProcess 能成功，退出码不是 STILL_ACTIVE，照样判死
    wchar_t command[] = L"cmd.exe /c exit 0";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    REQUIRE(::CreateProcessW(nullptr, command, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi) !=
            FALSE);
    REQUIRE(::WaitForSingleObject(pi.hProcess, 10000) == WAIT_OBJECT_0);
    CHECK_FALSE(ipc::isProcessAlive(static_cast<std::uint32_t>(pi.dwProcessId)));
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
#else
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0)
    {
        ::_exit(0);
    }
    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    CHECK_FALSE(ipc::isProcessAlive(static_cast<std::uint32_t>(child)));
#endif
}

TEST_CASE("the cross-process clock is monotonic and in milliseconds", "[ipc][clock]")
{
    const std::uint64_t a = util::monoNowMs();
    CHECK(a != 0u);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const std::uint64_t b = util::monoNowMs();
    CHECK(b >= a + 20u);
    CHECK(b < a + 5000u);
}

TEST_CASE("fourcc reads as its four letters in memory", "[util][fourcc]")
{
    constexpr std::uint32_t code = util::fourcc("RNRV");
    static_assert(code == util::fourcc('R', 'N', 'R', 'V'));
    unsigned char bytes[4] = {};
    std::memcpy(bytes, &code, 4);
    // 两端同机同字节序；发布的平台都是小端，段内前 4 字节读出来就是这四个字母
    CHECK(bytes[0] == 'R');
    CHECK(bytes[1] == 'N');
    CHECK(bytes[2] == 'R');
    CHECK(bytes[3] == 'V');
}
