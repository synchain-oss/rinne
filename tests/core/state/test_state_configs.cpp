// SPDX-License-Identifier: GPL-3.0-or-later
//
// CFGS 载荷层：键号与类型、默认值、越界值的纠正（枚举取默认、ui_scale 夹到 33..300）、已知键长度不对整份失败、
// 重复键、未知键原样保留，以及 Output 只认它那五个键。期望值都按键号裁定手写，不从键表推，键表改错了才会被发现。

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "StateTestKit.h"
#include "state/StateCodec.h"

namespace
{

using rinne::state::ConfigEntry;
using rinne::state::ConfigKey;
using rinne::state::Configs;
using rinne::state::ConfigType;
using rinne::state::DelayUnit;
using rinne::state::FbUnit;
using rinne::state::GroupOrigin;
using rinne::state::Role;
using rinne::state::TailMode;
using rinne::test::byteList;
using rinne::test::Bytes;
using rinne::test::concat;
using rinne::test::kv;
using rinne::test::sameConfigs;
using rinne::test::textBytes;

Configs sentinelConfigs()
{
    Configs configs;
    configs.group = 7;
    configs.uiScale = 222;
    configs.uiLang = "sentinel";
    configs.unknown.push_back(ConfigEntry{900, byteList({1, 2})});
    return configs;
}

Configs decodeOk(const Bytes& payload, Role role = Role::Input)
{
    Configs configs = sentinelConfigs();
    REQUIRE(rinne::state::decodeConfigs(payload.data(), payload.size(), role, configs));
    return configs;
}

void requireRejected(const Bytes& payload, Role role = Role::Input)
{
    Configs out = sentinelConfigs();
    CHECK_FALSE(rinne::state::decodeConfigs(payload.data(), payload.size(), role, out));
    CHECK(sameConfigs(out, sentinelConfigs()));
}

Bytes encodeOk(const Configs& configs, Role role = Role::Input)
{
    Bytes out;
    REQUIRE(rinne::state::encodeConfigs(configs, role, out));
    return out;
}

void requireEncodeFails(const Configs& configs, Role role = Role::Input)
{
    Bytes out = byteList({0xff});
    CHECK_FALSE(rinne::state::encodeConfigs(configs, role, out));
    CHECK(out == byteList({0xff}));
}

// 默认值按键号裁定手写：组号 0、来源未定、起播复位开、tail 自动、毫秒、百分比、失控保护开、缩放 100 %、
// 语言与预置名为空、预置未改动。
Bytes defaultInputPayload()
{
    return concat({kv(1, {0}), kv(2, {0}), kv(3, {1}), kv(4, {0}), kv(5, {0}), kv(6, {0}), kv(7, {1}), kv(8, {100, 0}),
                   kv(9, {}), kv(10, {}), kv(11, {0})});
}

Bytes defaultOutputPayload()
{
    return concat({kv(1, {0}), kv(2, {0}), kv(4, {0}), kv(8, {100, 0}), kv(9, {})});
}

// 编码结果里某个键的值字节；没有这个键时 REQUIRE 失败。
Bytes valueOf(const Bytes& payload, std::uint16_t key)
{
    std::size_t at = 0;
    while (at + 4u <= payload.size())
    {
        const std::uint16_t k = static_cast<std::uint16_t>(payload[at] | (payload[at + 1] << 8));
        const std::size_t length = static_cast<std::size_t>(payload[at + 2] | (payload[at + 3] << 8));
        if (k == key)
            return Bytes(payload.begin() + static_cast<std::ptrdiff_t>(at + 4u),
                         payload.begin() + static_cast<std::ptrdiff_t>(at + 4u + length));
        at += 4u + length;
    }
    FAIL("编码结果里没有键 " << key);
    return {};
}

} // namespace

TEST_CASE("configs: 键表与键号裁定一致", "[state][configs]")
{
    struct Expected
    {
        std::uint16_t key;
        const char* name;
        ConfigType type;
        bool output;
    };
    const std::vector<Expected> expected = {
        {1, "group", ConfigType::U8, true},          {2, "group_origin", ConfigType::U8, true},
        {3, "reset_on_play", ConfigType::U8, false}, {4, "tail_mode", ConfigType::U8, true},
        {5, "delay_unit", ConfigType::U8, false},    {6, "fb_unit", ConfigType::U8, false},
        {7, "guard_on", ConfigType::U8, false},      {8, "ui_scale", ConfigType::U16, true},
        {9, "ui_lang", ConfigType::Ascii, true},     {10, "preset_name", ConfigType::Utf8, false},
        {11, "preset_dirty", ConfigType::U8, false},
    };
    REQUIRE(rinne::state::kConfigFields.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i)
    {
        const auto& field = rinne::state::kConfigFields[i];
        INFO("键 " << expected[i].key);
        CHECK(static_cast<std::uint16_t>(field.key) == expected[i].key);
        CHECK(std::string(field.name) == expected[i].name);
        CHECK(field.type == expected[i].type);
        CHECK(field.input);
        CHECK(field.output == expected[i].output);
        CHECK(rinne::state::findConfigField(expected[i].key) == &field);
        CHECK(rinne::state::findConfigField(Role::Input, expected[i].key) == &field);
        CHECK((rinne::state::findConfigField(Role::Output, expected[i].key) == &field) == expected[i].output);
        if (field.type == ConfigType::U8 || field.type == ConfigType::U16)
        {
            CHECK(field.bytes == (field.type == ConfigType::U8 ? 1u : 2u));
            CHECK(field.minValue <= field.defaultValue);
            CHECK(field.defaultValue <= field.maxValue);
        }
    }
    CHECK(rinne::state::findConfigField(static_cast<std::uint16_t>(0)) == nullptr);
    CHECK(rinne::state::findConfigField(static_cast<std::uint16_t>(12)) == nullptr);
    CHECK(rinne::state::findConfigField(static_cast<Role>(9), 1) == nullptr);
}

TEST_CASE("configs: 默认值逐字节固定", "[state][configs]")
{
    CHECK(encodeOk(Configs{}, Role::Input) == defaultInputPayload());
    CHECK(encodeOk(Configs{}, Role::Output) == defaultOutputPayload());
    CHECK(sameConfigs(decodeOk(defaultInputPayload()), Configs{}));
    CHECK(sameConfigs(decodeOk(defaultOutputPayload(), Role::Output), Configs{}));

    // 空载荷 = 一个键都没有：全部默认，不算坏字节；写回去时已知键一个不少
    const Configs empty = decodeOk({});
    CHECK(sameConfigs(empty, Configs{}));
    CHECK(encodeOk(empty) == defaultInputPayload());
}

TEST_CASE("configs: 逐个已知键往返", "[state][configs]")
{
    struct Case
    {
        ConfigKey key;
        void (*set)(Configs&);
        Bytes value;
    };
    const std::vector<Case> cases = {
        {ConfigKey::Group, [](Configs& c) { c.group = 5; }, byteList({5})},
        {ConfigKey::GroupOrigin, [](Configs& c) { c.groupOrigin = GroupOrigin::Smart; }, byteList({2})},
        {ConfigKey::ResetOnPlay, [](Configs& c) { c.resetOnPlay = false; }, byteList({0})},
        {ConfigKey::TailMode, [](Configs& c) { c.tailMode = TailMode::ThirtySeconds; }, byteList({2})},
        {ConfigKey::DelayUnit, [](Configs& c) { c.delayUnit = DelayUnit::Samples; }, byteList({1})},
        {ConfigKey::FbUnit, [](Configs& c) { c.fbUnit = FbUnit::Decibel; }, byteList({1})},
        {ConfigKey::GuardOn, [](Configs& c) { c.guardOn = false; }, byteList({0})},
        {ConfigKey::UiScale, [](Configs& c) { c.uiScale = 300; }, byteList({0x2c, 0x01})},
        {ConfigKey::UiLang, [](Configs& c) { c.uiLang = "zh-CN"; }, textBytes("zh-CN")},
        {ConfigKey::PresetName, [](Configs& c) { c.presetName = "长保持 92%"; }, textBytes("长保持 92%")},
        {ConfigKey::PresetDirty, [](Configs& c) { c.presetDirty = true; }, byteList({1})},
    };
    REQUIRE(cases.size() == rinne::state::kConfigFields.size());
    for (const auto& c : cases)
    {
        const auto key = static_cast<std::uint16_t>(c.key);
        INFO("键 " << key);
        Configs configs;
        c.set(configs);
        const Bytes encoded = encodeOk(configs);
        CHECK(valueOf(encoded, key) == c.value);
        CHECK(sameConfigs(decodeOk(encoded), configs));
        // 只改这一个键：解码后其余字段仍是默认
        CHECK(sameConfigs(decodeOk(rinne::test::kv(key, c.value)), configs));
    }
}

TEST_CASE("configs: 越界值取默认，ui_scale 夹到 33..300", "[state][configs]")
{
    CHECK(decodeOk(kv(1, {8})).group == 8u);
    CHECK(decodeOk(kv(1, {9})).group == 0u);
    CHECK(decodeOk(kv(1, {255})).group == 0u);
    CHECK(decodeOk(kv(2, {3})).groupOrigin == GroupOrigin::Undecided);
    CHECK(decodeOk(kv(3, {2})).resetOnPlay);
    CHECK(decodeOk(kv(4, {3})).tailMode == TailMode::Auto);
    CHECK(decodeOk(kv(5, {2})).delayUnit == DelayUnit::Milliseconds);
    CHECK(decodeOk(kv(6, {2})).fbUnit == FbUnit::Percent);
    CHECK(decodeOk(kv(7, {2})).guardOn);
    CHECK_FALSE(decodeOk(kv(11, {2})).presetDirty);

    CHECK(decodeOk(kv(8, {0, 0})).uiScale == 33u); // 0 也夹到下限，不是「未设」
    CHECK(decodeOk(kv(8, {32, 0})).uiScale == 33u);
    CHECK(decodeOk(kv(8, {33, 0})).uiScale == 33u);
    CHECK(decodeOk(kv(8, {0x2c, 0x01})).uiScale == 300u);
    CHECK(decodeOk(kv(8, {0x2d, 0x01})).uiScale == 300u);
    CHECK(decodeOk(kv(8, {0xff, 0xff})).uiScale == 300u);

    // Output 上同一套规则
    CHECK(decodeOk(kv(1, {9}), Role::Output).group == 0u);
    CHECK(decodeOk(kv(8, {1, 0}), Role::Output).uiScale == 33u);
}

TEST_CASE("configs: 已知键长度不对整份失败", "[state][configs]")
{
    for (const auto& field : rinne::state::kConfigFields)
    {
        const auto key = static_cast<std::uint16_t>(field.key);
        INFO("键 " << key);
        if (field.type == ConfigType::U8 || field.type == ConfigType::U16)
        {
            requireRejected(kv(key, {}));
            requireRejected(kv(key, Bytes(field.bytes - 1u, 0u)));
            requireRejected(kv(key, Bytes(field.bytes + 1u, 0u)));
        }
        else
        {
            requireRejected(kv(key, Bytes(field.bytes + 1u, 'a')));
            CHECK(decodeOk(kv(key, Bytes(field.bytes, 'a'))).unknown.empty());
        }
        // 坏的这一条夹在合法的条目中间也一样
        requireRejected(concat({kv(900, {1}), kv(key, Bytes(field.bytes + 1u, 'a')), kv(901, {2})}));
        if (field.output)
            requireRejected(kv(key, Bytes(field.bytes + 1u, 'a')), Role::Output);
    }
}

TEST_CASE("configs: 已知键重复出现整份失败，未知键重复照收", "[state][configs]")
{
    requireRejected(concat({kv(1, {1}), kv(1, {2})}));
    requireRejected(concat({kv(8, {100, 0}), kv(3, {1}), kv(8, {100, 0})}));
    requireRejected(concat({kv(4, {1}), kv(4, {1})}), Role::Output);

    const Configs configs = decodeOk(concat({kv(900, {1}), kv(900, {2})}));
    REQUIRE(configs.unknown.size() == 2u);
    CHECK(configs.unknown[1].bytes == byteList({2}));
}

TEST_CASE("configs: 未知键原样保留，写在已知键之后、彼此顺序不变", "[state][configs]")
{
    const Configs configs = decodeOk(concat({kv(900, {0xaa}), kv(1, {3}), kv(12, {}), kv(65535, {0xbb, 0xcc})}));
    CHECK(configs.group == 3u);
    REQUIRE(configs.unknown.size() == 3u);
    CHECK(configs.unknown[0].key == 900u);
    CHECK(configs.unknown[0].bytes == byteList({0xaa}));
    CHECK(configs.unknown[1].key == 12u);
    CHECK(configs.unknown[1].bytes.empty());
    CHECK(configs.unknown[2].key == 65535u);

    Configs expected;
    expected.group = 3;
    const Bytes known = encodeOk(expected);
    CHECK(encodeOk(configs) == concat({known, kv(900, {0xaa}), kv(12, {}), kv(65535, {0xbb, 0xcc})}));
}

TEST_CASE("configs: Output 只认五个键，其余按未知键原样带着", "[state][configs]")
{
    // Input 专有的键在 Output 上不解读：长度「不对」也不管，原样保留
    const Bytes payload = concat({kv(1, {4}), kv(2, {2}), kv(3, {0, 0}), kv(4, {2}), kv(5, {1}), kv(6, {1}), kv(7, {0}),
                                  kv(8, {200, 0}), kv(9, textBytes("en")), kv(10, textBytes("Hold")), kv(11, {1})});
    const Configs configs = decodeOk(payload, Role::Output);
    CHECK(configs.group == 4u);
    CHECK(configs.groupOrigin == GroupOrigin::Smart);
    CHECK(configs.tailMode == TailMode::ThirtySeconds);
    CHECK(configs.uiScale == 200u);
    CHECK(configs.uiLang == "en");
    CHECK(configs.resetOnPlay);
    CHECK(configs.delayUnit == DelayUnit::Milliseconds);
    CHECK(configs.fbUnit == FbUnit::Percent);
    CHECK(configs.guardOn);
    CHECK(configs.presetName.empty());
    CHECK_FALSE(configs.presetDirty);
    REQUIRE(configs.unknown.size() == 6u);

    CHECK(encodeOk(configs, Role::Output) ==
          concat({kv(1, {4}), kv(2, {2}), kv(4, {2}), kv(8, {200, 0}), kv(9, textBytes("en")), kv(3, {0, 0}),
                  kv(5, {1}), kv(6, {1}), kv(7, {0}), kv(10, textBytes("Hold")), kv(11, {1})}));
}

TEST_CASE("configs: 语言代码与预置名的内容规则", "[state][configs]")
{
    SECTION("语言代码：字母、数字与连字符")
    {
        CHECK(decodeOk(kv(9, textBytes("zh-CN"))).uiLang == "zh-CN");
        CHECK(decodeOk(kv(9, textBytes("fr"))).uiLang == "fr");
        CHECK(decodeOk(kv(9, textBytes("09azAZ-"))).uiLang == "09azAZ-");
        for (const char* bad : {"zh_CN", "e n", "fr\"", "<b>", "en/", "en:"})
        {
            INFO(bad);
            CHECK(decodeOk(kv(9, textBytes(bad))).uiLang.empty());
        }
        CHECK(decodeOk(kv(9, {'e', 0x80})).uiLang.empty());
        CHECK(decodeOk(kv(9, {'e', 0x00})).uiLang.empty());
    }

    SECTION("预置名：合法 UTF-8")
    {
        CHECK(decodeOk(kv(10, textBytes("Long Hold"))).presetName == "Long Hold");
        CHECK(decodeOk(kv(10, {0xc2, 0x80})).presetName.size() == 2u); // U+0080，两字节的下限
        CHECK(decodeOk(kv(10, {0xe0, 0xa0, 0x80})).presetName.size() == 3u); // U+0800
        CHECK(decodeOk(kv(10, {0xf0, 0x90, 0x80, 0x80})).presetName.size() == 4u); // U+10000
        CHECK(decodeOk(kv(10, {0xf4, 0x8f, 0xbf, 0xbf})).presetName.size() == 4u); // U+10FFFF 是上界
        CHECK(decodeOk(kv(10, {0xed, 0x9f, 0xbf})).presetName.size() == 3u); // U+D7FF，代理区前一个
        CHECK(decodeOk(kv(10, {0xee, 0x80, 0x80})).presetName.size() == 3u); // U+E000，代理区后一个
        CHECK(decodeOk(kv(10, {' ', '~'})).presetName == " ~");
    }

    SECTION("坏 UTF-8 取默认，不整份失败")
    {
        const std::vector<Bytes> badNames = {
            byteList({0x80}), // 孤立的续字节
            byteList({0xc1, 0xbf}), // 首字节必然过长
            byteList({0xe0, 0x9f, 0xbf}), // 三字节的过长编码（U+07FF）
            byteList({0xf0, 0x8f, 0xbf, 0xbf}), // 四字节的过长编码（U+FFFF）
            byteList({0xed, 0xa0, 0x80}), // 代理区 U+D800
            byteList({0xed, 0xbf, 0xbf}), // 代理区 U+DFFF
            byteList({0xf4, 0x90, 0x80, 0x80}), // 超出 U+10FFFF
            byteList({0xf5, 0x80, 0x80, 0x80}), // 不合法的首字节
            byteList({'a', 0xe4, 0xbd}), // 末尾截断的多字节序列
            byteList({0xe4, 0x41, 0x80}), // 续字节不是 10xxxxxx
            byteList({0x1f}), // C0 控制字符
            byteList({'a', 0x00}), // NUL
            byteList({0x7f}), // DEL
        };
        for (const auto& name : badNames)
        {
            const Configs configs = decodeOk(kv(10, name));
            CHECK(configs.presetName.empty());
            CHECK(configs.unknown.empty());
        }
    }
}

TEST_CASE("configs: 编码按同一套规则纠正字段值", "[state][configs]")
{
    Configs configs;
    configs.group = 200;
    configs.groupOrigin = static_cast<GroupOrigin>(9);
    configs.tailMode = static_cast<TailMode>(7);
    configs.delayUnit = static_cast<DelayUnit>(3);
    configs.fbUnit = static_cast<FbUnit>(4);
    configs.uiScale = 5;
    configs.uiLang = "x_y";
    configs.presetName = std::string("bad") + static_cast<char>(0x01);
    CHECK(encodeOk(configs) == concat({kv(1, {0}), kv(2, {0}), kv(3, {1}), kv(4, {0}), kv(5, {0}), kv(6, {0}),
                                       kv(7, {1}), kv(8, {33, 0}), kv(9, {}), kv(10, {}), kv(11, {0})}));
    configs.uiScale = 4000;
    CHECK(valueOf(encodeOk(configs), 8) == byteList({0x2c, 0x01}));

    // 语言代码超长时整个不要；预置名超长时截在字符边界上（每个「回」占 3 字节，96 字节正好 32 个）
    configs.uiLang = "abcdefghi";
    std::string name;
    for (int i = 0; i < 33; ++i)
        name += "回";
    configs.presetName = name;
    const Bytes encoded = encodeOk(configs);
    CHECK(valueOf(encoded, 9).empty());
    CHECK(valueOf(encoded, 10) == textBytes(name.substr(0, 96)));
    configs.presetName = std::string(95, 'a') + "回";
    CHECK(valueOf(encodeOk(configs), 10) == textBytes(std::string(95, 'a')));
    configs.presetName = std::string(96, 'a');
    CHECK(valueOf(encodeOk(configs), 10) == textBytes(std::string(96, 'a')));
    configs.uiLang = "abcdefgh";
    CHECK(valueOf(encodeOk(configs), 9) == textBytes("abcdefgh"));
}

TEST_CASE("configs: 编码失败的情形", "[state][configs]")
{
    requireEncodeFails(Configs{}, static_cast<Role>(9));

    Configs knownInUnknown;
    knownInUnknown.unknown.push_back(ConfigEntry{1, byteList({2})});
    requireEncodeFails(knownInUnknown);
    // 对 Output 来说键 3 是未知键，可以带着
    Configs peerKey;
    peerKey.unknown.push_back(ConfigEntry{3, byteList({0})});
    Bytes out;
    CHECK(rinne::state::encodeConfigs(peerKey, Role::Output, out));
    requireEncodeFails(peerKey, Role::Input);

    Configs tooLongEntry;
    tooLongEntry.unknown.push_back(ConfigEntry{900, Bytes(65536u, 0x5au)});
    requireEncodeFails(tooLongEntry);

    Configs tooBig;
    tooBig.unknown.push_back(ConfigEntry{900, Bytes(40000u, 0x11u)});
    tooBig.unknown.push_back(ConfigEntry{901, Bytes(40000u, 0x22u)});
    requireEncodeFails(tooBig);
}

TEST_CASE("configs: 截断、尾部残留、角色与空指针", "[state][configs]")
{
    // 没有总长度字段：切在条目边界上是一份更短的合法载荷，切在条目中间必须整份失败
    const Bytes payload = concat({kv(1, {2}), kv(8, {150, 0}), kv(900, {1, 2})});
    const std::vector<std::size_t> boundaries = {0u, 5u, 11u, payload.size()};
    for (std::size_t size = 0; size <= payload.size(); ++size)
    {
        const Bytes prefix(payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(size));
        bool onBoundary = false;
        for (const std::size_t b : boundaries)
            onBoundary = onBoundary || b == size;
        INFO("前缀长度 " << size);
        if (onBoundary)
            decodeOk(prefix);
        else
            requireRejected(prefix);
    }

    requireRejected(payload, static_cast<Role>(9));
    Configs out = sentinelConfigs();
    CHECK_FALSE(rinne::state::decodeConfigs(nullptr, 4u, Role::Input, out));
    CHECK(sameConfigs(out, sentinelConfigs()));
    CHECK(rinne::state::decodeConfigs(nullptr, 0u, Role::Input, out));
    CHECK(sameConfigs(out, Configs{}));
}

TEST_CASE("configs: 未知条目的预算保证写得回去", "[state][configs]")
{
    // Input 全部已知键按最长的值写要 8×5 + 6 + (4+8) + (4+96) = 158 字节；未知条目最多用掉剩下的
    const std::size_t knownMax = 158u;
    const std::size_t budget = rinne::state::kMaxChunkBytes - knownMax;
    const Bytes atBudget = kv(900, Bytes(budget - 4u, 0x33u));
    const Configs configs = decodeOk(atBudget);
    // 塞满最长的值再写回，正好顶到单块上限
    Configs full = configs;
    full.uiLang = "abcdefgh";
    full.presetName = std::string(96, 'n');
    CHECK(encodeOk(full).size() == rinne::state::kMaxChunkBytes);

    requireRejected(kv(900, Bytes(budget - 3u, 0x33u)));

    // Output 的已知键少，预算就大：(4+1)×3 + 6 + (4+8) = 33
    const std::size_t outputBudget = rinne::state::kMaxChunkBytes - 33u;
    decodeOk(kv(900, Bytes(outputBudget - 4u, 0x33u)), Role::Output);
    requireRejected(kv(900, Bytes(outputBudget - 3u, 0x33u)), Role::Output);
}
