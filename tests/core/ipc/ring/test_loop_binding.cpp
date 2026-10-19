// SPDX-License-Identifier: GPL-3.0-or-later
//
// 音频线程怎么拿到段：会合槽的地址、绑定快照的发布与读取、块首租约（两段段的租约、身份比对、段太小就拒绝），
// 以及句柄的退休节奏（延迟到期才 release，release 之后映射表还要等自己的宽限期；时钟读出 0 或倒退都不提前）。
// 段用进程内后端建，句柄经 HandleKeeper 接管；时间一律注入，不睡眠。

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <string>

#include "ipc/ISegmentBackend.h"
#include "ipc/LoopBinding.h"
#include "ipc/LoopRing.h"
#include "ipc/Naming.h"
#include "ipc/SegmentBackendInProcess.h"
#include "util/MonoClock.h"

namespace
{

using namespace rinne::ipc;

// 会合段与数据段的段头规格：与后端单测里的一致（会合段从段头之后清零，数据段从 16 字节起）
const SegmentHeaderSpec kRvSpec{kRvMagic, kIpcAbi, kRvBytes, kRvHeaderBytes};
const SegmentHeaderSpec kRingSpec{kRingMagic, kIpcAbi, kRingBytes, 16};

std::string segmentName(const Naming& naming, SegmentId id)
{
    std::string name;
    REQUIRE(naming.name(id, name) == NameStatus::kOk);
    return name;
}

// 一个已建好、已初始化的段，映射在 backend 上
SegmentHandle openSegment(ISegmentBackend& backend, const Naming& naming, SegmentId id, const SegmentHeaderSpec& spec)
{
    SegmentView view;
    REQUIRE(backend.createOrOpen(segmentName(naming, id), spec.layoutBytes, view) == InitResult::kOk);
    if (id.isRendezvous())
    {
        auto* rv = static_cast<RvSegment*>(view.base);
        REQUIRE(backend.initHeader(view, headerFields(rv->header), spec, [&] {
            rv->header.layout_bytes.store(kRvBytes, std::memory_order_relaxed);
            rv->header.groups.store(kGroupCount, std::memory_order_relaxed);
            rv->header.group_stride.store(kGroupStride, std::memory_order_relaxed);
        }) == InitResult::kOk);
    }
    else
    {
        auto* ring = static_cast<RingSegment*>(view.base);
        REQUIRE(backend.initHeader(view, headerFields(ring->header), spec, [&] { LoopRing::initGeometry(*ring); }) ==
                InitResult::kOk);
    }
    return SegmentHandle(std::move(view), &backend);
}

} // namespace

TEST_CASE("roleSlot addresses the input and output slot of each group block", "[ipc][binding]")
{
    auto rv = std::make_unique<RvSegment>();
    auto* bytes = reinterpret_cast<char*>(rv.get());
    for (u32 group = 1; group <= kGroupCount; ++group)
    {
        CHECK(&roleSlot(*rv, group, Role::kInput) ==
              reinterpret_cast<RoleSlot*>(bytes + roleSlotOffset(group, /*output=*/false)));
        CHECK(&roleSlot(*rv, group, Role::kOutput) ==
              reinterpret_cast<RoleSlot*>(bytes + roleSlotOffset(group, /*output=*/true)));
    }
    CHECK(peerOf(Role::kInput) == Role::kOutput);
    CHECK(peerOf(Role::kOutput) == Role::kInput);
}

TEST_CASE("BindingPublisher numbers and retains every snapshot it publishes", "[ipc][binding]")
{
    BindingPublisher publisher;
    CHECK(publisher.current() == nullptr);
    CHECK(publisher.retained() == 0);

    LoopBinding b;
    b.group = 3;
    b.role = Role::kOutput;
    b.myCtl = makeCtl(1, kCtlPhaseActive);
    const LoopBinding* first = publisher.publish(b);
    REQUIRE(first != nullptr);
    CHECK(publisher.current() == first);
    CHECK(first->seq == 1u);
    CHECK(first->group == 3u);
    CHECK(first->role == Role::kOutput);
    CHECK(first->myCtl == makeCtl(1, kCtlPhaseActive));

    b.group = 5;
    const LoopBinding* second = publisher.publish(b);
    REQUIRE(second != nullptr);
    CHECK(publisher.current() == second);
    CHECK(second->seq == 2u);
    CHECK(second->group == 5u);
    // 旧的那一份还留着：音频线程可能正拿着它
    CHECK(first->seq == 1u);
    CHECK(first->group == 3u);
    CHECK(publisher.retained() == 2);

    publisher.clear();
    CHECK(publisher.current() == nullptr);
    CHECK(publisher.retained() == 2);
}

TEST_CASE("BoundBlock leases both segments and checks the owner word", "[ipc][binding]")
{
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    SegmentBackendInProcess backend;
    HandleKeeper keeper;
    SegmentHandle* const rv = keeper.adopt(openSegment(backend, naming, SegmentId::rendezvous(), kRvSpec));
    SegmentHandle* const ring = keeper.adopt(openSegment(backend, naming, SegmentId::groupData(1), kRingSpec));

    // 空绑定：不 ok，什么都不碰
    {
        BoundBlock block(nullptr);
        CHECK_FALSE(block.ok());
    }

    LoopBinding b;
    b.seq = 1;
    b.rv = rv;
    b.ring = ring;
    b.group = 1;
    b.role = Role::kInput;
    b.myCtl = makeCtl(2, kCtlPhaseActive);

    {
        BoundBlock block(&b);
        REQUIRE(block.ok());
        CHECK(&block.rv() == static_cast<RvSegment*>(rv->base()));
        CHECK(&block.ring() == static_cast<RingSegment*>(ring->base()));
        CHECK(&block.mine() == &roleSlot(block.rv(), 1, Role::kInput));
        CHECK(&block.peer() == &roleSlot(block.rv(), 1, Role::kOutput));
        CHECK(block.binding().seq == 1u);
        // 属主身份：整字还是自己写下的就是自己的
        CHECK_FALSE(block.stillMine());
        block.mine().ctl.store(b.myCtl);
        CHECK(block.stillMine());
        block.mine().ctl.store(makeCtl(3, kCtlPhaseActive)); // 被接管
        CHECK_FALSE(block.stillMine());
    }

    // 组号越界：不是本 abi 的绑定，拒绝
    b.group = kGroupCount + 1;
    {
        BoundBlock block(&b);
        CHECK_FALSE(block.ok());
    }
    b.group = 0;
    CHECK_FALSE(BoundBlock(&b).ok());

    // 句柄释放之后不再发租约：下一块的 BoundBlock 拿不到映射
    b.group = 1;
    CHECK_FALSE(rv->release(1000));
    CHECK(rv->release(1000 + SegmentHandle::kReleaseGraceMs));
    {
        BoundBlock block(&b);
        CHECK_FALSE(block.ok());
    }
    // 数据段还活着：仍然拒绝（会合段拿不到租约就整块不成立）
    CHECK(ring->valid());
}

TEST_CASE("HandleKeeper retires a handle only after the delay and its own grace period", "[ipc][binding]")
{
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    SegmentBackendInProcess backend;
    HandleKeeper keeper;
    SegmentHandle* const ring = keeper.adopt(openSegment(backend, naming, SegmentId::groupData(2), kRingSpec));
    REQUIRE(ring->valid());
    CHECK(keeper.liveCount() == 1);
    CHECK(keeper.retiringCount() == 0);

    // 不认识的指针与重复退休都是空操作
    keeper.retire(nullptr, 1000);
    keeper.retire(nullptr, 1100);
    keeper.retire(ring, 1000);
    keeper.retire(ring, 1200); // 已经退休：时间戳不刷新
    CHECK(keeper.liveCount() == 0);
    CHECK(keeper.retiringCount() == 1);

    keeper.tick(1100);
    CHECK(ring->valid());
    // 到点：release 被调用，映射表开始等自己的宽限期
    keeper.tick(1000 + HandleKeeper::kRetireDelayMs);
    CHECK_FALSE(ring->valid());
    CHECK(keeper.retiringCount() == 1);
    // 宽限期没满时一直返回 false，每拍接着调
    keeper.tick(1000 + HandleKeeper::kRetireDelayMs + SegmentHandle::kReleaseGraceMs - 1);
    CHECK(keeper.retiringCount() == 1);
    keeper.tick(1000 + HandleKeeper::kRetireDelayMs + SegmentHandle::kReleaseGraceMs);
    CHECK(keeper.retiringCount() == 1); // 已解映射，空壳留到析构

    // 时钟读出 0：什么都不动
    SegmentHandle* const other = keeper.adopt(openSegment(backend, naming, SegmentId::groupData(3), kRingSpec));
    keeper.retire(other, 5000);
    keeper.tick(0);
    CHECK(other->valid());
    keeper.tick(5000 + HandleKeeper::kRetireDelayMs - 1);
    CHECK(other->valid());
    // 时钟倒退：从现在重新计
    keeper.tick(100);
    keeper.tick(100 + HandleKeeper::kRetireDelayMs - 1);
    CHECK(other->valid());
    keeper.tick(100 + HandleKeeper::kRetireDelayMs);
    CHECK_FALSE(other->valid());
    CHECK(keeper.retiringCount() == 2);

    // 退休时读不出时钟（0）：从下一次读得出的那一拍起计，而不是从 0 起算、立刻到点
    SegmentHandle* const third = keeper.adopt(openSegment(backend, naming, SegmentId::groupData(4), kRingSpec));
    keeper.retire(third, 0);
    keeper.tick(9000);
    CHECK(third->valid());
    keeper.tick(9000 + HandleKeeper::kRetireDelayMs - 1);
    CHECK(third->valid());
    keeper.tick(9000 + HandleKeeper::kRetireDelayMs);
    CHECK_FALSE(third->valid());
}

TEST_CASE("HandleKeeper::clear drops every handle and leaves unfinished mappings to the reaper", "[ipc][binding]")
{
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    SegmentBackendInProcess backend;
    // 先把前面用例留下的映射回收干净，计数才只反映本用例
    SegmentHandle::reapRetired(1);
    SegmentHandle::reapRetired(1 + SegmentHandle::kReleaseGraceMs);
    const std::size_t pendingBefore = SegmentHandle::retiredPending();
    {
        HandleKeeper keeper;
        SegmentHandle* const live = keeper.adopt(openSegment(backend, naming, SegmentId::groupData(5), kRingSpec));
        SegmentHandle* const retiring = keeper.adopt(openSegment(backend, naming, SegmentId::groupData(6), kRingSpec));
        REQUIRE(live->valid());
        // 句柄析构时按真实的单调时钟收尾，所以这里也用它：宽限期从 t0 + kRetireDelayMs 起算，析构时还没满
        const u64 t0 = rinne::util::monoNowMs();
        keeper.retire(retiring, t0);
        keeper.tick(t0 + HandleKeeper::kRetireDelayMs); // 已经 release，宽限期还没满
        CHECK(keeper.liveCount() == 1);
        CHECK(keeper.retiringCount() == 1);

        keeper.clear();
        CHECK(keeper.liveCount() == 0);
        CHECK(keeper.retiringCount() == 0);
        // 两个映射都没走完宽限期：交给进程级的回收，而不是在 clear 里等
        CHECK(SegmentHandle::retiredPending() == pendingBefore + 2);
        keeper.clear(); // 幂等
    }
    // 回收表按自己的宽限期解映射：第一拍开始计时，满 kReleaseGraceMs 之后清空
    const u64 t1 = rinne::util::monoNowMs() + 10000;
    SegmentHandle::reapRetired(t1);
    SegmentHandle::reapRetired(t1 + SegmentHandle::kReleaseGraceMs);
    CHECK(SegmentHandle::retiredPending() == pendingBefore);
}
