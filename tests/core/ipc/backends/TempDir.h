// SPDX-License-Identifier: GPL-3.0-or-later
//
// POSIX 单测用的临时锁目录：本进程用 mkdtemp 在系统临时目录下新建，用完删掉。
// 删除只针对自己建的这一个目录，删之前再核一遍：真实路径的上一级就是系统临时目录、目录名以本类的前缀开头，
// 否则一个文件都不删（只会留下一个空的临时目录）。
#pragma once

#if !defined(_WIN32)

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

namespace rinne::test
{

class TempDir
{
public:
    static constexpr const char* kPrefix = "rinne-ipc-test-";

    TempDir()
    {
        std::string templ = (std::filesystem::temp_directory_path() / (std::string(kPrefix) + "XXXXXX")).string();
        REQUIRE(::mkdtemp(templ.data()) != nullptr);
        path_ = templ;
    }
    ~TempDir()
    {
        if (!safeToRemove(path_))
        {
            return;
        }
        std::error_code ec;
        std::filesystem::permissions(path_, std::filesystem::perms::owner_all, ec);
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::string& path() const noexcept { return path_; }

    // 纯函数：只有「系统临时目录下、以本类前缀开头的那一级目录」才许删
    static bool safeToRemove(const std::string& path)
    {
        std::error_code ec;
        const auto real = std::filesystem::canonical(path, ec);
        if (ec)
        {
            return false;
        }
        const auto tmp = std::filesystem::canonical(std::filesystem::temp_directory_path(), ec);
        if (ec)
        {
            return false;
        }
        const std::string name = real.filename().string();
        return real.parent_path() == tmp && name.size() > std::char_traits<char>::length(kPrefix) &&
               name.compare(0, std::char_traits<char>::length(kPrefix), kPrefix) == 0;
    }

private:
    std::string path_;
};

} // namespace rinne::test

#endif
