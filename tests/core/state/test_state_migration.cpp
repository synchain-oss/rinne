// SPDX-License-Identifier: GPL-3.0-or-later
//
// 加载入口：abi 的分流（零字节、坏头、高 abi 拒载并交回原字节、探针 abi 0 只认头、本构建的 abi）、逐级迁移的
// 骨架，以及失败与忽略时不碰出参。「原样回写」按字节判：拒载时交回的必须与收到的完全一样，处理器会把它们原样
// 存回工程。

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "StateTestKit.h"
#include "state/StateCodec.h"
#include "state/StateMigration.h"

namespace
{

using rinne::state::Chunk;
using rinne::state::Container;
using rinne::state::makeFourCc;
using rinne::state::MigrateFn;
using rinne::state::Role;
using rinne::state::StateDocument;
using rinne::state::StateLoadResult;
using rinne::state::StateLoadStatus;
using rinne::test::byteList;
using rinne::test::Bytes;
using rinne::test::sameDocument;
using rinne::test::sentinelDocument;

Bytes encodeOf(const StateDocument& document)
{
    Bytes bytes;
    REQUIRE(rinne::state::encodeDocument(document, bytes));
    return bytes;
}

Bytes withAbi(Bytes bytes, std::uint32_t abi)
{
    for (int i = 0; i < 4; ++i)
        bytes[4 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((abi >> (8 * i)) & 0xffu);
    return bytes;
}

// 加载这串字节，要求得到 expected、出参不变、只有拒载时才交回字节（且与输入相同）。
StateLoadResult requireUntouched(const Bytes& bytes, Role role, StateLoadStatus expected)
{
    StateDocument out = sentinelDocument(role);
    const StateLoadResult result = rinne::state::loadState(bytes.data(), bytes.size(), role, out);
    CHECK(result.status == expected);
    CHECK(sameDocument(out, sentinelDocument(role)));
    if (expected == StateLoadStatus::RejectedNewer)
        CHECK(result.preservedOriginal == bytes);
    else
        CHECK(result.preservedOriginal.empty());
    return result;
}

// 假的迁移链：每一级在容器末尾记一个块，块名写着它是第几级，用来核对顺序与次数。
template<int Step>
bool recordStep(Container& container)
{
    container.chunks.push_back(Chunk{makeFourCc('S', 'T', 'P', static_cast<char>('0' + Step)), {}});
    return true;
}

bool failStep(Container& container)
{
    container.chunks.push_back(Chunk{makeFourCc('F', 'A', 'I', 'L'), {}});
    return false;
}

std::string stepsOf(const std::vector<Chunk>& chunks)
{
    std::string steps;
    for (const auto& chunk : chunks)
        steps.push_back(static_cast<char>((chunk.fourCc >> 24) & 0xffu));
    return steps;
}

std::string stepsOf(const Container& container)
{
    return stepsOf(container.chunks);
}

// 假的一级迁移：把旧版的 ui_scale（1 字节）改写成本构建的 2 字节。改写之前这份载荷过不了本构建的解码。
bool widenUiScale(Container& container)
{
    container.set(rinne::state::kFourCcCfgs, rinne::test::kv(8, byteList({120, 0})));
    return true;
}

// 迁移出一份本构建解不开的容器：两个 CFGS 块。
bool duplicateConfigs(Container& container)
{
    container.chunks.push_back(Chunk{rinne::state::kFourCcCfgs, {}});
    return true;
}

// 假装本构建是 currentAbi、迁移链是 chain 来加载。
template<std::size_t N>
StateLoadResult loadAs(const Bytes& bytes, Role role, StateDocument& out, std::uint32_t currentAbi,
                       const std::array<MigrateFn, N>& chain)
{
    return rinne::state::testing::loadStateWith(bytes.data(), bytes.size(), role, out, currentAbi, chain.data(),
                                                chain.size());
}

} // namespace

TEST_CASE("load: 本构建的 abi 直接解出文档", "[state][migration]")
{
    for (const StateDocument& document : {rinne::test::sampleInputDocument(), rinne::test::sampleOutputDocument()})
    {
        const Bytes bytes = encodeOf(document);
        StateDocument decoded = sentinelDocument(document.role);
        const StateLoadResult result = rinne::state::loadState(bytes.data(), bytes.size(), document.role, decoded);
        CHECK(result.status == StateLoadStatus::Ok);
        CHECK(result.sourceAbi == rinne::state::kAbi);
        CHECK(result.preservedOriginal.empty());
        CHECK(sameDocument(decoded, document));
    }
}

TEST_CASE("load: 零字节什么都不改，也不算坏字节", "[state][migration]")
{
    requireUntouched({}, Role::Input, StateLoadStatus::Empty);
    const Bytes some = encodeOf(rinne::test::sampleInputDocument());
    StateDocument out = sentinelDocument(Role::Input);
    CHECK(rinne::state::loadState(some.data(), 0u, Role::Input, out).status == StateLoadStatus::Empty);
    CHECK(sameDocument(out, sentinelDocument(Role::Input)));
}

TEST_CASE("load: 坏字节什么都不改，不交回字节", "[state][migration]")
{
    const Bytes good = encodeOf(rinne::test::sampleOutputDocument());

    Bytes badMagic = good;
    badMagic[0] = 'X';
    requireUntouched(badMagic, Role::Output, StateLoadStatus::Corrupt);
    // magic 先于 abi：不是这个格式的字节，abi 字段写的什么都不算数，既不当新版数据交回，也不当探针
    requireUntouched(withAbi(badMagic, 9u), Role::Output, StateLoadStatus::Corrupt);
    requireUntouched(withAbi(badMagic, 0u), Role::Output, StateLoadStatus::Corrupt);
    requireUntouched(Bytes(good.begin(), good.begin() + 15), Role::Output, StateLoadStatus::Corrupt);
    requireUntouched(good, Role::Input, StateLoadStatus::Corrupt); // role 不符
    requireUntouched(Bytes(good.begin(), good.end() - 1), Role::Output, StateLoadStatus::Corrupt);

    // 容器没毛病，载荷坏了
    const Bytes badPayload =
        rinne::test::rawContainer(1, 2, {{rinne::state::kFourCcCfgs, rinne::test::kv(8, byteList({100}))}});
    requireUntouched(badPayload, Role::Output, StateLoadStatus::Corrupt);

    StateDocument out = sentinelDocument(Role::Output);
    CHECK(rinne::state::loadState(nullptr, good.size(), Role::Output, out).status == StateLoadStatus::Corrupt);
    CHECK(sameDocument(out, sentinelDocument(Role::Output)));
}

TEST_CASE("load: 高 abi 拒载，原字节一个不改地交回", "[state][migration]")
{
    const Bytes current = encodeOf(rinne::test::sampleInputDocument());

    for (const std::uint32_t abi : {2u, 7u, 0xffffffffu})
    {
        INFO("abi " << abi);
        const StateLoadResult result =
            requireUntouched(withAbi(current, abi), Role::Input, StateLoadStatus::RejectedNewer);
        CHECK(result.sourceAbi == abi);
    }

    // 头之后是什么都不看：块数超限、后面是垃圾，一样原样交回，宁可留着也不替用户丢掉新版数据
    Bytes garbage = rinne::test::rawContainer(9, 2, {});
    garbage[12] = 0xff;
    garbage.resize(64u, 0x5au);
    requireUntouched(garbage, Role::Output, StateLoadStatus::RejectedNewer);
    requireUntouched(Bytes(garbage.begin(), garbage.begin() + 16), Role::Output, StateLoadStatus::RejectedNewer);

    // 只有 role 照样比对（它的位置不随 abi 变）：对端的高版本字节判坏字节，不原样固化进本端的工程
    CHECK(requireUntouched(garbage, Role::Input, StateLoadStatus::Corrupt).sourceAbi == 9u);
    requireUntouched(withAbi(current, 2u), Role::Output, StateLoadStatus::Corrupt);
    // 头里写的、调用方期望的都是同一个不认识的 role：也不认
    requireUntouched(rinne::test::rawContainer(9, 9, {}), static_cast<Role>(9), StateLoadStatus::Corrupt);

    // 交回的字节下次再读，还是同一份高版本数据
    const Bytes newer = withAbi(current, 3u);
    StateDocument first = sentinelDocument(Role::Input);
    const StateLoadResult loaded = rinne::state::loadState(newer.data(), newer.size(), Role::Input, first);
    REQUIRE(loaded.status == StateLoadStatus::RejectedNewer);
    requireUntouched(loaded.preservedOriginal, Role::Input, StateLoadStatus::RejectedNewer);
}

TEST_CASE("load: 探针的 abi 0 只认头，内容一概不用", "[state][migration]")
{
    const Bytes current = encodeOf(rinne::test::sampleInputDocument());
    CHECK(requireUntouched(withAbi(current, 0u), Role::Input, StateLoadStatus::IgnoredProbe).sourceAbi == 0u);
    // 探针的布局与正式格式无关：头后面是什么、role 写的什么都不看
    Bytes probe = rinne::test::rawContainer(0, 1, {});
    probe.resize(40u, 0xeeu);
    requireUntouched(probe, Role::Output, StateLoadStatus::IgnoredProbe);
    requireUntouched(Bytes(probe.begin(), probe.begin() + 16), Role::Input, StateLoadStatus::IgnoredProbe);
}

TEST_CASE("load: 低 abi 先逐级迁移，再按本构建的规则解码", "[state][migration]")
{
    // 本构建还没有迁移函数，这里假装本构建是 abi 3，链是两级假迁移：每一级在容器末尾记一个块，
    // 这些块在 Output 上是未知块，解码后按顺序留在 unknownChunks 里
    const StateDocument document = rinne::test::sampleOutputDocument();
    const Bytes abi1 = encodeOf(document);
    const std::array<MigrateFn, 2> chain{&recordStep<1>, &recordStep<2>};

    StateDocument out = sentinelDocument(Role::Output);
    StateLoadResult result = loadAs(abi1, Role::Output, out, 3u, chain);
    CHECK(result.status == StateLoadStatus::Migrated);
    CHECK(result.sourceAbi == 1u);
    CHECK(result.preservedOriginal.empty());
    CHECK(stepsOf(out.unknownChunks) == "12");
    CHECK(rinne::test::sameConfigs(out.configs, document.configs));

    // abi 2 的字节只走第二级；本构建自己的 abi 一级都不走
    out = sentinelDocument(Role::Output);
    CHECK(loadAs(withAbi(abi1, 2u), Role::Output, out, 3u, chain).status == StateLoadStatus::Migrated);
    CHECK(stepsOf(out.unknownChunks) == "2");
    out = sentinelDocument(Role::Output);
    CHECK(loadAs(withAbi(abi1, 3u), Role::Output, out, 3u, chain).status == StateLoadStatus::Ok);
    CHECK(out.unknownChunks.empty());

    // 高于「本构建」的照样拒载并交回原字节
    out = sentinelDocument(Role::Output);
    result = loadAs(withAbi(abi1, 4u), Role::Output, out, 3u, chain);
    CHECK(result.status == StateLoadStatus::RejectedNewer);
    CHECK(result.preservedOriginal == withAbi(abi1, 4u));
    CHECK(sameDocument(out, sentinelDocument(Role::Output)));

    // 先迁移再解码：旧版的载荷按本构建的规则是坏的（ui_scale 只有 1 字节），迁移改写之后解得开
    const Bytes oldScale =
        rinne::test::rawContainer(1, 2, {{rinne::state::kFourCcCfgs, rinne::test::kv(8, byteList({120}))}});
    out = sentinelDocument(Role::Output);
    CHECK(loadAs(oldScale, Role::Output, out, 1u, std::array<MigrateFn, 0>{}).status == StateLoadStatus::Corrupt);
    CHECK(loadAs(oldScale, Role::Output, out, 2u, std::array<MigrateFn, 1>{&widenUiScale}).status ==
          StateLoadStatus::Migrated);
    CHECK(out.configs.uiScale == 120u);
}

TEST_CASE("load: 迁移失败或迁移后解不开，什么都不改", "[state][migration]")
{
    const Bytes abi1 = encodeOf(rinne::test::sampleOutputDocument());
    const auto requireCorrupt = [&](const auto& chain) {
        StateDocument out = sentinelDocument(Role::Output);
        const StateLoadResult result = loadAs(abi1, Role::Output, out, 3u, chain);
        CHECK(result.status == StateLoadStatus::Corrupt);
        CHECK(result.sourceAbi == 1u);
        CHECK(result.preservedOriginal.empty());
        CHECK(sameDocument(out, sentinelDocument(Role::Output)));
    };
    requireCorrupt(std::array<MigrateFn, 2>{&recordStep<1>, &failStep});
    requireCorrupt(std::array<MigrateFn, 2>{nullptr, &recordStep<2>});
    requireCorrupt(std::array<MigrateFn, 1>{&recordStep<1>}); // 链不够长
    requireCorrupt(std::array<MigrateFn, 2>{&duplicateConfigs, &recordStep<2>});
}

TEST_CASE("migration: 迁移链的骨架", "[state][migration]")
{
    // abi 1 是第一版：还没有任何迁移函数
    CHECK(rinne::state::kMigrators.size() + 1u == rinne::state::kAbi);
    Container container;
    CHECK(rinne::state::runMigrations(container, rinne::state::kAbi, rinne::state::kAbi,
                                      rinne::state::kMigrators.data(), rinne::state::kMigrators.size()));
    CHECK(container.chunks.empty());

    // 假链：chain[i] 把 abi i+1 升到 i+2
    const std::array<MigrateFn, 3> chain{&recordStep<1>, &recordStep<2>, &recordStep<3>};

    SECTION("从任一级出发都按顺序走到底，每级一次")
    {
        Container fromOne;
        REQUIRE(rinne::state::runMigrations(fromOne, 1, 4, chain.data(), chain.size()));
        CHECK(stepsOf(fromOne) == "123");
        Container fromThree;
        REQUIRE(rinne::state::runMigrations(fromThree, 3, 4, chain.data(), chain.size()));
        CHECK(stepsOf(fromThree) == "3");
        Container partial;
        REQUIRE(rinne::state::runMigrations(partial, 1, 3, chain.data(), chain.size()));
        CHECK(stepsOf(partial) == "12");
    }

    SECTION("一级失败就停，后面的不再执行")
    {
        const std::array<MigrateFn, 3> failing{&recordStep<1>, &failStep, &recordStep<3>};
        Container c;
        CHECK_FALSE(rinne::state::runMigrations(c, 1, 4, failing.data(), failing.size()));
        CHECK(stepsOf(c) == "1L");

        const std::array<MigrateFn, 3> withHole{&recordStep<1>, nullptr, &recordStep<3>};
        Container h;
        CHECK_FALSE(rinne::state::runMigrations(h, 1, 4, withHole.data(), withHole.size()));
        CHECK(stepsOf(h) == "1");
    }

    SECTION("链不够长、起点是探针或起点高于终点：一级都不执行")
    {
        Container c;
        CHECK_FALSE(rinne::state::runMigrations(c, 3, 5, chain.data(), chain.size())); // 要用到 chain[3]
        CHECK_FALSE(rinne::state::runMigrations(c, 3, 4, chain.data(), 2u)); // 要用到 chain[2]
        CHECK_FALSE(rinne::state::runMigrations(c, 0, 2, chain.data(), chain.size()));
        CHECK_FALSE(rinne::state::runMigrations(c, 3, 2, chain.data(), chain.size()));
        CHECK(c.chunks.empty());
    }
}
