// SPDX-License-Identifier: GPL-3.0-or-later
//
// 段头初始化协议（平台无关的那一半），在进程内后端上验：建段者认领后记下自己的 pid、最后发布 magic；附着方比对
// magic、abi 与真实大小；abi 不同的一方读得出对方的 abi 与版本、但拿到 kAbiMismatch；magic 不是我们的不碰；
// 没人认领的段由附着方认领；初始化者死了（pid 已死）时恰好一个附着方接管，活着或 pid 还没写下时立刻 kBusy、不清零；
// 只读校验绝不写；kBusy 之后的退避间隔。
// 竞争用测试钩子确定性地构造：输家停在「看到 0、认领之前」或「看到死者、接管之前」，等胜者发布并开始用这个段之后
// 才放行，断言胜者写下的东西一个字节都没被动过。

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "DiagCapture.h"
#include "ipc/ISegmentBackend.h"
#include "ipc/Naming.h"
#include "ipc/ProcessInfo.h"
#include "ipc/SegmentBackendInProcess.h"

namespace
{

using namespace rinne::ipc;

const SegmentHeaderSpec kRvSpec{kRvMagic, kIpcAbi, kRvBytes, kRvHeaderBytes};
// 数据段从 16 字节起清零：清零范围盖住 init_pid（偏移 60），它必须被跳过
const SegmentHeaderSpec kRingSpec{kRingMagic, kIpcAbi, kRingBytes, 16};

// 一定已经不存在的 pid：POSIX 上超出 pid_t 的正数范围，Windows 上没有这么大的进程号
constexpr std::uint32_t kDeadPid = 0xfffffff0u;

// 数据区里第 1023 个样本的最高字节正好是一页的最后一个字节（16384 + 4095）：建段者的写触碰会把它写成 0
constexpr std::size_t kPageEndSample = 1023;

std::string segName(const Naming& naming, SegmentId id)
{
    std::string name;
    REQUIRE(naming.name(id, name) == NameStatus::kOk);
    return name;
}

// initData 的观测：调了几次、每次调用时 magic 是不是初始化标记、init_pid 是不是已经记成本进程（几何字段必须在
// 认领并记下 pid 之后、发布之前写）。原子量：竞争用例在两个线程上同时初始化，Catch2 的断言不能在别的线程上用
struct InitProbe
{
    std::atomic<int> calls{0};
    std::atomic<bool> markerHeld{true};
    std::atomic<bool> pidRecorded{true};

    void observe(const SegmentHeaderFields& h)
    {
        calls.fetch_add(1);
        if (h.magic->load() != kInitMarker)
        {
            markerHeld.store(false);
        }
        if (h.initPid->load() != currentProcessId())
        {
            pidRecorded.store(false);
        }
    }
};

InitResult initRv(ISegmentBackend& backend, SegmentView& view, const SegmentHeaderSpec& spec = kRvSpec,
                  bool allowOverwrite = true, InitProbe* probe = nullptr)
{
    auto* rv = static_cast<RvSegment*>(view.base);
    const SegmentHeaderFields h = headerFields(rv->header);
    return backend.initHeader(
        view, h, spec,
        [&] {
            rv->header.layout_bytes.store(kRvBytes, std::memory_order_relaxed);
            rv->header.groups.store(kGroupCount, std::memory_order_relaxed);
            rv->header.group_stride.store(kGroupStride, std::memory_order_relaxed);
            if (probe != nullptr)
            {
                probe->observe(h);
            }
        },
        allowOverwrite);
}

InitResult initRing(ISegmentBackend& backend, SegmentView& view, InitProbe* probe = nullptr)
{
    auto* ring = static_cast<RingSegment*>(view.base);
    const SegmentHeaderFields h = headerFields(ring->header);
    return backend.initHeader(view, h, kRingSpec, [&] {
        ring->header.ring_frames.store(kRingFrames, std::memory_order_relaxed);
        ring->header.ring_channels.store(kRingChannels, std::memory_order_relaxed);
        if (probe != nullptr)
        {
            probe->observe(h);
        }
    });
}

// 让一个线程停在钩子里，直到主线程放行。等不到对方时按超时失败，不会把用例挂死
class Gate
{
public:
    void arriveAndWait() noexcept
    {
        reached_.store(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!opened_.load() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::yield();
        }
    }
    bool waitUntilReached() const
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!reached_.load())
        {
            if (std::chrono::steady_clock::now() > deadline)
            {
                return false;
            }
            std::this_thread::yield();
        }
        return true;
    }
    void open() noexcept { opened_.store(true); }

private:
    std::atomic<bool> reached_{false};
    std::atomic<bool> opened_{false};
};

// 进程内后端外面包一层：在指定的交错点上停住调用线程
class HookBackend final : public ISegmentBackend
{
public:
    HookBackend() = default;
    HookBackend(InitStage stage, Gate& gate) : stage_(stage), gate_(&gate) {}
    ~HookBackend() override { forgetRetired(); }

    InitResult createOrOpen(const std::string& name, std::size_t size, SegmentView& view) override
    {
        return inner_.createOrOpen(name, size, view);
    }
    InitResult openExisting(const std::string& name, SegmentView& view) override
    {
        return inner_.openExisting(name, view);
    }
    void unmap(SegmentView& view) noexcept override { inner_.unmap(view); }

protected:
    void onInitStageForTesting(InitStage stage, const SegmentView& view) noexcept override
    {
        (void)view;
        if (gate_ != nullptr && stage == stage_)
        {
            gate_->arriveAndWait();
        }
    }

private:
    SegmentBackendInProcess inner_;
    InitStage stage_ = InitStage::kBeforeClaim;
    Gate* gate_ = nullptr;
};

} // namespace

TEST_CASE("the creator records its pid, publishes magic last and an attacher accepts it", "[ipc][backend][header]")
{
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    const std::string name = segName(naming, SegmentId::rendezvous());
    SegmentBackendInProcess backend;

    SegmentView creator;
    REQUIRE(backend.createOrOpen(name, kRvBytes, creator) == InitResult::kOk);
    REQUIRE(creator.created);
    CHECK(creator.name == name);
    InitProbe probe;
    REQUIRE(initRv(backend, creator, kRvSpec, true, &probe) == InitResult::kOk);
    CHECK(probe.calls.load() == 1);
    CHECK(probe.markerHeld.load()); // 几何字段在认领之后、发布之前写
    CHECK(probe.pidRecorded.load()); // pid 在认领之后、写任何东西之前记下
    const auto* rv = static_cast<const RvSegment*>(creator.base);
    CHECK(rv->header.magic.load() == kRvMagic);
    CHECK(rv->header.abi.load() == kIpcAbi);
    CHECK(rv->header.generation.load() == 1u);
    CHECK(rv->header.layout_bytes.load() == kRvBytes);
    CHECK(rv->header.init_pid.load() == currentProcessId());

    SegmentView attacher;
    REQUIRE(backend.createOrOpen(name, kRvBytes, attacher) == InitResult::kOk);
    CHECK_FALSE(attacher.created);
    CHECK(attacher.base == creator.base);
    InitProbe attach;
    CHECK(initRv(backend, attacher, kRvSpec, true, &attach) == InitResult::kOk);
    CHECK(attach.calls.load() == 0); // 附着方不改几何
    CHECK(rv->header.generation.load() == 1u);
}

TEST_CASE("a different abi is visible through the frozen prologue and the slot version", "[ipc][backend][header][abi]")
{
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    const std::string name = segName(naming, SegmentId::rendezvous());
    SegmentBackendInProcess backend;
    rinne::test::DiagCapture diag;

    SegmentView v1;
    REQUIRE(backend.createOrOpen(name, kRvBytes, v1) == InitResult::kOk);
    REQUIRE(initRv(backend, v1) == InitResult::kOk);
    auto* rv = static_cast<RvSegment*>(v1.base);
    rv->groups[2].output.version.store(packVersion(0, 9, 0));

    // 一个 abi 2 的实例打开同名的会合段（会合段名不带 abi）：拿到 kAbiMismatch，从不写
    SegmentView v2;
    REQUIRE(backend.createOrOpen(name, kRvBytes, v2) == InitResult::kOk);
    SegmentHeaderSpec abi2 = kRvSpec;
    abi2.abi = 2;
    CHECK(initRv(backend, v2, abi2) == InitResult::kAbiMismatch);
    CHECK(diag.has(IpcDiagCode::kAbiMismatch));
    CHECK(rv->header.abi.load() == 1u);

    // 读得出对方：前 16 字节与端点槽 version 的绝对位置永久冻结，按字节偏移读，不依赖本版本的结构体；
    // 读 version 之前先核视图不小于永久冻结的下限
    REQUIRE(v2.size >= kRvMinBytes);
    const auto* bytes = static_cast<const unsigned char*>(v2.base);
    std::uint32_t magic = 0;
    std::uint32_t abi = 0;
    std::uint32_t version = 0;
    std::memcpy(&magic, bytes + 0, 4);
    std::memcpy(&abi, bytes + 4, 4);
    std::memcpy(&version, bytes + slotVersionOffset(3, true), 4);
    CHECK(slotVersionOffset(3, true) == 1024u * 3 + 256u + 56u);
    CHECK(magic == kRvMagic);
    CHECK(abi == 1u);
    CHECK(version == packVersion(0, 9, 0));
}

TEST_CASE("a foreign magic or a too small segment is never used", "[ipc][backend][header]")
{
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    SegmentBackendInProcess backend;
    rinne::test::DiagCapture diag;

    SECTION("foreign magic")
    {
        const std::string name = segName(naming, SegmentId::rendezvous());
        SegmentView view;
        REQUIRE(backend.createOrOpen(name, kRvBytes, view) == InitResult::kOk);
        static_cast<RvSegment*>(view.base)->header.magic.store(rinne::util::fourcc("XXXX"));
        SegmentView other;
        REQUIRE(backend.createOrOpen(name, kRvBytes, other) == InitResult::kOk);
        CHECK(initRv(backend, other) == InitResult::kFailed);
        CHECK(diag.has(IpcDiagCode::kForeign));
        CHECK(static_cast<RvSegment*>(view.base)->header.magic.load() == rinne::util::fourcc("XXXX"));
    }
    SECTION("existing segment smaller than the layout")
    {
        const std::string name = segName(naming, SegmentId::groupData(4));
        SegmentView small;
        REQUIRE(backend.createOrOpen(name, 4096, small) == InitResult::kOk);
        // 建段时就比布局小：不初始化
        CHECK(initRv(backend, small) == InitResult::kFailed);
        CHECK(diag.has(IpcDiagCode::kBadSize));
        auto* header = static_cast<RvHeader*>(small.base);
        CHECK(header->magic.load() == 0u);
        // 没发布、又比布局小：附着方也不认领
        SegmentView unclaimed;
        REQUIRE(backend.createOrOpen(name, kRvBytes, unclaimed) == InitResult::kOk);
        diag.clear();
        CHECK(initRv(backend, unclaimed) == InitResult::kFailed);
        CHECK(diag.has(IpcDiagCode::kBadSize));
        CHECK(header->magic.load() == 0u);
        // 头是对的，大小不够：附着方也不用
        header->abi.store(kIpcAbi);
        header->magic.store(kRvMagic);
        SegmentView again;
        REQUIRE(backend.createOrOpen(name, kRvBytes, again) == InitResult::kOk);
        CHECK(again.size == 4096u); // 回读真实大小，不是请求值
        diag.clear();
        CHECK(initRv(backend, again) == InitResult::kFailed);
        CHECK(diag.has(IpcDiagCode::kBadSize));
    }
}

TEST_CASE("read-only checks never write", "[ipc][backend][header]")
{
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    const std::string name = segName(naming, SegmentId::rendezvous());
    SegmentBackendInProcess backend;
    rinne::test::DiagCapture diag;

    SegmentView creator;
    REQUIRE(backend.createOrOpen(name, kRvBytes, creator) == InitResult::kOk);
    const auto* rv = static_cast<const RvSegment*>(creator.base);
    // 创建者还没初始化完：只读方与不许认领的附着方都立刻返回 kBusy，不报诊断，下一拍重试
    CHECK(backend.checkHeaderReadOnly(creator, rv->header.magic, rv->header.abi, kRvSpec) == InitResult::kBusy);
    SegmentView early;
    REQUIRE(backend.openExisting(name, early) == InitResult::kOk);
    CHECK(initRv(backend, early, kRvSpec, /*allowOverwrite=*/false) == InitResult::kBusy);
    CHECK(rv->header.magic.load() == 0u);
    CHECK(rv->header.abi.load() == 0u);
    CHECK(rv->header.init_pid.load() == 0u);
    CHECK(diag.events().empty());

    REQUIRE(initRv(backend, creator) == InitResult::kOk);
    SegmentView reader;
    REQUIRE(backend.openExistingReadOnly(name, reader) == InitResult::kOk);
    CHECK(backend.checkHeaderReadOnly(reader, rv->header.magic, rv->header.abi, kRvSpec) == InitResult::kOk);
    SegmentHeaderSpec abi2 = kRvSpec;
    abi2.abi = 2;
    CHECK(backend.checkHeaderReadOnly(reader, rv->header.magic, rv->header.abi, abi2) == InitResult::kAbiMismatch);
}

TEST_CASE("an attacher initialises a segment nobody has claimed", "[ipc][backend][header]")
{
    // 创建者死在认领之前（或者只是慢）：magic 为 0 的段从没被谁用过，附着方立刻认领、从 clearFrom 清零后初始化
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    const std::string name = segName(naming, SegmentId::rendezvous());
    SegmentBackendInProcess backend;

    SegmentView dead;
    REQUIRE(backend.createOrOpen(name, kRvBytes, dead) == InitResult::kOk);
    auto* rv = static_cast<RvSegment*>(dead.base);
    rv->groups[0].input.pid.store(4242); // 防御：就算有垃圾也清掉

    SegmentView attacher;
    REQUIRE(backend.createOrOpen(name, kRvBytes, attacher) == InitResult::kOk);
    REQUIRE_FALSE(attacher.created);
    InitProbe probe;
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(initRv(backend, attacher, kRvSpec, true, &probe) == InitResult::kOk);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(50)); // 不等谁
    CHECK(probe.calls.load() == 1);
    CHECK(probe.markerHeld.load());
    CHECK(probe.pidRecorded.load());
    CHECK(rv->header.magic.load() == kRvMagic);
    CHECK(rv->header.generation.load() == 1u);
    CHECK(rv->groups[0].input.pid.load() == 0u); // clearFrom 起清零
}

TEST_CASE("an attacher that loses the claim after the winner started using the segment leaves it intact",
          "[ipc][backend][header][interleave]")
{
    // 输家看到 0 之后被挂起，胜者认领、初始化、发布并开始用（认领一个槽）之后输家才醒：它的认领必然失败，
    // 从不清零、不重写 generation，附着到胜者发布的段
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    const std::string name = segName(naming, SegmentId::rendezvous());
    SegmentBackendInProcess creatorBackend;
    SegmentView dead;
    REQUIRE(creatorBackend.createOrOpen(name, kRvBytes, dead) == InitResult::kOk); // 建段者死在认领之前
    auto* rv = static_cast<RvSegment*>(dead.base);

    Gate gate;
    HookBackend loserBackend(ISegmentBackend::InitStage::kBeforeClaim, gate);
    HookBackend winnerBackend;
    InitProbe probe;
    InitResult loser = InitResult::kFailed;
    std::thread t([&] {
        SegmentView view;
        if (loserBackend.createOrOpen(name, kRvBytes, view) == InitResult::kOk)
        {
            loser = initRv(loserBackend, view, kRvSpec, true, &probe);
        }
    });
    REQUIRE(gate.waitUntilReached());

    SegmentView winner;
    REQUIRE(winnerBackend.createOrOpen(name, kRvBytes, winner) == InitResult::kOk);
    REQUIRE(initRv(winnerBackend, winner, kRvSpec, true, &probe) == InitResult::kOk);
    rv->groups[0].input.ctl.store(makeCtl(1, kCtlPhaseActive));
    rv->groups[0].input.pid.store(100);
    gate.open();
    t.join();

    CHECK(loser == InitResult::kOk);
    CHECK(probe.calls.load() == 1);
    CHECK(rv->header.generation.load() == 1u);
    CHECK(rv->groups[0].input.ctl.load() == makeCtl(1, kCtlPhaseActive));
    CHECK(rv->groups[0].input.pid.load() == 100u);
}

TEST_CASE("a creator that wakes after an attacher initialised the segment does not initialise it again",
          "[ipc][backend][header][interleave]")
{
    // 建段者拿到 created 之后、认领之前被挂起；附着方认领、初始化、发布并开始写环。建段者醒来后认领失败，
    // 不写触碰（那是非原子的逐字节写，会踩掉对端正在写的样本）、不重写 generation、不重写几何
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    const std::string name = segName(naming, SegmentId::groupData(2));
    Gate gate;
    HookBackend creatorBackend(ISegmentBackend::InitStage::kBeforeClaim, gate);
    HookBackend attacherBackend;
    InitProbe probe;
    InitResult creator = InitResult::kFailed;
    bool created = false;
    std::thread t([&] {
        SegmentView view;
        if (creatorBackend.createOrOpen(name, kRingBytes, view) == InitResult::kOk)
        {
            created = view.created;
            creator = initRing(creatorBackend, view, &probe);
        }
    });
    REQUIRE(gate.waitUntilReached());

    SegmentView attacher;
    REQUIRE(attacherBackend.createOrOpen(name, kRingBytes, attacher) == InitResult::kOk);
    REQUIRE_FALSE(attacher.created);
    REQUIRE(initRing(attacherBackend, attacher, &probe) == InitResult::kOk);
    auto* ring = static_cast<RingSegment*>(attacher.base);
    ring->header.writer_token.store(7);
    ring->data[kPageEndSample].store(1.0f, std::memory_order_relaxed);
    ring->header.write_head.store(4096);
    gate.open();
    t.join();

    CHECK(created);
    CHECK(creator == InitResult::kOk);
    CHECK(probe.calls.load() == 1);
    CHECK(ring->header.generation.load() == 1u);
    CHECK(ring->header.writer_token.load() == 7u);
    CHECK(ring->data[kPageEndSample].load() == 1.0f);
    CHECK(ring->header.write_head.load() == 4096u);
}

TEST_CASE("two attachers racing to initialise elect exactly one initialiser", "[ipc][backend][header][threads]")
{
    // 不加钩子的真竞争：只能有一方初始化；另一方附着到它发布的段，绝不在胜者开始用这一段之后再清零
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    const std::string name = segName(naming, SegmentId::rendezvous());
    SegmentBackendInProcess backend;
    SegmentView dead;
    REQUIRE(backend.createOrOpen(name, kRvBytes, dead) == InitResult::kOk);
    auto* rv = static_cast<RvSegment*>(dead.base);

    InitProbe probe;
    std::atomic<int> ready{0};
    InitResult results[2] = {InitResult::kFailed, InitResult::kFailed};
    const auto attach = [&](int who) {
        SegmentView view;
        if (backend.createOrOpen(name, kRvBytes, view) != InitResult::kOk)
        {
            return;
        }
        ready.fetch_add(1);
        while (ready.load() < 2)
        {
            std::this_thread::yield();
        }
        // 输家看到的可能是初始化标记（胜者还在写）：按契约先解映射，退避后重新映射再试
        RetryBackoff backoff(static_cast<std::uint64_t>(who) + 1);
        results[who] = initRv(backend, view, kRvSpec, true, &probe);
        for (int i = 0; i < 100 && results[who] == InitResult::kBusy; ++i)
        {
            backend.unmap(view);
            std::this_thread::sleep_for(std::chrono::milliseconds(backoff.nextDelayMs() / 25));
            if (backend.createOrOpen(name, kRvBytes, view) != InitResult::kOk)
            {
                return;
            }
            results[who] = initRv(backend, view, kRvSpec, true, &probe);
        }
        if (results[who] == InitResult::kOk)
        {
            // 拿到 kOk 就开始用：各认领一个槽。输掉的一方若在这之后清零，这里写下的就没了
            rv->groups[who].input.pid.store(100u + static_cast<u32>(who));
        }
    };
    std::thread a(attach, 0);
    std::thread b(attach, 1);
    a.join();
    b.join();

    CHECK(results[0] == InitResult::kOk);
    CHECK(results[1] == InitResult::kOk);
    CHECK(probe.calls.load() == 1);
    CHECK(probe.markerHeld.load());
    CHECK(rv->header.generation.load() == 1u);
    CHECK(rv->groups[0].input.pid.load() == 100u);
    CHECK(rv->groups[1].input.pid.load() == 101u);
}

TEST_CASE("an initialiser that is alive or has not recorded its pid yet is never taken over",
          "[ipc][backend][header][busy]")
{
    // magic 停在初始化标记：初始化者 pid 还没写下（认领之后的那一瞬），或者它还活着（含被复用的 pid、本进程）。
    // 附着方立刻返回 kBusy（诊断 kBusy），不睡、不清零、不改 pid；只读方从一开始就按「还没初始化完」处理、不报诊断
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    const std::string name = segName(naming, SegmentId::rendezvous());
    SegmentBackendInProcess backend;
    rinne::test::DiagCapture diag;
    SegmentView dead;
    REQUIRE(backend.createOrOpen(name, kRvBytes, dead) == InitResult::kOk);
    auto* rv = static_cast<RvSegment*>(dead.base);
    rv->header.magic.store(kInitMarker);
    rv->groups[0].input.pid.store(7);
    std::uint32_t owner = 0;
    SECTION("pid not recorded yet")
    {
        owner = 0;
    }
    SECTION("initialiser alive")
    {
        owner = currentProcessId();
    }
    rv->header.init_pid.store(owner);

    CHECK(backend.checkHeaderReadOnly(dead, rv->header.magic, rv->header.abi, kRvSpec) == InitResult::kBusy);
    CHECK(diag.events().empty());
    SegmentView attacher;
    REQUIRE(backend.createOrOpen(name, kRvBytes, attacher) == InitResult::kOk);
    InitProbe probe;
    const auto t0 = std::chrono::steady_clock::now();
    CHECK(initRv(backend, attacher, kRvSpec, true, &probe) == InitResult::kBusy);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(50)); // 立刻返回，不阻塞
    CHECK(diag.has(IpcDiagCode::kBusy));
    CHECK(probe.calls.load() == 0);
    CHECK(rv->header.magic.load() == kInitMarker);
    CHECK(rv->header.init_pid.load() == owner);
    CHECK(rv->groups[0].input.pid.load() == 7u);
}

TEST_CASE("a dead initialiser is taken over by exactly one attacher and the loser leaves the segment intact",
          "[ipc][backend][header][interleave]")
{
    // 初始化者记下 pid 之后死了：附着方用 CAS 把 init_pid 从死者换成自己来接管，清零（跳过 init_pid）、generation + 1、
    // 重写几何、发布。另一个附着方在接管之前被挂起，等胜者发布并开始写环之后才醒：CAS 失败，附着到胜者的段，什么都不动
    REQUIRE_FALSE(isProcessAlive(kDeadPid));
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    const std::string name = segName(naming, SegmentId::groupData(6));
    SegmentBackendInProcess deadBackend;
    SegmentView dead;
    REQUIRE(deadBackend.createOrOpen(name, kRingBytes, dead) == InitResult::kOk);
    auto* ring = static_cast<RingSegment*>(dead.base);
    ring->header.magic.store(kInitMarker);
    ring->header.init_pid.store(kDeadPid);
    ring->header.write_head.store(99); // 死者写了一半的东西
    ring->data[5].store(3.0f, std::memory_order_relaxed);

    Gate gate;
    HookBackend loserBackend(ISegmentBackend::InitStage::kBeforeTakeover, gate);
    HookBackend winnerBackend;
    InitProbe probe;
    InitResult loser = InitResult::kFailed;
    std::thread t([&] {
        SegmentView view;
        if (loserBackend.createOrOpen(name, kRingBytes, view) == InitResult::kOk)
        {
            loser = initRing(loserBackend, view, &probe);
        }
    });
    REQUIRE(gate.waitUntilReached());

    SegmentView winner;
    REQUIRE(winnerBackend.createOrOpen(name, kRingBytes, winner) == InitResult::kOk);
    REQUIRE(initRing(winnerBackend, winner, &probe) == InitResult::kOk);
    CHECK(ring->header.magic.load() == kRingMagic);
    CHECK(ring->header.init_pid.load() == currentProcessId()); // 清零跳过了它
    CHECK(ring->header.write_head.load() == 0u);
    CHECK(ring->data[5].load() == 0.0f);
    ring->header.writer_token.store(9);
    ring->data[7].store(2.0f, std::memory_order_relaxed);
    gate.open();
    t.join();

    CHECK(loser == InitResult::kOk);
    CHECK(probe.calls.load() == 1);
    CHECK(probe.markerHeld.load());
    CHECK(probe.pidRecorded.load());
    CHECK(ring->header.generation.load() == 1u);
    CHECK(ring->header.writer_token.load() == 9u);
    CHECK(ring->data[7].load() == 2.0f);
}

TEST_CASE("the retry backoff grows from 25 ms to a 250 ms cap with jitter", "[ipc][backend][backoff]")
{
    RetryBackoff a(1);
    RetryBackoff b(2);
    std::vector<u32> da;
    std::vector<u32> db;
    for (int i = 0; i < 40; ++i)
    {
        da.push_back(a.nextDelayMs());
        db.push_back(b.nextDelayMs());
    }
    const u32 ceilings[] = {50, 100, 200};
    for (std::size_t i = 0; i < da.size(); ++i)
    {
        INFO("attempt " << i << ": " << da[i] << " ms");
        CHECK(da[i] >= RetryBackoff::kMinDelayMs);
        CHECK(da[i] <= (i < 3 ? ceilings[i] : RetryBackoff::kMaxDelayMs));
    }
    CHECK(da != db); // 种子不同，相位就不同
    const std::set<u32> capped(da.begin() + 3, da.end());
    CHECK(capped.size() > 10); // 封顶之后仍有抖动，不会排成同一个节拍
    CHECK(*capped.rbegin() > RetryBackoff::kMaxDelayMs / 2);

    RetryBackoff again(1);
    for (const u32 d : da)
    {
        CHECK(again.nextDelayMs() == d); // 同一个种子给出同一串间隔
    }
    a.reset();
    CHECK(a.nextDelayMs() <= 50u);
}
