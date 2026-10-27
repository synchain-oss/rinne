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
