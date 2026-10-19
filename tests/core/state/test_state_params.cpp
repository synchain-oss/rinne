// SPDX-License-Identifier: GPL-3.0-or-later
//
// PRMS 载荷层：{id, f32} 列表的编解码、未知 id 原样保留、id 的字符集与长度、条数上限、重复 id，以及失败时不碰
// 出参。值域与「缺失补默认」不在这里：那是参数层的事，本层原样搬运 f32 的位型（含 NaN 载荷与 -0.0）。

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "StateTestKit.h"
#include "state/StateCodec.h"

namespace
{

using rinne::state::Params;
using rinne::state::ParamValue;
using rinne::test::bitsFloat;
using rinne::test::byteList;
using rinne::test::Bytes;
using rinne::test::floatBits;
using rinne::test::rawParams;
using rinne::test::sameParams;

Params sentinelParams()
{
    Params params;
    params.entries.push_back(ParamValue{"sentinel", 0.25f});
    return params;
}

void requireRejected(const Bytes& bytes)
{
    Params out = sentinelParams();
    CHECK_FALSE(rinne::state::decodeParams(bytes.data(), bytes.size(), out));
    CHECK(sameParams(out, sentinelParams()));
}

void requireEncodeFails(const Params& params)
{
    Bytes out = byteList({0xff});
    CHECK_FALSE(rinne::state::encodeParams(params, out));
    CHECK(out == byteList({0xff}));
}

Params manyParams(std::size_t count)
{
    Params params;
    for (std::size_t i = 0; i < count; ++i)
        params.entries.push_back(ParamValue{"p" + std::to_string(i), static_cast<float>(i)});
    return params;
}

} // namespace

TEST_CASE("params: 载荷逐字节固定", "[state][params]")
{
    Params params;
    params.entries.push_back(ParamValue{"mix", 0.5f});
    Bytes encoded;
    REQUIRE(rinne::state::encodeParams(params, encoded));
    // u32 count = 1 | u8 idLen = 3 | "mix" | f32 0.5 = 0x3F000000 小端
    CHECK(encoded == byteList({1, 0, 0, 0, 3, 'm', 'i', 'x', 0x00, 0x00, 0x00, 0x3f}));

    Params decoded;
    REQUIRE(rinne::state::decodeParams(encoded.data(), encoded.size(), decoded));
    CHECK(sameParams(decoded, params));
}

TEST_CASE("params: 往返一致，未知 id 原样保留、顺序不变", "[state][params]")
{
    Params params;
    params.entries = {{"loop_on", 1.0f}, {"from_a_newer_version", -0.5f}, {"feedback", 0.0f}};
    Bytes encoded;
    REQUIRE(rinne::state::encodeParams(params, encoded));
    Params decoded;
    REQUIRE(rinne::state::decodeParams(encoded.data(), encoded.size(), decoded));
    CHECK(sameParams(decoded, params));

    // 参数层保存时在解出来的列表上改值：认识的改掉，不认识的原样留着
    decoded.set("feedback", 40.0f);
    decoded.set("mix", 25.0f);
    REQUIRE(decoded.entries.size() == 4u);
    CHECK(decoded.entries[1].id == "from_a_newer_version");
    CHECK(decoded.find("feedback")->value == 40.0f);
    CHECK(decoded.entries[3].id == "mix");
    CHECK(decoded.find("absent") == nullptr);
    Bytes again;
    REQUIRE(rinne::state::encodeParams(decoded, again));
    Params reread;
    REQUIRE(rinne::state::decodeParams(again.data(), again.size(), reread));
    CHECK(sameParams(reread, decoded));
}

TEST_CASE("params: 空列表与最长的 id", "[state][params]")
{
    Bytes encoded;
    REQUIRE(rinne::state::encodeParams(Params{}, encoded));
    CHECK(encoded == byteList({0, 0, 0, 0}));
    Params decoded = sentinelParams();
    REQUIRE(rinne::state::decodeParams(encoded.data(), encoded.size(), decoded));
    CHECK(decoded.entries.empty());

    Params longest;
    longest.entries.push_back(ParamValue{std::string(rinne::state::kMaxParamIdBytes, 'p'), 1.0f});
    REQUIRE(rinne::state::encodeParams(longest, encoded));
    CHECK(encoded.size() == 4u + 1u + rinne::state::kMaxParamIdBytes + 4u);
    REQUIRE(rinne::state::decodeParams(encoded.data(), encoded.size(), decoded));
    CHECK(sameParams(decoded, longest));
}

TEST_CASE("params: f32 按位存取", "[state][params]")
{
    Params params;
    params.entries = {{"nan_payload", bitsFloat(0x7fc00001u)},
                      {"negative_zero", bitsFloat(0x80000000u)},
                      {"infinity", bitsFloat(0x7f800000u)},
                      {"subnormal", bitsFloat(0x00000001u)}};
    Bytes encoded;
    REQUIRE(rinne::state::encodeParams(params, encoded));
    Params decoded;
    REQUIRE(rinne::state::decodeParams(encoded.data(), encoded.size(), decoded));
    REQUIRE(decoded.entries.size() == 4u);
    CHECK(floatBits(decoded.entries[0].value) == 0x7fc00001u);
    CHECK(floatBits(decoded.entries[1].value) == 0x80000000u);
    CHECK(floatBits(decoded.entries[2].value) == 0x7f800000u);
    CHECK(floatBits(decoded.entries[3].value) == 0x00000001u);
}

TEST_CASE("params: 条数上限 256", "[state][params]")
{
    Bytes encoded;
    REQUIRE(rinne::state::encodeParams(manyParams(rinne::state::kMaxParamCount), encoded));
    Params decoded;
    REQUIRE(rinne::state::decodeParams(encoded.data(), encoded.size(), decoded));
    CHECK(decoded.entries.size() == rinne::state::kMaxParamCount);

    // 257 条都真的在：只有上限这一条拦它
    std::vector<rinne::test::RawParam> raw;
    for (std::uint32_t i = 0; i <= rinne::state::kMaxParamCount; ++i)
        raw.push_back({"p" + std::to_string(i), 0u});
    requireRejected(rawParams(raw));
    requireEncodeFails(manyParams(rinne::state::kMaxParamCount + 1u));
}

TEST_CASE("params: id 的长度与字符集", "[state][params]")
{
    SECTION("解码端")
    {
        requireRejected(rawParams({{"", 0u}})); // idLen = 0
        for (const int bad : {0x00, 0x20, 0x7f, 0x80, 0xff})
        {
            const std::string id = std::string("a") + static_cast<char>(bad);
            requireRejected(rawParams({{id, 0u}}));
        }
        // 边界上的字符照收
        Params decoded;
        const Bytes edges = rawParams({{"!~", 0u}});
        CHECK(rinne::state::decodeParams(edges.data(), edges.size(), decoded));
    }

    SECTION("编码端")
    {
        for (const std::string& bad :
             {std::string{}, std::string(rinne::state::kMaxParamIdBytes + 1u, 'p'), std::string("has space"),
              std::string("has\ttab"), std::string("del\x7f"), std::string("high") + static_cast<char>(0x80)})
        {
            Params params;
            params.entries.push_back(ParamValue{bad, 0.0f});
            requireEncodeFails(params);
            CHECK_FALSE(rinne::state::isValidParamId(bad));
        }
        CHECK(rinne::state::isValidParamId("delay_note"));
    }
}

TEST_CASE("params: 重复 id 整份拒收", "[state][params]")
{
    requireRejected(rawParams({{"feedback", 0u}, {"mix", 0u}, {"feedback", floatBits(1.0f)}}));

    Params params;
    params.entries = {{"feedback", 0.0f}, {"mix", 0.0f}, {"feedback", 1.0f}};
    requireEncodeFails(params);
}

TEST_CASE("params: 截断、尾部多余字节与空指针", "[state][params]")
{
    Params params;
    params.entries = {{"feedback", 25.0f}, {"delay_ms", 100.0f}};
    Bytes encoded;
    REQUIRE(rinne::state::encodeParams(params, encoded));

    for (std::size_t size = 0; size < encoded.size(); ++size)
        requireRejected(Bytes(encoded.begin(), encoded.begin() + static_cast<std::ptrdiff_t>(size)));

    for (int extra = 1; extra <= 4; ++extra)
    {
        Bytes trailing = encoded;
        trailing.insert(trailing.end(), static_cast<std::size_t>(extra), 0u);
        requireRejected(trailing);
    }

    // 声明的条数多于实际
    requireRejected(rawParams({{"feedback", 0u}}, 2));

    Params out = sentinelParams();
    CHECK_FALSE(rinne::state::decodeParams(nullptr, 0u, out));
    CHECK_FALSE(rinne::state::decodeParams(nullptr, encoded.size(), out));
    CHECK(sameParams(out, sentinelParams()));
}
