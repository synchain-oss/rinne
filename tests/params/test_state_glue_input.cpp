// SPDX-License-Identifier: GPL-3.0-or-later
//
// Input 的 state 胶水（src/input/InputStateGlue.*）接上真实的参数树：
//   · 未定型时全量应用（参数与设置，以最后一份为准，不加 stateSeq）；定型后只改参数、设置与组号不动、stateSeq 加一；
//   · Empty / Corrupt / IgnoredProbe 保持当前；RejectedNewer 原字节回写到下一次成功读入，期间的改动不进 state；
//   · setState 与 prepare 的先后无关；limiter 与 ceiling 随工程保存、加载不复位；
//   · 保存时先 set 已知参数再裁剪（245 / 246 个未知 id，长 id 的载荷上限），丢弃条数计进 droppedUnknownParams；
//   · 编码失败交出空块；认领转正的回调在 getState、setState、定型三处被调用；markStateDirty 的两个标志。
// 分流与保存顺序的纯逻辑另在 tests/core/state/test_state_glue_common.cpp 里直接测。

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <juce_audio_processors/juce_audio_processors.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../src/input/InputParams.h"
#include "../../src/input/InputStateGlue.h"
#include "core/state/StateTestKit.h"
#include "engine/EngineParams.h"
#include "params/ParamsTestProcessor.h"
#include "state/StateCodec.h"
#include "state/StateDocument.h"
#include "state/StateMigration.h"

namespace
{

using Catch::Approx;
using rinne::input::InputParams;
using rinne::input::InputStateGlue;
using rinne::state::Configs;
using rinne::state::ParamValue;
using rinne::state::Role;
using rinne::state::StateDocument;
using rinne::state::StateLoadStatus;
using rinne::test::byteList;
using rinne::test::Bytes;
using rinne::test::params::ParamsTestProcessor;

const std::vector<std::string> kIds = {"feedback", "loop_on",  "mix",      "delay_mode", "delay_ms", "delay_note",
                                       "low_cut",  "high_cut", "polarity", "limiter",    "ceiling"};

// 处理器、参数层与被测的胶水，和生产里一样：参数层先建，胶水接在它的参数树上
struct Rig
{
    ParamsTestProcessor processor;
    InputParams params{processor};
    InputStateGlue glue{params.tree()};

    juce::RangedAudioParameter& parameter(const std::string& id)
    {
        auto* found = params.tree().getParameter(id);
        REQUIRE(found != nullptr);
        return *found;
    }

    float plain(const std::string& id)
    {
        auto& p = parameter(id);
        return p.convertFrom0to1(p.getValue());
    }

    // 像界面或宿主那样写参数
    void setPlain(const std::string& id, float value)
    {
        auto& p = parameter(id);
        p.setValueNotifyingHost(p.convertTo0to1(value));
    }

    std::vector<float> plainValues()
    {
        std::vector<float> values;
        for (const auto& id : kIds)
            values.push_back(plain(id));
        return values;
    }

    void load(const Bytes& bytes) { glue.setState(bytes.data(), static_cast<int>(bytes.size())); }

    Bytes save()
    {
        juce::MemoryBlock block;
        glue.getState(block);
        const auto* data = static_cast<const std::uint8_t*>(block.getData());
        return Bytes(data, data + block.getSize());
    }

    void prepare()
    {
        processor.setRateAndBufferSizeDetails(48000.0, 512);
        processor.prepareToPlay(48000.0, 512);
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
    StateDocument document = rinne::test::sentinelDocument(Role::Input);
    REQUIRE(rinne::state::loadState(bytes.data(), bytes.size(), Role::Input, document).status == StateLoadStatus::Ok);
    return document;
}

float savedValue(const StateDocument& document, const std::string& id)
{
    const ParamValue* entry = document.params.find(id);
    REQUIRE(entry != nullptr);
    return entry->value;
}

// 状态 A：11 个参数与全部设置都不是默认值，另带一个未知参数、一个未知键与一个未知块
StateDocument stateA()
{
    StateDocument document = rinne::state::makeDefaultDocument(Role::Input);
    document.params.entries = {{"feedback", 40.0f},  {"loop_on", 0.0f},    {"mix", 60.0f},      {"delay_mode", 1.0f},
                               {"delay_ms", 250.0f}, {"delay_note", 5.0f}, {"low_cut", 100.0f}, {"high_cut", 5000.0f},
                               {"polarity", 1.0f},   {"limiter", 0.0f},    {"ceiling", -10.0f}, {"future_a", 0.25f}};
    Configs& c = document.configs;
    c.group = 3;
    c.groupOrigin = rinne::state::GroupOrigin::User;
    c.resetOnPlay = false;
    c.tailMode = rinne::state::TailMode::Infinite;
    c.delayUnit = rinne::state::DelayUnit::Samples;
    c.fbUnit = rinne::state::FbUnit::Decibel;
    c.guardOn = false;
    c.uiScale = 150;
    c.uiLang = "fr";
    c.presetName = "Dub 1/4D";
    c.presetDirty = true;
    c.unknown.push_back({300, byteList({0x0a})});
    document.unknownChunks.push_back({rinne::state::makeFourCc('X', 'T', 'R', 'A'), byteList({0x0a})});
    return document;
}

// 状态 B：另一组值，缺 mix 与 ceiling 两个参数（应用时取默认），带另一个未知参数与未知块
StateDocument stateB()
{
    StateDocument document = rinne::state::makeDefaultDocument(Role::Input);
    document.params.entries = {{"future_b", 0.75f}, {"feedback", 20.0f},   {"loop_on", 1.0f},  {"delay_mode", 0.0f},
                               {"delay_ms", 75.0f}, {"delay_note", 12.0f}, {"low_cut", 50.0f}, {"high_cut", 12000.0f},
                               {"polarity", 0.0f},  {"limiter", 1.0f}};
    Configs& c = document.configs;
    c.group = 5;
    c.groupOrigin = rinne::state::GroupOrigin::Smart;
    c.tailMode = rinne::state::TailMode::ThirtySeconds;
    c.uiScale = 90;
    c.uiLang = "zh";
    c.presetName = "B";
    document.unknownChunks.push_back({rinne::state::makeFourCc('B', 'B', 'B', 'B'), byteList({0x0b})});
    return document;
}

// state 里的参数值经参数层的规则（缺失取默认、夹进范围）之后应当落在参数树里的样子
std::vector<float> expectedValues(const StateDocument& document)
{
    const std::vector<float> defaults = {0.0f, 1.0f, 25.0f, 0.0f, 100.0f, 10.0f, 20.0f, 20000.0f, 0.0f, 1.0f, -6.0f};
    std::vector<float> values;
    for (std::size_t i = 0; i < kIds.size(); ++i)
    {
        const ParamValue* entry = document.params.find(kIds[i]);
        values.push_back(entry != nullptr ? entry->value : defaults[i]);
    }
    return values;
}

void checkValues(Rig& rig, const std::vector<float>& expected)
{
    for (std::size_t i = 0; i < kIds.size(); ++i)
    {
        INFO("参数 " << kIds[i]);
        CHECK(rig.plain(kIds[i]) == Approx(expected[i]).margin(1.0e-4));
    }
}

// 同一组设置，unknown 也逐条比
bool sameConfigs(const Configs& left, const Configs& right)
{
    return rinne::test::sameConfigs(left, right);
}

std::vector<rinne::test::RawParam> rawKnown(const std::vector<std::string>& ids)
{
    std::vector<rinne::test::RawParam> raw;
    const std::vector<float> values = expectedValues(rinne::state::makeDefaultDocument(Role::Input));
    for (std::size_t i = 0; i < kIds.size(); ++i)
    {
        for (const auto& id : ids)
        {
            if (id == kIds[i])
                raw.push_back({id, rinne::test::floatBits(values[i])});
        }
    }
    return raw;
}

Bytes prmsState(const std::vector<rinne::test::RawParam>& params)
{
    return rinne::test::rawContainer(rinne::state::kAbi, 1u,
                                     {{rinne::state::kFourCcPrms, rinne::test::rawParams(params)}});
}

Bytes newerState(std::uint8_t tag)
{
    return rinne::test::rawContainer(rinne::state::kAbi + 1u, 1u,
                                     {{rinne::state::makeFourCc('N', 'E', 'W', 'R'), byteList({tag})}});
}

const Bytes kCorrupt = byteList({'n', 'o', 't', ' ', 'a', ' ', 's', 't', 'a', 't', 'e', '!', 0, 0, 0, 0});

// 接住 updateHostDisplay 发出的 ChangeDetails
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

TEST_CASE("input glue: 新实例的状态位与引擎开关是默认值", "[params][state-glue]")
{
    Rig rig;
    CHECK_FALSE(rig.glue.lastLoadStatus().has_value());
    CHECK_FALSE(rig.glue.holdingNewerState());
    CHECK_FALSE(rig.glue.isSettled());
    CHECK(rig.glue.stateSeq() == 0u);
    CHECK(rig.glue.guardOn());
    CHECK(rig.glue.resetOnPlay());
    CHECK(rig.glue.tailMode() == rinne::engine::TailMode::kAuto);
    CHECK(sameConfigs(rig.glue.configs(), Configs{}));

    // 新实例保存的是参数表默认值加默认设置
    const StateDocument saved = decodeOf(rig.save());
    REQUIRE(saved.params.entries.size() == kIds.size());
    const std::vector<float> defaults = expectedValues(rinne::state::makeDefaultDocument(Role::Input));
    for (std::size_t i = 0; i < kIds.size(); ++i)
    {
        CHECK(saved.params.entries[i].id == kIds[i]);
        CHECK(saved.params.entries[i].value == Approx(defaults[i]).margin(1.0e-4));
    }
    CHECK(sameConfigs(saved.configs, Configs{}));
}

TEST_CASE("input glue: 未定型时全量应用参数与设置，以最后一份为准，不加 stateSeq", "[params][state-glue]")
{
    Rig rig;
    const StateDocument a = stateA();
    rig.load(encodeOf(a));
    CHECK(rig.glue.lastLoadStatus() == StateLoadStatus::Ok);
    checkValues(rig, expectedValues(a));
    CHECK(sameConfigs(rig.glue.configs(), a.configs));
    CHECK_FALSE(rig.glue.guardOn());
    CHECK_FALSE(rig.glue.resetOnPlay());
    CHECK(rig.glue.tailMode() == rinne::engine::TailMode::kInfinite);
    CHECK(rig.glue.stateSeq() == 0u);
    CHECK_FALSE(rig.glue.isSettled());

    // 保存：未知参数、未知键与未知块原样写回
    const StateDocument savedA = decodeOf(rig.save());
    CHECK(savedA.params.find("future_a") != nullptr);
    CHECK(sameConfigs(savedA.configs, a.configs));
    REQUIRE(savedA.unknownChunks.size() == 1u);
    CHECK(savedA.unknownChunks[0].payload == byteList({0x0a}));

    // 第二份：整份换成 B，缺的参数回到默认值，A 的未知项不再留着
    const StateDocument b = stateB();
    rig.load(encodeOf(b));
    checkValues(rig, expectedValues(b));
    CHECK(rig.plain("mix") == Approx(25.0f));
    CHECK(rig.plain("ceiling") == Approx(-6.0f));
    CHECK(sameConfigs(rig.glue.configs(), b.configs));
    CHECK(rig.glue.guardOn());
    CHECK(rig.glue.resetOnPlay());
    CHECK(rig.glue.tailMode() == rinne::engine::TailMode::kThirtySeconds);
    CHECK(rig.glue.stateSeq() == 0u);

    const StateDocument savedB = decodeOf(rig.save());
    CHECK(savedB.params.find("future_a") == nullptr);
    CHECK(savedB.params.entries.front().id == "future_b"); // 未知 id 留在原位，缺的已知参数追加在末尾
    CHECK(savedValue(savedB, "mix") == Approx(25.0f));
    REQUIRE(savedB.unknownChunks.size() == 1u);
    CHECK(savedB.unknownChunks[0].payload == byteList({0x0b}));
}

TEST_CASE("input glue: 参数值按参数层的规则落地：非有限值取默认，越界夹进范围", "[params][state-glue]")
{
    Rig rig;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    StateDocument document = rinne::state::makeDefaultDocument(Role::Input);
    document.params.entries = {{"feedback", nan}, {"mix", 500.0f},      {"delay_ms", -3.0f}, {"delay_note", 99.0f},
                               {"limiter", inf},  {"ceiling", -100.0f}, {"high_cut", -inf}};
    rig.setPlain("feedback", 33.0f);
    rig.load(encodeOf(document));
    CHECK(rig.plain("feedback") == Approx(0.0f)); // NaN 取默认值，不保留之前的 33
    CHECK(rig.plain("mix") == Approx(100.0f));
    CHECK(rig.plain("delay_ms") == Approx(1.0f));
    CHECK(rig.plain("delay_note") == Approx(15.0f));
    CHECK(rig.plain("limiter") == Approx(1.0f));
    CHECK(rig.plain("ceiling") == Approx(-24.0f));
    CHECK(rig.plain("high_cut") == Approx(20000.0f)); // -inf 也是非有限值：取默认，不夹到下限
}

TEST_CASE("input glue: 定型后只改参数，设置与组号不动，stateSeq 每次恰好加一", "[params][state-glue]")
{
    Rig rig;
    const StateDocument a = stateA();
    rig.load(encodeOf(a));
    CHECK(rig.glue.markUserAction());
    CHECK(rig.glue.isSettled());
    CHECK(rig.glue.stateSeq() == 0u); // 定型本身不加序号

    const StateDocument b = stateB();
    rig.load(encodeOf(b));
    CHECK(rig.glue.lastLoadStatus() == StateLoadStatus::Ok);
    checkValues(rig, expectedValues(b)); // 参数照 B，缺的取默认
    CHECK(sameConfigs(rig.glue.configs(), a.configs)); // 设置仍是 A，组号 3 不动
    CHECK(rig.glue.configs().group == 3);
    CHECK_FALSE(rig.glue.guardOn());
    CHECK(rig.glue.tailMode() == rinne::engine::TailMode::kInfinite);
    CHECK(rig.glue.stateSeq() == 1u);

    // 保存：参数列表是 B 的（含 B 的未知 id），设置与未知块还是 A 的
    const StateDocument saved = decodeOf(rig.save());
    CHECK(saved.params.find("future_b") != nullptr);
    CHECK(saved.params.find("future_a") == nullptr);
    CHECK(sameConfigs(saved.configs, a.configs));
    REQUIRE(saved.unknownChunks.size() == 1u);
    CHECK(saved.unknownChunks[0].payload == byteList({0x0a}));

    // 每一份成功读入加一；没读进来的不加
    rig.load(encodeOf(a));
    CHECK(rig.glue.stateSeq() == 2u);
    checkValues(rig, expectedValues(a));
    rig.load(kCorrupt);
    rig.load(Bytes{});
    rig.load(newerState(1));
    CHECK(rig.glue.stateSeq() == 2u);

    // 定型不可撤销
    CHECK_FALSE(rig.glue.markUserAction());
    CHECK(rig.glue.isSettled());
}

namespace
{

// 参数通知里读到的 stateSeq：引擎每块先读序号、再读参数，序号先到才能让新参数落在淡入里
struct SeqProbe final : juce::AudioProcessorParameter::Listener
{
    explicit SeqProbe(const InputStateGlue& g) : glue(g) {}
    void parameterValueChanged(int, float) override { seen.push_back(glue.stateSeq()); }
    void parameterGestureChanged(int, bool) override {}
    const InputStateGlue& glue;
    std::vector<std::uint32_t> seen;
};

} // namespace

TEST_CASE("input glue: 定型后的 setState 先加 stateSeq 再写参数", "[params][state-glue]")
{
    Rig rig;
    rig.load(encodeOf(stateA()));
    REQUIRE(rig.glue.markUserAction());
    SeqProbe probe(rig.glue);
    rig.parameter("feedback").addListener(&probe);
    rig.load(encodeOf(stateB())); // feedback 40 → 20
    rig.parameter("feedback").removeListener(&probe);
    REQUIRE(probe.seen.size() == 1u);
    CHECK(probe.seen[0] == 1u);
}

namespace
{

// 参数通知里回头调 getState（宿主收到参数通知时可能这么做），记下每次交出的字节；throwOnce 时第一次通知抛出
struct SaveProbe final : juce::AudioProcessorParameter::Listener
{
    explicit SaveProbe(InputStateGlue& g, bool throwFirst = false) : glue(g), throwOnce(throwFirst) {}
    void parameterValueChanged(int, float) override
    {
        if (throwOnce)
        {
            throwOnce = false;
            throw std::runtime_error("listener");
        }
        juce::MemoryBlock block;
        glue.getState(block);
        const auto* data = static_cast<const std::uint8_t*>(block.getData());
        saves.emplace_back(data, data + block.getSize());
    }
    void parameterGestureChanged(int, bool) override {}
    InputStateGlue& glue;
    bool throwOnce;
    std::vector<Bytes> saves;
};

} // namespace

TEST_CASE("input glue: setState 写参数树期间的 getState 交出这次要写的值，不交新旧混合的一份", "[params][state-glue]")
{
    // 未定型（全量应用）与定型后（只改参数）两条路各走一遍
    for (const bool settled : {false, true})
    {
        INFO((settled ? "已定型" : "未定型"));
        Rig rig;
        if (settled)
            REQUIRE(rig.glue.markUserAction());
        SaveProbe probe(rig.glue);
        rig.parameter("feedback").addListener(&probe);
        // feedback 0 → 40 最先写；ceiling −6 → −10 最后写，通知到来时还没写。ceiling 与 delay_ms 给不在步进格上的值，
        // 落进参数树时各自落到 −10 与 250
        StateDocument incoming = stateA();
        incoming.params.set("ceiling", -10.03f);
        incoming.params.set("delay_ms", 250.0004f);
        rig.load(encodeOf(incoming));
        rig.parameter("feedback").removeListener(&probe);
        REQUIRE(probe.saves.size() == 1u);

        const StateDocument during = decodeOf(probe.saves[0]);
        CHECK(savedValue(during, "feedback") == Approx(40.0f).margin(1.0e-4));
        CHECK(savedValue(during, "ceiling") == Approx(-10.0f).margin(1.0e-4));
        CHECK(savedValue(during, "delay_ms") == Approx(250.0f).margin(1.0e-3));
        // 与写完之后保存的逐字节相同：交出的是落到步进格上的值，对数刻度的参数也一样
        CHECK(probe.saves[0] == rig.save());
    }
}

TEST_CASE("input glue: setState 写参数树时抛出，之后的 getState 回到读参数树", "[params][state-glue]")
{
    Rig rig;
    SaveProbe probe(rig.glue, true);
    rig.parameter("feedback").addListener(&probe);
    CHECK_THROWS_AS(rig.load(encodeOf(stateA())), std::runtime_error); // feedback 已写成 40，其余没写
    rig.parameter("feedback").removeListener(&probe);

    const StateDocument saved = decodeOf(rig.save());
    CHECK(savedValue(saved, "feedback") == Approx(40.0f).margin(1.0e-4));
    CHECK(savedValue(saved, "ceiling") == Approx(-6.0f).margin(1.0e-4)); // 参数树里的值，不是这次没写完的 −10
}

TEST_CASE("input glue: Empty、Corrupt、IgnoredProbe 保持当前，不回到默认", "[params][state-glue]")
{
    Rig rig;
    rig.load(encodeOf(stateA()));
    // 当前 = A 加上之后的改动
    rig.setPlain("feedback", 33.0f);
    rig.glue.editConfigs([](Configs& c) { c.group = 4; });
    const std::vector<float> before = rig.plainValues();
    const Configs configsBefore = rig.glue.configs();
    const Bytes savedBefore = rig.save();

    const Bytes probe = rinne::test::rawContainer(rinne::state::kProbeAbi, 1u, {});
    const Bytes otherRole = encodeOf(rinne::test::sampleOutputDocument()); // 对端的字节：Corrupt
    struct Case
    {
        const char* name;
        const void* data;
        int size;
        StateLoadStatus status;
    };
    const Case cases[] = {
        {"空指针", nullptr, 0, StateLoadStatus::Empty},
        {"零字节", savedBefore.data(), 0, StateLoadStatus::Empty},
        {"负长度", savedBefore.data(), -5, StateLoadStatus::Empty},
        {"坏字节", kCorrupt.data(), static_cast<int>(kCorrupt.size()), StateLoadStatus::Corrupt},
        {"对端的 state", otherRole.data(), static_cast<int>(otherRole.size()), StateLoadStatus::Corrupt},
        {"探针", probe.data(), static_cast<int>(probe.size()), StateLoadStatus::IgnoredProbe},
    };
    for (const auto& c : cases)
    {
        INFO(c.name);
        rig.glue.setState(c.data, c.size);
        CHECK(rig.glue.lastLoadStatus() == c.status);
        CHECK(rig.plainValues() == before);
        CHECK(sameConfigs(rig.glue.configs(), configsBefore));
        CHECK(rig.glue.configs().group == 4);
        CHECK_FALSE(rig.glue.holdingNewerState());
        CHECK(rig.glue.stateSeq() == 0u);
        CHECK(rig.save() == savedBefore);
    }
}

TEST_CASE("input glue: RejectedNewer 原字节回写到下一次成功读入，期间的改动不进 state", "[params][state-glue]")
{
    Rig rig;
    const StateDocument a = stateA();
    rig.load(encodeOf(a));

    const Bytes newer = newerState(1);
    rig.load(newer);
    CHECK(rig.glue.lastLoadStatus() == StateLoadStatus::RejectedNewer);
    CHECK(rig.glue.holdingNewerState());
    checkValues(rig, expectedValues(a)); // 不应用
    CHECK(rig.save() == newer);

    // 保留期间改参数、改设置：保存交回的仍是原字节
    rig.setPlain("feedback", 12.0f);
    rig.glue.editConfigs([](Configs& c) { c.group = 8; });
    CHECK(rig.save() == newer);

    // 没读进来的不动保留
    rig.load(kCorrupt);
    rig.load(Bytes{});
    CHECK(rig.glue.holdingNewerState());
    CHECK(rig.save() == newer);

    // 再来一份更高 abi：以最后一份为准
    const Bytes newer2 = newerState(2);
    rig.load(newer2);
    CHECK(rig.save() == newer2);

    // 成功读入取代保留：之后保存的是实例当前的 state
    const StateDocument b = stateB();
    rig.load(encodeOf(b));
    CHECK_FALSE(rig.glue.holdingNewerState());
    const StateDocument saved = decodeOf(rig.save());
    CHECK(savedValue(saved, "feedback") == Approx(20.0f));
    CHECK(sameConfigs(saved.configs, b.configs));

    // 定型之后也一样：更高 abi 照样接管，下一份成功读入只取参数并清掉保留
    REQUIRE(rig.glue.markUserAction());
    rig.load(newer);
    CHECK(rig.save() == newer);
    rig.load(encodeOf(a));
    CHECK_FALSE(rig.glue.holdingNewerState());
    CHECK(rig.glue.stateSeq() == 1u);
    CHECK(sameConfigs(decodeOf(rig.save()).configs, b.configs));
}

TEST_CASE("input glue: setState 在 prepare 之前与之后结果相同", "[params][state-glue]")
{
    const Bytes bytes = encodeOf(stateA());
    Rig early;
    early.load(bytes);
    early.prepare();

    Rig late;
    late.prepare();
    late.load(bytes);

    checkValues(early, expectedValues(stateA()));
    CHECK(early.plainValues() == late.plainValues());
    CHECK(sameConfigs(early.glue.configs(), late.glue.configs()));
    CHECK(early.glue.guardOn() == late.glue.guardOn());
    CHECK(early.glue.tailMode() == late.glue.tailMode());
    CHECK(early.save() == late.save());
}

TEST_CASE("input glue: limiter 关与非默认的 ceiling 随工程保存，加载不复位", "[params][state-glue]")
{
    Rig source;
    source.setPlain("limiter", 0.0f);
    source.setPlain("ceiling", -12.5f);
    const Bytes bytes = source.save();
    const StateDocument saved = decodeOf(bytes);
    CHECK(savedValue(saved, "limiter") == 0.0f);
    CHECK(savedValue(saved, "ceiling") == Approx(-12.5f));

    // 新实例（默认 limiter 开、ceiling -6）加载：按 state 里的值
    Rig fresh;
    REQUIRE(fresh.plain("limiter") == 1.0f);
    fresh.load(bytes);
    CHECK(fresh.plain("limiter") == 0.0f);
    CHECK(fresh.plain("ceiling") == Approx(-12.5f));
    CHECK(fresh.save() == bytes);

    // 定型之后的 setState 同样不复位
    Rig settled;
    REQUIRE(settled.glue.markUserAction());
    settled.load(bytes);
    CHECK(settled.plain("limiter") == 0.0f);
    CHECK(settled.plain("ceiling") == Approx(-12.5f));
}

TEST_CASE("input glue: 245 个未知 id 加 11 个参数存取为 Ok，246 个时只丢最后一个未知 id", "[params][state-glue]")
{
    std::vector<rinne::test::RawParam> unknown;
    for (std::size_t i = 0; i < 246u; ++i)
        unknown.push_back({"u" + std::to_string(i), rinne::test::floatBits(0.5f)});

    SECTION("245 个未知 id 加 11 个参数：一条不丢")
    {
        std::vector<rinne::test::RawParam> raw(unknown.begin(), unknown.begin() + 245);
        const auto known = rawKnown(kIds);
        raw.insert(raw.end(), known.begin(), known.end());
        Rig rig;
        rig.load(prmsState(raw));
        REQUIRE(rig.glue.lastLoadStatus() == StateLoadStatus::Ok);
        const Bytes bytes = rig.save();
        CHECK(rig.glue.droppedUnknownParams() == 0u);
        CHECK(rig.glue.encodeFailures() == 0u);
        const StateDocument saved = decodeOf(bytes);
        CHECK(saved.params.entries.size() == rinne::state::kMaxParamCount);
        Rig again;
        again.load(bytes);
        CHECK(again.glue.lastLoadStatus() == StateLoadStatus::Ok);
        CHECK(again.save() == bytes);
    }

    SECTION("246 个未知 id 加 10 个参数：补上第 11 个时丢最后那个未知 id")
    {
        std::vector<std::string> tenIds(kIds.begin(), kIds.end() - 1); // 缺 ceiling
        std::vector<rinne::test::RawParam> raw = unknown;
        const auto known = rawKnown(tenIds);
        raw.insert(raw.end(), known.begin(), known.end());
        Rig rig;
        rig.load(prmsState(raw));
        REQUIRE(rig.glue.lastLoadStatus() == StateLoadStatus::Ok);
        const Bytes bytes = rig.save();
        REQUIRE_FALSE(bytes.empty());
        CHECK(rig.glue.droppedUnknownParams() == 1u);
        const StateDocument saved = decodeOf(bytes);
        REQUIRE(saved.params.entries.size() == rinne::state::kMaxParamCount);
        for (std::size_t i = 0; i < 245u; ++i)
            CHECK(saved.params.entries[i].id == "u" + std::to_string(i));
        CHECK(saved.params.find("u245") == nullptr);
        for (const auto& id : kIds)
            CHECK(saved.params.find(id) != nullptr);
        CHECK(savedValue(saved, "ceiling") == Approx(-6.0f));

        // 丢掉的那条不在实例里了：再存不再丢，计数不再涨
        CHECK(rig.save() == bytes);
        CHECK(rig.glue.droppedUnknownParams() == 1u);
        Rig again;
        again.load(bytes);
        CHECK(again.glue.lastLoadStatus() == StateLoadStatus::Ok);
    }

    SECTION("246 个未知 id、一个已知参数都没有：先 set 再裁剪才存得出来")
    {
        // 先裁剪再 set 的话，裁剪时只有 246 条、一条不丢，补完 11 个参数是 257 条，整份编不出来
        Rig rig;
        rig.load(prmsState(unknown));
        REQUIRE(rig.glue.lastLoadStatus() == StateLoadStatus::Ok);
        const Bytes bytes = rig.save();
        REQUIRE_FALSE(bytes.empty());
        CHECK(rig.glue.encodeFailures() == 0u);
        CHECK(rig.glue.droppedUnknownParams() == 1u);
        const StateDocument saved = decodeOf(bytes);
        CHECK(saved.params.entries.size() == rinne::state::kMaxParamCount);
        CHECK(saved.params.find("u244") != nullptr);
        CHECK(saved.params.find("u245") == nullptr);
    }
}

TEST_CASE("input glue: 未知 id 很长时，保存的 PRMS 压进 64 KiB", "[params][state-glue]")
{
    // 252 个 255 字节的未知 id：PRMS 65524 字节，在 64 KiB 之内，读得进来。补上 11 个参数后是 263 条、65662 字节，
    // 两个上限都超出。从末尾丢 7 条未知 id 之后两个上限都满足（条数 256、载荷 63842 字节）。
    // 胶水总是补齐 11 个短 id 的参数，载荷上限不可能单独起作用：单独按字节丢的情形在 core 的用例里
    // （tests/core/state/test_state_glue_common.cpp）。
    std::vector<rinne::test::RawParam> wide;
    for (std::size_t i = 0; i < 252u; ++i)
        wide.push_back({rinne::test::longestParamId(i), rinne::test::floatBits(0.5f)});
    Rig rig;
    rig.load(prmsState(wide));
    REQUIRE(rig.glue.lastLoadStatus() == StateLoadStatus::Ok);
    const Bytes bytes = rig.save();
    REQUIRE_FALSE(bytes.empty());
    CHECK(rig.glue.droppedUnknownParams() == 7u);

    rinne::state::Container container;
    REQUIRE(rinne::state::decodeContainer(bytes.data(), bytes.size(), Role::Input, container));
    const auto* prms = container.find(rinne::state::kFourCcPrms);
    REQUIRE(prms != nullptr);
    CHECK(prms->payload.size() <= rinne::state::kMaxParamBytes);
    CHECK(prms->payload.size() == 63842u);
    const StateDocument saved = decodeOf(bytes);
    CHECK(saved.params.find(rinne::test::longestParamId(244u)) != nullptr);
    CHECK(saved.params.find(rinne::test::longestParamId(245u)) == nullptr);
    for (const auto& id : kIds)
        CHECK(saved.params.find(id) != nullptr);
}

TEST_CASE("input glue: 编码失败交出空块并计数", "[params][state-glue]")
{
    Rig rig;
    // 设置的未知键里混进这一端认识的键（键 1 = group）：写回会写出两份，文档编不出来
    rig.glue.editConfigs([](Configs& c) { c.unknown.push_back({1, byteList({2})}); });
    juce::MemoryBlock block;
    block.append("stale", 5);
    rig.glue.getState(block);
    CHECK(block.getSize() == 0u);
    CHECK(rig.glue.encodeFailures() == 1u);

    // 坏的那一项去掉就恢复正常
    rig.glue.editConfigs([](Configs& c) { c.unknown.clear(); });
    CHECK_FALSE(rig.save().empty());
    CHECK(rig.glue.encodeFailures() == 1u);
}

TEST_CASE("input glue: 认领转正的回调在 getState、setState 与定型三处各调一次", "[params][state-glue]")
{
    Rig rig;
    int calls = 0;
    bool settledInHook = false;
    rig.glue.setConfirmClaimHook([&] {
        ++calls;
        settledInHook = rig.glue.isSettled(); // 回调在锁外调用：在回调里读胶水不会死锁
    });

    rig.save();
    CHECK(calls == 1);
    rig.load(encodeOf(stateA()));
    CHECK(calls == 2);
    for (const Bytes& bytes : {Bytes{}, kCorrupt, newerState(1), rinne::test::rawContainer(0u, 1u, {})})
        rig.load(bytes); // 不论结果
    CHECK(calls == 6);
    rig.save(); // 保留原字节时也调
    CHECK(calls == 7);

    CHECK(rig.glue.markUserAction());
    CHECK(calls == 8);
    CHECK(settledInHook);
    CHECK_FALSE(rig.glue.markUserAction()); // 定型只有一次
    CHECK_FALSE(rig.glue.markUserAction());
    CHECK(calls == 8);

    // 拆掉回调之后不再调
    rig.glue.setConfirmClaimHook({});
    rig.save();
    CHECK(calls == 8);
}

TEST_CASE("input glue: markStateDirty 同时带 nonParameterStateChanged 与 programChanged", "[params][state-glue]")
{
    Rig rig;
    ChangeSpy spy;
    rig.processor.addListener(&spy);
    // 存取 state 本身不发这类通知
    rig.load(encodeOf(stateA()));
    rig.save();
    CHECK(spy.changes.empty());

    rig.glue.markStateDirty();
    REQUIRE(spy.changes.size() == 1u);
    CHECK(spy.changes[0].nonParameterStateChanged);
    CHECK(spy.changes[0].programChanged);
    CHECK_FALSE(spy.changes[0].latencyChanged);
    CHECK_FALSE(spy.changes[0].parameterInfoChanged);
    CHECK(rig.processor.getNumPrograms() == 1);
    rig.processor.removeListener(&spy);
}

TEST_CASE("input glue: snapshot 只写命令序号与三个工程开关", "[params][state-glue]")
{
    Rig rig;
    rig.load(encodeOf(stateA()));
    REQUIRE(rig.glue.markUserAction());
    rig.load(encodeOf(stateB()));

    rinne::engine::EngineParams out;
    out.feedbackPercent = 77.0f;
    out.presetSeq = 9u;
    out.panicSeq = 4u;
    rig.glue.snapshot(out);
    CHECK(out.stateSeq == 1u);
    CHECK_FALSE(out.guardOn);
    CHECK_FALSE(out.resetOnPlay);
    CHECK(out.tailMode == rinne::engine::TailMode::kInfinite);
    CHECK(out.feedbackPercent == 77.0f); // 参数字段归 InputParams::snapshot
    CHECK(out.presetSeq == 9u);
    CHECK(out.panicSeq == 4u);
}
