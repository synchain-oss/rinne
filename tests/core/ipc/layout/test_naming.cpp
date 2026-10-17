// SPDX-License-Identifier: GPL-3.0-or-later
//
// 段名：三族前缀 × 9 段 × 2 平台 × {正式, 测试} 与 golden 逐条相等、都不超过 31 字符；会合段名不带 abi；
// 测试作用域只经 API 注入；超长的前缀、坏的名字在碰操作系统之前就被拒。

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

#include "GoldenReader.h"
#include "ipc/Naming.h"
#include "ipc/ProcessInfo.h"
#include "util/RinneVersion.h"

namespace
{

using namespace rinne::ipc;

// golden 里 uid 与测试标签写成占位符；这里用固定值拼名，再换回占位符比
constexpr std::uint32_t kUid = 0x01234567u;
const char* const kUidText = "01234567";
const char* const kTag = "t89abcde";

void replaceOnce(std::string& text, const std::string& from, const std::string& to)
{
    if (const auto at = text.find(from); at != std::string::npos)
    {
        text.replace(at, from.size(), to);
    }
}

std::string withPlaceholders(std::string name)
{
    replaceOnce(name, kUidText, "<uid8>");
    replaceOnce(name, kTag, "<tag8>");
    return name;
}

NameScope testScope()
{
    const auto scope = NameScope::forTesting(kTag);
    REQUIRE(scope.has_value());
    return *scope;
}

} // namespace

TEST_CASE("segment names match the golden for every prefix, platform and scope", "[ipc][naming][golden]")
{
    const auto golden = rinne::test::loadIpcGolden();
    INFO("golden: " << golden.path);
    REQUIRE(golden.loaded);
    const auto& rows = golden.sections.at("names");
    REQUIRE(rows.size() == 3u * 2u * 2u * 9u);

    std::size_t i = 0;
    for (const auto& family : kPrefixFamilies)
    {
        for (const auto platform : {NamePlatform::kWin32, NamePlatform::kPosix})
        {
            for (const bool testing : {false, true})
            {
                for (std::uint32_t g = 0; g <= kGroupCount; ++g)
                {
                    const NameScope scope = testing ? testScope() : NameScope::formal();
                    std::string name;
                    REQUIRE(composeSegmentName(platform, family, scope, kUid, SegmentId{g}, name) == NameStatus::kOk);
                    const auto& row = rows.at(i++);
                    INFO(row.line);
                    CHECK(row.str("platform") == std::string(platform == NamePlatform::kWin32 ? "win32" : "posix"));
                    CHECK(row.str("scope") == std::string(testing ? "testing" : "formal"));
                    CHECK(row.str("prefix") == std::string(family.win32));
                    CHECK(row.str("segment") == (g == 0 ? std::string("rv") : "g" + std::to_string(g)));
                    CHECK(row.str("name") == withPlaceholders(name));
                    CHECK(row.num("length") == static_cast<long long>(name.size()));
                    CHECK(name.size() <= kMaxSegmentNameLength);
                }
            }
        }
    }
}

TEST_CASE("the rendezvous name never carries the abi, data names always do", "[ipc][naming]")
{
    for (const auto& family : kPrefixFamilies)
    {
        for (const auto platform : {NamePlatform::kWin32, NamePlatform::kPosix})
        {
            std::string rv;
            std::string g1;
            REQUIRE(composeSegmentName(platform, family, NameScope::formal(), kUid, SegmentId::rendezvous(), rv) ==
                    NameStatus::kOk);
            REQUIRE(composeSegmentName(platform, family, NameScope::formal(), kUid, SegmentId::groupData(1), g1) ==
                    NameStatus::kOk);
            CHECK(rv.find(".v") == std::string::npos);
            CHECK(rv.size() >= 3);
            CHECK(rv.compare(rv.size() - 3, 3, ".rv") == 0);
            CHECK(g1.find(".v" + std::to_string(kIpcAbi) + ".g1") != std::string::npos);
        }
    }
}

TEST_CASE("an injected long prefix is rejected as too long", "[ipc][naming][mutation]")
{
    // 必须变红的反例：前缀多出几个字符，最长的名字就过了 31；会合段名短，同一个前缀下仍在上限内
    const PrefixFamily longFamily{"SynchainRinneDevelop", "SnchRinneDevelop"};
    std::string name;
    CHECK(composeSegmentName(NamePlatform::kPosix, longFamily, NameScope::formal(), kUid, SegmentId::groupData(8),
                             name) == NameStatus::kTooLong);
    CHECK(name.empty());
    CHECK(composeSegmentName(NamePlatform::kWin32, longFamily, NameScope::formal(), kUid, SegmentId::groupData(8),
                             name) == NameStatus::kTooLong);
    CHECK(composeSegmentName(NamePlatform::kWin32, longFamily, testScope(), kUid, SegmentId::groupData(8), name) ==
          NameStatus::kTooLong);
    CHECK(composeSegmentName(NamePlatform::kWin32, longFamily, NameScope::formal(), kUid, SegmentId::rendezvous(),
                             name) == NameStatus::kOk);
}

TEST_CASE("bad prefixes, groups and names are rejected", "[ipc][naming]")
{
    std::string name;
    const PrefixFamily dotted{"Synchain.Rinne", "Snch.Rinne"};
    const PrefixFamily empty{"", ""};
    CHECK(composeSegmentName(NamePlatform::kPosix, dotted, NameScope::formal(), kUid, SegmentId{1}, name) ==
          NameStatus::kBadPrefix);
    CHECK(composeSegmentName(NamePlatform::kPosix, empty, NameScope::formal(), kUid, SegmentId{1}, name) ==
          NameStatus::kBadPrefix);
    CHECK(composeSegmentName(NamePlatform::kPosix, kPrefixFamilies[0], NameScope::formal(), kUid, SegmentId{9}, name) ==
          NameStatus::kBadSegment);

    CHECK(validateSegmentName(NamePlatform::kWin32, "Local\\SynchainRinne.rv") == NameStatus::kOk);
    CHECK(validateSegmentName(NamePlatform::kWin32, "Global\\SynchainRinne.rv") == NameStatus::kBadHead);
    CHECK(validateSegmentName(NamePlatform::kWin32, "/SnchRinne.rv") == NameStatus::kBadHead);
    CHECK(validateSegmentName(NamePlatform::kPosix, "/SnchRinne.01234567.rv") == NameStatus::kOk);
    CHECK(validateSegmentName(NamePlatform::kPosix, "/SnchRinne/x.rv") == NameStatus::kBadChar);
    CHECK(validateSegmentName(NamePlatform::kPosix, "/") == NameStatus::kBadChar);
    CHECK(validateSegmentName(NamePlatform::kPosix, "/..") == NameStatus::kBadChar);
    CHECK(validateSegmentName(NamePlatform::kPosix, "/a..b") == NameStatus::kBadChar);
    CHECK(validateSegmentName(NamePlatform::kPosix, "/SnchRinne.rv.") == NameStatus::kBadChar);
    CHECK(validateSegmentName(NamePlatform::kPosix, "/Snch Rinne.rv") == NameStatus::kBadChar);
    CHECK(validateSegmentName(NamePlatform::kPosix, std::string("/") + std::string(30, 'a')) == NameStatus::kOk);
    CHECK(validateSegmentName(NamePlatform::kPosix, std::string("/") + std::string(31, 'a')) == NameStatus::kTooLong);
}

TEST_CASE("test scopes are injected only through the API and stay distinct", "[ipc][naming][scope]")
{
    CHECK_FALSE(NameScope::formal().isTesting());
    CHECK(NameScope::isValidTestTag("t0123abc"));
    CHECK_FALSE(NameScope::isValidTestTag("t0123ab")); // 太短
    CHECK_FALSE(NameScope::isValidTestTag("t0123abcd")); // 太长
    CHECK_FALSE(NameScope::isValidTestTag("00123abc")); // 不以 t 开头：会和 uid8 撞上
    CHECK_FALSE(NameScope::isValidTestTag("t0123ABC")); // 只许小写十六进制
    CHECK_FALSE(NameScope::forTesting("x").has_value());

    std::set<std::string> tags;
    for (int i = 0; i < 64; ++i)
    {
        const NameScope s = NameScope::randomForTesting();
        CHECK(s.isTesting());
        CHECK(NameScope::isValidTestTag(s.tag()));
        tags.insert(std::string(s.tag()));
    }
    CHECK(tags.size() >= 60); // 28 位随机数，64 个里撞上几个的概率可以忽略

    // 同一标签、两个平台的测试名同形同长；与同一 uid 的正式名不同
    std::string win;
    std::string posix;
    std::string formal;
    REQUIRE(composeSegmentName(NamePlatform::kWin32, kPrefixFamilies[1], testScope(), kUid, SegmentId{3}, win) ==
            NameStatus::kOk);
    REQUIRE(composeSegmentName(NamePlatform::kPosix, kPrefixFamilies[1], testScope(), kUid, SegmentId{3}, posix) ==
            NameStatus::kOk);
    REQUIRE(composeSegmentName(NamePlatform::kPosix, kPrefixFamilies[1], NameScope::formal(), kUid, SegmentId{3},
                               formal) == NameStatus::kOk);
    CHECK(win == "Local\\SnchRinneD.t89abcde.v1.g3");
    CHECK(posix == "/SnchRinneD.t89abcde.v1.g3");
    CHECK(formal == "/SnchRinneD.01234567.v1.g3");
}

TEST_CASE("the build's Naming uses the configured prefix and the host platform", "[ipc][naming]")
{
    const Naming naming;
    CHECK_FALSE(naming.scope().isTesting());
    CHECK(naming.family().win32 == std::string_view(rinne::build::kShmPrefix));
    std::string rv;
    REQUIRE(naming.name(SegmentId::rendezvous(), rv) == NameStatus::kOk);
#if defined(_WIN32)
    CHECK(rv == "Local\\" + std::string(rinne::build::kShmPrefix) + ".rv");
#else
    std::string uid;
    const char* hex = "0123456789abcdef";
    for (int shift = 28; shift >= 0; shift -= 4)
    {
        uid.push_back(hex[(currentUserId() >> shift) & 0xfu]);
    }
    CHECK(rv == "/" + std::string(naming.family().posix) + "." + uid + ".rv");
#endif
    const Naming scoped(NameScope::randomForTesting());
    std::string scopedRv;
    REQUIRE(scoped.name(SegmentId::rendezvous(), scopedRv) == NameStatus::kOk);
    CHECK(scopedRv != rv);
    CHECK(scopedRv.find(std::string(scoped.scope().tag())) != std::string::npos);
}
