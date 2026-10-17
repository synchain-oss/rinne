// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/ipc.json
//
// POSIX 的共享内存段后端（macOS 发布用；Linux 上只编进单测）：shm_open / mmap，加一套 flock 锁文件模拟
// Windows「最后一个句柄关掉段就消失」的语义。只在非实时线程调用；音频线程只拿映射后的裸指针。
//
// 名字：调用方传 Naming 拼好的 "/SnchRinne.<uid8>.rv" 之类，含开头的 '/' 不超过 31 个字符（macOS 的上限，
// 用户态头文件里拿不到这个宏，只能写常量）。超长或字符不合法时在碰文件系统之前就返回 kFailedBadName。
//
// 生命周期：每个段配一个锁文件 <锁目录>/<名字去掉 '/'>.lock，每个映射持有它的一把 LOCK_SH，直到 unmap。
//   - 锁目录 = 真实 home（按有效 uid 查密码库，不看可能被容器化的 HOME）下的
//     Library/Application Support/Synchain/Rinne/ipc。不放 Caches（会被系统清理），也不放 TMPDIR：
//     进程外托管 AU 的服务里 TMPDIR 是服务专属的子目录，与宿主进程看到的不是同一处。
//     测试只能经构造参数 LockDirForTesting 换一个目录；发布版不读任何环境变量。
//   - 所有转换（建、附着、离开）都在全局 <锁目录>/lifecycle.lock 的 LOCK_EX 下做：BSD flock 的升降级不是原子的
//     （先放旧锁再上新锁），只有在这把全局锁下，「EX 探测 → 降为 SH」与「离开时 EX 探测 → unlink」之间才不会
//     插进别人的转换。
//   - 全局锁有界等待，等不到就放弃这一次：createOrOpen 2 s（加载与改组，很少）；openExisting 100 ms（25 Hz 重试）；
//     离开 200 ms（等不到就不做 unlink 判定，段留给下一个创建者清理）。一旦等满超时，之后 1 s 内本实例的取锁
//     只试一次不等（断路）：锁被一个挂住的进程拿着时，卸载时的 N 个映射、25 Hz 的重试不会把等待乘上去。
//     断路窗口内的失败只计数不上报，计数随下一条同类诊断的 suppressed 报出。
//   - createOrOpen：对段锁文件试 LOCK_EX|LOCK_NB。
//       拿到 ⇒ 没有活着的持有者 ⇒ 先 shm_unlink 残段（崩溃留下的、旧尺寸的都在这里清掉，只清本用户名下的），
//              再 shm_open(O_CREAT|O_EXCL, 0600) + ftruncate，created = true；
//       EWOULDBLOCK ⇒ 有活持有者 ⇒ 以 O_RDWR 附着，绝不 ftruncate（macOS 上一个 shm 对象只能 ftruncate 一次）。
//     之后都降为 LOCK_SH，持有到 unmap。
//   - openExisting / openExistingReadOnly：只附着、绝不建；没有活持有者的段等同于 Windows 上「已经不存在」，
//     安静地返回 kFailed，不碰它。只读打开用 O_RDONLY + PROT_READ，同样持 LOCK_SH。
//   - 映射：fstat 回读真实大小（macOS 按页上取整，可能大于请求值，对容量校验是安全的超集），mmap 后立刻关掉
//     shm 的 fd；锁 fd 与名字放在后端私有的记录里，SegmentView::mapping 指向它。
//   - unmap：munmap，然后在全局锁下试 LOCK_EX|LOCK_NB，拿到 ⇒ 自己是最后一个离开者 ⇒ shm_unlink；最后关锁 fd。
//     DAW 崩溃时内核释放它持有的所有 flock，下一个 createOrOpen 就走清理分支。
//   - 视图没经 unmap 就被丢掉（句柄在宽限期届满前析构，插件卸载时的常态）：映射留到进程退出，锁的那一半在
//     后端析构时补做。所以后端必须比用它的句柄活得久，一个视图也必须由映射它的那个后端实例 unmap。
//   - 必须用 flock，不能用 fcntl：flock 按「打开的文件描述」计，同一进程里两个实例各自 open 锁文件也照样互斥；
//     fcntl 的记录锁按进程计，同进程的第二个实例会误以为没有持有者而 unlink 活段。
//
// 错误：errno 归类后决定返回值（IpcDiag::classifyErrno）：EPERM / EACCES / ENOSYS → kFailedDenied（宿主沙盒），
// ENAMETOOLONG → kFailedBadName，其余 kFailed（下一拍重试）。诊断一律在放掉全局锁之后才交给 sink。
// 锁页尽力而为：失败只报诊断，不影响返回值。
//
// 已知限制：
//   - App Sandbox 里运行插件的宿主：沙盒要求 POSIX shm 名带 App Group 前缀（31 字符放不下），锁目录也在容器之外，
//     这个后端在那类宿主里会被拒（kFailedDenied），界面显示「IPC 不可用」。
//   - DAW 运行期间手动删掉锁目录：活着的持有者仍拿着旧锁文件（已无名字）上的 SH，新来的创建者会建出新锁文件、
//     判段「没有持有者」并清掉活段，两边各用一份。只能靠「运行中别删这个目录」规避。
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

#include "ipc/ISegmentBackend.h"

namespace rinne::ipc
{

class SegmentBackendPosix final : public ISegmentBackend
{
public:
    static constexpr const char* kLifecycleLockName = "lifecycle.lock";
    static constexpr const char* kHomeRelativeLockDir = "Library/Application Support/Synchain/Rinne/ipc";

    static constexpr std::chrono::milliseconds kCreateLockWait{2000};
    static constexpr std::chrono::milliseconds kAttachLockWait{100};
    static constexpr std::chrono::milliseconds kLeaveLockWait{200};
    static constexpr std::chrono::milliseconds kLockBreakerWindow{1000};

    // 测试用的锁目录（必须是绝对路径）。只有单测与测试 peer 进程用；同一组段的所有进程必须给同一个目录。
    struct LockDirForTesting
    {
        std::string path;
    };

    SegmentBackendPosix();
    explicit SegmentBackendPosix(LockDirForTesting lockDir);
    ~SegmentBackendPosix() override;
    SegmentBackendPosix(const SegmentBackendPosix&) = delete;
    SegmentBackendPosix& operator=(const SegmentBackendPosix&) = delete;

    InitResult createOrOpen(const std::string& name, std::size_t size, SegmentView& view) override;
    InitResult openExisting(const std::string& name, SegmentView& view) override;
    InitResult openExistingReadOnly(const std::string& name, SegmentView& view) override;
    void unmap(SegmentView& view) override;
    void tryLock(const SegmentView& view) override;

    // 默认锁目录：<真实 home>/kHomeRelativeLockDir；查不到 home 时返回空串，之后所有打开都失败并报 kLockDir
    static std::string defaultLockDir();

    const std::string& lockDir() const noexcept { return lockDir_; }

private:
    struct Mapping;
    struct PendingDiag;
    enum class Mode
    {
        kCreateOrOpen,
        kOpenExisting,
        kOpenReadOnly,
    };

    InitResult openSegment(const std::string& name, std::size_t size, SegmentView& view, Mode mode);
    // 持全局锁的那一段；失败原因记进 diag，调用方放锁之后再上报
    InitResult openLocked(const std::string& shm, std::size_t size, SegmentView& view, Mode mode, PendingDiag& diag);
    void track(Mapping* m) noexcept;
    void untrack(Mapping* m) noexcept;
    // 离开：全局锁下 EX 探测 → 最后离开者 shm_unlink → 关锁 fd。不 munmap；不分配、不抛（后端析构里也调它）
    void leave(Mapping& m) noexcept;
    std::chrono::milliseconds lockWait(std::chrono::milliseconds normal) const noexcept;
    void noteLockFailure(int error, const std::string& lockPath, std::chrono::milliseconds wait,
                         PendingDiag& diag) noexcept;

    std::string lockDir_;
    std::atomic<long long> lastLockTimeoutMs_{-1}; // steady_clock 毫秒；-1 = 从没超时过
    std::atomic<std::uint32_t> suppressedLockFailures_{0}; // 断路窗口内略去、尚未随诊断报出的失败次数
    std::mutex liveMutex_; // 保护 live_（几个线程共用一个后端实例时）
    Mapping* live_ = nullptr; // 本实例映射出去、尚未 unmap 的视图（侵入式双向链表，插入与摘除不分配）
};

} // namespace rinne::ipc
