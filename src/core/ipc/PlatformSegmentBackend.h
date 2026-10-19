// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/ipc.json
//
// 「这个平台用哪个共享内存段后端」的唯一接缝。两个插件与会合层只认这个别名，不各自写死后端类型：
//   Windows   SegmentBackendWin32（Local\ 命名空间）
//   Apple     SegmentBackendPosix（shm_open + flock 生命周期锁）
//   其余平台  SegmentBackendInProcess：只让 core 在没有真后端的平台（Linux 的 core-only 构建）上编得过、测得了，
//             同一进程里的实例彼此可见，跨进程不可见，不是可发布的形态。
// 选择只在预处理期发生，别名之外不加任何间接层，音频线程的访问路径不变。
// 沙盒宿主里 POSIX 被拒（kFailedDenied）时的同进程备用后端是条件扩展，接在 Apple 这一支；v1 不实现。
#pragma once

#if defined(_WIN32)
#include "ipc/SegmentBackendWin32.h"
#elif defined(__APPLE__)
#include "ipc/SegmentBackendPosix.h"
#else
#include "ipc/SegmentBackendInProcess.h"
#endif

namespace rinne::ipc
{

#if defined(_WIN32)
using PlatformSegmentBackend = SegmentBackendWin32;
#elif defined(__APPLE__)
using PlatformSegmentBackend = SegmentBackendPosix;
#else
using PlatformSegmentBackend = SegmentBackendInProcess;
#endif

// 本平台后端的稳定 ASCII 名，写进诊断文本：win32、posix 或 inprocess
const char* platformSegmentBackendName() noexcept;

} // namespace rinne::ipc
