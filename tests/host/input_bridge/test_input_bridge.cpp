// SPDX-License-Identifier: GPL-3.0-or-later
//
// Input 的原生桥（InputBridge，编辑器的纯逻辑那一层）接在真处理器上：每个角色函数的回执与失败码、各事件的字段集合
// （拿不到的可缺省字段确实不出现）、silentCause 的四个来源、notices 的五条、单位与预设、Panic 与恢复、三盏灯的判据、
// 首帧与 1 Hz 兜底。
// 时钟、剪贴板、浏览器与许可证都是替身；全局设置写进测试沙盒的目录，用户预设写进每个用例自己的临时目录。

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "../pairing/PairingKit.h"
#include "InputBridge.h"
#include "UiDefaultsStore.h"
#include "engine/SafetyMachine.h"
#include "state/StateCodec.h"

namespace
{

using namespace rinne::pairing_test;
using rinne::InputProcessor;
using rinne::OutputProcessor;
using rinne::input::addPeerAudioFacts;
using rinne::input::buildInfo;
using rinne::input::buildLamps;
using rinne::input::buildNotices;
using rinne::input::delayReasonOf;
using rinne::input::InputBridge;
using rinne::input::isL2;
using rinne::input::LampFacts;
using rinne::input::levelDb;
using rinne::input::NoticeFacts;
using rinne::input::peerBypassOf;
using rinne::input::silentCauseOf;
using rinne::input::SilentFacts;
namespace diag = rinne::diag;
namespace engine = rinne::engine;
namespace uidefaults = rinne::uidefaults;
namespace wiring_test = rinne::wiring_test;

juce::Array<juce::var> args(std::initializer_list<juce::var> list)
{
    juce::Array<juce::var> out;
    for (const auto& v : list)
        out.add(v);
    return out;
}

bool isOk(const juce::var& response)
{
    const auto ok = response.getProperty("ok", juce::var());
    return ok.isBool() && static_cast<bool>(ok);
}

juce::String reasonOf(const juce::var& response)
{
    return response.getProperty("reason", juce::var()).toString();
}

std::set<std::string> keysOf(const juce::var& object)
{
    std::set<std::string> keys;
    if (auto* o = object.getDynamicObject())
        for (const auto& entry : o->getProperties())
            keys.insert(entry.name.toString().toStdString());
    return keys;
}

// 替身记下的系统动作
struct Recorded
{
    std::vector<juce::String> clipboard;
    bool clipboardWorks = true;
    std::vector<juce::String> launched;
    int licenses = 0;
    bool licenseWorks = true;
};

InputBridge::Hooks hooksFor(Bench& bench, Recorded& rec)
{
    InputBridge::Hooks hooks;
    hooks.nowMs = [&bench] { return bench.nowMs(); };
    hooks.writeClipboard = [&rec](const juce::String& text) {
        rec.clipboard.push_back(text);
        return rec.clipboardWorks;
    };
    hooks.launchUrl = [&rec](const juce::String& url) {
        rec.launched.push_back(url);
        return true;
    };
    hooks.showLicense = [&rec] {
        ++rec.licenses;
        return rec.licenseWorks;
    };
    return hooks;
}

// 这个用例自己的用户预设目录（本进程新建的）。只删这一层的文件，不递归
class TempPresetDir
{
public:
    TempPresetDir()
        : dir_(juce::File::getSpecialLocation(juce::File::tempDirectory)
                   .getChildFile("rinne-input-bridge-presets-" + juce::String::toHexString(juce::Random().nextInt64())))
    {
        dir_.createDirectory();
    }
    ~TempPresetDir()
    {
        for (const auto& f : dir_.findChildFiles(juce::File::findFiles, false))
            f.deleteFile();
        dir_.deleteFile();
    }
    const juce::File& dir() const noexcept { return dir_; }

private:
    juce::File dir_;
};

InputProcessor& addInputIn(Bench& bench, const juce::File& presets, bool prepare = true)
{
    return bench.add(std::make_unique<InputProcessor>(bench.config(), presets), prepare);
}

// 一个 Input 加它的桥。声明顺序就是拆的逆序：桥先走，处理器随 Bench 走，临时目录最后
struct Rig
{
    explicit Rig(bool prepare = true)
        : input(addInputIn(bench, presets.dir(), prepare)), bridge(input, hooksFor(bench, rec))
    {
    }

    TempPresetDir presets;
    Bench bench;
    Recorded rec;
    InputProcessor& input;
    InputBridge bridge;
    InputBridge::UiContext ui;
};

// 一帧 rinne.state 的 Input 部分（快照里共用的那几项由 WebViewHost 补，不在这一层）
juce::var stateFields(InputBridge& bridge)
{
    auto* o = new juce::DynamicObject();
    juce::var v(o);
    bridge.addStateFields(*o);
    return v;
}

juce::var noticeOf(const juce::var& fields, const char* id)
{
    const auto notices = fields.getProperty("notices", juce::var());
    if (auto* list = notices.getArray())
        for (const auto& n : *list)
            if (n.getProperty("id", juce::var()).toString() == id)
                return n;
    return {};
}

float plain(InputProcessor& input, const char* id)
{
    return input.parameterLayer()->tree().getRawParameterValue(id)->load();
}

engine::Meters& metersOf(InputProcessor& input)
{
    return input.instanceHost().inputMeters();
}

void setSafety(InputProcessor& input, engine::SafetyState state, engine::TripReason trip = engine::TripReason::None,
               bool cooling = false)
{
    metersOf(input).safety.store(engine::PackedSafety::pack(state, trip, engine::FadeReason::Load, cooling));
}

juce::var safetyOf(InputBridge& bridge)
{
    return stateFields(bridge).getProperty("safety", {});
}

juce::var lampsOf(const InputBridge::Frames& frames)
{
    return frames.meters.getProperty("lamps", {});
}

// 宿主那边看到的东西：参数变更与手势
class HostListener final : public juce::AudioProcessorListener
{
public:
    void audioProcessorParameterChanged(juce::AudioProcessor*, int, float) override { ++changes; }
    void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails& details) override
    {
        if (details.nonParameterStateChanged && details.programChanged)
            ++dirtyMarks;
    }
    void audioProcessorParameterChangeGestureBegin(juce::AudioProcessor*, int) override { ++begins; }
    void audioProcessorParameterChangeGestureEnd(juce::AudioProcessor*, int) override { ++ends; }

    int changes = 0;
    int begins = 0;
    int ends = 0;
    int dirtyMarks = 0;
};

// 只拒绝的后端：会合段与对端视图一律打不开
class RefusingBackend final : public ipc::ISegmentBackend
{
public:
    explicit RefusingBackend(ipc::InitResult result) : result_(result) {}
    ipc::InitResult createOrOpen(const std::string&, std::size_t, ipc::SegmentView&) override { return result_; }
    ipc::InitResult openExisting(const std::string&, ipc::SegmentView&) override { return result_; }
    void unmap(ipc::SegmentView& view) noexcept override { view = ipc::SegmentView(); }

private:
    ipc::InitResult result_;
};

} // namespace

// ---------------------------------------------------------------- 共用函数的回执

TEST_CASE("input bridge: the global-flag functions answer with the documented receipts", "[host][input_bridge]")
{
    formalOnly();
    Rig rig;
    auto& bridge = rig.bridge;

    SECTION("setRulesAck takes a non-negative integer")
    {
        CHECK(isOk(bridge.setRulesAck(args({juce::var(3)}))));
        CHECK(uidefaults::rulesAckVersion() == 3);
        for (const auto& bad : {args({}), args({juce::var(-1)}), args({juce::var("3")}), args({juce::var(1.5)})})
            CHECK(reasonOf(bridge.setRulesAck(bad)) == "bad-version");
        CHECK(uidefaults::rulesAckVersion() == 3);
    }
    SECTION("setGuideSeen only takes true and writes the Input flag")
    {
        for (const auto& bad : {args({}), args({juce::var(false)}), args({juce::var(1)}), args({juce::var("true")})})
            CHECK(reasonOf(bridge.setGuideSeen(bad)) == "bad-value");
        CHECK_FALSE(uidefaults::guideSeenGlobalInput());
        CHECK(isOk(bridge.setGuideSeen(args({juce::var(true)}))));
        CHECK(uidefaults::guideSeenGlobalInput());
        CHECK_FALSE(uidefaults::guideSeenGlobalOutput());
    }
    SECTION("setDiagLog takes a boolean and echoes it")
    {
        const auto on = bridge.setDiagLog(args({juce::var(true)}));
        CHECK(isOk(on));
        CHECK(static_cast<bool>(on.getProperty("value", juce::var())));
        CHECK(uidefaults::diagLogEnabled());
        for (const auto& bad : {args({}), args({juce::var(0)}), args({juce::var("false")})})
            CHECK(reasonOf(bridge.setDiagLog(bad)) == "bad-value");
        CHECK(isOk(bridge.setDiagLog(args({juce::var(false)}))));
        CHECK_FALSE(uidefaults::diagLogEnabled());
    }
    SECTION("none of them settles the instance or marks the project dirty")
    {
        DirtyListener dirty;
        rig.input.addListener(&dirty);
        bridge.setRulesAck(args({juce::var(2)}));
        bridge.setGuideSeen(args({juce::var(true)}));
        bridge.setDiagLog(args({juce::var(true)}));
        CHECK(dirty.dirtyMarks == 0);
        CHECK_FALSE(settled(rig.input));
        rig.input.removeListener(&dirty);
    }
}

TEST_CASE("input bridge: setGroup, openExternal, showLicense and resolveTakeover receipts", "[host][input_bridge]")
{
    formalOnly();
    Rig rig;
    auto& bridge = rig.bridge;
    DirtyListener dirty;
    rig.input.addListener(&dirty);

    SECTION("setGroup takes a letter, settles and marks dirty")
    {
        for (const auto& bad : {args({}), args({juce::var("Z")}), args({juce::var("c")}), args({juce::var(3)})})
            CHECK(reasonOf(bridge.setGroup(bad)) == "bad-group");
        CHECK(dirty.dirtyMarks == 0);
        const auto r = bridge.setGroup(args({juce::var("C")}));
        CHECK(isOk(r));
        CHECK(r.getProperty("group", juce::var()).toString() == "C");
        CHECK(configsOf(rig.input).group == 3);
        CHECK(configsOf(rig.input).groupOrigin == state::GroupOrigin::User);
        CHECK(settled(rig.input));
        CHECK(dirty.dirtyMarks == 1);
    }
    SECTION("openExternal goes through the whitelist before the launcher")
    {
        CHECK(isOk(bridge.openExternal(args({juce::var("https://github.com/synchain-oss/rinne/issues")}))));
        CHECK(reasonOf(bridge.openExternal(args({juce::var("https://github.com/synchain-oss/rinne?x")}))) ==
              "not-allowed");
        REQUIRE(rig.rec.launched.size() == 1);
        CHECK_FALSE(settled(rig.input));
    }
    SECTION("showLicense reports whether the system opened it")
    {
        CHECK(isOk(bridge.showLicense()));
        rig.rec.licenseWorks = false;
        CHECK(reasonOf(bridge.showLicense()) == "unavailable");
        CHECK(rig.rec.licenses == 2);
    }
    SECTION("resolveTakeover with nothing pending")
    {
        CHECK(reasonOf(bridge.resolveTakeover(args({juce::var(true)}))) == "no-pending");
        CHECK(reasonOf(bridge.resolveTakeover(args({juce::var(false)}))) == "no-pending");
        CHECK(reasonOf(bridge.resolveTakeover(args({juce::var(1)}))) == "bad-value");
        CHECK(reasonOf(bridge.resolveTakeover(args({}))) == "bad-value");
        CHECK_FALSE(settled(rig.input));
    }
    rig.input.removeListener(&dirty);
}

TEST_CASE("input bridge: copyDiagnostics previews and copies the text with the ui line", "[host][input_bridge][diag]")
{
    formalOnly();
    Rig rig;
    rig.bench.run(100);
    InputBridge::UiContext ui;
    ui.lang = "en";
    ui.uiScale = 1.5f;
    ui.webview = "WebView2 131.0.2903.70";

    const auto preview = rig.bridge.copyDiagnostics(args({juce::var(false)}), ui);
    REQUIRE(isOk(preview));
    CHECK_FALSE(static_cast<bool>(preview.getProperty("copied", {})));
    const juce::String text = preview.getProperty("text", {}).toString();
    CHECK(text.startsWith("rinne-diag v1\n"));
    CHECK(text.endsWith("\nui: webview=WebView2-131.0.2903.70 scale=1.50 lang=en"));
    CHECK(rig.rec.clipboard.empty());

    const auto copied = rig.bridge.copyDiagnostics(args({juce::var(true)}), ui);
    CHECK(isOk(copied));
    REQUIRE(rig.rec.clipboard.size() == 1);
    CHECK(rig.rec.clipboard.front() == copied.getProperty("text", {}).toString());

    rig.rec.clipboardWorks = false;
    const auto failed = rig.bridge.copyDiagnostics(args({juce::var(true)}), ui);
    CHECK_FALSE(isOk(failed));
    CHECK(reasonOf(failed) == "clipboard");
    CHECK_FALSE(settled(rig.input));
}

TEST_CASE("input bridge: moveToFreeGroup outside a conflict", "[host][input_bridge]")
{
    formalOnly();
    Rig rig;
    rig.bench.run(200);
    const auto r = rig.bridge.moveToFreeGroup();
    CHECK_FALSE(isOk(r));
    CHECK(reasonOf(r) == "not-conflict");
    CHECK_FALSE(settled(rig.input));
}

TEST_CASE("input bridge: moveToFreeGroup re-pairs a duplicated Input", "[host][input_bridge]")
{
    formalOnly();
    needsSharedMemory();
    TempPresetDir presets;
    Bench bench;
    InputProcessor& original = addInputIn(bench, presets.dir());
    REQUIRE(original.selectGroup(1));
    InputProcessor& copy = addInputIn(bench, presets.dir());
    restore(copy, groupState(state::Role::Input, 1));
    bench.run(300);
    REQUIRE(stateOf(copy) == ipc::RendezvousState::kConflict);

    Recorded rec;
    InputBridge bridge(copy, hooksFor(bench, rec));
    // 组冲突是 L2 条件之一
    CHECK(static_cast<bool>(safetyOf(bridge).getProperty("l2", {})));
    const auto r = bridge.moveToFreeGroup();
    REQUIRE(isOk(r));
    CHECK(r.getProperty("group", juce::var()).toString() == "B");
    CHECK(settled(copy));
    CHECK(runUntil(bench, [&] { return holds(copy, 2); }, 2000));
    CHECK_FALSE(bridge.pendingTakeover().has_value());
    CHECK_FALSE(static_cast<bool>(safetyOf(bridge).getProperty("l2", {})));
}

TEST_CASE("input bridge: taking over only accepts a real group", "[host][input_bridge]")
{
    formalOnly();
    Rig rig;
    const auto before = configsOf(rig.input).group;
    for (const int bad : {0, -1, static_cast<int>(ipc::kGroupCount) + 1})
    {
        INFO(bad);
        CHECK_FALSE(rig.input.takeOverGroup(bad));
    }
    CHECK(configsOf(rig.input).group == before);
    CHECK_FALSE(settled(rig.input));
}

TEST_CASE("input bridge: a repair onto a dormant slot asks first, then takes it over or cancels",
          "[host][input_bridge][takeover]")
{
    formalOnly();
    needsSharedMemory();
    TempPresetDir presets;
    Bench bench;
    std::vector<InputProcessor*> holders;
    for (int group = 1; group <= 8; ++group)
    {
        InputProcessor& holder = addInputIn(bench, presets.dir());
        REQUIRE(holder.selectGroup(group));
        holders.push_back(&holder);
    }
    InputProcessor& copy = addInputIn(bench, presets.dir());
    restore(copy, groupState(state::Role::Input, 1));
    bench.run(300);
    REQUIRE(stateOf(copy) == ipc::RendezvousState::kConflict);

    Recorded rec;
    InputBridge bridge(copy, hooksFor(bench, rec));

    const auto none = bridge.moveToFreeGroup();
    CHECK_FALSE(isOk(none));
    CHECK(reasonOf(none) == "no-free-group");
    CHECK_FALSE(bridge.pendingTakeover().has_value());
    CHECK_FALSE(stateFields(bridge).hasProperty("takeoverGroup"));

    holders[1]->releaseResources();
    bench.setRunning(*holders[1], false);
    bench.run(300);
    DirtyListener dirty;
    copy.addListener(&dirty);
    const auto asked = bridge.moveToFreeGroup();
    CHECK_FALSE(isOk(asked));
    CHECK(reasonOf(asked) == "needs-confirm");
    REQUIRE(bridge.pendingTakeover().has_value());
    CHECK(*bridge.pendingTakeover() == 2u);
    CHECK_FALSE(settled(copy));
    CHECK(dirty.dirtyMarks == 0);

    const auto fields = stateFields(bridge);
    CHECK(static_cast<bool>(fields.getProperty("takeover", juce::var())));
    CHECK(fields.getProperty("takeoverGroup", juce::var()).toString() == "B");
    CHECK(fields.getProperty("group", juce::var()).toString() == "A");

    SECTION("accept takes the dormant slot with the confirmed intent")
    {
        const auto r = bridge.resolveTakeover(args({juce::var(true)}));
        CHECK(isOk(r));
        CHECK(static_cast<bool>(r.getProperty("accepted", juce::var())));
        CHECK(settled(copy));
        CHECK(dirty.dirtyMarks == 1);
        CHECK(configsOf(copy).group == 2);
        CHECK(runUntil(bench, [&] { return holds(copy, 2); }, 4000));
        CHECK_FALSE(stateFields(bridge).hasProperty("takeoverGroup"));
    }
    SECTION("cancel leaves everything where it was")
    {
        const auto r = bridge.resolveTakeover(args({juce::var(false)}));
        CHECK(isOk(r));
        CHECK_FALSE(static_cast<bool>(r.getProperty("accepted", juce::var())));
        CHECK_FALSE(settled(copy));
        CHECK(dirty.dirtyMarks == 0);
        bench.run(300);
        CHECK(groupOf(copy) == 1);
    }
    SECTION("a takeover that does not go through answers no-pending")
    {
        auto hooks = hooksFor(bench, rec);
        hooks.takeOverGroup = [](int) { return false; };
        InputBridge refusing(copy, std::move(hooks));
        REQUIRE(reasonOf(refusing.moveToFreeGroup()) == "needs-confirm");
        const auto r = refusing.resolveTakeover(args({juce::var(true)}));
        CHECK(reasonOf(r) == "no-pending");
        CHECK_FALSE(settled(copy));
    }
    SECTION("picking a group by hand drops the pending takeover")
    {
        CHECK(isOk(bridge.setGroup(args({juce::var("A")}))));
        CHECK_FALSE(bridge.pendingTakeover().has_value());
    }
    SECTION("the conflict going away drops the pending takeover")
    {
        bench.remove(*holders[0]);
        CHECK(runUntil(bench, [&] { return holds(copy, 1); }, 4000));
        CHECK_FALSE(static_cast<bool>(stateFields(bridge).getProperty("takeover", juce::var())));
        CHECK_FALSE(bridge.pendingTakeover().has_value());
    }
    copy.removeListener(&dirty);
}

// ---------------------------------------------------------------- 参数与手势

TEST_CASE("input bridge: setParam validates by kind, clamps and answers the actual value",
          "[host][input_bridge][params]")
{
    formalOnly();
    Rig rig;
    auto& bridge = rig.bridge;
    const auto set = [&bridge](const char* id, const juce::var& v) {
        return bridge.setParam(args({juce::var(id), v}));
    };

    SECTION("unknown or non-string ids")
    {
        CHECK(reasonOf(bridge.setParam(args({}))) == "bad-param");
        CHECK(reasonOf(bridge.setParam(args({juce::var(1), juce::var(1)}))) == "bad-param");
        CHECK(reasonOf(set("nope", juce::var(1))) == "bad-param");
        CHECK(reasonOf(bridge.beginParamGesture(args({juce::var("nope")}))) == "bad-param");
        CHECK(reasonOf(bridge.beginParamGesture(args({}))) == "bad-param");
        CHECK(reasonOf(bridge.endParamGesture(args({juce::var("nope")}))) == "bad-param");
        CHECK_FALSE(settled(rig.input));
    }
    SECTION("a continuous parameter takes finite numbers only and clamps them")
    {
        for (const auto& bad : {juce::var("10"), juce::var(true), juce::var(), juce::var(std::nan("")),
                                juce::var(std::numeric_limits<double>::infinity())})
            CHECK(reasonOf(set("feedback", bad)) == "bad-value");
        CHECK(plain(rig.input, "feedback") == 0.0f);

        const auto r = set("feedback", juce::var(33.3));
        REQUIRE(isOk(r));
        CHECK(keysOf(r) == std::set<std::string>{"ok", "value", "seq"});
        CHECK(static_cast<double>(r.getProperty("value", {})) == 33.3);
        CHECK(settled(rig.input));
        CHECK(static_cast<double>(set("feedback", juce::var(500)).getProperty("value", {})) == 150.0);
        CHECK(static_cast<double>(set("feedback", juce::var(-1)).getProperty("value", {})) == 0.0);
        CHECK(static_cast<double>(set("ceiling", juce::var(-3.26)).getProperty("value", {})) == -3.3);
    }
    SECTION("a switch takes booleans only")
    {
        for (const auto& bad : {juce::var(0), juce::var("false"), juce::var()})
            CHECK(reasonOf(set("loop_on", bad)) == "bad-value");
        const auto r = set("loop_on", juce::var(false));
        REQUIRE(isOk(r));
        CHECK(r.getProperty("value", {}).isBool());
        CHECK_FALSE(static_cast<bool>(r.getProperty("value", {})));
        CHECK(plain(rig.input, "loop_on") == 0.0f);
    }
    SECTION("a choice takes the bridge name of a step")
    {
        for (const auto& bad : {juce::var(1), juce::var("Sync"), juce::var("n4"), juce::var()})
            CHECK(reasonOf(set("delay_mode", bad)) == "bad-value");
        CHECK(set("delay_mode", juce::var("sync")).getProperty("value", {}).toString() == "sync");
        CHECK(plain(rig.input, "delay_mode") == 1.0f);
        CHECK(set("delay_note", juce::var("n8t")).getProperty("value", {}).toString() == "n8t");
    }
    SECTION("setLimiter is setParam on limiter")
    {
        for (const auto& bad : {args({}), args({juce::var(0)}), args({juce::var("on")})})
            CHECK(reasonOf(bridge.setLimiter(bad)) == "bad-value");
        CHECK(plain(rig.input, "limiter") == 1.0f);
        const auto r = bridge.setLimiter(args({juce::var(false)}));
        REQUIRE(isOk(r));
        CHECK(keysOf(r) == std::set<std::string>{"ok", "value", "seq"});
        CHECK_FALSE(static_cast<bool>(r.getProperty("value", {})));
        CHECK(plain(rig.input, "limiter") == 0.0f);
    }
}

TEST_CASE("input bridge: the setParam receipt and the echo frame carry the same seq", "[host][input_bridge][params]")
{
    formalOnly();
    Rig rig;
    auto& bridge = rig.bridge;
    bridge.firstFrames(rig.ui);

    const auto r = bridge.setParam(args({juce::var("mix"), juce::var(40)}));
    REQUIRE(isOk(r));
    const auto frames = bridge.poll(rig.ui);
    REQUIRE_FALSE(frames.params.isVoid());
    CHECK(keysOf(frames.params) == std::set<std::string>{"seq", "values"});
    CHECK(frames.params.getProperty("seq", {}) == r.getProperty("seq", {}));
    CHECK(static_cast<double>(frames.params.getProperty("values", {}).getProperty("mix", {})) == 40.0);
    CHECK(bridge.poll(rig.ui).params.isVoid());

    // 写了同一个值也有回声：页面在等这个 seq
    const auto again = bridge.setParam(args({juce::var("mix"), juce::var(40)}));
    const auto echo = bridge.poll(rig.ui);
    REQUIRE_FALSE(echo.params.isVoid());
    CHECK(echo.params.getProperty("seq", {}) == again.getProperty("seq", {}));

    // 回执之后页面重发了请求：回声跟着首帧走，用的还是回执的 seq，下一拍不再补一帧
    const auto beforeFirst = bridge.setParam(args({juce::var("mix"), juce::var(41)}));
    const auto first = bridge.firstFrames(rig.ui);
    CHECK(first.params.getProperty("seq", {}) == beforeFirst.getProperty("seq", {}));
    CHECK(bridge.poll(rig.ui).params.isVoid());

    // 落在两个步进正中间：回执报的是参数实际落到的值，不是自己换算的，与回声逐字相同
    const auto tie = bridge.setParam(args({juce::var("ceiling"), juce::var(-3.25)}));
    REQUIRE(isOk(tie));
    const auto tieEcho = bridge.poll(rig.ui);
    REQUIRE_FALSE(tieEcho.params.isVoid());
    CHECK(tie.getProperty("value", {}) == tieEcho.params.getProperty("values", {}).getProperty("ceiling", {}));

    // 自动化没有回执，帧自己取下一个 seq
    auto* feedback = rig.input.parameterLayer()->tree().getParameter("feedback");
    feedback->setValueNotifyingHost(feedback->convertTo0to1(12.0f));
    const auto automation = bridge.poll(rig.ui);
    REQUIRE_FALSE(automation.params.isVoid());
    CHECK(static_cast<int>(automation.params.getProperty("seq", {})) > static_cast<int>(again.getProperty("seq", {})));
    CHECK(static_cast<double>(automation.params.getProperty("values", {}).getProperty("feedback", {})) == 12.0);
}

TEST_CASE("input bridge: rinne.params carries all eleven ids in bridge form", "[host][input_bridge][params]")
{
    formalOnly();
    Rig rig;
    const auto frames = rig.bridge.firstFrames(rig.ui);
    const auto values = frames.params.getProperty("values", {});
    CHECK(keysOf(values) == std::set<std::string>{"feedback", "loop_on", "mix", "delay_mode", "delay_ms", "delay_note",
                                                  "low_cut", "high_cut", "polarity", "limiter", "ceiling"});
    CHECK(static_cast<double>(values.getProperty("feedback", {})) == 0.0);
    CHECK(values.getProperty("loop_on", {}).isBool());
    CHECK(static_cast<bool>(values.getProperty("loop_on", {})));
    CHECK(static_cast<double>(values.getProperty("mix", {})) == 25.0);
    CHECK(values.getProperty("delay_mode", {}).toString() == "time");
    CHECK(static_cast<double>(values.getProperty("delay_ms", {})) == 100.0);
    CHECK(values.getProperty("delay_note", {}).toString() == "n4");
    CHECK(static_cast<double>(values.getProperty("high_cut", {})) == 20000.0);
    CHECK_FALSE(static_cast<bool>(values.getProperty("polarity", {})));
    CHECK(static_cast<bool>(values.getProperty("limiter", {})));
    CHECK(static_cast<double>(values.getProperty("ceiling", {})) == -6.0);
}

TEST_CASE("input bridge: gestures reach the host as one begin and one end", "[host][input_bridge][params]")
{
    formalOnly();
    Rig rig;
    auto& bridge = rig.bridge;
    HostListener host;
    rig.input.addListener(&host);
    const auto mix = args({juce::var("mix")});

    SECTION("a drag: the begin settles, repeats do not nest, an end without a begin does nothing")
    {
        CHECK_FALSE(settled(rig.input));
        REQUIRE(isOk(bridge.beginParamGesture(mix)));
        CHECK(settled(rig.input));
        CHECK(host.begins == 1);
        REQUIRE(isOk(bridge.beginParamGesture(mix)));
        CHECK(host.begins == 1);
        REQUIRE(isOk(bridge.setParam(args({juce::var("mix"), juce::var(30)}))));
        REQUIRE(isOk(bridge.setParam(args({juce::var("mix"), juce::var(31)}))));
        CHECK(host.begins == 1);
        CHECK(host.ends == 0);
        REQUIRE(isOk(bridge.endParamGesture(mix)));
        CHECK(host.ends == 1);
        REQUIRE(isOk(bridge.endParamGesture(mix)));
        CHECK(host.ends == 1);
    }
    SECTION("a single write is its own gesture")
    {
        REQUIRE(isOk(bridge.setParam(args({juce::var("mix"), juce::var(30)}))));
        CHECK(host.begins == 1);
        CHECK(host.ends == 1);
    }
    SECTION("gestures still open when the page goes away are closed")
    {
        REQUIRE(isOk(bridge.beginParamGesture(mix)));
        REQUIRE(isOk(bridge.beginParamGesture(args({juce::var("feedback")}))));
        bridge.endAllGestures();
        CHECK(host.ends == 2);
        bridge.endAllGestures();
        CHECK(host.ends == 2);
        // 收掉之后不再算开着：再写一次是一次独立的手势
        REQUIRE(isOk(bridge.setParam(args({juce::var("mix"), juce::var(30)}))));
        CHECK(host.begins == 3);
        CHECK(host.ends == 3);
    }
    SECTION("the destructor closes what is still open")
    {
        {
            InputBridge second(rig.input, hooksFor(rig.bench, rig.rec));
            REQUIRE(isOk(second.beginParamGesture(mix)));
            CHECK(host.ends == 0);
        }
        CHECK(host.ends == 1);
    }
    rig.input.removeListener(&host);
}

TEST_CASE("input bridge: nothing is written to the host before the page asks", "[host][input_bridge][safety]")
{
    formalOnly();
    Rig rig(false);
    HostListener host;
    rig.input.addListener(&host);
    auto& bridge = rig.bridge;

    // 首帧、每拍的事件、重复请求：一样都不写
    stateFields(bridge);
    bridge.firstFrames(rig.ui);
    for (int i = 0; i < 5; ++i)
    {
        bridge.poll(rig.ui);
        bridge.tick(rig.ui);
    }
    bridge.firstFrames(rig.ui);
    CHECK(host.changes == 0);
    CHECK(host.begins == 0);
    CHECK(host.ends == 0);
    CHECK(host.dirtyMarks == 0);
    CHECK_FALSE(settled(rig.input));
    // 三个安全默认值原样
    CHECK(plain(rig.input, "feedback") == 0.0f);
    CHECK(plain(rig.input, "limiter") == 1.0f);
    CHECK(plain(rig.input, "ceiling") == -6.0f);
    CHECK(configsOf(rig.input).guardOn);
    rig.input.removeListener(&host);
}

// ---------------------------------------------------------------- Panic 与恢复

TEST_CASE("input bridge: panic cuts, repeated panics never recover, resume waits for the cooldown",
          "[host][input_bridge][safety]")
{
    formalOnly();
    Rig rig;
    auto& bridge = rig.bridge;
    rig.bench.run(100);
    CHECK(safetyOf(bridge).getProperty("cut", juce::var("x")).isVoid());
    CHECK(reasonOf(bridge.resumeLoop()) == "not-cut");

    const auto first = bridge.panic();
    REQUIRE(isOk(first));
    CHECK(keysOf(first) == std::set<std::string>{"ok", "cut"});
    CHECK(first.getProperty("cut", {}).toString() == "panic");
    rig.bench.run(100);
    const auto cut = safetyOf(bridge);
    CHECK(cut.getProperty("cut", {}).toString() == "panic");
    CHECK(static_cast<bool>(cut.getProperty("l2", {})));
    CHECK(static_cast<int>(cut.getProperty("cooldownMs", {})) > 0);

    // 连点：还是切断，回执也不变
    for (int i = 0; i < 5; ++i)
    {
        CHECK(bridge.panic().getProperty("cut", {}).toString() == "panic");
        rig.bench.run(50);
        CHECK(safetyOf(bridge).getProperty("cut", {}).toString() == "panic");
    }

    // 还在冷却：恢复被拒，剩余时长是估算，没有被算成 0
    const auto early = bridge.resumeLoop();
    CHECK_FALSE(isOk(early));
    CHECK(reasonOf(early) == "cooldown");
    CHECK(static_cast<int>(early.getProperty("remainingMs", {})) > 0);
    CHECK(static_cast<int>(early.getProperty("remainingMs", {})) <= 1500);
    rig.bench.run(100);
    CHECK(safetyOf(bridge).getProperty("cut", {}).toString() == "panic");

    // 冷却过了才恢复，而且不会自己恢复
    rig.bench.run(2000);
    CHECK(safetyOf(bridge).getProperty("cut", {}).toString() == "panic");
    CHECK(static_cast<int>(safetyOf(bridge).getProperty("cooldownMs", {})) == 0);
    REQUIRE(isOk(bridge.resumeLoop()));
    rig.bench.run(50);
    const auto back = safetyOf(bridge);
    CHECK(back.getProperty("cut", juce::var("x")).isVoid());
    CHECK_FALSE(static_cast<bool>(back.getProperty("l2", {})));
    // 恢复的结果不靠记性：没切断了就是 not-cut
    CHECK(reasonOf(bridge.resumeLoop()) == "not-cut");
}

TEST_CASE("input bridge: panic and resume do not settle the instance", "[host][input_bridge][safety]")
{
    formalOnly();
    Rig rig;
    DirtyListener dirty;
    rig.input.addListener(&dirty);
    rig.bench.run(100);
    REQUIRE(isOk(rig.bridge.panic()));
    rig.bench.run(1700);
    REQUIRE(isOk(rig.bridge.resumeLoop()));
    CHECK_FALSE(settled(rig.input));
    CHECK(dirty.dirtyMarks == 0);
    rig.input.removeListener(&dirty);
}

TEST_CASE("input bridge: the cut stays what it was and the cooldown is an estimate on the bit",
          "[host][input_bridge][safety]")
{
    formalOnly();
    Rig rig(false);
    auto& bridge = rig.bridge;
    const auto cutAfter = [&](engine::TripReason reason) {
        setSafety(rig.input, engine::SafetyState::Tripped, reason);
        return safetyOf(bridge).getProperty("cut", {}).toString();
    };
    CHECK(cutAfter(engine::TripReason::Nan) == "nan");
    CHECK(cutAfter(engine::TripReason::Huge) == "huge");
    CHECK(cutAfter(engine::TripReason::Runaway) == "runaway");
    CHECK(cutAfter(engine::TripReason::Panic) == "panic");

    // 已经切断的不改原因
    setSafety(rig.input, engine::SafetyState::Tripped, engine::TripReason::Runaway);
    CHECK(bridge.panic().getProperty("cut", {}).toString() == "runaway");

    // 冷却位一来就从 1500 倒数；到点而位还在，写 1 不写 0
    setSafety(rig.input, engine::SafetyState::Tripped, engine::TripReason::Panic, true);
    CHECK(static_cast<int>(safetyOf(bridge).getProperty("cooldownMs", {})) == 1500);
    // state 里向上取整到 100 ms，免得每拍都因为这个数变了多发一帧；回执不取整
    rig.bench.idle(10);
    CHECK(static_cast<int>(safetyOf(bridge).getProperty("cooldownMs", {})) == 1500);
    CHECK(static_cast<int>(bridge.resumeLoop().getProperty("remainingMs", {})) == 1490);
    rig.bench.idle(490);
    CHECK(static_cast<int>(bridge.resumeLoop().getProperty("remainingMs", {})) == 1000);
    CHECK(static_cast<int>(safetyOf(bridge).getProperty("cooldownMs", {})) == 1000);
    rig.bench.idle(1500);
    CHECK(reasonOf(bridge.resumeLoop()) == "cooldown");
    CHECK(static_cast<int>(bridge.resumeLoop().getProperty("remainingMs", {})) == 1);
    // 位走了就是 0，估算不留
    setSafety(rig.input, engine::SafetyState::Tripped, engine::TripReason::Panic, false);
    CHECK(static_cast<int>(safetyOf(bridge).getProperty("cooldownMs", {})) == 0);
    // 淡入中单独报，不算切断
    setSafety(rig.input, engine::SafetyState::FadeIn);
    const auto fading = safetyOf(bridge);
    CHECK(static_cast<bool>(fading.getProperty("fadeIn", {})));
    CHECK(fading.getProperty("cut", juce::var("x")).isVoid());
}

TEST_CASE("input bridge: l2 is any of the four conditions", "[host][input_bridge][safety]")
{
    CHECK_FALSE(isL2(nullptr, "connected", true, false));
    CHECK(isL2("panic", "connected", true, false));
    CHECK(isL2(nullptr, "conflict", true, false));
    CHECK(isL2(nullptr, "abiMismatch", true, false));
    CHECK(isL2(nullptr, "connected", false, true));
    // 限制器开着时失控灯亮是它在干活；限制器关着而灯不亮也没事
    CHECK_FALSE(isL2(nullptr, "connected", true, true));
    CHECK_FALSE(isL2(nullptr, "connected", false, false));
}

// ---------------------------------------------------------------- dismissHint 与导出提示

TEST_CASE("input bridge: dismissHint keeps ids per instance and outlives the editor", "[host][input_bridge][hints]")
{
    formalOnly();
    Rig rig;
    for (const auto& bad : {args({}), args({juce::var(1)}), args({juce::var("")})})
        CHECK(reasonOf(rig.bridge.dismissHint(bad)) == "bad-id");
    DirtyListener dirty;
    rig.input.addListener(&dirty);
    REQUIRE(isOk(rig.bridge.dismissHint(args({juce::var("order")}))));
    REQUIRE(isOk(rig.bridge.dismissHint(args({juce::var("order")}))));
    const auto fields = stateFields(rig.bridge);
    const auto* list = fields.getProperty("dismissedHints", {}).getArray();
    REQUIRE(list != nullptr);
    REQUIRE(list->size() == 1);
    CHECK(list->getFirst().toString() == "order");
    CHECK(dirty.dirtyMarks == 0);
    CHECK_FALSE(settled(rig.input));

    InputBridge reopened(rig.input, hooksFor(rig.bench, rig.rec));
    CHECK(stateFields(reopened).getProperty("dismissedHints", {}).getArray()->size() == 1);
    rig.input.removeListener(&dirty);
}

TEST_CASE("input bridge: export causes accumulate until confirmed", "[host][input_bridge][hints]")
{
    using rinne::input::trackExportCauses;
    InputProcessor::UiSession session;
    rinne::loop::LoopStatusFields loop;

    // 计数没动就不并：引擎那个位是「最近一次」的，不是这次的
    loop.exportEvents = 0;
    loop.exportCauses = 8;
    trackExportCauses(session, loop);
    CHECK(session.exportBits == 0u);
    CHECK(session.exportLatch.pending() == 0u);

    loop.exportEvents = 2;
    loop.exportCauses = 1;
    trackExportCauses(session, loop);
    loop.exportEvents = 3;
    loop.exportCauses = 4;
    trackExportCauses(session, loop);
    CHECK(session.exportBits == 5u);
    CHECK(session.exportLatch.pending() == 3u);

    // 回绕只比不等
    loop.exportEvents = 3;
    loop.exportCauses = 2;
    trackExportCauses(session, loop);
    CHECK(session.exportBits == 5u);
}

TEST_CASE("input bridge: confirming the export hint clears the fields and new events latch again",
          "[host][input_bridge][hints]")
{
    formalOnly();
    needsSharedMemory();
    Rig rig;
    rig.bench.run(100);
    // 离线渲染里的超长块（超过支持上限）：环路这一块不走，导出不一致计一次，原因是宿主违约
    std::array<std::vector<float>, 2> data;
    for (auto& ch : data)
        ch.assign(40000u, 0.0f);
    const auto oversizedBlock = [&] {
        float* channels[2] = {data[0].data(), data[1].data()};
        juce::AudioBuffer<float> buffer(channels, 2, 40000);
        juce::MidiBuffer midi;
        rig.input.processBlock(buffer, midi);
    };
    const auto causes = [&] { return stateFields(rig.bridge).getProperty("exportCauses", {}); };
    CHECK(static_cast<int>(causes().getProperty("count", {})) == 0);

    rig.input.setNonRealtime(true);
    oversizedBlock();
    CHECK(static_cast<int>(causes().getProperty("bits", {})) == 1);
    CHECK(static_cast<int>(causes().getProperty("count", {})) == 1);
    oversizedBlock();
    CHECK(static_cast<int>(causes().getProperty("count", {})) == 2);

    REQUIRE(isOk(rig.bridge.dismissHint(args({juce::var("exportMismatch")}))));
    CHECK(static_cast<int>(causes().getProperty("bits", {})) == 0);
    CHECK(static_cast<int>(causes().getProperty("count", {})) == 0);
    // 确认不是「关掉一条提示」：不进 dismissedHints，否则页面重开时会把窗口关着期间的新事件当基线藏掉
    CHECK(stateFields(rig.bridge).getProperty("dismissedHints", {}).getArray()->isEmpty());

    oversizedBlock();
    CHECK(static_cast<int>(causes().getProperty("bits", {})) == 1);
    CHECK(static_cast<int>(causes().getProperty("count", {})) == 1);

    // 确认的是「到现在为止」：上次读 state 之后才来的事件也算进去，不留到下一帧再弹
    REQUIRE(isOk(rig.bridge.dismissHint(args({juce::var("exportMismatch")}))));
    oversizedBlock();
    REQUIRE(isOk(rig.bridge.dismissHint(args({juce::var("exportMismatch")}))));
    CHECK(static_cast<int>(causes().getProperty("bits", {})) == 0);
    CHECK(static_cast<int>(causes().getProperty("count", {})) == 0);
}

// ---------------------------------------------------------------- 工程设置与单位

TEST_CASE("input bridge: the project settings validate, settle, mark dirty and survive a reload",
          "[host][input_bridge][cfg]")
{
    formalOnly();
    Rig rig;
    auto& bridge = rig.bridge;
    DirtyListener dirty;
    rig.input.addListener(&dirty);

    for (const auto& bad : {args({}), args({juce::var(1)}), args({juce::var("true")})})
    {
        CHECK(reasonOf(bridge.setGuardOn(bad)) == "bad-value");
        CHECK(reasonOf(bridge.setResetOnPlay(bad)) == "bad-value");
    }
    for (const auto& bad : {args({}), args({juce::var("Auto")}), args({juce::var("30")}), args({juce::var(1)})})
        CHECK(reasonOf(bridge.setTailMode(bad)) == "bad-value");
    for (const auto& bad : {args({}), args({juce::var("percent")}), args({juce::var("DB")}), args({juce::var(1)})})
        CHECK(reasonOf(bridge.setFbUnit(bad)) == "bad-value");
    for (const auto& bad : {args({}), args({juce::var("s")}), args({juce::var("MS")}), args({juce::var(1)})})
        CHECK(reasonOf(bridge.setDelayUnit(bad)) == "bad-value");
    CHECK(dirty.dirtyMarks == 0);
    CHECK_FALSE(settled(rig.input));

    // 默认：% 与 ms，保护开，起播清空开
    auto cfg = stateFields(bridge).getProperty("cfg", {});
    CHECK(keysOf(cfg) == std::set<std::string>{"resetOnPlay", "tailMode", "delayUnit", "fbUnit", "guardOn"});
    CHECK(cfg.getProperty("fbUnit", {}).toString() == "pct");
    CHECK(cfg.getProperty("delayUnit", {}).toString() == "ms");
    CHECK(static_cast<bool>(cfg.getProperty("guardOn", {})));
    CHECK(static_cast<bool>(cfg.getProperty("resetOnPlay", {})));
    CHECK(cfg.getProperty("tailMode", {}).toString() == "auto");

    const auto fb = bridge.setFbUnit(args({juce::var("db")}));
    CHECK(isOk(fb));
    CHECK(keysOf(fb) == std::set<std::string>{"ok", "value"});
    CHECK(fb.getProperty("value", {}).toString() == "db");
    CHECK(settled(rig.input));
    CHECK(dirty.dirtyMarks == 1);
    CHECK(configsOf(rig.input).fbUnit == state::FbUnit::Decibel);

    CHECK(bridge.setDelayUnit(args({juce::var("samples")})).getProperty("value", {}).toString() == "samples");
    CHECK(dirty.dirtyMarks == 2);
    CHECK(configsOf(rig.input).delayUnit == state::DelayUnit::Samples);

    CHECK_FALSE(static_cast<bool>(bridge.setGuardOn(args({juce::var(false)})).getProperty("value", {})));
    CHECK_FALSE(static_cast<bool>(bridge.setResetOnPlay(args({juce::var(false)})).getProperty("value", {})));
    CHECK(bridge.setTailMode(args({juce::var("30s")})).getProperty("value", {}).toString() == "30s");
    CHECK(dirty.dirtyMarks == 5);
    // 外壳跟上：宿主问尾长读的是外壳那一份
    CHECK(rig.input.getTailLengthSeconds() == 30.0);

    // 写进工程：换一个实例读回来还是这些
    const auto saved = savedState(rig.input);
    InputProcessor& second = addInputIn(rig.bench, rig.presets.dir());
    restore(second, saved);
    InputBridge reopened(second, hooksFor(rig.bench, rig.rec));
    cfg = stateFields(reopened).getProperty("cfg", {});
    CHECK(cfg.getProperty("fbUnit", {}).toString() == "db");
    CHECK(cfg.getProperty("delayUnit", {}).toString() == "samples");
    CHECK_FALSE(static_cast<bool>(cfg.getProperty("guardOn", {})));
    CHECK_FALSE(static_cast<bool>(cfg.getProperty("resetOnPlay", {})));
    CHECK(cfg.getProperty("tailMode", {}).toString() == "30s");

    // 单位改回去也是一次用户动作
    CHECK(bridge.setFbUnit(args({juce::var("pct")})).getProperty("value", {}).toString() == "pct");
    CHECK(bridge.setDelayUnit(args({juce::var("ms")})).getProperty("value", {}).toString() == "ms");
    CHECK(configsOf(rig.input).fbUnit == state::FbUnit::Percent);
    CHECK(configsOf(rig.input).delayUnit == state::DelayUnit::Milliseconds);
    rig.input.removeListener(&dirty);
}

// ---------------------------------------------------------------- 预设

TEST_CASE("input bridge: loading a preset goes through the fade and tracks the current preset",
          "[host][input_bridge][presets]")
{
    formalOnly();
    Rig rig;
    auto& bridge = rig.bridge;
    DirtyListener dirty;
    rig.input.addListener(&dirty);
    const auto& factory = rig.input.presetStore()->factoryPresets();
    REQUIRE(factory.size() == 8);
    const auto seq = rig.input.presetStore()->presetSeq();

    // 新实例没有当前预设
    auto preset = stateFields(bridge).getProperty("preset", {});
    CHECK(keysOf(preset) == std::set<std::string>{"id", "dirty", "name"});
    CHECK(preset.getProperty("id", juce::var("x")).isVoid());
    CHECK(preset.getProperty("name", juce::var("x")).isVoid());
    CHECK_FALSE(static_cast<bool>(preset.getProperty("dirty", {})));

    SECTION("failures change nothing")
    {
        CHECK(reasonOf(bridge.loadPreset(args({juce::var("u-000000000000")}))) == "not-found");
        CHECK(reasonOf(bridge.loadPreset(args({juce::var("f-nope")}))) == "not-found");
        CHECK(reasonOf(bridge.loadPreset(args({juce::var(1)}))) == "not-found");
        CHECK(reasonOf(bridge.loadPreset(args({}))) == "not-found");
        rig.presets.dir().getChildFile("u-0123456789ab.rinnepreset").replaceWithText("not json");
        CHECK(reasonOf(bridge.loadPreset(args({juce::var("u-0123456789ab")}))) == "bad-file");
        CHECK(rig.input.presetStore()->presetSeq() == seq);
        CHECK(dirty.dirtyMarks == 0);
        CHECK_FALSE(settled(rig.input));
    }
    SECTION("a factory preset: two-stage fade, three-language name, clean")
    {
        const auto& target = factory[1];
        REQUIRE(isOk(bridge.loadPreset(args({juce::var(target.id)}))));
        CHECK(rig.input.presetStore()->presetSeq() == seq + 1u);
        CHECK(settled(rig.input));
        CHECK(dirty.dirtyMarks == 1);
        CHECK(configsOf(rig.input).presetName == target.en.toStdString());
        CHECK_FALSE(configsOf(rig.input).presetDirty);
        preset = stateFields(bridge).getProperty("preset", {});
        CHECK(preset.getProperty("id", {}).toString() == target.id);
        CHECK(keysOf(preset.getProperty("name", {})) == std::set<std::string>{"zh", "en", "fr"});
        CHECK(preset.getProperty("name", {}).getProperty("fr", {}).toString() == target.fr);

        rig.bench.run(100);
        CHECK(rig.input.instanceHost().counts().fadeIns[static_cast<std::size_t>(engine::FadeReason::Preset)] == 1u);

        // 安全默认值不随预设动
        CHECK(plain(rig.input, "limiter") == 1.0f);
        CHECK(plain(rig.input, "ceiling") == -6.0f);
    }
    SECTION("only a real change of a sound parameter from the page makes it dirty")
    {
        REQUIRE(isOk(bridge.loadPreset(args({juce::var(factory[0].id)}))));
        const auto dirtyNow = [&] {
            return static_cast<bool>(stateFields(bridge).getProperty("preset", {}).getProperty("dirty", {}));
        };
        CHECK_FALSE(dirtyNow());
        // 工程设置不属于预设
        REQUIRE(isOk(bridge.setLimiter(args({juce::var(false)}))));
        REQUIRE(isOk(bridge.setParam(args({juce::var("ceiling"), juce::var(-3)}))));
        REQUIRE(isOk(bridge.setParam(args({juce::var("loop_on"), juce::var(false)}))));
        CHECK_FALSE(dirtyNow());
        // 写同一个值
        const auto mix = static_cast<double>(plain(rig.input, "mix"));
        REQUIRE(isOk(bridge.setParam(args({juce::var("mix"), juce::var(mix)}))));
        CHECK_FALSE(dirtyNow());
        // 宿主自动化
        auto* parameter = rig.input.parameterLayer()->tree().getParameter("high_cut");
        parameter->setValueNotifyingHost(parameter->convertTo0to1(5000.0f));
        CHECK_FALSE(dirtyNow());
        // 真的动了
        REQUIRE(isOk(bridge.setParam(args({juce::var("mix"), juce::var(mix + 1.0)}))));
        CHECK(dirtyNow());
        CHECK(configsOf(rig.input).presetDirty);
        // 再载入一次就干净了
        REQUIRE(isOk(bridge.loadPreset(args({juce::var(factory[0].id)}))));
        CHECK_FALSE(dirtyNow());
    }
    SECTION("with no current preset nothing is dirty")
    {
        REQUIRE(isOk(bridge.setParam(args({juce::var("mix"), juce::var(70)}))));
        CHECK_FALSE(configsOf(rig.input).presetDirty);
        CHECK_FALSE(static_cast<bool>(stateFields(bridge).getProperty("preset", {}).getProperty("dirty", {})));
    }
    rig.input.removeListener(&dirty);
}

TEST_CASE("input bridge: a dirty bit with no current preset to hang on does not count", "[host][input_bridge][presets]")
{
    formalOnly();
    Rig rig;
    rig.input.stateGlue()->editConfigs([](state::Configs& next) { next.presetDirty = true; });
    CHECK_FALSE(static_cast<bool>(stateFields(rig.bridge).getProperty("preset", {}).getProperty("dirty", {})));
}

TEST_CASE("input bridge: only a save that went through settles the instance", "[host][input_bridge][presets]")
{
    formalOnly();
    Rig rig;
    CHECK(reasonOf(rig.bridge.savePreset(args({juce::var("")}))) == "bad-name");
    CHECK_FALSE(settled(rig.input));
    REQUIRE(isOk(rig.bridge.savePreset(args({juce::var("Mine")}))));
    CHECK(settled(rig.input));
}

TEST_CASE("input bridge: saving filters the name and answers the documented reasons", "[host][input_bridge][presets]")
{
    formalOnly();
    Rig rig;
    auto& bridge = rig.bridge;
    bridge.firstFrames(rig.ui);
    REQUIRE(isOk(bridge.setParam(args({juce::var("mix"), juce::var(66)}))));
    bridge.poll(rig.ui);

    for (const auto& bad :
         {args({}), args({juce::var(3)}), args({juce::var("  \t\n ")}), args({juce::var("\x01\x7f")})})
        CHECK(reasonOf(bridge.savePreset(bad)) == "bad-name");

    const auto saved = bridge.savePreset(args({juce::var("  My\tPatch\n ")}));
    REQUIRE(isOk(saved));
    CHECK(keysOf(saved) == std::set<std::string>{"ok", "id"});
    const juce::String id = saved.getProperty("id", {}).toString();
    CHECK(id.startsWith("u-"));
    CHECK(settled(rig.input));
    CHECK(configsOf(rig.input).presetName == "MyPatch");

    const auto preset = stateFields(bridge).getProperty("preset", {});
    CHECK(preset.getProperty("id", {}).toString() == id);
    CHECK(preset.getProperty("name", {}).toString() == "MyPatch");
    CHECK_FALSE(static_cast<bool>(preset.getProperty("dirty", {})));

    // 保存成功之后当拍重发列表
    const auto frames = bridge.poll(rig.ui);
    REQUIRE_FALSE(frames.presets.isVoid());
    CHECK(frames.presets.getProperty("current", {}).toString() == id);
    const auto* user = frames.presets.getProperty("user", {}).getArray();
    REQUIRE(user != nullptr);
    REQUIRE(user->size() == 1);
    CHECK(keysOf(user->getFirst()) == std::set<std::string>{"id", "name"});
    CHECK(user->getFirst().getProperty("name", {}).toString() == "MyPatch");

    // 重名：不允许覆盖就 exists（不分大小写）；overwrite 只认真
    CHECK(reasonOf(bridge.savePreset(args({juce::var("mypatch")}))) == "exists");
    CHECK(reasonOf(bridge.savePreset(args({juce::var("MyPatch"), juce::var(false)}))) == "exists");
    CHECK(reasonOf(bridge.savePreset(args({juce::var("MyPatch"), juce::var(1)}))) == "exists");
    CHECK(reasonOf(bridge.savePreset(args({juce::var("MyPatch"), juce::var("true")}))) == "exists");
    const auto over = bridge.savePreset(args({juce::var("MYPATCH"), juce::var(true)}));
    REQUIRE(isOk(over));
    CHECK(over.getProperty("id", {}).toString() == id);

    // 截到 24 个码点（汉字也算 1 个）
    const juce::String longName = juce::String::repeatedString(juce::String::fromUTF8("\xe5\x9b\x9e"), 30);
    REQUIRE(isOk(bridge.savePreset(args({juce::var(longName)}))));
    const std::string stored = configsOf(rig.input).presetName;
    CHECK(juce::String::fromUTF8(stored.data(), static_cast<int>(stored.size())).length() == 24);

    // 用户目录写不进去：用一个普通文件占住上级
    const juce::File blocker = rig.presets.dir().getChildFile("blocker");
    REQUIRE(blocker.replaceWithText("x"));
    InputProcessor& blocked = addInputIn(rig.bench, blocker.getChildFile("Presets"));
    InputBridge blockedBridge(blocked, hooksFor(rig.bench, rig.rec));
    CHECK(reasonOf(blockedBridge.savePreset(args({juce::var("Nope")}))) == "write-failed");
    CHECK(configsOf(blocked).presetName.empty());
}

TEST_CASE("input bridge: deleting answers the documented reasons and clears a deleted current preset",
          "[host][input_bridge][presets]")
{
    formalOnly();
    Rig rig;
    auto& bridge = rig.bridge;
    bridge.firstFrames(rig.ui);
    DirtyListener dirty;

    CHECK(reasonOf(bridge.deletePreset(args({}))) == "not-found");
    CHECK(reasonOf(bridge.deletePreset(args({juce::var(1)}))) == "not-found");
    CHECK(reasonOf(bridge.deletePreset(args({juce::var("u-000000000000")}))) == "not-found");
    const juce::String factoryId = rig.input.presetStore()->factoryPresets().front().id;
    CHECK(reasonOf(bridge.deletePreset(args({juce::var(factoryId)}))) == "factory");
    CHECK(rig.input.presetStore()->factoryPresets().size() == 8);

    const juce::String other = bridge.savePreset(args({juce::var("Other")})).getProperty("id", {}).toString();
    const juce::String current = bridge.savePreset(args({juce::var("Current")})).getProperty("id", {}).toString();
    bridge.poll(rig.ui);

    // 删的不是当前的：当前不动，列表重发
    REQUIRE(isOk(bridge.deletePreset(args({juce::var(other)}))));
    auto frames = bridge.poll(rig.ui);
    REQUIRE_FALSE(frames.presets.isVoid());
    CHECK(frames.presets.getProperty("user", {}).getArray()->size() == 1);
    CHECK(stateFields(bridge).getProperty("preset", {}).getProperty("id", {}).toString() == current);

    // 删的是当前的：参数不动，没有当前预设了，工程里的名字变了要标脏，但删文件本身不算定型的动作
    const float mix = plain(rig.input, "mix");
    rig.input.addListener(&dirty);
    REQUIRE(isOk(bridge.deletePreset(args({juce::var(current)}))));
    CHECK(dirty.dirtyMarks == 1);
    CHECK(plain(rig.input, "mix") == mix);
    const auto preset = stateFields(bridge).getProperty("preset", {});
    CHECK(preset.getProperty("id", juce::var("x")).isVoid());
    CHECK(preset.getProperty("name", juce::var("x")).isVoid());
    CHECK_FALSE(static_cast<bool>(preset.getProperty("dirty", {})));
    CHECK(configsOf(rig.input).presetName.empty());
    frames = bridge.poll(rig.ui);
    REQUIRE_FALSE(frames.presets.isVoid());
    CHECK(frames.presets.getProperty("current", juce::var("x")).isVoid());
    CHECK(frames.presets.getProperty("user", {}).getArray()->isEmpty());
    rig.input.removeListener(&dirty);
}

TEST_CASE("input bridge: rinne.presets lists factory and user presets", "[host][input_bridge][presets]")
{
    formalOnly();
    Rig rig;
    const auto frames = rig.bridge.firstFrames(rig.ui);
    REQUIRE_FALSE(frames.presets.isVoid());
    CHECK(keysOf(frames.presets) == std::set<std::string>{"factory", "user", "current"});
    const auto* factory = frames.presets.getProperty("factory", {}).getArray();
    REQUIRE(factory != nullptr);
    REQUIRE(factory->size() == 8);
    for (const auto& item : *factory)
    {
        CHECK(keysOf(item) == std::set<std::string>{"id", "name"});
        CHECK(keysOf(item.getProperty("name", {})) == std::set<std::string>{"zh", "en", "fr"});
    }
    CHECK(frames.presets.getProperty("user", {}).getArray()->isEmpty());
    CHECK(frames.presets.getProperty("current", juce::var("x")).isVoid());
}

TEST_CASE("input bridge: the project keeps only a name, the id is found again", "[host][input_bridge][presets]")
{
    formalOnly();
    Rig rig;
    const juce::String userId = rig.bridge.savePreset(args({juce::var("Studio A")})).getProperty("id", {}).toString();
    const auto& factory = rig.input.presetStore()->factoryPresets().front();
    const auto reloaded = [&](const juce::String& name) {
        InputProcessor& input = addInputIn(rig.bench, rig.presets.dir());
        restore(input, wiring_test::stateBytes(state::Role::Input, [&name](state::StateDocument& d) {
                    d.configs.presetName = name.toStdString();
                }));
        InputBridge bridge(input, hooksFor(rig.bench, rig.rec));
        return stateFields(bridge).getProperty("preset", {});
    };

    CHECK(reloaded(factory.en).getProperty("id", {}).toString() == factory.id);
    CHECK(reloaded(factory.zh).getProperty("id", {}).toString() == factory.id);
    CHECK(keysOf(reloaded(factory.en).getProperty("name", {})) == std::set<std::string>{"zh", "en", "fr"});
    CHECK(reloaded("studio a").getProperty("id", {}).toString() == userId);

    // 换了一台机器，预设不在：名字照写，只是没有 ID
    const auto missing = reloaded("Somewhere Else");
    CHECK(missing.getProperty("id", juce::var("x")).isVoid());
    CHECK(missing.getProperty("name", {}).toString() == "Somewhere Else");
}

// ---------------------------------------------------------------- rinne.state

TEST_CASE("input bridge: rinne.state carries the Input fields and leaves out what it does not know",
          "[host][input_bridge][state]")
{
    formalOnly();
    const std::set<std::string> base{"restored", "takeover",       "group",        "groupOrigin", "cfg",    "preset",
                                     "delay",    "dismissedHints", "exportCauses", "silentCause", "safety", "build"};

    SECTION("before prepare: no sample rate anywhere, nothing optional invented")
    {
        Rig rig(false);
        const auto fields = stateFields(rig.bridge);
        CHECK(keysOf(fields) == base);
        CHECK_FALSE(static_cast<bool>(fields.getProperty("restored", {})));
        CHECK_FALSE(static_cast<bool>(fields.getProperty("takeover", {})));
        CHECK(fields.getProperty("group", juce::var("x")).isVoid());
        CHECK(fields.getProperty("groupOrigin", {}).toString() == "auto");
        CHECK(fields.getProperty("silentCause", juce::var("x")).isVoid());

        const auto delay = fields.getProperty("delay", {});
        CHECK(keysOf(delay) == std::set<std::string>{"reason", "maxBlock", "noTempo"});
        CHECK(delay.getProperty("reason", {}).toString() == "ok");

        const auto safety = fields.getProperty("safety", {});
        CHECK(keysOf(safety) == std::set<std::string>{"cut", "cooldownMs", "l2", "fadeIn"});
        CHECK(safety.getProperty("cut", juce::var("x")).isVoid());
        CHECK(static_cast<int>(safety.getProperty("cooldownMs", {})) == 0);
        CHECK_FALSE(static_cast<bool>(safety.getProperty("l2", {})));

        const auto causes = fields.getProperty("exportCauses", {});
        CHECK(keysOf(causes) == std::set<std::string>{"bits", "count"});
        CHECK(static_cast<int>(causes.getProperty("count", {})) == 0);
        // 宿主测试里没有包装层，也没有构建标签：build 只剩编译时就定了的平台
        CHECK(keysOf(fields.getProperty("build", {})) == std::set<std::string>{"arch"});
    }
    SECTION("after prepare and a manual pick")
    {
        Rig rig;
        REQUIRE(rig.input.selectGroup(4));
        rig.bench.run(200);
        auto expected = base;
        expected.insert("sampleRate");
        const auto fields = stateFields(rig.bridge);
        CHECK(keysOf(fields) == expected);
        CHECK(fields.getProperty("group", {}).toString() == "D");
        CHECK(fields.getProperty("groupOrigin", {}).toString() == "manual");
        CHECK(static_cast<int>(fields.getProperty("sampleRate", {})) == 48000);

        const auto delay = fields.getProperty("delay", {});
        CHECK(keysOf(delay) == std::set<std::string>{"actualMs", "minMs", "reason", "maxBlock", "bpm", "noTempo"});
        CHECK(static_cast<double>(delay.getProperty("actualMs", {})) > 0.0);
        CHECK(static_cast<double>(delay.getProperty("minMs", {})) > 0.0);
        CHECK(static_cast<int>(delay.getProperty("maxBlock", {})) == kBlock);
    }
    SECTION("a restored project shows as restored")
    {
        Rig rig;
        restore(rig.input, groupState(state::Role::Input, 3));
        const auto fields = stateFields(rig.bridge);
        CHECK(static_cast<bool>(fields.getProperty("restored", {})));
        CHECK(fields.getProperty("group", {}).toString() == "C");
        CHECK(fields.getProperty("groupOrigin", {}).toString() == "project");
    }
}

TEST_CASE("input bridge: delay says whether the tempo came from the host", "[host][input_bridge][state]")
{
    formalOnly();
    const auto syncMode = [](Rig& rig) {
        auto* mode = rig.input.parameterLayer()->tree().getParameter("delay_mode");
        REQUIRE(mode != nullptr);
        mode->setValueNotifyingHost(mode->convertTo0to1(1.0f));
    };
    {
        Rig rig;
        syncMode(rig);
        rig.bench.run(100);
        const auto delay = stateFields(rig.bridge).getProperty("delay", {});
        CHECK(static_cast<double>(delay.getProperty("bpm", {})) == 120.0);
        CHECK_FALSE(static_cast<bool>(delay.getProperty("noTempo", {})));
    }
    {
        // 同步档上宿主从没给过速度：速度是兜底的那个，要说明白。提示位的口径在 core：本会话拿到过速度就不再提示
        Rig rig;
        rig.input.setPlayHead(nullptr);
        syncMode(rig);
        rig.bench.run(100);
        const auto delay = stateFields(rig.bridge).getProperty("delay", {});
        CHECK(static_cast<bool>(delay.getProperty("noTempo", {})));
        CHECK(static_cast<double>(delay.getProperty("bpm", {})) == 120.0);
    }
}

TEST_CASE("input bridge: delay.reason tells raised from capped and overrun", "[host][input_bridge][state]")
{
    namespace loop = rinne::loop;
    loop::LoopStatusFields fields;
    fields.delayUserMs = 100.0f;
    fields.delayMin = 256;
    fields.delayMinReason = static_cast<std::uint8_t>(loop::DelayMinReason::kPrepare);
    // 下限的来源一直有值，用户要的 D 不低于下限就是 ok
    CHECK(std::string(delayReasonOf(fields, 48000u)) == "ok");
    // 采样率不知道时判不了
    fields.delayUserMs = 1.0f;
    CHECK(std::string(delayReasonOf(fields, 0u)) == "ok");
    CHECK(std::string(delayReasonOf(fields, 48000u)) == "raised");
    fields.delayMinReason = static_cast<std::uint8_t>(loop::DelayMinReason::kHostViolation);
    CHECK(std::string(delayReasonOf(fields, 48000u)) == "overrun");
    fields.delayHints = loop::kDelayHintClipped;
    CHECK(std::string(delayReasonOf(fields, 48000u)) == "capped");
}

TEST_CASE("input bridge: the build line takes the commit from the label and leaves out what it cannot tell",
          "[host][input_bridge][state]")
{
    using Wrapper = juce::AudioProcessor::WrapperType;
    const auto sha = [](const juce::var& build) { return build.getProperty("sha", juce::var()); };

    const auto vst3 = buildInfo("0.10.12+5f3a9c1", Wrapper::wrapperType_VST3);
    CHECK(keysOf(vst3) == std::set<std::string>{"sha", "fmt", "arch"});
    CHECK(sha(vst3).toString() == "5f3a9c1");
    CHECK(vst3.getProperty("fmt", {}).toString() == "VST3");
    CHECK(buildInfo("x", Wrapper::wrapperType_AudioUnit).getProperty("fmt", {}).toString() == "AU");
    const juce::String full = "0123456789abcdef0123456789abcdef01234567";
    CHECK(sha(buildInfo("1.0.0-rc.1+ci+" + full, Wrapper::wrapperType_VST3)).toString() == full);
    for (const char* label : {"", "0.10.12", "0.10.12+", "0.10.12+5f3a9c", "0.10.12+5F3A9C1", "0.10.12+5f3a9cg",
                              "0.10.12+dirty", "0.10.12+0123456789abcdef0123456789abcdef012345678"})
    {
        INFO(label);
        CHECK(sha(buildInfo(label, Wrapper::wrapperType_VST3)).isVoid());
    }
    CHECK(keysOf(buildInfo("0.10.12+5f3a9c1", Wrapper::wrapperType_Standalone)) ==
          std::set<std::string>{"sha", "arch"});
}

// ---------------------------------------------------------------- silentCause

TEST_CASE("input bridge: silentCause order is host bypass, host paused, peer bypass, stalled",
          "[host][input_bridge][silent]")
{
    CHECK(silentCauseOf(SilentFacts{}) == nullptr);
    CHECK(std::string(silentCauseOf(SilentFacts{true, true, true, true})) == "hostBypass");
    CHECK(std::string(silentCauseOf(SilentFacts{false, true, true, true})) == "hostPaused");
    CHECK(std::string(silentCauseOf(SilentFacts{false, true, false, true})) == "peerBypass");
    CHECK(std::string(silentCauseOf(SilentFacts{false, false, false, true})) == "stalled");
}

TEST_CASE("input bridge: silentCause reads its four sources by value, whatever the pair state",
          "[host][input_bridge][silent]")
{
    formalOnly();
    const auto causeOf = [](InputBridge& bridge) {
        return stateFields(bridge).getProperty("silentCause", juce::var());
    };

    SECTION("host bypass follows the block type")
    {
        Rig rig;
        rig.bench.run(50);
        CHECK(causeOf(rig.bridge).isVoid());
        rig.bench.setBypassed(rig.input, true);
        rig.bench.run(20);
        CHECK(causeOf(rig.bridge).toString() == "hostBypass");
        rig.bench.setBypassed(rig.input, false);
        rig.bench.run(20);
        CHECK(causeOf(rig.bridge).isVoid());
    }
    SECTION("host paused: 250 ms without samples, on the injected clock")
    {
        Rig rig;
        rig.bench.run(50);
        CHECK(causeOf(rig.bridge).isVoid());
        rig.bench.setRunning(rig.input, false);
        rig.bench.idle(240);
        CHECK(causeOf(rig.bridge).isVoid());
        rig.bench.idle(10);
        CHECK(causeOf(rig.bridge).toString() == "hostPaused");
        rig.bench.setRunning(rig.input, true);
        rig.bench.run(10);
        CHECK(causeOf(rig.bridge).isVoid());
    }
    SECTION("an instance that never processed audio is not paused")
    {
        Rig rig(false);
        rig.bench.idle(500);
        CHECK(causeOf(rig.bridge).isVoid());
    }
    SECTION("stalled comes from the safety state")
    {
        Rig rig(false);
        setSafety(rig.input, engine::SafetyState::Stalled);
        CHECK(causeOf(rig.bridge).toString() == "stalled");
        setSafety(rig.input, engine::SafetyState::Run);
        CHECK(causeOf(rig.bridge).isVoid());
    }
}

TEST_CASE("input bridge: the bypass bit only counts on a slot that reads stable and is active",
          "[host][input_bridge][silent]")
{
    ipc::SlotSample slot;
    slot.stable = true;
    slot.ctl = ipc::kCtlPhaseActive;
    slot.rtFlags = ipc::kRtBypassed;
    CHECK(peerBypassOf(slot));

    auto notStable = slot;
    notStable.stable = false;
    CHECK_FALSE(peerBypassOf(notStable));

    for (const auto phase : {ipc::kCtlPhaseFree, ipc::kCtlPhaseClaiming, ipc::kCtlPhaseDormant})
    {
        auto other = slot;
        other.ctl = phase;
        CHECK_FALSE(peerBypassOf(other));
    }

    auto notBypassed = slot;
    notBypassed.rtFlags = ipc::kRtStalled;
    CHECK_FALSE(peerBypassOf(notBypassed));
}

TEST_CASE("input bridge: peerBypass reads the bypass bit of the Output slot", "[host][input_bridge][silent]")
{
    formalOnly();
    needsSharedMemory();
    TempPresetDir presets;
    Bench bench;
    InputProcessor& input = addInputIn(bench, presets.dir());
    OutputProcessor& output = bench.addOutput();
    REQUIRE(input.selectGroup(1));
    REQUIRE(output.selectGroup(1));
    bench.run(300);
    Recorded rec;
    InputBridge bridge(input, hooksFor(bench, rec));
    CHECK_FALSE(bridge.silentFacts().peerBypass);
    bench.setBypassed(output, true);
    bench.run(200);
    CHECK(bridge.silentFacts().peerBypass);
    CHECK(stateFields(bridge).getProperty("silentCause", {}).toString() == "peerBypass");
    bench.setBypassed(output, false);
    bench.run(200);
    CHECK_FALSE(bridge.silentFacts().peerBypass);
    // 别的组的 Output 旁路与这里无关
    REQUIRE(input.selectGroup(2));
    bench.setBypassed(output, true);
    bench.run(200);
    CHECK_FALSE(bridge.silentFacts().peerBypass);
}

// ---------------------------------------------------------------- notices

TEST_CASE("input bridge: notices are built from the five facts, in a fixed order", "[host][input_bridge][notices]")
{
    CHECK(buildNotices(NoticeFacts{}).isEmpty());

    const auto only = [](const NoticeFacts& facts) {
        const auto list = buildNotices(facts);
        REQUIRE(list.size() == 1);
        return list.getFirst();
    };
    NoticeFacts denied;
    denied.ipcDenied = true;
    CHECK(keysOf(only(denied)) == std::set<std::string>{"id"});
    CHECK(only(denied).getProperty("id", {}).toString() == "ipcSandbox");

    NoticeFacts order;
    order.outputFirst = true;
    CHECK(only(order).getProperty("id", {}).toString() == "outOfOrder");

    NoticeFacts unstable;
    unstable.unstable = true;
    CHECK(only(unstable).getProperty("id", {}).toString() == "unstable");

    // a 是工程的、b 是本构建的；页面取值时自己对调，这里不对调
    NoticeFacts newer;
    newer.heldNewerAbi = state::kAbi + 5u;
    const auto n = only(newer);
    CHECK(keysOf(n) == std::set<std::string>{"id", "a", "b"});
    CHECK(n.getProperty("id", {}).toString() == "newerProject");
    CHECK(n.getProperty("a", {}).toString() == "ABI " + juce::String(static_cast<int>(state::kAbi + 5u)));
    CHECK(n.getProperty("b", {}).toString() == "ABI " + juce::String(static_cast<int>(state::kAbi)));

    NoticeFacts corrupt;
    corrupt.decodeFailed = true;
    CHECK(only(corrupt).getProperty("id", {}).toString() == "stateDecodeFailed");

    const NoticeFacts all{true, true, true, 3u, true};
    const auto list = buildNotices(all);
    REQUIRE(list.size() == 5);
    std::vector<juce::String> ids;
    for (const auto& item : list)
        ids.push_back(item.getProperty("id", {}).toString());
    CHECK(ids ==
          std::vector<juce::String>{"ipcSandbox", "outOfOrder", "unstable", "newerProject", "stateDecodeFailed"});
}

TEST_CASE("input bridge: outOfOrder and unstable come from the loop status", "[host][input_bridge][notices]")
{
    namespace loop = rinne::loop;
    loop::LoopStatusFields fields;
    NoticeFacts facts;
    rinne::input::applyLoopFacts(facts, fields);
    CHECK_FALSE(facts.outputFirst);
    CHECK_FALSE(facts.unstable);
    fields.orderVerdict = static_cast<std::uint8_t>(loop::OrderVerdict::kOutputFirst);
    fields.unstable = 1u;
    rinne::input::applyLoopFacts(facts, fields);
    CHECK(facts.outputFirst);
    CHECK(facts.unstable);
    fields.orderVerdict = static_cast<std::uint8_t>(loop::OrderVerdict::kCorrect);
    fields.unstable = 0u;
    rinne::input::applyLoopFacts(facts, fields);
    CHECK_FALSE(facts.outputFirst);
    CHECK_FALSE(facts.unstable);
}

TEST_CASE("input bridge: newerProject and stateDecodeFailed come from the state glue", "[host][input_bridge][notices]")
{
    formalOnly();
    Rig rig;
    auto& bridge = rig.bridge;
    CHECK_FALSE(stateFields(bridge).hasProperty("notices"));

    auto newer = wiring_test::stateBytes(state::Role::Input, [](state::StateDocument& d) { d.configs.group = 2; });
    newer[4] = static_cast<std::uint8_t>(state::kAbi + 1u);
    restore(rig.input, newer);
    REQUIRE(rig.input.stateGlue()->holdingNewerState());
    CHECK(bridge.noticeFacts().heldNewerAbi == state::kAbi + 1u);
    const auto shown = noticeOf(stateFields(bridge), "newerProject");
    REQUIRE_FALSE(shown.isVoid());
    CHECK(shown.getProperty("a", {}).toString() == "ABI " + juce::String(static_cast<int>(state::kAbi + 1u)));
    CHECK(shown.getProperty("b", {}).toString() == "ABI " + juce::String(static_cast<int>(state::kAbi)));
    CHECK(noticeOf(stateFields(bridge), "stateDecodeFailed").isVoid());

    const std::vector<std::uint8_t> garbage(32, 0x5a);
    restore(rig.input, garbage);
    CHECK(bridge.noticeFacts().decodeFailed);
    CHECK_FALSE(noticeOf(stateFields(bridge), "stateDecodeFailed").isVoid());
    CHECK_FALSE(noticeOf(stateFields(bridge), "newerProject").isVoid());

    restore(rig.input, groupState(state::Role::Input, 3));
    CHECK_FALSE(rig.input.stateGlue()->holdingNewerState());
    CHECK(bridge.noticeFacts().heldNewerAbi == 0u);
    CHECK_FALSE(stateFields(bridge).hasProperty("notices"));
}

TEST_CASE("input bridge: ipcSandbox only when the backend says the system refused", "[host][input_bridge][notices]")
{
    formalOnly();
    needsSharedMemory();
    RefusingBackend denied(ipc::InitResult::kFailedDenied);
    RefusingBackend badName(ipc::InitResult::kFailedBadName);
    TempPresetDir presets;
    Bench bench;
    const auto addWith = [&](ipc::ISegmentBackend& backend) -> InputProcessor& {
        auto config = bench.config();
        config.backend = &backend;
        return bench.add(std::make_unique<InputProcessor>(config, presets.dir()), true);
    };
    InputProcessor& sandboxed = addWith(denied);
    InputProcessor& misnamed = addWith(badName);
    REQUIRE(sandboxed.selectGroup(1));
    REQUIRE(misnamed.selectGroup(1));
    bench.run(400);
    REQUIRE(stateOf(sandboxed) == ipc::RendezvousState::kIpcUnavailable);
    REQUIRE(stateOf(misnamed) == ipc::RendezvousState::kIpcUnavailable);

    Recorded rec;
    InputBridge sandboxedBridge(sandboxed, hooksFor(bench, rec));
    InputBridge misnamedBridge(misnamed, hooksFor(bench, rec));
    CHECK(sandboxedBridge.noticeFacts().ipcDenied);
    CHECK_FALSE(noticeOf(stateFields(sandboxedBridge), "ipcSandbox").isVoid());
    CHECK_FALSE(misnamedBridge.noticeFacts().ipcDenied);
    CHECK_FALSE(stateFields(misnamedBridge).hasProperty("notices"));
}

TEST_CASE("input bridge: outOfOrder follows the order evidence of the Input engine", "[host][input_bridge][notices]")
{
    formalOnly();
    needsSharedMemory();
    TempPresetDir presets;
    Bench bench;
    // 先建 Output：Bench 每个周期按建立顺序处理，Output 每拍都在 Input 前面
    OutputProcessor& output = bench.addOutput();
    InputProcessor& input = addInputIn(bench, presets.dir());
    REQUIRE(output.selectGroup(1));
    REQUIRE(input.selectGroup(1));
    Recorded rec;
    InputBridge bridge(input, hooksFor(bench, rec));
    const auto cycle = [&](std::uint64_t ms) {
        for (std::uint64_t t = 0; t < ms; t += 10)
        {
            bench.playHead.hostNs = bench.clock.ns;
            bench.step();
        }
    };
    bench.playHead.playing = true;
    cycle(400);
    for (int i = 0; i < 100 && !bridge.noticeFacts().outputFirst; ++i)
        cycle(10);
    CHECK(bridge.noticeFacts().outputFirst);
    CHECK_FALSE(noticeOf(stateFields(bridge), "outOfOrder").isVoid());
}

// ---------------------------------------------------------------- rinne.pair

TEST_CASE("input bridge: the peer's channels and sample rate only count while the peer is there",
          "[host][input_bridge][pair]")
{
    const auto factsOf = [](const diag::DiagPeer& facts) {
        auto* o = new juce::DynamicObject();
        juce::var v(o);
        addPeerAudioFacts(*o, facts);
        return v;
    };
    diag::DiagPeer peer;
    peer.channels = 2u;
    peer.sampleRate = 48000u;
    CHECK(keysOf(factsOf(peer)).empty());

    peer.present = true;
    CHECK(keysOf(factsOf(peer)) == std::set<std::string>{"channels", "sampleRate"});
    peer.channels = 1u;
    CHECK(static_cast<int>(factsOf(peer).getProperty("channels", {})) == 1);
    // 处理器只接受单声道与立体声：别的值不是真的声道数；采样率没给就不发
    peer.channels = 0u;
    peer.sampleRate = 0u;
    CHECK(keysOf(factsOf(peer)).empty());
    peer.channels = 3u;
    CHECK(keysOf(factsOf(peer)).empty());
}

TEST_CASE("input bridge: rinne.pair has the documented fields and only the optional ones it knows",
          "[host][input_bridge][pair]")
{
    formalOnly();
    needsSharedMemory();
    TempPresetDir presets;
    Bench bench;
    InputProcessor& input = addInputIn(bench, presets.dir());
    Recorded rec;
    InputBridge bridge(input, hooksFor(bench, rec));
    bench.run(200);
    const InputBridge::UiContext ui;

    SECTION("alone: no peer, no colour, nothing guessed")
    {
        const auto frame = bridge.firstFrames(ui).pair;
        CHECK(keysOf(frame) == std::set<std::string>{"seq", "state", "groupMask", "peer", "self", "verify"});
        CHECK(frame.getProperty("state", {}).toString() == "searching");
        const auto peer = frame.getProperty("peer", {});
        CHECK(keysOf(peer) == std::set<std::string>{"name", "color", "version", "holder"});
        for (const char* key : {"name", "color", "version", "holder"})
            CHECK(peer.getProperty(key, juce::var("x")).isVoid());
        const auto self = frame.getProperty("self", {});
        CHECK(keysOf(self) == std::set<std::string>{"name", "channels"});
        CHECK(self.getProperty("name", juce::var("x")).isVoid());
        CHECK(static_cast<int>(self.getProperty("channels", {})) == 2);
        CHECK(frame.getProperty("verify", {}).toString() == "unknown");
    }
    SECTION("paired with named, coloured tracks")
    {
        OutputProcessor& output = bench.addOutput();
        setTrack(input, juce::String("Vox Lead"), juce::Colour(0xff2d79c6));
        setTrack(output, juce::String("Vox Lead"), juce::Colour(0xff112233));
        REQUIRE(runUntil(bench, [&] { return pairStateOf(input) == PairState::kConnected; }, 4000));
        const auto frame = bridge.firstFrames(ui).pair;
        CHECK(frame.getProperty("state", {}).toString() == "connected");
        CHECK(frame.getProperty("verify", {}).toString() == "match");
        const auto peer = frame.getProperty("peer", {});
        CHECK(keysOf(peer) == std::set<std::string>{"name", "color", "channels", "version", "holder", "sampleRate"});
        CHECK(peer.getProperty("color", {}).toString() == "#112233");
        CHECK(static_cast<int>(peer.getProperty("sampleRate", {})) == 48000);
        const auto self = frame.getProperty("self", {});
        CHECK(keysOf(self) == std::set<std::string>{"name", "color", "channels"});
        CHECK(self.getProperty("color", {}).toString() == "#2d79c6");
        CHECK(self.getProperty("name", {}).toString() == "Vox Lead");
        // 宿主这次给了空名字：不算给了，沿用上次的
        setTrack(input, juce::String(), juce::Colour(0xff2d79c6));
        CHECK(bridge.firstFrames(ui).pair.getProperty("self", {}).getProperty("name", {}).toString() == "Vox Lead");
    }
    SECTION("a mono insert reports one channel")
    {
        juce::AudioProcessor::BusesLayout mono;
        mono.inputBuses.add(juce::AudioChannelSet::mono());
        mono.outputBuses.add(juce::AudioChannelSet::mono());
        REQUIRE(input.setBusesLayout(mono));
        CHECK(static_cast<int>(bridge.firstFrames(ui).pair.getProperty("self", {}).getProperty("channels", {})) == 1);
    }
}

// ---------------------------------------------------------------- rinne.meters 与灯

TEST_CASE("input bridge: rinne.meters has the documented fields, in dBFS, silence as null",
          "[host][input_bridge][meters]")
{
    formalOnly();
    Rig rig(false);
    auto& m = metersOf(rig.input);
    m.inputPeak.publishMax(0.5f);
    m.returnPeak.publishMax(1.0f);
    m.outputPeak.publishMax(0.25f);
    m.dryPartPeak.publishMax(0.1f);
    m.limiterReductionDb.publishMax(3.04f);
    m.highCutLimited.store(true);

    auto frames = rig.bridge.tick(rig.ui);
    REQUIRE_FALSE(frames.meters.isVoid());
    const auto& frame = frames.meters;
    CHECK(keysOf(frame) ==
          std::set<std::string>{"seq", "in", "ret", "out", "dryPart", "retPart", "gr", "highCutLimited", "lamps"});
    CHECK(static_cast<double>(frame.getProperty("in", {})) == -6.0);
    CHECK(static_cast<double>(frame.getProperty("ret", {})) == 0.0);
    CHECK(static_cast<double>(frame.getProperty("out", {})) == -12.0);
    CHECK(static_cast<double>(frame.getProperty("dryPart", {})) == -20.0);
    CHECK(frame.getProperty("retPart", juce::var("x")).isVoid());
    CHECK(static_cast<double>(frame.getProperty("gr", {})) == -3.0);
    CHECK(static_cast<bool>(frame.getProperty("highCutLimited", {})));
    CHECK(keysOf(frame.getProperty("lamps", {})) ==
          std::set<std::string>{"dc", "nonfinite", "guard", "nonfiniteCount", "guardDb"});

    // 读走即清零
    frames = rig.bridge.tick(rig.ui);
    REQUIRE_FALSE(frames.meters.isVoid());
    CHECK(frames.meters.getProperty("in", juce::var("x")).isVoid());
    CHECK(static_cast<double>(frames.meters.getProperty("gr", {})) == 0.0);

    // 桥函数之后的 poll 不碰峰值：高频写参数不会把峰值切碎
    m.inputPeak.publishMax(0.5f);
    REQUIRE(isOk(rig.bridge.setParam(args({juce::var("mix"), juce::var(40)}))));
    CHECK(rig.bridge.poll(rig.ui).meters.isVoid());
    CHECK(static_cast<double>(rig.bridge.tick(rig.ui).meters.getProperty("in", {})) == -6.0);

    CHECK(levelDb(0.0f).isVoid());
    CHECK(levelDb(std::numeric_limits<float>::infinity()).isVoid());
    CHECK(levelDb(std::numeric_limits<float>::quiet_NaN()).isVoid());
}

TEST_CASE("input bridge: peaks from before the first frame are not shown as current", "[host][input_bridge][meters]")
{
    formalOnly();
    Rig rig(false);
    auto& m = metersOf(rig.input);

    // 窗口关着时攒下的峰值：新开的桥一来就丢
    m.inputPeak.publishMax(0.9f);
    m.limiterReductionDb.publishMax(6.0f);
    {
        InputBridge reopened(rig.input, hooksFor(rig.bench, rig.rec));
        CHECK(reopened.tick(rig.ui).meters.getProperty("in", juce::var("x")).isVoid());
    }

    // 页面起来之前攒下的：首帧之前再丢一次，每种峰值都丢
    m.inputPeak.publishMax(0.9f);
    m.returnPeak.publishMax(0.9f);
    m.outputPeak.publishMax(0.9f);
    m.dryPartPeak.publishMax(0.9f);
    m.returnPartPeak.publishMax(0.9f);
    m.limiterReductionDb.publishMax(6.0f);
    const auto first = rig.bridge.firstFrames(rig.ui);
    REQUIRE_FALSE(first.meters.isVoid());
    for (const char* key : {"in", "ret", "out", "dryPart", "retPart"})
    {
        INFO(key);
        CHECK(first.meters.getProperty(key, juce::var("x")).isVoid());
    }
    CHECK(static_cast<double>(first.meters.getProperty("gr", {})) == 0.0);

    // 之后的是真的
    m.inputPeak.publishMax(0.5f);
    CHECK(static_cast<double>(rig.bridge.tick(rig.ui).meters.getProperty("in", {})) == -6.0);
}

TEST_CASE("input bridge: the three lamps from their facts", "[host][input_bridge][meters]")
{
    const auto lamp = [](const LampFacts& facts, const char* key) {
        return buildLamps(facts).getProperty(key, {}).toString();
    };
    LampFacts facts;
    CHECK(lamp(facts, "dc") == "ok");
    CHECK(lamp(facts, "nonfinite") == "ok");
    CHECK(lamp(facts, "guard") == "ok");
    CHECK(static_cast<double>(buildLamps(facts).getProperty("guardDb", {})) == 0.0);

    LampFacts dc;
    dc.dcHeld = true;
    CHECK(lamp(dc, "dc") == "blocked");

    LampFacts zeroed;
    zeroed.loopNan = true;
    zeroed.nonfiniteCount = 7u;
    CHECK(lamp(zeroed, "nonfinite") == "zeroed");
    CHECK(static_cast<int>(buildLamps(zeroed).getProperty("nonfiniteCount", {})) == 7);
    zeroed.cut = "nan";
    CHECK(lamp(zeroed, "nonfinite") == "latched");

    // 失控灯：切断 > 自动降低 > 持续压限；压低量取非负数，不到显示精度不算压低
    LampFacts guard;
    guard.guardGainDb = -3.26f;
    CHECK(lamp(guard, "guard") == "reduced");
    CHECK(static_cast<double>(buildLamps(guard).getProperty("guardDb", {})) == 3.3);
    guard.cut = "runaway";
    CHECK(lamp(guard, "guard") == "cut");
    LampFacts tiny;
    tiny.guardGainDb = -0.04f;
    CHECK(lamp(tiny, "guard") == "ok");
    CHECK(static_cast<double>(buildLamps(tiny).getProperty("guardDb", {})) == 0.0);

    // 持续压限要限制器开着
    LampFacts held;
    held.nearFullHeld = true;
    held.limiterOn = true;
    CHECK(lamp(held, "guard") == "limiting");
    held.limiterOn = false;
    CHECK(lamp(held, "guard") == "ok");
}

TEST_CASE("input bridge: the dc lamp needs -40 dBFS held for a second, on the injected clock",
          "[host][input_bridge][meters]")
{
    formalOnly();
    Rig rig(false);
    auto& m = metersOf(rig.input);
    const auto dcLamp = [&] { return lampsOf(rig.bridge.firstFrames(rig.ui)).getProperty("dc", {}).toString(); };

    // 电平两侧：正好在门限上不算超
    m.returnDcMean.store(InputBridge::kDcLampLevel);
    rig.bench.idle(2000);
    CHECK(dcLamp() == "ok");
    rig.bench.idle(2000);
    CHECK(dcLamp() == "ok");

    // 时长两侧
    m.returnDcMean.store(InputBridge::kDcLampLevel * 1.01f);
    CHECK(dcLamp() == "ok");
    rig.bench.idle(990);
    CHECK(dcLamp() == "ok");
    rig.bench.idle(10);
    CHECK(dcLamp() == "blocked");

    // 掉下来立刻灭，再超过要重新等满一秒
    m.returnDcMean.store(0.0f);
    CHECK(dcLamp() == "ok");
    m.returnDcMean.store(InputBridge::kDcLampLevel * 2.0f);
    CHECK(dcLamp() == "ok");
    rig.bench.idle(500);
    CHECK(dcLamp() == "ok");
}

TEST_CASE("input bridge: the limiting lamp needs the near-full time and the limiter on", "[host][input_bridge][meters]")
{
    formalOnly();
    Rig rig(false);
    auto& m = metersOf(rig.input);
    const auto guardLamp = [&] { return lampsOf(rig.bridge.firstFrames(rig.ui)).getProperty("guard", {}).toString(); };
    const auto limit = static_cast<float>(engine::safety::kGuardLimitSeconds);

    m.nearFullSeconds.store(limit * 0.99f);
    CHECK(guardLamp() == "ok");
    m.nearFullSeconds.store(limit);
    CHECK(guardLamp() == "limiting");
    CHECK_FALSE(static_cast<bool>(safetyOf(rig.bridge).getProperty("l2", {})));

    // 限制器关着：没有压限可言，灯不亮，也就不是 L2
    REQUIRE(isOk(rig.bridge.setLimiter(args({juce::var(false)}))));
    CHECK(guardLamp() == "ok");
    CHECK_FALSE(static_cast<bool>(safetyOf(rig.bridge).getProperty("l2", {})));

    // 限制器关而失控保护在压低：灯亮，这才是 L2
    m.guardGainDb.store(-6.0f);
    CHECK(guardLamp() == "reduced");
    CHECK(static_cast<bool>(safetyOf(rig.bridge).getProperty("l2", {})));
    REQUIRE(isOk(rig.bridge.setLimiter(args({juce::var(true)}))));
    CHECK_FALSE(static_cast<bool>(safetyOf(rig.bridge).getProperty("l2", {})));
}

TEST_CASE("input bridge: nonfiniteCount adds what the ring read and what the writer zeroed",
          "[host][input_bridge][meters]")
{
    formalOnly();
    needsSharedMemory();
    TempPresetDir presets;
    Bench bench;
    InputProcessor& input = addInputIn(bench, presets.dir());
    OutputProcessor& output = bench.addOutput();
    REQUIRE(input.selectGroup(1));
    REQUIRE(output.selectGroup(1));
    bench.run(300);
    Recorded rec;
    InputBridge bridge(input, hooksFor(bench, rec));
    const InputBridge::UiContext ui;
    const auto count = [&] {
        return static_cast<int>(lampsOf(bridge.firstFrames(ui)).getProperty("nonfiniteCount", {}));
    };
    REQUIRE(count() == 0);

    // 环里读到的
    metersOf(input).returnNonFinite.store(3u);
    CHECK(count() == 3);

    // 写方写环前清洗掉的：往 Output 送一块带 NaN 的
    std::array<std::vector<float>, 2> nan;
    for (auto& ch : nan)
        ch.assign(static_cast<std::size_t>(kBlock), std::numeric_limits<float>::quiet_NaN());
    float* channels[2] = {nan[0].data(), nan[1].data()};
    juce::AudioBuffer<float> buffer(channels, 2, kBlock);
    juce::MidiBuffer midi;
    output.processBlock(buffer, midi);
    bench.run(100);
    metersOf(input).returnNonFinite.store(3u);
    CHECK(count() > 3);
}

// ---------------------------------------------------------------- 事件节拍

TEST_CASE("input bridge: the first frames go out together and are not sent again on the next beat",
          "[host][input_bridge][events]")
{
    formalOnly();
    Rig rig;
    rig.bench.run(1500); // 智能默认走完
    auto& bridge = rig.bridge;
    const auto first = bridge.firstFrames(rig.ui);
    CHECK_FALSE(first.state); // rinne.state 是 WebViewHost 在调这里之前发的
    REQUIRE_FALSE(first.pair.isVoid());
    REQUIRE_FALSE(first.params.isVoid());
    REQUIRE_FALSE(first.meters.isVoid());
    REQUIRE_FALSE(first.presets.isVoid());

    // 紧接着的一拍什么都不重发
    const auto next = bridge.tick(rig.ui);
    CHECK_FALSE(next.state);
    CHECK(next.pair.isVoid());
    CHECK(next.params.isVoid());
    CHECK(next.meters.isVoid());
    CHECK(next.presets.isVoid());

    // 重复请求每次都再发，seq 接着数
    const auto again = bridge.firstFrames(rig.ui);
    REQUIRE_FALSE(again.pair.isVoid());
    REQUIRE_FALSE(again.params.isVoid());
    REQUIRE_FALSE(again.meters.isVoid());
    REQUIRE_FALSE(again.presets.isVoid());
    CHECK(static_cast<int>(again.pair.getProperty("seq", {})) > static_cast<int>(first.pair.getProperty("seq", {})));
    CHECK(static_cast<int>(again.params.getProperty("seq", {})) >
          static_cast<int>(first.params.getProperty("seq", {})));
    CHECK(static_cast<int>(again.meters.getProperty("seq", {})) >
          static_cast<int>(first.meters.getProperty("seq", {})));
}

TEST_CASE("input bridge: every event falls back to once a second", "[host][input_bridge][events]")
{
    formalOnly();
    Rig rig;
    rig.bench.run(1500);
    auto& bridge = rig.bridge;
    bridge.firstFrames(rig.ui);

    int states = 0;
    int pairs = 0;
    int params = 0;
    int meters = 0;
    int presets = 0;
    const auto ticking = [&](std::uint64_t ms) {
        for (std::uint64_t t = 0; t < ms; t += 10)
        {
            rig.bench.step();
            const auto f = bridge.tick(rig.ui);
            states += f.state ? 1 : 0;
            pairs += f.pair.isVoid() ? 0 : 1;
            params += f.params.isVoid() ? 0 : 1;
            meters += f.meters.isVoid() ? 0 : 1;
            presets += f.presets.isVoid() ? 0 : 1;
        }
    };
    ticking(990);
    CHECK(states == 0);
    CHECK(pairs == 0);
    CHECK(params == 0);
    CHECK(meters == 0);
    CHECK(presets == 0);
    ticking(20);
    CHECK(states == 1);
    CHECK(pairs == 1);
    CHECK(params == 1);
    CHECK(meters == 1);
    CHECK(presets == 1);
    ticking(1000);
    CHECK(states == 2);
    CHECK(pairs == 2);
    CHECK(params == 2);
    CHECK(meters == 2);
    CHECK(presets == 2);
}

TEST_CASE("input bridge: changes go out on the same beat", "[host][input_bridge][events]")
{
    formalOnly();
    Rig rig;
    rig.bench.run(1500);
    auto& bridge = rig.bridge;
    auto ui = rig.ui;
    bridge.firstFrames(ui);

    // 设置变了：rinne.state 当拍发；配对没变：rinne.pair 不发
    REQUIRE(isOk(bridge.setTailMode(args({juce::var("infinite")}))));
    auto frames = bridge.poll(ui);
    CHECK(frames.state);
    CHECK(frames.pair.isVoid());
    CHECK_FALSE(bridge.poll(ui).state);

    // 全局位签名看不见，桥函数改了照样当拍发
    REQUIRE(isOk(bridge.setRulesAck(args({juce::var(7)}))));
    CHECK(bridge.poll(ui).state);
    CHECK_FALSE(bridge.poll(ui).state);
    REQUIRE(isOk(bridge.setGuideSeen(args({juce::var(true)}))));
    CHECK(bridge.poll(ui).state);
    CHECK_FALSE(bridge.poll(ui).state);
    REQUIRE(isOk(bridge.setDiagLog(args({juce::var(false)}))));
    CHECK(bridge.poll(ui).state);
    CHECK_FALSE(bridge.poll(ui).state);

    // 界面那边换了语言或缩放也算变
    ui.lang = "fr";
    CHECK(bridge.poll(ui).state);
    CHECK_FALSE(bridge.poll(ui).state);
    ui.uiScale = 1.25f;
    CHECK(bridge.poll(ui).state);
    CHECK_FALSE(bridge.poll(ui).state);

    // 参数变了：rinne.params 不等兜底就发
    auto* parameter = rig.input.parameterLayer()->tree().getParameter("polarity");
    parameter->setValueNotifyingHost(1.0f);
    frames = bridge.poll(ui);
    REQUIRE_FALSE(frames.params.isVoid());
    CHECK(static_cast<bool>(frames.params.getProperty("values", {}).getProperty("polarity", {})));

    // 电平变了：rinne.meters 当拍发
    metersOf(rig.input).inputPeak.publishMax(0.5f);
    CHECK_FALSE(bridge.tick(ui).meters.isVoid());
}

TEST_CASE("input bridge: rinne.pair goes out when the pair changes", "[host][input_bridge][events]")
{
    formalOnly();
    needsSharedMemory();
    TempPresetDir presets;
    Bench bench;
    InputProcessor& input = addInputIn(bench, presets.dir());
    Recorded rec;
    InputBridge bridge(input, hooksFor(bench, rec));
    bench.run(1500);
    const InputBridge::UiContext ui;
    bridge.firstFrames(ui);
    OutputProcessor& output = bench.addOutput();
    REQUIRE(output.selectGroup(groupOf(input)));
    int pairs = 0;
    juce::var last;
    for (int i = 0; i < 30; ++i)
    {
        bench.step();
        const auto f = bridge.poll(ui);
        if (!f.pair.isVoid())
        {
            ++pairs;
            last = f.pair;
        }
    }
    CHECK(pairs >= 1);
    CHECK(last.getProperty("state", {}).toString() != "searching");
}

TEST_CASE("input bridge: a state that cannot be encoded is reported once on rinne.error",
          "[host][input_bridge][events]")
{
    formalOnly();
    Rig rig;
    const InputBridge::UiContext ui;
    CHECK(rig.bridge.poll(ui).errors.isEmpty());

    rig.input.stateGlue()->editConfigs([](state::Configs& c) { c.unknown.push_back(state::ConfigEntry{4, {0}}); });
    CHECK(savedState(rig.input).empty());
    auto frames = rig.bridge.poll(ui);
    REQUIRE(frames.errors.size() == 1);
    CHECK(keysOf(frames.errors.getFirst()) == std::set<std::string>{"seq", "code"});
    CHECK(frames.errors.getFirst().getProperty("code", {}).toString() == "stateEncode");
    CHECK(rig.bridge.poll(ui).errors.isEmpty());

    InputBridge reopened(rig.input, hooksFor(rig.bench, rig.rec));
    CHECK(reopened.poll(ui).errors.isEmpty());
    CHECK(savedState(rig.input).empty());
    CHECK(reopened.poll(ui).errors.size() == 1);
}
