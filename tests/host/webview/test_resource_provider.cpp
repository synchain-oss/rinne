// SPDX-License-Identifier: GPL-3.0-or-later
//
// 资源提供者：地址归一、按文件名扁平反查、MIME 映射、空源不造假资源。
// 提供者只认二进制数据的最小描述，这里用静态数据做一个假的源。

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>

#include "ResourceProvider.h"

namespace
{

using rinne::webview::ResourceProvider;

const char* const kNames[] = {"index.html", "shared.js", "app.mjs", "tokens.css",
                              "face.woff2", "data.bin",  "dup.js",  "dup.js"};
const char* const kSymbols[] = {"index_html", "shared_js", "app_mjs",  "tokens_css",
                                "face_woff2", "data_bin",  "dup_js_a", "dup_js_b"};
const char kBodies[][16] = {"<html>", "export{}", "export{}", "body{}", "wOF2", "BIN", "first", "second"};

const char* getResource(const char* symbol, int& size)
{
    for (std::size_t i = 0; i < sizeof(kSymbols) / sizeof(kSymbols[0]); ++i)
    {
        if (std::strcmp(symbol, kSymbols[i]) == 0)
        {
            size = static_cast<int>(std::strlen(kBodies[i]));
            return kBodies[i];
        }
    }
    size = 0;
    return nullptr;
}

const char* getNothing(const char*, int& size)
{
    size = 0;
    return nullptr;
}

ResourceProvider::Source fullSource()
{
    ResourceProvider::Source s;
    s.resourceCount = static_cast<int>(sizeof(kNames) / sizeof(kNames[0]));
    s.originalFilenames = kNames;
    s.resourceNames = kSymbols;
    s.getNamedResource = getResource;
    return s;
}

std::string body(const ResourceProvider::Hit& hit)
{
    return std::string(hit.data, static_cast<std::size_t>(hit.size));
}

} // namespace

TEST_CASE("resource urls are normalised to root relative paths", "[webview][resource]")
{
    CHECK(ResourceProvider::normalizePath("/") == "index.html");
    CHECK(ResourceProvider::normalizePath("") == "index.html");
    CHECK(ResourceProvider::normalizePath("/index.html") == "index.html");
    CHECK(ResourceProvider::normalizePath("/input/index.html") == "input/index.html");
    CHECK(ResourceProvider::normalizePath("/shared/x.js?v=2") == "shared/x.js");
    CHECK(ResourceProvider::normalizePath("/shared/x.js#frag") == "shared/x.js");
    CHECK(ResourceProvider::normalizePath("/a.css?x=1#y") == "a.css");

    // 完整地址去掉来源
    CHECK(ResourceProvider::normalizePath("https://juce.backend/input/index.html") == "input/index.html");
    CHECK(ResourceProvider::normalizePath("https://juce.backend/") == "index.html");
    CHECK(ResourceProvider::normalizePath("https://juce.backend") == "index.html");
    CHECK(ResourceProvider::normalizePath("https://juce.backend/shared/x.js?v=1") == "shared/x.js");
}

TEST_CASE("lookup matches by file name only, whatever the directory", "[webview][resource]")
{
    const ResourceProvider provider(fullSource());

    const auto direct = provider.lookup("/index.html");
    REQUIRE(direct.has_value());
    CHECK(body(*direct) == "<html>");
    CHECK(std::string(direct->mime) == "text/html");

    // 目录层级不进包：不同目录下的同名引用命中同一个文件
    const auto nested = provider.lookup("/input/index.html");
    REQUIRE(nested.has_value());
    CHECK(body(*nested) == "<html>");
    const auto shared = provider.lookup("/shared/shared.js");
    REQUIRE(shared.has_value());
    CHECK(body(*shared) == "export{}");
    CHECK(std::string(shared->mime) == "text/javascript");

    // 根请求归一成首页
    const auto root = provider.lookup("/");
    REQUIRE(root.has_value());
    CHECK(body(*root) == "<html>");

    const auto full = provider.lookup("https://juce.backend/input/index.html?x=1");
    REQUIRE(full.has_value());
    CHECK(body(*full) == "<html>");
}

TEST_CASE("lookup serves module files with a script mime", "[webview][resource]")
{
    const ResourceProvider provider(fullSource());
    const auto mjs = provider.lookup("/app.mjs");
    REQUIRE(mjs.has_value());
    CHECK(std::string(mjs->mime) == "text/javascript"); // 模块脚本必须是脚本类型，否则浏览器拒载
    const auto font = provider.lookup("/fonts/face.woff2");
    REQUIRE(font.has_value());
    CHECK(std::string(font->mime) == "font/woff2");
    CHECK(std::string(provider.lookup("/tokens.css")->mime) == "text/css");
    CHECK(std::string(provider.lookup("/data.bin")->mime) == "application/octet-stream");
}

TEST_CASE("lookup misses return nothing", "[webview][resource]")
{
    const ResourceProvider provider(fullSource());
    CHECK_FALSE(provider.lookup("/missing.js").has_value());
    CHECK_FALSE(provider.lookup("/shared/").has_value()); // 目录地址：文件名是空串，归一后落到整段
    CHECK_FALSE(provider.lookup("/INDEX.HTML").has_value()); // 文件名区分大小写
}

TEST_CASE("an empty source never invents resources", "[webview][resource]")
{
    const ResourceProvider empty{ResourceProvider::Source{}};
    CHECK_FALSE(empty.lookup("/").has_value());
    CHECK_FALSE(empty.lookup("/index.html").has_value());

    // 描述不完整（缺任何一个指针）同样不造假
    auto noGetter = fullSource();
    noGetter.getNamedResource = nullptr;
    CHECK_FALSE(ResourceProvider(noGetter).lookup("/index.html").has_value());
    auto noNames = fullSource();
    noNames.originalFilenames = nullptr;
    CHECK_FALSE(ResourceProvider(noNames).lookup("/index.html").has_value());
    auto noSymbols = fullSource();
    noSymbols.resourceNames = nullptr;
    CHECK_FALSE(ResourceProvider(noSymbols).lookup("/index.html").has_value());
}

TEST_CASE("a resource whose data cannot be fetched is a miss", "[webview][resource]")
{
    auto source = fullSource();
    source.getNamedResource = getNothing;
    CHECK_FALSE(ResourceProvider(source).lookup("/index.html").has_value());
}

TEST_CASE("duplicate file names resolve to the first entry", "[webview][resource]")
{
    // 这就是构建期必须断言文件名唯一的原因：重名时后者永远取不到
    const ResourceProvider provider(fullSource());
    const auto hit = provider.lookup("/dup.js");
    REQUIRE(hit.has_value());
    CHECK(body(*hit) == "first");
}

TEST_CASE("mime table", "[webview][resource]")
{
    CHECK(std::string(ResourceProvider::mimeForExtension("html")) == "text/html");
    CHECK(std::string(ResourceProvider::mimeForExtension("HTM")) == "text/html");
    CHECK(std::string(ResourceProvider::mimeForExtension("css")) == "text/css");
    CHECK(std::string(ResourceProvider::mimeForExtension("js")) == "text/javascript");
    CHECK(std::string(ResourceProvider::mimeForExtension("mjs")) == "text/javascript");
    CHECK(std::string(ResourceProvider::mimeForExtension("json")) == "application/json");
    CHECK(std::string(ResourceProvider::mimeForExtension("svg")) == "image/svg+xml");
    CHECK(std::string(ResourceProvider::mimeForExtension("woff2")) == "font/woff2");
    CHECK(std::string(ResourceProvider::mimeForExtension("woff")) == "font/woff");
    CHECK(std::string(ResourceProvider::mimeForExtension("ttf")) == "font/ttf");
    CHECK(std::string(ResourceProvider::mimeForExtension("png")) == "image/png");
    CHECK(std::string(ResourceProvider::mimeForExtension("")) == "application/octet-stream");
    CHECK(std::string(ResourceProvider::mimeForExtension("exe")) == "application/octet-stream");
}

// ---------------------------------------------------------------------------------------------
// 导航闸的判定：页面带着原生桥，顶层导航只能留在资源提供者的根下面（外加页面隐藏时借道的空白页）。
// 两个平台的根都喂：Windows 与 Android 是 https://juce.backend/，macOS、iOS 与 Linux 是 juce://juce.backend/。
// ---------------------------------------------------------------------------------------------

namespace
{

const juce::String kWinRoot = "https://juce.backend/";
const juce::String kMacRoot = "juce://juce.backend/";

bool allowed(const juce::String& url, const juce::String& root = kWinRoot)
{
    return ResourceProvider::isRootNavigation(url, root);
}

} // namespace

TEST_CASE("navigation gate lets the resource root and everything under it through", "[webview][navigation]")
{
    for (const auto& root : {kWinRoot, kMacRoot})
    {
        INFO(root.toStdString());
        const juce::String origin = root.upToLastOccurrenceOf("/", false, false); // 去掉末尾斜杠
        CHECK(allowed(root, root));
        CHECK(allowed(origin, root)); // 没有路径的写法
        CHECK(allowed(root + "input/index.html", root));
        CHECK(allowed(root + "output/index.html", root));
        CHECK(allowed(root + "shared/x.js?v=2#frag", root));
        CHECK(allowed(root + "?x=1", root));
        CHECK(allowed(origin + "?x=1", root));
        CHECK(allowed(origin + "#frag", root));
    }
}

TEST_CASE("navigation gate ignores case in the scheme and the host only", "[webview][navigation]")
{
    CHECK(allowed("HTTPS://JUCE.BACKEND/input/index.html"));
    CHECK(allowed("Https://Juce.Backend/"));
    CHECK(allowed("JUCE://JUCE.BACKEND/input/index.html", kMacRoot));
    // 大小写不同的根也一样（调用方给的根不一定是小写）
    CHECK(allowed("https://juce.backend/", "HTTPS://JUCE.BACKEND/"));
    // 路径区分大小写，但路径不参与判定：留在根下面就放行
    CHECK(allowed("https://juce.backend/INPUT/Index.HTML"));
}

TEST_CASE("navigation gate lets the blank page through, exactly that string", "[webview][navigation]")
{
    CHECK(allowed("about:blank"));
    CHECK(allowed("about:blank", kMacRoot));
    CHECK(allowed("about:blank", "")); // 根未知（不认识的系统）时仍放行借道的空白页

    // 只认整串，别的 about: 地址、带后缀的、大写的都拦
    CHECK_FALSE(allowed("about:blank#x"));
    CHECK_FALSE(allowed("about:blank?x"));
    CHECK_FALSE(allowed("about:blank "));
    CHECK_FALSE(allowed(" about:blank"));
    CHECK_FALSE(allowed("ABOUT:BLANK"));
    CHECK_FALSE(allowed("about:srcdoc"));
    CHECK_FALSE(allowed("about:"));
    CHECK_FALSE(allowed("about:blank/https://juce.backend/"));
}

TEST_CASE("navigation gate blocks other sites, including look-alike hosts", "[webview][navigation]")
{
    CHECK_FALSE(allowed("https://example.com/"));
    CHECK_FALSE(allowed("https://synchain.ca/"));

    // 形似的主机名：多一段、少一段、前缀、后缀、连字符、末尾的点
    CHECK_FALSE(allowed("https://juce.backend.example.com/"));
    CHECK_FALSE(allowed("https://example.juce.backend/"));
    CHECK_FALSE(allowed("https://evil-juce.backend/"));
    CHECK_FALSE(allowed("https://juce.backendx/"));
    CHECK_FALSE(allowed("https://xjuce.backend/"));
    CHECK_FALSE(allowed("https://juce-backend/"));
    CHECK_FALSE(allowed("https://juce.backend./"));
    CHECK_FALSE(allowed("https://juce/"));
    CHECK_FALSE(allowed("https://backend/"));
    CHECK_FALSE(allowed("https://juce%2ebackend/"));

    // 根只出现在路径、查询串或片段里，不算
    CHECK_FALSE(allowed("https://example.com/https://juce.backend/"));
    CHECK_FALSE(allowed("https://example.com/?next=https://juce.backend/"));
    CHECK_FALSE(allowed("https://example.com/#https://juce.backend/"));
    CHECK_FALSE(allowed("https://example.com/juce.backend/"));

    // 用户信息：@ 前面才是用户，写成 juce.backend@example.com 或 example.com@juce.backend 都拦
    CHECK_FALSE(allowed("https://juce.backend@example.com/"));
    CHECK_FALSE(allowed("https://example.com@juce.backend/"));
    CHECK_FALSE(allowed("https://user:pw@juce.backend/"));
    // 反斜杠：浏览器把它当斜杠，解析器之间容易不一致，一律拦
    CHECK_FALSE(allowed("https://juce.backend\\@example.com/"));
    CHECK_FALSE(allowed("https://example.com\\.juce.backend/"));
    CHECK_FALSE(allowed("https://juce.backend\\evil/"));
}

TEST_CASE("navigation gate blocks non-ASCII look-alikes and invisible characters", "[webview][navigation]")
{
    // 西里尔字母（U+0441）冒充拉丁字母 c
    const auto homoglyph = juce::String("https://ju") + juce::String::charToString(0x0441) + "e.backend/";
    CHECK_FALSE(allowed(homoglyph));
    // 零宽空格（U+200B）跟在主机名后
    const auto zeroWidth = juce::String("https://juce.backend") + juce::String::charToString(0x200B) + "/";
    CHECK_FALSE(allowed(zeroWidth));
    // 全角点（U+FF0E）
    const auto fullWidthDot = juce::String("https://juce") + juce::String::charToString(0xFF0E) + "backend/";
    CHECK_FALSE(allowed(fullWidthDot));

    // 控制字符与空白：任何位置都拦
    CHECK_FALSE(allowed(" https://juce.backend/"));
    CHECK_FALSE(allowed("https://juce.backend/ "));
    CHECK_FALSE(allowed("https://juce.backend/a b"));
    CHECK_FALSE(allowed("https://juce.backend/\n"));
    CHECK_FALSE(allowed("https://juce.backend/\t"));
    CHECK_FALSE(allowed(juce::String("https://juce.backend/") + juce::String::charToString(0x7F)));
    CHECK_FALSE(allowed(juce::String("https://juce.backend/") + juce::String::charToString(0x01)));
    CHECK_FALSE(allowed(juce::String("https://juce.backend") + juce::String::charToString(0x0A) + ".evil/"));
}

TEST_CASE("navigation gate blocks a downgraded or different scheme on the same host", "[webview][navigation]")
{
    // http 降级
    CHECK_FALSE(allowed("http://juce.backend/"));
    CHECK_FALSE(allowed("http://juce.backend/input/index.html"));
    CHECK_FALSE(allowed("HTTP://JUCE.BACKEND/"));
    // 两个平台的根不通用：Windows 的页面不能去 juce:，macOS 的页面不能去 https:
    CHECK_FALSE(allowed("juce://juce.backend/"));
    CHECK_FALSE(allowed("https://juce.backend/", kMacRoot));
    CHECK_FALSE(allowed("http://juce.backend/", kMacRoot));
    // 别的协议，主机写成根的主机也不行
    CHECK_FALSE(allowed("ftp://juce.backend/"));
    CHECK_FALSE(allowed("wss://juce.backend/"));
    CHECK_FALSE(allowed("file://juce.backend/x"));
    CHECK_FALSE(allowed("file:///C:/Windows/win.ini"));
    CHECK_FALSE(allowed("file:///etc/passwd"));
    CHECK_FALSE(allowed("javascript:alert(1)"));
    CHECK_FALSE(allowed("JavaScript:alert(1)"));
    CHECK_FALSE(allowed("javascript://juce.backend/%0Aalert(1)"));
    CHECK_FALSE(allowed("data:text/html,<script>alert(1)</script>"));
    CHECK_FALSE(allowed("data:text/plain;charset=UTF-8,Error code: 9")); // JUCE 自己的错误页：我们关掉了，不放
    CHECK_FALSE(allowed("blob:https://juce.backend/1234"));
    CHECK_FALSE(allowed("view-source:https://juce.backend/"));
    CHECK_FALSE(allowed("chrome://settings"));
    CHECK_FALSE(allowed("edge://settings"));
    // 协议部分不合文法
    CHECK_FALSE(allowed("1https://juce.backend/"));
    CHECK_FALSE(allowed("ht tps://juce.backend/"));
    CHECK_FALSE(allowed("ht_tps://juce.backend/"));
    CHECK_FALSE(allowed("://juce.backend/"));
}

TEST_CASE("navigation gate blocks any port, even the default one", "[webview][navigation]")
{
    // 本插件从不用带端口的地址；带端口说明不是我们发起的。默认端口也不例外
    CHECK_FALSE(allowed("https://juce.backend:443/"));
    CHECK_FALSE(allowed("https://juce.backend:8443/"));
    CHECK_FALSE(allowed("https://juce.backend:80/"));
    CHECK_FALSE(allowed("https://juce.backend:/"));
    CHECK_FALSE(allowed("https://juce.backend:evil/"));
    CHECK_FALSE(allowed("juce://juce.backend:1/", kMacRoot));
    CHECK_FALSE(allowed("https://juce.backend:443"));
    // 端口写在路径里不算端口
    CHECK(allowed("https://juce.backend/a:443"));
}

TEST_CASE("navigation gate blocks scheme-less, empty and malformed input", "[webview][navigation]")
{
    CHECK_FALSE(allowed(""));
    CHECK_FALSE(allowed("   "));
    CHECK_FALSE(allowed("juce.backend/input/index.html"));
    CHECK_FALSE(allowed("//juce.backend/input/index.html"));
    CHECK_FALSE(allowed("/input/index.html"));
    CHECK_FALSE(allowed("input/index.html"));
    CHECK_FALSE(allowed("https:juce.backend"));
    CHECK_FALSE(allowed("https:/juce.backend/"));
    CHECK_FALSE(allowed("https:///juce.backend/"));
    CHECK_FALSE(allowed("https://"));
    CHECK_FALSE(allowed("https://?x"));
    CHECK_FALSE(allowed("https://#x"));
}

TEST_CASE("navigation gate with an unknown or malformed root only lets the blank page through", "[webview][navigation]")
{
    // 不认识的系统：getResourceProviderRoot 给空串。此时没有可以放行的根，不能退成「全放」
    CHECK_FALSE(allowed("https://juce.backend/", ""));
    CHECK_FALSE(allowed("https://example.com/", ""));
    CHECK_FALSE(allowed("", ""));
    CHECK(allowed("about:blank", ""));
    // 根本身读不懂，同样什么都不放
    CHECK_FALSE(allowed("https://juce.backend/", "not a url"));
    CHECK_FALSE(allowed("https://juce.backend/", "https://"));
    CHECK_FALSE(allowed("https://", "https://"));
    // 协议部分不合文法的根：就算地址与根逐字相同也不放
    CHECK_FALSE(allowed("ht_tps://juce.backend/", "ht_tps://juce.backend/"));
    CHECK_FALSE(allowed("1https://juce.backend/", "1https://juce.backend/"));

    // 地址与根逐字相同，但写法本身就是危险的（用户信息、反斜杠、非 ASCII 主机）：不因为「与根相同」而放行
    const auto homoglyph = juce::String("https://ju") + juce::String::charToString(0x0441) + "e.backend/";
    for (const auto& bad :
         {juce::String("https://user@juce.backend/"), juce::String("https://juce.backend\\x/"), homoglyph})
    {
        INFO(bad.toStdString());
        CHECK_FALSE(allowed(bad, bad));
    }
}

TEST_CASE("log lines made from untrusted urls are bounded and have no control characters", "[webview][navigation]")
{
    CHECK(ResourceProvider::urlForLog("https://juce.backend/a") == "https://juce.backend/a");
    CHECK(ResourceProvider::urlForLog("").isEmpty());

    // 换行、回车、制表、退格与删除符不能把日志行断开或伪造出新的一行
    CHECK(ResourceProvider::urlForLog("a\nb\rc\td") == "a?b?c?d");
    CHECK(ResourceProvider::urlForLog(juce::String("x") + juce::String::charToString(0x7F) + "y") == "x?y");
    CHECK(ResourceProvider::urlForLog(juce::String("x") + juce::String::charToString(0x08) + "y") == "x?y");
    CHECK_FALSE(ResourceProvider::urlForLog("a\nRinne input: fake line").contains("\n"));

    // 长度上限按字符数；默认 120
    const juce::String longUrl = "https://example.com/" + juce::String::repeatedString("a", 500);
    CHECK(ResourceProvider::urlForLog(longUrl).length() == 120);
    CHECK(ResourceProvider::urlForLog(longUrl, 10) == "https://ex");
    CHECK(ResourceProvider::urlForLog(longUrl, 0).isEmpty());
    CHECK(ResourceProvider::urlForLog("abc", 3) == "abc");
    CHECK(ResourceProvider::urlForLog("abcd", 3) == "abc");
    // 中文等多字节字符按字符数截，不会切到字节中间
    const auto cjk =
        juce::String::charToString(0x4E2D) + juce::String::charToString(0x6587) + juce::String::charToString(0x5B57);
    CHECK(ResourceProvider::urlForLog(cjk, 2) == cjk.substring(0, 2));
}
