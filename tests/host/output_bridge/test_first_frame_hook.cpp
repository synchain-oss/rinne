// SPDX-License-Identifier: GPL-3.0-or-later
//
// 首帧钩子 emitFirstFrames：桥契约时序要求收到 requestInitialState 立刻把本角色所有状态类事件各发一帧，
// Output 的首帧 rinne.pair 经这个钩子发。WebViewHost 要浏览器组件，宿主测试目标编不进它，所以分两半钉：
//   - 源码：WebViewHost.cpp 的 handleRequestInitialState 在发完首帧 rinne.state（emitState）之后调用钩子恰好一次，
//     面板在场的那一支返回之前不调用；全文件只有这一个调用点；基类的默认实现是空的；OutputEditor 的覆写只发一帧；
//   - 行为：OutputBridge::firstFrames 给出的就是那一帧，并把两个事件的基线对齐到它，紧接着的一拍不会重发
//     （见 test_output_bridge.cpp 的事件节拍用例），页面重载后再来一次 seq 接着数。
// 扫描器自己带正反例，保证它不是空转。源文件路径由本文件的 __FILE__ 推出，
// 取法同 tests/host/wiring/test_wiring_source_order.cpp；找不到文件就判红，不静默跳过。

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>

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

// 去掉行注释与块注释（注释里提到钩子不算数）。这几处源码里没有含 // 或 /* 的字符串字面量
std::string stripComments(const std::string& src)
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
        else if (src.compare(i, 2, "/*") == 0)
        {
            const std::size_t end = src.find("*/", i + 2);
            i = end == std::string::npos ? src.size() : end + 1;
        }
        else
        {
            out += src[i];
        }
    }
    return out;
}

// 签名之后第一个花括号起、到与它配对的右花括号为止的函数体（含两端的花括号）；找不到返回空串
std::string functionBody(const std::string& text, const std::string& signature)
{
    const std::size_t at = text.find(signature);
    if (at == std::string::npos)
        return {};
    const std::size_t open = text.find('{', at + signature.size());
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

std::size_t countOf(const std::string& text, const std::string& needle)
{
    std::size_t n = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size()))
        ++n;
    return n;
}

constexpr const char* kCall = "emitFirstFrames();";

// requestInitialState 的处理函数体：钩子恰好调用一次，在首帧 rinne.state 之后，在面板那一支的 return 之后
bool callsHookOnceAfterFirstState(const std::string& body)
{
    if (countOf(body, kCall) != 1)
        return false;
    const std::size_t hook = body.find(kCall);
    const std::size_t state = body.find("emitState();");
    const std::size_t panelReturn = body.find("return;");
    return state != std::string::npos && state < hook && panelReturn != std::string::npos && panelReturn < hook;
}

} // namespace

TEST_CASE("first-frame hook: the scanner tells a good handler from the bad ones", "[host][output_bridge][hygiene]")
{
    const std::string guard = "if (fallback_ != nullptr) { log(); complete(ok()); return; }\n";
    const std::string good = "{ " + guard + "ready = true; complete(ok()); emitState(); emitFirstFrames(); }";
    CHECK(callsHookOnceAfterFirstState(good));
    // 没调用、调了两次、在首帧 state 之前调、在面板那一支里调
    CHECK_FALSE(callsHookOnceAfterFirstState("{ " + guard + "complete(ok()); emitState(); }"));
    CHECK_FALSE(callsHookOnceAfterFirstState("{ " + guard + "emitState(); emitFirstFrames(); emitFirstFrames(); }"));
    CHECK_FALSE(callsHookOnceAfterFirstState("{ " + guard + "emitFirstFrames(); emitState(); }"));
    CHECK_FALSE(
        callsHookOnceAfterFirstState("{ if (fallback_ != nullptr) { emitFirstFrames(); return; }\n emitState(); }"));
    // 函数体按花括号配对取，注释里的调用不算
    const std::string source = "void A::f() { x(); }\nvoid A::g(int a)\n{ if (a) { y(); } // emitFirstFrames();\n}\n";
    CHECK(functionBody(source, "void A::g(") == "{ if (a) { y(); } // emitFirstFrames();\n}");
    CHECK(countOf(stripComments(functionBody(source, "void A::g(")), kCall) == 0);
    CHECK(functionBody(source, "void A::missing(").empty());
}

TEST_CASE("first-frame hook: requestInitialState calls it exactly once, right after the first rinne.state",
          "[host][output_bridge][hygiene]")
{
    const std::string host = stripComments(readSource("src/plugin-common/WebViewHost.cpp"));
    const std::string body = functionBody(host, "void WebViewHost::handleRequestInitialState(");
    REQUIRE_FALSE(body.empty());
    CHECK(callsHookOnceAfterFirstState(body));
    // 全文件只有这一个调用点：定时器、重试、语言与缩放都不发首帧
    CHECK(countOf(host, kCall) == 1);

    // 默认实现是空操作：只给子类留位置，基类自己的首帧顺序不变
    const std::string header = stripComments(readSource("src/plugin-common/WebViewHost.h"));
    CHECK(countOf(header, "virtual void emitFirstFrames() {}") == 1);

    // Output 的覆写只补一帧 rinne.pair，取自 OutputBridge::firstFrames（它同时对齐两个事件的基线）
    const std::string editor = stripComments(readSource("src/output/OutputEditor.cpp"));
    const std::string hook = functionBody(editor, "void OutputEditor::emitFirstFrames()");
    REQUIRE_FALSE(hook.empty());
    CHECK(countOf(hook, "emitEvent(") == 1);
    CHECK(countOf(hook, "bridge_.firstFrames(") == 1);
    CHECK(countOf(hook, "emitState(") == 0);
}
