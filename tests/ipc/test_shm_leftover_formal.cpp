// SPDX-License-Identifier: GPL-3.0-or-later
//
// 残段检查（正式作用域）：按名模块生成正式族的全部段名（rv 与 g1..g8），逐个只读探一遍，一个都不许存在。
// 这条用例永远不建任何东西，mac CI 的 shm-leftover.sh 在验证前后各跑一次 `rinne_ipc_tests "[shm-leftover]"`。
//   Windows  OpenFileMappingW 失败且错误是 ERROR_FILE_NOT_FOUND 才算「不存在」；其它错误按存在处理（宁可判红，
//            不能把权限问题当成干净）。
//   macOS    shm_open(O_RDONLY) 失败且 errno == ENOENT 才算不存在；其它 errno 按存在处理。
//   Linux    PlatformSegmentBackend 是进程内后端，正式作用域从不创建 POSIX 共享内存段：没有可探的对象，
//            用例按「不适用」通过。
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

#include "ipc/Naming.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace
{

using namespace rinne::ipc;

#if defined(_WIN32)
std::wstring widen(const std::string& text)
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

bool segmentExists(const std::string& name)
{
    const std::wstring wide = widen(name);
    const HANDLE handle = ::OpenFileMappingW(FILE_MAP_READ, FALSE, wide.c_str());
    if (handle != nullptr)
    {
        ::CloseHandle(handle);
        return true;
    }
    return ::GetLastError() != ERROR_FILE_NOT_FOUND;
}
#elif defined(__APPLE__)
bool segmentExists(const std::string& name)
{
    const int fd = ::shm_open(name.c_str(), O_RDONLY, 0);
    if (fd >= 0)
    {
        ::close(fd);
        return true;
    }
    return errno != ENOENT;
}
#endif

} // namespace

TEST_CASE("the formal scope leaves no shared memory behind", "[shm-leftover]")
{
#if defined(_WIN32) || defined(__APPLE__)
    const Naming naming(NameScope::formal());
    int leftovers = 0;
    for (u32 group = 0; group <= kGroupCount; ++group)
    {
        std::string name;
        REQUIRE(naming.name(SegmentId{group}, name) == NameStatus::kOk);
        if (segmentExists(name))
        {
            ++leftovers;
            UNSCOPED_INFO("leftover segment: " << name);
        }
    }
    CHECK(leftovers == 0);
#else
    // 进程内平台没有操作系统级的段可探：正式作用域的名字只在这个进程的映射表里用，套件里也没有别的用例建它
    SUCCEED("in-process platform: the formal scope never creates OS-level segments");
#endif
}
