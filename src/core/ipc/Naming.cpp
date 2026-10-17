// SPDX-License-Identifier: GPL-3.0-or-later

#include "ipc/Naming.h"

#include <chrono>
#include <random>

#include "ipc/IpcDiag.h"
#include "ipc/ProcessInfo.h"
#include "util/RinneVersion.h"

namespace rinne::ipc
{

static_assert(findPrefixFamily(build::kShmPrefix) != nullptr,
              "RINNE_SHM_PREFIX 必须是 Naming.h 里三族之一（根 CMakeLists.txt 已限定三个值，两边要一致）");

namespace
{

constexpr std::string_view kWin32Head = "Local\\";
constexpr std::string_view kPosixHead = "/";
constexpr char kHex[] = "0123456789abcdef";

bool isAlnum(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

bool isNameChar(char c) noexcept
{
    return isAlnum(c) || c == '.' || c == '_' || c == '-';
}

bool isLowerHex(char c) noexcept
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

void appendHex8(std::string& out, std::uint32_t value)
{
    for (int shift = 28; shift >= 0; shift -= 4)
    {
        out.push_back(kHex[(value >> shift) & 0xfu]);
    }
}

} // namespace

const char* nameStatusName(NameStatus status) noexcept
{
    switch (status)
    {
    case NameStatus::kOk:
        return "ok";
    case NameStatus::kBadPrefix:
        return "bad-prefix";
    case NameStatus::kBadSegment:
        return "bad-segment";
    case NameStatus::kBadHead:
        return "bad-head";
    case NameStatus::kBadChar:
        return "bad-char";
    case NameStatus::kTooLong:
        return "too-long";
    }
    return "unknown";
}

bool NameScope::isValidTestTag(std::string_view tag) noexcept
{
    if (tag.size() != kTagLength || tag[0] != 't')
    {
        return false;
    }
    for (std::size_t i = 1; i < tag.size(); ++i)
    {
        if (!isLowerHex(tag[i]))
        {
            return false;
        }
    }
    return true;
}

std::optional<NameScope> NameScope::forTesting(std::string_view tag) noexcept
{
    if (!isValidTestTag(tag))
    {
        return std::nullopt;
    }
    NameScope scope;
    tag.copy(scope.tag_, kTagLength);
    scope.tag_[kTagLength] = '\0';
    return scope;
}

NameScope NameScope::randomForTesting()
{
    // random_device 在个别实现上是确定性的，再混进时钟与 pid，并行的测试进程也不会撞上
    std::random_device device;
    const auto ticks = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    std::seed_seq seed{device(), device(), static_cast<std::uint32_t>(ticks), static_cast<std::uint32_t>(ticks >> 32),
                       currentProcessId()};
    std::mt19937 rng(seed);
    NameScope scope;
    scope.tag_[0] = 't';
    for (std::size_t i = 1; i < kTagLength; ++i)
    {
        scope.tag_[i] = kHex[rng() & 0xfu];
    }
    scope.tag_[kTagLength] = '\0';
    return scope;
}

NameStatus composeSegmentName(NamePlatform platform, const PrefixFamily& family, const NameScope& scope,
                              std::uint32_t uid, SegmentId segment, std::string& out)
{
    out.clear();
    if (segment.group > kGroupCount)
    {
        return NameStatus::kBadSegment;
    }
    // 正式名在 Windows 上用全名族，其余（POSIX 的正式名、两个平台的测试名）用短写族
    const bool longFamily = platform == NamePlatform::kWin32 && !scope.isTesting();
    const std::string_view prefix = longFamily ? family.win32 : family.posix;
    if (prefix.empty())
    {
        return NameStatus::kBadPrefix;
    }
    for (const char c : prefix)
    {
        if (!isAlnum(c))
        {
            return NameStatus::kBadPrefix;
        }
    }

    out.append(platform == NamePlatform::kWin32 ? kWin32Head : kPosixHead);
    out.append(prefix);
    if (scope.isTesting())
    {
        out.push_back('.');
        out.append(scope.tag());
    }
    else if (platform == NamePlatform::kPosix)
    {
        out.push_back('.');
        appendHex8(out, uid);
    }
    out.push_back('.');
    if (segment.isRendezvous())
    {
        out.append("rv"); // 会合段名永不带 abi
    }
    else
    {
        out.push_back('v');
        out.append(std::to_string(kIpcAbi));
        out.append(".g");
        out.append(std::to_string(segment.group));
    }

    const NameStatus status = validateSegmentName(platform, out);
    if (status != NameStatus::kOk)
    {
        out.clear();
    }
    return status;
}

NameStatus validateSegmentName(NamePlatform platform, std::string_view name) noexcept
{
    const std::string_view head = platform == NamePlatform::kWin32 ? kWin32Head : kPosixHead;
    if (name.substr(0, head.size()) != head)
    {
        return NameStatus::kBadHead;
    }
    const std::string_view body = name.substr(head.size());
    // 每节非空（不许 ".."、开头或结尾的 '.'），也就排除了 "." 与 ".." 这两个名字
    if (body.empty() || body.front() == '.' || body.back() == '.' || body.find("..") != std::string_view::npos)
    {
        return NameStatus::kBadChar;
    }
    for (const char c : body)
    {
        if (!isNameChar(c))
        {
            return NameStatus::kBadChar;
        }
    }
    if (name.size() > kMaxSegmentNameLength)
    {
        return NameStatus::kTooLong;
    }
    return NameStatus::kOk;
}

void reportBadSegmentName(const std::string& name, NameStatus status) noexcept
{
    IpcDiagEvent e;
    e.op = IpcDiagOp::kName;
    e.code = status == NameStatus::kTooLong ? IpcDiagCode::kNameTooLong : IpcDiagCode::kOther;
    e.segment = name.c_str();
    e.detail = status == NameStatus::kTooLong ? "segment name longer than 31 characters"
                                              : "segment name has a bad platform head or characters";
    reportIpcDiag(e);
}

Naming::Naming(NameScope scope) : scope_(scope), family_(findPrefixFamily(build::kShmPrefix)), uid_(currentUserId()) {}

NameStatus Naming::name(SegmentId segment, std::string& out) const
{
    return composeSegmentName(kHostNamePlatform, *family_, scope_, uid_, segment, out);
}

} // namespace rinne::ipc
