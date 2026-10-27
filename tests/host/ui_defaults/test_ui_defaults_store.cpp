// SPDX-License-Identifier: GPL-3.0-or-later
//
// 界面全局默认存储：键名、各键互不串、白名单与范围校验、磁盘上被改动过的值。
// 全程把落盘目录指向本测试独占的临时目录（setStorageDirForTests），不碰用户的真实设置。

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <string>
#include <utility>
#include <vector>

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

// ---------------------------------------------------------------------------------------------
// 进程间锁与一次读回。真锁是系统的命名互斥量或文件锁，单个进程里排不出争用，所以用 setLockForTests 装一对替身，
// 在「拿到锁」的那一刻模拟另一个进程刚写完，再看本进程的读写是不是从拿锁之后才开文件、写完才放锁。
// ---------------------------------------------------------------------------------------------

namespace
{

// 记录每一次拿锁与放锁，并允许在拿到锁的瞬间插一段「别的进程做的事」
class LockProbe
{
public:
    LockProbe()
    {
        store::setLockForTests(
            [this](int timeoutMs) {
                waits.push_back(timeoutMs);
                ++enters;
                if (!grant)
                    return false;
                ++depth;
                maxDepth = depth > maxDepth ? depth : maxDepth;
                if (whileWaiting)
                {
                    auto once = std::move(whileWaiting);
                    whileWaiting = nullptr;
                    once();
                }
                return true;
            },
            [this]() {
                ++exits;
                --depth;
                if (atRelease)
                    atRelease();
            });
    }

    ~LockProbe() { store::setLockForTests({}, {}); }

    LockProbe(const LockProbe&) = delete;
    LockProbe& operator=(const LockProbe&) = delete;

    int enters = 0;
    int exits = 0;
    int depth = 0;
    int maxDepth = 0;
    bool grant = true;
    std::vector<int> waits;
    std::function<void()> whileWaiting; // 拿到锁之后第一件事（只做一次）：别的进程在我们等锁期间写了盘
    std::function<void()> atRelease; // 放锁的那一刻：看盘上是不是已经是写完的样子
};

} // namespace

TEST_CASE("ui defaults: the lock name and the wait are fixed and bounded", "[ui_defaults][lock]")
{
    CHECK(juce::String(store::kProcessLockName).startsWith("Rinne"));
    CHECK(store::kProcessLockWaitMs > 0);
    CHECK(store::kProcessLockWaitMs <= 2000); // 别的进程卡死时，消息线程最多等这么久
}

TEST_CASE("ui defaults: every read and write takes the process lock exactly once, and gives it back",
          "[ui_defaults][lock]")
{
    ScratchStore scratch;
    LockProbe probe;

    const std::vector<std::pair<const char*, std::function<void()>>> calls = {
        {"langChosenGlobal", [] { (void)store::langChosenGlobal(); }},
        {"setLangChosenGlobal", [] { store::setLangChosenGlobal(true); }},
        {"langGlobal", [] { (void)store::langGlobal(); }},
        {"setLangGlobal", [] { store::setLangGlobal("fr"); }},
        {"rulesAckVersion", [] { (void)store::rulesAckVersion(); }},
        {"setRulesAckVersion", [] { store::setRulesAckVersion(2); }},
        {"guideSeenGlobalInput", [] { (void)store::guideSeenGlobalInput(); }},
        {"setGuideSeenGlobalInput", [] { store::setGuideSeenGlobalInput(true); }},
        {"guideSeenGlobalOutput", [] { (void)store::guideSeenGlobalOutput(); }},
        {"setGuideSeenGlobalOutput", [] { store::setGuideSeenGlobalOutput(true); }},
        {"uiScalePercentInput", [] { (void)store::uiScalePercentInput(); }},
        {"setUiScalePercentInput", [] { store::setUiScalePercentInput(125); }},
        {"uiScalePercentOutput", [] { (void)store::uiScalePercentOutput(); }},
        {"setUiScalePercentOutput", [] { store::setUiScalePercentOutput(75); }},
        {"diagLogEnabled", [] { (void)store::diagLogEnabled(); }},
        {"setDiagLogEnabled", [] { store::setDiagLogEnabled(true); }},
        {"guideSeenGlobal", [] { (void)store::guideSeenGlobal("input"); }},
        {"setGuideSeenGlobal", [] { store::setGuideSeenGlobal("output", true); }},
        {"uiScalePercent", [] { (void)store::uiScalePercent("input"); }},
        {"setUiScalePercent", [] { store::setUiScalePercent("input", 65); }},
        {"readUiFlags", [] { (void)store::readUiFlags("input"); }},
    };

    for (const auto& call : calls)
    {
        INFO(call.first);
        const int enters = probe.enters;
        const int exits = probe.exits;
        call.second();
        CHECK(probe.enters == enters + 1);
        CHECK(probe.exits == exits + 1);
        CHECK(probe.depth == 0);
    }
    CHECK(probe.maxDepth == 1); // 从不嵌套
    for (const int wait : probe.waits)
        CHECK(wait == store::kProcessLockWaitMs);
}

TEST_CASE("ui defaults: a write that is refused up front does not need the lock", "[ui_defaults][lock]")
{
    ScratchStore scratch;
    LockProbe probe;
    store::setLangGlobal("de");
    store::setRulesAckVersion(-1);
    store::setUiScalePercentInput(9999);
    store::setUiScalePercentOutput(0);
    CHECK(probe.enters == 0);
    CHECK(probe.exits == 0);
    CHECK_FALSE(scratch.settingsFile().existsAsFile());
}

TEST_CASE("ui defaults: a write keeps what another process wrote while this one waited for the lock",
          "[ui_defaults][lock]")
{
    ScratchStore scratch;
    LockProbe probe;
    // 我们在等锁时，另一个进程写好了诊断日志开关；拿到锁之后才开文件，就读得到它，整体重写时不会把它丢掉
    probe.whileWaiting = [&scratch] { scratch.writeRaw(entry("diag_log_enabled", "1")); };

    store::setUiScalePercentInput(125);

    CHECK(probe.enters == 1);
    probe.whileWaiting = nullptr;
    CHECK(store::uiScalePercentInput() == 125);
    CHECK(store::diagLogEnabled()); // 别的进程写的那个键还在
}

TEST_CASE("ui defaults: a read sees what another process wrote while this one waited for the lock",
          "[ui_defaults][lock]")
{
    ScratchStore scratch;
    LockProbe probe;
    probe.whileWaiting = [&scratch] { scratch.writeRaw(entry("lang_global", "en")); };
    CHECK(store::langGlobal() == "en");

    probe.whileWaiting = [&scratch] {
        scratch.writeRaw(entry("guide_seen_global_input", "1") + "\n" + entry("rules_ack_version", "9"));
    };
    const auto flags = store::readUiFlags("input");
    CHECK(flags.guideSeen);
    CHECK(flags.rulesAck == 9);
}

TEST_CASE("ui defaults: the value is already on disk when the lock is released", "[ui_defaults][lock]")
{
    ScratchStore scratch;
    LockProbe probe;
    juce::String onDiskAtRelease;
    probe.atRelease = [&] { onDiskAtRelease = scratch.settingsFile().loadFileAsString(); };

    store::setUiScalePercentOutput(150);

    // 别的进程拿到锁时读到的就是这份：写在放锁之前落了盘，不是之后
    CHECK(onDiskAtRelease.contains("name=\"ui_scale_percent_output\""));
    CHECK(onDiskAtRelease.contains("val=\"150\""));
}

TEST_CASE("ui defaults: when the lock cannot be taken the work still happens and nothing is released",
          "[ui_defaults][lock]")
{
    ScratchStore scratch;
    LockProbe probe;
    probe.grant = false;

    // 别的进程卡死时不能让开窗跟着失败：不带锁照常读写，代价只是这一次可能丢更新
    store::setLangGlobal("fr");
    CHECK(store::langGlobal() == "fr");
    store::setUiScalePercentInput(80);
    CHECK(store::readUiFlags("input").rulesAck == 0);

    CHECK(probe.enters == 4);
    CHECK(probe.exits == 0); // 没拿到就不放
    CHECK(probe.depth == 0);
}

TEST_CASE("ui defaults: with the stand-in removed the real lock is used and values still round-trip",
          "[ui_defaults][lock]")
{
    ScratchStore scratch;
    {
        LockProbe probe; // 装上再拆掉
    }
    store::setUiScalePercentOutput(200);
    store::setDiagLogEnabled(true);
    CHECK(store::uiScalePercentOutput() == 200);
    CHECK(store::diagLogEnabled());
    // 真锁可重入：同一个线程连着读写不会卡住（上面每一步都拿过放过）
    CHECK(store::readUiFlags("output").diagLog);
}

TEST_CASE("ui defaults: readUiFlags agrees with the single reads, field by field", "[ui_defaults][flags]")
{
    ScratchStore scratch;

    // 全新：全部默认，而且只读不建文件
    {
        const auto flags = store::readUiFlags("input");
        CHECK_FALSE(flags.langChosen);
        CHECK(flags.rulesAck == 0);
        CHECK_FALSE(flags.guideSeen);
        CHECK_FALSE(flags.diagLog);
        CHECK_FALSE(scratch.settingsFile().existsAsFile());
    }

    store::setLangChosenGlobal(true);
    store::setRulesAckVersion(4);
    store::setGuideSeenGlobalInput(true);
    store::setDiagLogEnabled(true);

    for (const char* role : {"input", "output", "anything"})
    {
        INFO(role);
        const auto flags = store::readUiFlags(role);
        CHECK(flags.langChosen == store::langChosenGlobal());
        CHECK(flags.rulesAck == store::rulesAckVersion());
        CHECK(flags.guideSeen == store::guideSeenGlobal(role));
        CHECK(flags.diagLog == store::diagLogEnabled());
    }

    // 各字段读各自的键：一个为真另一个为假时不能串（上面的断言里它们恰好同真）
    store::setLangChosenGlobal(true);
    store::setDiagLogEnabled(false);
    CHECK(store::readUiFlags("input").langChosen);
    CHECK_FALSE(store::readUiFlags("input").diagLog);
    store::setLangChosenGlobal(false);
    store::setDiagLogEnabled(true);
    CHECK_FALSE(store::readUiFlags("input").langChosen);
    CHECK(store::readUiFlags("input").diagLog);
    store::setLangChosenGlobal(true);
    store::setDiagLogEnabled(true);

    // 引导已看按角色取：Input 看过、Output 没看过；其余角色名当 Output
    CHECK(store::readUiFlags("input").guideSeen);
    CHECK_FALSE(store::readUiFlags("output").guideSeen);
    CHECK_FALSE(store::readUiFlags("monitor").guideSeen);
    store::setGuideSeenGlobalOutput(true);
    CHECK(store::readUiFlags("output").guideSeen);
    CHECK(store::readUiFlags("monitor").guideSeen);

    // 取消也读得回来
    store::setLangChosenGlobal(false);
    store::setDiagLogEnabled(false);
    const auto cleared = store::readUiFlags("input");
    CHECK_FALSE(cleared.langChosen);
    CHECK_FALSE(cleared.diagLog);
    CHECK(cleared.rulesAck == 4);
}

TEST_CASE("ui defaults: readUiFlags validates what is on disk like the single reads do", "[ui_defaults][flags]")
{
    ScratchStore scratch;

    scratch.writeRaw(entry("guide_seen_global_input", "banana") + "\n" + entry("diag_log_enabled", "1") + "\n" +
                     entry("guide_seen_global_output", "1") + "\n" + entry("rules_ack_version", "-4") + "\n" +
                     entry("lang_chosen_global", "1"));
    auto flags = store::readUiFlags("input");
    CHECK_FALSE(flags.guideSeen); // 垃圾值按没看过
    CHECK(flags.diagLog);
    CHECK(flags.langChosen);
    CHECK(flags.rulesAck == 0); // 负数按 0
    CHECK(store::readUiFlags("output").guideSeen);

    scratch.writeRaw(entry("rules_ack_version", "2"));
    CHECK(store::readUiFlags("input").rulesAck == 2);

    // 整个文件损坏时全部回到默认，不抛出
    REQUIRE(scratch.settingsFile().replaceWithText("this is not xml"));
    flags = store::readUiFlags("input");
    CHECK_FALSE(flags.langChosen);
    CHECK(flags.rulesAck == 0);
    CHECK_FALSE(flags.guideSeen);
    CHECK_FALSE(flags.diagLog);
}
