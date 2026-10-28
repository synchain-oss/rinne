// SPDX-License-Identifier: GPL-3.0-or-later
//
// 宿主参数 ID 的实测：照参数表（src/input/ParamTable.h）真建 JUCE 的参数对象，挂进名为 PARAMS 的参数树，
// 用 JUCE 自己的函数取宿主看到的 ID，再与 tests/golden/params-input.json 的 vst3 / au 两列对拍。
// 宿主在工程里按这个数记自动化：它一变，用户已存的工程与自动化就对不上。
//
// JUCE 8.0.8 的两个包装层按两个开关算宿主 ID：JUCE_FORCE_USE_LEGACY_PARAM_IDS 为 1 时一律用参数下标；为 0 时
//   · 先取参数的 ID 字符串：参数派生自 HostedAudioProcessorParameter 时是 getParameterID()，否则退成参数下标；
//   · VST3：VST3ClientExtensions::convertJuceParameterId(ID, 开关)，即 String::hashCode()，
//     JUCE_USE_STUDIO_ONE_COMPATIBLE_PARAMETERS 为 1 时再清最高位；
//   · AU：同一个 hashCode()，同一个开关决定清不清最高位（AU 包装层里的 generateAUParameterID，不对外导出，这里按它的
//     式子算）。
// 两个开关的值从 tests/golden/identity.json 读（见下文 HostIdSwitches），用例先钉住它们是 0 与 1。
// 本文件钉住：ID 字符串就是参数表里的 id，不带 versionHint；versionHint 是 1；两列都等于 golden；
// 同一个 id 换个 versionHint，宿主 ID 不变（versionHint 不进散列）；AU 列参数的先后按 versionHint 再按宿主 ID 排。
// 参数对象的刻度与步进不影响 ID，这里一律用线性范围；完整的参数树布局与刻度由参数层自己的用例核对。
// 不链 JUCE 的另一条路径（纯 C++ 复算散列）在 tests/core/params 里，不编译的一条是 contract-static-params：三条要一致。

#include <catch2/catch_test_macros.hpp>

#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/params/ParamsGolden.h"

namespace
{

using namespace rinne::test::params;

constexpr int kVersionHint = 1;

// 插件构建里决定宿主参数 ID 的两个开关。真源是根 CMakeLists.txt 里只挂在两个插件目标上的那份宏定义，
// tests/golden/identity.json 记着它们的值，contract-static-identity 核对两处一致、且两个宏在全部 CMake 文件里只出现
// 这一次。所以本目标不另设这两个宏（另设一份就成了第二个真源，那条检查也会拦），而是从 identity.json 读值，
// 交给 JUCE 的运行期函数，按两个包装层的写法逐支算。
struct HostIdSwitches
{
    std::optional<bool> legacyIds; // JUCE_FORCE_USE_LEGACY_PARAM_IDS
    std::optional<bool> studioOne; // JUCE_USE_STUDIO_ONE_COMPATIBLE_PARAMETERS
};

// identity.json 里 "键": "0" 或 "1" 的一项；找不到或不是这两个值时为空
std::optional<bool> definitionFlag(const std::string& text, const std::string& key)
{
    const std::string needle = "\"" + key + "\": \"";
    const auto at = text.find(needle);
    if (at == std::string::npos)
    {
        return std::nullopt;
    }
    const auto value = at + needle.size();
    if (value + 1 >= text.size() || text[value + 1] != '"')
    {
        return std::nullopt;
    }
    return text[value] == '1'   ? std::optional<bool>(true)
           : text[value] == '0' ? std::optional<bool>(false)
                                : std::nullopt;
}

const HostIdSwitches& switches()
{
    static const HostIdSwitches loaded = [] {
        std::ifstream in(goldenPath().parent_path() / "identity.json", std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return HostIdSwitches{definitionFlag(text, "JUCE_FORCE_USE_LEGACY_PARAM_IDS"),
                              definitionFlag(text, "JUCE_USE_STUDIO_ONE_COMPATIBLE_PARAMETERS")};
    }();
    return loaded;
}

// 用到宿主 ID 的用例先调它：两个开关都读得出来才往下算
const HostIdSwitches& requireSwitches()
{
    INFO("tests/golden/identity.json 的 definitions 里要有两个开关，值是 \"0\" 或 \"1\"");
    REQUIRE(switches().legacyIds.has_value());
    REQUIRE(switches().studioOne.has_value());
    return switches();
}

// 最小的处理器：只为承载参数树
struct ParamsTestProcessor final : juce::AudioProcessor
{
    ParamsTestProcessor()
        : juce::AudioProcessor(BusesProperties()
                                   .withInput("In", juce::AudioChannelSet::stereo(), true)
                                   .withOutput("Out", juce::AudioChannelSet::stereo(), true))
    {
    }

    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    const juce::String getName() const override { return "ParamsTest"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}
};

// 参数表的一行建成对应的 JUCE 参数对象
std::unique_ptr<juce::RangedAudioParameter> makeParameter(const Param& p, int versionHint = kVersionHint)
{
    const juce::ParameterID id{p.id, versionHint};
    if (p.kind == 'B')
    {
        return std::make_unique<juce::AudioParameterBool>(
            id, p.name, p.def != 0.0, juce::AudioParameterBoolAttributes().withAutomatable(p.automatable));
    }
    if (p.kind == 'C')
    {
        juce::StringArray labels;
        for (const auto& label : choicesOf(p.id))
        {
            labels.add(label);
        }
        return std::make_unique<juce::AudioParameterChoice>(
            id, p.name, labels, static_cast<int>(p.def),
            juce::AudioParameterChoiceAttributes().withAutomatable(p.automatable));
    }
    return std::make_unique<juce::AudioParameterFloat>(
        id, p.name,
        juce::NormalisableRange<float>(static_cast<float>(p.lo), static_cast<float>(p.hi), static_cast<float>(p.step)),
        static_cast<float>(p.def), juce::AudioParameterFloatAttributes().withAutomatable(p.automatable));
}

juce::AudioProcessorValueTreeState::ParameterLayout makeLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;
    for (const auto& p : table())
    {
        layout.add(makeParameter(p));
    }
    return layout;
}

struct LayoutUnderTest
{
    ParamsTestProcessor processor;
    juce::AudioProcessorValueTreeState tree{processor, nullptr, "PARAMS", makeLayout()};
};

// 包装层取 ID 字符串的那一步：派生自 HostedAudioProcessorParameter 的取 getParameterID()，否则退成下标
juce::String wrapperParamId(const juce::AudioProcessorParameter& param)
{
    if (const auto* hosted = dynamic_cast<const juce::HostedAudioProcessorParameter*>(&param))
    {
        return hosted->getParameterID();
    }
    return juce::String(param.getParameterIndex());
}

// VST3 包装层（generateVSTParamIDForParam 与它的调用处）：强制旧式 ID 时是参数下标，否则散列 ID 字符串
std::uint32_t vst3Id(const juce::AudioProcessorParameter& param, const HostIdSwitches& s)
{
    if (*s.legacyIds)
    {
        return static_cast<std::uint32_t>(param.getParameterIndex());
    }
    return juce::VST3ClientExtensions::convertJuceParameterId(wrapperParamId(param), *s.studioOne);
}

// AU 包装层（generateAUParameterID）：强制旧式 ID 时是参数下标，否则散列 ID 字符串，再按开关清最高位
std::uint32_t auId(const juce::AudioProcessorParameter& param, const HostIdSwitches& s)
{
    if (*s.legacyIds)
    {
        return static_cast<std::uint32_t>(param.getParameterIndex());
    }
    auto id = static_cast<std::uint32_t>(wrapperParamId(param).hashCode());
    if (*s.studioOne)
    {
        id &= ~(std::uint32_t{1} << 31);
    }
    return id;
}

// AU 宿主看到的参数列表顺序：先按宿主 ID 排，再按 versionHint 稳定排序（JUCE 的 AU 包装层就是这两步）
std::vector<std::string> auListOrder(const juce::Array<juce::AudioProcessorParameter*>& params, const HostIdSwitches& s)
{
    struct Entry
    {
        std::uint32_t id;
        int versionHint;
        std::string paramId;
    };
    std::vector<Entry> entries;
    for (const auto* param : params)
    {
        entries.push_back({auId(*param, s), param->getVersionHint(), wrapperParamId(*param).toStdString()});
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.id < b.id; });
    std::stable_sort(entries.begin(), entries.end(),
                     [](const Entry& a, const Entry& b) { return a.versionHint < b.versionHint; });
    std::vector<std::string> order;
    for (const auto& e : entries)
    {
        order.push_back(e.paramId);
    }
    return order;
}

} // namespace

TEST_CASE("juce parameters built from the table carry the bare id and version hint 1", "[params][host-id]")
{
    LayoutUnderTest layout;
    const auto& params = layout.processor.getParameters();
    REQUIRE(params.size() == static_cast<int>(table().size()));
    for (std::size_t i = 0; i < table().size(); ++i)
    {
        const Param& p = table()[i];
        INFO("参数 " << p.id);
        const auto* param = params[static_cast<int>(i)];
        REQUIRE(param != nullptr);
        // 宿主看到的先后就是表里的序号
        CHECK(param->getParameterIndex() == p.index);
        // 包装层走的是 getParameterID()，不是退成下标的那一支
        REQUIRE(dynamic_cast<const juce::HostedAudioProcessorParameter*>(param) != nullptr);
        // ID 字符串就是表里的 id：versionHint 另存，不拼进字符串
        CHECK(wrapperParamId(*param) == juce::String(p.id));
        CHECK(param->getVersionHint() == kVersionHint);
        CHECK(param->isAutomatable() == p.automatable);
        CHECK(param->getName(64) == juce::String(p.name));
        if (p.kind == 'C')
        {
            CHECK(param->getNumSteps() == static_cast<int>(choicesOf(p.id).size()));
        }
    }
}

TEST_CASE("a parameter tree named PARAMS finds every parameter by its bare id", "[params][host-id]")
{
    LayoutUnderTest layout;
    CHECK(layout.tree.state.getType() == juce::Identifier("PARAMS"));
    for (const auto& p : table())
    {
        INFO("参数 " << p.id);
        CHECK(layout.tree.getParameter(p.id) != nullptr);
        CHECK(layout.tree.getRawParameterValue(p.id) != nullptr);
        // 参数树里记的也是不带 versionHint 的 id
        CHECK(layout.tree.state.getChildWithProperty("id", juce::String(p.id)).isValid());
        CHECK(layout.tree.getParameter(juce::String(p.id) + ",1") == nullptr);
    }
}

TEST_CASE("the plugin build hashes the id string and clears the top bit", "[params][host-id]")
{
    const HostIdSwitches& s = requireSwitches();
    // 改成 1 就换成参数下标：宿主按下标记自动化，参数一追加或调序，已存的工程与自动化全部对不上
    CHECK_FALSE(*s.legacyIds);
    // 改成 0 会让最高位为 1 的那几个 ID（feedback 等）全部变掉
    CHECK(*s.studioOne);
}

TEST_CASE("host ids computed by juce equal the golden columns", "[params][host-id][golden]")
{
    const HostIdSwitches& s = requireSwitches();
    LayoutUnderTest layout;
    const Golden golden = loadGolden();
    REQUIRE(golden.loaded);
    std::set<std::uint32_t> vst3Seen;
    std::set<std::uint32_t> auSeen;
    for (const auto* param : layout.processor.getParameters())
    {
        const std::string id = wrapperParamId(*param).toStdString();
        INFO("参数 " << id);
        const Line* row = golden.row(id);
        REQUIRE(row != nullptr);
        const std::uint32_t vst3 = vst3Id(*param, s);
        const std::uint32_t au = auId(*param, s);
        CHECK(row->num("vst3") == std::optional<double>(static_cast<double>(vst3)));
        CHECK(row->num("au") == std::optional<double>(static_cast<double>(au)));
        // 不链 JUCE 的那条复算路径给出同一个数
        CHECK(vst3 == hostParamId(id));
        CHECK(au == hostParamId(id));
        CHECK(vst3 < 0x80000000u);
        CHECK(vst3Seen.insert(vst3).second);
        CHECK(auSeen.insert(au).second);
    }
    CHECK(vst3Seen.size() == table().size());
}

TEST_CASE("the version hint does not enter the host id", "[params][host-id]")
{
    const HostIdSwitches& s = requireSwitches();
    for (const auto& p : table())
    {
        INFO("参数 " << p.id);
        const auto one = makeParameter(p, 1);
        const auto two = makeParameter(p, 2);
        CHECK(wrapperParamId(*one) == wrapperParamId(*two));
        CHECK(vst3Id(*one, s) == vst3Id(*two, s));
        CHECK(auId(*one, s) == auId(*two, s));
        CHECK(one->getVersionHint() == 1);
        CHECK(two->getVersionHint() == 2);
    }
    // 拼进 versionHint 的字符串会得到另一个数：上面的等式不是空转
    CHECK(juce::VST3ClientExtensions::convertJuceParameterId("feedback,1", true) != hostParamId("feedback"));
}

TEST_CASE("au lists the parameters by version hint and then by host id", "[params][host-id][au]")
{
    const HostIdSwitches& s = requireSwitches();
    LayoutUnderTest layout;
    // 契约文档写明的 AU 列表顺序：11 个都是 versionHint 1，所以只按宿主 ID 升序，与序号无关
    const std::vector<std::string> expected = {"mix",      "limiter",    "loop_on",    "low_cut",
                                               "high_cut", "delay_mode", "delay_note", "polarity",
                                               "ceiling",  "delay_ms",   "feedback"};
    CHECK(auListOrder(layout.processor.getParameters(), s) == expected);

    // 以后追加一个参数：它的宿主 ID 比原有 11 个都小（"x" 的散列是 120）。versionHint 也取 1 就插到最前面，
    // 原有参数在 AU 列表里的位置全部后移一格；取 2 才排在原有 11 个后面，原有的先后不变
    const auto grownOrder = [&s](int versionHint) {
        ParamsTestProcessor processor;
        juce::AudioProcessorValueTreeState::ParameterLayout grown = makeLayout();
        grown.add(std::make_unique<juce::AudioParameterBool>(juce::ParameterID{"x", versionHint}, "Added", false));
        juce::AudioProcessorValueTreeState tree{processor, nullptr, "PARAMS", std::move(grown)};
        return auListOrder(processor.getParameters(), s);
    };
    REQUIRE(hostParamId("x") == 120u);
    auto appended = expected;
    appended.push_back("x");
    CHECK(grownOrder(2) == appended);
    auto inserted = expected;
    inserted.insert(inserted.begin(), "x");
    CHECK(grownOrder(1) == inserted);
}
