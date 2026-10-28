// SPDX-License-Identifier: GPL-3.0-or-later
//
// 工厂预设（presets/factory/*.json，经 BinaryData 嵌进二进制）：8 个全部能解析并通过校验、内容与设计表一致，
// 校验器对各种坏样例都拒收。
//
// 内容表在这里按设计另写一遍（独立于 JSON 文件）：任何一个预设的值被改动，对拍就变红。
// 档位参数写的是档位下标（明文值口径，与参数契约、工程 state 一致）；1/4D、1/8 与 Sync 的下标从参数表的档位行查，
// 不手写数字。

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <juce_core/juce_core.h>

#include <RinnePresetData.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../../src/input/PresetStore.h"
#include "core/params/ParamsGolden.h"

namespace
{

using namespace rinne::test::params;
using rinne::input::FactoryPreset;
using rinne::input::kFactoryPresetCount;
using rinne::input::kPresetParamCount;
using rinne::input::parseFactoryPreset;
using rinne::input::PresetStore;
using rinne::input::PresetValues;

// 参数表里某个档位参数、某段文字对应的下标
int choiceIndex(const std::string& param, const std::string& label)
{
    const std::vector<std::string> labels = choicesOf(param);
    for (std::size_t i = 0; i < labels.size(); ++i)
    {
        if (labels[i] == label)
            return static_cast<int>(i);
    }
    FAIL("no choice " << label << " on " << param);
    return -1;
}

// 参数表里这个 ID 那一行
const Param& paramRow(const std::string& id)
{
    for (const Param& p : table())
    {
        if (id == p.id)
            return p;
    }
    FAIL("no parameter " << id);
    return table().front();
}

struct Expected
{
    const char* id;
    const char* zh;
    const char* en;
    const char* fr;
    double feedback;
    double mix;
    int delayMode; // 0 Time、1 Sync
    double delayMs;
    int delayNote;
};

// 设计表。其余三个参数（low_cut、high_cut、polarity）所有预设都取参数表默认值
std::array<Expected, kFactoryPresetCount> designTable()
{
    const int sync = choiceIndex("delay_mode", "Sync");
    const int note4d = choiceIndex("delay_note", "1/4D");
    const int note8 = choiceIndex("delay_note", "1/8");
    const int note4 = choiceIndex("delay_note", "1/4"); // 默认档
    return {{
        {"f-init", "Init", "Init", "Initialisation", 0, 25, 0, 100, note4},
        {"f-slapback-90", "Slapback 90 ms", "Slapback 90 ms", "Slapback 90 ms", 50, 25, 0, 90, note4},
        {"f-dub-quarter-dotted", "Dub 1/4D", "Dub 1/4D", "Dub 1/4D", 65, 50, sync, 100, note4d},
        {"f-ping-pong-eighth", "Ping-Pong 1/8", "Ping-Pong 1/8", "Ping-pong 1/8", 60, 25, sync, 100, note8},
        {"f-shortest-comb", "Shortest Comb", "Shortest Comb", "Peigne minimal", 80, 50, 0, 1, note4},
        {"f-long-hold-92", "Long Hold 92 %", "Long Hold 92 %", "Maintien long 92 %", 92, 50, 0, 100, note4},
        {"f-edge-95", "Edge 95 %", "Edge 95 %", "Bord d'oscillation 95 %", 95, 50, 0, 100, note4},
        {"f-light-mix-30", "Light Mix 30 %", "Light Mix 30 %", "M\xC3\xA9lange l\xC3\xA9ger 30 %", 75, 30, 0, 100,
         note4},
    }};
}

juce::String utf8(const char* text)
{
    return juce::String::fromUTF8(text);
}

// 嵌进二进制的第 i 份原文（按原始文件名排序，与 PresetStore 的次序相同）
std::vector<std::pair<juce::String, juce::String>> embeddedFiles()
{
    std::vector<std::pair<juce::String, juce::String>> files;
    for (int i = 0; i < RinnePresetData::namedResourceListSize; ++i)
    {
        int size = 0;
        const char* data = RinnePresetData::getNamedResource(RinnePresetData::namedResourceList[i], size);
        REQUIRE(data != nullptr);
        files.emplace_back(juce::String(RinnePresetData::originalFilenames[i]), juce::String::fromUTF8(data, size));
    }
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return files;
}

// 把原文里的 from 恰好换成 to 一次；找不到或不止一处都是用例写错了
juce::String replaced(const juce::String& text, const juce::String& from, const juce::String& to)
{
    REQUIRE(text.indexOf(from) >= 0);
    REQUIRE(text.indexOf(text.indexOf(from) + 1, from) < 0);
    return text.replace(from, to);
}

} // namespace

TEST_CASE("factory presets: all 8 parse, pass validation and match the design table", "[presets][factory]")
{
    const juce::File unused =
        juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("rinne-presets-unused");
    const PresetStore store(unused);
    INFO("factory problem: " << store.factoryProblem());
    REQUIRE(store.factoryPresetsValid());
    const std::vector<FactoryPreset>& presets = store.factoryPresets();
    REQUIRE(presets.size() == kFactoryPresetCount);

    const auto design = designTable();
    const int note4 = choiceIndex("delay_note", "1/4");
    for (std::size_t n = 0; n < presets.size(); ++n)
    {
        const FactoryPreset& preset = presets[n];
        const Expected& want = design[n];
        INFO("preset " << want.id);
        // 先后、id、三语名；zh 与 en 相同
        CHECK(preset.id == juce::String(want.id));
        CHECK(preset.zh == utf8(want.zh));
        CHECK(preset.en == utf8(want.en));
        CHECK(preset.fr == utf8(want.fr));
        CHECK(preset.zh == preset.en);
        CHECK(preset.zh.isNotEmpty());
        CHECK(preset.fr.isNotEmpty());
        // 内容
        CHECK(preset.values[0] == static_cast<float>(want.feedback));
        CHECK(preset.values[1] == static_cast<float>(want.mix));
        CHECK(preset.values[2] == static_cast<float>(want.delayMode));
        CHECK(preset.values[3] == static_cast<float>(want.delayMs));
        CHECK(preset.values[4] == static_cast<float>(want.delayNote));
        CHECK(preset.values[5] == static_cast<float>(paramRow("low_cut").def));
        CHECK(preset.values[6] == static_cast<float>(paramRow("high_cut").def));
        CHECK(preset.values[7] == static_cast<float>(paramRow("polarity").def));
        // 反馈不超过 100 %：每个预设都不需要限制器兜底
        CHECK(preset.values[0] <= 100.0f);
        // 每个值都在参数表的范围里
        for (std::size_t i = 0; i < kPresetParamCount; ++i)
        {
            const Param& row = paramRow(rinne::input::presetParamId(i));
            INFO("参数 " << row.id);
            CHECK(static_cast<double>(preset.values[i]) >= row.lo);
            CHECK(static_cast<double>(preset.values[i]) <= row.hi);
        }
    }

    // 1/4D 与 1/8 的下标与参数表一致，Sync 是 delay_mode 的第二档；非同步的预设都用默认时值
    CHECK(presets[2].values[4] == static_cast<float>(choiceIndex("delay_note", "1/4D")));
    CHECK(presets[3].values[4] == static_cast<float>(choiceIndex("delay_note", "1/8")));
    CHECK(presets[2].values[2] == static_cast<float>(choiceIndex("delay_mode", "Sync")));
    CHECK(presets[3].values[2] == static_cast<float>(choiceIndex("delay_mode", "Sync")));
    CHECK(presets[0].values[4] == static_cast<float>(note4));

    // 第 1 个预设逐项等于参数表的默认值
    for (std::size_t i = 0; i < kPresetParamCount; ++i)
    {
        const Param& row = paramRow(rinne::input::presetParamId(i));
        INFO("参数 " << row.id);
        CHECK(static_cast<double>(presets[0].values[i]) == row.def);
    }
    CHECK(presets[0].values == rinne::input::presetDefaults());

    // id 互不相同
    for (std::size_t a = 0; a < presets.size(); ++a)
    {
        for (std::size_t b = a + 1; b < presets.size(); ++b)
            CHECK(presets[a].id != presets[b].id);
    }
}

TEST_CASE("factory presets: the embedded files carry exactly the 8 sound parameters, one per line",
          "[presets][factory]")
{
    const auto files = embeddedFiles();
    REQUIRE(files.size() == kFactoryPresetCount);
    for (const auto& [name, text] : files)
    {
        INFO("file " << name);
        juce::var root;
        REQUIRE(juce::JSON::parse(text, root).wasOk());
        const juce::DynamicObject* params = root.getProperty("params", {}).getDynamicObject();
        REQUIRE(params != nullptr);
        // 键恰好是 8 个声音参数，按参数表序号；没有 loop_on、limiter、ceiling，也没有组号之类的别的键
        REQUIRE(params->getProperties().size() == static_cast<int>(kPresetParamCount));
        for (int i = 0; i < params->getProperties().size(); ++i)
            CHECK(params->getProperties().getName(i).toString() ==
                  juce::String(rinne::input::presetParamId(static_cast<std::size_t>(i))));
        for (const char* forbidden : {"loop_on", "limiter", "ceiling", "group"})
            CHECK_FALSE(params->hasProperty(forbidden));
        // 每个参数占一行（可逐行 diff）：行首是缩进加键名
        for (std::size_t i = 0; i < kPresetParamCount; ++i)
            CHECK(text.contains("\n    \"" + juce::String(rinne::input::presetParamId(i)) + "\": "));
        // 顶层键与三语名
        CHECK(text.startsWith("{\n  \"schema\": 1,\n  \"id\": \"f-"));
        CHECK(text.contains("\n  \"name\": {\n    \"zh\": "));
        CHECK(text.endsWith("}\n"));
    }
}

TEST_CASE("factory presets: the validator accepts the shipped files and rejects every kind of damage",
          "[presets][factory]")
{
    const auto files = embeddedFiles();
    REQUIRE(files.size() == kFactoryPresetCount);
    // 以第 3 个（Sync、1/4D、反馈 65）为底，逐处弄坏
    const juce::String good = files[2].second;
    {
        juce::String problem;
        const auto parsed = parseFactoryPreset(good, &problem);
        INFO(problem);
        REQUIRE(parsed.has_value());
        CHECK(problem.isEmpty());
    }

    struct Damage
    {
        const char* what;
        juce::String text;
    };
    const std::vector<Damage> damages = {
        {"not JSON", "feedback = 65"},
        {"top level is an array", "[1, 2, 3]"},
        {"truncated file", good.substring(0, good.length() / 2)},
        {"schema 2", replaced(good, "\"schema\": 1", "\"schema\": 2")},
        {"schema as text", replaced(good, "\"schema\": 1", "\"schema\": \"1\"")},
        {"user-shaped id", replaced(good, "\"f-dub-quarter-dotted\"", "\"u-0123456789ab\"")},
        {"upper-case id", replaced(good, "\"f-dub-quarter-dotted\"", "\"f-Dub\"")},
        {"id with a trailing dash", replaced(good, "\"f-dub-quarter-dotted\"", "\"f-dub-\"")},
        {"missing fr name", replaced(good, "    \"fr\": \"Dub 1/4D\"\n", "")},
        {"empty zh name", replaced(good, "\"zh\": \"Dub 1/4D\"", "\"zh\": \"\"")},
        {"fr name is a number", replaced(good, "\"fr\": \"Dub 1/4D\"", "\"fr\": 5")},
        {"extra language in name",
         replaced(good, "    \"fr\": \"Dub 1/4D\"\n", "    \"fr\": \"Dub 1/4D\",\n    \"de\": \"Dub 1/4D\"\n")},
        {"name languages out of order", replaced(good, "    \"zh\": \"Dub 1/4D\",\n    \"en\": \"Dub 1/4D\",\n",
                                                 "    \"en\": \"Dub 1/4D\",\n    \"zh\": \"Dub 1/4D\",\n")},
        {"forbidden key loop_on",
         replaced(good, "    \"polarity\": 0\n", "    \"polarity\": 0,\n    \"loop_on\": 1\n")},
        {"forbidden key limiter",
         replaced(good, "    \"polarity\": 0\n", "    \"polarity\": 0,\n    \"limiter\": 1\n")},
        {"forbidden key ceiling",
         replaced(good, "    \"polarity\": 0\n", "    \"polarity\": 0,\n    \"ceiling\": -6\n")},
        {"unknown extra key", replaced(good, "    \"polarity\": 0\n", "    \"polarity\": 0,\n    \"group\": 3\n")},
        {"missing parameter", replaced(good, "    \"high_cut\": 20000,\n", "")},
        {"parameters out of table order", replaced(good, "    \"low_cut\": 20,\n    \"high_cut\": 20000,\n",
                                                   "    \"high_cut\": 20000,\n    \"low_cut\": 20,\n")},
        {"feedback above range", replaced(good, "\"feedback\": 65", "\"feedback\": 151")},
        {"mix below range", replaced(good, "\"mix\": 50", "\"mix\": -1")},
        {"delay_ms above range", replaced(good, "\"delay_ms\": 100", "\"delay_ms\": 4001")},
        {"delay_note above range", replaced(good, "\"delay_note\": 12", "\"delay_note\": 16")},
        {"delay_mode not a whole number", replaced(good, "\"delay_mode\": 1", "\"delay_mode\": 0.5")},
        {"parameter as text", replaced(good, "\"feedback\": 65", "\"feedback\": \"65\"")},
        {"parameter as a boolean", replaced(good, "\"polarity\": 0", "\"polarity\": false")},
        {"parameter as null", replaced(good, "\"mix\": 50", "\"mix\": null")},
        {"parameter overflowing to infinity", replaced(good, "\"feedback\": 65", "\"feedback\": 1e999")},
        {"top-level key added", replaced(good, "  \"params\": {", "  \"loop_on\": 1,\n  \"params\": {")},
    };
    for (const Damage& damage : damages)
    {
        INFO("damage: " << damage.what);
        juce::String problem;
        const auto parsed = parseFactoryPreset(damage.text, &problem);
        CHECK_FALSE(parsed.has_value());
        CHECK(problem.isNotEmpty());
    }
}

TEST_CASE("factory presets: parameter ids and bounds in the store come straight from the parameter table",
          "[presets][factory]")
{
    // 声音参数 = 参数表里除 loop_on、limiter、ceiling 之外的 8 个，先后同表
    std::vector<std::string> sound;
    for (const Param& p : table())
    {
        const std::string id = p.id;
        if (id != "loop_on" && id != "limiter" && id != "ceiling")
            sound.push_back(id);
    }
    REQUIRE(sound.size() == kPresetParamCount);
    for (std::size_t i = 0; i < kPresetParamCount; ++i)
        CHECK(sound[i] == rinne::input::presetParamId(i));
    CHECK(rinne::input::presetParamId(kPresetParamCount) == nullptr);

    // 夹值：端点内原样，越界夹回，开关与档位取整，NaN 取默认
    for (std::size_t i = 0; i < kPresetParamCount; ++i)
    {
        const Param& row = paramRow(sound[i]);
        INFO("参数 " << row.id);
        CHECK(static_cast<double>(rinne::input::clampPresetValue(i, row.lo - 1000.0)) == row.lo);
        CHECK(static_cast<double>(rinne::input::clampPresetValue(i, row.hi + 1000.0)) == row.hi);
        CHECK(static_cast<double>(rinne::input::clampPresetValue(i, std::numeric_limits<double>::infinity())) ==
              row.hi);
        CHECK(static_cast<double>(rinne::input::clampPresetValue(i, -std::numeric_limits<double>::infinity())) ==
              row.lo);
        CHECK(static_cast<double>(rinne::input::clampPresetValue(i, std::nan(""))) == row.def);
        CHECK(static_cast<double>(rinne::input::clampPresetValue(i, row.def)) == row.def);
    }
    CHECK(rinne::input::clampPresetValue(2, 0.4) == 0.0f); // delay_mode 取整
    CHECK(rinne::input::clampPresetValue(2, 0.6) == 1.0f);
    CHECK(rinne::input::clampPresetValue(4, 12.6) == 13.0f); // delay_note 取整
    CHECK(rinne::input::clampPresetValue(7, 0.5) == 1.0f); // polarity
}
