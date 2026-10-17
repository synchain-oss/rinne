// SPDX-License-Identifier: GPL-3.0-or-later
//
// 共享内存段名。一个会合段 rv 加 8 个数据段，名字由「平台头 + 前缀族 + 作用域 + 段」拼成：
//
//                  正式（构建期前缀）                              测试作用域（t + 6 位小写十六进制）
//   Windows        Local\SynchainRinne.rv                          Local\SnchRinne.t1a2b3c.rv
//                  Local\SynchainRinne.v1.g1 … g8                  Local\SnchRinne.t1a2b3c.v1.g1 … g8
//   POSIX          /SnchRinne.<uid8>.rv                            /SnchRinne.t1a2b3c.rv
//                  /SnchRinne.<uid8>.v1.g1 … g8                    /SnchRinne.t1a2b3c.v1.g1 … g8
//
// - 会合段名永不带 abi：abi 不同的两个版本打开的是同一个 rv，才能互相看见并报告不兼容；数据段名带 v<abi>，
//   不同 abi 的环互不相干。
// - 前缀族由构建期的 RINNE_SHM_PREFIX 决定，只有三族：正式版、本机 gates 构建（带 D）、探针（带 P）。
//   POSIX 名用短写（Synchain → Snch），给 uid8 腾出位置：同一台机器上不同用户的段互不相见。
// - 测试作用域只经 API 注入（NameScope::forTesting / randomForTesting），发布版不读任何环境变量。测试名在两个
//   平台上同形同长，都用短写族名；作用域标签以 t 开头，不是十六进制，与任何 uid8 都撞不上。
// - 名字（含平台头）不超过 kMaxSegmentNameLength = 31 个字符，两个平台统一按 macOS 的 POSIX 上限检查。
//   给 abi 留了两位数的增长空间：abi 到 99 时最长的名字（Windows 上的测试数据段名）正好 31，golden 与单测都核这一条。
//   超长、字符不合法时拼名失败，调用方按「IPC 不可用」处理，只出干声。
// 只在非实时线程调用（拼名要分配）。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "ipc/SegmentLayout.h"

namespace rinne::ipc
{

enum class NamePlatform : std::uint8_t
{
    kWin32,
    kPosix,
};

#if defined(_WIN32)
inline constexpr NamePlatform kHostNamePlatform = NamePlatform::kWin32;
#else
inline constexpr NamePlatform kHostNamePlatform = NamePlatform::kPosix;
#endif

enum class NameStatus : std::uint8_t
{
    kOk,
    kBadPrefix, // 前缀族为空或含字母数字以外的字符
    kBadSegment, // 组号不在 0..8（0 = 会合段）
    kBadHead, // 平台头不对：Windows 要 Local\，POSIX 要 /
    kBadChar, // 名字里有 [A-Za-z0-9._-] 以外的字符，或某一节为空
    kTooLong, // 超过 kMaxSegmentNameLength
};

const char* nameStatusName(NameStatus status) noexcept;

// 段：group == 0 是会合段 rv，1..8 是组 A–H 的数据段 g1..g8
struct SegmentId
{
    std::uint32_t group = 0;

    static constexpr SegmentId rendezvous() noexcept { return SegmentId{0}; }
    static constexpr SegmentId groupData(std::uint32_t g) noexcept { return SegmentId{g}; }
    constexpr bool isRendezvous() const noexcept { return group == 0; }
};

// 前缀族：Windows 用全名，POSIX 用短写
struct PrefixFamily
{
    std::string_view win32;
    std::string_view posix;
};

// 构建期允许的三族，顺序固定：正式、本机 gates、探针。与根 CMakeLists.txt 的三个值一一对应（contract-static-ipc 对拍）
inline constexpr PrefixFamily kPrefixFamilies[3] = {
    {"SynchainRinne", "SnchRinne"},
    {"SynchainRinneD", "SnchRinneD"},
    {"SynchainRinneP", "SnchRinneP"},
};

// 按 Windows 全名找族；不是三族之一时返回 nullptr
constexpr const PrefixFamily* findPrefixFamily(std::string_view win32Prefix) noexcept
{
    for (const auto& family : kPrefixFamilies)
    {
        if (family.win32 == win32Prefix)
        {
            return &family;
        }
    }
    return nullptr;
}

// 作用域：正式（标签为空）或测试（t + 6 位小写十六进制）。只能经下面三个工厂得到。
class NameScope
{
public:
    static constexpr std::size_t kTagLength = 7;

    static NameScope formal() noexcept { return NameScope{}; }
    // 标签不合法时返回空
    static std::optional<NameScope> forTesting(std::string_view tag) noexcept;
    // 随机标签；同一台机器上并行跑的测试进程互不相撞
    static NameScope randomForTesting();

    static bool isValidTestTag(std::string_view tag) noexcept;

    bool isTesting() const noexcept { return tag_[0] != '\0'; }
    std::string_view tag() const noexcept { return std::string_view(tag_); }

private:
    NameScope() noexcept = default;
    char tag_[kTagLength + 1] = {};
};

// 拼段名：平台、前缀族、作用域、uid（只有 POSIX 的正式名用到）与段。成功时 out 是完整的平台名。
// abi 只给单测核增长空间用；发布代码一律用本构建的 kIpcAbi。
NameStatus composeSegmentName(NamePlatform platform, const PrefixFamily& family, const NameScope& scope,
                              std::uint32_t uid, SegmentId segment, std::string& out, std::uint32_t abi = kIpcAbi);

// 核对一个现成的平台名：平台头、字符集、每节非空、长度。后端在碰操作系统之前再查一遍。
NameStatus validateSegmentName(NamePlatform platform, std::string_view name) noexcept;

// 本构建的段名：构建期前缀、本机平台、当前有效用户。插件只用正式作用域；测试与 peer 进程传测试作用域。
class Naming
{
public:
    explicit Naming(NameScope scope = NameScope::formal());

    NameStatus name(SegmentId segment, std::string& out) const;
    const NameScope& scope() const noexcept { return scope_; }
    const PrefixFamily& family() const noexcept { return *family_; }

private:
    NameScope scope_;
    const PrefixFamily* family_;
    std::uint32_t uid_;
};

} // namespace rinne::ipc
