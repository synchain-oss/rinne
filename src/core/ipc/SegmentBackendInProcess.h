// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/ipc.json
//
// 进程内的段后端：名字 → 一块 64 字节对齐、清零的缓冲，同一进程里按同一个名字打开的实例共享它，跨进程不可见。
// 语义对齐 Windows：同名已存在时忽略请求大小、回填真实大小，created = false。
// 只给单测用（Linux 的 core-only 构建里它也是 PlatformSegmentBackend，只为让会合与环路的代码在那里编得过、测得了）；
// 插件从不用它。名字按本机平台的规则校验，与真后端拿到的名字同形。
#pragma once

#include <string>

#include "ipc/ISegmentBackend.h"

namespace rinne::ipc
{

class SegmentBackendInProcess final : public ISegmentBackend
{
public:
    InitResult createOrOpen(const std::string& name, std::size_t size, SegmentView& view) override;
    InitResult openExisting(const std::string& name, SegmentView& view) override;
    // 只清视图；缓冲由全局表持有到进程退出或 resetAllForTesting，别的实例可能还在用同名段
    void unmap(SegmentView& view) override;

    // 清空所有模拟段：每个用例开始时调用，用例之间不共享残留。调用时不得有任何视图仍在使用。
    static void resetAllForTesting();
};

} // namespace rinne::ipc
