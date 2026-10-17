// SPDX-License-Identifier: GPL-3.0-or-later
//
// 本机的真后端（Windows 上是 Win32，其余平台是 POSIX；Linux 上它只编进单测）：两个后端实例按同一个名字建或开，
// 看到的是同一块内存；同名段已存在时请求大小被忽略、回读真实大小；最后一个映射离开后段消失；坏名字与超长名字在
// 碰操作系统之前就被拒。POSIX 另验锁文件的生命周期：残段清理、后端析构时补做离开、锁目录写不进去时归为被拒。
// 段名一律用随机的测试作用域，与维护者 DAW 里的正式版、并行跑的其他测试互不相见。

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <memory>
#include <string>

#include "DiagCapture.h"
#include "ipc/ISegmentBackend.h"
#include "ipc/Naming.h"

#if defined(_WIN32)
#include "ipc/SegmentBackendWin32.h"
#else
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "TempDir.h"
#include "ipc/SegmentBackendPosix.h"
#endif

namespace
{

using namespace rinne::ipc;

#if defined(_WIN32)

struct BackendFixture
{
    std::unique_ptr<ISegmentBackend> make() const { return std::make_unique<SegmentBackendWin32>(); }
};

#else

// 每个用例一个临时锁目录（本进程 mkdtemp 新建，用完由 TempDir 核过路径再删）
struct BackendFixture
{
    std::unique_ptr<SegmentBackendPosix> make() const
    {
        return std::make_unique<SegmentBackendPosix>(SegmentBackendPosix::LockDirForTesting{lockDir});
    }

    rinne::test::TempDir dir;
    const std::string lockDir = dir.path();
};

#endif

std::string segName(SegmentId id = SegmentId::groupData(5))
{
    std::string name;
    REQUIRE(Naming(NameScope::randomForTesting()).name(id, name) == NameStatus::kOk);
    return name;
}

} // namespace

TEST_CASE("two backend instances share one segment and read back its real size", "[ipc][backend][platform]")
{
    BackendFixture fixture;
    auto a = fixture.make();
    auto b = fixture.make();
    const std::string name = segName();

    SegmentView va;
    REQUIRE(a->createOrOpen(name, kRingBytes, va) == InitResult::kOk);
    CHECK(va.created);
    CHECK(va.size >= kRingBytes);
    CHECK(va.name == name);
    static_cast<unsigned char*>(va.base)[kRingBytes - 1] = 0x5a;

    // 同名段已存在：请求更大的尺寸也只拿到原来那么大
    SegmentView vb;
    REQUIRE(b->createOrOpen(name, kRingBytes * 2, vb) == InitResult::kOk);
    CHECK_FALSE(vb.created);
    CHECK(vb.size == va.size);
    CHECK(static_cast<const unsigned char*>(vb.base)[kRingBytes - 1] == 0x5a);

    SegmentView ro;
    REQUIRE(b->openExistingReadOnly(name, ro) == InitResult::kOk);
    CHECK(static_cast<const unsigned char*>(ro.base)[kRingBytes - 1] == 0x5a);

    b->unmap(ro);
    b->unmap(vb);
    CHECK(vb.base == nullptr);
    SegmentView again;
    REQUIRE(b->openExisting(name, again) == InitResult::kOk); // a 还映射着
    b->unmap(again);
    a->unmap(va);

    // 最后一个映射离开后段就没了
    SegmentView gone;
    CHECK(b->openExisting(name, gone) == InitResult::kFailed);
    CHECK(gone.base == nullptr);
}

TEST_CASE("the full header protocol works across two backend instances", "[ipc][backend][platform][header]")
{
    BackendFixture fixture;
    auto a = fixture.make();
    auto b = fixture.make();
    const std::string name = segName(SegmentId::rendezvous());
    const SegmentHeaderSpec spec{kRvMagic, kIpcAbi, kRvBytes, kRvHeaderBytes};
    const auto init = [&](ISegmentBackend& backend, SegmentView& view, const SegmentHeaderSpec& s) {
        auto* rv = static_cast<RvSegment*>(view.base);
        return backend.initHeader(view, &rv->header.magic, &rv->header.abi, &rv->header.generation, s,
                                  [&] { rv->header.layout_bytes.store(kRvBytes, std::memory_order_relaxed); });
    };

    SegmentView va;
    REQUIRE(a->createOrOpen(name, kRvBytes, va) == InitResult::kOk);
    REQUIRE(init(*a, va, spec) == InitResult::kOk);
    SegmentView vb;
    REQUIRE(b->createOrOpen(name, kRvBytes, vb) == InitResult::kOk);
    CHECK(init(*b, vb, spec) == InitResult::kOk);
    SegmentHeaderSpec abi2 = spec;
    abi2.abi = 2;
    SegmentView vc;
    REQUIRE(b->createOrOpen(name, kRvBytes, vc) == InitResult::kOk);
    CHECK(init(*b, vc, abi2) == InitResult::kAbiMismatch);
    b->unmap(vc);
    b->unmap(vb);
    a->unmap(va);
}

TEST_CASE("missing segments fail quietly, bad names fail loudly", "[ipc][backend][platform]")
{
    BackendFixture fixture;
    auto backend = fixture.make();
    rinne::test::DiagCapture diag;

    SegmentView view;
    CHECK(backend->openExisting(segName(), view) == InitResult::kFailed);
    CHECK(backend->openExistingReadOnly(segName(), view) == InitResult::kFailed);
    CHECK(diag.events().empty()); // 25 Hz 重试路径上的常态，不刷诊断

#if defined(_WIN32)
    const std::string tooLong = "Local\\" + std::string(26, 'a');
    const std::string badHead = "/SnchRinne.rv";
#else
    const std::string tooLong = "/" + std::string(31, 'a');
    const std::string badHead = "Local\\SnchRinne.rv";
#endif
    CHECK(backend->createOrOpen(tooLong, kRvBytes, view) == InitResult::kFailedBadName);
    CHECK(diag.has(IpcDiagCode::kNameTooLong));
    CHECK(backend->openExisting(tooLong, view) == InitResult::kFailedBadName);
    diag.clear();
    CHECK(backend->createOrOpen(badHead, kRvBytes, view) == InitResult::kFailedBadName);
    CHECK_FALSE(diag.events().empty());
    CHECK(view.base == nullptr);
}

#if !defined(_WIN32)

TEST_CASE("an orphaned segment with no lock holder is cleared and re-created", "[ipc][backend][posix]")
{
    BackendFixture fixture;
    const std::string name = segName();
    // 崩溃留下的残段：有名字、有旧内容、没有任何锁持有者
    const int fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    REQUIRE(fd >= 0);
    REQUIRE(::ftruncate(fd, 4096) == 0);
    void* old = ::mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    REQUIRE(old != MAP_FAILED);
    std::memset(old, 0x77, 4096);
    ::munmap(old, 4096);
    ::close(fd);

    auto backend = fixture.make();
    SegmentView view;
    REQUIRE(backend->createOrOpen(name, kRingBytes, view) == InitResult::kOk);
    CHECK(view.created);
    CHECK(view.size >= kRingBytes); // 旧尺寸的残段被撤掉，按新尺寸重建
    CHECK(static_cast<const unsigned char*>(view.base)[0] == 0);
    backend->unmap(view);
}

TEST_CASE("destroying the backend finishes leaving for views it never unmapped", "[ipc][backend][posix]")
{
    BackendFixture fixture;
    const std::string name = segName();
    {
        auto backend = fixture.make();
        SegmentView view;
        REQUIRE(backend->createOrOpen(name, kRingBytes, view) == InitResult::kOk);
        // 不 unmap 就丢掉后端：映射留到进程退出，锁的那一半在析构里补做
    }
    auto other = fixture.make();
    SegmentView view;
    CHECK(other->openExisting(name, view) == InitResult::kFailed);
}

TEST_CASE("an unwritable lock directory is classified as denied", "[ipc][backend][posix][denied]")
{
    if (::geteuid() == 0)
    {
        SKIP("root 不受目录权限限制");
    }
    BackendFixture fixture;
    const std::string locked = fixture.lockDir + "/locked";
    REQUIRE(::mkdir(locked.c_str(), 0500) == 0);
    SegmentBackendPosix backend(SegmentBackendPosix::LockDirForTesting{locked});
    rinne::test::DiagCapture diag;
    SegmentView view;
    CHECK(backend.createOrOpen(segName(), kRingBytes, view) == InitResult::kFailedDenied);
    CHECK(diag.has(IpcDiagCode::kShmDenied));
    CHECK(view.base == nullptr);
}

TEST_CASE("a held lifecycle lock fails retries at once and is reported with the skipped count",
          "[ipc][backend][posix][lock]")
{
    BackendFixture fixture;
    auto backend = fixture.make();
    const std::string name = segName();
    SegmentView view;
    REQUIRE(backend->createOrOpen(name, kRingBytes, view) == InitResult::kOk); // 建好锁目录与 lifecycle.lock

    // 别的进程拿着全局锁不放（这里用同一进程里另开的 fd 模拟：flock 按打开的文件描述互斥）
    const std::string lifecycle = fixture.lockDir + "/" + SegmentBackendPosix::kLifecycleLockName;
    const int holder = ::open(lifecycle.c_str(), O_RDONLY | O_CLOEXEC);
    REQUIRE(holder >= 0);
    REQUIRE(::flock(holder, LOCK_EX | LOCK_NB) == 0);

    rinne::test::DiagCapture diag;
    // 重试路径（附着）只试一次、不等
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 3; ++i)
    {
        SegmentView attach;
        CHECK(backend->openExisting(name, attach) == InitResult::kFailed);
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(elapsed < std::chrono::milliseconds(100));
    CHECK(diag.events().empty()); // 只计数

    // 建段路径有界等待，等满之后报一条，带上前面略去的 3 次
    SegmentView create;
    CHECK(backend->createOrOpen(segName(), kRingBytes, create) == InitResult::kFailed);
    const auto events = diag.events();
    REQUIRE(events.size() == 1);
    CHECK(events[0].op == IpcDiagOp::kLifecycleLock);
    CHECK(events[0].code == IpcDiagCode::kBusy);
    CHECK(events[0].suppressed == 3u);

    ::close(holder);
    // 锁放开以后照常；略去的计数已经随上一条报出，不会再报一遍
    diag.clear();
    SegmentView again;
    REQUIRE(backend->openExisting(name, again) == InitResult::kOk);
    backend->unmap(again);
    backend->unmap(view);
    CHECK(diag.events().empty());
}

TEST_CASE("the temp lock directory guard only accepts its own directory", "[ipc][backend][posix]")
{
    // 只核判定，不在任何真实路径上触发删除
    using rinne::test::TempDir;
    CHECK_FALSE(TempDir::safeToRemove("/"));
    CHECK_FALSE(TempDir::safeToRemove(std::filesystem::temp_directory_path().string()));
    CHECK_FALSE(TempDir::safeToRemove(SegmentBackendPosix::defaultLockDir()));
    CHECK_FALSE(TempDir::safeToRemove("relative/path"));
    const TempDir dir;
    CHECK(TempDir::safeToRemove(dir.path()));
    CHECK_FALSE(TempDir::safeToRemove(dir.path() + "/.."));
}

TEST_CASE("the default lock directory lives under the real home", "[ipc][backend][posix]")
{
    const std::string dir = SegmentBackendPosix::defaultLockDir();
    REQUIRE_FALSE(dir.empty());
    CHECK(dir.front() == '/');
    const std::string tail = "/Library/Application Support/Synchain/Rinne/ipc";
    REQUIRE(dir.size() > tail.size());
    CHECK(dir.compare(dir.size() - tail.size(), tail.size(), tail) == 0);
}

#endif
