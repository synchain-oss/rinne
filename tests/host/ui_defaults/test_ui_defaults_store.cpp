// SPDX-License-Identifier: GPL-3.0-or-later
//
// 界面全局默认存储：键名、各键互不串、白名单与范围校验、磁盘上被改动过的值。
// 全程把落盘目录指向本测试独占的临时目录（setStorageDirForTests），不碰用户的真实设置。

#include <catch2/catch_test_macros.hpp>

#include <string>

#include <juce_core/juce_core.h>

#include "UiDefaultsStore.h"

namespace
{

namespace store = rinne::uidefaults;

// 系统临时目录下本测试独占的一个目录；用完只删里面的普通文件和这个空目录，不做递归删除。
// 构造时把存储指向它，析构时恢复默认位置
class ScratchStore
{
public:
    ScratchStore()
    {
        dir_ = juce::File::getSpecialLocation(juce::File::tempDirectory)
                   .getNonexistentChildFile("rinne-ui-defaults-test", "", false);
        REQUIRE(dir_.createDirectory().wasOk());
        store::setStorageDirForTests(dir_);
    }

    ~ScratchStore()
    {
        store::setStorageDirForTests(juce::File());
        juce::Array<juce::File> files;
        dir_.findChildFiles(files, juce::File::findFiles, false);
        for (const auto& f : files)
            f.deleteFile();
        dir_.deleteFile(); // 此时是空目录
    }

    ScratchStore(const ScratchStore&) = delete;
    ScratchStore& operator=(const ScratchStore&) = delete;

    juce::File settingsFile() const { return dir_.getChildFile("ui-defaults.settings"); }
    juce::File dir() const { return dir_; }

    // 直接改写磁盘上的 XML，模拟被用户或别的工具改过的文件
    void writeRaw(const juce::String& attributes) const
    {
        REQUIRE(settingsFile().replaceWithText("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<PROPERTIES>\n" +
                                               attributes + "\n</PROPERTIES>\n"));
    }

private:
    juce::File dir_;
};

juce::String entry(const char* name, const juce::String& value)
{
    return "  <VALUE name=\"" + juce::String(name) + "\" val=\"" + value + "\"/>";
}

} // namespace

TEST_CASE("ui defaults: a fresh store reports no choices made", "[ui_defaults]")
{
    ScratchStore scratch;
    CHECK_FALSE(store::langChosenGlobal());
    CHECK(store::langGlobal().isEmpty());
    CHECK(store::rulesAckVersion() == 0);
    CHECK_FALSE(store::guideSeenGlobalInput());
    CHECK_FALSE(store::guideSeenGlobalOutput());
    CHECK(store::uiScalePercentInput() == 0);
    CHECK(store::uiScalePercentOutput() == 0);
    CHECK_FALSE(store::diagLogEnabled());
    CHECK_FALSE(scratch.settingsFile().existsAsFile()); // 只读不建文件
}

TEST_CASE("ui defaults: every value survives a write and read", "[ui_defaults]")
{
    ScratchStore scratch;

    store::setLangChosenGlobal(true);
    store::setLangGlobal("fr");
    store::setRulesAckVersion(7);
    store::setGuideSeenGlobalInput(true);
    store::setGuideSeenGlobalOutput(true);
    store::setUiScalePercentInput(125);
    store::setUiScalePercentOutput(75);
    store::setDiagLogEnabled(true);

    CHECK(store::langChosenGlobal());
    CHECK(store::langGlobal() == "fr");
    CHECK(store::rulesAckVersion() == 7);
    CHECK(store::guideSeenGlobalInput());
    CHECK(store::guideSeenGlobalOutput());
    CHECK(store::uiScalePercentInput() == 125);
    CHECK(store::uiScalePercentOutput() == 75);
    CHECK(store::diagLogEnabled());

    // 取消也能写回
    store::setLangChosenGlobal(false);
    store::setDiagLogEnabled(false);
    CHECK_FALSE(store::langChosenGlobal());
    CHECK_FALSE(store::diagLogEnabled());
}

TEST_CASE("ui defaults: values are written to disk at once under the documented key names", "[ui_defaults]")
{
    ScratchStore scratch;

    store::setLangChosenGlobal(true);
    store::setLangGlobal("en");
    store::setRulesAckVersion(3);
    store::setGuideSeenGlobalInput(true);
    store::setGuideSeenGlobalOutput(true);
    store::setUiScalePercentInput(150);
    store::setUiScalePercentOutput(200);
    store::setDiagLogEnabled(true);

    // 不依赖定时保存：写完文件就在磁盘上，宿主被强杀也不丢
    REQUIRE(scratch.settingsFile().existsAsFile());
    const auto text = scratch.settingsFile().loadFileAsString();
    for (const char* key :
         {"lang_chosen_global", "lang_global", "rules_ack_version", "guide_seen_global_input",
          "guide_seen_global_output", "ui_scale_percent_input", "ui_scale_percent_output", "diag_log_enabled"})
        CHECK(text.contains("name=\"" + juce::String(key) + "\""));

    // 不再保留旧产品里的导览键
    CHECK_FALSE(text.contains("tour_seen_global"));
}

TEST_CASE("ui defaults: Input and Output keys do not leak into each other", "[ui_defaults]")
{
    ScratchStore scratch;

    store::setGuideSeenGlobalInput(true);
    CHECK(store::guideSeenGlobalInput());
    CHECK_FALSE(store::guideSeenGlobalOutput());

    store::setUiScalePercentOutput(300);
    CHECK(store::uiScalePercentOutput() == 300);
    CHECK(store::uiScalePercentInput() == 0);

    store::setUiScalePercentInput(65);
    CHECK(store::uiScalePercentInput() == 65);
    CHECK(store::uiScalePercentOutput() == 300);

    // 「选过语言」和「选的是哪个」是两个键，互不牵连
    store::setLangGlobal("zh");
    CHECK_FALSE(store::langChosenGlobal());
    store::setLangChosenGlobal(true);
    store::setLangGlobal("en");
    CHECK(store::langChosenGlobal());
    CHECK(store::langGlobal() == "en");
}

TEST_CASE("ui defaults: role keyed wrappers hit the matching keys", "[ui_defaults]")
{
    ScratchStore scratch;

    store::setGuideSeenGlobal("input", true);
    CHECK(store::guideSeenGlobalInput());
    CHECK_FALSE(store::guideSeenGlobalOutput());
    CHECK(store::guideSeenGlobal("input"));
    CHECK_FALSE(store::guideSeenGlobal("output"));

    store::setUiScalePercent("output", 175);
    CHECK(store::uiScalePercentOutput() == 175);
    CHECK(store::uiScalePercentInput() == 0);
    CHECK(store::uiScalePercent("output") == 175);
    CHECK(store::uiScalePercent("input") == 0);

    store::setUiScalePercent("input", 80);
    CHECK(store::uiScalePercentInput() == 80);
    CHECK(store::uiScalePercentOutput() == 175);

    // 其余角色名一律当 Output
    store::setUiScalePercent("anything", 50);
    CHECK(store::uiScalePercentOutput() == 50);
    CHECK(store::uiScalePercentInput() == 80);
}

TEST_CASE("ui defaults: language only accepts zh, en, fr", "[ui_defaults]")
{
    ScratchStore scratch;

    for (const char* ok : {"zh", "en", "fr"})
    {
        store::setLangGlobal(ok);
        CHECK(store::langGlobal() == ok);
    }

    // 不在白名单里的值既不写盘，也不覆盖已有的值
    store::setLangGlobal("de");
    store::setLangGlobal("");
    store::setLangGlobal("FR");
    CHECK(store::langGlobal() == "fr");

    // 磁盘上被改成不认识的值：按没存过处理
    scratch.writeRaw(entry("lang_global", "de"));
    CHECK(store::langGlobal().isEmpty());
    scratch.writeRaw(entry("lang_global", "en"));
    CHECK(store::langGlobal() == "en");
}

TEST_CASE("ui defaults: scale percent accepts 33 to 300 only", "[ui_defaults]")
{
    ScratchStore scratch;

    store::setUiScalePercentOutput(33);
    CHECK(store::uiScalePercentOutput() == 33);
    store::setUiScalePercentOutput(300);
    CHECK(store::uiScalePercentOutput() == 300);

    // 范围外的写入直接忽略，已有的值保持不变
    store::setUiScalePercentOutput(32);
    CHECK(store::uiScalePercentOutput() == 300);
    store::setUiScalePercentOutput(301);
    CHECK(store::uiScalePercentOutput() == 300);
    store::setUiScalePercentOutput(0);
    CHECK(store::uiScalePercentOutput() == 300);
    store::setUiScalePercentOutput(-100);
    CHECK(store::uiScalePercentOutput() == 300);

    store::setUiScalePercentInput(50);
    store::setUiScalePercentInput(9999);
    CHECK(store::uiScalePercentInput() == 50);

    // 磁盘上被改成范围外的值：按没存过处理
    scratch.writeRaw(entry("ui_scale_percent_input", "9999") + "\n" + entry("ui_scale_percent_output", "5"));
    CHECK(store::uiScalePercentInput() == 0);
    CHECK(store::uiScalePercentOutput() == 0);
    scratch.writeRaw(entry("ui_scale_percent_input", "33") + "\n" + entry("ui_scale_percent_output", "300"));
    CHECK(store::uiScalePercentInput() == 33);
    CHECK(store::uiScalePercentOutput() == 300);
}

TEST_CASE("ui defaults: rules acknowledgement ignores negatives, on disk too", "[ui_defaults]")
{
    ScratchStore scratch;

    store::setRulesAckVersion(5);
    CHECK(store::rulesAckVersion() == 5);
    store::setRulesAckVersion(-1);
    CHECK(store::rulesAckVersion() == 5);
    store::setRulesAckVersion(0);
    CHECK(store::rulesAckVersion() == 0);

    scratch.writeRaw(entry("rules_ack_version", "-4"));
    CHECK(store::rulesAckVersion() == 0);
    scratch.writeRaw(entry("rules_ack_version", "2"));
    CHECK(store::rulesAckVersion() == 2);
}

TEST_CASE("ui defaults: boolean keys tolerate garbage on disk", "[ui_defaults]")
{
    ScratchStore scratch;
    scratch.writeRaw(entry("guide_seen_global_input", "banana") + "\n" + entry("diag_log_enabled", "1") + "\n" +
                     entry("guide_seen_global_output", "0"));
    CHECK_FALSE(store::guideSeenGlobalInput());
    CHECK(store::diagLogEnabled());
    CHECK_FALSE(store::guideSeenGlobalOutput());

    // 整个文件损坏时全部回到默认，不抛出
    REQUIRE(scratch.settingsFile().replaceWithText("this is not xml"));
    CHECK_FALSE(store::langChosenGlobal());
    CHECK(store::langGlobal().isEmpty());
    CHECK(store::uiScalePercentInput() == 0);
}

TEST_CASE("ui defaults: switching the test directory switches the data", "[ui_defaults]")
{
    ScratchStore first;
    store::setUiScalePercentInput(150);
    REQUIRE(store::uiScalePercentInput() == 150);

    {
        ScratchStore second; // 构造时把存储指向它
        CHECK(store::uiScalePercentInput() == 0);
        store::setUiScalePercentInput(65);
        CHECK(store::uiScalePercentInput() == 65);
    } // 析构时存储恢复默认位置，下面重新指回第一个

    store::setStorageDirForTests(first.dir());
    CHECK(store::uiScalePercentInput() == 150);
}
