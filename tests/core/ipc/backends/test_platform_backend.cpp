// SPDX-License-Identifier: GPL-3.0-or-later
//
// 本机的真后端（Windows 上是 Win32，其余平台是 POSIX；Linux 上它只编进单测）：两个后端实例按同一个名字建或开，
// 看到的是同一块内存；同名段已存在时请求大小被忽略、回读真实大小；最后一个映射离开后段消失；坏名字与超长名字在
// 碰操作系统之前就被拒；初始化者死在认领之后时，按契约退避重试的几个持有者在有限时间内恢复。
// POSIX 另验锁文件的生命周期：残段清理、后端析构时补做离开、锁目录写不进去时归为被拒。
// 段名一律用随机的测试作用域，与维护者 DAW 里的正式版、并行跑的其他测试互不相见。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "DiagCapture.h"
#include "ipc/ISegmentBackend.h"
#include "ipc/Naming.h"
#include "ipc/ProcessInfo.h"

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
        return backend.initHeader(view, headerFields(rv->header), s,
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

namespace
{

// 一个按契约重试的持有者的结果：建或开 → 段头初始化；kBusy 就立刻解映射、按 RetryBackoff 退避后再试。
// 拿到 kOk 就一直映射着（它在用这个段），直到所有持有者都拿到结果
struct HolderOutcome
{
    InitResult result = InitResult::kBusy;
    int attempts = 0;
    int attemptsWhileStuck = 0; // 卡住的映射放手之前开始的尝试
    bool succeededWhileStuck = false;
    bool created = false; // 成功的那一次是不是它新建了段
    std::uint32_t generation = 0;
    std::chrono::steady_clock::duration doneAt{};
    std::chrono::steady_clock::duration longestAttempt{};
};

std::vector<HolderOutcome> runRetryingHolders(const BackendFixture& fixture, const std::string& name, int holders,
                                              const std::atomic<bool>& stuckGone,
                                              const std::chrono::steady_clock::time_point start,
                                              const std::function<void()>& whileRunning)
{
    const SegmentHeaderSpec spec{kRvMagic, kIpcAbi, kRvBytes, kRvHeaderBytes};
    std::vector<HolderOutcome> outcomes(static_cast<std::size_t>(holders));
    std::atomic<int> finished{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < holders; ++i)
    {
        threads.emplace_back([&, i] {
            HolderOutcome& out = outcomes[static_cast<std::size_t>(i)];
            auto backend = fixture.make();
            RetryBackoff backoff(0x5eed0000u + static_cast<std::uint64_t>(i));
            SegmentView kept;
            const auto deadline = start + std::chrono::seconds(20);
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (!stuckGone.load())
                {
                    ++out.attemptsWhileStuck;
                }
                const auto t0 = std::chrono::steady_clock::now();
                SegmentView view;
                InitResult r = backend->createOrOpen(name, kRvBytes, view);
                if (r == InitResult::kOk)
                {
                    auto* rv = static_cast<RvSegment*>(view.base);
                    r = backend->initHeader(view, headerFields(rv->header), spec, [rv] {
                        rv->header.layout_bytes.store(kRvBytes, std::memory_order_relaxed);
                    });
                }
                out.longestAttempt = std::max(out.longestAttempt, std::chrono::steady_clock::now() - t0);
                ++out.attempts;
                out.result = r;
                if (r == InitResult::kOk)
                {
                    // 标志在卡住的映射放手之前就置上：此刻仍为假，说明整次尝试期间它都还映射着
                    out.succeededWhileStuck = !stuckGone.load();
                    out.created = view.created;
                    out.generation = static_cast<RvSegment*>(view.base)->header.generation.load();
                    out.doneAt = std::chrono::steady_clock::now() - start;
                    kept = std::move(view);
                    break;
                }
                if (view.base != nullptr)
                {
                    backend->unmap(view); // 契约：先解映射，绝不在原映射上重试
                }
                if (r != InitResult::kBusy && r != InitResult::kFailed)
                {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(backoff.nextDelayMs()));
            }
            finished.fetch_add(1);
            while (finished.load() < holders)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (kept.base != nullptr)
            {
                backend->unmap(kept);
            }
        });
    }
    whileRunning();
    for (auto& t : threads)
    {
        t.join();
    }
    return outcomes;
}

// 一定已经不存在的 pid：POSIX 上超出 pid_t 的正数范围，Windows 上没有这么大的进程号
constexpr std::uint32_t kDeadPid = 0xfffffff0u;

} // namespace

TEST_CASE("retrying holders recover once a holder stuck in initialisation lets go",
          "[ipc][backend][platform][recovery]")
{
    // 初始化者认领之后就死了，它记下的 pid 又接管不了（这里写成本进程：活着；被复用的 pid 也是这样）。
    // 三个持有者按契约重试：看到 kBusy 立刻解映射、带抖动退避。卡住的映射（死者的句柄）放手之后，各持有者放手的空档
    // 总会重叠，段消失，下一次尝试由内核新建：恰好一个持有者新建并初始化，其余附着到同一个段
    BackendFixture fixture;
    const std::string name = segName(SegmentId::rendezvous());
    auto stuckBackend = fixture.make();
    SegmentView stuck;
    REQUIRE(stuckBackend->createOrOpen(name, kRvBytes, stuck) == InitResult::kOk);
    auto* stuckRv = static_cast<RvSegment*>(stuck.base);
    stuckRv->header.magic.store(kInitMarker);
    stuckRv->header.init_pid.store(currentProcessId());

    constexpr int kHolders = 3;
    std::atomic<bool> stuckGone{false};
    std::chrono::steady_clock::duration goneAt{};
    const auto start = std::chrono::steady_clock::now();
    const auto outcomes = runRetryingHolders(fixture, name, kHolders, stuckGone, start, [&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(400)); // 卡住期间各持有者都只拿到 kBusy
        goneAt = std::chrono::steady_clock::now() - start;
        stuckGone.store(true);
        stuckBackend->unmap(stuck); // 死者的句柄由内核关掉
    });

    int creators = 0;
    std::chrono::steady_clock::duration lastDone{};
    for (const auto& o : outcomes)
    {
        CHECK(o.result == InitResult::kOk);
        CHECK_FALSE(o.succeededWhileStuck);
        CHECK(o.attemptsWhileStuck >= 1);
        CHECK(o.generation == 1u); // 内核新建的段，初始化一次
        // 每次尝试都立刻返回，不在映射着段的情况下阻塞
        CHECK(o.longestAttempt < std::chrono::milliseconds(200));
        creators += o.created ? 1 : 0;
        lastDone = std::max(lastDone, o.doneAt);
    }
    CHECK(creators == 1);
    const auto recoveryMs = std::chrono::duration_cast<std::chrono::milliseconds>(lastDone - goneAt).count();
    INFO("recovered " << recoveryMs << " ms after the stuck holder let go");
    CHECK(recoveryMs < 5000);
}

TEST_CASE("retrying holders take over a segment whose initialiser died, without waiting for anyone to let go",
          "[ipc][backend][platform][recovery]")
{
    // 初始化者记下的 pid 已经死了：不必等谁放手，恰好一个持有者用 CAS 接管、重初始化，其余附着到它发布的段
    BackendFixture fixture;
    REQUIRE_FALSE(isProcessAlive(kDeadPid));
    const std::string name = segName(SegmentId::rendezvous());
    auto stuckBackend = fixture.make();
    SegmentView stuck;
    REQUIRE(stuckBackend->createOrOpen(name, kRvBytes, stuck) == InitResult::kOk);
    auto* stuckRv = static_cast<RvSegment*>(stuck.base);
    stuckRv->header.magic.store(kInitMarker);
    stuckRv->header.init_pid.store(kDeadPid);
    stuckRv->groups[0].input.pid.store(4242); // 死者写了一半的东西

    const std::atomic<bool> stuckGone{false};
    const auto outcomes = runRetryingHolders(fixture, name, 3, stuckGone, std::chrono::steady_clock::now(), [] {});
    for (const auto& o : outcomes)
    {
        CHECK(o.result == InitResult::kOk);
        CHECK(o.generation == 1u); // 恰好一次接管：0 + 1
        CHECK(o.doneAt < std::chrono::seconds(5));
    }
    CHECK(stuckRv->header.magic.load() == kRvMagic);
    CHECK(stuckRv->header.init_pid.load() == currentProcessId());
    CHECK(stuckRv->groups[0].input.pid.load() == 0u);
    stuckBackend->unmap(stuck);
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
