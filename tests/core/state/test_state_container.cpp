// SPDX-License-Identifier: GPL-3.0-or-later
//
// 容器层：16 字节头、块的边界与 4 字节补齐、块数与单块长度的上限、未知 fourcc 原样保留，以及失败时不碰出参。
// 坏字节用例都只违反一条规则，其余部分合法：删掉对应的那一条检查，用例就会变红。

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "StateTestKit.h"
#include "state/StateCodec.h"

namespace
{

using rinne::state::Chunk;
using rinne::state::Container;
using rinne::state::Header;
using rinne::state::kFourCcCfgs;
using rinne::state::kFourCcPrms;
using rinne::state::makeFourCc;
using rinne::state::Role;
using rinne::test::byteList;
using rinne::test::Bytes;
using rinne::test::rawContainer;

constexpr std::uint32_t kInput = 1u;
constexpr std::uint32_t kOutput = 2u;

Container sentinelContainer()
{
    Container container;
    container.role = Role::Output;
    container.chunks.push_back(Chunk{makeFourCc('S', 'E', 'N', 'T'), byteList({0x2a})});
    return container;
}

bool sameContainer(const Container& left, const Container& right)
{
    if (left.role != right.role || left.chunks.size() != right.chunks.size())
        return false;
    for (std::size_t i = 0; i < left.chunks.size(); ++i)
    {
        if (left.chunks[i].fourCc != right.chunks[i].fourCc || left.chunks[i].payload != right.chunks[i].payload)
            return false;
    }
    return true;
}

// 这串字节必须被拒收，且出参保持原样。
void requireRejected(const Bytes& bytes, Role role)
{
    Container out = sentinelContainer();
    CHECK_FALSE(rinne::state::decodeContainer(bytes.data(), bytes.size(), role, out));
    CHECK(sameContainer(out, sentinelContainer()));
}

std::vector<rinne::test::RawChunk> emptyChunks(std::size_t count)
{
    std::vector<rinne::test::RawChunk> chunks;
    for (std::size_t i = 0; i < count; ++i)
        chunks.push_back(
            {makeFourCc('E', 'M', static_cast<char>('A' + i / 26u), static_cast<char>('A' + i % 26u)), {}});
    return chunks;
}

} // namespace

TEST_CASE("container: 头与块逐字节固定", "[state][container]")
{
    // Output、一个 CFGS 块、载荷 5 字节（补 3 个 0）
    const Bytes bytes = byteList({'R', 'N', 'S', 'T', 1, 0, 0, 0, // magic、abi = 1
                                  2,   0,   0,   0,   1, 0, 0, 0, // role = Output、chunkCount = 1
                                  'C', 'F', 'G', 'S', 5, 0, 0, 0, // fourcc、sizeBytes
                                  1,   0,   1,   0,   7, 0, 0, 0}); // 载荷与补齐
    Header header;
    REQUIRE(rinne::state::parseHeader(bytes.data(), bytes.size(), header));
    CHECK(header.magic == rinne::state::kMagic);
    CHECK(header.abi == 1u);
    CHECK(header.role == kOutput);
    CHECK(header.chunkCount == 1u);

    Container container;
    REQUIRE(rinne::state::decodeContainer(bytes.data(), bytes.size(), Role::Output, container));
    CHECK(container.role == Role::Output);
    REQUIRE(container.chunks.size() == 1u);
    CHECK(container.chunks[0].fourCc == kFourCcCfgs);
    CHECK(container.chunks[0].payload == byteList({1, 0, 1, 0, 7}));

    Bytes encoded;
    REQUIRE(rinne::state::encodeContainer(container, encoded));
    CHECK(encoded == bytes);

    // 纯头（0 块）是合法的空容器；头短一个字节就不是头
    Container empty;
    empty.role = Role::Input;
    REQUIRE(rinne::state::encodeContainer(empty, encoded));
    CHECK(encoded == rawContainer(1, kInput, {}));
    Container decodedEmpty = sentinelContainer();
    REQUIRE(rinne::state::decodeContainer(encoded.data(), encoded.size(), Role::Input, decodedEmpty));
    CHECK(decodedEmpty.role == Role::Input);
    CHECK(decodedEmpty.chunks.empty());

    Header untouched;
    untouched.magic = 0x12345678u;
    CHECK_FALSE(rinne::state::parseHeader(bytes.data(), 15u, untouched));
    CHECK(untouched.magic == 0x12345678u);
}

TEST_CASE("container: 往返一致，未知 fourcc 原样保留", "[state][container]")
{
    Container container;
    container.role = Role::Input;
    container.chunks.push_back(Chunk{makeFourCc('F', 'U', 'T', 'R'), byteList({0xde, 0xad, 0xbe})});
    container.chunks.push_back(Chunk{kFourCcCfgs, byteList({1, 0, 1, 0, 2})});
    container.chunks.push_back(Chunk{kFourCcPrms, {}});

    Bytes encoded;
    REQUIRE(rinne::state::encodeContainer(container, encoded));
    CHECK(encoded.size() == 16u + (8u + 3u + 1u) + (8u + 5u + 3u) + 8u);

    Container decoded;
    REQUIRE(rinne::state::decodeContainer(encoded.data(), encoded.size(), Role::Input, decoded));
    CHECK(sameContainer(decoded, container));

    Bytes again;
    REQUIRE(rinne::state::encodeContainer(decoded, again));
    CHECK(again == encoded);
}

TEST_CASE("container: 4 字节补齐", "[state][container]")
{
    for (std::size_t size = 0; size <= 5u; ++size)
    {
        Container container;
        container.role = Role::Input;
        container.chunks.push_back(Chunk{kFourCcPrms, Bytes(size, 0x5au)});
        Bytes encoded;
        REQUIRE(rinne::state::encodeContainer(container, encoded));
        CHECK(encoded == rawContainer(1, kInput, {{kFourCcPrms, Bytes(size, 0x5au)}}));

        Container decoded;
        REQUIRE(rinne::state::decodeContainer(encoded.data(), encoded.size(), Role::Input, decoded));
        CHECK(sameContainer(decoded, container));

        // 补齐不在：少一个字节都拒收（载荷正好对齐时没有补齐，这一格跳过）
        if (size % 4u != 0u)
            requireRejected(Bytes(encoded.begin(), encoded.end() - 1), Role::Input);
    }

    // 补齐字节的内容不看
    Bytes nonZeroPadding = rawContainer(1, kInput, {{kFourCcPrms, byteList({9})}});
    nonZeroPadding.back() = 0xff;
    Container decoded;
    REQUIRE(rinne::state::decodeContainer(nonZeroPadding.data(), nonZeroPadding.size(), Role::Input, decoded));
    CHECK(decoded.chunks[0].payload == byteList({9}));

    // 第二块的头紧跟在第一块的补齐之后：漏读补齐会把补齐当成下一块的 fourcc
    const Bytes two = rawContainer(1, kInput, {{kFourCcPrms, byteList({1})}, {kFourCcCfgs, byteList({2, 3})}});
    REQUIRE(rinne::state::decodeContainer(two.data(), two.size(), Role::Input, decoded));
    REQUIRE(decoded.chunks.size() == 2u);
    CHECK(decoded.chunks[1].fourCc == kFourCcCfgs);
    CHECK(decoded.chunks[1].payload == byteList({2, 3}));
}

TEST_CASE("container: 单块长度上限 64 KiB", "[state][container]")
{
    const Bytes atLimit = rawContainer(1, kInput, {{kFourCcPrms, Bytes(rinne::state::kMaxChunkBytes, 0x11u)}});
    Container decoded;
    REQUIRE(rinne::state::decodeContainer(atLimit.data(), atLimit.size(), Role::Input, decoded));
    Bytes encoded;
    REQUIRE(rinne::state::encodeContainer(decoded, encoded));
    CHECK(encoded == atLimit);

    // 多一个字节：数据真的都在，只有上限这一条拦它
    requireRejected(rawContainer(1, kInput, {{kFourCcPrms, Bytes(rinne::state::kMaxChunkBytes + 1u, 0x11u)}}),
                    Role::Input);

    decoded.chunks[0].payload.push_back(0x22);
    Bytes tooBig = byteList({0xff});
    CHECK_FALSE(rinne::state::encodeContainer(decoded, tooBig));
    CHECK(tooBig == byteList({0xff}));
}

TEST_CASE("container: 块数上限 64", "[state][container]")
{
    const Bytes atLimit = rawContainer(1, kInput, emptyChunks(rinne::state::kMaxChunkCount));
    Container decoded;
    REQUIRE(rinne::state::decodeContainer(atLimit.data(), atLimit.size(), Role::Input, decoded));
    CHECK(decoded.chunks.size() == rinne::state::kMaxChunkCount);

    // 65 块都真的在：只有上限这一条拦它
    requireRejected(rawContainer(1, kInput, emptyChunks(rinne::state::kMaxChunkCount + 1u)), Role::Input);

    decoded.chunks.push_back(Chunk{makeFourCc('M', 'O', 'R', 'E'), {}});
    Bytes tooMany = byteList({0xff});
    CHECK_FALSE(rinne::state::encodeContainer(decoded, tooMany));
    CHECK(tooMany == byteList({0xff}));
}

TEST_CASE("container: 坏字节一律拒收，且不碰出参", "[state][container]")
{
    const Bytes good = rawContainer(1, kOutput, {{kFourCcCfgs, byteList({1, 0, 1, 0, 7})}});

    SECTION("magic 不符")
    {
        Bytes bytes = good;
        bytes[3] = 'X';
        requireRejected(bytes, Role::Output);
    }

    SECTION("role 不是期望的那一端")
    {
        requireRejected(good, Role::Input);
        // 头里写的、调用方期望的都是同一个不认识的值：也不认
        requireRejected(rawContainer(1, 9, {}), static_cast<Role>(9));
    }

    SECTION("块头截断")
    {
        Bytes bytes = rawContainer(1, kOutput, {});
        bytes[12] = 1; // 声明 1 块
        rinne::test::putU32(bytes, kFourCcCfgs); // 只有 fourcc，没有 sizeBytes
        requireRejected(bytes, Role::Output);
    }

    SECTION("声明的长度超出数据")
    {
        Bytes bytes = good;
        bytes[20] = 9; // sizeBytes = 9，实际只有 8 字节（载荷 5 + 补齐 3）
        requireRejected(bytes, Role::Output);
    }

    SECTION("声明的块数多于实际")
    {
        requireRejected(rawContainer(1, kOutput, {{kFourCcCfgs, byteList({1, 0, 1, 0, 7})}}, 2), Role::Output);
    }

    SECTION("最后一块之后还有字节")
    {
        for (int extra = 1; extra <= 4; ++extra)
        {
            Bytes bytes = good;
            bytes.insert(bytes.end(), static_cast<std::size_t>(extra), 0u);
            requireRejected(bytes, Role::Output);
        }
    }

    SECTION("空指针与过短")
    {
        requireRejected({}, Role::Output);
        requireRejected(Bytes(good.begin(), good.begin() + 15), Role::Output);
        Container out = sentinelContainer();
        CHECK_FALSE(rinne::state::decodeContainer(nullptr, 0u, Role::Output, out));
        CHECK_FALSE(rinne::state::decodeContainer(nullptr, good.size(), Role::Output, out));
        CHECK(sameContainer(out, sentinelContainer()));
        Header header;
        CHECK_FALSE(rinne::state::parseHeader(nullptr, 16u, header));
    }
}

TEST_CASE("container: 编码拒绝不认识的 role", "[state][container]")
{
    Container container;
    container.role = static_cast<Role>(3);
    Bytes out = byteList({0xff});
    CHECK_FALSE(rinne::state::encodeContainer(container, out));
    CHECK(out == byteList({0xff}));
}

TEST_CASE("container: find / set / remove", "[state][container]")
{
    Container container;
    CHECK(container.find(kFourCcPrms) == nullptr);

    container.set(kFourCcPrms, byteList({1, 2, 3}));
    container.set(kFourCcCfgs, {});
    container.set(kFourCcPrms, byteList({4})); // 原位替换，不追加
    REQUIRE(container.chunks.size() == 2u);
    CHECK(container.chunks[0].fourCc == kFourCcPrms);
    CHECK(container.find(kFourCcPrms)->payload == byteList({4}));

    container.chunks.push_back(Chunk{kFourCcPrms, byteList({5})});
    container.remove(kFourCcPrms); // 删掉所有同名的块
    REQUIRE(container.chunks.size() == 1u);
    CHECK(container.chunks[0].fourCc == kFourCcCfgs);
    container.remove(kFourCcPrms);
    CHECK(container.chunks.size() == 1u);
}
