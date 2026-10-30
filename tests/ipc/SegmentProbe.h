// SPDX-License-Identifier: GPL-3.0-or-later
//
// 共享内存段的只读探针：问某个名字的段此刻还在不在。只探，从不建、从不写、从不删。
//   Windows  OpenFileMappingW 失败且错误是 ERROR_FILE_NOT_FOUND 才算「不存在」；其它错误按存在处理（宁可判红，
//            不能把权限问题当成干净）。
//   macOS    shm_open(O_RDONLY) 失败且 errno == ENOENT 才算不存在；其它 errno 按存在处理。
//   Linux    PlatformSegmentBackend 是进程内后端，从不创建操作系统级的段：没有可探的对象，一律「不存在」。
// 正式作用域的残段检查（test_shm_leftover_formal.cpp）与跨进程用例结束时的测试作用域残段检查共用这一份。
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

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

namespace rinne::test
{

#if defined(_WIN32)
inline std::wstring widenUtf8(const std::string& text)
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

inline bool segmentExists(const std::string& name)
{
    const std::wstring wide = widenUtf8(name);
    const HANDLE handle = ::OpenFileMappingW(FILE_MAP_READ, FALSE, wide.c_str());
    if (handle != nullptr)
    {
        ::CloseHandle(handle);
        return true;
    }
    return ::GetLastError() != ERROR_FILE_NOT_FOUND;
}
#elif defined(__APPLE__)
inline bool segmentExists(const std::string& name)
{
    const int fd = ::shm_open(name.c_str(), O_RDONLY, 0);
    if (fd >= 0)
    {
        ::close(fd);
        return true;
    }
    return errno != ENOENT;
}
#else
inline bool segmentExists(const std::string&)
{
    return false;
}
#endif

// 这个命名下全部段名（会合段与组 1..8 的数据段）里此刻还存在的，按段号从小到大
inline std::vector<std::string> existingSegments(const rinne::ipc::Naming& naming)
{
    std::vector<std::string> found;
    for (rinne::ipc::u32 group = 0; group <= rinne::ipc::kGroupCount; ++group)
    {
        std::string name;
        REQUIRE(naming.name(rinne::ipc::SegmentId{group}, name) == rinne::ipc::NameStatus::kOk);
        if (segmentExists(name))
        {
            found.push_back(name);
        }
    }
    return found;
}

// 把名字列表拼成一行，放进失败信息里
inline std::string joinNames(const std::vector<std::string>& names)
{
    std::string text;
    for (const std::string& name : names)
    {
        text += (text.empty() ? "" : ", ") + name;
    }
    return text;
}

} // namespace rinne::test
