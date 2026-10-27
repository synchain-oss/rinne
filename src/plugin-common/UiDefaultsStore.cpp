// SPDX-License-Identifier: GPL-3.0-or-later
// Derived from synchain-oss/scvb; see upstream/plugin-common.json
#include "UiDefaultsStore.h"

#include <juce_data_structures/juce_data_structures.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>

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

// 进程间锁：函数内静态，第一次用时才建，活到进程或插件库卸载，比任何一次读写都长
juce::InterProcessLock& processLock()
{
    static juce::InterProcessLock lock{kProcessLockName};
    return lock;
}

// 测试替身：锁的两个函数都有才顶替真锁；时钟替身独立，有就顶替单调时钟。
// 只由 setLockForTests 写，只在消息线程或单测线程访问
struct LockStandIn
{
    std::function<bool(int)> enter;
    std::function<void()> exit;
    std::function<std::int64_t()> nowMs;
};

LockStandIn& lockStandIn()
{
    static LockStandIn standIn;
    return standIn;
}

// 单调毫秒。避让窗口只许用单调时钟：墙钟会被校时或用户改时间，向后一跳就让窗口永不结束，
// 向前一跳又让它立刻结束
using MonotonicClock = std::chrono::steady_clock;
static_assert(MonotonicClock::is_steady, "the back-off window must be measured on a monotonic clock");

std::int64_t monotonicMs()
{
    const auto& standIn = lockStandIn();
    if (standIn.nowMs)
        return standIn.nowMs();
    return std::chrono::duration_cast<std::chrono::milliseconds>(MonotonicClock::now().time_since_epoch()).count();
}

// 拿锁超时之后的避让状态：窗口从第一次超时那一刻起算。只在消息线程或单测线程访问
struct LockBackoff
{
    bool active = false;
    std::int64_t sinceMs = 0;
};

LockBackoff& lockBackoff()
{
    static LockBackoff backoff;
    return backoff;
}

enum class Access
{
    read,
    write
};

// 这一次该等多久。窗口过期就在这里收掉，之后这次按满额等：卡住的锁每个窗口只让消息线程多等一次满额
int lockWaitMs(Access access, bool& inBackoff)
{
    auto& backoff = lockBackoff();
    if (backoff.active && monotonicMs() - backoff.sinceMs >= kProcessLockBackoffMs)
        backoff.active = false;
    inBackoff = backoff.active;
    if (inBackoff)
        return access == Access::read ? kProcessLockBackoffReadWaitMs : kProcessLockBackoffWriteWaitMs;
    return access == Access::read ? kProcessLockReadWaitMs : kProcessLockWriteWaitMs;
}

// 记下这次拿锁的结果。拿到了说明锁没卡住，立刻恢复满额等待；没拿到且不在窗口里，开一个新窗口
// （窗口里再超时不顺延，否则一个一直卡着的锁会让我们永远不再试满额）
void noteLockResult(bool held, bool wasInBackoff)
{
    auto& backoff = lockBackoff();
    if (held)
    {
        backoff.active = false;
    }
    else if (!wasInBackoff)
    {
        backoff.active = true;
        backoff.sinceMs = monotonicMs();
    }
}

std::unique_ptr<juce::PropertiesFile> openFile(juce::InterProcessLock* heldLock)
{
    juce::PropertiesFile::Options options;
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    // 0 表示 setValue 即刻落盘。默认值会起一个延迟保存的定时器，那既依赖消息管理器存活，
    // 也会在宿主强杀进程时丢掉刚写的值
    options.millisecondsBeforeSaving = 0;
    // JUCE 在读盘和写盘时各自再拿一次这把锁。我们已经在外层拿到了它（同一个对象，计数加一，不会再等），
    // 所以这里只是兜底；没拿到时必须传空：JUCE 拿锁没有等待上限，传了就会在别人卡死时无限等
    options.processLock = heldLock;

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

// 一次读或写的作用域：先拿进程间锁，再开文件；析构时先关文件（没落盘的写在这一步落盘），后放锁。
// 每个对外函数只开这样一个作用域，读写都整段在锁里。拿不到锁（超时，或系统不给）时不带锁照常做
class StoreSession
{
public:
    explicit StoreSession(Access access)
    {
        bool inBackoff = false;
        const int waitMs = lockWaitMs(access, inBackoff);
        auto& standIn = lockStandIn();
        if (standIn.enter && standIn.exit)
        {
            viaStandIn_ = true;
            held_ = standIn.enter(waitMs);
        }
        else
        {
            held_ = processLock().enter(waitMs);
        }
        noteLockResult(held_, inBackoff);
        file_ = openFile(held_ && !viaStandIn_ ? &processLock() : nullptr);
    }

    ~StoreSession()
    {
        file_.reset();
        if (!held_)
            return;
        if (viaStandIn_)
            lockStandIn().exit();
        else
            processLock().exit();
    }

    StoreSession(const StoreSession&) = delete;
    StoreSession& operator=(const StoreSession&) = delete;

    juce::PropertiesFile* file() const { return file_.get(); }

private:
    bool viaStandIn_ = false;
    bool held_ = false;
    std::unique_ptr<juce::PropertiesFile> file_;
};

// 各键的取值与校验，单个读取函数和一次读回共用，两条路不会各写各的
bool boolOf(const juce::PropertiesFile& f, const char* key)
{
    return f.getBoolValue(key, false);
}

int percentOf(const juce::PropertiesFile& f, const char* key)
{
    const int percent = f.getIntValue(key, 0);
    return inRange(percent) ? percent : 0;
}

int rulesAckOf(const juce::PropertiesFile& f)
{
    const int version = f.getIntValue(kKeyRulesAck, 0);
    return version > 0 ? version : 0;
}

const char* guideKey(const juce::String& role)
{
    return role == "input" ? kKeyGuideSeenInput : kKeyGuideSeenOutput;
}

bool readBool(const char* key)
{
    const StoreSession session(Access::read);
    const auto* f = session.file();
    return f != nullptr && boolOf(*f, key);
}

void writeBool(const char* key, bool value)
{
    const StoreSession session(Access::write);
    auto* f = session.file();
    if (f == nullptr)
        return;
    f->setValue(key, value);
    f->saveIfNeeded(); // 即刻落盘模式下已经写过；显式再来一次，免得以后改了选项就静默丢写
}

int readPercent(const char* key)
{
    const StoreSession session(Access::read);
    const auto* f = session.file();
    return f == nullptr ? 0 : percentOf(*f, key);
}

void writePercent(const char* key, int percent)
{
    if (!inRange(percent))
        return;
    const StoreSession session(Access::write);
    auto* f = session.file();
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
    const StoreSession session(Access::read);
    const auto* f = session.file();
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
    const StoreSession session(Access::write);
    auto* f = session.file();
    if (f == nullptr)
        return;
    f->setValue(kKeyLang, lang);
    f->saveIfNeeded();
}

void chooseLangGlobal(const juce::String& lang)
{
    if (!bridge::isSupportedLang(lang))
        return;
    const StoreSession session(Access::write);
    auto* f = session.file();
    if (f == nullptr)
        return;
    // 两个键在同一个会话里写完：一次拿锁、一次开文件。即刻落盘模式下每个键各写一次盘，但都在同一把锁里，
    // 拿锁的别的进程看不到只写了一半的状态。
    // 先写语言再写「选过」：中途被强杀时宁可有语言没「选过」（起始卡多问一次），也不要有「选过」没语言
    f->setValue(kKeyLang, lang);
    f->setValue(kKeyLangChosen, true);
    f->saveIfNeeded();
}

int rulesAckVersion()
{
    const StoreSession session(Access::read);
    const auto* f = session.file();
    return f == nullptr ? 0 : rulesAckOf(*f);
}

void setRulesAckVersion(int version)
{
    if (version < 0)
        return;
    const StoreSession session(Access::write);
    auto* f = session.file();
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

bridge::UiFlags readUiFlags(const juce::String& role)
{
    bridge::UiFlags flags;
    const StoreSession session(Access::read);
    const auto* f = session.file();
    if (f == nullptr)
        return flags;
    flags.langChosen = boolOf(*f, kKeyLangChosen);
    flags.rulesAck = rulesAckOf(*f);
    flags.guideSeen = boolOf(*f, guideKey(role));
    flags.diagLog = boolOf(*f, kKeyDiagLog);
    return flags;
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

void setLockForTests(std::function<bool(int)> enter, std::function<void()> exit, std::function<std::int64_t()> nowMs)
{
    auto& standIn = lockStandIn();
    standIn.enter = std::move(enter);
    standIn.exit = std::move(exit);
    standIn.nowMs = std::move(nowMs);
    lockBackoff() = LockBackoff{}; // 换了替身就是新的一局，上一局留下的避让窗口不能带过来
}

} // namespace rinne::uidefaults
