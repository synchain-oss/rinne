// SPDX-License-Identifier: GPL-3.0-or-later
//
// 编辑器工厂：正式身份走网页编辑器的构造缝，探针身份只走注入的探针工厂，
// 不认识的角色、探针身份却没有注入、构建里没有浏览器组件，都返回空指针（插件壳回退到通用编辑器）。
// 网页编辑器与探针编辑器都用替身，不碰真实的浏览器组件。

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

#include <juce_audio_processors/juce_audio_processors.h>

#include "RinneEditorFactory.h"
#include "util/RinneVersion.h"

namespace
{

namespace editor = rinne::editor;

class NullProcessor final : public juce::AudioProcessor
{
public:
    NullProcessor()
        : juce::AudioProcessor(BusesProperties()
                                   .withInput("In", juce::AudioChannelSet::stereo())
                                   .withOutput("Out", juce::AudioChannelSet::stereo()))
    {
    }

    const juce::String getName() const override { return "Null"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return true; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}
};

// 带标记的替身编辑器，用来认出是哪条路径造出来的
class StubEditor final : public juce::AudioProcessorEditor
{
public:
    StubEditor(juce::AudioProcessor& p, const char* tagText) : juce::AudioProcessorEditor(p), tag(tagText) {}
    const char* const tag;
};

struct Calls
{
    int web = 0;
    int probe = 0;
    juce::AudioProcessor* webProcessor = nullptr;
};

editor::Options makeOptions(const char* role, Calls& calls, juce::AudioProcessor& owner)
{
    editor::Options o;
    o.role = role;
    o.webViewEditorMaker = [&calls](juce::AudioProcessor& p) -> juce::AudioProcessorEditor* {
        ++calls.web;
        calls.webProcessor = &p;
        return new StubEditor(p, "web");
    };
    // 探针替身也要挂在某个处理器上；真实的插件壳里它由处理器基类造，这里借测试的处理器
    o.probeEditorFactory = [&calls, &owner]() -> juce::AudioProcessorEditor* {
        ++calls.probe;
        return new StubEditor(owner, "probe");
    };
    return o;
}

const char* tagOf(juce::AudioProcessorEditor* e)
{
    auto* s = dynamic_cast<StubEditor*>(e);
    return s != nullptr ? s->tag : nullptr;
}

} // namespace

TEST_CASE("editor factory: the build identity follows the probe flag", "[webview][editor]")
{
    CHECK(editor::currentIdentity() ==
          (rinne::build::kProbeBuild ? editor::Identity::probe : editor::Identity::formal));
}

TEST_CASE("editor factory: formal identity builds the web editor through the seam", "[webview][editor]")
{
    NullProcessor processor;
    Calls calls;
    for (const char* role : {"input", "output"})
    {
        const auto o = makeOptions(role, calls, processor);
        std::unique_ptr<juce::AudioProcessorEditor> e(editor::createEditor(processor, o, editor::Identity::formal));
        REQUIRE(e != nullptr);
        CHECK(std::string(tagOf(e.get())) == "web");
    }
    CHECK(calls.web == 2);
    CHECK(calls.probe == 0); // 正式身份绝不碰探针工厂
    CHECK(calls.webProcessor == &processor);
}

TEST_CASE("editor factory: probe identity builds only the injected probe editor", "[webview][editor]")
{
    NullProcessor processor;
    Calls calls;
    const auto o = makeOptions("input", calls, processor);
    std::unique_ptr<juce::AudioProcessorEditor> e(editor::createEditor(processor, o, editor::Identity::probe));
    REQUIRE(e != nullptr);
    CHECK(std::string(tagOf(e.get())) == "probe");
    CHECK(calls.probe == 1);
    CHECK(calls.web == 0); // 探针构建里不加载网页
}

TEST_CASE("editor factory: probe identity without an injected factory builds nothing", "[webview][editor]")
{
    NullProcessor processor;
    Calls calls;
    auto o = makeOptions("output", calls, processor);
    o.probeEditorFactory = nullptr;
    CHECK(editor::createEditor(processor, o, editor::Identity::probe) == nullptr);
    CHECK(calls.web == 0); // 也不会退回去造网页编辑器
}

TEST_CASE("editor factory: an unknown role builds nothing under either identity", "[webview][editor]")
{
    NullProcessor processor;
    Calls calls;
    for (const char* role : {"", "Input", "OUTPUT", "send", "return", "both"})
    {
        const auto o = makeOptions(role, calls, processor);
        CHECK(editor::createEditor(processor, o, editor::Identity::formal) == nullptr);
        CHECK(editor::createEditor(processor, o, editor::Identity::probe) == nullptr);
    }
    CHECK(calls.web == 0);
    CHECK(calls.probe == 0);
}

TEST_CASE("editor factory: the default identity is the one of this build", "[webview][editor]")
{
    NullProcessor processor;
    Calls calls;
    const auto o = makeOptions("input", calls, processor);
    std::unique_ptr<juce::AudioProcessorEditor> e(editor::createEditor(processor, o));
    REQUIRE(e != nullptr);
    CHECK(std::string(tagOf(e.get())) == (rinne::build::kProbeBuild ? "probe" : "web"));
}

TEST_CASE("editor factory: no seam and no browser component builds nothing, it does not crash", "[webview][editor]")
{
#if JUCE_WEB_BROWSER
    SUCCEED("this build has the real web editor; its construction needs a browser and is not exercised here");
#else
    NullProcessor processor;
    editor::Options o;
    o.role = "input";
    CHECK(editor::createEditor(processor, o, editor::Identity::formal) == nullptr);
#endif
}
