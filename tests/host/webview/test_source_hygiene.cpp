// SPDX-License-Identifier: GPL-3.0-or-later
//
// 网页宿主这一族源文件的卫生检查，三条：
//   1. 运行期字符串字面量只许 ASCII；
//   2. 只给单测用的两个存储钩子（目录重定向、进程间锁替身），发布代码（src/ 下除声明它们的两个文件以外）不得调用；
//   3. 宿主类（不进宿主测试目标，编译不到这里）里靠调用点守的东西：导航闸必须走纯函数判定并在不放行时返回假，
//      状态快照必须用一次读回，不能退回逐个读设置文件；页面和内核交上来的文字（启动失败、导航错误、地址）
//      写日志和上面板之前必须先过整形函数；定时器必须在运行时三态分支之前启动；面板在场时迟到的桥请求
//      只回执、不置位、不计数、不发首帧、不收回面板，离开面板只有点重试一条路；注册进页面的每个桥函数，
//      处理函数一开头就有面板在场的守卫，守卫分支只写日志、给回执、返回，会改状态的三个回 not-ready，
//      分支里不碰窗口、设置文件、内存状态与状态发送；设置语言必须一次写完语言值与「选过」位。
//
// 第一条：网页宿主这一族源文件里，运行期字符串字面量只许 ASCII。
// 原因：源文件按 UTF-8 存，字面量里一旦有中文，在中文代码页的机器上编译器会报字符集警告，
// 而第一方代码要求零警告。中文只能写在注释里。
// 这里用一个小的扫描器逐个源文件找「字符串或字符字面量里出现非 ASCII 字节」，
// 扫描器自己带正反例，保证它不是空转。源文件路径由本文件的 __FILE__ 推出，找不到文件就判红，不静默跳过。

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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

    // 两个插件壳的入口文件也在这一族里：createEditor 的接线写在那里
    struct Target
    {
        std::string name;
        juce::File file;
    };
    std::vector<Target> targets;
    for (const char* name : names)
        targets.push_back({name, dir.getChildFile(name)});
    const juce::File src = dir.getParentDirectory();
    targets.push_back({"InputPluginEntry.cpp", src.getChildFile("input").getChildFile("InputPluginEntry.cpp")});
    targets.push_back({"OutputPluginEntry.cpp", src.getChildFile("output").getChildFile("OutputPluginEntry.cpp")});

    for (const auto& target : targets)
    {
        const std::string& name = target.name;
        const juce::File& file = target.file;
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

// 数 needle 在 text 里出现几次（不重叠）
std::size_t countOf(const std::string& text, const std::string& needle)
{
    std::size_t n = 0;
    for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size()))
        ++n;
    return n;
}

// 守卫分支：函数体（去注释后，含外层花括号）的第一条语句必须是 `if (fallback_ != nullptr) { ... }`，
// 返回那一对花括号连同里面的内容；不是这个形状（守卫排在别的语句之后、条件被收窄、没有花括号）给空串
std::string panelGuardBranch(const std::string& body)
{
    if (body.empty() || body.front() != '{')
        return {};
    const std::string guard = "if (fallback_ != nullptr)";
    auto skipSpaces = [&body](std::size_t at) {
        while (at < body.size() && (body[at] == ' ' || body[at] == '\n' || body[at] == '\r' || body[at] == '\t'))
            ++at;
        return at;
    };
    auto at = skipSpaces(1);
    if (body.compare(at, guard.size(), guard) != 0)
        return {};
    at = skipSpaces(at + guard.size());
    if (at >= body.size() || body[at] != '{')
        return {};
    return functionBody(body.substr(at), "{");
}

// 面板在场时，桥函数一开头就拒绝或只回执：守卫是函数体的第一条语句，分支里依次是一行日志、一个回执、返回，
// 且返回是分支的最后一句。reply 是分支里该有的回执调用：requestInitialState 的回执形状是 { ok }，给 ok；
// 会改窗口、改设置文件或改内存状态的函数给 not-ready（给 ok 等于告诉隐藏的页面「已生效」）。
// 分支里除了这个回执，不得出现任何桥函数的回执逻辑，也不得碰缩放模型、语言成员、设置文件、窗口大小、
// 就绪位、活着的桥的计数、状态发送、面板、网页可见性与加载：这些都是「只回执、什么都不改」的反面
bool bridgeHandlerRefusesWhilePanelShown(const std::string& body, const std::string& reply)
{
    const auto branch = panelGuardBranch(body);
    if (branch.empty())
        return false;
    const std::string call = "complete(" + reply + ")";
    const auto replyAt = branch.find(call);
    const auto leave = branch.find("return;");
    if (replyAt == std::string::npos || leave == std::string::npos)
        return false;
    // 返回之后只剩分支的收尾花括号：返回是分支的最后一句，不是嵌在条件里的有条件返回，后面也没有别的语句；
    // 回执若排在返回之后，也会在这里被挡下
    if (branch.find_first_not_of(" \n\r\t", leave + 7) != branch.size() - 1)
        return false;
    // 真机上靠这一行看这条路径走没走到；日志在回执之前
    const auto logAt = branch.find("logDiag(");
    if (logAt == std::string::npos || !(logAt < replyAt) ||
        !contains(branch, "bridge request ignored while the fallback panel is shown"))
        return false;
    std::string rest = branch;
    rest.erase(replyAt, call.size());
    for (const char* forbidden :
         {"bridge::",       "uidefaults::",       "persistUiScale(",   "scale_",        "lang_",
          "bridgeReady_",   "countedAsReady_",    "readyBridgeCount",  "stateSeq_",     "emitState(",
          "emitEvent(",     "fallback_",          "setVisible(",       "setSize(",      "setBounds(",
          "resized(",       "resizeToDesignBox(", "beginLoadAttempt(", "retryWebView(", "showFallback(",
          "setScaleLocked("})
        if (contains(rest, forbidden))
            return false;
    return true;
}

// makeOptions 里每一处 withNativeFunction 交给哪个处理函数：取这一段里第一个整词的 handleXxx(。
// 没有处理函数（内联的实现，核对不到守卫）给空串，调用方当作不合格
std::vector<std::string> registeredBridgeHandlers(const std::string& makeOptionsBody)
{
    std::vector<std::string> names;
    const std::string marker = "withNativeFunction(";
    for (auto at = makeOptionsBody.find(marker); at != std::string::npos;)
    {
        auto next = makeOptionsBody.find(marker, at + marker.size());
        auto end = next != std::string::npos ? next : makeOptionsBody.find("withEventListener(", at);
        const auto segment = makeOptionsBody.substr(at, end == std::string::npos ? std::string::npos : end - at);
        std::string name;
        for (auto h = segment.find("handle"); h != std::string::npos; h = segment.find("handle", h + 1))
        {
            if (h > 0 && isIdentChar(segment[h - 1]))
                continue;
            auto stop = h;
            while (stop < segment.size() && isIdentChar(segment[stop]))
                ++stop;
            if (stop < segment.size() && segment[stop] == '(')
                name = segment.substr(h, stop - h);
            break;
        }
        names.push_back(name);
        at = next;
    }
    return names;
}

// 注册进页面的桥函数里，处理函数开头没有合格守卫的那些（空名字也算）。text 是去注释后的整份源文件
std::vector<std::string> unguardedBridgeHandlers(const std::string& text, const std::vector<std::string>& handlers)
{
    std::vector<std::string> bad;
    for (const auto& name : handlers)
    {
        const auto body = name.empty() ? std::string{} : functionBody(text, "void WebViewHost::" + name + "(");
        const bool initial = name == "handleRequestInitialState";
        if (!bridgeHandlerRefusesWhilePanelShown(body, initial ? "bridge::okResponse()" : "bridge::respondNotReady()"))
            bad.push_back(name);
    }
    return bad;
}

// 离开面板只有点重试一条路：销毁面板的动作全文件只在重试里出现一次，网页重新可见只在加载尝试的开头出现一次，
// 重试经加载尝试复位就绪位与活着的桥的计数；以前那条「桥迟到就收回面板」的路径和它的原因记录不得再出现
bool panelLeavesOnlyThroughRetry(const std::string& text, const std::string& retry, const std::string& beginAttempt)
{
    return countOf(text, "fallback_.reset()") == 1 && contains(retry, "fallback_.reset()") &&
           countOf(text, "setVisible(true)") == 1 && contains(beginAttempt, "setVisible(true)") &&
           contains(retry, "beginLoadAttempt(") && contains(beginAttempt, "bridgeReady_ = false") &&
           contains(beginAttempt, "releaseReadyBridge()") && !contains(text, "withdrawFallbackForLateBridge") &&
           !contains(text, "fallbackReason_");
}

// 看门狗和启动失败上报都不碰已就绪的桥、也不在面板在场时再切一次：它们的前提写在条件里
bool watchdogAndBootErrorLeaveReadyAndShownAlone(const std::string& timerCallback, const std::string& bootError)
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

    // 面板在场时桥函数一开头就拒绝或只回执
    const std::string logLine = "logDiag(\"bridge request ignored while the fallback panel is shown (fn)\");";
    const std::string okReply = "bridge::okResponse()";
    const std::string notReady = "bridge::respondNotReady()";
    const std::string tail = " bridgeReady_ = true;\n if (!countedAsReady_) { countedAsReady_ = true; }\n"
                             " complete(bridge::okResponse());\n emitState(); }";
    const std::string goodInit =
        "{ if (fallback_ != nullptr)\n { " + logLine + "\n complete(bridge::okResponse());\n return;\n }\n" + tail;
    CHECK(bridgeHandlerRefusesWhilePanelShown(goodInit, okReply));
    CHECK(panelGuardBranch(goodInit).front() == '{');
    CHECK(panelGuardBranch(goodInit).back() == '}');
    CHECK_FALSE(bridgeHandlerRefusesWhilePanelShown(goodInit, notReady)); // 给 ok 的分支不是 not-ready 的分支

    const std::string work =
        " bool accepted = false;\n complete(bridge::respondSetLang(args, [&](const juce::String& c) {"
        " lang_ = c; uidefaults::chooseLangGlobal(c); accepted = true; }));\n"
        " if (accepted) emitState(); }";
    auto guarded = [&logLine](const std::string& inside, const std::string& cond = "if (fallback_ != nullptr)") {
        return "{ " + cond + "\n { " + logLine + "\n" + inside + " }\n";
    };
    const std::string goodLang = guarded(" complete(bridge::respondNotReady());\n return;\n") + work;
    CHECK(bridgeHandlerRefusesWhilePanelShown(goodLang, notReady));
    CHECK_FALSE(bridgeHandlerRefusesWhilePanelShown(goodLang, okReply)); // 会改状态的函数不得在面板在场时回 ok

    // 形状：守卫必须是函数体的第一条语句，条件不能收窄，要有花括号
    CHECK_FALSE(bridgeHandlerRefusesWhilePanelShown("{" + work, notReady)); // 没有这个分支
    CHECK_FALSE(bridgeHandlerRefusesWhilePanelShown( // 守卫排在别的语句之后（先改了东西才判面板）
        "{ lang_ = c;\n" + guarded(" complete(bridge::respondNotReady());\n return;\n").substr(2) + work, notReady));
    CHECK_FALSE(bridgeHandlerRefusesWhilePanelShown( // 条件收窄，只管一部分面板
        guarded(" complete(bridge::respondNotReady());\n return;\n",
                "if (fallback_ != nullptr && bootError_.isEmpty())") +
            work,
        notReady));
    CHECK_FALSE(bridgeHandlerRefusesWhilePanelShown( // 条件写反
        guarded(" complete(bridge::respondNotReady());\n return;\n", "if (fallback_ == nullptr)") + work, notReady));
    CHECK_FALSE(bridgeHandlerRefusesWhilePanelShown( // 没有花括号，守卫只管一句
        "{ if (fallback_ != nullptr)\n return;\n" + work, notReady));
    CHECK_FALSE(bridgeHandlerRefusesWhilePanelShown("", notReady));
    CHECK(panelGuardBranch("{ x = 1; }").empty());

    // 分支的三件事：日志、回执、返回
    CHECK_FALSE(bridgeHandlerRefusesWhilePanelShown( // 分支不返回，一路落到真正的处理
        guarded(" complete(bridge::respondNotReady());\n") + work, notReady));
    CHECK_FALSE(bridgeHandlerRefusesWhilePanelShown( // 分支里不给回执，页面的调用悬空
        guarded(" return;\n") + work, notReady));
    CHECK_FALSE(bridgeHandlerRefusesWhilePanelShown( // 回执排在返回之后
        guarded(" return;\n complete(bridge::respondNotReady());\n") + work, notReady));
    CHECK_FALSE(bridgeHandlerRefusesWhilePanelShown( // 返回之后还有语句
        guarded(" complete(bridge::respondNotReady());\n return;\n lang_ = c;\n") + work, notReady));
    CHECK_FALSE(bridgeHandlerRefusesWhilePanelShown( // 返回之后还有无害的语句：返回不是分支的最后一句
        guarded(" complete(bridge::respondNotReady());\n return;\n int unused = 0;\n") + work, notReady));
    CHECK_FALSE(bridgeHandlerRefusesWhilePanelShown( // 有条件的返回：条件不成立时一路落到真正的处理
        guarded(" complete(bridge::respondNotReady());\n if (c) { return; }\n") + work, notReady));
    CHECK_FALSE(bridgeHandlerRefusesWhilePanelShown( // 回了两次
        guarded(" complete(bridge::respondNotReady());\n complete(bridge::respondNotReady());\n return;\n") + work,
        notReady));
    CHECK_FALSE(bridgeHandlerRefusesWhilePanelShown( // 分支里没有那一行日志，真机上看不出这条路径走没走到
        "{ if (fallback_ != nullptr)\n { complete(bridge::respondNotReady());\n return;\n }\n" + work, notReady));
    CHECK_FALSE(bridgeHandlerRefusesWhilePanelShown( // 日志排在回执之后
        "{ if (fallback_ != nullptr)\n { complete(bridge::respondNotReady());\n " + logLine + "\n return;\n }\n" + work,
        notReady));
    CHECK_FALSE(bridgeHandlerRefusesWhilePanelShown( // 日志不是约定的那一行，grep 不到
        "{ if (fallback_ != nullptr)\n { logDiag(\"ignored\");\n complete(bridge::respondNotReady());\n return;\n }\n" +
            work,
        notReady));
    CHECK_FALSE(bridgeHandlerRefusesWhilePanelShown( // 回执调的是别的函数：把合格的回执换成了正常处理
        guarded(" complete(bridge::respondSetLang(args, {}));\n return;\n") + work, notReady));

    // 分支里不得改窗口、设置文件、内存状态，不得发状态、动面板与网页
    for (const char* forbidden : {"resizeToDesignBox();",
                                  "setSize(w, h);",
                                  "setBounds(r);",
                                  "resized();",
                                  "emitState();",
                                  "emitEvent(id, v);",
                                  "uidefaults::chooseLangGlobal(c);",
                                  "persistUiScale(s);",
                                  "scale_.apply(s);",
                                  "scale_.setLocked(true);",
                                  "lang_ = c;",
                                  "bridgeReady_ = true;",
                                  "countedAsReady_ = true;",
                                  "readyBridgeCount().fetch_add(1);",
                                  "stateSeq_ = 0;",
                                  "fallback_.reset();",
                                  "webView_->setVisible(true);",
                                  "beginLoadAttempt();",
                                  "retryWebView();",
                                  "showFallback(r);",
                                  "setScaleLocked(true);",
                                  "bridge::respondSetUiScale(scale_, r, a, w);"})
        CHECK_FALSE(bridgeHandlerRefusesWhilePanelShown(
            guarded(std::string(" ") + forbidden + "\n complete(bridge::respondNotReady());\n return;\n") + work,
            notReady));
    CHECK_FALSE(bridgeHandlerRefusesWhilePanelShown( // requestInitialState 的分支里发首帧
        "{ if (fallback_ != nullptr)\n { " + logLine +
            "\n complete(bridge::okResponse());\n emitState();\n return;\n }\n" + tail,
        okReply));

    // 注册进页面的桥函数：每一个的处理函数开头都得有合格守卫
    const std::string goodMake =
        "{ options = options.withNativeFunction(juce::Identifier(bridge::Fn::A), [this](const auto& a, auto c) {"
        " handleAlpha(a, std::move(c)); })\n.withNativeFunction(juce::Identifier(bridge::Fn::B), [this](const auto& a, "
        "auto c) { handleBeta(a, std::move(c)); });\n options = options.withEventListener(id, [this](const juce::var& "
        "p) { handleBootError(p); }); }";
    const auto names = registeredBridgeHandlers(goodMake);
    REQUIRE(names.size() == 2);
    CHECK(names[0] == "handleAlpha");
    CHECK(names[1] == "handleBeta"); // 事件监听里的 handleBootError 不是桥函数，不算
    CHECK(registeredBridgeHandlers("{ x; }").empty());
    const auto inlineNames = registeredBridgeHandlers( // 内联的实现：没有处理函数可核对，给空名字
        "{ options.withNativeFunction(id, [](const auto&, auto c) { c(ok()); }); }");
    REQUIRE(inlineNames.size() == 1);
    CHECK(inlineNames[0].empty());
    const auto wholeWord = registeredBridgeHandlers( // 整词：rehandle 之类不算
        "{ options.withNativeFunction(id, [](const auto& a, auto c) { rehandle(a); handleX(a, c); }); }");
    REQUIRE(wholeWord.size() == 1);
    CHECK(wholeWord[0] == "handleX");

    const std::string initOk =
        "void WebViewHost::handleRequestInitialState(const A& a, C complete)\n" + goodInit + "\n";
    const std::string betaOk = "void WebViewHost::handleBeta(const A& a, C complete)\n" + goodLang + "\n";
    CHECK(unguardedBridgeHandlers(initOk + betaOk, {"handleRequestInitialState", "handleBeta"}).empty());
    // requestInitialState 的回执是 ok，其余的是 not-ready：名字对错了就是不合格
    CHECK(unguardedBridgeHandlers(initOk + betaOk, {"handleBeta", "handleRequestInitialState"}).empty());
    const std::string betaWrongReply = "void WebViewHost::handleBeta(const A& a, C complete)\n" + goodInit + "\n";
    CHECK(unguardedBridgeHandlers(initOk + betaWrongReply, {"handleBeta"}).size() == 1);
    // 新加了第二个桥函数却忘了守卫：就是这一类
    const std::string betaNoGuard = "void WebViewHost::handleBeta(const A& a, C complete)\n{" + work + "\n";
    const auto missing = unguardedBridgeHandlers(initOk + betaNoGuard, {"handleRequestInitialState", "handleBeta"});
    REQUIRE(missing.size() == 1);
    CHECK(missing[0] == "handleBeta");
    CHECK(unguardedBridgeHandlers(initOk, {"handleRequestInitialState", "handleGamma"}).size() == 1); // 没有函数体
    CHECK(unguardedBridgeHandlers(initOk, {"handleRequestInitialState", ""}).size() == 1); // 内联的实现

    // 离开面板只有点重试一条路
    const std::string goodHost = "{ fallback_.reset(); }\n { webView_->setVisible(true); }";
    const std::string goodRetry = "{ fallback_.reset();\n beginLoadAttempt(); }";
    const std::string goodBegin = "{ releaseReadyBridge();\n bridgeReady_ = false;\n webView_->setVisible(true); }";
    CHECK(panelLeavesOnlyThroughRetry(goodHost, goodRetry, goodBegin));
    CHECK_FALSE(panelLeavesOnlyThroughRetry( // 第二处销毁面板：有人又加了一条收回的路
        goodHost + "\n { fallback_.reset(); }", goodRetry, goodBegin));
    CHECK_FALSE(panelLeavesOnlyThroughRetry( // 第二处让网页重新可见
        goodHost + "\n { webView_->setVisible(true); }", goodRetry, goodBegin));
    CHECK_FALSE(panelLeavesOnlyThroughRetry(goodHost, "{ beginLoadAttempt(); }", goodBegin)); // 销毁面板不在重试里
    CHECK_FALSE(panelLeavesOnlyThroughRetry(goodHost, "{ fallback_.reset(); }", goodBegin)); // 重试不经加载尝试
    CHECK_FALSE(panelLeavesOnlyThroughRetry( // 加载尝试不复位就绪位
        goodHost, goodRetry, "{ releaseReadyBridge();\n webView_->setVisible(true); }"));
    CHECK_FALSE(panelLeavesOnlyThroughRetry( // 加载尝试不还活着的桥的计数
        goodHost, goodRetry, "{ bridgeReady_ = false;\n webView_->setVisible(true); }"));
    CHECK_FALSE(panelLeavesOnlyThroughRetry( // 旧的收回函数还在
        goodHost + "\n void WebViewHost::withdrawFallbackForLateBridge() {}", goodRetry, goodBegin));
    CHECK_FALSE(panelLeavesOnlyThroughRetry( // 旧的原因记录还在
        goodHost + "\n { fallbackReason_ = reason; }", goodRetry, goodBegin));

    // 看门狗和启动失败上报的前提
    const std::string goodWatchdog = "{ if (!bridgeReady_ && fallback_ == nullptr && late()) show(); }";
    const std::string goodBootError = "{ if (bridgeReady_ || fallback_ != nullptr) return; show(); }";
    CHECK(watchdogAndBootErrorLeaveReadyAndShownAlone(goodWatchdog, goodBootError));
    CHECK_FALSE(watchdogAndBootErrorLeaveReadyAndShownAlone( // 看门狗不看面板在不在
        "{ if (!bridgeReady_ && late()) show(); }", goodBootError));
    CHECK_FALSE(watchdogAndBootErrorLeaveReadyAndShownAlone( // 看门狗不看桥就绪了没有
        "{ if (fallback_ == nullptr && late()) show(); }", goodBootError));
    CHECK_FALSE(watchdogAndBootErrorLeaveReadyAndShownAlone( // 启动失败上报不看桥就绪了没有
        goodWatchdog, "{ if (fallback_ != nullptr) return; show(); }"));
    CHECK_FALSE(watchdogAndBootErrorLeaveReadyAndShownAlone( // 启动失败上报不看面板在不在
        goodWatchdog, "{ if (bridgeReady_) return; show(); }"));

    // 设置语言
    CHECK(setLangWritesOnce("{ lang_ = c; uidefaults::chooseLangGlobal(c); }"));
    CHECK_FALSE(
        setLangWritesOnce("{ lang_ = c; uidefaults::setLangGlobal(c); uidefaults::setLangChosenGlobal(true); }"));
    CHECK_FALSE(
        setLangWritesOnce("{ lang_ = c; uidefaults::chooseLangGlobal(c); uidefaults::setLangChosenGlobal(true); }"));
    CHECK_FALSE(setLangWritesOnce("{ lang_ = c; }"));
}

TEST_CASE("the web host shapes page text, starts its timer early, guards every bridge function while the panel is "
          "shown and writes the language once",
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

    // 面板在场时迟到的桥请求，以及离开面板的唯一出口
    const auto initialState = functionBody(text, "void WebViewHost::handleRequestInitialState(");
    const auto retry = functionBody(text, "void WebViewHost::retryWebView()");
    REQUIRE_FALSE(initialState.empty());
    REQUIRE_FALSE(retry.empty());
    CHECK(bridgeHandlerRefusesWhilePanelShown(initialState, "bridge::okResponse()"));
    CHECK(panelLeavesOnlyThroughRetry(text, retry, begin));

    // 注册进页面的每个桥函数，处理函数一开头都有面板在场的守卫：四个通用桥函数一个不能少，将来再加的也逃不掉
    const auto makeOptions = functionBody(text, "juce::WebBrowserComponent::Options WebViewHost::makeOptions()");
    REQUIRE_FALSE(makeOptions.empty());
    const auto handlers = registeredBridgeHandlers(makeOptions);
    for (const char* expected :
         {"handleRequestInitialState", "handleSetLang", "handleSetUiScale", "handleCommitUiScale"})
        CHECK(std::count(handlers.begin(), handlers.end(), std::string(expected)) == 1); // 抽取没有失灵
    CHECK(unguardedBridgeHandlers(text, handlers).empty());

    const auto timerCallback = functionBody(text, "void WebViewHost::timerCallback()");
    REQUIRE_FALSE(timerCallback.empty());
    CHECK(watchdogAndBootErrorLeaveReadyAndShownAlone(timerCallback, bootError));

    // 设置语言
    const auto setLang = functionBody(text, "void WebViewHost::handleSetLang(");
    REQUIRE_FALSE(setLang.empty());
    CHECK(setLangWritesOnce(setLang));
}
