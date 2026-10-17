// SPDX-License-Identifier: GPL-3.0-or-later
//
// 段头初始化协议（平台无关的那一半），在进程内后端上验：建段者最后发布 magic；附着方比对 magic、abi 与真实大小；
// abi 不同的一方读得出对方的 abi 与版本、但拿到 kAbiMismatch；magic 不是我们的不碰；创建者死在初始化中途时
// 覆盖式重初始化；只读校验绝不写。

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>

#include "DiagCapture.h"
#include "ipc/ISegmentBackend.h"
#include "ipc/Naming.h"
#include "ipc/SegmentBackendInProcess.h"

namespace
{

using namespace rinne::ipc;

const SegmentHeaderSpec kRvSpec{kRvMagic, kIpcAbi, kRvBytes, kRvHeaderBytes};

std::string segName(const Naming& naming, SegmentId id)
{
    std::string name;
    REQUIRE(naming.name(id, name) == NameStatus::kOk);
    return name;
}

InitResult initRv(ISegmentBackend& backend, SegmentView& view, const SegmentHeaderSpec& spec = kRvSpec,
                  bool allowOverwrite = true, int* initCalls = nullptr)
{
    auto* rv = static_cast<RvSegment*>(view.base);
    return backend.initHeader(
        view, &rv->header.magic, &rv->header.abi, &rv->header.generation, spec,
        [&] {
            // 几何字段在 magic 发布之前写好
            CHECK(rv->header.magic.load() == 0);
            rv->header.layout_bytes.store(kRvBytes, std::memory_order_relaxed);
            rv->header.groups.store(kGroupCount, std::memory_order_relaxed);
            rv->header.group_stride.store(kGroupStride, std::memory_order_relaxed);
            if (initCalls != nullptr)
            {
                ++*initCalls;
            }
        },
        allowOverwrite);
}

} // namespace

TEST_CASE("the creator publishes magic last and an attacher accepts it", "[ipc][backend][header]")
{
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    const std::string name = segName(naming, SegmentId::rendezvous());
    SegmentBackendInProcess backend;

    SegmentView creator;
    REQUIRE(backend.createOrOpen(name, kRvBytes, creator) == InitResult::kOk);
    REQUIRE(creator.created);
    CHECK(creator.name == name);
    int calls = 0;
    REQUIRE(initRv(backend, creator, kRvSpec, true, &calls) == InitResult::kOk);
    CHECK(calls == 1);
    const auto* rv = static_cast<const RvSegment*>(creator.base);
    CHECK(rv->header.magic.load() == kRvMagic);
    CHECK(rv->header.abi.load() == kIpcAbi);
    CHECK(rv->header.generation.load() == 1u);
    CHECK(rv->header.layout_bytes.load() == kRvBytes);

    SegmentView attacher;
    REQUIRE(backend.createOrOpen(name, kRvBytes, attacher) == InitResult::kOk);
    CHECK_FALSE(attacher.created);
    CHECK(attacher.base == creator.base);
    calls = 0;
    CHECK(initRv(backend, attacher, kRvSpec, true, &calls) == InitResult::kOk);
    CHECK(calls == 0); // 附着方不改几何
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

    // 读得出对方：前 16 字节与端点槽 version 的绝对位置永久冻结，按字节偏移读，不依赖本版本的结构体
    const auto* bytes = static_cast<const unsigned char*>(v2.base);
    std::uint32_t magic = 0;
    std::uint32_t abi = 0;
    std::uint32_t version = 0;
    std::memcpy(&magic, bytes + 0, 4);
    std::memcpy(&abi, bytes + 4, 4);
    std::memcpy(&version, bytes + 1024 * 3 + 256 + 56, 4);
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
        // 头是对的，大小不够：附着方也不用
        auto* header = static_cast<RvHeader*>(small.base);
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

    SegmentView creator;
    REQUIRE(backend.createOrOpen(name, kRvBytes, creator) == InitResult::kOk);
    const auto* rv = static_cast<const RvSegment*>(creator.base);
    // 创建者还没初始化完：只读方与不许覆盖的附着方都立刻返回 kFailed，下一拍重试
    CHECK(backend.checkHeaderReadOnly(creator, rv->header.magic, rv->header.abi, kRvSpec) == InitResult::kFailed);
    SegmentView early;
    REQUIRE(backend.openExisting(name, early) == InitResult::kOk);
    CHECK(initRv(backend, early, kRvSpec, /*allowOverwrite=*/false) == InitResult::kFailed);
    CHECK(rv->header.magic.load() == 0u);
    CHECK(rv->header.abi.load() == 0u);

    REQUIRE(initRv(backend, creator) == InitResult::kOk);
    SegmentView reader;
    REQUIRE(backend.openExistingReadOnly(name, reader) == InitResult::kOk);
    CHECK(backend.checkHeaderReadOnly(reader, rv->header.magic, rv->header.abi, kRvSpec) == InitResult::kOk);
    SegmentHeaderSpec abi2 = kRvSpec;
    abi2.abi = 2;
    CHECK(backend.checkHeaderReadOnly(reader, rv->header.magic, rv->header.abi, abi2) == InitResult::kAbiMismatch);
}

TEST_CASE("an attacher re-initialises a segment whose creator died before publishing magic", "[ipc][backend][header]")
{
    SegmentBackendInProcess::resetAllForTesting();
    const Naming naming(NameScope::randomForTesting());
    const std::string name = segName(naming, SegmentId::rendezvous());
    SegmentBackendInProcess backend;

    SegmentView dead;
    REQUIRE(backend.createOrOpen(name, kRvBytes, dead) == InitResult::kOk);
    auto* rv = static_cast<RvSegment*>(dead.base);
    // 创建者写了一半就没了：magic 没发布，槽里留着垃圾
    rv->header.abi.store(kIpcAbi);
    rv->groups[0].input.pid.store(4242);

    SegmentView attacher;
    REQUIRE(backend.createOrOpen(name, kRvBytes, attacher) == InitResult::kOk);
    REQUIRE_FALSE(attacher.created);
    int calls = 0;
    CHECK(initRv(backend, attacher, kRvSpec, true, &calls) == InitResult::kOk);
    CHECK(calls == 1);
    CHECK(rv->header.magic.load() == kRvMagic);
    CHECK(rv->header.generation.load() == 1u); // 原来是 0，覆盖一次 + 1
    CHECK(rv->groups[0].input.pid.load() == 0u); // clearFrom 起清零
}
