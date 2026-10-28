// SPDX-License-Identifier: GPL-3.0-or-later
//
// 载入预设（PresetStore::load）：只改 8 个声音参数，每个参数恰好一对手势，成功时 presetSeq 恰好加一，
// 且先加序号、再写参数（第一个被写的参数的通知里序号已经加过一）；失败什么都不改、序号不动；
// loop_on、limiter、ceiling 在载入前后不变。
// 用真实的参数树（InputParams）和工厂预设；用户预设的载入与越界夹回在 test_presets_user.cpp。
// 「预设前后组号不变」这一格：组号不在参数树里，PresetStore 也不接触它（载入只经参数树写 8 个声音参数），
// 没有可以断言的地方；rig 里的载入预设场景只核引擎层（淡入原因、不解除锁存）。

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../../src/input/InputParams.h"
#include "../../src/input/PresetStore.h"
#include "params/ParamsTestProcessor.h"

namespace
{

using namespace rinne::test::params;
using Catch::Approx;
using rinne::input::InputParams;
using rinne::input::kPresetParamCount;
using rinne::input::PresetError;
using rinne::input::PresetStore;

// 记下每个参数收到的手势与值变化，按发生的先后
class Recorder final : public juce::AudioProcessorParameter::Listener
{
public:
    struct Event
    {
        int parameter;
        enum Kind
        {
            kBegin,
            kEnd,
            kValue,
        } kind;
    };

    void parameterValueChanged(int index, float) override { events.push_back({index, Event::kValue}); }
    void parameterGestureChanged(int index, bool starting) override
    {
        events.push_back({index, starting ? Event::kBegin : Event::kEnd});
    }

    int count(int parameter, Event::Kind kind) const
    {
        int n = 0;
        for (const Event& e : events)
            n += (e.parameter == parameter && e.kind == kind) ? 1 : 0;
        return n;
    }

    std::vector<Event> events;
};

struct Fixture
{
    ParamsTestProcessor processor;
    InputParams params{processor};
    PresetStore store{juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("rinne-presets-unused")};
    Recorder recorder;

    Fixture()
    {
        for (auto* parameter : processor.getParameters())
            parameter->addListener(&recorder);
    }

    ~Fixture()
    {
        for (auto* parameter : processor.getParameters())
            parameter->removeListener(&recorder);
    }

    juce::RangedAudioParameter& parameter(const std::string& id)
    {
        auto* found = params.tree().getParameter(id);
        REQUIRE(found != nullptr);
        return *found;
    }

    void setPlain(const std::string& id, float plain)
    {
        auto& p = parameter(id);
        p.setValueNotifyingHost(p.convertTo0to1(plain));
    }

    float raw(const std::string& id) { return params.tree().getRawParameterValue(id)->load(); }

    int indexOf(const std::string& id) { return parameter(id).getParameterIndex(); }

    // 8 个声音参数的当前明文值
    rinne::input::PresetValues sound()
    {
        rinne::input::PresetValues values{};
        for (std::size_t i = 0; i < kPresetParamCount; ++i)
            values[i] = raw(rinne::input::presetParamId(i));
        return values;
    }

    // 三个不属于预设的参数的当前值
    std::array<float, 3> project() { return {raw("loop_on"), raw("limiter"), raw("ceiling")}; }

    void makeProjectNonDefault()
    {
        setPlain("loop_on", 0.0f);
        setPlain("limiter", 0.0f);
        setPlain("ceiling", -12.0f);
    }

    void makeSoundNonDefault()
    {
        setPlain("feedback", 99.0f);
        setPlain("mix", 1.0f);
        setPlain("delay_mode", 1.0f);
        setPlain("delay_ms", 3000.0f);
        setPlain("delay_note", 3.0f);
        setPlain("low_cut", 500.0f);
        setPlain("high_cut", 3000.0f);
        setPlain("polarity", 1.0f);
    }
};

} // namespace

TEST_CASE("load: a factory preset sets the 8 sound parameters and nothing else", "[presets][load]")
{
    Fixture f;
    REQUIRE(f.store.factoryPresetsValid());
    f.makeProjectNonDefault();
    const auto projectBefore = f.project();
    f.recorder.events.clear();

    // Dub 1/4D：反馈 65、混合 50、Sync、1/4D，其余默认
    const auto result = f.store.load("f-dub-quarter-dotted", f.params.tree());
    REQUIRE(result.ok());
    CHECK(result.id == "f-dub-quarter-dotted");
    CHECK(result.name == "Dub 1/4D");

    CHECK(f.raw("feedback") == Approx(65.0f));
    CHECK(f.raw("mix") == Approx(50.0f));
    CHECK(f.raw("delay_mode") == 1.0f);
    CHECK(f.raw("delay_ms") == Approx(100.0f).margin(0.001));
    CHECK(f.raw("delay_note") == 12.0f);
    CHECK(f.raw("low_cut") == Approx(20.0f));
    CHECK(f.raw("high_cut") == Approx(20000.0f));
    CHECK(f.raw("polarity") == 0.0f);

    // loop_on、limiter、ceiling 原样不动
    CHECK(f.project() == projectBefore);
    CHECK(f.raw("loop_on") == 0.0f);
    CHECK(f.raw("limiter") == 0.0f);
    CHECK(f.raw("ceiling") == Approx(-12.0f));

    // 每个声音参数恰好一对手势，三个工程参数没有任何手势或值变化
    for (std::size_t i = 0; i < kPresetParamCount; ++i)
    {
        const std::string id = rinne::input::presetParamId(i);
        INFO("参数 " << id);
        const int index = f.indexOf(id);
        CHECK(f.recorder.count(index, Recorder::Event::kBegin) == 1);
        CHECK(f.recorder.count(index, Recorder::Event::kEnd) == 1);
        CHECK(f.recorder.count(index, Recorder::Event::kValue) == 1);
    }
    for (const char* id : {"loop_on", "limiter", "ceiling"})
    {
        INFO("参数 " << id);
        const int index = f.indexOf(id);
        CHECK(f.recorder.count(index, Recorder::Event::kBegin) == 0);
        CHECK(f.recorder.count(index, Recorder::Event::kEnd) == 0);
        CHECK(f.recorder.count(index, Recorder::Event::kValue) == 0);
    }

    // 手势是一个参数接一个参数：开始、值变化、结束，不交叉；先后同参数表
    REQUIRE(f.recorder.events.size() == 3 * kPresetParamCount);
    for (std::size_t i = 0; i < kPresetParamCount; ++i)
    {
        const int index = f.indexOf(rinne::input::presetParamId(i));
        CHECK(f.recorder.events[3 * i].parameter == index);
        CHECK(f.recorder.events[3 * i].kind == Recorder::Event::kBegin);
        CHECK(f.recorder.events[3 * i + 1].parameter == index);
        CHECK(f.recorder.events[3 * i + 1].kind == Recorder::Event::kValue);
        CHECK(f.recorder.events[3 * i + 2].parameter == index);
        CHECK(f.recorder.events[3 * i + 2].kind == Recorder::Event::kEnd);
    }
}

TEST_CASE("load: every factory preset leaves loop_on, limiter and ceiling alone and ignores the current values",
          "[presets][load]")
{
    Fixture f;
    REQUIRE(f.store.factoryPresetsValid());
    for (const auto& preset : f.store.factoryPresets())
    {
        INFO("preset " << preset.id);
        // 每次都从一个与预设几乎处处不同的状态出发：结果只取决于预设，不取决于当前值
        f.makeSoundNonDefault();
        f.makeProjectNonDefault();
        const auto projectBefore = f.project();
        REQUIRE(f.store.load(preset.id, f.params.tree()).ok());
        for (std::size_t i = 0; i < kPresetParamCount; ++i)
        {
            INFO("参数 " << rinne::input::presetParamId(i));
            CHECK(f.raw(rinne::input::presetParamId(i)) == Approx(preset.values[i]).margin(0.002));
        }
        CHECK(f.project() == projectBefore);
    }
}

TEST_CASE("load: presetSeq goes up by exactly one per successful load and stays put on failure", "[presets][load]")
{
    Fixture f;
    REQUIRE(f.store.factoryPresetsValid());
    CHECK(f.store.presetSeq() == 0u);

    REQUIRE(f.store.load("f-init", f.params.tree()).ok());
    CHECK(f.store.presetSeq() == 1u);
    // 同一个预设再载一次也加一（序号是「又载入了一次」的通知）
    REQUIRE(f.store.load("f-init", f.params.tree()).ok());
    CHECK(f.store.presetSeq() == 2u);
    REQUIRE(f.store.load("f-edge-95", f.params.tree()).ok());
    CHECK(f.store.presetSeq() == 3u);

    // 失败：什么都不改，序号不变，也没有手势
    f.makeSoundNonDefault();
    const auto soundBefore = f.sound();
    f.recorder.events.clear();
    const std::uint32_t seqBefore = f.store.presetSeq();
    for (const char* id : {"f-no-such-preset", "u-000000000000", "", "../f-init", "u-ZZZZZZZZZZZZ", "F-INIT"})
    {
        INFO("id '" << id << "'");
        const auto result = f.store.load(id, f.params.tree());
        CHECK_FALSE(result.ok());
        CHECK(result.error == PresetError::kNotFound);
        CHECK(std::string(rinne::input::presetReason(result.error)) == "not-found");
        CHECK(f.store.presetSeq() == seqBefore);
    }
    CHECK(f.recorder.events.empty());
    CHECK(f.sound() == soundBefore);
}

namespace
{

// 参数通知里读到的 presetSeq：引擎每块先读序号、再读参数，序号先到才能让新参数落在降到 0 与淡入里
struct SeqProbe final : juce::AudioProcessorParameter::Listener
{
    explicit SeqProbe(const PresetStore& s) : store(s) {}
    void parameterValueChanged(int, float) override { seen.push_back(store.presetSeq()); }
    void parameterGestureChanged(int, bool) override {}
    const PresetStore& store;
    std::vector<std::uint32_t> seen;
};

} // namespace

TEST_CASE("load: presetSeq goes up before the first parameter is written", "[presets][load]")
{
    Fixture f;
    REQUIRE(f.store.factoryPresetsValid());

    // 第一个被写的参数（参数表里的第一个声音参数）的通知里，序号已经加过一；每次载入都是
    const std::string first = rinne::input::presetParamId(0);
    SeqProbe probe(f.store);
    f.parameter(first).addListener(&probe);
    REQUIRE(f.store.load("f-init", f.params.tree()).ok());
    REQUIRE(f.store.load("f-edge-95", f.params.tree()).ok());
    f.parameter(first).removeListener(&probe);
    REQUIRE(probe.seen.size() == 2u);
    CHECK(probe.seen[0] == 1u);
    CHECK(probe.seen[1] == 2u);

    // 8 个声音参数的通知里读到的序号都是加过之后的，没有哪个参数早于序号
    SeqProbe all(f.store);
    for (std::size_t i = 0; i < kPresetParamCount; ++i)
        f.parameter(rinne::input::presetParamId(i)).addListener(&all);
    REQUIRE(f.store.load("f-dub-quarter-dotted", f.params.tree()).ok());
    for (std::size_t i = 0; i < kPresetParamCount; ++i)
        f.parameter(rinne::input::presetParamId(i)).removeListener(&all);
    REQUIRE(all.seen.size() == kPresetParamCount);
    for (const std::uint32_t seq : all.seen)
        CHECK(seq == 3u);

    // 失败的载入没有任何通知，序号也不动
    SeqProbe none(f.store);
    f.parameter(first).addListener(&none);
    CHECK_FALSE(f.store.load("f-no-such-preset", f.params.tree()).ok());
    f.parameter(first).removeListener(&none);
    CHECK(none.seen.empty());
    CHECK(f.store.presetSeq() == 3u);
}

TEST_CASE("load: a tree missing a sound parameter is refused before anything changes", "[presets][load]")
{
    // 少 delay_note 的参数树（接线错误）：载入返回 not-ready，不改任何参数，序号不变
    ParamsTestProcessor processor;
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    for (std::size_t i = 0; i < rinne::input::kParamCount; ++i)
    {
        auto parameter = rinne::input::makeInputParameter(i);
        if (parameter->getParameterID() != "delay_note")
            layout.add(std::move(parameter));
    }
    juce::AudioProcessorValueTreeState tree(processor, nullptr, "PARAMS", std::move(layout));

    PresetStore store{juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("rinne-presets-unused")};
    REQUIRE(store.factoryPresetsValid());
    const float feedbackBefore = tree.getRawParameterValue("feedback")->load();
    const auto result = store.load("f-long-hold-92", tree);
    CHECK_FALSE(result.ok());
    CHECK(result.error == PresetError::kNotReady);
    CHECK(std::string(rinne::input::presetReason(result.error)) == "not-ready");
    CHECK(tree.getRawParameterValue("feedback")->load() == feedbackBefore);
    CHECK(store.presetSeq() == 0u);
}

TEST_CASE("load: readValues reads the same plain values the tree holds", "[presets][load]")
{
    Fixture f;
    f.makeSoundNonDefault();
    const auto values = PresetStore::readValues(f.params.tree());
    CHECK(values == f.sound());
    CHECK(values[0] == Approx(99.0f));
    CHECK(values[2] == 1.0f);
    CHECK(values[4] == 3.0f);
    CHECK(values[7] == 1.0f);
}

TEST_CASE("presetSeq is a lock-free atomic the audio thread may read", "[presets][load]")
{
    PresetStore store{juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("rinne-presets-unused")};
    static_assert(noexcept(store.presetSeq()), "音频线程读序号不能抛");
    CHECK(store.presetSeq() == 0u);
}
