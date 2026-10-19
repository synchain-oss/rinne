// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/ipc.json

#include "ipc/IpcDiag.h"

#include <atomic>
#include <cerrno>
#include <string_view>
#include <utility>

namespace rinne::ipc
{

namespace
{

std::atomic<IpcDiagSink>& sinkSlot() noexcept
{
    static std::atomic<IpcDiagSink> slot{nullptr};
    return slot;
}

std::uint32_t saturatingIncrement(std::uint32_t v) noexcept
{
    return v == 0xffffffffu ? v : v + 1u;
}

} // namespace

IpcDiagSink setIpcDiagSink(IpcDiagSink sink) noexcept
{
    return sinkSlot().exchange(sink, std::memory_order_acq_rel);
}

IpcDiagSink ipcDiagSink() noexcept
{
    return sinkSlot().load(std::memory_order_acquire);
}

void reportIpcDiag(const IpcDiagEvent& event) noexcept
{
    const IpcDiagSink sink = sinkSlot().load(std::memory_order_acquire);
    if (sink != nullptr)
    {
        sink(event);
    }
}

IpcDiagCode classifyErrno(int error) noexcept
{
    switch (error)
    {
    case EPERM:
    case EACCES:
    case ENOSYS:
        return IpcDiagCode::kShmDenied;
    case ENAMETOOLONG:
        return IpcDiagCode::kNameTooLong;
    case ETIMEDOUT:
    case EAGAIN:
    case EINTR:
        return IpcDiagCode::kBusy;
    default:
        break;
    }
    // EWOULDBLOCK 在多数平台上等于 EAGAIN，不能和它写进同一个 switch
    if (error == EWOULDBLOCK)
    {
        return IpcDiagCode::kBusy;
    }
    return IpcDiagCode::kOther;
}

const char* ipcDiagOpName(IpcDiagOp op) noexcept
{
    switch (op)
    {
    case IpcDiagOp::kName:
        return "name";
    case IpcDiagOp::kLockDir:
        return "lock-dir";
    case IpcDiagOp::kLifecycleLock:
        return "lifecycle-lock";
    case IpcDiagOp::kSegmentLock:
        return "segment-lock";
    case IpcDiagOp::kOpen:
        return "shm-open";
    case IpcDiagOp::kTruncate:
        return "shm-truncate";
    case IpcDiagOp::kStat:
        return "shm-stat";
    case IpcDiagOp::kMap:
        return "shm-map";
    case IpcDiagOp::kUnlink:
        return "shm-unlink";
    case IpcDiagOp::kLockPages:
        return "lock-pages";
    case IpcDiagOp::kHeader:
        return "header";
    }
    return "unknown";
}

const char* ipcDiagCodeName(IpcDiagCode code) noexcept
{
    switch (code)
    {
    case IpcDiagCode::kOther:
        return "other";
    case IpcDiagCode::kShmDenied:
        return "shm-denied";
    case IpcDiagCode::kNameTooLong:
        return "name-too-long";
    case IpcDiagCode::kAbiMismatch:
        return "abi-mismatch";
    case IpcDiagCode::kUnsupportedBlock:
        return "unsupported-block";
    case IpcDiagCode::kBusy:
        return "busy";
    case IpcDiagCode::kBadSize:
        return "bad-size";
    case IpcDiagCode::kForeign:
        return "foreign";
    }
    return "unknown";
}

IpcDiagLimiter::Verdict IpcDiagLimiter::onEvent(const IpcDiagEvent& event, std::uint32_t nowMs)
{
    Verdict verdict;
    const std::string_view segment = event.segment;

    Entry* hit = nullptr;
    for (auto& e : entries_)
    {
        if (e.op == event.op && e.code == event.code && e.error == event.error && e.segment == segment)
        {
            hit = &e;
            break;
        }
    }

    // 同一键、窗口内：只计数
    if (hit != nullptr && (nowMs - hit->lastEmitMs) < kRepeatWindowMs)
    {
        hit->suppressed = saturatingIncrement(hit->suppressed);
        return verdict;
    }

    // 要放行了，先过全局上限。被挡下的这一条不更新键的状态（下次照样再试），只进全局计数
    if (!windowStarted_ || (nowMs - windowStartMs_) >= kRepeatWindowMs)
    {
        windowStarted_ = true;
        windowStartMs_ = nowMs;
        linesInWindow_ = 0;
    }
    if (linesInWindow_ >= kMaxLinesPerWindow)
    {
        rateLimited_ = saturatingIncrement(rateLimited_);
        return verdict;
    }
    ++linesInWindow_;

    verdict.emit = true;
    verdict.rateLimited = std::exchange(rateLimited_, 0u);
    if (hit != nullptr)
    {
        verdict.repeatsSuppressed = std::exchange(hit->suppressed, 0u);
        hit->lastEmitMs = nowMs;
        return verdict;
    }

    if (entries_.size() >= kMaxKeys)
    {
        // 淘汰最久没放行过的键（无符号差值最大的最旧，回绕也成立）
        auto oldest = entries_.begin();
        for (auto it = entries_.begin(); it != entries_.end(); ++it)
        {
            if ((nowMs - it->lastEmitMs) > (nowMs - oldest->lastEmitMs))
            {
                oldest = it;
            }
        }
        entries_.erase(oldest);
    }
    Entry e;
    e.op = event.op;
    e.code = event.code;
    e.error = event.error;
    e.segment.assign(segment);
    e.lastEmitMs = nowMs;
    entries_.push_back(std::move(e));
    return verdict;
}

std::string formatIpcDiagLine(const IpcDiagEvent& event, const IpcDiagLimiter::Verdict& verdict)
{
    std::string line = "ipc ";
    line += ipcDiagOpName(event.op);
    line += ' ';
    line += ipcDiagCodeName(event.code);
    line += " errno ";
    line += std::to_string(event.error);
    line += " segment ";
    line += event.segment[0] != '\0' ? event.segment : "-";
    if (event.path[0] != '\0')
    {
        line += " path ";
        line += event.path;
    }
    line += " -- ";
    line += event.detail;
    if (event.suppressed > 0)
    {
        line += " (backend suppressed " + std::to_string(event.suppressed) + ")";
    }
    if (verdict.repeatsSuppressed > 0)
    {
        line += " (" + std::to_string(verdict.repeatsSuppressed) + " identical reports suppressed since the last line)";
    }
    if (verdict.rateLimited > 0)
    {
        line += " (" + std::to_string(verdict.rateLimited) + " ipc reports dropped by the rate limit)";
    }
    return line;
}

} // namespace rinne::ipc
