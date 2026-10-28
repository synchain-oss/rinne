// SPDX-License-Identifier: GPL-3.0-or-later
//
// 两端 state 胶水共用的判定（src/core/state/StateGlueCommon.h）：加载结果的分流、定型、按应用方式并入实例的文档，
// 以及保存时交出什么（原样回写、先 set 已知参数再裁剪、编码失败交出空块）。胶水本身的接线用例在
// tests/params/test_state_glue*.cpp。

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "StateTestKit.h"
#include "state/StateCodec.h"
#include "state/StateDocument.h"
#include "state/StateGlueCommon.h"
#include "state/StateMigration.h"

namespace
{

using rinne::state::Role;
using rinne::state::StateDocument;
using rinne::state::StateLoadStatus;
using rinne::state::glue::ApplyMode;
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

// 头写更高 abi、role 是这一端的字节：loadState 判 RejectedNewer。块区随便填，判读先于块结构。
Bytes newerBytes(Role role, std::uint8_t tag)
{
    return rinne::test::rawContainer(rinne::state::kAbi + 1u, static_cast<std::uint32_t>(role),
                                     {{rinne::state::makeFourCc('N', 'E', 'W', 'R'), byteList({tag})}});
}

Bytes probeBytes(Role role)
{
    return rinne::test::rawContainer(rinne::state::kProbeAbi, static_cast<std::uint32_t>(role), {});
}

const Bytes kCorrupt = byteList({'n', 'o', 't', ' ', 'a', ' ', 's', 't', 'a', 't', 'e', '!', 0, 0, 0, 0});

// 11 个已知参数，id 取 k0..k10，值是序号
std::vector<std::string> knownIdStrings()
{
    std::vector<std::string> ids;
    for (int i = 0; i < 11; ++i)
        ids.push_back("k" + std::to_string(i));
    return ids;
}

std::vector<rinne::state::glue::KnownParam> knownOf(const std::vector<std::string>& ids)
{
    std::vector<rinne::state::glue::KnownParam> known;
    for (std::size_t i = 0; i < ids.size(); ++i)
        known.push_back({ids[i].c_str(), static_cast<float>(i)});
    return known;
}

StateDocument documentWithUnknownParams(std::size_t count, std::size_t idLength)
{
    StateDocument document = rinne::state::makeDefaultDocument(Role::Input);
    for (std::size_t i = 0; i < count; ++i)
        document.params.entries.push_back({rinne::test::paramIdOfLength(idLength, i), 0.5f});
    return document;
}

StateDocument loadedBack(const Bytes& bytes)
{
    StateDocument back = sentinelDocument(Role::Input);
    REQUIRE(rinne::state::loadState(bytes.data(), bytes.size(), Role::Input, back).status == StateLoadStatus::Ok);
    return back;
}

} // namespace

TEST_CASE("glue: 只有成功读入才应用，定型前全量、定型后只取参数", "[state][glue]")
{
    using rinne::state::glue::applyModeFor;
    for (const bool settled : {false, true})
    {
        INFO("settled = " << settled);
        const ApplyMode success = settled ? ApplyMode::ParamsOnly : ApplyMode::Full;
        CHECK(applyModeFor(StateLoadStatus::Ok, settled) == success);
        CHECK(applyModeFor(StateLoadStatus::Migrated, settled) == success);
        CHECK(applyModeFor(StateLoadStatus::Empty, settled) == ApplyMode::None);
        CHECK(applyModeFor(StateLoadStatus::IgnoredProbe, settled) == ApplyMode::None);
        CHECK(applyModeFor(StateLoadStatus::RejectedNewer, settled) == ApplyMode::None);
        CHECK(applyModeFor(StateLoadStatus::Corrupt, settled) == ApplyMode::None);
    }
}

TEST_CASE("glue: loadForApply 按结果更新写回保留，失败与忽略不碰读入的文档", "[state][glue]")
{
    using rinne::state::glue::loadForApply;
    for (const Role role : {Role::Input, Role::Output})
    {
        INFO("role = " << static_cast<int>(role));
        const StateDocument sample =
            role == Role::Input ? rinne::test::sampleInputDocument() : rinne::test::sampleOutputDocument();
        const Bytes good = encodeOf(sample);
        rinne::state::RawWriteBack writeBack;

        // 没在保留时：Empty、IgnoredProbe、Corrupt 什么都不改，也不开始保留
        const Bytes probe = probeBytes(role);
        const std::array<std::pair<const Bytes*, StateLoadStatus>, 3> nothing{{
            {nullptr, StateLoadStatus::Empty},
            {&probe, StateLoadStatus::IgnoredProbe},
            {&kCorrupt, StateLoadStatus::Corrupt},
        }};
        for (const auto& [bytes, status] : nothing)
        {
            StateDocument loaded = sentinelDocument(role);
            const auto step = loadForApply(bytes != nullptr ? bytes->data() : nullptr,
                                           bytes != nullptr ? bytes->size() : 0u, role, false, writeBack, loaded);
            CHECK(step.status == status);
            CHECK(step.mode == ApplyMode::None);
            CHECK(sameDocument(loaded, sentinelDocument(role)));
            CHECK_FALSE(writeBack.holding());
        }

        // 更高 abi：接管原字节，不应用
        const Bytes newer = newerBytes(role, 1);
        StateDocument loaded = sentinelDocument(role);
        auto step = loadForApply(newer.data(), newer.size(), role, false, writeBack, loaded);
        CHECK(step.status == StateLoadStatus::RejectedNewer);
        CHECK(step.mode == ApplyMode::None);
        CHECK(sameDocument(loaded, sentinelDocument(role)));
        REQUIRE(writeBack.holding());
        CHECK(writeBack.bytes() == newer);

        // 保留期间：Empty、IgnoredProbe、Corrupt 照样什么都不改，保留的仍是那份原字节
        for (const auto& [bytes, status] : nothing)
        {
            StateDocument untouched = sentinelDocument(role);
            step = loadForApply(bytes != nullptr ? bytes->data() : nullptr, bytes != nullptr ? bytes->size() : 0u, role,
                                true, writeBack, untouched);
            CHECK(step.status == status);
            CHECK(step.mode == ApplyMode::None);
            CHECK(writeBack.bytes() == newer);
        }

        // 再来一份更高 abi：以最后一份为准
        const Bytes newer2 = newerBytes(role, 2);
        step = loadForApply(newer2.data(), newer2.size(), role, false, writeBack, loaded);
        CHECK(step.mode == ApplyMode::None);
        CHECK(writeBack.bytes() == newer2);

        // 成功读入：清掉保留；应用方式按定型与否
        for (const bool settled : {false, true})
        {
            StateDocument fresh = sentinelDocument(role);
            step = loadForApply(good.data(), good.size(), role, settled, writeBack, fresh);
            CHECK(step.status == StateLoadStatus::Ok);
            CHECK(step.mode == (settled ? ApplyMode::ParamsOnly : ApplyMode::Full));
            CHECK(sameDocument(fresh, sample));
            CHECK_FALSE(writeBack.holding());
            REQUIRE(loadForApply(newer.data(), newer.size(), role, settled, writeBack, fresh).status ==
                    StateLoadStatus::RejectedNewer);
        }
    }
}

TEST_CASE("glue: mergeLoaded 全量换整份，只取参数时设置与未知块保持当前", "[state][glue]")
{
    using rinne::state::glue::mergeLoaded;
    const StateDocument loaded = rinne::test::sampleInputDocument();

    StateDocument full = sentinelDocument(Role::Input);
    mergeLoaded(full, StateDocument(loaded), ApplyMode::Full);
    CHECK(sameDocument(full, loaded));

    StateDocument paramsOnly = sentinelDocument(Role::Input);
    mergeLoaded(paramsOnly, StateDocument(loaded), ApplyMode::ParamsOnly);
    CHECK(rinne::test::sameParams(paramsOnly.params, loaded.params)); // 含未知 id future_param
    CHECK(rinne::test::sameConfigs(paramsOnly.configs, sentinelDocument(Role::Input).configs)); // 含组号与未知键
    CHECK(paramsOnly.configs.group == 6);
    REQUIRE(paramsOnly.unknownChunks.size() == 1u);
    CHECK(paramsOnly.unknownChunks[0].fourCc == rinne::state::makeFourCc('S', 'E', 'N', 'T'));

    StateDocument none = sentinelDocument(Role::Input);
    mergeLoaded(none, StateDocument(loaded), ApplyMode::None);
    CHECK(sameDocument(none, sentinelDocument(Role::Input)));

    // Output 没有参数：只取参数时什么都不改
    StateDocument output = rinne::test::sampleOutputDocument();
    StateDocument other = rinne::state::makeDefaultDocument(Role::Output);
    other.configs.group = 7;
    mergeLoaded(output, std::move(other), ApplyMode::ParamsOnly);
    CHECK(sameDocument(output, rinne::test::sampleOutputDocument()));
}

TEST_CASE("glue: 定型只发生一次，不可撤销", "[state][glue]")
{
    rinne::state::glue::Settlement settlement;
    CHECK_FALSE(settlement.settled());
    CHECK(settlement.markUserAction());
    CHECK(settlement.settled());
    CHECK_FALSE(settlement.markUserAction());
    CHECK_FALSE(settlement.markUserAction());
    CHECK(settlement.settled());
}

TEST_CASE("glue: 保留原字节时保存原样交出，文档与已知参数一概不看", "[state][glue]")
{
    const auto ids = knownIdStrings();
    const auto known = knownOf(ids);
    rinne::state::RawWriteBack writeBack;
    StateDocument loaded = rinne::state::makeDefaultDocument(Role::Input);
    const Bytes newer = newerBytes(Role::Input, 9);
    REQUIRE(
        rinne::state::glue::loadForApply(newer.data(), newer.size(), Role::Input, false, writeBack, loaded).status ==
        StateLoadStatus::RejectedNewer);

    StateDocument document = rinne::test::sampleInputDocument();
    Bytes out = byteList({0xee});
    const auto saved = rinne::state::glue::bytesForSave(document, writeBack, known.data(), known.size(), out);
    CHECK(saved.ok);
    CHECK(saved.heldOriginal);
    CHECK(saved.dropped == 0u);
    CHECK(out == newer);
    CHECK(sameDocument(document, rinne::test::sampleInputDocument())); // 保留期间的改动不进 state
}

TEST_CASE("glue: 保存时先把已知参数 set 进文档，再从末尾裁掉未知 id", "[state][glue]")
{
    const auto ids = knownIdStrings();
    const auto known = knownOf(ids);
    const rinne::state::RawWriteBack none;

    for (const std::size_t unknownCount : {std::size_t{245}, std::size_t{246}})
    {
        INFO("未知 id " << unknownCount << " 个");
        // 文档里只有未知 id：已知参数要在保存时补上。先裁剪再补的话，246 个时补完 257 条，整份编不出来
        StateDocument document = documentWithUnknownParams(unknownCount, 8u);
        Bytes out;
        const auto saved = rinne::state::glue::bytesForSave(document, none, known.data(), known.size(), out);
        REQUIRE(saved.ok);
        CHECK_FALSE(saved.heldOriginal);
        CHECK(saved.dropped == unknownCount - 245u);

        const StateDocument back = loadedBack(out);
        REQUIRE(back.params.entries.size() == rinne::state::kMaxParamCount);
        for (std::size_t i = 0; i < ids.size(); ++i)
        {
            const auto* entry = back.params.find(ids[i]);
            REQUIRE(entry != nullptr);
            CHECK(entry->value == static_cast<float>(i));
        }
        // 未知 id 原样、顺序不变；丢的只是最末那条
        for (std::size_t i = 0; i < 245u; ++i)
            CHECK(back.params.entries[i].id == rinne::test::paramIdOfLength(8u, i));
        CHECK(back.params.find(rinne::test::paramIdOfLength(8u, 245u)) == nullptr);

        // 丢掉的未知 id 不在实例的文档里了：再存一次不再丢，也不再计
        Bytes again;
        const auto second = rinne::state::glue::bytesForSave(document, none, known.data(), known.size(), again);
        CHECK(second.ok);
        CHECK(second.dropped == 0u);
        CHECK(again == out);
    }
}

TEST_CASE("glue: 已知参数在文档里时就地改值，不重复追加", "[state][glue]")
{
    const auto ids = knownIdStrings();
    const auto known = knownOf(ids);
    StateDocument document = rinne::state::makeDefaultDocument(Role::Input);
    document.params.entries = {{"k3", 99.0f}, {"unknown_a", 1.0f}, {"k0", 42.0f}};
    Bytes out;
    const auto saved = rinne::state::glue::bytesForSave(document, {}, known.data(), known.size(), out);
    REQUIRE(saved.ok);
    REQUIRE(document.params.entries.size() == 12u);
    CHECK(document.params.entries[0].id == "k3");
    CHECK(document.params.entries[0].value == 3.0f);
    CHECK(document.params.entries[1].id == "unknown_a");
    CHECK(document.params.entries[2].id == "k0");
    CHECK(document.params.entries[2].value == 0.0f);
    CHECK(document.params.entries[3].id == "k1"); // 缺的按 known 的顺序追加在末尾
    CHECK(sameDocument(loadedBack(out), document));
}

TEST_CASE("glue: 未知 id 很长时按 64 KiB 的载荷上限裁剪", "[state][glue]")
{
    // 252 个 255 字节的未知 id 共 65524 字节，读得进来；只补一个 13 字节的已知参数，条数（253）仍在上限内，
    // 载荷 65537 字节超出：只按字节上限丢最末一条未知 id
    const std::string id = "feedback";
    const std::array<rinne::state::glue::KnownParam, 1> known{{{id.c_str(), 75.0f}}};
    StateDocument document = documentWithUnknownParams(252u, rinne::state::kMaxParamIdBytes);
    Bytes out;
    const auto saved = rinne::state::glue::bytesForSave(document, {}, known.data(), known.size(), out);
    REQUIRE(saved.ok);
    CHECK(saved.dropped == 1u);
    const StateDocument back = loadedBack(out);
    CHECK(back.params.entries.size() == 252u);
    CHECK(back.params.find(id) != nullptr);
    CHECK(back.params.find(rinne::test::paramIdOfLength(rinne::state::kMaxParamIdBytes, 251u)) == nullptr);
    CHECK(back.params.find(rinne::test::paramIdOfLength(rinne::state::kMaxParamIdBytes, 250u)) != nullptr);
}

TEST_CASE("glue: 编码失败交出空块，不交半截字节", "[state][glue]")
{
    const auto ids = knownIdStrings();
    const auto known = knownOf(ids);

    // 设置里的未知键混进这一端认识的键（键 1 = group）：写回时会写出两份，编码拒绝
    StateDocument badConfigs = rinne::test::sampleInputDocument();
    badConfigs.configs.unknown.push_back({1, byteList({2})});
    // 未知块里混进这一端认识的块：同理
    StateDocument badChunks = rinne::test::sampleOutputDocument();
    badChunks.unknownChunks.push_back({rinne::state::kFourCcCfgs, byteList({})});

    for (StateDocument* document : {&badConfigs, &badChunks})
    {
        const bool input = document->role == Role::Input;
        Bytes out = byteList({0xaa, 0xbb});
        const auto saved = rinne::state::glue::bytesForSave(*document, {}, input ? known.data() : nullptr,
                                                            input ? known.size() : 0u, out);
        CHECK_FALSE(saved.ok);
        CHECK_FALSE(saved.heldOriginal);
        CHECK(out.empty());
    }

    // 已知参数的 id 为空、或给了条数却没给数组：都按失败处理，不访问空指针
    StateDocument document = rinne::test::sampleInputDocument();
    Bytes out = byteList({0xaa});
    const std::array<rinne::state::glue::KnownParam, 1> nullId{{{nullptr, 1.0f}}};
    CHECK_FALSE(rinne::state::glue::bytesForSave(document, {}, nullId.data(), nullId.size(), out).ok);
    CHECK(out.empty());
    out = byteList({0xaa});
    CHECK_FALSE(rinne::state::glue::bytesForSave(document, {}, nullptr, 3u, out).ok);
    CHECK(out.empty());

    // 对照：同一份文档去掉坏的那一项就编得出来
    badConfigs.configs.unknown.pop_back();
    CHECK(rinne::state::glue::bytesForSave(badConfigs, {}, known.data(), known.size(), out).ok);
    CHECK_FALSE(out.empty());
}
