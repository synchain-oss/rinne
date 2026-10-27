// SPDX-License-Identifier: GPL-3.0-or-later
//
// 导出不一致锁存的验收落点（口径见 ExportLatch.h 与 ADR-0009）：
//   - 触发：离线块里的宿主违约、主读头补 0、对端报告违约、重同步（除起播沿与换绑定），一块至多计一次。
//   - 反例：块长不超声明值、D 不小于最大块长的干净离线导出，什么都不计。
//   - 实时块里的同样的事不计。
//   - 块长不变性：同一个事件在同一个样本位置发生，怎么切块都恰好计一次。
//   - 0 样本的调用不推进任何计数；只算账不出样本的块（快速路径）照样计数，计数与出样本的运行相同。
//   - 已读值在消息线程一侧：确认之后不再锁存，之后再有新事件又会锁存。
//
// 用 Rig 驱动：Input 在前、写方在后，会合段里替 Output 端发布一个槽（块序号每块 +1）。

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "TimingKit.h"
#include "loop/ExportLatch.h"

namespace
{

using namespace rinne::timing_test;
using rinne::loop::ExportBlockFacts;
using rinne::loop::exportCauses;
using rinne::loop::ExportLatchView;
using rinne::loop::kExportCauseOverrun;
using rinne::loop::kExportCausePeerOverMax;
using rinne::loop::kExportCauseResync;
using rinne::loop::kExportCauseViolation;
using rinne::loop::resyncCountsForExport;

TransportInfo offline(bool playing = false)
{
    TransportInfo t;
    t.nonrealtime = true;
    t.isPlaying = playing;
    return t;
}

TransportInfo realtime(bool playing = false)
{
    TransportInfo t;
    t.isPlaying = playing;
    return t;
}

constexpr std::int64_t kDeclared = 1024;
constexpr std::int64_t kSafeDelay = 2048; // D 不小于最大块长

} // namespace

// ---- 纯函数：谁算、谁不算 ----

TEST_CASE("导出锁存 原因位：四类触发各算一次，非离线块一概不算")
{
    ExportBlockFacts facts;
    CHECK(exportCauses(facts) == 0u); // 什么都没发生（也不是离线块）

    facts.nonrealtime = true;
    CHECK(exportCauses(facts) == 0u); // 离线块，什么都没发生

    ExportBlockFacts v = facts;
    v.hostViolation = true;
    CHECK(exportCauses(v) == kExportCauseViolation);

    ExportBlockFacts u = facts;
    u.unsupportedBlock = true;
    CHECK(exportCauses(u) == kExportCauseViolation); // 超长块也是违约

    ExportBlockFacts o = facts;
    o.overrun = 1;
    CHECK(exportCauses(o) == kExportCauseOverrun);

    ExportBlockFacts p = facts;
    p.peerOverMax = true;
    CHECK(exportCauses(p) == kExportCausePeerOverMax);

    ExportBlockFacts r = facts;
    r.resynced = true;
    r.resyncReason = rinne::loop::SyncReason::kShift;
    CHECK(exportCauses(r) == kExportCauseResync);

    // 一块里几件事一起发生：原因位叠加，但仍是同一个块（计数由调用方每块计一次）
    ExportBlockFacts all = facts;
    all.hostViolation = true;
    all.overrun = 5;
    all.peerOverMax = true;
    all.resynced = true;
    all.resyncReason = rinne::loop::SyncReason::kJump;
    CHECK(exportCauses(all) ==
          (kExportCauseViolation | kExportCauseOverrun | kExportCausePeerOverMax | kExportCauseResync));

    // 实时块：同样的事一概不算
    all.nonrealtime = false;
    CHECK(exportCauses(all) == 0u);
}

TEST_CASE("导出锁存 重同步的原因：写方 / 格式 / 断档恢复 / 跳变 / 位移算，起播沿 / 换绑定 / 吸收 / 改 D 不算")
{
    // 验证点：把起播沿也算进去 → 本用例变红（每次导出起点都会锁存）。
    using rinne::loop::SyncReason;
    CHECK(resyncCountsForExport(SyncReason::kWriter));
    CHECK(resyncCountsForExport(SyncReason::kFormat));
    CHECK(resyncCountsForExport(SyncReason::kStallResume));
    CHECK(resyncCountsForExport(SyncReason::kJump));
    CHECK(resyncCountsForExport(SyncReason::kShift));
    CHECK_FALSE(resyncCountsForExport(SyncReason::kPlay));
    CHECK_FALSE(resyncCountsForExport(SyncReason::kBindSwitch));
    CHECK_FALSE(resyncCountsForExport(SyncReason::kOffset));
    CHECK_FALSE(resyncCountsForExport(SyncReason::kDChange));
}

// ---- 消息线程一侧：已读值 ----

TEST_CASE("导出锁存 已读值：确认之后不再锁存，新事件重新锁存，计数回绕无碍")
{
    ExportLatchView view;
    CHECK_FALSE(view.latched());

    view.update(1);
    CHECK(view.latched());
    CHECK(view.pending() == 1u);

    view.acknowledge();
    CHECK_FALSE(view.latched());
    CHECK(view.acknowledged() == 1u);
    view.update(1); // 同一个计数再读一遍：仍不锁存
    CHECK_FALSE(view.latched());

    view.update(4); // 又来了 3 个事件
    CHECK(view.latched());
    CHECK(view.pending() == 3u);

    view.acknowledge();
    view.acknowledge(); // 重复确认无害
    CHECK_FALSE(view.latched());

    // 无符号回绕：已读 0xFFFFFFFF，计数回绕到 0
    ExportLatchView wrap;
    wrap.update(0xFFFFFFFFu);
    wrap.acknowledge();
    wrap.update(0u);
    CHECK(wrap.latched());
    CHECK(wrap.pending() == 1u);
}

TEST_CASE("导出锁存 状态里的计数：noteExport 计一次并记下原因，原因为 0 不动")
{
    rinne::loop::LoopStatus status;
    status.noteExport(0);
    CHECK(status.snapshot().exportEvents == 0u);
    CHECK(status.snapshot().exportCauses == 0u);

    status.noteExport(kExportCauseOverrun);
    status.noteExport(kExportCauseResync | kExportCauseViolation);
    const auto fields = status.snapshot();
    CHECK(fields.exportEvents == 2u);
    CHECK(fields.exportCauses == (kExportCauseResync | kExportCauseViolation)); // 最近一次的
}

// ---- 会话：不该锁存的 ----

TEST_CASE("导出锁存 反例：块长不超声明值、D 不小于最大块长的干净离线导出不锁存，与块长无关")
{
    // 验证点：把任何一块都计一次 → 本用例变红。
    for (const bool produce : {true, false})
    {
        for (const int n : {1, 7, 64, 100, 128, 256, 480, 1000, 1024})
        {
            Rig rig(48000.0, kDeclared);
            rig.enablePeerSlot();
            const EngineParams params = timeParams(kSafeDelay);
            rig.run(96000, {n}, params, offline(), produce);
            const LoopStatusFields s = rig.status();
            INFO("n = " << n << " produce = " << produce);
            CHECK(s.exportEvents == 0u);
            CHECK(s.exportCauses == 0u);
            CHECK(s.resyncCount == 0u);
            CHECK(s.overrun == 0u);
        }
    }
}

TEST_CASE("导出锁存 反例：离线渲染从走带开始，起播沿的复位不锁存；第一次配上的对齐不锁存")
{
    Rig rig(48000.0, kDeclared);
    rig.enablePeerSlot();
    const EngineParams params = timeParams(kSafeDelay);

    // 先停着跑几块，再起播（离线渲染都是这样开始的）
    for (int i = 0; i < 4; ++i)
    {
        rig.step(1024, params, offline(false));
    }
    for (int i = 0; i < 60; ++i)
    {
        rig.step(1024, params, offline(true));
    }
    const LoopStatusFields s = rig.status();
    CHECK(s.resyncByReason[static_cast<int>(rinne::loop::SyncReason::kPlay)] == 1u); // 起播沿确实发生了
    CHECK(s.exportEvents == 0u);

    // 同采样率重新 prepare（导出开始前宿主一般会再 prepare 一次）：什么都不计
    REQUIRE(rig.session().prepare(48000.0));
    for (int i = 0; i < 20; ++i)
    {
        rig.step(1024, params, offline(true));
    }
    CHECK(rig.status().exportEvents == 0u);
}

TEST_CASE("导出锁存 实时块里的同样的事不计：违约、补 0、对端违约、重同步")
{
    const EngineParams safe = timeParams(kSafeDelay);

    {
        INFO("host violation");
        Rig rig(48000.0, kDeclared);
        rig.enablePeerSlot();
        for (int i = 0; i < 4; ++i)
        {
            rig.step(1024, safe, realtime());
        }
        const BlockResult r = rig.step(2048, safe, realtime());
        CHECK(r.hostViolation);
        CHECK(rig.status().exportEvents == 0u);
    }
    {
        INFO("overrun");
        Rig rig(48000.0, kDeclared);
        rig.enablePeerSlot();
        const EngineParams tight = timeParams(1024);
        rig.stepOutputFirst(1024, tight, realtime());
        rig.write(512);
        const BlockResult r = rig.feed(1024, tight, realtime());
        CHECK(r.overrun == 512u);
        CHECK(rig.status().exportEvents == 0u);
    }
    {
        INFO("peer over max");
        Rig rig(48000.0, kDeclared);
        rig.enablePeerSlot();
        rig.setPeerOverMax(true);
        for (int i = 0; i < 6; ++i)
        {
            rig.step(1024, safe, realtime());
        }
        CHECK(rig.status().exportEvents == 0u);
    }
    {
        INFO("resync");
        Rig rig(48000.0, kDeclared);
        rig.enablePeerSlot();
        for (int i = 0; i < 4; ++i)
        {
            rig.step(1024, safe, realtime());
        }
        rig.write(20000); // 写头跳到前面
        const BlockResult r = rig.feed(1024, safe, realtime());
        CHECK(r.resynced);
        CHECK(rig.status().exportEvents == 0u);
    }
}

// ---- 会话：该锁存的 ----

TEST_CASE("导出锁存 宿主违约：离线块里块长超过声明值，每块计一次，原因是违约")
{
    Rig rig(48000.0, kDeclared);
    rig.enablePeerSlot();
    const EngineParams params = timeParams(kSafeDelay);
    for (int i = 0; i < 4; ++i)
    {
        rig.step(1024, params, offline());
    }
    REQUIRE(rig.status().exportEvents == 0u);

    for (int i = 1; i <= 3; ++i)
    {
        const BlockResult r = rig.step(2048, params, offline());
        CHECK(r.hostViolation);
        CHECK(rig.status().exportEvents == static_cast<std::uint32_t>(i));
        CHECK((rig.status().exportCauses & kExportCauseViolation) != 0u);
    }
}

TEST_CASE("导出锁存 超长块（超过 32768）：离线块里计一次，实时块不计")
{
    for (const bool nonrealtime : {true, false})
    {
        Rig rig(48000.0, kDeclared);
        rig.enablePeerSlot();
        const EngineParams params = timeParams(kSafeDelay);
        for (int i = 0; i < 4; ++i)
        {
            rig.step(1024, params, nonrealtime ? offline() : realtime());
        }
        const BlockResult r = rig.feed(40000, params, nonrealtime ? offline() : realtime(), false);
        CHECK(r.unsupportedBlock);
        CHECK(rig.status().exportEvents == (nonrealtime ? 1u : 0u));
        if (nonrealtime)
        {
            CHECK(rig.status().exportCauses == kExportCauseViolation);
        }
    }
}

TEST_CASE("导出锁存 主读头补 0：写方这一块没跟上，离线块里计一次，原因是补 0")
{
    Rig rig(48000.0, kDeclared);
    rig.enablePeerSlot();
    const EngineParams params = timeParams(1024); // D = 1024：写方一块只写了一半，后一半读不到
    rig.stepOutputFirst(1024, params, offline()); // 写方先到：同步偏移锚定成 1024
    REQUIRE(rig.status().exportEvents == 0u);

    rig.write(512);
    const BlockResult r = rig.feed(1024, params, offline());
    CHECK(r.overrun == 512u);
    CHECK_FALSE(r.resynced);
    CHECK(rig.status().exportEvents == 1u);
    CHECK(rig.status().exportCauses == kExportCauseOverrun);
}

TEST_CASE("导出锁存 对端报告违约：只在对端的新块上看一次，同一个块不重复计")
{
    Rig rig(48000.0, kDeclared);
    rig.enablePeerSlot();
    const EngineParams params = timeParams(kSafeDelay);
    rig.setPeerOverMax(true);

    // 第一块：槽里还没有对端的块，只记下块序号，不计
    rig.step(1024, params, offline());
    CHECK(rig.status().exportEvents == 0u);
    // 之后每个对端的新块带着违约位：每块计一次
    rig.step(1024, params, offline());
    CHECK(rig.status().exportEvents == 1u);
    CHECK(rig.status().exportCauses == kExportCausePeerOverMax);
    rig.step(1024, params, offline());
    CHECK(rig.status().exportEvents == 2u);

    // 第三块末尾发布的那个新块，在下一次读的时候也带着违约位：再计一次（这是它第一次被读到）。
    // 之后对端不再发布新块（块序号不动）：同一个块上的标志不重复计
    for (int i = 0; i < 5; ++i)
    {
        rig.feed(1024, params, offline());
        rig.write(1024);
    }
    CHECK(rig.status().exportEvents == 3u);

    // 对端的标志清掉：不再计
    rig.setPeerOverMax(false);
    for (int i = 0; i < 5; ++i)
    {
        rig.step(1024, params, offline());
    }
    CHECK(rig.status().exportEvents == 3u);
}

TEST_CASE("导出锁存 槽里一开始就有带违约位的旧块：第一次读到只记块序号，不计；之后的新块才计")
{
    // 验证点：第一次读到也计 → 本用例变红。
    Rig rig(48000.0, kDeclared);
    rig.enablePeerSlot();
    const EngineParams params = timeParams(kSafeDelay);
    rig.setPeerOverMax(true);
    // 本会话第一次读之前，槽里已经有对端发布过的块（块序号不是 0，带着违约位）：可能是上一个会话留下的
    rig.publishPeer(offline());
    rig.publishPeer(offline());

    rig.step(1024, params, offline());
    CHECK(rig.status().exportEvents == 0u);
    // 对端在这一周期发布了新块（带违约位）：下一块读到它，才计
    rig.step(1024, params, offline());
    CHECK(rig.status().exportEvents == 1u);
    CHECK(rig.status().exportCauses == kExportCausePeerOverMax);
}

TEST_CASE("导出锁存 对端的块序号每块都记账，不管是不是离线块：实时块看过的块，下一个离线块不重复计")
{
    // 验证点：块序号只在离线块上记账 → 本用例变红。
    Rig rig(48000.0, kDeclared);
    rig.enablePeerSlot();
    const EngineParams params = timeParams(kSafeDelay);
    rig.setPeerOverMax(true);

    // 离线块：槽里还没有对端的块，只记下块序号；写完之后对端发布一个带违约位的新块
    rig.step(1024, params, offline());
    REQUIRE(rig.status().exportEvents == 0u);

    // 实时块读到了这个带违约位的块（实时块不计）；对端这一周期没有再发布
    rig.feed(1024, params, realtime());
    rig.write(1024);
    CHECK(rig.status().exportEvents == 0u);

    // 紧接着的离线块读到的还是同一个块：它已经被看过了，不重复计
    rig.feed(1024, params, offline());
    rig.write(1024);
    CHECK(rig.status().exportEvents == 0u);

    // 对端发布了新块（仍带违约位）：这一次才计
    rig.publishPeer(offline());
    rig.feed(1024, params, offline());
    rig.write(1024);
    CHECK(rig.status().exportEvents == 1u);
    CHECK(rig.status().exportCauses == kExportCausePeerOverMax);
}

TEST_CASE("导出锁存 重同步：换写方、改格式、写头跳变、断档恢复各计一次；起播沿不计")
{
    // 验证点：把重同步这一项去掉 → 本用例变红。
    Rig rig(48000.0, kDeclared);
    rig.enablePeerSlot();
    const EngineParams params = timeParams(kSafeDelay);
    for (int i = 0; i < 6; ++i)
    {
        rig.step(1024, params, offline());
    }
    REQUIRE(rig.status().exportEvents == 0u);

    // 换写方
    rig.attach(rig.ring(), 2);
    rig.step(1024, params, offline());
    CHECK(rig.status().exportEvents == 1u);
    CHECK(rig.status().exportCauses == kExportCauseResync);
    CHECK(rig.status().resyncByReason[static_cast<int>(rinne::loop::SyncReason::kWriter)] == 1u);
    for (int i = 0; i < 3; ++i)
    {
        rig.step(1024, params, offline());
    }
    CHECK(rig.status().exportEvents == 1u); // 稳定下来之后不再计

    // 写方改了采样率（epoch 变了）
    ipc::LoopRing::publishWriter(rig.ring(), 2, 44100, 2);
    rig.step(1024, params, offline());
    CHECK(rig.status().exportEvents == 2u);
    CHECK(rig.status().resyncByReason[static_cast<int>(rinne::loop::SyncReason::kFormat)] == 1u);
    ipc::LoopRing::publishWriter(rig.ring(), 2, 48000, 2); // 改回去也是一次
    rig.step(1024, params, offline());
    CHECK(rig.status().exportEvents == 3u);
    for (int i = 0; i < 3; ++i)
    {
        rig.step(1024, params, offline());
    }

    // 写头一下子跳到前面（超过 8192 帧）
    rig.write(20000);
    rig.step(1024, params, offline());
    CHECK(rig.status().exportEvents == 4u);
    CHECK(rig.status().resyncByReason[static_cast<int>(rinne::loop::SyncReason::kJump)] == 1u);
    for (int i = 0; i < 3; ++i)
    {
        rig.step(1024, params, offline());
    }

    // 写方停了一阵（断档），恢复时重建读头
    for (int i = 0; i < 8; ++i)
    {
        rig.feed(1024, params, offline());
    }
    const std::uint32_t beforeResume = rig.status().exportEvents;
    CHECK(rig.status().stalled == 1u);
    rig.write(1024 * 9);
    rig.feed(1024, params, offline());
    CHECK(rig.status().resyncByReason[static_cast<int>(rinne::loop::SyncReason::kStallResume)] == 1u);
    CHECK(rig.status().exportEvents > beforeResume);
    CHECK((rig.status().exportCauses & kExportCauseResync) != 0u);

    // 起播沿不计
    for (int i = 0; i < 6; ++i)
    {
        rig.step(1024, params, offline(false));
    }
    const std::uint32_t quiet = rig.status().exportEvents;
    rig.step(1024, params, offline(true));
    CHECK(rig.status().resyncByReason[static_cast<int>(rinne::loop::SyncReason::kPlay)] == 1u);
    CHECK(rig.status().exportEvents == quiet);
}

// ---- 块长不变性、0 样本、快速路径 ----

namespace
{

// 在 61440 样本处换写方（一个块边界，下面的块长都整除它），其余时候一切正常；返回最终状态
LoopStatusFields runWithOneEvent(int n, bool produce, bool zeroLengthCalls)
{
    Rig rig(48000.0, kDeclared);
    rig.enablePeerSlot();
    const EngineParams params = timeParams(kSafeDelay);
    const auto stepSome = [&](std::int64_t total) {
        for (std::int64_t done = 0; done < total; done += n)
        {
            if (zeroLengthCalls)
            {
                rig.feed(0, params, offline(), produce);
            }
            rig.step(n, params, offline(), produce);
            if (zeroLengthCalls)
            {
                rig.feed(0, params, offline(), produce);
            }
        }
    };
    stepSome(61440);
    rig.attach(rig.ring(), 2); // 换写方：同一个样本位置上的一个事件
    stepSome(30720);
    return rig.status();
}

} // namespace

TEST_CASE("导出锁存 块长不变性：同一个事件在同一个样本位置发生，怎么切块都恰好计一次")
{
    for (const int n : {64, 128, 256, 512, 1024})
    {
        const LoopStatusFields s = runWithOneEvent(n, true, false);
        INFO("n = " << n);
        CHECK(s.exportEvents == 1u);
        CHECK(s.exportCauses == kExportCauseResync);
        CHECK(s.resyncCount == 1u);
    }
}

TEST_CASE("导出锁存 0 样本的调用不推进任何计数：穿插任意多次，结果与不穿插逐字段相同")
{
    const LoopStatusFields plain = runWithOneEvent(256, true, false);
    const LoopStatusFields withZero = runWithOneEvent(256, true, true);
    CHECK(withZero.exportEvents == plain.exportEvents);
    CHECK(withZero.exportCauses == plain.exportCauses);
    CHECK(withZero.resyncCount == plain.resyncCount);
    CHECK(withZero.overrun == plain.overrun);
    CHECK(withZero.orderChecks == plain.orderChecks);
    CHECK(withZero.drift == plain.drift);
    CHECK(withZero.writeHead == plain.writeHead);

    // 离线违约的那一块前后穿插 0 样本调用：违约照样只计一次
    Rig rig(48000.0, kDeclared);
    rig.enablePeerSlot();
    const EngineParams params = timeParams(kSafeDelay);
    for (int i = 0; i < 4; ++i)
    {
        rig.step(1024, params, offline());
    }
    for (int i = 0; i < 1000; ++i)
    {
        rig.feed(0, params, offline());
    }
    rig.step(2048, params, offline());
    for (int i = 0; i < 1000; ++i)
    {
        rig.feed(0, params, offline());
    }
    CHECK(rig.status().exportEvents == 1u);
}

TEST_CASE("导出锁存 快速路径（只算账不出样本）期间照常计数，与出样本的运行逐字段相同")
{
    // 验证点：把计数挪进「出样本」的分支里 → 本用例变红。
    const LoopStatusFields full = runWithOneEvent(256, true, false);
    const LoopStatusFields fast = runWithOneEvent(256, false, false);
    CHECK(fast.exportEvents == 1u);
    CHECK(fast.exportCauses == full.exportCauses);
    CHECK(fast.resyncCount == full.resyncCount);
    CHECK(fast.overrun == full.overrun);

    // 违约、补 0、对端违约在快速路径里一样计
    const EngineParams params = timeParams(1024);
    {
        INFO("host violation");
        Rig rig(48000.0, kDeclared);
        rig.enablePeerSlot();
        rig.step(1024, params, offline(), false);
        rig.step(2048, params, offline(), false);
        CHECK(rig.status().exportEvents == 1u);
        CHECK(rig.status().exportCauses == kExportCauseViolation);
    }
    {
        INFO("overrun");
        Rig rig(48000.0, kDeclared);
        rig.enablePeerSlot();
        rig.stepOutputFirst(1024, params, offline(), false);
        rig.write(512);
        const BlockResult r = rig.feed(1024, params, offline(), false);
        CHECK(r.overrun == 512u);
        CHECK(rig.status().exportEvents == 1u);
        CHECK(rig.status().exportCauses == kExportCauseOverrun);
    }
    {
        INFO("peer over max");
        Rig rig(48000.0, kDeclared);
        rig.enablePeerSlot();
        rig.setPeerOverMax(true);
        for (int i = 0; i < 4; ++i)
        {
            rig.step(1024, timeParams(kSafeDelay), offline(), false);
        }
        CHECK(rig.status().exportEvents == 3u); // 第一块只记块序号，之后每个对端的新块一次
        CHECK(rig.status().exportCauses == kExportCausePeerOverMax);
    }
}

TEST_CASE("导出锁存 确认之后新事件重新锁存（会话 + 消息线程一侧）")
{
    Rig rig(48000.0, kDeclared);
    rig.enablePeerSlot();
    const EngineParams params = timeParams(kSafeDelay);
    ExportLatchView view;
    for (int i = 0; i < 6; ++i)
    {
        rig.step(1024, params, offline());
    }
    view.update(rig.status().exportEvents);
    CHECK_FALSE(view.latched());

    rig.step(2048, params, offline()); // 违约
    view.update(rig.status().exportEvents);
    CHECK(view.latched());

    view.acknowledge();
    for (int i = 0; i < 6; ++i)
    {
        rig.step(1024, params, offline());
    }
    view.update(rig.status().exportEvents);
    CHECK_FALSE(view.latched()); // 确认之后没有新事件：不锁存

    rig.step(2048, params, offline()); // 又一次违约
    view.update(rig.status().exportEvents);
    CHECK(view.latched());
}
