// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/plugin-common.json
#include "UiDefaultsStore.h"

#include <juce_data_structures/juce_data_structures.h>

#include <memory>

#include "BridgeBase.h"

namespace rinne::uidefaults
{

namespace
{

constexpr const char* kKeyLangChosen = "lang_chosen_global";
constexpr const char* kKeyLang = "lang_global";
constexpr const char* kKeyRulesAck = "rules_ack_version";
constexpr const char* kKeyGuideSeenInput = "guide_seen_global_input";
constexpr const char* kKeyGuideSeenOutput = "guide_seen_global_output";
constexpr const char* kKeyUiScaleInput = "ui_scale_percent_input";
constexpr const char* kKeyUiScaleOutput = "ui_scale_percent_output";
constexpr const char* kKeyDiagLog = "diag_log_enabled";

// 缩放百分比的范围校验：夹取后不变就是在范围内。边界的真源是桥层那一对常量
bool inRange(int percent)
{
    return bridge::clampUiScalePercent(percent) == percent;
}

// 测试注入的落盘目录；空表示用默认位置。只由 setStorageDirForTests 写，只在消息线程或单测线程访问
juce::File& testDirRef()
{
    static juce::File dir;
    return dir;
}

std::unique_ptr<juce::PropertiesFile> openFile()
{
    juce::PropertiesFile::Options options;
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    // 0 表示 setValue 即刻落盘。默认值会起一个延迟保存的定时器，那既依赖消息管理器存活，
    // 也会在宿主强杀进程时丢掉刚写的值
    options.millisecondsBeforeSaving = 0;

    const juce::File& testDir = testDirRef();
    if (testDir != juce::File())
        return std::make_unique<juce::PropertiesFile>(testDir.getChildFile("ui-defaults.settings"), options);

    options.applicationName = "ui-defaults";
    options.folderName = "Synchain/Rinne";
    options.filenameSuffix = "settings";
    options.osxLibrarySubFolder = "Application Support";
    options.commonToAllUsers = false;
    return std::make_unique<juce::PropertiesFile>(options);
}

bool readBool(const char* key)
{
    const auto f = openFile();
    return f != nullptr && f->getBoolValue(key, false);
}

void writeBool(const char* key, bool value)
{
    const auto f = openFile();
    if (f == nullptr)
        return;
    f->setValue(key, value);
    f->saveIfNeeded(); // 即刻落盘模式下已经写过；显式再来一次，免得以后改了选项就静默丢写
}

int readPercent(const char* key)
{
    const auto f = openFile();
    if (f == nullptr)
        return 0;
    const int percent = f->getIntValue(key, 0);
    return inRange(percent) ? percent : 0;
}

void writePercent(const char* key, int percent)
{
    if (!inRange(percent))
        return;
    const auto f = openFile();
    if (f == nullptr)
        return;
    f->setValue(key, percent);
    f->saveIfNeeded();
}

} // namespace

bool langChosenGlobal()
{
    return readBool(kKeyLangChosen);
}

void setLangChosenGlobal(bool chosen)
{
    writeBool(kKeyLangChosen, chosen);
}

juce::String langGlobal()
{
    const auto f = openFile();
    if (f == nullptr)
        return {};
    const juce::String v = f->getValue(kKeyLang, juce::String());
    // 磁盘上是用户可编辑的 XML，来路不明的值不许进界面语言位。白名单与桥层是同一张表，
    // 漏掉一种语言比不做白名单更坏：漏掉的那种语言的用户选了语言却存不下来，
    // 而「选过」那个布尔照写，重载后语言回落、起始卡又被挡住
    return bridge::isSupportedLang(v) ? v : juce::String();
}

void setLangGlobal(const juce::String& lang)
{
    if (!bridge::isSupportedLang(lang))
        return;
    const auto f = openFile();
    if (f == nullptr)
        return;
    f->setValue(kKeyLang, lang);
    f->saveIfNeeded();
}

int rulesAckVersion()
{
    const auto f = openFile();
    if (f == nullptr)
        return 0;
    const int version = f->getIntValue(kKeyRulesAck, 0);
    return version > 0 ? version : 0;
}

void setRulesAckVersion(int version)
{
    if (version < 0)
        return;
    const auto f = openFile();
    if (f == nullptr)
        return;
    f->setValue(kKeyRulesAck, version);
    f->saveIfNeeded();
}

bool guideSeenGlobalInput()
{
    return readBool(kKeyGuideSeenInput);
}

void setGuideSeenGlobalInput(bool seen)
{
    writeBool(kKeyGuideSeenInput, seen);
}

bool guideSeenGlobalOutput()
{
    return readBool(kKeyGuideSeenOutput);
}

void setGuideSeenGlobalOutput(bool seen)
{
    writeBool(kKeyGuideSeenOutput, seen);
}

int uiScalePercentInput()
{
    return readPercent(kKeyUiScaleInput);
}

void setUiScalePercentInput(int percent)
{
    writePercent(kKeyUiScaleInput, percent);
}

int uiScalePercentOutput()
{
    return readPercent(kKeyUiScaleOutput);
}

void setUiScalePercentOutput(int percent)
{
    writePercent(kKeyUiScaleOutput, percent);
}

bool diagLogEnabled()
{
    return readBool(kKeyDiagLog);
}

void setDiagLogEnabled(bool enabled)
{
    writeBool(kKeyDiagLog, enabled);
}

bool guideSeenGlobal(const juce::String& role)
{
    return role == "input" ? guideSeenGlobalInput() : guideSeenGlobalOutput();
}

void setGuideSeenGlobal(const juce::String& role, bool seen)
{
    if (role == "input")
        setGuideSeenGlobalInput(seen);
    else
        setGuideSeenGlobalOutput(seen);
}

int uiScalePercent(const juce::String& role)
{
    return role == "input" ? uiScalePercentInput() : uiScalePercentOutput();
}

void setUiScalePercent(const juce::String& role, int percent)
{
    if (role == "input")
        setUiScalePercentInput(percent);
    else
        setUiScalePercentOutput(percent);
}

void setStorageDirForTests(const juce::File& dir)
{
    testDirRef() = dir;
}

} // namespace rinne::uidefaults
