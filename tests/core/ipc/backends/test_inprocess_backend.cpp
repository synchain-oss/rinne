// SPDX-License-Identifier: GPL-3.0-or-later
//
// 进程内后端：语义对齐 Windows（同名已存在时忽略请求大小、回填真实大小）；缓冲 64 字节对齐、清零；
// 名字按本机平台的规则校验；resetAllForTesting 让用例之间不共享残留。本平台的别名与后端名一并核对。

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

#include "DiagCapture.h"
#include "ipc/Naming.h"
#include "ipc/PlatformSegmentBackend.h"
#include "ipc/SegmentBackendInProcess.h"

using namespace rinne::ipc;

TEST_CASE("the in-process backend shares a zeroed, aligned buffer by name", "[ipc][backend][inprocess]")
{
    SegmentBackendInProcess::resetAllForTesting();
    std::string name;
    REQUIRE(Naming(NameScope::randomForTesting()).name(SegmentId::groupData(2), name) == NameStatus::kOk);
    SegmentBackendInProcess a;
    SegmentBackendInProcess b;

    SegmentView va;
    REQUIRE(a.createOrOpen(name, kRingBytes, va) == InitResult::kOk);
    CHECK(va.created);
    CHECK(va.size == kRingBytes);
    CHECK(reinterpret_cast<std::uintptr_t>(va.base) % 64 == 0);
    const auto* bytes = static_cast<const unsigned char*>(va.base);
    CHECK(bytes[0] == 0);
    CHECK(bytes[kRingBytes - 1] == 0);

    SegmentView vb;
    REQUIRE(b.createOrOpen(name, kRvBytes, vb) == InitResult::kOk);
    CHECK_FALSE(vb.created);
    CHECK(vb.base == va.base);
    CHECK(vb.size == kRingBytes); // 回填真实大小，不是请求值

    SegmentView vc;
    REQUIRE(b.openExisting(name, vc) == InitResult::kOk);
    CHECK(vc.base == va.base);
    b.unmap(vc);
    CHECK(vc.base == nullptr);
    // unmap 只清视图：别的实例还在用
    SegmentView vd;
    CHECK(b.openExisting(name, vd) == InitResult::kOk);

    SegmentBackendInProcess::resetAllForTesting();
    SegmentView ve;
    CHECK(b.openExisting(name, ve) == InitResult::kFailed);
    CHECK(a.createOrOpen(name, 0, ve) == InitResult::kFailed);
}

TEST_CASE("the in-process backend validates names like the real one", "[ipc][backend][inprocess]")
{
    SegmentBackendInProcess backend;
    rinne::test::DiagCapture diag;
    SegmentView view;
    const std::string head = kHostNamePlatform == NamePlatform::kWin32 ? "Local\\" : "/";
    CHECK(backend.createOrOpen(head + std::string(32 - head.size(), 'a'), 64, view) == InitResult::kFailedBadName);
    CHECK(diag.has(IpcDiagCode::kNameTooLong));
    CHECK(backend.createOrOpen(head + "a b", 64, view) == InitResult::kFailedBadName);
    CHECK(backend.openExisting("nohead", view) == InitResult::kFailedBadName);
}

TEST_CASE("the platform alias picks the backend for this platform", "[ipc][backend]")
{
    const std::string name = platformSegmentBackendName();
#if defined(_WIN32)
    CHECK(name == "win32");
#elif defined(__APPLE__)
    CHECK(name == "posix");
#else
    CHECK(name == "inprocess");
#endif
    PlatformSegmentBackend backend;
    ISegmentBackend& base = backend;
    SegmentView view;
    CHECK(base.openExisting(std::string(kHostNamePlatform == NamePlatform::kWin32 ? "Local\\" : "/") +
                                "SnchRinne.t0000000.rv",
                            view) == InitResult::kFailed);
}
