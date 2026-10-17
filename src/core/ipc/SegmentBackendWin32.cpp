// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/ipc.json

#include "ipc/SegmentBackendWin32.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace rinne::ipc
{

namespace
{

// 名字已经过 checkName，只有 ASCII
std::wstring toWide(const std::string& name)
{
    return std::wstring(name.begin(), name.end());
}

IpcDiagCode classifyWin32(DWORD error) noexcept
{
    switch (error)
    {
    case ERROR_ACCESS_DENIED:
        return IpcDiagCode::kShmDenied;
    case ERROR_FILENAME_EXCED_RANGE:
        return IpcDiagCode::kNameTooLong;
    case ERROR_INVALID_HANDLE:
        // 同名的内核对象不是文件映射（别的程序用这个名字建了事件、互斥量之类）
        return IpcDiagCode::kForeign;
    default:
        return IpcDiagCode::kOther;
    }
}

InitResult fail(IpcDiagOp op, DWORD error, const std::string& name, const char* detail) noexcept
{
    IpcDiagEvent e;
    e.op = op;
    e.code = classifyWin32(error);
    e.error = static_cast<int>(error);
    e.segment = name.c_str();
    e.detail = detail;
    reportIpcDiag(e);
    return initResultFor(e.code);
}

// 映射好了：回读真实大小，填视图。真实大小取视图所在区域的大小（VirtualQuery）：映射整段的视图按页对齐，
// 区域大小就是段大小按页上取整，对容量校验是安全的超集。section 句柄不是文件句柄，GetFileSizeEx 对它不适用。
// 读不出来时撤掉映射、返回失败，绝不交出一个大小为 0 的「成功」视图。
InitResult finishView(HANDLE h, void* base, bool created, const std::string& name, SegmentView& view)
{
    MEMORY_BASIC_INFORMATION mbi{};
    if (::VirtualQuery(base, &mbi, sizeof(mbi)) == 0 || mbi.RegionSize == 0)
    {
        const DWORD error = ::GetLastError();
        ::UnmapViewOfFile(base);
        ::CloseHandle(h);
        return fail(IpcDiagOp::kMap, error, name, "VirtualQuery on the mapped view failed");
    }
    view.base = base;
    view.size = static_cast<std::size_t>(mbi.RegionSize);
    view.mapping = h;
    view.created = created;
    view.name = name;
    return InitResult::kOk;
}

} // namespace

InitResult SegmentBackendWin32::createOrOpen(const std::string& name, std::size_t size, SegmentView& view)
{
    view.reset();
    if (size == 0)
    {
        return InitResult::kFailed;
    }
    if (const InitResult r = checkName(NamePlatform::kWin32, name); r != InitResult::kOk)
    {
        return r;
    }
    const auto size64 = static_cast<unsigned long long>(size);
    const HANDLE h =
        ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(size64 >> 32),
                             static_cast<DWORD>(size64 & 0xffffffffu), toWide(name).c_str());
    if (h == nullptr)
    {
        return fail(IpcDiagOp::kOpen, ::GetLastError(), name, "CreateFileMappingW failed");
    }
    const bool created = ::GetLastError() != ERROR_ALREADY_EXISTS;

    void* base = ::MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (base == nullptr)
    {
        const DWORD error = ::GetLastError();
        ::CloseHandle(h);
        return fail(IpcDiagOp::kMap, error, name, "MapViewOfFile failed");
    }
    // 锁页推迟到 initHeader 的初始化者分支：先写触碰再锁；附着方不锁
    return finishView(h, base, created, name, view);
}

InitResult SegmentBackendWin32::openExisting(const std::string& name, SegmentView& view)
{
    return openExistingWithAccess(name, view, false);
}

InitResult SegmentBackendWin32::openExistingReadOnly(const std::string& name, SegmentView& view)
{
    return openExistingWithAccess(name, view, true);
}

InitResult SegmentBackendWin32::openExistingWithAccess(const std::string& name, SegmentView& view, bool readOnly)
{
    view.reset();
    if (const InitResult r = checkName(NamePlatform::kWin32, name); r != InitResult::kOk)
    {
        return r;
    }
    const DWORD access = readOnly ? FILE_MAP_READ : FILE_MAP_ALL_ACCESS;
    const HANDLE h = ::OpenFileMappingW(access, FALSE, toWide(name).c_str());
    if (h == nullptr)
    {
        const DWORD error = ::GetLastError();
        if (error == ERROR_FILE_NOT_FOUND)
        {
            return InitResult::kFailed; // 段还没人建：25 Hz 重试路径上的常态，不刷诊断
        }
        return fail(IpcDiagOp::kOpen, error, name, "OpenFileMappingW failed");
    }
    void* base = ::MapViewOfFile(h, access, 0, 0, 0);
    if (base == nullptr)
    {
        const DWORD error = ::GetLastError();
        ::CloseHandle(h);
        return fail(IpcDiagOp::kMap, error, name, "MapViewOfFile failed");
    }
    return finishView(h, base, false, name, view);
}

void SegmentBackendWin32::tryLock(const SegmentView& view)
{
    if (view.base == nullptr || view.size == 0)
    {
        return;
    }
    // 不动工作集：最小 / 最大工作集是整个宿主进程的设置，插件不该替 DAW 改，改了也收不回来（别的实例可能还在用）。
    // VirtualLock 能锁多少受最小工作集约束：宿主自己调大过时能锁住，默认设置下环那么大的段多半锁不住，
    // 这时页面已经写触碰过、照常可用，只报一条诊断。解映射时系统自动解锁。
    if (::VirtualLock(view.base, view.size) == FALSE)
    {
        IpcDiagEvent e;
        e.op = IpcDiagOp::kLockPages;
        e.error = static_cast<int>(::GetLastError());
        e.segment = view.name.c_str();
        e.detail = "VirtualLock failed (working set too small); pages stay pageable";
        reportIpcDiag(e);
    }
}

void SegmentBackendWin32::unmap(SegmentView& view)
{
    if (view.base != nullptr)
    {
        ::UnmapViewOfFile(view.base);
    }
    if (view.mapping != nullptr)
    {
        ::CloseHandle(static_cast<HANDLE>(view.mapping));
    }
    view.reset();
}

} // namespace rinne::ipc
