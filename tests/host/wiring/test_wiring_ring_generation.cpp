// SPDX-License-Identifier: GPL-3.0-or-later
//
// 环映射的代号经正式构建的壳交给引擎：同一个环上因 ctl 变化重新发布绑定（智能认领转正、只让 Input 同采样率重新
// prepare、两端一起停用再恢复）时代号不变，流不断：没有换绑定的重同步、没有重新淡入，回送与不打断的对照运行逐位相同；
// Output 的写方助手也不重置。真的换了映射（放开再认领同一组拿到新句柄、两端一起换组）时照旧判换段：重建读头、
// 从首个有效读样本起重新淡入。转正的 CAS 落地、新快照还没发布的那一块，拿着旧快照照样是属主。
// 一对真处理器经测试作用域的共享内存配对，时钟是假的，组号由智能默认定（不经界面选组）。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "WiringTestKit.h"
#include "engine/SafetyEvents.h"
#include "ipc/LoopBinding.h"
#include "ipc/LoopRing.h"
#include "ipc/SegmentLayout.h"
#include "loop/LoopSession.h"
#include "loop/StreamSync.h"

namespace
{

using namespace rinne::wiring_test;
namespace engine = rinne::engine;
namespace loop = rinne::loop;

constexpr int kSettle = 80; // 配上之后再走 800 ms：D = 100 ms，500 ms 淡入走完
constexpr int kLead = 30; // 打断之前录的块数
constexpr int kTail = 100; // 打断之后录的块数

bool provisional(const rinne::RinneProcessorBase& p)
{
    const std::uint64_t ctl = p.instanceHost().rendezvous().myCtl();
    return ctl != 0 && (ipc::ctlState(ctl) & ipc::kCtlProvisional) != 0;
}

const ipc::LoopBinding* bindingOf(const rinne::RinneProcessorBase& p)
{
    return p.instanceHost().rendezvous().bindings().current();
}

void saveState(rinne::RinneProcessorBase& p)
{
    juce::MemoryBlock block;
    p.getStateInformation(block); // 第一次 getState：智能认领转正
}

// 两端都不经界面选组，按智能默认认领（带 provisional），跑到配上为止；反馈 50 %，再走到淡入结束
void pairSmart(Rig& rig)
{
    rig.input = rig.makeInput();
    rig.output = rig.makeOutput();
    rig.input->prepareToPlay(kRate, rig.declared);
    rig.output->prepareToPlay(kRate, rig.declared);
    hostSetParam(*rig.input, "feedback", 50.0f);
    REQUIRE(rig.pairUp());
    REQUIRE(provisional(*rig.input));
    REQUIRE(provisional(*rig.output));
    REQUIRE(rig.input->instanceHost().group() != 0u);
    REQUIRE(rig.input->instanceHost().group() == rig.output->instanceHost().group());
    rig.cycles(kSettle);
    REQUIRE(rig.input->instanceHost().inputEngine().lastBlockResult().paired);
}

enum class Interrupt
{
    kNone, // 对照：什么都不做
    kConfirm, // 两端各一次 getState：智能认领转正
    kReprepareInput, // 只让 Input 同采样率、同块长 release 再 prepare，中间没有时间流过
};

struct Take
{
    std::vector<float> out; // Input 之后的左声道：打断之前 kLead 块、之后 kTail 块
    std::uint64_t returnedBefore = 0; // 打断之前一块的非零回送样本数
    std::array<std::uint32_t, 6> fadeIns{}; // 打断之后新增的淡入请求（按原因）
    std::uint32_t resyncs = 0; // 打断之后新增的重同步
    int bindSwitchBlocks = 0; // 打断之后以「换绑定」重同步的块
    int unpairedBlocks = 0; // 打断之后没配上的块
};

Take run(Interrupt what)
{
    Take take;
    Rig rig;
    rig.source = &sine;
    pairSmart(rig);
    rig.record = true;
    rig.cycles(kLead);

    auto& input = rig.input->instanceHost();
    take.returnedBefore = input.inputEngine().lastBlockResult().nonZeroReturn;
    const auto counts = input.counts();
    const std::uint32_t resyncs = input.inputEngine().loopStatus().resyncCount;
    const ipc::LoopBinding* inBefore = bindingOf(*rig.input);
    const ipc::LoopBinding* outBefore = bindingOf(*rig.output);
    REQUIRE(inBefore != nullptr);
    REQUIRE(outBefore != nullptr);

    switch (what)
    {
    case Interrupt::kNone:
        break;
    case Interrupt::kConfirm: {
        saveState(*rig.input);
        saveState(*rig.output);
        REQUIRE_FALSE(provisional(*rig.input));
        REQUIRE_FALSE(provisional(*rig.output));
        // 转正的 CAS 落了地、新快照也发布了；音频线程若在块首拿的是旧快照，读 ctl 时看到的是转正后的整字：
        // 仍是属主，交给引擎的段照旧
        // ⛔ stillMine 只比 myCtl（或会合不给 confirmedCtl）→ 这里变红：那一块引擎拿到空段，回送断开再重新配对
        for (const ipc::LoopBinding* before : {inBefore, outBefore})
        {
            const ipc::BoundBlock late(before);
            REQUIRE(late.ok());
            CHECK(late.stillMine());
        }
        break;
    }
    case Interrupt::kReprepareInput:
        rig.input->releaseResources();
        rig.input->prepareToPlay(kRate, rig.declared);
        break;
    }
    if (what != Interrupt::kNone)
    {
        // 确实重新发布了（不是空断言），而环还是那个句柄、映射代号不变
        const ipc::LoopBinding* inAfter = bindingOf(*rig.input);
        REQUIRE(inAfter != nullptr);
        CHECK(inAfter->seq != inBefore->seq);
        CHECK(inAfter->ring == inBefore->ring);
        CHECK(inAfter->ringGeneration == inBefore->ringGeneration);
        const ipc::LoopBinding* outAfter = bindingOf(*rig.output);
        REQUIRE(outAfter != nullptr);
        CHECK(outAfter->ring == outBefore->ring);
        CHECK(outAfter->ringGeneration == outBefore->ringGeneration);
        CHECK(outAfter->writerToken == outBefore->writerToken);
    }

    for (int i = 0; i < kTail; ++i)
    {
        rig.cycle();
        const loop::BlockResult& r = input.inputEngine().lastBlockResult();
        if (!r.paired)
            ++take.unpairedBlocks;
        if (r.resynced && r.resyncReason == loop::SyncReason::kBindSwitch)
            ++take.bindSwitchBlocks;
    }
    const auto after = input.counts();
    for (std::size_t i = 0; i < take.fadeIns.size(); ++i)
        take.fadeIns[i] = after.fadeIns[i] - counts.fadeIns[i];
    take.resyncs = input.inputEngine().loopStatus().resyncCount - resyncs;
    take.out = rig.outLeft;
    CHECK(rig.canaryHits == 0);
    CHECK(rig.outputTouched == 0);
    return take;
}

long firstDiff(const std::vector<float>& a, const std::vector<float>& b)
{
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i)
        if (!sameBits(a[i], b[i]))
            return static_cast<long>(i);
    return a.size() == b.size() ? -1 : static_cast<long>(n);
}

void checkUninterrupted(const Take& take, const Take& reference)
{
    // 打断之前回送确实在响（不是空断言）
    REQUIRE(take.returnedBefore > 0u);
    REQUIRE(take.out.size() == static_cast<std::size_t>((kLead + kTail) * kBlock));
    // ⛔ 壳把绑定的发布序号当映射代号（InstanceHost::process 里 ringGeneration = seq），或会合每次发布都给新代号
    //    → 下一块判成换段：重建读头、以「换绑定」重同步、请求 FirstPair 淡入，这几条连同逐位相同一起变红
    CHECK(take.bindSwitchBlocks == 0);
    CHECK(take.resyncs == 0u);
    CHECK(take.unpairedBlocks == 0);
    CHECK(take.fadeIns == std::array<std::uint32_t, 6>{});
    CHECK(firstDiff(take.out, reference.out) == -1);
}

} // namespace

TEST_CASE("confirming a smart claim mid-stream keeps the ring generation and the return stream bit for bit",
          "[host][wiring][ring_generation]")
{
    formalOnly();
    needsSharedMemory();
    const Take reference = run(Interrupt::kNone);
    CHECK(reference.bindSwitchBlocks == 0);
    CHECK(reference.fadeIns == std::array<std::uint32_t, 6>{});
    const Take confirmed = run(Interrupt::kConfirm);
    checkUninterrupted(confirmed, reference);
}

TEST_CASE("re-preparing the Input at the same rate keeps the ring generation and the return stream bit for bit",
          "[host][wiring][ring_generation]")
{
    formalOnly();
    needsSharedMemory();
    const Take reference = run(Interrupt::kNone);
    const Take reprepared = run(Interrupt::kReprepareInput);
    checkUninterrupted(reprepared, reference);
}

TEST_CASE("confirming the Output's smart claim does not restart its writer's stall count",
          "[host][wiring][ring_generation]")
{
    formalOnly();
    needsSharedMemory();
    Rig rig;
    rig.source = &sine;
    pairSmart(rig);
    auto& output = rig.output->instanceHost();
    const std::uint32_t reclaims = output.outputEngine().reclaimCount();
    {
        // 上一任写到一半就停下：公告领先写头。现任写方连续被挡满 1 s 的音频（100 块）才回收
        const ipc::BoundBlock lease(bindingOf(*rig.output));
        REQUIRE(lease.ok());
        const std::uint64_t head = ipc::LoopRing::writeHead(lease.ring());
        lease.ring().header.write_intent.store(head + 64u);
    }
    rig.cycles(60);
    REQUIRE(output.outputEngine().reclaimCount() == reclaims);
    saveState(*rig.output);
    REQUIRE_FALSE(provisional(*rig.output));
    rig.cycles(50);
    // ⛔ 映射代号随转正变了（发布序号当代号）→ 写方助手重置、被挡的帧从 0 重新计 → 110 块时还没回收，这里变红
    CHECK(output.outputEngine().reclaimCount() == reclaims + 1u);
}

TEST_CASE("a real change of mapping is still a ring switch: rebuilt read heads and a fresh fade-in",
          "[host][wiring][ring_generation]")
{
    formalOnly();
    needsSharedMemory();
    Rig rig;
    rig.source = &sine;
    pairSmart(rig);
    auto& input = rig.input->instanceHost();
    REQUIRE(input.inputEngine().lastBlockResult().nonZeroReturn > 0u);
    const auto before = input.counts();
    const ipc::LoopBinding* old = bindingOf(*rig.input);
    REQUIRE(old != nullptr);
    const std::uint32_t group = input.group();
    const auto firstPair = static_cast<std::size_t>(engine::FadeReason::FirstPair);

    SECTION("the Input lets go and claims the same group again: the same segment through a new handle")
    {
        // 两块之间放开再认领：环里的流连续（同一块段、同一个写方），映射是新的
        input.setGroup(0, ipc::ClaimIntent::kManual);
        input.setGroup(group, ipc::ClaimIntent::kManual);
        rig.tick();
        const ipc::LoopBinding* now = bindingOf(*rig.input);
        REQUIRE(now != nullptr);
        CHECK(now->ring != old->ring);
        CHECK(now->ringGeneration > old->ringGeneration);
    }
    SECTION("both ends move to another group")
    {
        const std::uint32_t other = group == 1u ? 2u : 1u;
        REQUIRE(rig.input->selectGroup(other));
        REQUIRE(rig.output->selectGroup(other));
        rig.tick();
        const ipc::LoopBinding* now = bindingOf(*rig.input);
        REQUIRE(now != nullptr);
        CHECK(now->group == other);
        CHECK(now->ringGeneration > old->ringGeneration);
    }
    // 下一块：两边都配着、换了段 → 旧读头淡出、读头从新段重建，以「换绑定」重同步，从首个有效读样本起重新淡入
    rig.cycle();
    const loop::BlockResult& r = input.inputEngine().lastBlockResult();
    CHECK(r.paired);
    CHECK(r.resynced);
    CHECK(r.resyncReason == loop::SyncReason::kBindSwitch);
    CHECK(input.counts().fadeIns[firstPair] == before.fadeIns[firstPair] + 1u);
}

TEST_CASE("both ends stopped for 300 ms around release and prepare: the suspend flush as before, no ring switch",
          "[host][wiring][ring_generation]")
{
    formalOnly();
    needsSharedMemory();
    Rig rig;
    rig.source = &sine;
    pairSmart(rig);
    auto& input = rig.input->instanceHost();
    const auto before = input.counts();
    const std::uint32_t resyncs = input.inputEngine().loopStatus().resyncCount;
    const ipc::LoopBinding* old = bindingOf(*rig.input);
    REQUIRE(old != nullptr);

    rig.input->releaseResources();
    rig.output->releaseResources();
    rig.advance(300'000'000);
    rig.input->prepareToPlay(kRate, rig.declared);
    rig.output->prepareToPlay(kRate, rig.declared);
    const ipc::LoopBinding* now = bindingOf(*rig.input);
    REQUIRE(now != nullptr);
    CHECK(now->seq != old->seq); // 休眠、唤醒各重新发布了一次
    CHECK(now->ring == old->ring);
    CHECK(now->ringGeneration == old->ringGeneration);

    int bindSwitchBlocks = 0;
    for (int i = 0; i < 20; ++i)
    {
        rig.cycle();
        const loop::BlockResult& r = input.inputEngine().lastBlockResult();
        CHECK(r.paired);
        if (r.resynced && r.resyncReason == loop::SyncReason::kBindSwitch)
            ++bindSwitchBlocks;
    }
    // 两端一起挂起的判定只认环的句柄（这次改动之前就是），照旧请求一次清空；同一个环不判换段
    const auto after = input.counts();
    CHECK(after.suspendFlushes == before.suspendFlushes + 1u);
    CHECK(after.suspendClears == before.suspendClears + 1u);
    CHECK(bindSwitchBlocks == 0);
    CHECK(input.inputEngine().loopStatus().resyncCount == resyncs);
    CHECK(after.fadeIns == before.fadeIns);
}
