// SPDX-License-Identifier: GPL-3.0-or-later
//
// 用户预设（PresetStore 的保存、列表、覆盖、删除、载入）：名字规则、重名、文件格式、坏文件、写失败。
// 全程把用户目录指向本测试独占的临时目录（构造参数注入），不碰用户的真实目录。
// 写失败用两种办法造：目录路径被一个普通文件挡住（真实的失败），以及注入「替换目标」这一步失败（验证清理）。

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "../../src/input/InputParams.h"
#include "../../src/input/PresetStore.h"
#include "params/ParamsTestProcessor.h"

namespace
{

using namespace rinne::test::params;
using Catch::Approx;
using rinne::input::isUserPresetId;
using rinne::input::kPresetParamCount;
using rinne::input::normalisePresetName;
using rinne::input::PresetError;
using rinne::input::PresetStore;
using rinne::input::PresetValues;
using rinne::input::UserPreset;

constexpr const char* kScratchPrefix = "rinne-presets-test";

// 只有「系统临时目录的直接子目录、名字以本测试的前缀开头」才算本测试自己的临时目录。纯函数，清理前核对
bool ownsScratch(const juce::File& dir)
{
    const juce::File temp = juce::File::getSpecialLocation(juce::File::tempDirectory);
    return dir != juce::File() && temp != juce::File() && dir.getParentDirectory() == temp &&
           dir.getFileName().startsWith(kScratchPrefix);
}

// 系统临时目录下本测试独占的一个目录。清理只在核对过归属之后做，逐层删普通文件再删空目录，不用递归删除接口
class ScratchDir
{
public:
    ScratchDir()
    {
        root_ = juce::File::getSpecialLocation(juce::File::tempDirectory)
                    .getNonexistentChildFile(kScratchPrefix, "", false);
        REQUIRE(root_.createDirectory().wasOk());
    }

    ~ScratchDir()
    {
        if (!ownsScratch(root_))
            return;
        std::vector<juce::File> dirs{root_};
        for (std::size_t i = 0; i < dirs.size() && dirs.size() < 64; ++i)
        {
            juce::Array<juce::File> entries;
            dirs[i].findChildFiles(entries, juce::File::findFilesAndDirectories, false);
            for (const juce::File& entry : entries)
            {
                if (entry.isDirectory())
                    dirs.push_back(entry);
                else
                    entry.deleteFile();
            }
        }
        for (auto it = dirs.rbegin(); it != dirs.rend(); ++it)
            it->deleteFile(); // 此时都是空目录，最深的先删
    }

    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;

    const juce::File& root() const { return root_; }

private:
    juce::File root_;
};

// 目录里的全部文件名，排好序
std::vector<juce::String> namesIn(const juce::File& dir)
{
    juce::Array<juce::File> entries;
    dir.findChildFiles(entries, juce::File::findFilesAndDirectories, false);
    std::vector<juce::String> names;
    for (const juce::File& e : entries)
        names.push_back(e.getFileName());
    std::sort(names.begin(), names.end());
    return names;
}

juce::String repeat(const juce::String& unit, int times)
{
    juce::String out;
    for (int i = 0; i < times; ++i)
        out += unit;
    return out;
}

const PresetValues kSample{{40.0f, 35.5f, 1.0f, 250.125f, 7.0f, 120.0f, 8000.0f, 1.0f}};
const PresetValues kOther{{80.0f, 60.0f, 0.0f, 1.0f, 15.0f, 20.0f, 20000.0f, 0.0f}};

struct Env
{
    ScratchDir scratch;
    juce::File dir = scratch.root().getChildFile("Presets");
    PresetStore store{dir};
};

juce::File fileOf(const juce::File& dir, const juce::String& id)
{
    return dir.getChildFile(id + ".rinnepreset");
}

// 一个手写的用户预设文件（params 的内容原样放进去）
juce::String handWritten(const juce::String& id, const juce::String& name, const juce::String& params,
                         const juce::String& schema = "1")
{
    return "{\"schema\": " + schema + ", \"id\": \"" + id + "\", \"name\": \"" + name + "\", \"params\": {" + params +
           "}}";
}

} // namespace

TEST_CASE("scratch guard: only a direct child of the temp directory with the test prefix may be cleaned",
          "[presets][user]")
{
    const juce::File temp = juce::File::getSpecialLocation(juce::File::tempDirectory);
    CHECK_FALSE(ownsScratch(juce::File()));
    CHECK_FALSE(ownsScratch(temp));
    CHECK_FALSE(ownsScratch(temp.getParentDirectory()));
    CHECK_FALSE(ownsScratch(juce::File::getSpecialLocation(juce::File::userHomeDirectory)));
    CHECK_FALSE(ownsScratch(juce::File::getCurrentWorkingDirectory()));
    CHECK_FALSE(ownsScratch(temp.getChildFile("somebody-else")));
    CHECK_FALSE(ownsScratch(temp.getChildFile(kScratchPrefix).getChildFile("nested")));
    CHECK(ownsScratch(temp.getChildFile(kScratchPrefix)));
    CHECK(ownsScratch(temp.getChildFile(juce::String(kScratchPrefix) + "2")));
}

TEST_CASE("names: control characters and edge whitespace go, 24 code points stay, visible characters are allowed",
          "[presets][user][names]")
{
    CHECK(normalisePresetName("Echo") == "Echo");
    CHECK(normalisePresetName("  Echo  ") == "Echo");
    // C0、DEL、C1 控制字符直接删掉（不换成空格）
    CHECK(normalisePresetName(juce::String::fromUTF8("a\x01\x02"
                                                     "b\x7f"
                                                     "c")) == "abc");
    CHECK(normalisePresetName(juce::String::fromUTF8("x\xC2\x85y\xC2\x9Fz")) == "xyz");
    CHECK(normalisePresetName(juce::String::fromUTF8("\tEcho\n")) == "Echo");
    CHECK(normalisePresetName(juce::String::fromUTF8("Ec\tho")) == "Echo");
    // 不间断空格与全角空格也算空白
    CHECK(normalisePresetName(juce::String::fromUTF8("\xC2\xA0 Echo\xC2\xA0")) == "Echo");
    CHECK(normalisePresetName(juce::String::fromUTF8("\xE3\x80\x80"
                                                     "Echo"
                                                     "\xE3\x80\x80")) == "Echo");
    CHECK(normalisePresetName("Dub  Echo") == "Dub  Echo"); // 中间的空白不动

    // 去完是空串
    CHECK(normalisePresetName("").isEmpty());
    CHECK(normalisePresetName("   ").isEmpty());
    CHECK(normalisePresetName(juce::String::fromUTF8("\x01\x02\x7f")).isEmpty());
    CHECK(normalisePresetName(juce::String::fromUTF8("\xC2\x85")).isEmpty());
    CHECK(normalisePresetName(juce::String::fromUTF8("\xC2\xA0\xE3\x80\x80")).isEmpty());

    // 其余可见字符都允许，文件名不用显示名
    CHECK(normalisePresetName("A/B:C*D?") == "A/B:C*D?");
    CHECK(normalisePresetName("<x>|\"y\"\\z") == "<x>|\"y\"\\z");
    CHECK(normalisePresetName("..") == "..");

    // 24 个码点；汉字与非基本平面字符各算 1 个
    CHECK(normalisePresetName(repeat("a", 24)) == repeat("a", 24));
    CHECK(normalisePresetName(repeat("a", 25)) == repeat("a", 24));
    const juce::String han = juce::String::charToString(0x6C49);
    CHECK(normalisePresetName(repeat(han, 24)) == repeat(han, 24));
    CHECK(normalisePresetName(repeat(han, 24)).length() == 24);
    CHECK(normalisePresetName(repeat(han, 30)) == repeat(han, 24));
    const juce::String smile = juce::String::charToString(0x1F600);
    CHECK(normalisePresetName(repeat(smile, 24)) == repeat(smile, 24));
    CHECK(normalisePresetName(repeat(smile, 24)).length() == 24);
    CHECK(normalisePresetName(repeat(smile, 25)) == repeat(smile, 24));
    // 在 24 个码点处截断不会劈开一个字符，截完尾部的空白也要去掉
    CHECK(normalisePresetName(repeat("a", 23) + smile + "b") == repeat("a", 23) + smile);
    CHECK(normalisePresetName(repeat("a", 23) + " b") == repeat("a", 23));
    // 控制字符先删再数：删完正好 24 个就整个保留
    CHECK(normalisePresetName(repeat("a", 12) + juce::String::fromUTF8("\x01") + repeat("a", 12)) == repeat("a", 24));
}

TEST_CASE("user presets: save, list, file format and round trip", "[presets][user]")
{
    Env e;
    CHECK(e.store.listUser().presets.empty());
    CHECK_FALSE(e.dir.exists()); // 没保存过，目录还没建

    const auto saved = e.store.save("Echo A", false, kSample);
    REQUIRE(saved.ok());
    CHECK(isUserPresetId(saved.id));
    CHECK(saved.name == "Echo A");
    CHECK(saved.id.length() == 14);

    // 目录里只有这一个文件，文件名是 id 加扩展名，与显示名无关
    const auto names = namesIn(e.dir);
    REQUIRE(names.size() == 1);
    CHECK(names[0] == saved.id + ".rinnepreset");

    // 文件格式：键序固定、每个参数一行、末尾换行
    const juce::String text = fileOf(e.dir, saved.id).loadFileAsString();
    CHECK(text.startsWith("{\n  \"schema\": 1,\n  \"id\": \"" + saved.id +
                          "\",\n  \"name\": \"Echo A\",\n  \"params\": {\n"));
    CHECK(text.endsWith("\n  }\n}\n"));
    CHECK(text.contains(
        "\n    \"feedback\": 40,\n    \"mix\": 35.5,\n    \"delay_mode\": 1,\n    \"delay_ms\": 250.125,\n"
        "    \"delay_note\": 7,\n    \"low_cut\": 120,\n    \"high_cut\": 8000,\n    \"polarity\": 1\n"));

    const auto list = e.store.listUser();
    CHECK(list.skipped == 0);
    REQUIRE(list.presets.size() == 1);
    CHECK(list.presets[0].id == saved.id);
    CHECK(list.presets[0].name == "Echo A");
    CHECK(list.presets[0].values == kSample);

    // 第二个预设：不同的 id
    const auto second = e.store.save("Echo B", false, kOther);
    REQUIRE(second.ok());
    CHECK(second.id != saved.id);
    CHECK(e.store.listUser().presets.size() == 2);
    CHECK(namesIn(e.dir).size() == 2);
}

TEST_CASE("user presets: a negative zero and other values are written in their shortest form", "[presets][user]")
{
    UserPreset preset;
    preset.id = "u-0123456789ab";
    preset.name = "Zero";
    preset.values = kSample;
    preset.values[0] = -0.0f; // feedback
    const juce::String text = rinne::input::serialiseUserPreset(preset);
    CHECK(text.contains("\n    \"feedback\": 0,\n"));
    CHECK_FALSE(text.contains("\"feedback\": -0"));
    CHECK(text.contains("\"mix\": 35.5,"));
    CHECK(text.contains("\"delay_ms\": 250.125,"));
}

TEST_CASE("user presets: names with Chinese characters, symbols and non-BMP characters survive the file",
          "[presets][user]")
{
    Env e;
    const juce::String name = juce::String::charToString(0x6C49) + juce::String::charToString(0x5B57) + " / : * ? " +
                              juce::String::charToString(0x1F600) + juce::String::fromUTF8(" \xC3\xA9");
    const auto saved = e.store.save(name, false, kSample);
    REQUIRE(saved.ok());
    CHECK(saved.name == name);
    const auto list = e.store.listUser();
    REQUIRE(list.presets.size() == 1);
    CHECK(list.presets[0].name == name);
    CHECK(list.skipped == 0);
}

TEST_CASE("user presets: the name is cleaned before it is stored", "[presets][user][names]")
{
    Env e;
    // 控制字符与首尾空白去掉，超长截断，存下来的是规范化后的名字
    const auto cleaned = e.store.save(juce::String::fromUTF8("  Echo\x01  "), false, kSample);
    REQUIRE(cleaned.ok());
    CHECK(cleaned.name == "Echo");
    CHECK(e.store.listUser().presets[0].name == "Echo");

    const auto longName = e.store.save(repeat("x", 40), false, kSample);
    REQUIRE(longName.ok());
    CHECK(longName.name == repeat("x", 24));

    // 去完是空串：bad-name，不建文件
    Env fresh;
    for (const juce::String& bad : {juce::String(), juce::String("   "), juce::String::fromUTF8("\x01\x02"),
                                    juce::String::fromUTF8("\xC2\x85\x7f")})
    {
        const auto result = fresh.store.save(bad, true, kSample);
        CHECK_FALSE(result.ok());
        CHECK(result.error == PresetError::kBadName);
        CHECK(std::string(rinne::input::presetReason(result.error)) == "bad-name");
    }
    CHECK_FALSE(fresh.dir.exists());
}

TEST_CASE("user presets: a duplicate name is refused unless overwrite is allowed, and overwriting keeps the id",
          "[presets][user]")
{
    Env e;
    const auto first = e.store.save("Echo", false, kSample);
    REQUIRE(first.ok());
    const juce::String before = fileOf(e.dir, first.id).loadFileAsString();

    // 不分大小写、先去掉首尾空白再比较
    for (const char* again : {"Echo", "echo", "ECHO", "  eChO  "})
    {
        INFO("name '" << again << "'");
        const auto result = e.store.save(again, false, kOther);
        CHECK_FALSE(result.ok());
        CHECK(result.error == PresetError::kExists);
        CHECK(std::string(rinne::input::presetReason(result.error)) == "exists");
    }
    // 被拒之后什么都没变
    CHECK(fileOf(e.dir, first.id).loadFileAsString() == before);
    CHECK(namesIn(e.dir).size() == 1);

    // 允许覆盖：沿用原 id，内容和显示名换成新的
    const auto overwritten = e.store.save("ECHO", true, kOther);
    REQUIRE(overwritten.ok());
    CHECK(overwritten.id == first.id);
    CHECK(overwritten.name == "ECHO");
    CHECK(namesIn(e.dir) == std::vector<juce::String>{first.id + ".rinnepreset"}); // 没有临时文件留下
    const auto list = e.store.listUser();
    REQUIRE(list.presets.size() == 1);
    CHECK(list.presets[0].id == first.id);
    CHECK(list.presets[0].name == "ECHO");
    CHECK(list.presets[0].values == kOther);

    // 名字不同时 overwrite 不起作用：照样新建
    const auto fresh = e.store.save("Other", true, kSample);
    REQUIRE(fresh.ok());
    CHECK(fresh.id != first.id);
    CHECK(e.store.listUser().presets.size() == 2);
}

TEST_CASE("user presets: the list is sorted by name without regard to case, then by id", "[presets][user]")
{
    Env e;
    for (const char* name : {"beta", "Alpha", "alpha2", "Beta2", "Gamma"})
        REQUIRE(e.store.save(name, false, kSample).ok());
    const auto list = e.store.listUser();
    REQUIRE(list.presets.size() == 5);
    const std::vector<juce::String> want = {"Alpha", "alpha2", "beta", "Beta2", "Gamma"};
    for (std::size_t i = 0; i < want.size(); ++i)
        CHECK(list.presets[i].name == want[i]);
}

TEST_CASE("user presets: delete removes only user presets", "[presets][user][delete]")
{
    Env e;
    const auto a = e.store.save("A", false, kSample);
    const auto b = e.store.save("B", false, kOther);
    REQUIRE(a.ok());
    REQUIRE(b.ok());

    const auto removed = e.store.remove(a.id);
    CHECK(removed.ok());
    CHECK(removed.id == a.id);
    CHECK(removed.name == "A");
    CHECK_FALSE(fileOf(e.dir, a.id).exists());
    CHECK(fileOf(e.dir, b.id).existsAsFile());
    CHECK(e.store.listUser().presets.size() == 1);

    // 再删一次：不存在
    const auto again = e.store.remove(a.id);
    CHECK_FALSE(again.ok());
    CHECK(again.error == PresetError::kNotFound);

    // 工厂预设删不掉，返回专门的错误码；内置的 8 个一个不少
    for (const auto& preset : e.store.factoryPresets())
    {
        const auto result = e.store.remove(preset.id);
        CHECK_FALSE(result.ok());
        CHECK(result.error == PresetError::kFactory);
        CHECK(std::string(rinne::input::presetReason(result.error)) == "factory");
    }
    CHECK(e.store.factoryPresets().size() == rinne::input::kFactoryPresetCount);
    CHECK(fileOf(e.dir, b.id).existsAsFile());

    // 不是预设 id 的东西一律 not-found，不会按路径去碰别的文件
    const juce::File decoy = e.scratch.root().getChildFile("outside.rinnepreset");
    REQUIRE(decoy.replaceWithText("keep"));
    for (const char* id : {"../outside", "..\\outside", "outside", "", "u-ZZZZZZZZZZZZ", "u-123", "f-no-such-preset"})
    {
        INFO("id '" << id << "'");
        const auto result = e.store.remove(id);
        CHECK_FALSE(result.ok());
        CHECK(result.error == PresetError::kNotFound);
    }
    CHECK(decoy.existsAsFile());
    CHECK(decoy.loadFileAsString() == "keep");

    // 十二位十六进制才是预设 id：差一位或多一位的文件名即使内容是好的，也不是预设——
    // 列表不收也不数，按这个 id 删除是 not-found 且文件原样还在
    for (const char* oddId : {"u-0123456789a", "u-0123456789abc"})
    {
        INFO("id '" << oddId << "'");
        CHECK_FALSE(isUserPresetId(oddId));
        const juce::File odd = fileOf(e.dir, oddId);
        UserPreset content;
        content.id = oddId;
        content.name = "Odd";
        content.values = kSample;
        REQUIRE(odd.replaceWithText(rinne::input::serialiseUserPreset(content)));
        const auto result = e.store.remove(oddId);
        CHECK_FALSE(result.ok());
        CHECK(result.error == PresetError::kNotFound);
        CHECK(odd.existsAsFile());
        const auto listed = e.store.listUser();
        CHECK(listed.presets.size() == 1); // 只有 B
        CHECK(listed.skipped == 0);
    }
    CHECK(isUserPresetId("u-0123456789ab"));
}

TEST_CASE("user presets: unreadable files are skipped and counted, never deleted", "[presets][user]")
{
    Env e;
    REQUIRE(e.dir.createDirectory().wasOk());
    const auto put = [&e](const juce::String& fileName, const juce::String& text) {
        REQUIRE(e.dir.getChildFile(fileName).replaceWithText(text));
    };
    const juce::String ok = "\"feedback\": 33";

    put("u-111111111111.rinnepreset", handWritten("u-111111111111", "Fine", ok)); // 好的
    // 下面六个名字合规但读不通：计数
    put("u-aaaaaaaaaaaa.rinnepreset", "this is not json");
    put("u-bbbbbbbbbbbb.rinnepreset", handWritten("u-bbbbbbbbbbbb", "Newer", ok, "2")); // 更新的格式
    put("u-cccccccccccc.rinnepreset", handWritten("u-dddddddddddd", "Wrong id", ok)); // 文件里的 id 与文件名不符
    put("u-eeeeeeeeeeee.rinnepreset", repeat("x", 20 * 1024)); // 体量超限
    put("u-222222222222.rinnepreset", handWritten("u-222222222222", repeat("n", 25), ok)); // 名字超长
    put("u-333333333333.rinnepreset", handWritten("u-333333333333", "Text value", "\"feedback\": \"high\""));
    // 下面这些不是预设文件，不计数
    put("notes.txt", "hello");
    put("readme.rinnepreset", "hello");
    put("u-444444444444.rinnepreset.tmp", "half a file");

    const auto before = namesIn(e.dir);
    const auto list = e.store.listUser();
    REQUIRE(list.presets.size() == 1);
    CHECK(list.presets[0].id == "u-111111111111");
    CHECK(list.presets[0].name == "Fine");
    CHECK(list.skipped == 6);
    // 没有删除任何文件
    CHECK(namesIn(e.dir) == before);

    // 好文件缺的参数取默认值
    const PresetValues defaults = rinne::input::presetDefaults();
    CHECK(list.presets[0].values[0] == 33.0f);
    for (std::size_t i = 1; i < kPresetParamCount; ++i)
        CHECK(list.presets[0].values[i] == defaults[i]);

    // 保存照常工作，坏文件原样留着
    REQUIRE(e.store.save("New", false, kSample).ok());
    CHECK(e.store.listUser().skipped == 6);
    CHECK(e.store.listUser().presets.size() == 2);
}

TEST_CASE("user presets: parsing is lenient about content and strict about shape", "[presets][user]")
{
    using rinne::input::parseUserPreset;
    const juce::String id = "u-0123456789ab";
    const PresetValues defaults = rinne::input::presetDefaults();

    // 没有任何参数：全取默认
    auto parsed = parseUserPreset(handWritten(id, "Empty", ""), id);
    REQUIRE(parsed.has_value());
    CHECK(parsed->values == defaults);
    CHECK(parsed->id == id);

    // 越界夹回范围，不认识的键与三个工程参数的键都忽略
    parsed = parseUserPreset(handWritten(id, "Wild",
                                         "\"feedback\": 999, \"mix\": -5, \"delay_mode\": 7, \"delay_ms\": 99999, "
                                         "\"delay_note\": -3, \"low_cut\": 1, \"high_cut\": 1e9, \"polarity\": 2, "
                                         "\"foo\": 1, \"loop_on\": 0, \"limiter\": 0, \"ceiling\": -20"),
                             id);
    REQUIRE(parsed.has_value());
    CHECK(parsed->values == PresetValues{{150.0f, 0.0f, 1.0f, 4000.0f, 0.0f, 20.0f, 20000.0f, 1.0f}});

    // 名字：读入时去控制字符与首尾空白
    parsed = parseUserPreset(handWritten(id, "  Padded  ", ""), id);
    REQUIRE(parsed.has_value());
    CHECK(parsed->name == "Padded");

    // 形状不对一律拒收
    CHECK_FALSE(parseUserPreset("", id).has_value());
    CHECK_FALSE(parseUserPreset("[]", id).has_value());
    CHECK_FALSE(parseUserPreset(handWritten(id, "X", "", "2"), id).has_value()); // schema
    CHECK_FALSE(parseUserPreset(handWritten(id, "X", "", "\"1\""), id).has_value());
    CHECK_FALSE(parseUserPreset(handWritten("u-ffffffffffff", "X", ""), id).has_value()); // id 不符
    CHECK_FALSE(parseUserPreset(handWritten(id, "", ""), id).has_value()); // 名字空
    CHECK_FALSE(parseUserPreset(handWritten(id, "   ", ""), id).has_value());
    CHECK_FALSE(parseUserPreset(handWritten(id, repeat("n", 25), ""), id).has_value()); // 名字超长
    CHECK_FALSE(parseUserPreset("{\"schema\": 1, \"id\": \"" + id + "\", \"params\": {}}", id).has_value()); // 没有名字
    CHECK_FALSE(
        parseUserPreset("{\"schema\": 1, \"id\": \"" + id + "\", \"name\": \"X\"}", id).has_value()); // 没有 params
    CHECK_FALSE(parseUserPreset(handWritten(id, "X", "\"mix\": true"), id).has_value()); // 类型不对
    CHECK_FALSE(parseUserPreset(handWritten(id, "X", "\"mix\": null"), id).has_value());
    CHECK_FALSE(parseUserPreset(handWritten("f-init", "X", ""), "f-init").has_value()); // 不是用户 id
}

TEST_CASE("user presets: a user preset may share a name with a factory preset", "[presets][user]")
{
    Env e;
    REQUIRE(e.store.factoryPresetsValid());
    const auto saved = e.store.save("Init", false, kSample);
    REQUIRE(saved.ok());
    CHECK(saved.id != "f-init");
    CHECK(e.store.listUser().presets.size() == 1);
    CHECK(e.store.factoryPresets().size() == rinne::input::kFactoryPresetCount);
    CHECK(e.store.factoryPresets()[0].id == "f-init");

    // 靠 id 区分：各载各的
    ParamsTestProcessor processor;
    rinne::input::InputParams params(processor);
    REQUIRE(e.store.load(saved.id, params.tree()).ok());
    CHECK(params.tree().getRawParameterValue("feedback")->load() == Approx(40.0f));
    REQUIRE(e.store.load("f-init", params.tree()).ok());
    CHECK(params.tree().getRawParameterValue("feedback")->load() == Approx(0.0f));
}

TEST_CASE("user presets: load restores the saved values and leaves loop_on, limiter and ceiling alone",
          "[presets][user][load]")
{
    Env e;
    ParamsTestProcessor processor;
    rinne::input::InputParams params(processor);
    auto set = [&params](const char* id, float plain) {
        auto* p = params.tree().getParameter(id);
        REQUIRE(p != nullptr);
        p->setValueNotifyingHost(p->convertTo0to1(plain));
    };
    const auto raw = [&params](const char* id) { return params.tree().getRawParameterValue(id)->load(); };

    set("feedback", 40.0f);
    set("mix", 35.5f);
    set("delay_mode", 1.0f);
    set("delay_ms", 250.125f);
    set("delay_note", 7.0f);
    set("low_cut", 120.0f);
    set("high_cut", 8000.0f);
    set("polarity", 1.0f);
    // 保存的是参数树里当前的 8 个值，不含工程参数
    const auto saved = e.store.save("Mine", false, params.tree());
    REQUIRE(saved.ok());
    const auto list = e.store.listUser();
    REQUIRE(list.presets.size() == 1);
    CHECK(list.presets[0].values[0] == Approx(40.0f));
    CHECK(list.presets[0].values[3] == Approx(250.125f).margin(0.001));
    {
        const juce::String text = fileOf(e.dir, saved.id).loadFileAsString();
        CHECK_FALSE(text.contains("loop_on"));
        CHECK_FALSE(text.contains("limiter"));
        CHECK_FALSE(text.contains("ceiling"));
    }

    // 弄乱声音参数和工程参数，再载入
    set("feedback", 99.0f);
    set("mix", 1.0f);
    set("delay_mode", 0.0f);
    set("delay_ms", 3000.0f);
    set("delay_note", 3.0f);
    set("low_cut", 500.0f);
    set("high_cut", 3000.0f);
    set("polarity", 0.0f);
    set("loop_on", 0.0f);
    set("limiter", 0.0f);
    set("ceiling", -12.0f);
    CHECK(e.store.presetSeq() == 0u);
    const auto loaded = e.store.load(saved.id, params.tree());
    REQUIRE(loaded.ok());
    CHECK(loaded.id == saved.id);
    CHECK(loaded.name == "Mine");
    CHECK(e.store.presetSeq() == 1u);
    CHECK(raw("feedback") == Approx(40.0f));
    CHECK(raw("mix") == Approx(35.5f));
    CHECK(raw("delay_mode") == 1.0f);
    CHECK(raw("delay_ms") == Approx(250.125f).margin(0.001));
    CHECK(raw("delay_note") == 7.0f);
    CHECK(raw("low_cut") == Approx(120.0f).margin(0.1));
    CHECK(raw("high_cut") == Approx(8000.0f).margin(1.0));
    CHECK(raw("polarity") == 1.0f);
    CHECK(raw("loop_on") == 0.0f);
    CHECK(raw("limiter") == 0.0f);
    CHECK(raw("ceiling") == Approx(-12.0f));

    // 手写的文件：越界值夹回范围，不认识的键与 loop_on、limiter、ceiling 忽略
    set("loop_on", 1.0f);
    set("limiter", 1.0f);
    set("ceiling", -6.0f);
    const juce::String wildId = "u-5555555555aa";
    REQUIRE(e.dir.getChildFile(wildId + ".rinnepreset")
                .replaceWithText(handWritten(wildId, "Wild",
                                             "\"feedback\": 999, \"mix\": -5, \"delay_mode\": 7, \"delay_ms\": 99999, "
                                             "\"delay_note\": -3, \"low_cut\": 1, \"high_cut\": 1e9, \"polarity\": 2, "
                                             "\"foo\": 1, \"loop_on\": 0, \"limiter\": 0, \"ceiling\": -20")));
    REQUIRE(e.store.load(wildId, params.tree()).ok());
    CHECK(e.store.presetSeq() == 2u);
    CHECK(raw("feedback") == 150.0f);
    CHECK(raw("mix") == 0.0f);
    CHECK(raw("delay_mode") == 1.0f);
    CHECK(raw("delay_ms") == Approx(4000.0f));
    CHECK(raw("delay_note") == 0.0f);
    CHECK(raw("low_cut") == Approx(20.0f));
    CHECK(raw("high_cut") == Approx(20000.0f));
    CHECK(raw("polarity") == 1.0f);
    CHECK(raw("loop_on") == 1.0f);
    CHECK(raw("limiter") == 1.0f);
    CHECK(raw("ceiling") == Approx(-6.0f));

    // 坏文件与不存在的 id：失败，序号不变，参数不动
    const juce::String badId = "u-aaaaaaaaaaaa";
    REQUIRE(e.dir.getChildFile(badId + ".rinnepreset").replaceWithText("not json"));
    const auto bad = e.store.load(badId, params.tree());
    CHECK_FALSE(bad.ok());
    CHECK(bad.error == PresetError::kBadFile);
    CHECK(std::string(rinne::input::presetReason(bad.error)) == "bad-file");
    const auto missing = e.store.load("u-bbbbbbbbbbbb", params.tree());
    CHECK_FALSE(missing.ok());
    CHECK(missing.error == PresetError::kNotFound);
    CHECK(e.store.presetSeq() == 2u);
    CHECK(raw("feedback") == 150.0f);
}

TEST_CASE("user presets: ids are 12 hex digits drawn from 48 random bits and never reuse an existing file",
          "[presets][user]")
{
    ScratchDir scratch;
    const juce::File dir = scratch.root().getChildFile("Presets");
    REQUIRE(dir.createDirectory().wasOk());
    // 一个读不通的坏文件占着 000000000003：撞上它也要换
    REQUIRE(fileOf(dir, "u-000000000003").replaceWithText("garbage"));

    std::vector<std::uint64_t> draws = {0xFFFF000000000001ull, 0xFFFF000000000001ull, 0x3ull, 0x4ull,
                                        0x0000ABCDEF12ull};
    std::size_t next = 0;
    PresetStore::Hooks hooks;
    hooks.randomBits = [&draws, &next] { return draws[std::min(next++, draws.size() - 1)]; };
    PresetStore store(dir, hooks);

    const auto first = store.save("One", false, kSample);
    REQUIRE(first.ok());
    CHECK(first.id == "u-000000000001"); // 只取低 48 位
    const auto second =
        store.save("Two", false, kSample); // 第一次抽到 ...0001（已有）、第二次抽到 3（坏文件占着）、再抽到 4
    REQUIRE(second.ok());
    CHECK(second.id == "u-000000000004");
    CHECK(fileOf(dir, "u-000000000003").loadFileAsString() == "garbage"); // 坏文件原样
    const auto third = store.save("Three", false, kSample);
    REQUIRE(third.ok());
    CHECK(third.id == "u-0000abcdef12"); // 前导零补满 12 位、小写
    CHECK(isUserPresetId(third.id));

    // 每次都撞上：有限次之后放弃，不死循环
    PresetStore::Hooks stuck;
    stuck.randomBits = [] { return std::uint64_t{1}; };
    PresetStore stuckStore(dir, stuck);
    const auto none = stuckStore.save("Four", false, kSample);
    CHECK_FALSE(none.ok());
    CHECK(none.error == PresetError::kWriteFailed);
}

TEST_CASE("user presets: the directory is created on the first save, deep paths included", "[presets][user]")
{
    ScratchDir scratch;
    const juce::File dir = scratch.root().getChildFile("a").getChildFile("b").getChildFile("Presets");
    PresetStore store(dir);
    CHECK_FALSE(dir.exists());
    CHECK(store.listUser().presets.empty()); // 读列表不建目录
    CHECK_FALSE(dir.exists());
    REQUIRE(store.save("Deep", false, kSample).ok());
    CHECK(dir.isDirectory());
    CHECK(store.listUser().presets.size() == 1);
}

TEST_CASE("user presets: canWrite probes the directory and leaves nothing behind", "[presets][user][write]")
{
    {
        Env e;
        CHECK(e.store.canWrite());
        CHECK(e.dir.isDirectory());
        CHECK(namesIn(e.dir).empty()); // 探测文件已删
        REQUIRE(e.store.save("After probe", false, kSample).ok());
        CHECK(e.store.canWrite());
        CHECK(namesIn(e.dir).size() == 1);
    }
    {
        // 目录路径被一个普通文件挡住：建不出目录，写不进去
        ScratchDir scratch;
        const juce::File blocker = scratch.root().getChildFile("blocked");
        REQUIRE(blocker.replaceWithText("I am a file"));
        PresetStore store(blocker.getChildFile("Presets"));
        CHECK_FALSE(store.canWrite());
    }
}

TEST_CASE("user presets: a directory that cannot be created gives write-failed and leaves nothing",
          "[presets][user][write]")
{
    ScratchDir scratch;
    const juce::File blocker = scratch.root().getChildFile("blocked");
    REQUIRE(blocker.replaceWithText("I am a file"));
    PresetStore store(blocker.getChildFile("Presets"));

    const auto result = store.save("Nope", false, kSample);
    CHECK_FALSE(result.ok());
    CHECK(result.error == PresetError::kWriteFailed);
    CHECK(std::string(rinne::input::presetReason(result.error)) == "write-failed");
    CHECK(blocker.existsAsFile());
    CHECK(blocker.loadFileAsString() == "I am a file");
    CHECK(namesIn(scratch.root()) == std::vector<juce::String>{"blocked"});
    CHECK(store.listUser().presets.empty());
    CHECK(store.presetSeq() == 0u);
}

TEST_CASE("user presets: a failed replace gives write-failed, cleans up the temp file and keeps the old file",
          "[presets][user][write]")
{
    ScratchDir scratch;
    const juce::File dir = scratch.root().getChildFile("Presets");
    bool failReplace = false;
    int replaceCalls = 0;
    juce::String tempTextAtReplace;
    bool tempWasBesideTarget = false;
    PresetStore::Hooks hooks;
    hooks.replaceFile = [&](const juce::File& from, const juce::File& to) {
        ++replaceCalls;
        tempTextAtReplace = from.loadFileAsString();
        tempWasBesideTarget = from.getParentDirectory() == to.getParentDirectory() && from.existsAsFile();
        return failReplace ? false : from.replaceFileIn(to);
    };
    PresetStore store(dir, hooks);

    // 新建：替换失败，目录里既没有目标也没有临时文件
    failReplace = true;
    const auto failed = store.save("Fresh", false, kSample);
    CHECK_FALSE(failed.ok());
    CHECK(failed.error == PresetError::kWriteFailed);
    CHECK(replaceCalls == 1);
    CHECK(tempWasBesideTarget);
    CHECK(tempTextAtReplace.contains("\"name\": \"Fresh\"")); // 替换之前临时文件已经写完整
    CHECK(tempTextAtReplace.endsWith("\n  }\n}\n"));
    CHECK(namesIn(dir).empty());
    CHECK(store.listUser().presets.empty());

    // 覆盖：先成功存一份，再让替换失败，旧文件的字节原样还在
    failReplace = false;
    const auto saved = store.save("Keep", false, kSample);
    REQUIRE(saved.ok());
    const juce::String before = fileOf(dir, saved.id).loadFileAsString();
    failReplace = true;
    const auto lost = store.save("keep", true, kOther);
    CHECK_FALSE(lost.ok());
    CHECK(lost.error == PresetError::kWriteFailed);
    CHECK(fileOf(dir, saved.id).loadFileAsString() == before);
    CHECK(namesIn(dir) == std::vector<juce::String>{saved.id + ".rinnepreset"});
    const auto list = store.listUser();
    REQUIRE(list.presets.size() == 1);
    CHECK(list.presets[0].name == "Keep");
    CHECK(list.presets[0].values == kSample);

    // 恢复之后同一个名字照常覆盖
    failReplace = false;
    REQUIRE(store.save("keep", true, kOther).ok());
    CHECK(store.listUser().presets[0].values == kOther);
}

TEST_CASE("user presets: a leftover temp file from a crash is replaced, and one that cannot be cleared fails cleanly",
          "[presets][user][write]")
{
    ScratchDir scratch;
    const juce::File dir = scratch.root().getChildFile("Presets");
    REQUIRE(dir.createDirectory().wasOk());
    PresetStore::Hooks hooks;
    hooks.randomBits = [] { return std::uint64_t{0x123456789abcull}; };
    PresetStore store(dir, hooks);

    // 上次崩溃留下的临时文件：被清掉重写，保存成功，目录里只剩目标
    const juce::File leftover = dir.getChildFile("u-123456789abc.rinnepreset.tmp");
    REQUIRE(leftover.replaceWithText(repeat("old garbage ", 200)));
    const auto saved = store.save("After crash", false, kSample);
    REQUIRE(saved.ok());
    CHECK(saved.id == "u-123456789abc");
    CHECK(namesIn(dir) == std::vector<juce::String>{"u-123456789abc.rinnepreset"});
    CHECK(fileOf(dir, saved.id).loadFileAsString().contains("\"name\": \"After crash\""));
    CHECK_FALSE(fileOf(dir, saved.id).loadFileAsString().contains("old garbage"));

    // 临时文件的名字被一个非空目录占着：清不掉，写失败，目标没有建出来
    REQUIRE(store.remove(saved.id).ok());
    const juce::File blockingDir = dir.getChildFile("u-123456789abc.rinnepreset.tmp");
    REQUIRE(blockingDir.createDirectory().wasOk());
    REQUIRE(blockingDir.getChildFile("inside.txt").replaceWithText("x"));
    const auto blocked = store.save("Blocked", false, kSample);
    CHECK_FALSE(blocked.ok());
    CHECK(blocked.error == PresetError::kWriteFailed);
    CHECK_FALSE(fileOf(dir, "u-123456789abc").exists());
    CHECK(blockingDir.getChildFile("inside.txt").existsAsFile());
}

TEST_CASE("user presets: the default directory is Synchain/Rinne/Presets under the user application data directory",
          "[presets][user]")
{
    const juce::File dir = PresetStore::defaultUserDirectory();
    const juce::String path = dir.getFullPathName().replaceCharacter('\\', '/');
    CHECK(path.endsWith("/Synchain/Rinne/Presets"));
    CHECK(dir.getFileName() == "Presets");
    CHECK(dir.getParentDirectory().getFileName() == "Rinne");
    CHECK(dir.getParentDirectory().getParentDirectory().getFileName() == "Synchain");
#if JUCE_MAC
    CHECK(path.contains("/Library/Application Support/Synchain/"));
#endif
    // 默认构造的库用它
    const PresetStore store;
    CHECK(store.userDirectory() == dir);
}
