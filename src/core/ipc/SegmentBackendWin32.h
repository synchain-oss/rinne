// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/ipc.json
//
// Windows 的共享内存段后端：CreateFileMappingW / OpenFileMappingW + MapViewOfFile，名字在 Local\ 命名空间
// （按登录会话隔开）。段随最后一个句柄关闭由内核回收，不需要 POSIX 那样的锁文件。
// 同名段已存在时 CreateFileMappingW 返回旧段的句柄、忽略请求的大小，所以真实大小一律经 GetFileSizeEx 回读：
// 拿请求值当真实值发布几何，小视图配大几何，写环就越界了。
// 只在非实时线程调用。本头文件不 include windows.h，免得 min / max 宏污染使用者。
#pragma once

#include <string>

#include "ipc/ISegmentBackend.h"

namespace rinne::ipc
{

class SegmentBackendWin32 final : public ISegmentBackend
{
public:
    InitResult createOrOpen(const std::string& name, std::size_t size, SegmentView& view) override;
    InitResult openExisting(const std::string& name, SegmentView& view) override;
    // FILE_MAP_READ：只读探测的最小权限，类型之外再从操作系统层面防住误写
    InitResult openExistingReadOnly(const std::string& name, SegmentView& view) override;
    void unmap(SegmentView& view) override;
    // VirtualLock，先把工作集上限抬高本段的大小；失败只报诊断，页面已经写触碰过
    void tryLock(const SegmentView& view) override;

private:
    InitResult openExistingWithAccess(const std::string& name, SegmentView& view, bool readOnly);
};

} // namespace rinne::ipc
