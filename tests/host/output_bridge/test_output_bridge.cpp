// SPDX-License-Identifier: GPL-3.0-or-later
//
// Output 的原生桥（OutputBridge，编辑器的纯逻辑那一层）接在真处理器上：每个角色函数的回执与失败码、rinne.state 与
// rinne.pair 的字段集合（拿不到的可缺省字段确实不出现）、notices 的四个来源、tail 模式的往返与标脏、复制诊断、
// 接管确认条、事件的变更检测与 1 Hz 兜底。时钟、剪贴板、浏览器与许可证都是替身；全局设置写进测试沙盒的目录。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "../pairing/PairingKit.h"
#include "OutputBridge.h"
#include "UiDefaultsStore.h"
#include "state/StateCodec.h"

namespace
{

using namespace rinne::pairing_test;
using rinne::InputProcessor;
using rinne::InstanceHostConfig;
using rinne::OutputProcessor;
using rinne::output::buildNotices;
using rinne::output::NoticeFacts;
using rinne::output::OutputBridge;
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

OutputBridge::Hooks hooksFor(Bench& bench, Recorded& rec)
{
    OutputBridge::Hooks hooks;
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

// 一帧 rinne.state 的 Output 部分（快照里共用的那几项由 WebViewHost 补，不在这一层）
juce::var stateFields(OutputBridge& bridge)
{
    auto* o = new juce::DynamicObject();
    juce::var v(o);
    bridge.addStateFields(*o);
    return v;
}

// 一帧 rinne.pair：先把基线对齐，再取首帧（与 requestInitialState 之后的那一帧同源）
juce::var pairFrame(OutputBridge& bridge)
{
    return bridge.firstFrames(OutputBridge::UiContext{});
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

// 只拒绝的后端：会合段与对端视图一律打不开。result 决定拒绝的形状
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

OutputProcessor& addOutputWith(Bench& bench, ipc::ISegmentBackend& backend)
{
    InstanceHostConfig config = bench.config();
    config.backend = &backend;
    return bench.add(std::make_unique<OutputProcessor>(config), true);
}

} // namespace

// ---------------------------------------------------------------- 回执

TEST_CASE("output bridge: the global-flag functions answer with the documented receipts", "[host][output_bridge]")
{
    formalOnly();
    Bench bench;
    OutputProcessor& output = bench.addOutput();
    Recorded rec;
    OutputBridge bridge(output, hooksFor(bench, rec));

    SECTION("setRulesAck takes a non-negative integer")
    {
        CHECK(isOk(bridge.setRulesAck(args({juce::var(3)}))));
        CHECK(uidefaults::rulesAckVersion() == 3);
        for (const auto& bad : {args({}), args({juce::var(-1)}), args({juce::var("3")}), args({juce::var(1.5)})})
        {
            const auto r = bridge.setRulesAck(bad);
            CHECK_FALSE(isOk(r));
            CHECK(reasonOf(r) == "bad-version");
        }
        CHECK(uidefaults::rulesAckVersion() == 3);
    }
    SECTION("setGuideSeen only takes true and writes the Output flag")
    {
        for (const auto& bad : {args({}), args({juce::var(false)}), args({juce::var(1)}), args({juce::var("true")})})
            CHECK(reasonOf(bridge.setGuideSeen(bad)) == "bad-value");
        CHECK_FALSE(uidefaults::guideSeenGlobalOutput());
        CHECK(isOk(bridge.setGuideSeen(args({juce::var(true)}))));
        CHECK(uidefaults::guideSeenGlobalOutput());
        CHECK_FALSE(uidefaults::guideSeenGlobalInput());
    }
    SECTION("setDiagLog takes a boolean and echoes it")
    {
        const auto on = bridge.setDiagLog(args({juce::var(true)}));
        CHECK(isOk(on));
        CHECK(on.getProperty("value", juce::var()).isBool());
        CHECK(static_cast<bool>(on.getProperty("value", juce::var())));
        CHECK(uidefaults::diagLogEnabled());
        for (const auto& bad : {args({}), args({juce::var(0)}), args({juce::var("false")})})
            CHECK(reasonOf(bridge.setDiagLog(bad)) == "bad-value");
        CHECK(uidefaults::diagLogEnabled());
        CHECK(isOk(bridge.setDiagLog(args({juce::var(false)}))));
        CHECK_FALSE(uidefaults::diagLogEnabled());
    }
    SECTION("none of them settles the instance or marks the project dirty")
    {
        DirtyListener dirty;
        output.addListener(&dirty);
        bridge.setRulesAck(args({juce::var(2)}));
        bridge.setGuideSeen(args({juce::var(true)}));
        bridge.setDiagLog(args({juce::var(true)}));
        CHECK(dirty.dirtyMarks == 0);
        CHECK_FALSE(settled(output));
        output.removeListener(&dirty);
    }
}

TEST_CASE("output bridge: setGroup, openExternal, showLicense and setTailMode receipts", "[host][output_bridge]")
{
    formalOnly();
    Bench bench;
    OutputProcessor& output = bench.addOutput();
    Recorded rec;
    OutputBridge bridge(output, hooksFor(bench, rec));
    DirtyListener dirty;
    output.addListener(&dirty);

    SECTION("setGroup takes a letter, settles and marks dirty")
    {
        for (const auto& bad : {args({}), args({juce::var("Z")}), args({juce::var("c")}), args({juce::var(3)})})
            CHECK(reasonOf(bridge.setGroup(bad)) == "bad-group");
        CHECK(dirty.dirtyMarks == 0);
        const auto r = bridge.setGroup(args({juce::var("C")}));
        CHECK(isOk(r));
        CHECK(r.getProperty("group", juce::var()).toString() == "C");
        CHECK(configsOf(output).group == 3);
        CHECK(configsOf(output).groupOrigin == state::GroupOrigin::User);
        CHECK(settled(output));
        CHECK(dirty.dirtyMarks == 1);
        CHECK(groupOf(output) == 3);
    }
    SECTION("openExternal goes through the whitelist before the launcher")
    {
        CHECK(isOk(bridge.openExternal(args({juce::var("https://github.com/synchain-oss/rinne/issues")}))));
        CHECK(reasonOf(bridge.openExternal(args({juce::var("https://github.com/synchain-oss/rinne?x")}))) ==
              "not-allowed");
        REQUIRE(rec.launched.size() == 1);
        CHECK(rec.launched.front() == "https://github.com/synchain-oss/rinne/issues");
        CHECK_FALSE(settled(output));
    }
    SECTION("showLicense reports whether the system opened it")
    {
        CHECK(isOk(bridge.showLicense()));
        rec.licenseWorks = false;
        CHECK(reasonOf(bridge.showLicense()) == "unavailable");
        CHECK(rec.licenses == 2);
        CHECK_FALSE(settled(output));
    }
    SECTION("setTailMode takes the three names and nothing else")
    {
        for (const auto& bad : {args({}), args({juce::var("Auto")}), args({juce::var("30")}), args({juce::var(1)})})
            CHECK(reasonOf(bridge.setTailMode(bad)) == "bad-value");
        CHECK(dirty.dirtyMarks == 0);
        CHECK_FALSE(settled(output));
        for (const char* mode : {"infinite", "30s", "auto"})
        {
            const auto r = bridge.setTailMode(args({juce::var(mode)}));
            CHECK(isOk(r));
            CHECK(r.getProperty("value", juce::var()).toString() == mode);
        }
        CHECK(dirty.dirtyMarks == 3);
        CHECK(settled(output));
    }
    SECTION("resolveTakeover with nothing pending")
    {
        CHECK(reasonOf(bridge.resolveTakeover(args({juce::var(true)}))) == "no-pending");
        CHECK(reasonOf(bridge.resolveTakeover(args({juce::var(false)}))) == "no-pending");
        CHECK(reasonOf(bridge.resolveTakeover(args({juce::var(1)}))) == "bad-value");
        CHECK(reasonOf(bridge.resolveTakeover(args({}))) == "bad-value");
        CHECK_FALSE(settled(output));
    }
    output.removeListener(&dirty);
}

TEST_CASE("output bridge: repairPair outside a conflict", "[host][output_bridge]")
{
    formalOnly();
    Bench bench;
    OutputProcessor& output = bench.addOutput();
    Recorded rec;
    OutputBridge bridge(output, hooksFor(bench, rec));
    bench.run(200);
    const auto r = bridge.repairPair();
    CHECK_FALSE(isOk(r));
    CHECK(reasonOf(r) == "not-conflict");
    CHECK_FALSE(settled(output));
}

TEST_CASE("output bridge: repairPair re-pairs a duplicated Output", "[host][output_bridge]")
{
    formalOnly();
    needsSharedMemory();
    Bench bench;
    OutputProcessor& original = bench.addOutput();
    REQUIRE(original.selectGroup(1));
    OutputProcessor& copy = bench.addOutput();
    restore(copy, groupState(state::Role::Output, 1));
    bench.run(300);
    REQUIRE(stateOf(copy) == ipc::RendezvousState::kConflict);

    Recorded rec;
    OutputBridge bridge(copy, hooksFor(bench, rec));
    const auto r = bridge.repairPair();
    REQUIRE(isOk(r));
    CHECK(r.getProperty("group", juce::var()).toString() == "B");
    CHECK(settled(copy));
    CHECK(runUntil(bench, [&] { return holds(copy, 2); }, 2000));
    CHECK_FALSE(bridge.pendingTakeover().has_value());
}

TEST_CASE("output bridge: a repair onto a dormant slot asks first, then takes it over or cancels",
          "[host][output_bridge][takeover]")
{
    formalOnly();
    needsSharedMemory();
    Bench bench;
    std::vector<OutputProcessor*> holders;
    for (int group = 1; group <= 8; ++group)
    {
        OutputProcessor& holder = bench.addOutput();
        REQUIRE(holder.selectGroup(group));
        holders.push_back(&holder);
    }
    OutputProcessor& copy = bench.addOutput();
    restore(copy, groupState(state::Role::Output, 1));
    bench.run(300);
    REQUIRE(stateOf(copy) == ipc::RendezvousState::kConflict);

    Recorded rec;
    OutputBridge bridge(copy, hooksFor(bench, rec));

    // 八个组的 Output 槽都被活着的实例占着：没有可去的组
    const auto none = bridge.repairPair();
    CHECK_FALSE(isOk(none));
    CHECK(reasonOf(none) == "no-free-group");
    CHECK_FALSE(bridge.pendingTakeover().has_value());

    // 组 B 的 Output 被宿主停用：槽休眠，能去的只剩它，要用户确认
    holders[1]->releaseResources();
    bench.setRunning(*holders[1], false);
    bench.run(300);
    DirtyListener dirty;
    copy.addListener(&dirty);
    const auto asked = bridge.repairPair();
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
        CHECK_FALSE(bridge.pendingTakeover().has_value());
        CHECK(settled(copy));
        CHECK(dirty.dirtyMarks == 1);
        CHECK(configsOf(copy).group == 2);
        CHECK(configsOf(copy).groupOrigin == state::GroupOrigin::User);
        CHECK(runUntil(bench, [&] { return holds(copy, 2); }, 4000));
        const auto after = stateFields(bridge);
        CHECK_FALSE(static_cast<bool>(after.getProperty("takeover", juce::var())));
        CHECK_FALSE(after.hasProperty("takeoverGroup"));
        // 接管过了，再确认就没有可确认的
        CHECK(reasonOf(bridge.resolveTakeover(args({juce::var(true)}))) == "no-pending");
    }
    SECTION("cancel leaves everything where it was")
    {
        const auto r = bridge.resolveTakeover(args({juce::var(false)}));
        CHECK(isOk(r));
        CHECK_FALSE(static_cast<bool>(r.getProperty("accepted", juce::var())));
        CHECK_FALSE(bridge.pendingTakeover().has_value());
        CHECK_FALSE(settled(copy));
        CHECK(dirty.dirtyMarks == 0);
        bench.run(300);
        CHECK(groupOf(copy) == 1);
        CHECK(stateOf(copy) == ipc::RendezvousState::kConflict);
    }
    SECTION("picking a group by hand drops the pending takeover")
    {
        CHECK(isOk(bridge.setGroup(args({juce::var("A")}))));
        CHECK_FALSE(bridge.pendingTakeover().has_value());
        CHECK(reasonOf(bridge.resolveTakeover(args({juce::var(true)}))) == "no-pending");
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

// ---------------------------------------------------------------- rinne.state

TEST_CASE("output bridge: rinne.state carries the Output fields and leaves out what it does not know",
          "[host][output_bridge][state]")
{
    formalOnly();
    Bench bench;
    OutputProcessor& output = bench.addOutput(false);
    Recorded rec;
    OutputBridge bridge(output, hooksFor(bench, rec));

    SECTION("before prepare: no sample rate anywhere, nothing optional invented")
    {
        const auto fields = stateFields(bridge);
        CHECK(keysOf(fields) == std::set<std::string>{"restored", "takeover", "group", "groupOrigin", "send", "build",
                                                      "cfg", "inputGuideSeen"});
        CHECK(keysOf(fields.getProperty("send", {})) == std::set<std::string>{"channels", "bypassed", "hostPaused"});
        CHECK_FALSE(static_cast<bool>(fields.getProperty("restored", {})));
        CHECK_FALSE(static_cast<bool>(fields.getProperty("takeover", {})));
        CHECK(fields.getProperty("group", juce::var("x")).isVoid());
        CHECK(fields.getProperty("groupOrigin", {}).toString() == "auto");
        CHECK(fields.getProperty("send", {}).getProperty("channels", {}).toString() == "ST");
        CHECK_FALSE(static_cast<bool>(fields.getProperty("send", {}).getProperty("hostPaused", {})));
        CHECK(fields.getProperty("cfg", {}).getProperty("tailMode", {}).toString() == "auto");
        CHECK_FALSE(static_cast<bool>(fields.getProperty("inputGuideSeen", {})));
        // 宿主测试里没有包装层，也没有构建标签：build 只剩编译时就定了的平台
        CHECK(keysOf(fields.getProperty("build", {})) == std::set<std::string>{"arch"});
        // 从没处理过音频的实例不算「宿主停了」，等多久都一样
        bench.idle(300);
        CHECK_FALSE(static_cast<bool>(stateFields(bridge).getProperty("send", {}).getProperty("hostPaused", {})));
    }
    SECTION("after prepare and a manual pick")
    {
        output.prepareToPlay(kRate, kBlock);
        REQUIRE(output.selectGroup(4));
        bench.run(200);
        uidefaults::setGuideSeenGlobalInput(true);
        const auto fields = stateFields(bridge);
        CHECK(keysOf(fields) == std::set<std::string>{"restored", "takeover", "group", "groupOrigin", "sampleRate",
                                                      "send", "build", "cfg", "inputGuideSeen"});
        CHECK(fields.getProperty("group", {}).toString() == "D");
        CHECK(fields.getProperty("groupOrigin", {}).toString() == "manual");
        CHECK(static_cast<int>(fields.getProperty("sampleRate", {})) == 48000);
        const auto send = fields.getProperty("send", {});
        CHECK(keysOf(send) == std::set<std::string>{"channels", "sampleRate", "bypassed", "hostPaused"});
        CHECK(static_cast<int>(send.getProperty("sampleRate", {})) == 48000);
        CHECK_FALSE(static_cast<bool>(send.getProperty("bypassed", {})));
        CHECK(static_cast<bool>(fields.getProperty("inputGuideSeen", {})));
    }
    SECTION("a mono insert reports MONO")
    {
        juce::AudioProcessor::BusesLayout mono;
        mono.inputBuses.add(juce::AudioChannelSet::mono());
        mono.outputBuses.add(juce::AudioChannelSet::mono());
        REQUIRE(output.setBusesLayout(mono));
        CHECK(stateFields(bridge).getProperty("send", {}).getProperty("channels", {}).toString() == "MONO");
    }
}

TEST_CASE("output bridge: the build line takes the commit from the label and leaves out what it cannot tell",
          "[host][output_bridge][state]")
{
    using rinne::output::buildInfo;
    using Wrapper = juce::AudioProcessor::WrapperType;
    const auto sha = [](const juce::var& build) { return build.getProperty("sha", juce::var()); };

    const auto vst3 = buildInfo("0.10.12+5f3a9c1", Wrapper::wrapperType_VST3);
    CHECK(keysOf(vst3) == std::set<std::string>{"sha", "fmt", "arch"});
    CHECK(sha(vst3).toString() == "5f3a9c1");
    CHECK(vst3.getProperty("fmt", {}).toString() == "VST3");
    CHECK(vst3.getProperty("arch", {}).toString().isNotEmpty());
    CHECK(buildInfo("x", Wrapper::wrapperType_AudioUnit).getProperty("fmt", {}).toString() == "AU");
    // 最后一个 + 之后的才是提交；40 位的完整散列也认
    const juce::String full = "0123456789abcdef0123456789abcdef01234567";
    CHECK(sha(buildInfo("1.0.0-rc.1+ci+" + full, Wrapper::wrapperType_VST3)).toString() == full);

    // 不是 7 到 40 位小写十六进制的一概不发；别的包装层不发 fmt
    for (const char* label : {"", "0.10.12", "0.10.12+", "0.10.12+5f3a9c", "0.10.12+5F3A9C1", "0.10.12+5f3a9cg",
                              "0.10.12+dirty", "0.10.12+0123456789abcdef0123456789abcdef012345678"})
    {
        INFO(label);
        CHECK(sha(buildInfo(label, Wrapper::wrapperType_VST3)).isVoid());
    }
    const auto standalone = buildInfo("0.10.12+5f3a9c1", Wrapper::wrapperType_Standalone);
    CHECK(keysOf(standalone) == std::set<std::string>{"sha", "arch"});
}

TEST_CASE("output bridge: send reports a host that stopped calling", "[host][output_bridge][state]")
{
    formalOnly();
    Bench bench;
    OutputProcessor& output = bench.addOutput();
    Recorded rec;
    OutputBridge bridge(output, hooksFor(bench, rec));
    bench.run(200);
    auto send = [&] { return stateFields(bridge).getProperty("send", {}); };
    CHECK_FALSE(static_cast<bool>(send().getProperty("hostPaused", {})));

    // 宿主不再调音频回调：250 ms 之后算暂停，回调一恢复就不算
    bench.setRunning(output, false);
    bench.idle(100);
    CHECK_FALSE(static_cast<bool>(send().getProperty("hostPaused", {})));
    bench.idle(200);
    CHECK(static_cast<bool>(send().getProperty("hostPaused", {})));
    bench.setRunning(output, true);
    bench.run(20);
    CHECK_FALSE(static_cast<bool>(send().getProperty("hostPaused", {})));
}

TEST_CASE("output bridge: send reports the host bypass", "[host][output_bridge][state]")
{
    formalOnly();
    // bypassed 看引擎的写状态（旁路写零）：要有环。验证宿主副本不开段，写状态停在 kIdle，配对态是 validator
    needsSharedMemory();
    Bench bench;
    OutputProcessor& output = bench.addOutput();
    Recorded rec;
    OutputBridge bridge(output, hooksFor(bench, rec));
    bench.run(200);
    auto send = [&] { return stateFields(bridge).getProperty("send", {}); };
    CHECK_FALSE(static_cast<bool>(send().getProperty("bypassed", {})));

    bench.setBypassed(output, true);
    bench.run(200);
    CHECK(static_cast<bool>(send().getProperty("bypassed", {})));
    bench.setBypassed(output, false);
    bench.run(200);
    CHECK_FALSE(static_cast<bool>(send().getProperty("bypassed", {})));
}

TEST_CASE("output bridge: a restored project shows as restored with its group and tail mode", "[host][output_bridge]")
{
    formalOnly();
    Bench bench;
    OutputProcessor& output = bench.addOutput();
    const auto bytes = wiring_test::stateBytes(state::Role::Output, [](state::StateDocument& d) {
        d.configs.group = 5;
        d.configs.groupOrigin = state::GroupOrigin::User;
        d.configs.tailMode = state::TailMode::Infinite;
    });
    restore(output, bytes);
    bench.run(200);
    Recorded rec;
    OutputBridge bridge(output, hooksFor(bench, rec));
    const auto fields = stateFields(bridge);
    CHECK(static_cast<bool>(fields.getProperty("restored", {})));
    CHECK(fields.getProperty("group", {}).toString() == "E");
    CHECK(fields.getProperty("groupOrigin", {}).toString() == "project");
    CHECK(fields.getProperty("cfg", {}).getProperty("tailMode", {}).toString() == "infinite");
    CHECK_FALSE(fields.hasProperty("notices"));
}

// ---------------------------------------------------------------- setTailMode

TEST_CASE("output bridge: setTailMode round-trips through the project", "[host][output_bridge][tail]")
{
    formalOnly();
    Bench bench;
    OutputProcessor& output = bench.addOutput();
    Recorded rec;
    OutputBridge bridge(output, hooksFor(bench, rec));
    DirtyListener dirty;
    output.addListener(&dirty);

    REQUIRE(isOk(bridge.setTailMode(args({juce::var("30s")}))));
    CHECK(dirty.dirtyMarks == 1);
    CHECK(settled(output));
    CHECK(configsOf(output).tailMode == state::TailMode::ThirtySeconds);
    CHECK(output.stateGlue()->tailMode() == state::TailMode::ThirtySeconds);
    CHECK(output.getTailMode() == rinne::RinneProcessorBase::TailMode::ThirtySeconds);
    CHECK(output.getTailLengthSeconds() == 30.0);
    CHECK(stateFields(bridge).getProperty("cfg", {}).getProperty("tailMode", {}).toString() == "30s");

    // 存进工程，换一个实例读回来：值保持，外壳报给宿主的 tail 也跟着
    const auto saved = savedState(output);
    OutputProcessor& reopened = bench.addOutput();
    restore(reopened, saved);
    CHECK(configsOf(reopened).tailMode == state::TailMode::ThirtySeconds);
    CHECK(reopened.getTailMode() == rinne::RinneProcessorBase::TailMode::ThirtySeconds);
    OutputBridge reopenedBridge(reopened, hooksFor(bench, rec));
    CHECK(stateFields(reopenedBridge).getProperty("cfg", {}).getProperty("tailMode", {}).toString() == "30s");

    // 不认识的值什么都不改
    CHECK(reasonOf(bridge.setTailMode(args({juce::var("forever")}))) == "bad-value");
    CHECK(dirty.dirtyMarks == 1);
    CHECK(configsOf(output).tailMode == state::TailMode::ThirtySeconds);
    output.removeListener(&dirty);
}

// ---------------------------------------------------------------- copyDiagnostics

TEST_CASE("output bridge: copyDiagnostics previews and copies the text with the ui line", "[host][output_bridge][diag]")
{
    formalOnly();
    Bench bench;
    OutputProcessor& output = bench.addOutput();
    Recorded rec;
    OutputBridge bridge(output, hooksFor(bench, rec));
    bench.run(100);
    OutputBridge::UiContext ui;
    ui.lang = "en";
    ui.uiScale = 1.5f;
    ui.webview = "WebView2 131.0.2903.70";

    const auto preview = bridge.copyDiagnostics(args({juce::var(false)}), ui);
    REQUIRE(isOk(preview));
    CHECK_FALSE(static_cast<bool>(preview.getProperty("copied", {})));
    const juce::String text = preview.getProperty("text", {}).toString();
    CHECK(text.startsWith("rinne-diag v1\n"));
    CHECK(text.endsWith("\nui: webview=WebView2-131.0.2903.70 scale=1.50 lang=en"));
    CHECK(text.contains("product=Rinne Host Test "));
    CHECK(rec.clipboard.empty());

    const auto copied = bridge.copyDiagnostics(args({juce::var(true)}), ui);
    CHECK(isOk(copied));
    CHECK(static_cast<bool>(copied.getProperty("copied", {})));
    REQUIRE(rec.clipboard.size() == 1);
    CHECK(rec.clipboard.front() == copied.getProperty("text", {}).toString());

    rec.clipboardWorks = false;
    const auto failed = bridge.copyDiagnostics(args({juce::var(true)}), ui);
    CHECK_FALSE(isOk(failed));
    CHECK_FALSE(static_cast<bool>(failed.getProperty("copied", {})));
    CHECK(reasonOf(failed) == "clipboard");
    CHECK_FALSE(settled(output));
}

TEST_CASE("output bridge: copyDiagnostics says not-ready when there is no diagnostics text",
          "[host][output_bridge][diag]")
{
    // 正式构建里诊断文本总拿得到；探针构建没有正式的诊断，处理器交空串
    if constexpr (!rinne::build::kProbeBuild)
        SKIP("formal build: the diagnostics text is always there");
    InstanceHostConfig config;
    config.scope = ipc::NameScope::randomForTesting();
    config.startTimer = false;
    OutputProcessor output(config);
    std::vector<juce::String> written;
    OutputBridge::Hooks hooks;
    hooks.writeClipboard = [&written](const juce::String& text) {
        written.push_back(text);
        return true;
    };
    OutputBridge bridge(output, hooks);
    const auto r = bridge.copyDiagnostics(args({juce::var(true)}), OutputBridge::UiContext{});
    CHECK_FALSE(isOk(r));
    CHECK(reasonOf(r) == "not-ready");
    CHECK(written.empty());
}

// ---------------------------------------------------------------- rinne.pair

TEST_CASE("output bridge: rinne.pair has the documented fields and only the optional ones it knows",
          "[host][output_bridge][pair]")
{
    formalOnly();
    needsSharedMemory();
    Bench bench;
    OutputProcessor& output = bench.addOutput();
    Recorded rec;
    OutputBridge bridge(output, hooksFor(bench, rec));
    bench.run(200);

    SECTION("alone: no peer, no colour, nothing guessed")
    {
        const auto frame = pairFrame(bridge);
        CHECK(keysOf(frame) == std::set<std::string>{"seq", "state", "groupMask", "peer", "self", "verify"});
        CHECK(frame.getProperty("state", {}).toString() == "searching");
        CHECK(static_cast<int>(frame.getProperty("groupMask", {})) == 0);
        const auto peer = frame.getProperty("peer", {});
        CHECK(keysOf(peer) == std::set<std::string>{"name", "color", "version", "holder"});
        for (const char* key : {"name", "color", "version", "holder"})
            CHECK(peer.getProperty(key, juce::var("x")).isVoid());
        const auto self = frame.getProperty("self", {});
        CHECK(keysOf(self) == std::set<std::string>{"name"});
        CHECK(self.getProperty("name", juce::var("x")).isVoid());
        CHECK(frame.getProperty("verify", {}).toString() == "unknown");
    }
    SECTION("paired with named, coloured tracks")
    {
        InputProcessor& input = bench.addInput();
        setTrack(output, juce::String("Vox Lead"), juce::Colour(0xff2d79c6));
        setTrack(input, juce::String("Vox Lead"), juce::Colour(0xff112233));
        REQUIRE(runUntil(bench, [&] { return pairStateOf(output) == PairState::kConnected; }, 4000));
        const auto frame = pairFrame(bridge);
        CHECK(frame.getProperty("state", {}).toString() == "connected");
        CHECK(frame.getProperty("verify", {}).toString() == "match");
        const auto peer = frame.getProperty("peer", {});
        CHECK(keysOf(peer) == std::set<std::string>{"name", "color", "channels", "version", "holder", "sampleRate"});
        CHECK(peer.getProperty("name", {}).toString() == "Vox Lead");
        CHECK(peer.getProperty("color", {}).toString() == "#112233");
        CHECK(static_cast<int>(peer.getProperty("channels", {})) == 2);
        CHECK(static_cast<int>(peer.getProperty("sampleRate", {})) == 48000);
        CHECK(peer.getProperty("version", {}).toString() == juce::String(rinne::build::kVersion));
        CHECK(peer.getProperty("holder", juce::var("x")).isVoid());
        const auto self = frame.getProperty("self", {});
        CHECK(keysOf(self) == std::set<std::string>{"name", "color"});
        CHECK(self.getProperty("name", {}).toString() == "Vox Lead");
        CHECK(self.getProperty("color", {}).toString() == "#2d79c6");

        // 宿主下一次只给颜色、或给了空名字：没给的与空的都沿用上次的（AU 只给名字）
        setTrack(output, std::nullopt, juce::Colour(0xff445566));
        setTrack(output, juce::String());
        const auto again = pairFrame(bridge).getProperty("self", {});
        CHECK(again.getProperty("name", {}).toString() == "Vox Lead");
        CHECK(again.getProperty("color", {}).toString() == "#445566");
    }
}

// ---------------------------------------------------------------- 事件节拍

TEST_CASE("output bridge: events go out on change and once a second otherwise", "[host][output_bridge][events]")
{
    formalOnly();
    needsSharedMemory();
    Bench bench;
    OutputProcessor& output = bench.addOutput();
    Recorded rec;
    OutputBridge bridge(output, hooksFor(bench, rec));
    bench.run(1500); // 智能默认走完，单独一个 Output 停在空组里等
    OutputBridge::UiContext ui;

    // 音频照常走、每拍都问一次：数各发了几帧
    int states = 0;
    int pairs = 0;
    juce::var lastPair;
    auto polling = [&](std::uint64_t ms) {
        for (std::uint64_t t = 0; t < ms; t += 10)
        {
            bench.step();
            const auto f = bridge.poll(ui);
            states += f.state ? 1 : 0;
            if (!f.pair.isVoid())
            {
                ++pairs;
                lastPair = f.pair;
            }
        }
    };

    // 首帧之后什么都没变：一秒之内不再发，到一秒两个各补一帧，seq 接着数
    const auto first = bridge.firstFrames(ui);
    CHECK(static_cast<int>(first.getProperty("seq", {})) == 1);
    polling(990);
    CHECK(states == 0);
    CHECK(pairs == 0);
    polling(20);
    CHECK(states == 1);
    CHECK(pairs == 1);
    CHECK(static_cast<int>(lastPair.getProperty("seq", {})) == 2);

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

    // 界面那边换了语言或缩放也算变
    ui.lang = "fr";
    CHECK(bridge.poll(ui).state);
    CHECK_FALSE(bridge.poll(ui).state);
    ui.uiScale = 1.25f;
    CHECK(bridge.poll(ui).state);
    CHECK_FALSE(bridge.poll(ui).state);

    // 配对变了（Input 进了同一组）：rinne.pair 不等兜底就发
    InputProcessor& input = bench.addInput();
    REQUIRE(input.selectGroup(groupOf(output)));
    pairs = 0;
    polling(300);
    CHECK(pairs >= 1);
    CHECK(static_cast<int>(lastPair.getProperty("seq", {})) >= 3);
    CHECK(lastPair.getProperty("state", {}).toString() != "searching");
}

TEST_CASE("output bridge: each requestInitialState gets exactly one first rinne.pair", "[host][output_bridge][events]")
{
    formalOnly();
    Bench bench;
    OutputProcessor& output = bench.addOutput();
    Recorded rec;
    OutputBridge bridge(output, hooksFor(bench, rec));
    bench.run(200);
    const OutputBridge::UiContext ui;

    // 首帧钩子给的那一帧之后，同一拍里的 poll 不再补发任何一个状态类事件
    const auto first = bridge.firstFrames(ui);
    CHECK(static_cast<int>(first.getProperty("seq", {})) == 1);
    auto frames = bridge.poll(ui);
    CHECK_FALSE(frames.state);
    CHECK(frames.pair.isVoid());

    // 页面重载后再请求一次：再给一帧，seq 接着数，同样不叠发
    const auto again = bridge.firstFrames(ui);
    CHECK(static_cast<int>(again.getProperty("seq", {})) == 2);
    frames = bridge.poll(ui);
    CHECK_FALSE(frames.state);
    CHECK(frames.pair.isVoid());
}

TEST_CASE("output bridge: a state that cannot be encoded is reported once on rinne.error",
          "[host][output_bridge][events]")
{
    formalOnly();
    Bench bench;
    OutputProcessor& output = bench.addOutput();
    Recorded rec;
    OutputBridge bridge(output, hooksFor(bench, rec));
    const OutputBridge::UiContext ui;
    CHECK(bridge.poll(ui).errors.isEmpty());

    // 往未知键里塞一个这一端认识的键：这份文档编不出来，保存交出空块（见 OutputStateGlue::editConfigs）
    output.stateGlue()->editConfigs([](state::Configs& c) { c.unknown.push_back(state::ConfigEntry{4, {0}}); });
    CHECK(savedState(output).empty());
    auto frames = bridge.poll(ui);
    REQUIRE(frames.errors.size() == 1);
    CHECK(keysOf(frames.errors.getFirst()) == std::set<std::string>{"seq", "code"});
    CHECK(frames.errors.getFirst().getProperty("code", {}).toString() == "stateEncode");
    CHECK(bridge.poll(ui).errors.isEmpty());

    // 编辑器打开之前的失败不补报：新开的一份桥从现在的累计数起算，再失败一次才报
    OutputBridge reopened(output, hooksFor(bench, rec));
    CHECK(reopened.poll(ui).errors.isEmpty());
    CHECK(savedState(output).empty());
    CHECK(reopened.poll(ui).errors.size() == 1);
}

// ---------------------------------------------------------------- notices

TEST_CASE("output bridge: notices are built from the four facts, in a fixed order", "[host][output_bridge][notices]")
{
    CHECK(buildNotices(NoticeFacts{}).isEmpty());

    auto only = [](NoticeFacts facts) {
        const auto list = buildNotices(facts);
        REQUIRE(list.size() == 1);
        return list.getFirst();
    };
    NoticeFacts denied;
    denied.ipcDenied = true;
    CHECK(keysOf(only(denied)) == std::set<std::string>{"id"});
    CHECK(only(denied).getProperty("id", {}).toString() == "ipcSandbox");

    NoticeFacts newer;
    newer.heldNewerAbi = 2;
    const auto n = only(newer);
    CHECK(keysOf(n) == std::set<std::string>{"id", "a", "b"});
    CHECK(n.getProperty("id", {}).toString() == "newerProject");
    CHECK(n.getProperty("a", {}).toString() == "ABI 2");
    CHECK(n.getProperty("b", {}).toString() == "ABI " + juce::String(static_cast<int>(state::kAbi)));

    NoticeFacts corrupt;
    corrupt.decodeFailed = true;
    CHECK(only(corrupt).getProperty("id", {}).toString() == "stateDecodeFailed");

    NoticeFacts order;
    order.outputFirst = true;
    CHECK(only(order).getProperty("id", {}).toString() == "outOfOrder");

    NoticeFacts all{true, 3, true, true};
    const auto list = buildNotices(all);
    REQUIRE(list.size() == 4);
    std::vector<juce::String> ids;
    for (const auto& item : list)
        ids.push_back(item.getProperty("id", {}).toString());
    CHECK(ids == std::vector<juce::String>{"ipcSandbox", "newerProject", "stateDecodeFailed", "outOfOrder"});
}

TEST_CASE("output bridge: newerProject and stateDecodeFailed come from the state glue",
          "[host][output_bridge][notices]")
{
    formalOnly();
    Bench bench;
    OutputProcessor& output = bench.addOutput();
    Recorded rec;
    OutputBridge bridge(output, hooksFor(bench, rec));
    CHECK_FALSE(stateFields(bridge).hasProperty("notices"));

    // 头里的 abi 改成比本构建高的：整份拒载、原样保留
    auto newer = wiring_test::stateBytes(state::Role::Output, [](state::StateDocument& d) { d.configs.group = 2; });
    newer[4] = static_cast<std::uint8_t>(state::kAbi + 1u);
    restore(output, newer);
    REQUIRE(output.stateGlue()->holdingNewerState());
    CHECK(bridge.noticeFacts().heldNewerAbi == state::kAbi + 1u);
    const auto shown = noticeOf(stateFields(bridge), "newerProject");
    REQUIRE_FALSE(shown.isVoid());
    CHECK(shown.getProperty("a", {}).toString() == "ABI " + juce::String(static_cast<int>(state::kAbi + 1u)));
    CHECK(noticeOf(stateFields(bridge), "stateDecodeFailed").isVoid());

    // 坏字节：解不出来；更新版本的那份仍在保留，两条同时在
    const std::vector<std::uint8_t> garbage(32, 0x5a);
    restore(output, garbage);
    CHECK(bridge.noticeFacts().decodeFailed);
    CHECK_FALSE(noticeOf(stateFields(bridge), "stateDecodeFailed").isVoid());
    CHECK_FALSE(noticeOf(stateFields(bridge), "newerProject").isVoid());

    // 一份本构建能读的 state 取代了它们
    restore(output, groupState(state::Role::Output, 3));
    CHECK_FALSE(output.stateGlue()->holdingNewerState());
    CHECK(bridge.noticeFacts().heldNewerAbi == 0u);
    CHECK_FALSE(stateFields(bridge).hasProperty("notices"));
}

TEST_CASE("output bridge: ipcSandbox only when the backend says the system refused", "[host][output_bridge][notices]")
{
    formalOnly();
    needsSharedMemory();
    RefusingBackend denied(ipc::InitResult::kFailedDenied);
    RefusingBackend badName(ipc::InitResult::kFailedBadName);
    RefusingBackend transient(ipc::InitResult::kFailed);
    Bench bench;
    OutputProcessor& sandboxed = addOutputWith(bench, denied);
    OutputProcessor& misnamed = addOutputWith(bench, badName);
    OutputProcessor& flaky = addOutputWith(bench, transient);
    for (OutputProcessor* p : {&sandboxed, &misnamed, &flaky})
        REQUIRE(p->selectGroup(1));
    bench.run(400);
    REQUIRE(stateOf(sandboxed) == ipc::RendezvousState::kIpcUnavailable);
    REQUIRE(stateOf(misnamed) == ipc::RendezvousState::kIpcUnavailable);
    REQUIRE(stateOf(flaky) != ipc::RendezvousState::kIpcUnavailable);

    Recorded rec;
    OutputBridge sandboxedBridge(sandboxed, hooksFor(bench, rec));
    OutputBridge misnamedBridge(misnamed, hooksFor(bench, rec));
    OutputBridge flakyBridge(flaky, hooksFor(bench, rec));
    CHECK(sandboxedBridge.noticeFacts().ipcDenied);
    CHECK_FALSE(noticeOf(stateFields(sandboxedBridge), "ipcSandbox").isVoid());
    CHECK_FALSE(misnamedBridge.noticeFacts().ipcDenied);
    CHECK_FALSE(stateFields(misnamedBridge).hasProperty("notices"));
    CHECK_FALSE(flakyBridge.noticeFacts().ipcDenied);
    CHECK_FALSE(stateFields(flakyBridge).hasProperty("notices"));
}

TEST_CASE("output bridge: outOfOrder follows the order evidence of the Output engine", "[host][output_bridge][notices]")
{
    formalOnly();
    needsSharedMemory();
    Bench bench;
    // 先建 Output：Bench 每个周期按建立顺序处理，Output 每拍都在 Input 前面
    OutputProcessor& output = bench.addOutput();
    InputProcessor& input = bench.addInput();
    REQUIRE(output.selectGroup(1));
    REQUIRE(input.selectGroup(1));
    Recorded rec;
    OutputBridge bridge(output, hooksFor(bench, rec));
    // 宿主给的时间戳每个周期前进一块，两端在同一周期里拿到同一个值
    auto cycle = [&](std::uint64_t ms) {
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
