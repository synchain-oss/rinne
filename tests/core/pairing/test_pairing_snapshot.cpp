// SPDX-License-Identifier: GPL-3.0-or-later
//
// 会合段的只读快照（pairing::PairingSnapshot），用进程内后端：另一个后端实例扮「别的实例」建段、写槽，快照经计数
// 后端去读。覆盖：段不存在时全空、要连着两次打不开才信；打开的节奏（平时按扫描间隔、force 时强制再试、时钟倒退立刻
// 再试、force 只对段不存在生效：卡在初始化一半的段做决定的拍子也按扫描间隔、ABI 不同的段按冲突重试间隔、后端拒绝之后
// 不再试）；还没发布完、几何不对与 ABI 不同的段都当没有数据；每端带 pid，别的进程的端点照样算占着；标签两次读到同一份
// 才信、撕裂时留着上一份、换了主人又撕裂就当未知、显示名不是合法 UTF-8 就不要；整个过程不建段、不写段。
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>
#include <vector>

#include "../ipc/rendezvous/RendezvousTestKit.h"
#include "ipc/PairingPolicy.h"
#include "ipc/ProcessInfo.h"
#include "pairing/PairingSnapshot.h"

namespace
{

using namespace rinne::test;
using rinne::pairing::EndFacts;
using rinne::pairing::PairingSnapshot;
using rinne::pairing::PeerFacts;
using rinne::pairing::peerFactsOf;

constexpr u64 kT0 = 100000; // 毫秒；观察者把 0 当成「读不出时钟」

// 摆一个 active、心跳新鲜的端点槽
RoleSlot& occupy(RvSegment& rv, u32 group, Role role, u32 pid, u64 nowMs, u64 claimMs, u32 gen = 1)
{
    RoleSlot& slot = roleSlot(rv, group, role);
    slot.pid.store(pid);
    slot.heartbeat_ms.store(nowMs);
    slot.claim_ms.store(claimMs);
    slot.version.store(packVersion(0, 9, 0));
    slot.sample_rate.store(48000);
    slot.max_block.store(512);
    slot.channels.store(2);
    slot.ctl.store(makeCtl(gen, kCtlPhaseActive));
    return slot;
}

// 别的实例建好的会合段，以及它用的后端（与快照的后端是两个实例，同名段共享同一块内存）
struct World
{
    World() : scope(NameScope::randomForTesting()), naming(scope) { SegmentBackendInProcess::resetAllForTesting(); }
    ~World()
    {
        if (view.base != nullptr)
            writer.unmap(view);
    }
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    RvSegment& publish(u32 abi = kIpcAbi)
    {
        view = openRv(writer, naming, abi);
        return rvOf(view);
    }

    NameScope scope;
    Naming naming;
    SegmentBackendInProcess writer;
    SegmentView view;
};

} // namespace

TEST_CASE("snapshot: with no segment every view is empty, and only two misses in a row count as absent",
          "[pairing][snapshot]")
{
    World world;
    CountingBackend backend;
    PairingSnapshot snapshot(&backend, world.scope);
    CHECK_FALSE(snapshot.usable()); // 还没试过

    snapshot.refresh(kT0, false);
    CHECK(backend.openExistingReadOnlyCalls == 1u);
    CHECK_FALSE(snapshot.mapped());
    CHECK_FALSE(snapshot.usable()); // 一次打不开不算：POSIX 后端在生命周期锁正忙时也这么返回

    snapshot.refresh(kT0 + PairingSnapshot::kRetryMs, false);
    CHECK(backend.openExistingReadOnlyCalls == 2u);
    CHECK_FALSE(snapshot.mapped());
    CHECK(snapshot.usable());
    CHECK_FALSE(snapshot.abiMismatch());
    CHECK_FALSE(snapshot.unavailable());
    for (const GroupView& group : snapshot.groupViews(kT0 + PairingSnapshot::kRetryMs))
    {
        CHECK(group.input.isFree());
        CHECK(group.output.isFree());
    }
    CHECK_FALSE(snapshot.end(1, Role::kInput).stable);
    CHECK_FALSE(peerFactsOf(snapshot.end(1, Role::kOutput)).present);
}

TEST_CASE("snapshot: an unopenable segment is retried at the scan interval, and at once on a deciding tick",
          "[pairing][snapshot]")
{
    World world;
    CountingBackend backend;
    PairingSnapshot snapshot(&backend, world.scope);
    snapshot.refresh(kT0, false);
    REQUIRE(backend.openExistingReadOnlyCalls == 1u);
    // 不做决定的拍子：不到扫描间隔不再试
    for (u64 t = kT0 + 40; t < kT0 + PairingSnapshot::kRetryMs; t += 40)
        snapshot.refresh(t, false);
    CHECK(backend.openExistingReadOnlyCalls == 1u);

    SECTION("the scan interval elapses")
    {
        snapshot.refresh(kT0 + PairingSnapshot::kRetryMs, false);
        CHECK(backend.openExistingReadOnlyCalls == 2u);
    }
    SECTION("the clock goes back")
    {
        snapshot.refresh(kT0 - 10, false);
        CHECK(backend.openExistingReadOnlyCalls == 2u);
    }
    SECTION("a deciding tick sees a segment created since the last try")
    {
        RvSegment& rv = world.publish();
        occupy(rv, 1, Role::kInput, rinne::ipc::currentProcessId(), kT0 + 200, kT0 + 200);
        snapshot.refresh(kT0 + 200, true);
        CHECK(backend.openExistingReadOnlyCalls == 2u);
        REQUIRE(snapshot.mapped());
        CHECK(snapshot.usable());
        CHECK(snapshot.groupViews(kT0 + 200)[0].input.isLive());
        // 映射着之后每拍只重扫，不再打开
        snapshot.refresh(kT0 + 240, true);
        snapshot.refresh(kT0 + 280, false);
        CHECK(backend.openExistingReadOnlyCalls == 2u);
    }
}

TEST_CASE("snapshot: a segment still being initialised, of the wrong geometry or of another abi is never used",
          "[pairing][snapshot]")
{
    World world;
    CountingBackend backend;

    SECTION("not yet published")
    {
        world.view = createView(world.writer, world.naming, SegmentId::rendezvous(), kRvBytes); // magic 还是 0
        PairingSnapshot snapshot(&backend, world.scope);
        snapshot.refresh(kT0, true);
        CHECK(backend.openExistingReadOnlyCalls == 1u);
        CHECK(backend.unmapCalls == 1u); // 立刻放下
        CHECK_FALSE(snapshot.mapped());
        CHECK_FALSE(snapshot.usable()); // 段在、只是还没发布完：不能当全空
        publishRv(world.writer, world.view);
        snapshot.refresh(kT0 + PairingSnapshot::kRetryMs, false);
        CHECK(snapshot.mapped());
    }
    SECTION("published with the wrong geometry")
    {
        RvSegment& rv = world.publish();
        rv.header.groups.store(kGroupCount - 1u);
        PairingSnapshot snapshot(&backend, world.scope);
        snapshot.refresh(kT0, true);
        snapshot.refresh(kT0 + PairingSnapshot::kRetryMs, true);
        CHECK(backend.openExistingReadOnlyCalls == 2u);
        CHECK_FALSE(snapshot.mapped());
        CHECK_FALSE(snapshot.usable());
    }
    SECTION("another abi")
    {
        world.publish(kIpcAbi + 1u);
        PairingSnapshot snapshot(&backend, world.scope);
        snapshot.refresh(kT0, false);
        CHECK(snapshot.abiMismatch());
        CHECK_FALSE(snapshot.mapped());
        CHECK_FALSE(snapshot.usable());
        // 要做决定也不更快：那时本来就不做智能默认，每个冲突重试间隔看一次
        snapshot.refresh(kT0 + 500, true);
        CHECK(backend.openExistingReadOnlyCalls == 1u);
        snapshot.refresh(kT0 + PairingSnapshot::kAbiRetryMs, false);
        CHECK(backend.openExistingReadOnlyCalls == 2u);
        CHECK(snapshot.abiMismatch());
    }
}

// 别的进程建了段、初始化到一半就崩了，段一直停在「还没发布完」：快照一直不能用，调用方的决定一直做不成、
// 每一拍都传 force。force 只对「段不存在」生效，这时照样按扫描间隔打开，不变成每拍一次打开、校验、解映射；
// 段没了之后又能强制再试
TEST_CASE("snapshot: a segment stuck mid-initialisation is retried at the scan interval even on deciding ticks",
          "[pairing][snapshot]")
{
    World world;
    world.view = createView(world.writer, world.naming, SegmentId::rendezvous(), kRvBytes); // magic 一直是 0
    CountingBackend backend;
    PairingSnapshot snapshot(&backend, world.scope);
    u64 t = kT0;
    for (; t < kT0 + 1000; t += 40)
        snapshot.refresh(t, true);
    // 40 ms 一拍、25 拍：只在 kT0、+280、+560、+840 各开一次
    CHECK(backend.openExistingReadOnlyCalls == 4u);
    CHECK(backend.unmapCalls == 4u);
    CHECK_FALSE(snapshot.mapped());
    CHECK_FALSE(snapshot.usable());

    // 残段被收掉了：到了间隔那一次看到段不存在，下一个要做决定的拍子不等间隔再试，两次打不开就当全空
    backend.forced = InitResult::kFailed;
    for (int i = 0; i < 10 && backend.openExistingReadOnlyCalls == 4u; ++i, t += 40)
        snapshot.refresh(t, true);
    REQUIRE(backend.openExistingReadOnlyCalls == 5u);
    CHECK_FALSE(snapshot.usable());
    snapshot.refresh(t, true);
    CHECK(backend.openExistingReadOnlyCalls == 6u);
    CHECK(snapshot.usable());

    // 有人重新建好了段：做决定的拍子当场看见
    backend.forced = InitResult::kOk;
    publishRv(world.writer, world.view);
    snapshot.refresh(t + 40, true);
    CHECK(backend.openExistingReadOnlyCalls == 7u);
    CHECK(snapshot.mapped());
}

TEST_CASE("snapshot: once the backend refuses it is never asked again", "[pairing][snapshot]")
{
    World world;
    CountingBackend backend;
    backend.forced = InitResult::kFailedDenied;
    PairingSnapshot snapshot(&backend, world.scope);
    snapshot.refresh(kT0, true);
    CHECK(snapshot.unavailable());
    CHECK_FALSE(snapshot.usable());
    snapshot.refresh(kT0 + 1000, true);
    snapshot.refresh(kT0 + 60000, false);
    CHECK(backend.openExistingReadOnlyCalls == 1u);
}

TEST_CASE("snapshot: every end carries its pid, and ends of other processes stay occupied for the policy",
          "[pairing][snapshot]")
{
    World world;
    RvSegment& rv = world.publish();
    const u32 self = rinne::ipc::currentProcessId();
    const u32 other = self + 1u;
    // 组 A：别的进程的 Output 在等 Input；组 B：别的进程的 Input，认领得最晚；组 C：本进程的 Input
    RoleSlot& waiting = occupy(rv, 1, Role::kOutput, other, kT0, kT0 - 100);
    waiting.cfg_flags.store(kCfgNeverPaired);
    occupy(rv, 2, Role::kInput, other, kT0, kT0 - 10);
    occupy(rv, 3, Role::kInput, self, kT0, kT0 - 500);

    CountingBackend backend;
    PairingSnapshot snapshot(&backend, world.scope);
    snapshot.refresh(kT0, false);
    REQUIRE(snapshot.mapped());
    const GroupViews groups = snapshot.groupViews(kT0);
    CHECK(groups[0].output.isLive());
    CHECK(groups[0].output.pid == other);
    CHECK(groups[0].output.neverPaired);
    CHECK(groups[1].input.isLive());
    CHECK(groups[1].input.pid == other);
    CHECK(groups[2].input.isLive());
    CHECK(groups[2].input.pid == self);
    CHECK(groups[2].input.claimMs == kT0 - 500);

    // 别的进程的端点不参与匹配、也不当空位：Output 配本进程的 Input，Input 不去别的进程在等的组
    CHECK(decideOutput(PairingSelf{Role::kOutput, self, 0, 0}, groups).group == 3u);
    CHECK(decideInput(PairingSelf{Role::kInput, self, 0, 0}, groups).group == 4u);

    // 对端快照照实带 pid（PairStateModel 据此报跨进程）
    const PeerFacts peer = peerFactsOf(snapshot.end(2, Role::kInput));
    CHECK(peer.present);
    CHECK(peer.pid == other);
    CHECK(peer.version == packVersion(0, 9, 0));
    CHECK(peer.sampleRate == 48000u);
    CHECK(peer.channels == 2u);
    CHECK(snapshot.end(2, Role::kInput).maxBlock == 512u);

    // 心跳陈旧：还占着，但不是活端
    snapshot.refresh(kT0 + RendezvousTiming::kLostHeartbeatMs, false);
    const GroupViews later = snapshot.groupViews(kT0 + RendezvousTiming::kLostHeartbeatMs);
    CHECK_FALSE(later[1].input.isFree());
    CHECK_FALSE(later[1].input.isLive());
}

TEST_CASE("snapshot: a label counts only when two reads agree, and a torn read keeps the last one until the slot "
          "changes hands",
          "[pairing][snapshot]")
{
    World world;
    RvSegment& rv = world.publish();
    const u32 self = rinne::ipc::currentProcessId();
    RoleSlot& slot = occupy(rv, 1, Role::kInput, self, kT0, kT0);
    REQUIRE(writeLabel(slot, makeLabelPayload("Lead Vox", 0xff336699u)));

    CountingBackend backend;
    PairingSnapshot snapshot(&backend, world.scope);
    snapshot.refresh(kT0, false);
    EndFacts end = snapshot.end(1, Role::kInput);
    CHECK(end.nameHash == fnv1a32("Lead Vox"));
    CHECK(end.name == "Lead Vox");
    CHECK(end.colourArgb == 0xff336699u);
    CHECK(snapshot.groupViews(kT0)[0].input.nameHash == fnv1a32("Lead Vox"));

    // 写到一半（序号停在奇数）、还是同一个主人：留着上一份
    slot.label_seq.fetch_add(1);
    snapshot.refresh(kT0 + 40, false);
    CHECK(snapshot.end(1, Role::kInput).nameHash == fnv1a32("Lead Vox"));

    // 槽换了主人（ctl 变了）又读不稳：当未知
    slot.ctl.store(makeCtl(2, kCtlPhaseActive));
    snapshot.refresh(kT0 + 80, false);
    end = snapshot.end(1, Role::kInput);
    CHECK(end.nameHash == 0u);
    CHECK(end.name.empty());
    CHECK(end.colourArgb == 0u);
    CHECK(snapshot.groupViews(kT0 + 80)[0].input.nameHash == 0u);

    // 新主人写完一份：读到它
    slot.label_seq.fetch_add(1);
    REQUIRE(writeLabel(slot, makeLabelPayload("Bass", 0u)));
    snapshot.refresh(kT0 + 120, false);
    CHECK(snapshot.end(1, Role::kInput).name == "Bass");
    CHECK(snapshot.end(1, Role::kInput).colourArgb == 0u);

    // 槽空了就清掉（槽里的字段还留着上一任的标签）
    slot.ctl.store(makeCtl(3, kCtlPhaseFree));
    snapshot.refresh(kT0 + 160, false);
    CHECK(snapshot.end(1, Role::kInput).nameHash == 0u);
    CHECK(snapshot.end(1, Role::kInput).name.empty());
}

TEST_CASE("snapshot: a display name that is not valid UTF-8 is dropped, its hash is kept", "[pairing][snapshot]")
{
    World world;
    RvSegment& rv = world.publish();
    const u32 self = rinne::ipc::currentProcessId();
    struct Case
    {
        const char* what;
        std::string bytes;
        bool shown;
    };
    const std::vector<Case> cases = {
        {"plain ASCII", "Kick In", true},
        {"two- and three-byte characters", "Gr\xC3\xB6\xC3\x9F\x65 \xE4\xBA\xBA\xE5\xA3\xB0", true},
        {"a four-byte character", "Pad \xF0\x9F\x8E\xB9", true},
        {"a missing continuation byte", "ab\xC3(", false},
        {"an overlong encoding", "\xC0\xAF\x65tc", false},
        {"a surrogate", "x\xED\xA0\x80", false},
        {"past U+10FFFF", "\xF4\x90\x80\x80", false},
        {"a stray continuation byte", "\x80xyz", false},
    };
    CountingBackend backend;
    PairingSnapshot snapshot(&backend, world.scope);
    u32 gen = 1;
    u64 now = kT0;
    for (const Case& c : cases)
    {
        INFO(c.what);
        RoleSlot& slot = occupy(rv, 2, Role::kOutput, self, now, now, gen++);
        REQUIRE(writeLabel(slot, makeLabelPayload(c.bytes, 0u)));
        snapshot.refresh(now, false);
        const EndFacts end = snapshot.end(2, Role::kOutput);
        CHECK(end.nameHash == fnv1a32(c.bytes));
        CHECK(end.name == (c.shown ? c.bytes : std::string()));
        now += 40;
    }
}

TEST_CASE("snapshot: refreshing and reading never creates or writes the segment", "[pairing][snapshot]")
{
    World world;
    RvSegment& rv = world.publish();
    const u32 self = rinne::ipc::currentProcessId();
    REQUIRE(writeLabel(occupy(rv, 1, Role::kInput, self, kT0, kT0), makeLabelPayload("Lead Vox", 0xff000000u)));
    occupy(rv, 1, Role::kOutput, self, kT0, kT0);
    occupy(rv, 5, Role::kInput, self + 1u, kT0, kT0);
    std::vector<unsigned char> before(kRvBytes);
    std::memcpy(before.data(), world.view.base, kRvBytes);

    CountingBackend backend;
    {
        PairingSnapshot snapshot(&backend, world.scope);
        for (u64 t = kT0; t < kT0 + 3000; t += 40)
        {
            snapshot.refresh(t, true);
            (void)snapshot.groupViews(t);
            for (u32 group = 1; group <= kGroupCount; ++group)
            {
                (void)snapshot.end(group, Role::kInput);
                (void)snapshot.end(group, Role::kOutput);
            }
        }
        CHECK(snapshot.mapped());
        CHECK(std::memcmp(before.data(), world.view.base, kRvBytes) == 0);
        CHECK(backend.createOrOpenCalls == 0u);
        CHECK(backend.openExistingCalls == 0u);
        CHECK(backend.openExistingReadOnlyCalls == 1u);
        CHECK(backend.tryLockCalls == 0u);

        snapshot.close();
        CHECK_FALSE(snapshot.mapped());
        CHECK(backend.unmapCalls == 1u);
    }
    CHECK(backend.unmapCalls == 1u); // 析构时已经没有映射要放
}
