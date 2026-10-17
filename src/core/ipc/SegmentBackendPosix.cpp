// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/ipc.json

#include "ipc/SegmentBackendPosix.h"

#include <cerrno>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <pwd.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "ipc/Naming.h"

namespace rinne::ipc
{

// 后端私有的映射记录。SegmentView::mapping 指向它；同一后端实例的在用记录串成双向链表，后端析构时据此补做离开。
struct SegmentBackendPosix::Mapping
{
    int lockFd = -1; // 段锁文件的 fd，持 LOCK_SH 直到离开
    std::string shmName;
    // 建映射时拼好的 lifecycle.lock 路径：离开时用同一把全局锁，leave() 不再分配（它会从析构里调用，不能抛）
    std::string lifecyclePath;
    void* base = nullptr;
    std::size_t size = 0;
    Mapping* prev = nullptr;
    Mapping* next = nullptr;
};

// 持全局锁期间发生的失败先记在这里，放锁之后再交给诊断 sink。note() 不抛：它出现在已经开了 fd 的失败路径上，
// 路径拷贝分配失败时宁可丢掉路径，也不能让异常越过还没关的 fd。
struct SegmentBackendPosix::PendingDiag
{
    bool set = false;
    IpcDiagOp op = IpcDiagOp::kName;
    IpcDiagCode code = IpcDiagCode::kOther;
    int error = 0;
    std::string path;
    const char* detail = "";
    std::uint32_t suppressed = 0;

    void note(IpcDiagOp o, int e, const char* d) noexcept
    {
        set = true;
        op = o;
        error = e;
        code = classifyErrno(e);
        detail = d;
    }
    void note(IpcDiagOp o, int e, const char* d, const std::string& p) noexcept
    {
        note(o, e, d);
        try
        {
            path = p;
        }
        catch (...)
        {
            path.clear();
        }
    }
    // 记过诊断的失败按分类返回；没记的（安静的失败）是可重试的 kFailed
    InitResult result() const noexcept { return set ? initResultFor(code) : InitResult::kFailed; }
};

namespace
{

constexpr auto kLifecycleLockPoll = std::chrono::milliseconds(1);

void report(IpcDiagOp op, IpcDiagCode code, int error, const std::string& segment, const std::string& path,
            const char* detail, std::uint32_t suppressed = 0) noexcept
{
    IpcDiagEvent e;
    e.op = op;
    e.code = code;
    e.error = error;
    e.segment = segment.c_str();
    e.path = path.c_str();
    e.detail = detail;
    e.suppressed = suppressed;
    reportIpcDiag(e);
}

long long steadyMs() noexcept
{
    return static_cast<long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

int openRetry(const char* path, int flags, int mode) noexcept
{
    int fd = -1;
    do
    {
        fd = ::open(path, flags, mode);
    } while (fd < 0 && errno == EINTR);
    return fd;
}

int flockRetry(int fd, int operation) noexcept
{
    int r = -1;
    do
    {
        r = ::flock(fd, operation);
    } while (r != 0 && errno == EINTR);
    return r;
}

bool isDirectory(const std::string& path, int& err)
{
    struct stat st
    {
    };
    if (::stat(path.c_str(), &st) != 0)
    {
        err = errno;
        return false;
    }
    if (!S_ISDIR(st.st_mode))
    {
        err = ENOTDIR;
        return false;
    }
    return true;
}

// mkdir -p：新建的每一级 0700，已存在的不动。失败时 err = errno
bool ensureDirectory(const std::string& path, int& err)
{
    if (path.empty() || path[0] != '/')
    {
        err = EINVAL;
        return false;
    }
    if (isDirectory(path, err))
    {
        return true; // 常态：锁目录早已建好，不碰上级目录
    }
    std::size_t pos = 1;
    while (pos <= path.size())
    {
        const std::size_t slash = path.find('/', pos);
        const std::size_t end = (slash == std::string::npos) ? path.size() : slash;
        if (end > pos)
        {
            const std::string prefix = path.substr(0, end);
            int statErr = 0;
            if (!isDirectory(prefix, statErr) && ::mkdir(prefix.c_str(), 0700) != 0 && errno != EEXIST)
            {
                err = errno;
                return false;
            }
        }
        if (slash == std::string::npos)
        {
            break;
        }
        pos = slash + 1;
    }
    return isDirectory(path, err);
}

// 全局 lifecycle.lock 的作用域持有（LOCK_EX，有界等待；wait = 0 时只试一次）。create = false 时文件不存在就不建
// （只读、附着与离开的路径：锁目录里从没建过任何东西，也就不可能有段）。路径由调用方拼好并在本对象生命期内有效，
// 这里只存引用：离开的路径不许分配。
class LifecycleLock
{
public:
    LifecycleLock(const std::string& path, bool create, std::chrono::milliseconds wait) : path_(path)
    {
        const int flags = create ? (O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW) : (O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        fd_ = openRetry(path_.c_str(), flags, 0600);
        if (fd_ < 0)
        {
            error_ = errno;
            return;
        }
        const auto deadline = std::chrono::steady_clock::now() + wait;
        for (;;)
        {
            if (flockRetry(fd_, LOCK_EX | LOCK_NB) == 0)
            {
                held_ = true;
                return;
            }
            if (errno != EWOULDBLOCK)
            {
                error_ = errno;
                break;
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                error_ = ETIMEDOUT;
                break;
            }
            std::this_thread::sleep_for(kLifecycleLockPoll);
        }
        ::close(fd_);
        fd_ = -1;
    }

    ~LifecycleLock()
    {
        if (fd_ >= 0)
        {
            ::close(fd_); // 关 fd 即释放 flock
        }
    }

    LifecycleLock(const LifecycleLock&) = delete;
    LifecycleLock& operator=(const LifecycleLock&) = delete;

    bool held() const noexcept { return held_; }
    int error() const noexcept { return error_; }
    const std::string& path() const noexcept { return path_; }

private:
    const std::string& path_;
    int fd_ = -1;
    int error_ = 0;
    bool held_ = false;
};

// 无主段的属主检查：只清自己（有效 uid）名下的段。段名里带着 uid，别的用户本来就碰不到同名段；这里是纵深防御：
// POSIX shm 是全机命名空间，锁目录却在各自的 home 里，另一个用户的活段在本用户的锁看来也是「没有持有者」。
// 返回 0 = 可以清（段不存在，或属于自己）；否则返回要上报的 errno。
int orphanRemovalBlocker(const std::string& shm) noexcept
{
    const int fd = ::shm_open(shm.c_str(), O_RDONLY, 0);
    if (fd < 0)
    {
        return errno == ENOENT ? 0 : errno;
    }
    struct stat st
    {
    };
    const int r = ::fstat(fd, &st);
    const int statErr = errno;
    ::close(fd);
    if (r != 0)
    {
        return statErr;
    }
    return st.st_uid == ::geteuid() ? 0 : EACCES;
}

} // namespace

std::string SegmentBackendPosix::defaultLockDir()
{
    // 真实 home 取自密码库（按有效 uid，与清残段时核属主同一口径），不取 HOME：宿主若被容器化，HOME 会指进容器，
    // 而同一用户的别的进程看到的是另一个目录，锁就不再是同一把。查不到时不退回环境变量：发布版不读环境变量。
    long bufSize = ::sysconf(_SC_GETPW_R_SIZE_MAX);
    if (bufSize <= 0)
    {
        bufSize = 16384;
    }
    std::vector<char> buf(static_cast<std::size_t>(bufSize));
    struct passwd pw
    {
    };
    struct passwd* result = nullptr;
    if (::getpwuid_r(::geteuid(), &pw, buf.data(), buf.size(), &result) != 0 || result == nullptr ||
        result->pw_dir == nullptr || result->pw_dir[0] != '/')
    {
        return {};
    }
    std::string home = result->pw_dir;
    while (home.size() > 1 && home.back() == '/')
    {
        home.pop_back();
    }
    return home + "/" + kHomeRelativeLockDir;
}

SegmentBackendPosix::SegmentBackendPosix() : lockDir_(defaultLockDir()) {}

SegmentBackendPosix::SegmentBackendPosix(LockDirForTesting lockDir) : lockDir_(std::move(lockDir.path)) {}

SegmentBackendPosix::~SegmentBackendPosix()
{
    // 视图没经 unmap 就被丢掉：映射留到进程退出，锁这一半在这里补做。全局锁挂住时，第一个映射等满 kLeaveLockWait
    // 之后断路打开，其余映射各只试一次
    for (;;)
    {
        Mapping* m = nullptr;
        {
            const std::lock_guard<std::mutex> lock(liveMutex_);
            m = live_;
            if (m != nullptr)
            {
                live_ = m->next;
                if (live_ != nullptr)
                {
                    live_->prev = nullptr;
                }
            }
        }
        if (m == nullptr)
        {
            break;
        }
        leave(*m);
        delete m;
    }
}

std::chrono::milliseconds SegmentBackendPosix::lockWait(std::chrono::milliseconds normal) const noexcept
{
    const long long last = lastLockTimeoutMs_.load(std::memory_order_relaxed);
    if (last >= 0 && steadyMs() - last < kLockBreakerWindow.count())
    {
        return std::chrono::milliseconds(0);
    }
    return normal;
}

void SegmentBackendPosix::noteLockFailure(int error, const std::string& lockPath, std::chrono::milliseconds wait,
                                          PendingDiag& diag) noexcept
{
    if (error == ETIMEDOUT)
    {
        if (wait.count() == 0)
        {
            // 断路窗口内只试了一次：不上报，只计数，并进下一条同类诊断
            suppressedLockFailures_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        // 真正等满的那次超时才（重新）打开断路窗口；窗口内的失败不续长它
        lastLockTimeoutMs_.store(steadyMs(), std::memory_order_relaxed);
    }
    diag.note(IpcDiagOp::kLifecycleLock, error,
              error == ETIMEDOUT ? "timed out waiting for lifecycle.lock" : "cannot open lifecycle.lock", lockPath);
    diag.suppressed = suppressedLockFailures_.exchange(0, std::memory_order_relaxed);
}

void SegmentBackendPosix::track(Mapping* m) noexcept
{
    const std::lock_guard<std::mutex> lock(liveMutex_);
    m->prev = nullptr;
    m->next = live_;
    if (live_ != nullptr)
    {
        live_->prev = m;
    }
    live_ = m;
}

void SegmentBackendPosix::untrack(Mapping* m) noexcept
{
    const std::lock_guard<std::mutex> lock(liveMutex_);
    if (m->prev != nullptr)
    {
        m->prev->next = m->next;
    }
    else if (live_ == m)
    {
        live_ = m->next;
    }
    if (m->next != nullptr)
    {
        m->next->prev = m->prev;
    }
    m->prev = nullptr;
    m->next = nullptr;
}

InitResult SegmentBackendPosix::createOrOpen(const std::string& name, std::size_t size, SegmentView& view)
{
    return openSegment(name, size, view, Mode::kCreateOrOpen);
}

InitResult SegmentBackendPosix::openExisting(const std::string& name, SegmentView& view)
{
    return openSegment(name, 0, view, Mode::kOpenExisting);
}

InitResult SegmentBackendPosix::openExistingReadOnly(const std::string& name, SegmentView& view)
{
    return openSegment(name, 0, view, Mode::kOpenReadOnly);
}

InitResult SegmentBackendPosix::openSegment(const std::string& name, std::size_t size, SegmentView& view, Mode mode)
{
    view.reset();
    const bool create = (mode == Mode::kCreateOrOpen);
    if (create && size == 0)
    {
        return InitResult::kFailed;
    }
    if (const NameStatus status = validateSegmentName(NamePlatform::kPosix, name); status != NameStatus::kOk)
    {
        reportBadSegmentName(name, status);
        return InitResult::kFailedBadName;
    }
    if (lockDir_.empty() || lockDir_[0] != '/')
    {
        report(IpcDiagOp::kLockDir, IpcDiagCode::kOther, ENOENT, name, lockDir_,
               "cannot resolve the lock directory (no home directory, or not an absolute path)");
        return InitResult::kFailed;
    }
    if (create)
    {
        int err = 0;
        if (!ensureDirectory(lockDir_, err))
        {
            const IpcDiagCode code = classifyErrno(err);
            report(IpcDiagOp::kLockDir, code, err, name, lockDir_, "cannot create the lock directory");
            return initResultFor(code);
        }
    }

    PendingDiag diag;
    const InitResult r = openLocked(name, size, view, mode, diag);
    // 到这里全局锁已经放掉，诊断此刻才交给 sink
    if (diag.set)
    {
        report(diag.op, diag.code, diag.error, name, diag.path, diag.detail, diag.suppressed);
    }
    return r;
}

InitResult SegmentBackendPosix::openLocked(const std::string& shm, std::size_t size, SegmentView& view, Mode mode,
                                           PendingDiag& diag)
{
    const bool create = (mode == Mode::kCreateOrOpen);

    // 映射记录（含两份字符串）在拿任何 fd 或锁之前就分配好：分配失败只会把异常抛出去，不会留下已开的 fd 或映射。
    // 之后直到成功返回，不再有可能抛的分配。离开时也用这份现成的路径，所以 leave() 不分配。
    auto record = std::make_unique<Mapping>();
    record->shmName = shm;
    record->lifecyclePath = lockDir_ + "/" + kLifecycleLockName;
    const std::string lockPath = lockDir_ + "/" + shm.substr(1) + ".lock";
    std::string viewName = shm;

    // 全局生命周期锁：从这里到函数返回，本进程与别的进程都插不进任何转换
    const auto wait = lockWait(create ? kCreateLockWait : kAttachLockWait);
    LifecycleLock life(record->lifecyclePath, create, wait);
    if (!life.held())
    {
        if (!create && life.error() == ENOENT)
        {
            return InitResult::kFailed; // 锁目录里从没建过东西，不可能有段；安静地等下一拍
        }
        noteLockFailure(life.error(), life.path(), wait, diag);
        return diag.result();
    }
    suppressedLockFailures_.store(0, std::memory_order_relaxed);

    const int lockFlags = create ? (O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW) : (O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    const int lockFd = openRetry(lockPath.c_str(), lockFlags, 0600);
    if (lockFd < 0)
    {
        const int err = errno;
        if (!create && err == ENOENT)
        {
            return InitResult::kFailed; // 这个段从没被建过
        }
        diag.note(IpcDiagOp::kSegmentLock, err, "cannot open the segment lock file", lockPath);
        return diag.result();
    }

    bool created = false;
    int shmFd = -1;
    if (flockRetry(lockFd, LOCK_EX | LOCK_NB) == 0)
    {
        // 没有活着的持有者
        if (!create)
        {
            // 附着与只读路径：等同 Windows 上段已随最后一个句柄消失。不碰残段，留给下一个创建者清理
            ::close(lockFd);
            return InitResult::kFailed;
        }
        if (const int blocker = orphanRemovalBlocker(shm); blocker != 0)
        {
            ::close(lockFd);
            diag.note(IpcDiagOp::kOpen, blocker, "segment exists but is not ours (another user?); not touching it");
            return diag.result();
        }
        if (::shm_unlink(shm.c_str()) != 0 && errno != ENOENT)
        {
            const int err = errno;
            ::close(lockFd);
            diag.note(IpcDiagOp::kUnlink, err, "cannot remove an orphaned segment before re-creating it");
            return diag.result();
        }
        shmFd = ::shm_open(shm.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
        if (shmFd < 0)
        {
            const int err = errno;
            ::close(lockFd);
            diag.note(IpcDiagOp::kOpen, err, "shm_open(O_CREAT|O_EXCL) failed");
            return diag.result();
        }
        if (::ftruncate(shmFd, static_cast<off_t>(size)) != 0)
        {
            const int err = errno;
            ::close(shmFd);
            ::shm_unlink(shm.c_str());
            ::close(lockFd);
            diag.note(IpcDiagOp::kTruncate, err, "ftruncate on a freshly created segment failed");
            return diag.result();
        }
        created = true;
        // EX → SH。BSD flock 的转换不是原子的（先放 EX 再上 SH），所以必须在全局锁下做
        if (flockRetry(lockFd, LOCK_SH | LOCK_NB) != 0)
        {
            const int err = errno;
            ::close(shmFd);
            ::shm_unlink(shm.c_str());
            ::close(lockFd);
            diag.note(IpcDiagOp::kSegmentLock, err, "cannot downgrade the segment lock to shared", lockPath);
            return diag.result();
        }
    }
    else
    {
        const int err = errno;
        if (err != EWOULDBLOCK)
        {
            ::close(lockFd);
            diag.note(IpcDiagOp::kSegmentLock, err, "flock probe on the segment lock file failed", lockPath);
            return diag.result();
        }
        // 有活持有者：附着。先拿 SH（全局锁下只有 SH 持有者，不会等），再打开段；绝不 ftruncate
        if (flockRetry(lockFd, LOCK_SH | LOCK_NB) != 0)
        {
            const int shErr = errno;
            ::close(lockFd);
            diag.note(IpcDiagOp::kSegmentLock, shErr, "cannot take the shared segment lock", lockPath);
            return diag.result();
        }
        const int oflag = (mode == Mode::kOpenReadOnly) ? O_RDONLY : O_RDWR;
        shmFd = ::shm_open(shm.c_str(), oflag, 0);
        if (shmFd < 0)
        {
            const int openErr = errno;
            ::close(lockFd);
            diag.note(IpcDiagOp::kOpen, openErr,
                      openErr == ENOENT ? "segment missing although its lock is held" : "shm_open(attach) failed");
            return diag.result();
        }
    }

    // 失败清理：自己刚建的段要撤掉（此刻只有自己持锁），附着的不动
    const auto failAfterOpen = [&](IpcDiagOp op, int err, const char* detail) {
        if (shmFd >= 0)
        {
            ::close(shmFd);
        }
        if (created)
        {
            ::shm_unlink(shm.c_str());
        }
        ::close(lockFd);
        diag.note(op, err, detail);
        return diag.result();
    };

    struct stat st
    {
    };
    if (::fstat(shmFd, &st) != 0)
    {
        return failAfterOpen(IpcDiagOp::kStat, errno, "fstat on the segment failed");
    }
    if (st.st_size <= 0)
    {
        return failAfterOpen(IpcDiagOp::kStat, EINVAL, "segment has no size");
    }
    const auto realSize = static_cast<std::size_t>(st.st_size);
    const int prot = (mode == Mode::kOpenReadOnly) ? PROT_READ : (PROT_READ | PROT_WRITE);
    void* base = ::mmap(nullptr, realSize, prot, MAP_SHARED, shmFd, 0);
    if (base == MAP_FAILED)
    {
        return failAfterOpen(IpcDiagOp::kMap, errno, "mmap of the segment failed");
    }
    ::close(shmFd); // 映射建立后 shm 的 fd 不再需要
    shmFd = -1;

    Mapping* m = record.release();
    m->lockFd = lockFd;
    m->base = base;
    m->size = realSize;
    track(m);

    view.base = base;
    view.size = realSize;
    view.mapping = m;
    view.created = created;
    view.name = std::move(viewName);
    // 锁页推迟到 initHeader 的建段分支（写触碰之后再锁，与 Win32 同序）；附着方不锁
    return InitResult::kOk;
}

void SegmentBackendPosix::leave(Mapping& m) noexcept
{
    PendingDiag diag;
    {
        const auto wait = lockWait(kLeaveLockWait);
        LifecycleLock life(m.lifecyclePath, /*create=*/false, wait);
        if (life.held())
        {
            suppressedLockFailures_.store(0, std::memory_order_relaxed);
            // SH → EX 的探测（不是原子的：先放 SH 再试 EX；失败时 SH 已经放掉，这正是离开要的）
            if (flockRetry(m.lockFd, LOCK_EX | LOCK_NB) == 0)
            {
                // 最后一个离开者：撤掉名字。别的进程里已有的映射不受影响
                if (::shm_unlink(m.shmName.c_str()) != 0 && errno != ENOENT)
                {
                    diag.note(IpcDiagOp::kUnlink, errno, "last holder could not unlink the segment");
                }
            }
        }
        else
        {
            // 拿不到全局锁就不做 unlink 判定（否则可能与别人的建段交错）；段留给下一个创建者清理
            noteLockFailure(life.error(), life.path(), wait, diag);
        }
        // 关锁 fd 即放掉剩下的锁；此刻全局锁（若拿到）仍在手里，整个离开落在锁内
        ::close(m.lockFd);
        m.lockFd = -1;
    }
    if (diag.set)
    {
        report(diag.op, diag.code, diag.error, m.shmName, diag.path, diag.detail, diag.suppressed);
    }
}

void SegmentBackendPosix::unmap(SegmentView& view)
{
    auto* m = static_cast<Mapping*>(view.mapping);
    if (m != nullptr)
    {
        if (m->base != nullptr)
        {
            ::munmap(m->base, m->size);
        }
        untrack(m);
        leave(*m);
        delete m;
    }
    else if (view.base != nullptr && view.size != 0)
    {
        ::munmap(view.base, view.size);
    }
    view.reset();
}

void SegmentBackendPosix::tryLock(const SegmentView& view)
{
    if (view.base == nullptr || view.size == 0)
    {
        return;
    }
    if (::mlock(view.base, view.size) != 0)
    {
        // 尽力而为：页面已在 initHeader 里写触碰过，锁不住只上报、不失败。这里不持全局锁
        const int err = errno;
        report(IpcDiagOp::kLockPages, IpcDiagCode::kOther, err, view.name, {}, "mlock failed; pages stay pageable");
    }
}

} // namespace rinne::ipc
