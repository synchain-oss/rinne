// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/ipc.json
//
// 共享内存段后端的诊断出口。后端打不开、建不出、锁不上时，对调用方只回一个 InitResult；失败发生在哪一步、
// 操作系统报的错误码、归到哪一类、是哪个段，都经这里旁路出去。插件侧的平台日志（plugin-common/PlatformLog）
// 装一个 sink 把它落进文件；没装 sink 时什么都不做，rinne_core 的单测与没接日志的构建行为不变。
//
// 约定：
//   - 只在非实时线程上报：报告点只在后端的 createOrOpen / openExisting / unmap / tryLock 与段头初始化里，
//     这些按接口约定只在持生命周期锁的非实时线程调用（不一定是消息线程：宿主可能在 prepare / release 里调到）。
//     sink 在报告者的线程上同步调用，应当尽快返回，不得回调段后端。
//   - POSIX 后端放掉全局 lifecycle.lock 之后才上报，sink 里做文件 I/O 也卡不住别的进程的段操作。
//   - 同一事件可能高频重复（25 Hz 的重试路径上每次失败都报一条），落盘的 sink 要过 IpcDiagLimiter。
//   - 事件里的字符串只在回调期间有效，sink 要留存就自己拷贝。段名与 detail 是 ASCII；path 是文件系统给的
//     UTF-8（用户目录名可能不是 ASCII）。
//   - sink 是每个链接了 rinne_core 的二进制里唯一的一个函数指针；两个插件是两个二进制，各有一份。
//   - 本诊断不进任何共享内存段，只是进程内的观测口。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rinne::ipc
{

// 失败发生在哪一步。数值只在进程内区分，不持久化、不跨进程。
enum class IpcDiagOp : std::uint32_t
{
    kName, // 段名不合法或超长（在碰操作系统之前就拦下）
    kLockDir, // POSIX：锁目录解析或建立失败
    kLifecycleLock, // POSIX：全局 lifecycle.lock 打开或加锁失败（含有界等待超时）
    kSegmentLock, // POSIX：段锁文件打开或 flock 失败
    kOpen, // shm_open / CreateFileMappingW / OpenFileMappingW 失败
    kTruncate, // POSIX：创建者 ftruncate 失败
    kStat, // POSIX：fstat 失败或拿到 0 尺寸
    kMap, // mmap / MapViewOfFile 失败
    kUnlink, // POSIX：清残段或最后离开者 shm_unlink 失败
    kLockPages, // 创建者锁页失败（尽力而为，不影响返回值）
    kHeader, // 段头校验：magic 不是我们的、abi 不同、真实大小小于布局
};

// 失败归到哪一类。前四类决定界面上的红字：
//   kShmDenied        操作系统拒绝（EPERM / EACCES / ENOSYS，Windows 的拒绝访问）：多半是宿主沙盒；本进程内不会自己好
//   kNameTooLong      段名超出 31 字符或 ENAMETOOLONG：配置错误
//   kAbiMismatch      段头的 magic 是我们的、abi 不同：对端是不兼容的版本
//   kUnsupportedBlock 宿主声明的最大块长超过 32768：由接线层在 prepare 时报，后端不报
// 其余：kBusy 暂时性失败（锁等待超时等），下一拍重试；kBadSize 段的真实大小小于布局；kForeign 段头的 magic 不是
// 我们的（同名段属于别的程序）；kOther 未归类。
enum class IpcDiagCode : std::uint32_t
{
    kOther,
    kShmDenied,
    kNameTooLong,
    kAbiMismatch,
    kUnsupportedBlock,
    kBusy,
    kBadSize,
    kForeign,
};

struct IpcDiagEvent
{
    IpcDiagOp op = IpcDiagOp::kName;
    IpcDiagCode code = IpcDiagCode::kOther;
    int error = 0; // errno 或 GetLastError()；0 = 不适用
    const char* segment = ""; // 平台段名；从不为 nullptr，可能为空串
    const char* path = ""; // 相关的文件或目录路径；从不为 nullptr，可能为空串
    const char* detail = ""; // 静态 ASCII 描述；从不为 nullptr
    // 本条之前、同一后端实例略去未报的同类失败次数（只用于 lifecycle.lock 的断路窗口；0 = 没有）
    std::uint32_t suppressed = 0;
};

// sink 必须 noexcept：报告点在后端的失败清理路径里，不允许异常穿出
using IpcDiagSink = void (*)(const IpcDiagEvent& event) noexcept;

// 装 sink（nullptr = 恢复空操作），返回之前的 sink
IpcDiagSink setIpcDiagSink(IpcDiagSink sink) noexcept;
IpcDiagSink ipcDiagSink() noexcept;

// 后端在失败点调用：有 sink 就同步转给它
void reportIpcDiag(const IpcDiagEvent& event) noexcept;

// errno 的归类：EPERM / EACCES / ENOSYS → kShmDenied，ENAMETOOLONG → kNameTooLong，
// ETIMEDOUT / EWOULDBLOCK / EAGAIN / EINTR → kBusy，其余 kOther。Windows 的错误码在 Win32 后端里归类。
IpcDiagCode classifyErrno(int error) noexcept;

// 稳定的 ASCII 名（日志用）；未知值返回 "unknown"
const char* ipcDiagOpName(IpcDiagOp op) noexcept;
const char* ipcDiagCodeName(IpcDiagCode code) noexcept;

// 落盘前的去重与限流（纯逻辑，时间由调用方传入）。键 = (op, code, error, segment)：同一键在 kRepeatWindowMs 内
// 只放行一条，下一次放行时带上期间略去的次数；另有全局每窗口 kMaxLinesPerWindow 条的上限，挡住大量不同的键同时刷屏。
// 时间差按 uint32 无符号相减，毫秒计数器约 49 天回绕一次也不出错。不加锁：调用方保证同一时刻只有一个线程用它。
class IpcDiagLimiter
{
public:
    static constexpr std::uint32_t kRepeatWindowMs = 60000;
    static constexpr std::uint32_t kMaxLinesPerWindow = 64;
    static constexpr std::size_t kMaxKeys = 256;

    struct Verdict
    {
        bool emit = false;
        std::uint32_t repeatsSuppressed = 0; // 同一键在它上一条之后被略去的次数
        std::uint32_t rateLimited = 0; // 全局上限在上一条放行之后挡掉的条数（任意键）
    };

    Verdict onEvent(const IpcDiagEvent& event, std::uint32_t nowMs);
    std::size_t keyCount() const noexcept { return entries_.size(); }

private:
    struct Entry
    {
        IpcDiagOp op = IpcDiagOp::kName;
        IpcDiagCode code = IpcDiagCode::kOther;
        int error = 0;
        std::string segment;
        std::uint32_t lastEmitMs = 0;
        std::uint32_t suppressed = 0;
    };

    std::vector<Entry> entries_;
    bool windowStarted_ = false;
    std::uint32_t windowStartMs_ = 0;
    std::uint32_t linesInWindow_ = 0;
    std::uint32_t rateLimited_ = 0;
};

// 一条事件的日志行（不带时间戳），例如
//   ipc shm-open shm-denied errno 1 segment /SnchRinne.000001f5.rv -- shm_open(O_CREAT|O_EXCL) failed
std::string formatIpcDiagLine(const IpcDiagEvent& event, const IpcDiagLimiter::Verdict& verdict);

} // namespace rinne::ipc
