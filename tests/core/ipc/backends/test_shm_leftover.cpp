// SPDX-License-Identifier: GPL-3.0-or-later
//
// 共享内存段不残留：正常释放（unmap）之后、以及持有者没有释放就退出之后，系统里都不留本测试作用域的段；
//   Windows  段随最后一个句柄关闭由内核回收：子进程建段后不释放就退出，父进程再打开必然失败。
//   POSIX    名字会留到有人 shm_unlink：持有者崩溃后名字还在，下一个建段者清掉残段重建，最后一个离开者撤名字，
//            之后 shm_open 必然失败。
// 标签 [shm-leftover] 与 mac CI 的残段检查同名；那个检查跑的是跨进程套件里按正式作用域探残段的同标签用例。

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "ipc/ISegmentBackend.h"
#include "ipc/Naming.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "ipc/SegmentBackendWin32.h"
#else
#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ipc/SegmentBackendPosix.h"
#endif

namespace
{

using namespace rinne::ipc;

std::vector<std::string> allNames(const Naming& naming)
{
    std::vector<std::string> names;
    for (std::uint32_t g = 0; g <= kGroupCount; ++g)
    {
        std::string name;
        REQUIRE(naming.name(SegmentId{g}, name) == NameStatus::kOk);
        names.push_back(name);
    }
    return names;
}

std::size_t sizeOf(std::uint32_t group)
{
    return group == 0 ? kRvBytes : kRingBytes;
}

// 建一个段并完成段头初始化（rv 与数据段用各自的 magic）
bool createInitialised(ISegmentBackend& backend, const std::string& name, std::uint32_t group, SegmentView& view)
{
    if (backend.createOrOpen(name, sizeOf(group), view) != InitResult::kOk)
    {
        return false;
    }
    if (group == 0)
    {
        auto* rv = static_cast<RvSegment*>(view.base);
        const SegmentHeaderSpec spec{kRvMagic, kIpcAbi, kRvBytes, kRvHeaderBytes};
        return backend.initHeader(view, &rv->header.magic, &rv->header.abi, &rv->header.generation, spec) ==
               InitResult::kOk;
    }
    auto* ring = static_cast<RingSegment*>(view.base);
    const SegmentHeaderSpec spec{kRingMagic, kIpcAbi, kRingBytes, 64};
    return backend.initHeader(view, &ring->header.magic, &ring->header.abi, &ring->header.generation, spec) ==
           InitResult::kOk;
}

#if defined(_WIN32)

// 子进程从环境变量拿到父进程的测试作用域（测试代码可以读环境变量，发布代码不行）
constexpr const char* kChildScopeEnv = "RINNE_TEST_SHM_LEFTOVER_SCOPE";

std::unique_ptr<ISegmentBackend> makeBackend(const std::string& = {})
{
    return std::make_unique<SegmentBackendWin32>();
}

// 段是否还在：Windows 上能打开就是还在
bool segmentExists(const std::string& name)
{
    SegmentBackendWin32 backend;
    SegmentView view;
    if (backend.openExistingReadOnly(name, view) != InitResult::kOk)
    {
        return false;
    }
    backend.unmap(view);
    return true;
}

#else

std::unique_ptr<ISegmentBackend> makeBackend(const std::string& lockDir)
{
    return std::make_unique<SegmentBackendPosix>(SegmentBackendPosix::LockDirForTesting{lockDir});
}

// 段是否还在：POSIX 上名字还在就算（不管有没有持有者）
bool segmentExists(const std::string& name)
{
    const int fd = ::shm_open(name.c_str(), O_RDONLY, 0);
    if (fd < 0)
    {
        return errno != ENOENT;
    }
    ::close(fd);
    return true;
}

struct TempLockDir
{
    TempLockDir()
    {
        std::string templ = (std::filesystem::temp_directory_path() / "rinne-leftover-XXXXXX").string();
        REQUIRE(::mkdtemp(templ.data()) != nullptr);
        path = templ;
    }
    ~TempLockDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    TempLockDir(const TempLockDir&) = delete;
    TempLockDir& operator=(const TempLockDir&) = delete;
    std::string path;
};

#endif

} // namespace

TEST_CASE("segments released normally leave nothing behind", "[ipc][backend][shm-leftover]")
{
#if defined(_WIN32)
    const std::string lockDir;
#else
    TempLockDir dir;
    const std::string lockDir = dir.path;
#endif
    const Naming naming(NameScope::randomForTesting());
    const auto names = allNames(naming);
    {
        auto backend = makeBackend(lockDir);
        std::vector<SegmentView> views(names.size());
        for (std::uint32_t g = 0; g <= kGroupCount; ++g)
        {
            INFO(names[g]);
            REQUIRE(createInitialised(*backend, names[g], g, views[g]));
            CHECK(segmentExists(names[g]));
        }
        for (auto& view : views)
        {
            backend->unmap(view);
        }
    }
    for (const auto& name : names)
    {
        INFO(name);
        CHECK_FALSE(segmentExists(name));
    }
}

TEST_CASE("segments of a holder that exits without releasing do not survive", "[ipc][backend][shm-leftover]")
{
    const NameScope scope = NameScope::randomForTesting();
    const Naming naming(scope);
    const auto names = allNames(naming);
#if defined(_WIN32)
    // 子进程是本测试程序自己，只跑下面那个隐藏用例：建段、不释放、直接退出
    wchar_t exe[MAX_PATH] = {};
    REQUIRE(::GetModuleFileNameW(nullptr, exe, MAX_PATH) > 0);
    std::wstring command = L"\"" + std::wstring(exe) + L"\" \"[shm-leftover-child]\"";
    REQUIRE(::SetEnvironmentVariableA(kChildScopeEnv, std::string(scope.tag()).c_str()) != FALSE);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL started = ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                                          nullptr, &si, &pi);
    ::SetEnvironmentVariableA(kChildScopeEnv, nullptr);
    REQUIRE(started != FALSE);
    REQUIRE(::WaitForSingleObject(pi.hProcess, 60000) == WAIT_OBJECT_0);
    DWORD code = 1;
    ::GetExitCodeProcess(pi.hProcess, &code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    REQUIRE(code == 0); // 子进程确实建好了段
    // 持有者一退出，句柄就由内核关掉，段随之消失
    for (const auto& name : names)
    {
        INFO(name);
        CHECK_FALSE(segmentExists(name));
    }
#else
    TempLockDir dir;
    const pid_t child = ::fork();
    REQUIRE(child >= 0);
    if (child == 0)
    {
        // 子进程：建段，不 unmap、不跑析构，直接退出（等同崩溃）。退出码告诉父进程建没建成
        auto backend = makeBackend(dir.path);
        for (std::uint32_t g = 0; g <= kGroupCount; ++g)
        {
            SegmentView view;
            if (!createInitialised(*backend, names[g], g, view))
            {
                ::_exit(2);
            }
        }
        ::_exit(0);
    }
    int status = 0;
    REQUIRE(::waitpid(child, &status, 0) == child);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == 0);

    auto backend = makeBackend(dir.path);
    for (std::uint32_t g = 0; g <= kGroupCount; ++g)
    {
        INFO(names[g]);
        // 名字还在，但没有持有者：附着方不认它，下一个建段者清掉残段重建
        CHECK(segmentExists(names[g]));
        SegmentView attach;
        CHECK(backend->openExisting(names[g], attach) == InitResult::kFailed);
        SegmentView view;
        REQUIRE(backend->createOrOpen(names[g], sizeOf(g), view) == InitResult::kOk);
        CHECK(view.created);
        backend->unmap(view); // 最后一个离开者撤掉名字
        CHECK_FALSE(segmentExists(names[g]));
    }
#endif
}

#if defined(_WIN32)
// 上一个用例的子进程：从环境变量拿作用域，建段后不释放就退出。隐藏用例，只在显式按标签选中时运行
TEST_CASE("shm leftover child: create and exit without releasing", "[.][shm-leftover-child]")
{
    char tag[16] = {};
    const DWORD length = ::GetEnvironmentVariableA(kChildScopeEnv, tag, sizeof(tag));
    REQUIRE(length > 0);
    REQUIRE(length < sizeof(tag));
    const auto scope = NameScope::forTesting(tag);
    REQUIRE(scope.has_value());
    const auto names = allNames(Naming(*scope));
    auto* backend = new SegmentBackendWin32(); // 有意不释放：退出时句柄由内核关
    for (std::uint32_t g = 0; g <= kGroupCount; ++g)
    {
        auto* view = new SegmentView();
        if (!createInitialised(*backend, names[g], g, *view))
        {
            std::_Exit(2);
        }
    }
    std::_Exit(0);
}
#endif
