// SPDX-License-Identifier: GPL-3.0-or-later
//
// 正式构建的生命周期：换组之后旧的段句柄经会合里的 HandleKeeper 延后不少于 250 ms 才释放（外壳的 tick 与改组用同一个
// 时钟）；最后一个实例析构之后不留活映射（先撤绑定再析构会合，会合析构时把宽限期里的映射收掉）；reset 只记数。

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>

#include "WiringTestKit.h"
#include "ipc/ISegmentBackend.h"
#include "ipc/LoopBinding.h"

namespace
{

using namespace rinne::wiring_test;

} // namespace

TEST_CASE("after a group switch the old segment handle is released no sooner than 250 ms later",
          "[host][wiring][lifecycle]")
{
    formalOnly();
    needsSharedMemory();
    Rig rig;
    rig.source = &sine;
    rig.open(1);
    REQUIRE(rig.pairUp());
    rig.cycles(10);

    const ipc::LoopBinding* binding = rig.output->instanceHost().rendezvous().bindings().current();
    REQUIRE(binding != nullptr);
    const ipc::SegmentHandle* oldRing = binding->ring;
    REQUIRE(oldRing != nullptr);
    REQUIRE(oldRing->valid());

    // ⛔ tick 交给会合的时间与改组时的不是同一个钟（tickLocked 里 rendezvous_->tick(ms + 1000)）→ 第一拍就放掉
    const std::uint64_t switchedNs = rig.clock.ns;
    REQUIRE(rig.output->selectGroup(2));
    std::uint64_t releasedAfterNs = 0;
    for (int i = 0; i < 50 && releasedAfterNs == 0; ++i)
    {
        rig.cycle(); // 音频照走（Input 还在组 1，Output 在组 2）
        if (!oldRing->valid())
            releasedAfterNs = rig.clock.ns - switchedNs;
    }
    CHECK(releasedAfterNs >= 250'000'000ull);
    CHECK(releasedAfterNs <= 250'000'000ull + 2 * 4 * kBlockNs); // 到点之后的一两拍之内
    CHECK(rig.output->instanceHost().group() == 2);
}

TEST_CASE("no live mapping is left once the last instance is gone", "[host][wiring][lifecycle]")
{
    formalOnly();
    needsSharedMemory();
    CountingBackend backend;
    {
        Rig rig(kBlock, &backend);
        rig.source = &sine;
        rig.open(1);
        REQUIRE(rig.pairUp());
        rig.cycles(20);
        CHECK(backend.live.load() > 0);
        // 换一次组：旧句柄退休、还在宽限期里时实例就没了
        REQUIRE(rig.output->selectGroup(3));
        REQUIRE(rig.input->selectGroup(3));
        rig.cycles(30);
        rig.input->releaseResources();
        rig.output->releaseResources();
        // ⛔ 析构时不析构会合（InstanceHost 的析构里把会合放掉不管）→ 这里还有活映射
    }
    CHECK(backend.mapped.load() > 0);
    CHECK(backend.live.load() == 0);
}

TEST_CASE("reset from any thread only counts", "[host][wiring][lifecycle]")
{
    formalOnly();
    needsSharedMemory();
    Rig rig;
    rig.source = &sine;
    rig.open(1);
    REQUIRE(rig.pairUp());
    rig.cycles(10);
    const auto state = rig.input->instanceHost().rendezvousState();
    rig.input->reset();
    rig.output->reset();
    rig.cycles(10);
    CHECK(rig.paired());
    CHECK(rig.input->instanceHost().rendezvousState() == state);
    CHECK(rig.input->instanceHost().counts().suspendFlushes == 0);
}
