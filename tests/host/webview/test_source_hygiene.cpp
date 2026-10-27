// SPDX-License-Identifier: GPL-3.0-or-later
//
// 网页宿主这一族源文件的卫生检查，三条：
//   1. 运行期字符串字面量只许 ASCII；
//   2. 只给单测用的两个存储钩子（目录重定向、进程间锁替身），发布代码（src/ 下除声明它们的两个文件以外）不得调用；
//   3. 宿主类（不进宿主测试目标，编译不到这里）里靠调用点守的东西：导航闸必须走纯函数判定并在不放行时返回假，
//      状态快照必须用一次读回，不能退回逐个读设置文件；页面和内核交上来的文字（启动失败、导航错误、地址）
//      写日志和上面板之前必须先过整形函数；定时器必须在运行时三态分支之前启动；看门狗切面板之后桥才起来时
//      必须收回面板，且只收回超时那一种；设置语言必须一次写完语言值与「选过」位。
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

// 去掉 // 与 /* */ 注释，字符串和字符字面量里的斜杠不算。只为调用点扫描用：注释里提到函数名不该让扫描误判
std::string stripComments(const std::string& src)
{
    std::string out;
    enum class State
    {
        code,
        lineComment,
        blockComment,
        stringLiteral,
        charLiteral
    };
    State state = State::code;
    for (std::size_t i = 0; i < src.size(); ++i)
    {
        const char c = src[i];
        const char next = i + 1 < src.size() ? src[i + 1] : '\0';
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
            else
            {
                if (c == '"')
                    state = State::stringLiteral;
                else if (c == '\'')
                    state = State::charLiteral;
                out.push_back(c);
            }
            break;
        case State::lineComment:
            if (c == '\n')
            {
                state = State::code;
                out.push_back(c);
            }
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
            out.push_back(c);
            if (c == '\\' && next != '\0')
            {
                out.push_back(next);
                ++i;
            }
            else if ((state == State::stringLiteral && c == '"') || (state == State::charLiteral && c == '\''))
            {
                state = State::code;
            }
            break;
        }
    }
    return out;
}

bool isIdentChar(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

// body 里 token 作为整词出现的每一处，是否都落在某次 shaper(...) 调用的括号里面。
// 用来要求「来路不明的文字只经整形函数才碰到成员和日志」：整形函数的括号一合上，后面再出现 token 就算裸用
bool everyUseIsShaped(const std::string& body, const std::string& token, const std::string& shaper)
{
    std::size_t uses = 0;
    for (std::size_t at = body.find(token); at != std::string::npos; at = body.find(token, at + 1))
    {
        const bool wordStart = at == 0 || !isIdentChar(body[at - 1]);
        const bool wordEnd =
            at + token.size() >= body.size() || !isIdentChar(body[at + token.size()]) || !isIdentChar(token.back());
        if (!wordStart || !wordEnd)
            continue;
        ++uses;
        const auto call = body.rfind(shaper, at);
        if (call == std::string::npos)
            return false;
        int depth = 0;
        bool inside = false;
        for (std::size_t i = call + shaper.size() - 1; i < at; ++i)
        {
            if (body[i] == '(')
                ++depth;
            else if (body[i] == ')' && --depth == 0)
                break;
            if (i + 1 == at)
                inside = depth > 0;
        }
        if (!inside)
            return false;
    }
    return uses > 0; // 一处都没找到说明 token 抄错了，不能算过
}

// 定时器要在运行时探测之后、缺失与过旧两条直接返回的分支之前启动，且防重复启动
bool timerStartsBeforeRuntimeBranches(const std::string& body)
{
    const auto timer = body.find("startTimerHz(");
    const auto fallback = body.find("showFallback(");
    const auto early = body.find("return;");
    if (timer == std::string::npos || fallback == std::string::npos || early == std::string::npos)
        return false;
    const auto guard = body.rfind("isTimerRunning()", timer);
    return timer < fallback && timer < early && guard != std::string::npos && timer - guard < 40;
}

// 桥晚于看门狗起来时收回面板：只收超时那一种，在首帧之前，收回的动作不重新加载页面
bool lateBridgeWithdrawsPanel(const std::string& initialState, const std::string& withdraw,
                              const std::string& showFallback)
{
    const auto guard = initialState.find("fallbackReason_ == FallbackReason::LoadTimeout");
    const auto call = initialState.find("withdrawFallbackForLateBridge(");
    const auto emit = initialState.find("emitState()");
    if (guard == std::string::npos || call == std::string::npos || emit == std::string::npos)
        return false;
    if (!(guard < call && call < emit))
        return false;
    // 只许认超时这一个原因，别的原因出现在这个函数里就是放宽了
    std::size_t reasons = 0;
    for (auto at = initialState.find("FallbackReason::"); at != std::string::npos;
         at = initialState.find("FallbackReason::", at + 1))
        ++reasons;
    if (reasons != 1)
        return false;
    return contains(withdraw, "fallback_.reset()") && contains(withdraw, "setVisible(true)") &&
           contains(withdraw, "resizeToDesignBox()") && !contains(withdraw, "beginLoadAttempt(") &&
           contains(showFallback, "fallbackReason_ = reason");
}

// 收回之后不能再被顶回面板：看门狗和启动失败上报都以「桥未就绪」为前提
bool nothingReshowsPanelOnceReady(const std::string& timerCallback, const std::string& bootError)
{
    return contains(timerCallback, "!bridgeReady_ && fallback_ == nullptr") &&
           contains(bootError, "bridgeReady_ || fallback_ != nullptr");
}

// 设置语言一次写完语言值与「选过」位：只调合并入口，不再分两次调
bool setLangWritesOnce(const std::string& body)
{
    return contains(body, "uidefaults::chooseLangGlobal(") && !contains(body, "setLangGlobal(") &&
           !contains(body, "setLangChosenGlobal(");
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

TEST_CASE("call-site scanners for the review fixes: good and bad samples", "[webview][hygiene]")
{
    // 去注释：注释里的函数名不算，字符串里的斜杠不当注释
    CHECK(stripComments("a(); // b();\nc();") == "a(); \nc();");
    CHECK(stripComments("a(); /* b(); */ c();") == "a();  c();");
    CHECK(stripComments("log(\"https://x\"); // tail") == "log(\"https://x\"); ");
    CHECK(stripComments("char c = '\"'; // q") == "char c = '\"'; ");

    // 整形：每一处使用都在整形函数的括号里
    CHECK(everyUseIsShaped("{ x = shape(raw, 5); log(shape(raw)); }", "raw", "shape("));
    CHECK(everyUseIsShaped("{ x = shape(get(\"a\", raw).toString(), 5); }", "raw", "shape("));
    CHECK_FALSE(everyUseIsShaped("{ x = shape(raw, 5); log(raw); }", "raw", "shape(")); // 第二处裸用
    CHECK_FALSE(everyUseIsShaped("{ log(raw); x = shape(raw, 5); }", "raw", "shape(")); // 第一处裸用
    CHECK_FALSE(everyUseIsShaped("{ x = shape(other); log(raw); }", "raw", "shape(")); // 整形的括号已合上
    CHECK_FALSE(everyUseIsShaped("{ x = other(raw); }", "raw", "shape(")); // 没有整形函数
    CHECK_FALSE(everyUseIsShaped("{ x = shape(other); }", "raw", "shape(")); // token 一处都没有，抄错了不算过
    CHECK(everyUseIsShaped("{ x = shape(rawer); y = shape(raw); }", "raw", "shape(")); // 整词匹配：rawer 不是 raw
    CHECK(everyUseIsShaped("{ log(urlForLog(url, 200)); }", "url", "urlForLog("));
    CHECK(everyUseIsShaped("{ g(payload.getProperty(\"a\")); }", "getProperty(", "g(")); // 以括号结尾的 token

    // 定时器先于分支
    const std::string goodTimer = "{ runtime_ = probe();\n if (!isTimerRunning())\n startTimerHz(25);\n if (missing) { "
                                  "showFallback(1); return; }\n"
                                  " go(); }";
    CHECK(timerStartsBeforeRuntimeBranches(goodTimer));
    CHECK_FALSE(timerStartsBeforeRuntimeBranches( // 老写法：定时器在两条提前返回的分支之后
        "{ runtime_ = probe();\n if (missing) { showFallback(1); return; }\n go();\n if (!isTimerRunning())\n"
        " startTimerHz(25); }"));
    CHECK_FALSE(timerStartsBeforeRuntimeBranches( // 没有防重复
        "{ runtime_ = probe();\n startTimerHz(25);\n if (missing) { showFallback(1); return; }\n go(); }"));
    CHECK_FALSE(
        timerStartsBeforeRuntimeBranches("{ if (missing) { showFallback(1); return; }\n go(); }")); // 根本不启动

    // 迟到的桥
    const std::string goodInit =
        "{ bridgeReady_ = true;\n complete(ok());\n if (fallback_ != nullptr && fallbackReason_ == "
        "FallbackReason::LoadTimeout)\n withdrawFallbackForLateBridge();\n emitState(); }";
    const std::string goodWithdraw =
        "{ log(); fallback_.reset();\n webView_->setVisible(true);\n resizeToDesignBox();\n resized(); }";
    const std::string goodShow = "{ if (fallback_ != nullptr) return;\n fallbackReason_ = reason;\n build(); }";
    CHECK(lateBridgeWithdrawsPanel(goodInit, goodWithdraw, goodShow));
    CHECK_FALSE(lateBridgeWithdrawsPanel( // 没有任何收回
        "{ bridgeReady_ = true;\n complete(ok());\n emitState(); }", goodWithdraw, goodShow));
    CHECK_FALSE(lateBridgeWithdrawsPanel( // 不分原因一律收回
        "{ bridgeReady_ = true;\n if (fallback_ != nullptr)\n withdrawFallbackForLateBridge();\n emitState(); }",
        goodWithdraw, goodShow));
    CHECK_FALSE(lateBridgeWithdrawsPanel( // 把启动失败也算进去
        "{ if (fallback_ != nullptr && (fallbackReason_ == FallbackReason::LoadTimeout || fallbackReason_ == "
        "FallbackReason::BootError))\n withdrawFallbackForLateBridge();\n emitState(); }",
        goodWithdraw, goodShow));
    CHECK_FALSE(lateBridgeWithdrawsPanel( // 收在首帧之后：网页不可见，首帧发不出去
        "{ if (fallback_ != nullptr && fallbackReason_ == FallbackReason::LoadTimeout) { emitState();\n"
        " withdrawFallbackForLateBridge(); } }",
        goodWithdraw, goodShow));
    CHECK_FALSE(
        lateBridgeWithdrawsPanel(goodInit, "{ fallback_.reset();\n resizeToDesignBox(); }", goodShow)); // 网页没放回
    CHECK_FALSE(lateBridgeWithdrawsPanel(goodInit, "{ webView_->setVisible(true);\n resizeToDesignBox(); }", goodShow));
    CHECK_FALSE(lateBridgeWithdrawsPanel(goodInit, "{ fallback_.reset();\n webView_->setVisible(true); }", goodShow));
    CHECK_FALSE(lateBridgeWithdrawsPanel( // 收回时又重新加载：等于重试
        goodInit, "{ fallback_.reset();\n webView_->setVisible(true);\n resizeToDesignBox();\n beginLoadAttempt(); }",
        goodShow));
    CHECK_FALSE(lateBridgeWithdrawsPanel(goodInit, goodWithdraw, "{ if (fallback_ != nullptr) return; build(); }"));

    // 收回之后不再被顶回面板
    CHECK(nothingReshowsPanelOnceReady("{ if (!bridgeReady_ && fallback_ == nullptr && late()) show(); }",
                                       "{ if (bridgeReady_ || fallback_ != nullptr) return; show(); }"));
    CHECK_FALSE(nothingReshowsPanelOnceReady("{ if (fallback_ == nullptr && late()) show(); }",
                                             "{ if (bridgeReady_ || fallback_ != nullptr) return; show(); }"));
    CHECK_FALSE(nothingReshowsPanelOnceReady("{ if (!bridgeReady_ && fallback_ == nullptr && late()) show(); }",
                                             "{ if (fallback_ != nullptr) return; show(); }"));

    // 设置语言
    CHECK(setLangWritesOnce("{ lang_ = c; uidefaults::chooseLangGlobal(c); }"));
    CHECK_FALSE(
        setLangWritesOnce("{ lang_ = c; uidefaults::setLangGlobal(c); uidefaults::setLangChosenGlobal(true); }"));
    CHECK_FALSE(
        setLangWritesOnce("{ lang_ = c; uidefaults::chooseLangGlobal(c); uidefaults::setLangChosenGlobal(true); }"));
    CHECK_FALSE(setLangWritesOnce("{ lang_ = c; }"));
}

TEST_CASE("the web host shapes page text, starts its timer early, takes back a late-bridge panel and writes the "
          "language once",
          "[webview][hygiene]")
{
    const juce::File file =
        repoRoot().getChildFile("src").getChildFile("plugin-common").getChildFile("WebViewHost.cpp");
    REQUIRE(file.existsAsFile()); // 路径推不出来就是红，不静默跳过
    const std::string text = stripComments(file.loadFileAsString().toStdString());

    // 来路不明的文字只经整形函数才碰到成员和日志
    const auto bootError = functionBody(text, "void WebViewHost::handleBootError(");
    REQUIRE_FALSE(bootError.empty());
    CHECK(everyUseIsShaped(bootError, "getProperty(", "ResourceProvider::asciiForLog("));

    const auto navError = functionBody(text, "void WebViewHost::onNavigationError(");
    REQUIRE_FALSE(navError.empty());
    CHECK(everyUseIsShaped(navError, "errorInfo", "ResourceProvider::asciiForLog("));

    const auto navStarted = functionBody(text, "void WebViewHost::onNavigationStarted(");
    REQUIRE_FALSE(navStarted.empty());
    CHECK(everyUseIsShaped(navStarted, "url", "ResourceProvider::urlForLog("));

    const auto navFinished = functionBody(text, "void WebViewHost::onNavigationFinished(");
    REQUIRE_FALSE(navFinished.empty());
    CHECK(everyUseIsShaped(navFinished, "url", "ResourceProvider::urlForLog("));

    const auto newWindow = functionBody(text, "void newWindowAttemptingToLoad(");
    REQUIRE_FALSE(newWindow.empty());
    CHECK(everyUseIsShaped(newWindow, "newURL", "ResourceProvider::urlForLog("));

    // 定时器
    const auto begin = functionBody(text, "void WebViewHost::beginLoadAttempt()");
    REQUIRE_FALSE(begin.empty());
    CHECK(timerStartsBeforeRuntimeBranches(begin));

    // 迟到的桥
    const auto initialState = functionBody(text, "void WebViewHost::handleRequestInitialState(");
    const auto withdraw = functionBody(text, "void WebViewHost::withdrawFallbackForLateBridge()");
    const auto show = functionBody(text, "void WebViewHost::showFallback(");
    REQUIRE_FALSE(initialState.empty());
    REQUIRE_FALSE(withdraw.empty());
    REQUIRE_FALSE(show.empty());
    CHECK(lateBridgeWithdrawsPanel(initialState, withdraw, show));

    const auto timerCallback = functionBody(text, "void WebViewHost::timerCallback()");
    REQUIRE_FALSE(timerCallback.empty());
    CHECK(nothingReshowsPanelOnceReady(timerCallback, bootError));

    // 设置语言
    const auto setLang = functionBody(text, "void WebViewHost::handleSetLang(");
    REQUIRE_FALSE(setLang.empty());
    CHECK(setLangWritesOnce(setLang));
}
