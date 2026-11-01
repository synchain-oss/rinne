// SPDX-License-Identifier: GPL-3.0-or-later
//
// InputEditor 与切面板、首帧有关的几条接线。WebViewHost 要浏览器组件，宿主测试目标编不进编辑器，所以分两半钉：
//   - 源码（本文件）：panic 的守卫分支要先切断，别的函数的守卫分支一概什么都不做；
//     emitFirstFrames 把桥给的其余首帧发齐；切到兜底面板时收掉还开着的手势；
//     createEditor 经工厂造 InputEditor，把兜底面板的按钮带进去；
//   - 行为（test_input_bridge.cpp）：处理器的 panic 连点不恢复、恢复要等冷却；firstFrames、endAllGestures 给出的东西。
// 扫描器自己带正反例，保证它不是空转。源文件路径由本文件的 __FILE__ 推出，取法同
// output_bridge/test_first_frame_hook.cpp；找不到文件就判红，不静默跳过。

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <vector>

#include <juce_core/juce_core.h>

namespace
{

juce::File repoRoot()
{
    return juce::File::getCurrentWorkingDirectory()
        .getChildFile(__FILE__)
        .getParentDirectory()
        .getParentDirectory()
        .getParentDirectory()
        .getParentDirectory();
}

std::string readSource(const char* relative)
{
    const juce::File file = repoRoot().getChildFile(relative);
    REQUIRE(file.existsAsFile());
    return file.loadFileAsString().toStdString();
}

// 去掉行注释（注释里提到不算数）。这几处源码里没有含 // 的字符串字面量
std::string stripLineComments(const std::string& src)
{
    std::string out;
    out.reserve(src.size());
    for (std::size_t i = 0; i < src.size(); ++i)
    {
        if (src.compare(i, 2, "//") == 0)
        {
            while (i < src.size() && src[i] != '\n')
                ++i;
            if (i < src.size())
                out += '\n';
        }
        else
        {
            out += src[i];
        }
    }
    return out;
}

// 从 at 处起第一个花括号到与它配对的右花括号（含两端）；找不到返回空串
std::string blockAt(const std::string& text, std::size_t at)
{
    const std::size_t open = text.find('{', at);
    if (open == std::string::npos)
        return {};
    int depth = 0;
    for (std::size_t i = open; i < text.size(); ++i)
    {
        if (text[i] == '{')
            ++depth;
        else if (text[i] == '}' && --depth == 0)
            return text.substr(open, i - open + 1);
    }
    return {};
}

std::string functionBody(const std::string& text, const std::string& signature)
{
    const std::size_t at = text.find(signature);
    return at == std::string::npos ? std::string() : blockAt(text, at + signature.size());
}

// 函数体开头那个 if (panelShown()) 分支；没有返回空串
std::string panelGuard(const std::string& body)
{
    const std::size_t at = body.find("if (panelShown())");
    return at == std::string::npos ? std::string() : blockAt(body, at);
}

std::size_t countOf(const std::string& text, const std::string& needle)
{
    std::size_t n = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size()))
        ++n;
    return n;
}

// panic 的守卫：恰好切断一次，切断在回执之前；除此之外不碰处理器，也不经桥
bool guardCutsFirst(const std::string& guard)
{
    if (countOf(guard, "processor_.") != 1 || guard.find("bridge_.") != std::string::npos)
        return false;
    const std::size_t cut = guard.find("processor_.panic();");
    const std::size_t receipt = guard.find("complete(bridge::respondNotReady());");
    return cut != std::string::npos && receipt != std::string::npos && cut < receipt;
}

// 其余函数的守卫：只留日志与回执，处理器与桥一概不碰
bool guardDoesNothing(const std::string& guard)
{
    return !guard.empty() && guard.find("complete(bridge::respondNotReady());") != std::string::npos &&
           guard.find("processor_") == std::string::npos && guard.find("bridge_") == std::string::npos;
}

// 所有 void InputEditor::handleXxx( 的名字
std::vector<std::string> handlerNames(const std::string& source)
{
    const std::string prefix = "void InputEditor::handle";
    std::vector<std::string> names;
    for (std::size_t at = source.find(prefix); at != std::string::npos; at = source.find(prefix, at + prefix.size()))
    {
        const std::size_t end = source.find('(', at);
        names.push_back(source.substr(at, end - at + 1));
    }
    return names;
}

} // namespace

TEST_CASE("input editor source: the scanner tells a good guard from the bad ones", "[host][input_bridge][hygiene]")
{
    const std::string receipt = "complete(bridge::respondNotReady()); return;";
    CHECK(guardCutsFirst("{ processor_.panic(); logDiag(\"x\"); " + receipt + " }"));
    // 不切断、回执之后才切、切了别的、还经了桥
    CHECK_FALSE(guardCutsFirst("{ logDiag(\"x\"); " + receipt + " }"));
    CHECK_FALSE(guardCutsFirst("{ complete(bridge::respondNotReady()); processor_.panic(); return; }"));
    CHECK_FALSE(guardCutsFirst("{ processor_.panic(); processor_.resumeLoop(); " + receipt + " }"));
    CHECK_FALSE(guardCutsFirst("{ processor_.panic(); bridge_.panic(); " + receipt + " }"));

    CHECK(guardDoesNothing("{ logDiag(\"x\"); " + receipt + " }"));
    CHECK_FALSE(guardDoesNothing(""));
    CHECK_FALSE(guardDoesNothing("{ processor_.resumeLoop(); " + receipt + " }"));
    CHECK_FALSE(guardDoesNothing("{ bridge_.resumeLoop(); " + receipt + " }"));

    const std::string source = "void A::f() { if (panelShown()) { g(); } h(); }\n// void A::x() {}\n";
    CHECK(functionBody(source, "void A::f(") == "{ if (panelShown()) { g(); } h(); }");
    CHECK(panelGuard(functionBody(source, "void A::f(")) == "{ g(); }");
    CHECK(functionBody(stripLineComments(source), "void A::x(").empty());
}

TEST_CASE("input editor source: while the fallback panel is shown only panic still acts",
          "[host][input_bridge][hygiene]")
{
    const std::string source = stripLineComments(readSource("src/input/InputEditor.cpp"));
    const auto names = handlerNames(source);
    REQUIRE(names.size() == 24);
    for (const auto& name : names)
    {
        INFO(name);
        const std::string guard = panelGuard(functionBody(source, name));
        REQUIRE_FALSE(guard.empty());
        if (name == "void InputEditor::handlePanic(")
            CHECK(guardCutsFirst(guard));
        else
            CHECK(guardDoesNothing(guard));
    }
}

TEST_CASE("input editor source: the first frames are sent in full from the hook", "[host][input_bridge][hygiene]")
{
    const std::string source = stripLineComments(readSource("src/input/InputEditor.cpp"));
    CHECK(functionBody(source, "void InputEditor::emitFirstFrames(")
              .find("sendFrames(bridge_.firstFrames(uiContext()));") != std::string::npos);
    CHECK(functionBody(source, "void InputEditor::emitTick(").find("sendFrames(bridge_.tick(uiContext()));") !=
          std::string::npos);
    // 发帧把每种事件都发出去
    const std::string send = functionBody(source, "void InputEditor::sendFrames(");
    for (const char* event : {"api::kEvPair", "api::kEvParams", "api::kEvMeters", "api::kEvPresets", "api::kEvError"})
    {
        INFO(event);
        CHECK(send.find(event) != std::string::npos);
    }
    CHECK(send.find("emitState();") != std::string::npos);
}

TEST_CASE("input editor source: switching to the fallback panel closes open gestures", "[host][input_bridge][hygiene]")
{
    const std::string editor = stripLineComments(readSource("src/input/InputEditor.cpp"));
    CHECK(functionBody(editor, "void InputEditor::onFallbackPanelShown(").find("bridge_.endAllGestures();") !=
          std::string::npos);

    const std::string host = stripLineComments(readSource("src/plugin-common/WebViewHost.cpp"));
    CHECK(countOf(functionBody(host, "void WebViewHost::showFallback("), "onFallbackPanelShown();") == 1);
    CHECK(countOf(host, "onFallbackPanelShown();") == 1);
    CHECK(countOf(stripLineComments(readSource("src/plugin-common/WebViewHost.h")),
                  "virtual void onFallbackPanelShown() {}") == 1);
}

TEST_CASE("input editor source: createEditor builds the InputEditor with the panel controls",
          "[host][input_bridge][hygiene]")
{
    const std::string entry = stripLineComments(readSource("src/input/InputPluginEntry.cpp"));
    const std::string body = functionBody(entry, "InputProcessor::createEditor(");
    REQUIRE_FALSE(body.empty());
    const std::size_t controls = body.find("options.controls");
    const std::size_t maker = body.find("options.webViewEditorMaker");
    REQUIRE(controls != std::string::npos);
    REQUIRE(maker != std::string::npos);
    // 按钮先填好，造编辑器的 lambda 才捕获得到
    CHECK(controls < maker);
    CHECK(body.find("new input::InputEditor(*this, resources, controls)") != std::string::npos);

    const std::string editor = stripLineComments(readSource("src/input/InputEditor.cpp"));
    CHECK(editor.find("config.controls = controls;") != std::string::npos);
}
