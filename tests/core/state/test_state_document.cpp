// SPDX-License-Identifier: GPL-3.0-or-later
//
// 整份 state：哪一端认识哪个块、未知块与对端专有的块原样保留、已知块缺失 / 重复 / 损坏、块数预算，以及失败时
// 不碰出参。

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "StateTestKit.h"
#include "state/StateCodec.h"
#include "state/StateDocument.h"

namespace
{

using rinne::state::Chunk;
using rinne::state::Container;
using rinne::state::kFourCcCfgs;
using rinne::state::kFourCcPrms;
using rinne::state::makeFourCc;
using rinne::state::Role;
using rinne::state::StateDocument;
using rinne::test::byteList;
using rinne::test::Bytes;
using rinne::test::sameDocument;
using rinne::test::sentinelDocument;

const Bytes kEmptyParams = byteList({0, 0, 0, 0});
const Bytes kGroupThree = byteList({1, 0, 1, 0, 3}); // CFGS：group = 3

Container containerOf(Role role, std::vector<Chunk> chunks)
{
    Container container;
    container.role = role;
    container.chunks = std::move(chunks);
    return container;
}

StateDocument decodeOk(const Container& container, Role role)
{
    StateDocument document = sentinelDocument(role);
    REQUIRE(rinne::state::decodeDocument(container, role, document));
    return document;
}

void requireRejected(const Container& container, Role role)
{
    StateDocument out = sentinelDocument(Role::Input);
    CHECK_FALSE(rinne::state::decodeDocument(container, role, out));
    CHECK(sameDocument(out, sentinelDocument(Role::Input)));
}

Container encodeToContainer(const StateDocument& document)
{
    Bytes bytes;
    REQUIRE(rinne::state::encodeDocument(document, bytes));
    Container container;
    REQUIRE(rinne::state::decodeContainer(bytes.data(), bytes.size(), document.role, container));
    return container;
}

std::vector<Chunk> unknownChunks(std::size_t count)
{
    std::vector<Chunk> chunks;
    for (std::size_t i = 0; i < count; ++i)
        chunks.push_back(Chunk{makeFourCc('U', 'N', static_cast<char>('A' + i / 26u), static_cast<char>('A' + i % 26u)),
                               byteList({static_cast<int>(i)})});
    return chunks;
}

} // namespace

TEST_CASE("document: 示例文档往返逐字节一致", "[state][document]")
{
    for (const StateDocument& document : {rinne::test::sampleInputDocument(), rinne::test::sampleOutputDocument()})
    {
        Bytes encoded;
        REQUIRE(rinne::state::encodeDocument(document, encoded));
        Container container;
        REQUIRE(rinne::state::decodeContainer(encoded.data(), encoded.size(), document.role, container));
        if (document.role == Role::Input)
        {
            REQUIRE(container.chunks.size() == 3u);
            CHECK(container.chunks[0].fourCc == kFourCcPrms);
            CHECK(container.chunks[1].fourCc == kFourCcCfgs);
            CHECK(container.chunks[2].fourCc == makeFourCc('X', 'T', 'R', 'A'));
        }
        else
        {
            REQUIRE(container.chunks.size() == 1u);
            CHECK(container.chunks[0].fourCc == kFourCcCfgs);
        }

        const StateDocument decoded = decodeOk(container, document.role);
        CHECK(sameDocument(decoded, document));
        Bytes again;
        REQUIRE(rinne::state::encodeDocument(decoded, again));
        CHECK(again == encoded);
    }
}

TEST_CASE("document: 新实例的起点", "[state][document]")
{
    const Container input = encodeToContainer(rinne::state::makeDefaultDocument(Role::Input));
    REQUIRE(input.chunks.size() == 2u);
    CHECK(input.chunks[0].fourCc == kFourCcPrms);
    CHECK(input.chunks[0].payload == kEmptyParams);
    CHECK(input.chunks[1].fourCc == kFourCcCfgs);
    CHECK(sameDocument(decodeOk(input, Role::Input), rinne::state::makeDefaultDocument(Role::Input)));

    const Container output = encodeToContainer(rinne::state::makeDefaultDocument(Role::Output));
    REQUIRE(output.chunks.size() == 1u);
    CHECK(output.chunks[0].fourCc == kFourCcCfgs);
    CHECK(sameDocument(decodeOk(output, Role::Output), rinne::state::makeDefaultDocument(Role::Output)));
}

TEST_CASE("document: role 不符整份失败", "[state][document]")
{
    const Container input = containerOf(Role::Input, {Chunk{kFourCcCfgs, kGroupThree}});
    requireRejected(input, Role::Output);
    requireRejected(containerOf(static_cast<Role>(9), {}), static_cast<Role>(9));
}

TEST_CASE("document: 已知块缺失取默认，写回时补上", "[state][document]")
{
    const StateDocument noParams = decodeOk(containerOf(Role::Input, {Chunk{kFourCcCfgs, kGroupThree}}), Role::Input);
    CHECK(noParams.params.entries.empty());
    CHECK(noParams.configs.group == 3u);
    CHECK(noParams.unknownChunks.empty());

    const StateDocument noConfigs = decodeOk(containerOf(Role::Input, {Chunk{kFourCcPrms, kEmptyParams}}), Role::Input);
    CHECK(rinne::test::sameConfigs(noConfigs.configs, rinne::state::Configs{}));

    const StateDocument nothing = decodeOk(containerOf(Role::Output, {}), Role::Output);
    CHECK(sameDocument(nothing, rinne::state::makeDefaultDocument(Role::Output)));
    CHECK(encodeToContainer(nothing).chunks.size() == 1u);
}

TEST_CASE("document: 已知块重复或解不开整份失败", "[state][document]")
{
    requireRejected(containerOf(Role::Input, {Chunk{kFourCcCfgs, kGroupThree}, Chunk{kFourCcCfgs, kGroupThree}}),
                    Role::Input);
    requireRejected(containerOf(Role::Input, {Chunk{kFourCcPrms, kEmptyParams}, Chunk{kFourCcPrms, kEmptyParams}}),
                    Role::Input);
    requireRejected(containerOf(Role::Output, {Chunk{kFourCcCfgs, kGroupThree}, Chunk{kFourCcCfgs, kGroupThree}}),
                    Role::Output);

    // 载荷坏了：PRMS 声明 1 条却没有条目；CFGS 的 u16 键只给 1 字节
    requireRejected(containerOf(Role::Input, {Chunk{kFourCcPrms, byteList({1, 0, 0, 0})}}), Role::Input);
    requireRejected(containerOf(Role::Input, {Chunk{kFourCcCfgs, byteList({8, 0, 1, 0, 100})}}), Role::Input);
    requireRejected(containerOf(Role::Output, {Chunk{kFourCcCfgs, byteList({8, 0, 1, 0, 100})}}), Role::Output);
}

TEST_CASE("document: 未知块与对端专有的块原样保留", "[state][document]")
{
    // 未知块排在最前也保得住；写回时已知块按固定顺序在前，未知块彼此顺序不变
    const Chunk future{makeFourCc('F', 'U', 'T', 'R'), byteList({0xde, 0xad, 0xbe})};
    const Chunk other{makeFourCc('O', 'T', 'H', 'R'), {}};
    const StateDocument decoded = decodeOk(
        containerOf(Role::Input, {future, Chunk{kFourCcCfgs, kGroupThree}, other, Chunk{kFourCcPrms, kEmptyParams}}),
        Role::Input);
    REQUIRE(decoded.unknownChunks.size() == 2u);
    const Container again = encodeToContainer(decoded);
    REQUIRE(again.chunks.size() == 4u);
    CHECK(again.chunks[0].fourCc == kFourCcPrms);
    CHECK(again.chunks[1].fourCc == kFourCcCfgs);
    CHECK(again.chunks[2].fourCc == future.fourCc);
    CHECK(again.chunks[2].payload == future.payload);
    CHECK(again.chunks[3].fourCc == other.fourCc);

    // Output 收到 PRMS：不解读（载荷是什么都行），原样带着
    const Chunk peerParams{kFourCcPrms, byteList({0xff, 0xff})};
    const StateDocument output =
        decodeOk(containerOf(Role::Output, {peerParams, Chunk{kFourCcCfgs, kGroupThree}}), Role::Output);
    CHECK(output.params.entries.empty());
    REQUIRE(output.unknownChunks.size() == 1u);
    const Container outputAgain = encodeToContainer(output);
    REQUIRE(outputAgain.chunks.size() == 2u);
    CHECK(outputAgain.chunks[0].fourCc == kFourCcCfgs);
    CHECK(outputAgain.chunks[1].fourCc == kFourCcPrms);
    CHECK(outputAgain.chunks[1].payload == peerParams.payload);
}

TEST_CASE("document: 块数预算保证写得回去", "[state][document]")
{
    // Input 写回时一定有 PRMS 与 CFGS 两块：未知块最多 62 个
    std::vector<Chunk> chunks = unknownChunks(62);
    chunks.push_back(Chunk{kFourCcCfgs, kGroupThree});
    const StateDocument atBudget = decodeOk(containerOf(Role::Input, chunks), Role::Input);
    CHECK(encodeToContainer(atBudget).chunks.size() == rinne::state::kMaxChunkCount);

    chunks = unknownChunks(63);
    chunks.push_back(Chunk{kFourCcCfgs, kGroupThree}); // 容器里正好 64 块，写回会变成 65 块
    requireRejected(containerOf(Role::Input, chunks), Role::Input);

    // Output 只有 CFGS 一块：未知块最多 63 个
    const StateDocument output = decodeOk(containerOf(Role::Output, unknownChunks(63)), Role::Output);
    CHECK(encodeToContainer(output).chunks.size() == rinne::state::kMaxChunkCount);
    requireRejected(containerOf(Role::Output, unknownChunks(64)), Role::Output);
}

TEST_CASE("document: 编码拒绝会写出两份已知块或不认识的 role", "[state][document]")
{
    StateDocument document = rinne::state::makeDefaultDocument(Role::Input);
    document.unknownChunks.push_back(Chunk{kFourCcCfgs, kGroupThree});
    Bytes out = byteList({0xff});
    CHECK_FALSE(rinne::state::encodeDocument(document, out));
    document.unknownChunks.back().fourCc = kFourCcPrms;
    CHECK_FALSE(rinne::state::encodeDocument(document, out));
    CHECK_FALSE(rinne::state::encodeDocument(rinne::state::makeDefaultDocument(static_cast<Role>(9)), out));
    CHECK(out == byteList({0xff}));

    // Output 这边 PRMS 是未知块，可以带着
    StateDocument output = rinne::state::makeDefaultDocument(Role::Output);
    output.unknownChunks.push_back(Chunk{kFourCcPrms, {}});
    CHECK(rinne::state::encodeDocument(output, out));
}
