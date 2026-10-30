// SPDX-License-Identifier: GPL-3.0-or-later
//
// Output 的 state 胶水（src/output/OutputStateGlue.*）：Output 没有参数，state 只有 CFGS 的五个键。
//   · 未定型时全量应用设置（以最后一份为准，未知项原样留着）；定型后 setState 什么都不改；
//   · Empty / Corrupt / IgnoredProbe 保持当前；RejectedNewer 原字节回写到下一次成功读入，期间的改动不进 state；
//   · 编码失败交出空块；认领转正的回调在 getState、setState、定型三处被调用；markStateDirty 的两个标志；
//   · setState 交回这一次的读入结果、应用方式与换上的设置，口径与 applyModeFor 相同。

#include <catch2/catch_test_macros.hpp>

#include <juce_audio_processors/juce_audio_processors.h>

#include <cstdint>
#include <vector>

#include "../../src/output/OutputStateGlue.h"
#include "core/state/StateTestKit.h"
#include "params/ParamsTestProcessor.h"
#include "state/StateCodec.h"
#include "state/StateDocument.h"
#include "state/StateMigration.h"

namespace
{

using rinne::output::OutputStateGlue;
using rinne::state::Configs;
using rinne::state::Role;
using rinne::state::StateDocument;
using rinne::state::StateLoadStatus;
using rinne::test::byteList;
using rinne::test::Bytes;
using rinne::test::sameConfigs;
using rinne::test::params::ParamsTestProcessor;

struct Rig
{
    ParamsTestProcessor processor;
    OutputStateGlue glue{processor};

    void load(const Bytes& bytes) { glue.setState(bytes.data(), static_cast<int>(bytes.size())); }

    Bytes save()
    {
        juce::MemoryBlock block;
        glue.getState(block);
        const auto* data = static_cast<const std::uint8_t*>(block.getData());
        return Bytes(data, data + block.getSize());
    }
};

Bytes encodeOf(const StateDocument& document)
{
    Bytes bytes;
    REQUIRE(rinne::state::encodeDocument(document, bytes));
    return bytes;
}

StateDocument decodeOf(const Bytes& bytes)
{
    StateDocument document = rinne::test::sentinelDocument(Role::Output);
    REQUIRE(rinne::state::loadState(bytes.data(), bytes.size(), Role::Output, document).status == StateLoadStatus::Ok);
    return document;
}

// 状态 A：五个键都不是默认值，另带只有 Input 才认识的键 3（Output 当未知键原样带着）、一个未知块
StateDocument stateA()
{
    StateDocument document = rinne::test::sampleOutputDocument();
    document.configs.unknown.push_back({3, byteList({0})});
    document.unknownChunks.push_back({rinne::state::kFourCcPrms, byteList({0, 0, 0, 0})}); // 对端专有的块
    return document;
}

StateDocument stateB()
{
    StateDocument document = rinne::state::makeDefaultDocument(Role::Output);
    Configs& c = document.configs;
    c.group = 7;
    c.groupOrigin = rinne::state::GroupOrigin::User;
    c.tailMode = rinne::state::TailMode::Infinite;
    c.uiScale = 80;
    c.uiLang = "fr";
    return document;
}

Bytes newerState(std::uint8_t tag)
{
    return rinne::test::rawContainer(rinne::state::kAbi + 1u, 2u,
                                     {{rinne::state::makeFourCc('N', 'E', 'W', 'R'), byteList({tag})}});
}

const Bytes kCorrupt = byteList({'n', 'o', 't', ' ', 'a', ' ', 's', 't', 'a', 't', 'e', '!', 0, 0, 0, 0});

struct ChangeSpy final : juce::AudioProcessorListener
{
    void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override {}
    void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails& details) override
    {
        changes.push_back(details);
    }
    std::vector<ChangeDetails> changes;
};

} // namespace

TEST_CASE("output glue: 新实例保存默认设置", "[params][state-glue]")
{
    Rig rig;
    CHECK_FALSE(rig.glue.lastLoadStatus().has_value());
    CHECK(rig.glue.tailMode() == rinne::state::TailMode::Auto);
    CHECK(sameConfigs(rig.glue.configs(), Configs{}));
    const StateDocument saved = decodeOf(rig.save());
    CHECK(rinne::test::sameDocument(saved, rinne::state::makeDefaultDocument(Role::Output)));
}

TEST_CASE("output glue: 未定型时全量应用设置，以最后一份为准", "[params][state-glue]")
{
    Rig rig;
    const StateDocument a = stateA();
    rig.load(encodeOf(a));
    CHECK(rig.glue.lastLoadStatus() == StateLoadStatus::Ok);
    CHECK(sameConfigs(rig.glue.configs(), a.configs));
    CHECK(rig.glue.tailMode() == rinne::state::TailMode::ThirtySeconds);
    CHECK(rinne::test::sameDocument(decodeOf(rig.save()), a)); // 未知键与对端的块原样写回

    const StateDocument b = stateB();
    rig.load(encodeOf(b));
    CHECK(sameConfigs(rig.glue.configs(), b.configs));
    CHECK(rig.glue.tailMode() == rinne::state::TailMode::Infinite);
    CHECK(rinne::test::sameDocument(decodeOf(rig.save()), b));
}

TEST_CASE("output glue: 定型后 setState 什么都不改", "[params][state-glue]")
{
    Rig rig;
    const StateDocument a = stateA();
    rig.load(encodeOf(a));
    REQUIRE(rig.glue.markUserAction());
    const Bytes before = rig.save();

    rig.load(encodeOf(stateB()));
    CHECK(rig.glue.lastLoadStatus() == StateLoadStatus::Ok);
    CHECK(sameConfigs(rig.glue.configs(), a.configs));
    CHECK(rig.glue.configs().group == 2);
    CHECK(rig.glue.tailMode() == rinne::state::TailMode::ThirtySeconds);
    CHECK(rig.save() == before);

    // 写回保留照样按结果更新
    const Bytes newer = newerState(1);
    rig.load(newer);
    CHECK(rig.glue.holdingNewerState());
    CHECK(rig.save() == newer);
    rig.load(encodeOf(stateB()));
    CHECK_FALSE(rig.glue.holdingNewerState());
    CHECK(rig.save() == before);
}

TEST_CASE("output glue: Empty、Corrupt、IgnoredProbe 保持当前，RejectedNewer 原样回写", "[params][state-glue]")
{
    Rig rig;
    rig.load(encodeOf(stateA()));
    rig.glue.editConfigs([](Configs& c) { c.group = 4; });
    const Configs current = rig.glue.configs();
    const Bytes saved = rig.save();

    const Bytes probe = rinne::test::rawContainer(rinne::state::kProbeAbi, 2u, {});
    const Bytes inputBytes = encodeOf(rinne::test::sampleInputDocument()); // 对端的字节：Corrupt
    for (const Bytes& bytes : {Bytes{}, kCorrupt, probe, inputBytes})
    {
        rig.load(bytes);
        CHECK(sameConfigs(rig.glue.configs(), current));
        CHECK_FALSE(rig.glue.holdingNewerState());
        CHECK(rig.save() == saved);
    }
    CHECK(rig.glue.lastLoadStatus() == StateLoadStatus::Corrupt);

    const Bytes newer = newerState(1);
    rig.load(newer);
    CHECK(rig.glue.lastLoadStatus() == StateLoadStatus::RejectedNewer);
    CHECK(sameConfigs(rig.glue.configs(), current)); // 不应用
    rig.glue.editConfigs([](Configs& c) { c.group = 6; });
    CHECK(rig.save() == newer); // 保留期间的改动不进 state
    rig.load(kCorrupt);
    CHECK(rig.save() == newer);

    const StateDocument b = stateB();
    rig.load(encodeOf(b));
    CHECK_FALSE(rig.glue.holdingNewerState());
    CHECK(rinne::test::sameDocument(decodeOf(rig.save()), b));
}

TEST_CASE("output glue: 编码失败交出空块并计数", "[params][state-glue]")
{
    Rig rig;
    rig.glue.editConfigs([](Configs& c) { c.unknown.push_back({2, byteList({1})}); }); // 键 2 = group_origin
    juce::MemoryBlock block;
    block.append("stale", 5);
    rig.glue.getState(block);
    CHECK(block.getSize() == 0u);
    CHECK(rig.glue.encodeFailures() == 1u);
}

TEST_CASE("output glue: 认领转正的回调在 getState、setState 与定型三处各调一次", "[params][state-glue]")
{
    Rig rig;
    int calls = 0;
    rig.glue.setConfirmClaimHook([&] { ++calls; });
    rig.save();
    CHECK(calls == 1);
    for (const Bytes& bytes : {encodeOf(stateA()), Bytes{}, kCorrupt, newerState(1)})
        rig.load(bytes);
    CHECK(calls == 5);
    CHECK(rig.glue.markUserAction());
    CHECK(calls == 6);
    CHECK_FALSE(rig.glue.markUserAction());
    CHECK(calls == 6);
    rig.load(encodeOf(stateB())); // 定型之后的 setState 什么都不改，回调照样调
    CHECK(calls == 7);
}

TEST_CASE("output glue: markStateDirty 同时带 nonParameterStateChanged 与 programChanged", "[params][state-glue]")
{
    Rig rig;
    ChangeSpy spy;
    rig.processor.addListener(&spy);
    rig.load(encodeOf(stateA()));
    rig.save();
    CHECK(spy.changes.empty());
    rig.glue.markStateDirty();
    REQUIRE(spy.changes.size() == 1u);
    CHECK(spy.changes[0].nonParameterStateChanged);
    CHECK(spy.changes[0].programChanged);
    CHECK_FALSE(spy.changes[0].latencyChanged);
    CHECK_FALSE(spy.changes[0].parameterInfoChanged);
    rig.processor.removeListener(&spy);
}

TEST_CASE("output glue: setState 交回这一次的读入结果与应用方式，口径与 applyModeFor 相同", "[params][state-glue]")
{
    // 本构建的 abi 是 1，还没有更低的版本可迁，Migrated 造不出来；它的口径（与 Ok 相同）在 core 的分流用例里测
    const Bytes ok = encodeOf(stateB());
    const Bytes newer = newerState(1);
    const Bytes probe = rinne::test::rawContainer(rinne::state::kProbeAbi, 2u, {});
    struct Case
    {
        const char* name;
        const void* data;
        int size;
        StateLoadStatus status;
    };
    const Case cases[] = {
        {"成功读入", ok.data(), static_cast<int>(ok.size()), StateLoadStatus::Ok},
        {"更高版本", newer.data(), static_cast<int>(newer.size()), StateLoadStatus::RejectedNewer},
        {"空", nullptr, 0, StateLoadStatus::Empty},
        {"坏字节", kCorrupt.data(), static_cast<int>(kCorrupt.size()), StateLoadStatus::Corrupt},
        {"探针", probe.data(), static_cast<int>(probe.size()), StateLoadStatus::IgnoredProbe},
    };
    for (const bool settled : {false, true})
    {
        for (const auto& c : cases)
        {
            INFO(c.name << (settled ? "，定型后" : "，定型前"));
            Rig rig;
            if (settled)
                REQUIRE(rig.glue.markUserAction());
            const OutputStateGlue::SetStateResult result = rig.glue.setState(c.data, c.size);
            CHECK(result.status == c.status);
            CHECK(rig.glue.lastLoadStatus() == result.status);
            CHECK(result.mode == rinne::state::glue::applyModeFor(c.status, settled));
            // 设置副本只在全量应用时交回，交回的就是这一次换上的那份
            CHECK(result.applied.has_value() == (result.mode == rinne::state::glue::ApplyMode::Full));
            if (result.applied.has_value())
            {
                CHECK(result.applied->group == 7);
                CHECK(sameConfigs(*result.applied, rig.glue.configs()));
            }
        }
    }
}
