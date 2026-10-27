// SPDX-License-Identifier: GPL-3.0-or-later
//
// 网页宿主这一族源文件的卫生检查，两条：
//   1. 运行期字符串字面量只许 ASCII；
//   2. 只给单测用的存储目录重定向，发布代码（src/ 下除声明它的两个文件以外）不得调用。
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

TEST_CASE("the test-only storage redirect is not referenced by release code", "[webview][hygiene]")
{
    const juce::File src = repoRoot().getChildFile("src");
    REQUIRE(src.isDirectory()); // 路径推不出来就是红，不静默跳过

    juce::Array<juce::File> files;
    src.findChildFiles(files, juce::File::findFiles, true, "*.h;*.hpp;*.cpp;*.mm");
    REQUIRE(files.size() > 20); // 扫得到足够多的文件才算数

    int scanned = 0;
    for (const auto& f : files)
    {
        const auto name = f.getFileName();
        if (name == "UiDefaultsStore.h" || name == "UiDefaultsStore.cpp")
            continue;
        ++scanned;
        INFO(f.getFullPathName().toStdString());
        CHECK_FALSE(f.loadFileAsString().contains("setStorageDirForTests"));
    }
    CHECK(scanned > 20);

    // 声明方确实带着它，否则上面的扫描守的是一个不存在的名字
    CHECK(src.getChildFile("plugin-common")
              .getChildFile("UiDefaultsStore.h")
              .loadFileAsString()
              .contains("setStorageDirForTests"));
}
