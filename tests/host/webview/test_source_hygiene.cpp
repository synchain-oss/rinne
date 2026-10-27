// SPDX-License-Identifier: GPL-3.0-or-later
//
// 网页宿主这一族源文件的卫生检查，三条：
//   1. 运行期字符串字面量只许 ASCII；
//   2. 只给单测用的两个存储钩子（目录重定向、进程间锁替身），发布代码（src/ 下除声明它们的两个文件以外）不得调用；
//   3. 宿主类（不进宿主测试目标，编译不到这里）里两处靠调用点守的东西：导航闸必须走纯函数判定并在不放行时返回假，
//      状态快照必须用一次读回，不能退回逐个读设置文件。
//
// 第一条：网页宿主这一族源文件里，运行期字符串字面量只许 ASCII。
// 原因：源文件按 UTF-8 存，字面量里一旦有中文，在中文代码页的机器上编译器会报字符集警告，
// 而第一方代码要求零警告。中文只能写在注释里。
// 这里用一个小的扫描器逐个源文件找「字符串或字符字面量里出现非 ASCII 字节」，
// 扫描器自己带正反例，保证它不是空转。源文件路径由本文件的 __FILE__ 推出，找不到文件就判红，不静默跳过。

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <vector>

#include <juce_core/juce_core.h>

namespace
{

bool isHexDigit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// 返回出现在字符串或字符字面量里的非 ASCII 字节所在的行号（从 1 起，每行最多记一次）。
// 注释（行注释、块注释）里的非 ASCII 不算。认识：转义、原始字符串、数字分隔符
std::vector<int> nonAsciiLiteralLines(const std::string& src)
{
    enum class State
    {
        code,
        lineComment,
        blockComment,
        stringLiteral,
        charLiteral,
        rawString
    };

    std::vector<int> hits;
    State state = State::code;
    int line = 1;
    std::string rawEnd; // 原始字符串的结束标记：)delim"
    int lastHitLine = 0;

    for (std::size_t i = 0; i < src.size(); ++i)
    {
        const char c = src[i];
        const char next = i + 1 < src.size() ? src[i + 1] : '\0';
        const bool nonAscii = (static_cast<unsigned char>(c) & 0x80u) != 0;

        switch (state)
        {
        case State::code:
            if (c == '/' && next == '/')
            {
                state = State::lineComment;
                ++i;
            }
            else if (c == '/' && next == '*')
            {
                state = State::blockComment;
                ++i;
            }
            else if (c == 'R' && next == '"')
            {
                // 原始字符串：R"delim( ... )delim"
                std::size_t j = i + 2;
                std::string delim;
                while (j < src.size() && src[j] != '(')
                    delim.push_back(src[j++]);
                rawEnd = ")" + delim + "\"";
                state = State::rawString;
                i = j; // 停在 '('
            }
            else if (c == '"')
            {
                state = State::stringLiteral;
            }
            else if (c == '\'')
            {
                const bool separator = i > 0 && isHexDigit(src[i - 1]) && isHexDigit(next);
                if (!separator)
                    state = State::charLiteral;
            }
            break;

        case State::lineComment:
            if (c == '\n')
                state = State::code;
            break;

        case State::blockComment:
            if (c == '*' && next == '/')
            {
                state = State::code;
                ++i;
            }
            break;

        case State::stringLiteral:
        case State::charLiteral:
            if (c == '\\')
            {
                if (next != '\0' && (static_cast<unsigned char>(next) & 0x80u) != 0 && line != lastHitLine)
                {
                    hits.push_back(line);
                    lastHitLine = line;
                }
                if (next == '\n')
                    ++line;
                ++i; // 吞掉被转义的那个字符
            }
            else if ((state == State::stringLiteral && c == '"') || (state == State::charLiteral && c == '\''))
            {
                state = State::code;
            }
            else if (nonAscii && line != lastHitLine)
            {
                hits.push_back(line);
                lastHitLine = line;
            }
            break;

        case State::rawString:
            if (src.compare(i, rawEnd.size(), rawEnd) == 0)
            {
                state = State::code;
                i += rawEnd.size() - 1;
            }
            else if (nonAscii && line != lastHitLine)
            {
                hits.push_back(line);
                lastHitLine = line;
            }
            break;
        }

        if (c == '\n')
            ++line;
    }
    return hits;
}

// 仓库根：本文件在 tests/host/webview/ 下
juce::File repoRoot()
{
    return juce::File::getCurrentWorkingDirectory()
        .getChildFile(__FILE__)
        .getParentDirectory()
        .getParentDirectory()
        .getParentDirectory()
        .getParentDirectory();
}

} // namespace

TEST_CASE("ascii scanner: a non-ASCII byte inside a string literal is flagged", "[webview][ascii]")
{
    const std::string cjk = "\xe4\xb8\xad"; // 一个汉字的 UTF-8 字节
    CHECK(nonAsciiLiteralLines("const char* s = \"abc" + cjk + "\";\n") == std::vector<int>{1});
    CHECK(nonAsciiLiteralLines("int a;\nint b;\nconst char* s = \"" + cjk + "\";\n") == std::vector<int>{3});
    CHECK(nonAsciiLiteralLines("char c = '" + cjk + "';\n") == std::vector<int>{1});
    CHECK(nonAsciiLiteralLines("auto s = R\"x(" + cjk + ")x\";\n") == std::vector<int>{1});
    CHECK(nonAsciiLiteralLines("auto s = \"a\\" + cjk + "\";\n") == std::vector<int>{1}); // 转义后紧跟非 ASCII
}

TEST_CASE("ascii scanner: non-ASCII in comments and plain ASCII literals is fine", "[webview][ascii]")
{
    const std::string cjk = "\xe4\xb8\xad";
    CHECK(nonAsciiLiteralLines("// " + cjk + "\nint a;\n").empty());
    CHECK(nonAsciiLiteralLines("/* " + cjk + " */ int a;\n").empty());
    CHECK(nonAsciiLiteralLines("/* \"" + cjk + " */ int a;\n").empty()); // 块注释里的引号不开字面量
    CHECK(nonAsciiLiteralLines("const char* s = \"ascii only\"; // " + cjk + "\n").empty());
    CHECK(nonAsciiLiteralLines("// it's " + cjk + "\nint a;\n").empty()); // 注释里的单引号不开字符字面量
    CHECK(nonAsciiLiteralLines("const char* s = \"say \\\"hi\\\"\";\n").empty());
    CHECK(nonAsciiLiteralLines("int big = 0xff'ff; // " + cjk + "\n").empty()); // 数字分隔符不是字符字面量
    CHECK(nonAsciiLiteralLines("int a = 1; // \"\n const char* s = \"x\";\n").empty());
}

TEST_CASE("ascii scanner: a literal after a comment is still scanned", "[webview][ascii]")
{
    const std::string cjk = "\xe4\xb8\xad";
    CHECK(nonAsciiLiteralLines("// " + cjk + "\nconst char* s = \"" + cjk + "\";\n") == std::vector<int>{2});
    CHECK(nonAsciiLiteralLines("/* a\nb */ const char* s = \"" + cjk + "\";\n") == std::vector<int>{2});
}

TEST_CASE("web host sources carry no non-ASCII string literals", "[webview][ascii]")
{
    const juce::File dir = repoRoot().getChildFile("src").getChildFile("plugin-common");
    REQUIRE(dir.isDirectory()); // 路径推不出来就是红，不静默跳过

    const char* const names[] = {
        "BridgeBase.h",
        "BridgeBase.cpp",
        "FallbackPanel.h",
        "FallbackPanel.cpp",
        "PlatformWebView.h",
        "PlatformWebView.cpp",
        "PlatformWebViewRuntime.cpp",
        "ResourceProvider.h",
        "ResourceProvider.cpp",
        "RinneEditorFactory.h",
        "RinneEditorFactory.cpp",
        "UiDefaultsStore.h",
        "UiDefaultsStore.cpp",
        "WebViewHost.h",
        "WebViewHost.cpp",
        "WebViewRevealGate.h",
    };

    for (const char* name : names)
    {
        const juce::File file = dir.getChildFile(name);
        INFO(name);
        REQUIRE(file.existsAsFile());
        juce::MemoryBlock bytes;
        REQUIRE(file.loadFileAsData(bytes));
        const std::string text(static_cast<const char*>(bytes.getData()), bytes.getSize());
        const auto lines = nonAsciiLiteralLines(text);
        for (int l : lines)
            FAIL_CHECK(std::string(name) << " line " << l << " has a non-ASCII string literal");
        CHECK(lines.empty());
    }
}

TEST_CASE("the test-only storage hooks are not referenced by release code", "[webview][hygiene]")
{
    const juce::File src = repoRoot().getChildFile("src");
    REQUIRE(src.isDirectory()); // 路径推不出来就是红，不静默跳过

    juce::Array<juce::File> files;
    src.findChildFiles(files, juce::File::findFiles, true, "*.h;*.hpp;*.cpp;*.mm");
    REQUIRE(files.size() > 20); // 扫得到足够多的文件才算数

    for (const char* hook : {"setStorageDirForTests", "setLockForTests"})
    {
        INFO(hook);
        int scanned = 0;
        for (const auto& f : files)
        {
            const auto name = f.getFileName();
            if (name == "UiDefaultsStore.h" || name == "UiDefaultsStore.cpp")
                continue;
            ++scanned;
            INFO(f.getFullPathName().toStdString());
            CHECK_FALSE(f.loadFileAsString().contains(hook));
        }
        CHECK(scanned > 20);

        // 声明方确实带着它，否则上面的扫描守的是一个不存在的名字
        CHECK(src.getChildFile("plugin-common").getChildFile("UiDefaultsStore.h").loadFileAsString().contains(hook));
    }
}

namespace
{

// 取 text 里第一个以 signature 开头的函数体（含花括号）；找不到或花括号不配对给空串。
// 只数花括号，字符串和注释里的花括号不处理：被检的两个函数体里没有
std::string functionBody(const std::string& text, const std::string& signature)
{
    const auto at = text.find(signature);
    if (at == std::string::npos)
        return {};
    const auto open = text.find('{', at);
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

bool contains(const std::string& text, const std::string& needle)
{
    return text.find(needle) != std::string::npos;
}

// 导航闸的调用点要求：先问纯函数，不放行就返回假，且返回假排在「导航开始」记账之前
bool navigationGateWellFormed(const std::string& body)
{
    const auto ask = body.find("ResourceProvider::isRootNavigation(");
    const auto refuse = body.find("return false;");
    const auto account = body.find("onNavigationStarted(");
    return ask != std::string::npos && refuse != std::string::npos && account != std::string::npos && ask < refuse &&
           refuse < account;
}

// 快照要用一次读回，不能逐个读：这些名字都是设置文件的单项读取
bool snapshotReadsFlagsInOneCall(const std::string& body)
{
    if (!contains(body, "uidefaults::readUiFlags("))
        return false;
    for (const char* single : {"langChosenGlobal(", "rulesAckVersion(", "guideSeenGlobal(", "diagLogEnabled(",
                               "langGlobal(", "uiScalePercent"})
        if (contains(body, single))
            return false;
    return true;
}

} // namespace

TEST_CASE("call-site scanners: good and bad samples", "[webview][hygiene]")
{
    const std::string goodGate = "bool pageAboutToLoad(const juce::String& url) override\n{\n"
                                 "    if (!ResourceProvider::isRootNavigation(url, root))\n    {\n"
                                 "        log();\n        return false;\n    }\n"
                                 "    owner_.onNavigationStarted(url);\n    return true;\n}\n";
    CHECK(navigationGateWellFormed(functionBody(goodGate, "bool pageAboutToLoad(")));
    // 不问纯函数
    CHECK_FALSE(navigationGateWellFormed(functionBody("bool pageAboutToLoad(const juce::String& url) override\n{\n"
                                                      " owner_.onNavigationStarted(url);\n return true;\n}\n",
                                                      "bool pageAboutToLoad(")));
    // 问了但不拦
    CHECK_FALSE(navigationGateWellFormed(functionBody(
        "bool pageAboutToLoad(const juce::String& url) override\n{\n"
        " if (!ResourceProvider::isRootNavigation(url, root)) { log(); }\n owner_.onNavigationStarted(url);\n"
        " return true;\n}\n",
        "bool pageAboutToLoad(")));
    // 先记账后拦
    CHECK_FALSE(navigationGateWellFormed(
        functionBody("bool pageAboutToLoad(const juce::String& url) override\n{\n owner_.onNavigationStarted(url);\n"
                     " if (!ResourceProvider::isRootNavigation(url, root)) { return false; }\n return true;\n}\n",
                     "bool pageAboutToLoad(")));
    CHECK(functionBody("int f() { if (a) { b(); } return 0; } int g() { return 1; }", "int f(") ==
          "{ if (a) { b(); } return 0; }");
    CHECK(functionBody("int f() { return 0;", "int f(").empty());
    CHECK(functionBody("int g() { return 0; }", "int f(").empty());

    CHECK(snapshotReadsFlagsInOneCall("{ return build(seed, uidefaults::readUiFlags(role)); }"));
    CHECK_FALSE(
        snapshotReadsFlagsInOneCall("{ UiFlags f; f.diagLog = uidefaults::diagLogEnabled(); return build(f); }"));
    CHECK_FALSE(snapshotReadsFlagsInOneCall("{ auto f = uidefaults::readUiFlags(role); "
                                            "f.rulesAck = uidefaults::rulesAckVersion(); return build(f); }"));
    CHECK_FALSE(snapshotReadsFlagsInOneCall("{ return build(seed, {}); }"));
}

TEST_CASE("the web host gates navigation through the pure function and reads stored flags in one call",
          "[webview][hygiene]")
{
    const juce::File file =
        repoRoot().getChildFile("src").getChildFile("plugin-common").getChildFile("WebViewHost.cpp");
    REQUIRE(file.existsAsFile()); // 路径推不出来就是红，不静默跳过
    const std::string text = file.loadFileAsString().toStdString();

    const auto gate = functionBody(text, "bool pageAboutToLoad(");
    REQUIRE_FALSE(gate.empty());
    CHECK(navigationGateWellFormed(gate));

    const auto snapshot = functionBody(text, "juce::var WebViewHost::buildSnapshot()");
    REQUIRE_FALSE(snapshot.empty());
    CHECK(snapshotReadsFlagsInOneCall(snapshot));
}
