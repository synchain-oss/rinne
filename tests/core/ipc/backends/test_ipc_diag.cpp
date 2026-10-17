// SPDX-License-Identifier: GPL-3.0-or-later
//
// 后端的诊断出口：sink 的装与换、errno 的归类（被拒、超长、暂时性）、落盘前的去重限流与日志行的格式。

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <string>

#include "DiagCapture.h"
#include "ipc/ISegmentBackend.h"
#include "ipc/IpcDiag.h"

namespace
{

using namespace rinne::ipc;

IpcDiagEvent event(IpcDiagOp op, IpcDiagCode code, int error, const char* segment)
{
    IpcDiagEvent e;
    e.op = op;
    e.code = code;
    e.error = error;
    e.segment = segment;
    e.detail = "detail";
    return e;
}

} // namespace

TEST_CASE("reports reach the installed sink and nothing else", "[ipc][diag]")
{
    const IpcDiagSink before = ipcDiagSink();
    {
        rinne::test::DiagCapture capture;
        reportIpcDiag(event(IpcDiagOp::kOpen, IpcDiagCode::kShmDenied, EPERM, "/SnchRinne.t0000000.rv"));
        const auto events = capture.events();
        REQUIRE(events.size() == 1);
        CHECK(events[0].op == IpcDiagOp::kOpen);
        CHECK(events[0].code == IpcDiagCode::kShmDenied);
        CHECK(events[0].error == EPERM);
        CHECK(events[0].segment == "/SnchRinne.t0000000.rv");
    }
    CHECK(ipcDiagSink() == before);
    // 没装 sink 时报告是空操作
    const IpcDiagSink saved = setIpcDiagSink(nullptr);
    reportIpcDiag(event(IpcDiagOp::kOpen, IpcDiagCode::kOther, 0, ""));
    setIpcDiagSink(saved);
}

TEST_CASE("errno values are classified and mapped to init results", "[ipc][diag]")
{
    CHECK(classifyErrno(EPERM) == IpcDiagCode::kShmDenied);
    CHECK(classifyErrno(EACCES) == IpcDiagCode::kShmDenied);
    CHECK(classifyErrno(ENOSYS) == IpcDiagCode::kShmDenied);
    CHECK(classifyErrno(ENAMETOOLONG) == IpcDiagCode::kNameTooLong);
    CHECK(classifyErrno(ETIMEDOUT) == IpcDiagCode::kBusy);
    CHECK(classifyErrno(EWOULDBLOCK) == IpcDiagCode::kBusy);
    CHECK(classifyErrno(EAGAIN) == IpcDiagCode::kBusy);
    CHECK(classifyErrno(ENOENT) == IpcDiagCode::kOther);
    CHECK(classifyErrno(0) == IpcDiagCode::kOther);

    CHECK(initResultFor(IpcDiagCode::kShmDenied) == InitResult::kFailedDenied);
    CHECK(initResultFor(IpcDiagCode::kNameTooLong) == InitResult::kFailedBadName);
    CHECK(initResultFor(IpcDiagCode::kAbiMismatch) == InitResult::kAbiMismatch);
    CHECK(initResultFor(IpcDiagCode::kBusy) == InitResult::kFailed);
    CHECK(initResultFor(IpcDiagCode::kOther) == InitResult::kFailed);
    CHECK_FALSE(initFailed(InitResult::kOk));
    CHECK_FALSE(initFailed(InitResult::kAbiMismatch));
    CHECK(initFailed(InitResult::kFailedDenied));

    CHECK(std::string(ipcDiagCodeName(IpcDiagCode::kShmDenied)) == "shm-denied");
    CHECK(std::string(ipcDiagCodeName(IpcDiagCode::kNameTooLong)) == "name-too-long");
    CHECK(std::string(ipcDiagCodeName(IpcDiagCode::kAbiMismatch)) == "abi-mismatch");
    CHECK(std::string(ipcDiagCodeName(IpcDiagCode::kUnsupportedBlock)) == "unsupported-block");
    CHECK(std::string(ipcDiagOpName(IpcDiagOp::kLifecycleLock)) == "lifecycle-lock");
}

TEST_CASE("the limiter lets one line per key through each window", "[ipc][diag][limiter]")
{
    IpcDiagLimiter limiter;
    const auto e = event(IpcDiagOp::kLifecycleLock, IpcDiagCode::kBusy, ETIMEDOUT, "/SnchRinne.t0000000.rv");
    CHECK(limiter.onEvent(e, 1000).emit);
    for (int i = 0; i < 5; ++i)
    {
        CHECK_FALSE(limiter.onEvent(e, 1000 + 100u * static_cast<unsigned>(i)).emit);
    }
    // 别的键不受影响
    CHECK(limiter.onEvent(event(IpcDiagOp::kOpen, IpcDiagCode::kBusy, ETIMEDOUT, "/x"), 1500).emit);
    const auto next = limiter.onEvent(e, 1000 + IpcDiagLimiter::kRepeatWindowMs);
    CHECK(next.emit);
    CHECK(next.repeatsSuppressed == 5);
    CHECK(limiter.keyCount() == 2);

    // 毫秒计数器回绕也照样按窗口算
    IpcDiagLimiter wrap;
    CHECK(wrap.onEvent(e, 0xffffff00u).emit);
    CHECK_FALSE(wrap.onEvent(e, 0x00000010u).emit);
}

TEST_CASE("the limiter caps distinct keys per window and evicts the oldest key", "[ipc][diag][limiter]")
{
    IpcDiagLimiter limiter;
    std::string segments[IpcDiagLimiter::kMaxLinesPerWindow + 3];
    std::uint32_t emitted = 0;
    for (std::uint32_t i = 0; i < IpcDiagLimiter::kMaxLinesPerWindow + 3; ++i)
    {
        segments[i] = "/seg" + std::to_string(i);
        if (limiter.onEvent(event(IpcDiagOp::kOpen, IpcDiagCode::kOther, 1, segments[i].c_str()), 10).emit)
        {
            ++emitted;
        }
    }
    CHECK(emitted == IpcDiagLimiter::kMaxLinesPerWindow);
    const auto later = limiter.onEvent(event(IpcDiagOp::kOpen, IpcDiagCode::kOther, 1, "/later"),
                                       10 + IpcDiagLimiter::kRepeatWindowMs);
    CHECK(later.emit);
    CHECK(later.rateLimited == 3);

    IpcDiagLimiter many;
    for (std::uint32_t i = 0; i < IpcDiagLimiter::kMaxKeys + 10; ++i)
    {
        const std::string seg = "/k" + std::to_string(i);
        // 每条隔一个窗口，全局上限不起作用，只看键的数量
        many.onEvent(event(IpcDiagOp::kOpen, IpcDiagCode::kOther, 1, seg.c_str()), i * IpcDiagLimiter::kRepeatWindowMs);
    }
    CHECK(many.keyCount() == IpcDiagLimiter::kMaxKeys);
}

TEST_CASE("a diagnostic line carries op, class, errno, segment and counts", "[ipc][diag]")
{
    IpcDiagEvent e = event(IpcDiagOp::kOpen, IpcDiagCode::kShmDenied, EPERM, "/SnchRinne.000001f5.rv");
    e.path = "/tmp/lock";
    e.suppressed = 2;
    IpcDiagLimiter::Verdict verdict;
    verdict.emit = true;
    verdict.repeatsSuppressed = 3;
    const std::string line = formatIpcDiagLine(e, verdict);
    CHECK(line.rfind("ipc shm-open shm-denied errno " + std::to_string(EPERM) +
                         " segment /SnchRinne.000001f5.rv path "
                         "/tmp/lock -- detail",
                     0) == 0);
    CHECK(line.find("(backend suppressed 2)") != std::string::npos);
    CHECK(line.find("(3 identical reports suppressed since the last line)") != std::string::npos);
    CHECK(formatIpcDiagLine(event(IpcDiagOp::kName, IpcDiagCode::kOther, 0, ""), {}).find("segment - --") !=
          std::string::npos);
}
