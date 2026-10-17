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

#include "ipc/Naming.h"

namespace rinne::ipc
{

namespace
{

// 名字已经过 validateSegmentName，只有 ASCII
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

InitResult rejectName(const std::string& name, NameStatus status) noexcept
{
    reportBadSegmentName(name, status);
    return InitResult::kFailedBadName;
}

// GetFileSizeEx 对 section 句柄给出精确的段大小；失败时退回 VirtualQuery（按 64 KB 对齐的上界，
// 对容量校验仍是安全的超集）
std::size_t realSectionSize(HANDLE h, void* base) noexcept
{
    LARGE_INTEGER li{};
    if (::GetFileSizeEx(h, &li) != FALSE && li.QuadPart > 0)
    {
        return static_cast<std::size_t>(li.QuadPart);
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (::VirtualQuery(base, &mbi, sizeof(mbi)) != 0 && mbi.RegionSize > 0)
    {
        return mbi.RegionSize;
    }
    return 0;
}

} // namespace

InitResult SegmentBackendWin32::createOrOpen(const std::string& name, std::size_t size, SegmentView& view)
{
    view.reset();
    if (size == 0)
    {
        return InitResult::kFailed;
    }
    if (const NameStatus status = validateSegmentName(NamePlatform::kWin32, name); status != NameStatus::kOk)
    {
        return rejectName(name, status);
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

    view.base = base;
    view.size = realSectionSize(h, base);
    view.mapping = h;
    view.created = created;
    view.name = name;
    // 锁页推迟到 initHeader 的建段分支：先写触碰再锁；附着方不锁
    return InitResult::kOk;
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
    if (const NameStatus status = validateSegmentName(NamePlatform::kWin32, name); status != NameStatus::kOk)
    {
        return rejectName(name, status);
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
    view.base = base;
    view.size = realSectionSize(h, base);
    view.mapping = h;
    view.created = false;
    view.name = name;
    return InitResult::kOk;
}

void SegmentBackendWin32::tryLock(const SegmentView& view)
{
    if (view.base == nullptr || view.size == 0)
    {
        return;
    }
    const HANDLE process = ::GetCurrentProcess();
    SIZE_T minWs = 0;
    SIZE_T maxWs = 0;
    if (::GetProcessWorkingSetSize(process, &minWs, &maxWs) != FALSE)
    {
        ::SetProcessWorkingSetSize(process, minWs, maxWs + static_cast<SIZE_T>(view.size));
    }
    if (::VirtualLock(view.base, view.size) == FALSE)
    {
        IpcDiagEvent e;
        e.op = IpcDiagOp::kLockPages;
        e.error = static_cast<int>(::GetLastError());
        e.segment = view.name.c_str();
        e.detail = "VirtualLock failed; pages stay pageable";
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
