// SPDX-License-Identifier: GPL-3.0-or-later
//
// 两个角色共用的几个桥函数的纯逻辑（BridgeBase）：openExternal 的白名单与回执、copyDiagnostics 的两种失败、
// 诊断末行的 ui 段、几个参数的严格校验。白名单是原生侧的最后一道，表里每一行都是一种绕过的写法。

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <limits>
#include <string>
#include <vector>

#include "BridgeBase.h"

namespace
{

namespace b = rinne::bridge;

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

} // namespace

TEST_CASE("openExternal whitelist: the addresses the pages link to are allowed", "[host][bridge][external]")
{
    for (const char* url : {
             "https://github.com/synchain-oss/rinne",
             "https://github.com/synchain-oss/rinne/",
             "https://github.com/synchain-oss/rinne/issues",
             "https://github.com/synchain-oss/rinne/blob/v0.10.12/docs/USER_GUIDE.zh-CN.md",
             "https://github.com/synchain-oss/rinne/blob/v1.0.0-rc.1/THIRD-PARTY-NOTICES.md",
             "https://github.com/synchain-oss/rinne/blob/prod/THIRD-PARTY-NOTICES.md",
             "https://synchain.ca/",
             "https://synchain.ca",
             "https://synchain.ca/rinne",
             // scheme 与 host 不分大小写（浏览器按小写请求同一个地方）
             "HTTPS://GitHub.com/synchain-oss/rinne",
             "https://SYNCHAIN.CA/",
         })
    {
        INFO(url);
        CHECK(b::isAllowedExternalUrl(url));
    }
}

TEST_CASE("openExternal whitelist: everything else is refused", "[host][bridge][external]")
{
    for (const char* url : {
             // 空串与别的 scheme
             "",
             "http://synchain.ca/",
             "http://github.com/synchain-oss/rinne",
             "ftp://synchain.ca/",
             "file:///C:/Windows/notepad.exe",
             "javascript:alert(1)",
             "data:text/html,hi",
             "https:synchain.ca/",
             "https:/synchain.ca/",
             "https:///synchain.ca/",
             // 与 https:// 等长的别的前缀：scheme 必须逐字节核对，不能只数长度
             "httpx://synchain.ca/",
             "file:///synchain.ca/",
             "//synchain.ca/",
             "synchain.ca",
             // 别的 host，以及看起来很像的 host
             "https://example.com/",
             "https://gist.github.com/synchain-oss/rinne",
             "https://www.github.com/synchain-oss/rinne",
             "https://github.com.evil.example/synchain-oss/rinne",
             "https://githubXcom/synchain-oss/rinne",
             "https://synchain.ca.evil.example/",
             "https://evilsynchain.ca/",
             "https://www.synchain.ca/",
             "https://synchain.ca./",
             "https://github.com./synchain-oss/rinne",
             // 用户信息、端口与 IPv6 字面量
             "https://synchain.ca@evil.example/",
             "https://user@synchain.ca/",
             "https://user:pass@github.com/synchain-oss/rinne",
             "https://github.com@evil.example/synchain-oss/rinne",
             "https://synchain.ca:443/",
             "https://github.com:8443/synchain-oss/rinne",
             "https://[::1]/",
             // github.com 上别的仓：路径前缀伪装
             "https://github.com/",
             "https://github.com",
             "https://github.com/synchain-oss",
             "https://github.com/synchain-oss/",
             "https://github.com/synchain-oss/rinne-evil",
             "https://github.com/synchain-oss/rinnex/issues",
             "https://github.com/synchain-oss/scvb",
             "https://github.com/Synchain-oss/rinne",
             "https://github.com/evil/synchain-oss/rinne",
             "https://github.com//synchain-oss/rinne",
             // 点段：浏览器先折叠再请求，前缀就被绕过了
             "https://github.com/synchain-oss/rinne/../../evil/repo",
             "https://github.com/synchain-oss/rinne/./x",
             "https://github.com/synchain-oss/rinne/..",
             "https://synchain.ca/../x",
             // 查询串与片段，含空的
             "https://synchain.ca/?",
             "https://synchain.ca/#",
             "https://synchain.ca/?#",
             "https://synchain.ca?",
             "https://synchain.ca#",
             "https://synchain.ca/?a=1",
             "https://synchain.ca/#top",
             "https://github.com/synchain-oss/rinne?",
             "https://github.com/synchain-oss/rinne#",
             "https://github.com/synchain-oss/rinne/blob/prod/docs/USER_GUIDE.zh-CN.md#windows",
             "https://github.com/synchain-oss/rinne?tab=readme",
             // 百分号编码：不解码，一律拒
             "https://github.com/synchain-oss/rinne/%2e%2e/%2e%2e/evil",
             "https://github.com/synchain-oss/rinne%2F..%2F..%2Fevil",
             "https://github.com/synchain-oss%2Frinne",
             "https://synchain.ca/%41",
             "https://%73ynchain.ca/",
             // 反斜杠、空白与控制字符
             "https://github.com\\evil.example/synchain-oss/rinne",
             "https://github.com/synchain-oss/rinne\\..\\..\\evil",
             "https://synchain.ca/ x",
             " https://synchain.ca/",
             "https://synchain.ca/\t",
             "https://synchain.ca/\nx",
             // 路径里不在字符表内的字符
             "https://synchain.ca/<script>",
             "https://synchain.ca/a\"b",
             "https://github.com/synchain-oss/rinne/@evil",
         })
    {
        INFO(url);
        CHECK_FALSE(b::isAllowedExternalUrl(url));
    }
    // 非 ASCII：同形字的 host
    CHECK_FALSE(b::isAllowedExternalUrl(juce::String::fromUTF8("https://g\xd1\x96thub.com/synchain-oss/rinne")));
    CHECK_FALSE(b::isAllowedExternalUrl(juce::String::fromUTF8("https://synchain.ca/\xe4\xb8\xad")));
    // 超长：上限 2048 字节，恰好 2048 还放行
    const juce::String site = "https://synchain.ca/";
    CHECK(b::isAllowedExternalUrl(site + juce::String::repeatedString("a", 2048 - site.length())));
    CHECK_FALSE(b::isAllowedExternalUrl(site + juce::String::repeatedString("a", 2049 - site.length())));
    CHECK_FALSE(b::isAllowedExternalUrl(site + juce::String::repeatedString("a", 3000)));
}

TEST_CASE("openExternal: the receipt and when the launcher is called", "[host][bridge][external]")
{
    std::vector<juce::String> launched;
    auto launch = [&launched](const juce::String& url) {
        launched.push_back(url);
        return true;
    };

    const auto ok = b::respondOpenExternal(args({juce::var("https://synchain.ca/")}), launch);
    CHECK(isOk(ok));
    REQUIRE(launched.size() == 1);
    CHECK(launched.front() == "https://synchain.ca/");

    // 不在白名单、不是字符串、缺参：not-allowed，不调启动器
    for (const auto& bad : {args({juce::var("https://example.com/")}), args({juce::var(1)}), args({}),
                            args({juce::var("https://synchain.ca/#x")})})
    {
        const auto r = b::respondOpenExternal(bad, launch);
        CHECK_FALSE(isOk(r));
        CHECK(reasonOf(r) == "not-allowed");
    }
    CHECK(launched.size() == 1);

    // 地址合格但系统没打开：unavailable
    const auto failed =
        b::respondOpenExternal(args({juce::var("https://synchain.ca/")}), [](const juce::String&) { return false; });
    CHECK_FALSE(isOk(failed));
    CHECK(reasonOf(failed) == "unavailable");
    CHECK(reasonOf(b::respondOpenExternal(args({juce::var("https://synchain.ca/")}), {})) == "unavailable");
}

TEST_CASE("copyDiagnostics: preview, commit and the two failure codes", "[host][bridge][diag]")
{
    std::vector<juce::String> written;
    auto write = [&written](const juce::String& text) {
        written.push_back(text);
        return true;
    };

    SECTION("false and anything that is not exactly true only preview")
    {
        for (const auto& preview :
             {args({juce::var(false)}), args({}), args({juce::var(1)}), args({juce::var("true")})})
        {
            const auto r = b::respondCopyDiagnostics(preview, "rinne-diag v1\nui: x", write);
            CHECK(isOk(r));
            CHECK(r.getProperty("copied", juce::var()).isBool());
            CHECK_FALSE(static_cast<bool>(r.getProperty("copied", juce::var())));
            CHECK(r.getProperty("text", juce::var()).toString() == "rinne-diag v1\nui: x");
        }
        CHECK(written.empty());
    }
    SECTION("true writes the clipboard and says so")
    {
        const auto r = b::respondCopyDiagnostics(args({juce::var(true)}), "rinne-diag v1\nui: x", write);
        CHECK(isOk(r));
        CHECK(static_cast<bool>(r.getProperty("copied", juce::var())));
        REQUIRE(written.size() == 1);
        CHECK(written.front() == "rinne-diag v1\nui: x");
    }
    SECTION("no text yet: not-ready, the clipboard is not touched")
    {
        for (const auto& a : {args({juce::var(true)}), args({juce::var(false)})})
        {
            const auto r = b::respondCopyDiagnostics(a, {}, write);
            CHECK_FALSE(isOk(r));
            CHECK(r.getProperty("copied", juce::var()).isBool());
            CHECK_FALSE(static_cast<bool>(r.getProperty("copied", juce::var())));
            CHECK(reasonOf(r) == "not-ready");
            CHECK_FALSE(r.hasProperty("text"));
        }
        CHECK(written.empty());
    }
    SECTION("the clipboard did not take it: clipboard")
    {
        const auto r = b::respondCopyDiagnostics(args({juce::var(true)}), "rinne-diag v1",
                                                 [](const juce::String&) { return false; });
        CHECK_FALSE(isOk(r));
        CHECK_FALSE(static_cast<bool>(r.getProperty("copied", juce::var())));
        CHECK(reasonOf(r) == "clipboard");
        // 没有写入口也按写不进去处理
        CHECK(reasonOf(b::respondCopyDiagnostics(args({juce::var(true)}), "rinne-diag v1", {})) == "clipboard");
    }
}

TEST_CASE("diagnostics: the ui line is the last line and stays inside the fixed vocabulary", "[host][bridge][diag]")
{
    CHECK(b::uiDiagLine("WebView2 131.0.2903.70", 1.0f, "zh") ==
          "ui: webview=WebView2-131.0.2903.70 scale=1.00 lang=zh");
    CHECK(b::uiDiagLine("WebKit (system)", 1.5f, "en") == "ui: webview=WebKit-(system) scale=1.50 lang=en");
    CHECK(b::uiDiagLine("WebView2 not found", 0.65f, "fr") == "ui: webview=WebView2-not-found scale=0.65 lang=fr");
    // 不认识的语言按中文；带路径的运行时描述整串不要
    CHECK(b::uiDiagLine("runtime\\x64\\msedgewebview2", 2.0f, "de") == "ui: webview=unknown scale=2.00 lang=zh");
    CHECK(b::uiDiagLine({}, 1.25f, "en") == "ui: webview=unknown scale=1.25 lang=en");

    const juce::String ui = "ui: webview=x scale=1.00 lang=zh";
    CHECK(b::withUiDiagLine("rinne-diag v1\nevents(last 0 of 0):\n", ui) ==
          "rinne-diag v1\nevents(last 0 of 0):\n" + ui);
    CHECK(b::withUiDiagLine("rinne-diag v1", ui) == "rinne-diag v1\n" + ui);
    CHECK(b::withUiDiagLine({}, ui).isEmpty());
}

TEST_CASE("bridge arguments: booleans, rule versions and group letters are strict", "[host][bridge]")
{
    bool flag = false;
    CHECK(b::jsBoolArg(args({juce::var(true)}), flag));
    CHECK(flag);
    CHECK(b::jsBoolArg(args({juce::var(false)}), flag));
    CHECK_FALSE(flag);
    for (const auto& bad : {args({}), args({juce::var(1)}), args({juce::var("true")}), args({juce::var(0.0)})})
        CHECK_FALSE(b::jsBoolArg(bad, flag));

    int version = -1;
    CHECK(b::parseRulesAckArg(args({juce::var(3)}), version));
    CHECK(version == 3);
    CHECK(b::parseRulesAckArg(args({juce::var(0)}), version));
    CHECK(version == 0);
    CHECK(b::parseRulesAckArg(args({juce::var(4.0)}), version));
    CHECK(version == 4);
    for (const auto& bad : {args({}), args({juce::var(-1)}), args({juce::var(1.5)}), args({juce::var("2")}),
                            args({juce::var(true)}), args({juce::var(static_cast<juce::int64>(1) << 40)}),
                            args({juce::var(std::numeric_limits<double>::infinity())})})
        CHECK_FALSE(b::parseRulesAckArg(bad, version));

    int group = 0;
    CHECK(b::parseGroupArg(args({juce::var("A")}), group));
    CHECK(group == 1);
    CHECK(b::parseGroupArg(args({juce::var("H")}), group));
    CHECK(group == 8);
    for (const auto& bad : {args({}), args({juce::var("I")}), args({juce::var("a")}), args({juce::var("AB")}),
                            args({juce::var("")}), args({juce::var(1)})})
        CHECK_FALSE(b::parseGroupArg(bad, group));

    CHECK(b::groupLetter(1) == "A");
    CHECK(b::groupLetter(8) == "H");
    CHECK(b::groupLetter(0).isEmpty());
    CHECK(b::groupLetter(9).isEmpty());
}
